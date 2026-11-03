#include "audio.h"
#include "tray.h"
#include "input/input.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

#include "desktop.h"
#include "input/actions.h"
#include "input/keybindings.h"
#include "input/xkb.h"
#include "proto/data_device.h"
#include "proto/seat.h"
#include "proto/surface.h"
#include "proto/xdg_shell.h"
#include "backend/backend.h"
#include "server.h"
#include "toolbar.h"
#include "util/log.h"
#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* Small set helpers for held keys/buttons                                  */
/* ------------------------------------------------------------------------ */

static bool set_has(const uint32_t *a, size_t n, uint32_t v)
{
	for (size_t i = 0; i < n; i++) {
		if (a[i] == v)
			return true;
	}
	return false;
}

static bool set_add(uint32_t *a, size_t *n, size_t cap, uint32_t v)
{
	if (set_has(a, *n, v) || *n == cap)
		return false;
	a[(*n)++] = v;
	return true;
}

static bool set_remove(uint32_t *a, size_t *n, uint32_t v)
{
	for (size_t i = 0; i < *n; i++) {
		if (a[i] == v) {
			memmove(a + i, a + i + 1, (*n - i - 1) * sizeof *a);
			(*n)--;
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

bool rei_input_init(struct rei_server *server, char *err, size_t errlen)
{
	struct rei_input *in = rei_xcalloc(1, sizeof *in);
	in->server = server;
	in->bar_press_tray = -1;
	wl_list_init(&in->keyboard_focus_destroy.link);
	wl_list_init(&in->pointer_focus_destroy.link);
	wl_list_init(&in->grab_destroy.link);

	in->xkb = rei_xkb_create(server->config, err, errlen);
	if (!in->xkb)
		goto fail;
	in->bindings = rei_bindings_from_config(server->config, err, errlen);
	if (!in->bindings)
		goto fail;

	server->input = in;
	return true;
fail:
	rei_xkb_destroy(in->xkb);
	free(in);
	return false;
}

/* Builds what depends on `cfg` (keybindings, and the keymap if the layout changed) and
 * only then swaps it in, so a bad config leaves everything as it was. The server's own
 * config pointer is updated by the caller afterwards. */
bool rei_input_prepare_reload(struct rei_server *server, const struct rei_config *cfg,
			      struct rei_input_reload *out, char *err, size_t errlen)
{
	const struct rei_config *old = server->config;
	*out = (struct rei_input_reload){ 0 };
	out->bindings = rei_bindings_from_config(cfg, err, errlen);
	if (!out->bindings)
		return false;
	out->keymap_changed = strcmp(old->kb_rules, cfg->kb_rules) || strcmp(old->kb_model, cfg->kb_model) ||
			      strcmp(old->kb_layout, cfg->kb_layout) || strcmp(old->kb_variant, cfg->kb_variant) ||
			      strcmp(old->kb_options, cfg->kb_options);
	if (out->keymap_changed) {
		out->xkb = rei_xkb_create(cfg, err, errlen);
		if (!out->xkb) {
			rei_bindings_free(out->bindings);
			out->bindings = NULL;
			return false;
		}
	}
	return true;
}

void rei_input_commit_reload(struct rei_server *server, struct rei_input_reload *r)
{
	struct rei_input *in = server->input;
	rei_bindings_free(in->bindings);
	in->bindings = r->bindings;
	if (r->xkb) {
		rei_xkb_destroy(in->xkb);
		in->xkb = r->xkb;
	}
	r->bindings = NULL;
	r->xkb = NULL;
}

void rei_input_finish(struct rei_server *server)
{
	struct rei_input *in = server->input;
	if (!in)
		return;
	server->input = NULL;
	wl_list_remove(&in->keyboard_focus_destroy.link);
	wl_list_remove(&in->pointer_focus_destroy.link);
	wl_list_remove(&in->grab_destroy.link);
	rei_bindings_free(in->bindings);
	rei_xkb_destroy(in->xkb);
	free(in);
}

static void update_pointer(struct rei_input *in, uint32_t time_ms, bool send_motion);

/* ------------------------------------------------------------------------ */
/* Keyboard focus                                                           */
/* ------------------------------------------------------------------------ */

static void on_keyboard_focus_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_input *in = wl_container_of(l, in, keyboard_focus_destroy);
	wl_list_remove(&in->keyboard_focus_destroy.link);
	wl_list_init(&in->keyboard_focus_destroy.link);
	in->keyboard_focus = NULL;  /* The client is gone; there is nobody to tell. */
}

static void set_keyboard_focus(struct rei_input *in, struct rei_surface *surface)
{
	if (in->keyboard_focus == surface)
		return;

	struct wl_client *old_client =
		in->keyboard_focus ? wl_resource_get_client(in->keyboard_focus->resource) : NULL;
	if (in->keyboard_focus) {
		rei_seat_keyboard_leave(in->server->seat, in->keyboard_focus);
		wl_list_remove(&in->keyboard_focus_destroy.link);
		wl_list_init(&in->keyboard_focus_destroy.link);
	}
	in->keyboard_focus = surface;
	if (!surface)
		return;

	in->keyboard_focus_destroy.notify = on_keyboard_focus_destroyed;
	wl_signal_add(&surface->destroy_signal, &in->keyboard_focus_destroy);
	rei_seat_keyboard_enter(in->server->seat, surface, in->pressed, in->n_pressed);

	/* The clipboard follows keyboard focus between clients. */
	struct wl_client *client = wl_resource_get_client(surface->resource);
	if (client != old_client)
		rei_data_device_send_selection(in->server, client);
}

void rei_input_refocus(struct rei_server *server)
{
	struct rei_input *in = server->input;
	if (!in || server->shutting_down)
		return;
	struct rei_surface *target = rei_xdg_topmost_grab_surface(server);
	if (!target)
		target = rei_wm_keyboard_surface(server);
	set_keyboard_focus(in, target);

	/* The scene changed (a window appeared, vanished, moved or was raised), so
	 * the surface under a stationary pointer may have changed too. Clients get
	 * enter/leave without needing the mouse to move. */
	update_pointer(in, (uint32_t)(rei_now_ns() / 1000000ull), false);
	rei_server_schedule_flush(server);
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                     */
/* ------------------------------------------------------------------------ */

void rei_input_key(struct rei_server *server, uint32_t time_ms, uint32_t code, bool pressed)
{
	struct rei_input *in = server->input;
	if (!in)
		return;

	/* Ignore a press of a key already down or a release of one that isn't:
	 * backends can deliver either (e.g. focus changes), and clients must see
	 * a consistent sequence. */
	if (pressed ? !set_add(in->pressed, &in->n_pressed, REI_MAX_PRESSED, code)
		    : !set_remove(in->pressed, &in->n_pressed, code))
		return;

	bool mods_changed = rei_xkb_update_key(in->xkb, code, pressed);

	/* Escape closes the tray's window list. */
	if (pressed && code == 1 && rei_tray_menu_is_open(server)) {
		set_add(in->swallowed, &in->n_swallowed, REI_MAX_PRESSED, code);
		rei_tray_menu_close(server);
		return;
	}

	/* Ctrl+Alt+F1..F12 produce XF86Switch_VT_n, which only the compositor can act on. */
	if (pressed && server->backend && server->backend->impl->switch_vt) {
		xkb_keysym_t sym = rei_xkb_sym(in->xkb, code);
		if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
			set_add(in->swallowed, &in->n_swallowed, REI_MAX_PRESSED, code);
			server->backend->impl->switch_vt(server->backend,
				(unsigned)(sym - XKB_KEY_XF86Switch_VT_1) + 1);
			return;
		}
	}

	if (pressed) {
		uint32_t mods = rei_xkb_mods(in->xkb);
		const struct rei_action *action =
			rei_bindings_match(in->bindings, mods, rei_xkb_base_sym(in->xkb, code));
		if (!action)
			action = rei_bindings_match(in->bindings, mods,
						    xkb_keysym_to_lower(rei_xkb_sym(in->xkb, code)));
		if (action) {
			set_add(in->swallowed, &in->n_swallowed, REI_MAX_PRESSED, code);
			rei_actions_run(server, action);
			return;
		}
	} else if (set_remove(in->swallowed, &in->n_swallowed, code)) {
		return;  /* The press was consumed, so its release is too. */
	}

	if (!in->keyboard_focus)
		return;
	rei_seat_keyboard_key(server->seat, in->keyboard_focus, time_ms, code, pressed);
	if (mods_changed)
		rei_seat_keyboard_modifiers(server->seat, in->keyboard_focus);
	rei_server_schedule_flush(server);
}

/* ------------------------------------------------------------------------ */
/* Pointer                                                                  */
/* ------------------------------------------------------------------------ */

static void on_pointer_focus_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_input *in = wl_container_of(l, in, pointer_focus_destroy);
	wl_list_remove(&in->pointer_focus_destroy.link);
	wl_list_init(&in->pointer_focus_destroy.link);
	in->pointer_focus = NULL;
}

static void on_grab_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_input *in = wl_container_of(l, in, grab_destroy);
	wl_list_remove(&in->grab_destroy.link);
	wl_list_init(&in->grab_destroy.link);
	in->grab_surface = NULL;
}

static void set_pointer_focus(struct rei_input *in, struct rei_surface *surface, double sx, double sy)
{
	if (in->pointer_focus == surface)
		return;
	if (in->pointer_focus) {
		rei_seat_pointer_leave(in->server->seat, in->pointer_focus);
		wl_list_remove(&in->pointer_focus_destroy.link);
		wl_list_init(&in->pointer_focus_destroy.link);
	}
	in->pointer_focus = surface;
	if (surface) {
		in->pointer_focus_destroy.notify = on_pointer_focus_destroyed;
		wl_signal_add(&surface->destroy_signal, &in->pointer_focus_destroy);
		rei_seat_pointer_enter(in->server->seat, surface, sx, sy);
	}
}

void rei_input_clamp_cursor(struct rei_server *s)
{
	if (wl_list_empty(&s->outputs))
		return;

	double best_d = INFINITY, bx = s->cursor_x, by = s->cursor_y;
	const struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		int32_t w, h;
		rei_output_logical_size(o, &w, &h);
		/* Last addressable position is one pixel inside the edge. */
		double cx = fmin(fmax(s->cursor_x, o->x), o->x + w - 1);
		double cy = fmin(fmax(s->cursor_y, o->y), o->y + h - 1);
		double d = hypot(cx - s->cursor_x, cy - s->cursor_y);
		if (d < best_d) {
			best_d = d;
			bx = cx;
			by = cy;
		}
	}
	s->cursor_x = bx;
	s->cursor_y = by;
}

/* Re-derives pointer focus and delivers motion after the cursor or the scene
 * changed. */
static void update_pointer(struct rei_input *in, uint32_t time_ms, bool send_motion)
{
	struct rei_server *s = in->server;

	if (in->move_view) {
		struct rei_view *v = in->move_view;
		if (!v->floating) {
			/* A tiled window dragged by its handle stays put until the pointer
			 * has really moved, so a plain click doesn't pull it out of the grid. */
			if (hypot(s->cursor_x - in->drag_start_x, s->cursor_y - in->drag_start_y) < 6.0)
				return;
			if (!rei_wm_drag_float(v)) {
				in->move_view = NULL;
				return;
			}
			in->move_view_x = v->x;
			in->move_view_y = v->y;
		}
		rei_wm_float_move(v, in->move_view_x + (int32_t)lround(s->cursor_x - in->move_cursor_x),
				  in->move_view_y + (int32_t)lround(s->cursor_y - in->move_cursor_y));
		rei_wm_drag_update(v, s->cursor_x, s->cursor_y);
		return;
	}

	if (in->resize_view) {
		/* Edges bits follow xdg_toplevel: top 1, bottom 2, left 4, right 8. */
		int32_t dx = (int32_t)lround(s->cursor_x - in->move_cursor_x);
		int32_t dy = (int32_t)lround(s->cursor_y - in->move_cursor_y);
		struct rei_rect r = in->resize_start;
		const int32_t min_w = 80, min_h = 40;
		if (in->resize_edges & 4) { r.x += dx; r.w -= dx; }
		if (in->resize_edges & 8) { r.w += dx; }
		if (in->resize_edges & 1) { r.y += dy; r.h -= dy; }
		if (in->resize_edges & 2) { r.h += dy; }
		if (r.w < min_w) {  /* Clamp, keeping the opposite edge where it was. */
			if (in->resize_edges & 4) r.x = in->resize_start.x + in->resize_start.w - min_w;
			r.w = min_w;
		}
		if (r.h < min_h) {
			if (in->resize_edges & 1) r.y = in->resize_start.y + in->resize_start.h - min_h;
			r.h = min_h;
		}
		rei_wm_float_set_rect(in->resize_view, r.x, r.y, r.w, r.h);
		return;
	}

	if (in->grab_surface) {
		double sx = s->cursor_x - in->grab_origin_x, sy = s->cursor_y - in->grab_origin_y;
		if (send_motion)
			rei_seat_pointer_motion(s->seat, in->grab_surface, time_ms, sx, sy);
		return;
	}

	/* The notches belong to the compositor: over one, no client has the pointer. */
	struct rei_view *nv = NULL;
	enum rei_notch_part part = REI_NOTCH_NONE;
	bool over_notch = rei_wm_notch_at(s, s->cursor_x, s->cursor_y, &nv, &part);
	rei_wm_set_notch_hover(s, over_notch ? nv : NULL, over_notch ? part : REI_NOTCH_NONE);
	if (over_notch) {
		set_pointer_focus(in, NULL, 0, 0);
		return;
	}

	struct rei_surface *hit = NULL;
	double sx = 0, sy = 0;
	rei_wm_pick(s, s->cursor_x, s->cursor_y, &hit, &sx, &sy);

	/* The toolbar is the compositor's. A window over it (a floating one) wins,
	 * since it is drawn above. */
	struct rei_output *bar_output = NULL;
	int bar_ws = 0;
	if (!hit && rei_toolbar_hit(s, s->cursor_x, s->cursor_y, &bar_output, &bar_ws)) {
		rei_toolbar_set_hover(s, bar_output, bar_ws);
		set_pointer_focus(in, NULL, 0, 0);
		return;
	}
	rei_toolbar_set_hover(s, NULL, 0);
	if (hit != in->pointer_focus)
		set_pointer_focus(in, hit, sx, sy);
	else if (hit && send_motion)
		rei_seat_pointer_motion(s->seat, hit, time_ms, sx, sy);

	/* Optionally, focus follows the pointer (only on real motion, so windows
	 * appearing or vanishing under a still pointer don't steal focus). */
	if (send_motion && s->config->focus_follows_mouse && hit && !rei_xdg_topmost_grab_surface(s)) {
		struct rei_view *v = rei_wm_view_for_surface(s, hit);
		if (v && v != rei_wm_active_view(s))
			rei_wm_focus_view(v);
	}
}

static void pointer_moved(struct rei_input *in, uint32_t time_ms)
{
	rei_tray_menu_motion(in->server, in->server->cursor_x, in->server->cursor_y);
	rei_input_clamp_cursor(in->server);
	update_pointer(in, time_ms, true);
	rei_server_cursor_moved(in->server);  /* Drawn in the scene unless a hardware cursor shows it. */
	rei_server_schedule_flush(in->server);
}

void rei_input_pointer_motion(struct rei_server *s, uint32_t time_ms, double dx, double dy)
{
	if (!s->input || wl_list_empty(&s->outputs))
		return;
	s->cursor_x += dx;
	s->cursor_y += dy;
	pointer_moved(s->input, time_ms);
}

void rei_input_pointer_motion_abs(struct rei_server *s, uint32_t time_ms, double x, double y)
{
	if (!s->input || wl_list_empty(&s->outputs))
		return;
	s->cursor_x = x;
	s->cursor_y = y;
	pointer_moved(s->input, time_ms);
}

void rei_input_pointer_button(struct rei_server *s, uint32_t time_ms, uint32_t button, bool pressed)
{
	struct rei_input *in = s->input;
	if (!in)
		return;

	if (pressed ? !set_add(in->buttons, &in->n_buttons, REI_MAX_BUTTONS, button)
		    : !set_remove(in->buttons, &in->n_buttons, button))
		return;

	if (pressed && rei_tray_menu_is_open(s)) {
		/* The window list takes the press (picking a row, or just closing). The release is
		 * swallowed like a toolbar press's, so no client sees half a click. */
		rei_tray_menu_press(s, s->cursor_x, s->cursor_y);
		in->bar_press = true;
		in->bar_press_ws = 0;
		in->bar_press_volume = false;
		in->bar_press_tray = -1;
		rei_server_schedule_flush(s);
		return;
	}

	if (pressed && in->n_buttons == 1 && button == 272 && !in->move_view && !in->resize_view) {
		/* A press on a notch is the compositor's, not the client's. */
		struct rei_view *nv;
		enum rei_notch_part part;
		if (rei_wm_notch_at(s, s->cursor_x, s->cursor_y, &nv, &part)) {
			in->notch_press = part;
			in->notch_view = nv;
			if (part == REI_NOTCH_HANDLE) {
				/* Press and hold to drag. A tiled window waits for the pointer
				 * to move (see update_pointer) before leaving the grid. */
				in->move_view = nv;
				in->move_cursor_x = s->cursor_x;
				in->move_cursor_y = s->cursor_y;
				in->drag_start_x = s->cursor_x;
				in->drag_start_y = s->cursor_y;
				in->move_view_x = nv->x;
				in->move_view_y = nv->y;
			}
			rei_server_schedule_flush(s);
			return;
		}
	}

	if (pressed && in->n_buttons == 1 && !in->move_view && !in->resize_view && !in->pointer_focus) {
		/* A press on the toolbar is the compositor's, not a client's. */
		struct rei_output *bo;
		int ws;
		if (rei_toolbar_hit(s, s->cursor_x, s->cursor_y, &bo, &ws)) {
			in->bar_press = true;
			in->bar_press_volume = ws == 0 && rei_toolbar_volume_at(s, s->cursor_x, s->cursor_y);
			in->bar_press_tray = -1;
			struct rei_output *tray_output;
			int tray_index;
			if (ws == 0 && !in->bar_press_volume &&
			    rei_toolbar_tray_at(s, s->cursor_x, s->cursor_y, &tray_output, &tray_index)) {
				in->bar_press_tray = tray_index;
				bo = tray_output;
			}
			in->bar_press_ws = ws;
			in->bar_press_output = bo;
			rei_server_schedule_flush(s);
			return;
		}
	}

	if (pressed && in->n_buttons == 1 && !in->move_view && !in->resize_view) {
		/* First button down: this click decides focus. */
		struct rei_surface *target = in->pointer_focus;
		rei_xdg_dismiss_popups(s, target);

		/* Mod+drag moves (left button) or resizes (right) a floating window,
		 * wherever in it the pointer is; the client never sees the click. */
		struct rei_view *under = target ? rei_wm_view_for_surface(s, target) : NULL;
		uint32_t mod = rei_composition_mod(s->config->composition_key);
		if (under && under->floating && (rei_xkb_mods(in->xkb) & mod) &&
		    (button == 272 || button == 273)) {
			rei_wm_focus_view(under);
			in->move_cursor_x = s->cursor_x;
			in->move_cursor_y = s->cursor_y;
			if (button == 272) {
				in->move_view = under;
				in->move_view_x = under->x;
				in->move_view_y = under->y;
			} else {
				struct rei_rect r = rei_view_rect(under);
				in->resize_view = under;
				in->resize_start = r;
				/* Grab the corner nearest the pointer. */
				in->resize_edges = (s->cursor_x < r.x + r.w / 2.0 ? 4u : 8u) |
						   (s->cursor_y < r.y + r.h / 2.0 ? 1u : 2u);
			}
			if (in->grab_surface) {
				wl_list_remove(&in->grab_destroy.link);
				wl_list_init(&in->grab_destroy.link);
				in->grab_surface = NULL;
			}
			set_pointer_focus(in, NULL, 0, 0);
			rei_server_schedule_flush(s);
			return;
		}

		if (!target)
			rei_wm_focus_output_at(s, s->cursor_x, s->cursor_y);
		if (target) {
			struct rei_view *view = rei_wm_view_for_surface(s, target);
			if (view)
				rei_wm_focus_view(view);
			/* Implicit grab, remembering where the surface is. */
			in->grab_surface = target;
			in->grab_destroy.notify = on_grab_destroyed;
			wl_signal_add(&target->destroy_signal, &in->grab_destroy);
			double sx, sy;
			struct rei_surface *again;
			rei_wm_pick(s, s->cursor_x, s->cursor_y, &again, &sx, &sy);
			in->grab_origin_x = s->cursor_x - sx;
			in->grab_origin_y = s->cursor_y - sy;
		}
	}

	if (in->move_view || in->resize_view || in->notch_press != REI_NOTCH_NONE || in->bar_press) {
		/* A move, resize, notch or toolbar click owns the pointer; the client sees no buttons. */
		if (in->n_buttons == 0) {
			/* End the grab FIRST. What follows (dropping a window, closing one)
			 * changes focus, which re-evaluates the pointer and would otherwise
			 * find a grab still in progress and act on it. */
			struct rei_view *moved = in->move_view;
			struct rei_view *clicked = in->notch_view;
			enum rei_notch_part pressed_part = in->notch_press;
			in->move_view = NULL;
			in->resize_view = NULL;
			in->notch_press = REI_NOTCH_NONE;
			in->notch_view = NULL;
			bool bar_clicked = in->bar_press;
			int bar_ws = in->bar_press_ws;
			struct rei_output *bar_output = in->bar_press_output;
			bool volume_press = in->bar_press_volume;
			int tray_press = in->bar_press_tray;
			in->bar_press_tray = -1;
			in->bar_press = false;
			in->bar_press_volume = false;
			in->bar_press_ws = 0;
			in->bar_press_output = NULL;

			/* The volume text: a click toggles mute. */
			if (bar_clicked && volume_press && rei_toolbar_volume_at(s, s->cursor_x, s->cursor_y))
				rei_audio_toggle_mute(s);

			/* A tray tile: clicked if released on the same one. */
			if (bar_clicked && tray_press >= 0) {
				struct rei_output *to;
				int ti;
				if (rei_toolbar_tray_at(s, s->cursor_x, s->cursor_y, &to, &ti) && to == bar_output &&
				    ti == tray_press)
					rei_toolbar_tray_click(s, to, ti);
			}

			/* A workspace button is clicked if the press and release are on it. */
			if (bar_clicked && bar_ws > 0) {
				struct rei_output *o;
				int ws;
				if (rei_toolbar_hit(s, s->cursor_x, s->cursor_y, &o, &ws) && o == bar_output && ws == bar_ws)
					rei_wm_goto_workspace(s, bar_ws);
			}

			if (moved)  /* Drops a lifted window into place, or settles a floating one. */
				rei_wm_drag_finish(moved, s->cursor_x, s->cursor_y);

			/* A button counts as clicked if it is released while still over it. */
			if (clicked && pressed_part != REI_NOTCH_NONE && pressed_part != REI_NOTCH_HANDLE) {
				struct rei_view *nv;
				enum rei_notch_part part;
				if (rei_wm_notch_at(s, s->cursor_x, s->cursor_y, &nv, &part) &&
				    nv == clicked && part == pressed_part)
					rei_wm_notch_activate(s, nv, part);
			}
			update_pointer(in, time_ms, false);  /* Pick whatever is under the cursor now. */
		}
		rei_server_schedule_flush(s);
		return;
	}

	struct rei_surface *to = in->grab_surface ? in->grab_surface : in->pointer_focus;
	if (to)
		in->last_button_serial = rei_seat_pointer_button(s->seat, to, time_ms, button, pressed);

	if (!pressed && in->n_buttons == 0 && in->grab_surface) {
		wl_list_remove(&in->grab_destroy.link);
		wl_list_init(&in->grab_destroy.link);
		in->grab_surface = NULL;
		update_pointer(in, time_ms, false);  /* The pointer may now be over something else. */
	}
	rei_server_schedule_flush(s);
}

void rei_input_pointer_axis(struct rei_server *s, uint32_t time_ms, bool horizontal, double value)
{
	struct rei_input *in = s->input;
	if (!in || in->move_view || in->resize_view || in->notch_press != REI_NOTCH_NONE || in->bar_press)
		return;

	/* The wheel over a toolbar steps through that output's workspaces. */
	struct rei_output *bar_output;
	int bar_ws;
	if (!in->pointer_focus && !in->grab_surface && !horizontal && value != 0 &&
	    rei_toolbar_hit(s, s->cursor_x, s->cursor_y, &bar_output, &bar_ws)) {
		if (rei_toolbar_volume_at(s, s->cursor_x, s->cursor_y))
			rei_audio_change_volume(s, value > 0 ? -s->config->volume_step : s->config->volume_step);
		else
			rei_toolbar_scroll(s, bar_output, value > 0 ? 1 : -1);
		return;
	}
	struct rei_surface *to = in->grab_surface ? in->grab_surface : in->pointer_focus;
	if (!to)
		return;
	rei_seat_pointer_axis(s->seat, to, time_ms, horizontal, value);
	rei_server_schedule_flush(s);
}

void rei_input_release_all(struct rei_server *s, uint32_t time_ms)
{
	struct rei_input *in = s->input;
	if (!in)
		return;
	while (in->n_buttons)
		rei_input_pointer_button(s, time_ms, in->buttons[in->n_buttons - 1], false);
	while (in->n_pressed)
		rei_input_key(s, time_ms, in->pressed[in->n_pressed - 1], false);
}

/* Both grabs need a button held, a matching serial, and a floating window:
 * tiled windows don't move or resize freely. */
static bool begin_grab(struct rei_server *s, struct rei_view *view, uint32_t serial)
{
	struct rei_input *in = s->input;
	if (!in || in->n_buttons == 0 || serial != in->last_button_serial || !view->floating)
		return false;
	in->move_cursor_x = s->cursor_x;
	in->move_cursor_y = s->cursor_y;

	/* The client gives up the pointer for the duration. */
	if (in->grab_surface) {
		wl_list_remove(&in->grab_destroy.link);
		wl_list_init(&in->grab_destroy.link);
		in->grab_surface = NULL;
	}
	set_pointer_focus(in, NULL, 0, 0);
	return true;
}

bool rei_input_begin_move(struct rei_server *s, struct rei_view *view, uint32_t serial)
{
	if (!begin_grab(s, view, serial))
		return false;
	s->input->move_view = view;
	s->input->move_view_x = view->x;
	s->input->move_view_y = view->y;
	return true;
}

bool rei_input_begin_resize(struct rei_server *s, struct rei_view *view, uint32_t serial,
			    uint32_t edges)
{
	if (edges == 0 || !begin_grab(s, view, serial))
		return false;
	s->input->resize_view = view;
	s->input->resize_edges = edges;
	s->input->resize_start = rei_view_rect(view);
	return true;
}
