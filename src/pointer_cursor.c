#include "pointer_cursor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <event2/event.h>
#include <wayland-server-core.h>

#include "confirm.h"
#include "config.h"
#include "desktop.h"
#include "input/input.h"
#include "msgbuf.h"
#include "output.h"
#include "popup.h"
#include "proto/surface.h"
#include "runprompt.h"
#include "server.h"
#include "toolbar.h"
#include "tray.h"
#include "util/log.h"
#include "util/util.h"
#include "volpop.h"

enum request { REQ_NONE, REQ_SHAPE, REQ_SURFACE, REQ_HIDDEN };

struct rei_pointer_cursor {
	struct rei_cursor_theme *theme;
	char theme_setting[96];   /* what the config asked for; "" = automatic */
	int size, pixel_scale;

	enum request req;
	struct wl_client *req_client;
	enum rei_cursor_shape req_shape;
	struct rei_surface *req_surface;
	int32_t req_hot_x, req_hot_y;
	struct wl_listener req_surface_destroy;

	/* What is drawn. */
	struct rei_cursor_image shown;
	bool hidden;
	uint64_t shown_id;   /* the pixbuf's id, to tell a new picture from the same one */

	/* An animated theme cursor: which one (the theme's first frame), the frame now shown, and the timer. */
	const struct rei_cursor_image *anim_base, *anim_cur;
	struct event *anim_timer;
};

static void drop_surface(struct rei_pointer_cursor *p)
{
	if (p->req_surface) {
		wl_list_remove(&p->req_surface_destroy.link);
		p->req_surface = NULL;
	}
}

static void on_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_pointer_cursor *p = wl_container_of(l, p, req_surface_destroy);
	wl_list_remove(&p->req_surface_destroy.link);
	p->req_surface = NULL;
	p->req = REQ_NONE;
	p->req_client = NULL;
}

static void on_cursor_surface_commit(struct rei_surface *surface, void *data)
{
	(void)surface;
	rei_pointer_cursor_refresh(data);
}

/* ---- What the compositor's own things want ---- */

static enum rei_cursor_shape resize_shape(uint32_t edges)
{
	/* xdg_toplevel edges: top 1, bottom 2, left 4, right 8. */
	switch (edges) {
	case 1: return REI_SHAPE_N_RESIZE;
	case 2: return REI_SHAPE_S_RESIZE;
	case 4: return REI_SHAPE_W_RESIZE;
	case 8: return REI_SHAPE_E_RESIZE;
	case 5: return REI_SHAPE_NW_RESIZE;
	case 9: return REI_SHAPE_NE_RESIZE;
	case 6: return REI_SHAPE_SW_RESIZE;
	case 10: return REI_SHAPE_SE_RESIZE;
	default: return REI_SHAPE_DEFAULT;
	}
}

static enum rei_cursor_shape ui_shape(struct rei_server *s)
{
	struct rei_input *in = s->input;
	if (in && in->move_view)
		return REI_SHAPE_GRABBING;
	if (in && in->resize_view)
		return resize_shape(in->resize_edges);

	/* The message stack view covers everything: the pointer is the compositor's, a hand over a music button. */
	if (rei_msg_stack_open(s))
		return rei_msg_stack_hover(s) >= 0 ? REI_SHAPE_POINTER : REI_SHAPE_DEFAULT;

	/* Modal and floating things first: they are above whatever window the pointer is over. */
	if (rei_confirm_is_open(s))
		return rei_confirm_hover(s) >= 0 ? REI_SHAPE_POINTER : REI_SHAPE_DEFAULT;
	if (rei_prompt_is_open(s) && rei_prompt_contains(s, s->cursor_x, s->cursor_y))
		return REI_SHAPE_TEXT;
	if (rei_volpop_is_open(s) && rei_volpop_hover(s) > 0)
		return REI_SHAPE_POINTER;
	if (rei_popup_is_open(s)) {
		struct rei_menu_state *m = rei_popup_state(s);
		if (m && m->hover >= 0)
			return REI_SHAPE_POINTER;
	}

	if (s->notch_hover_view) {
		switch (s->notch_hover_part) {
		case REI_NOTCH_HANDLE: return REI_SHAPE_GRAB;
		case REI_NOTCH_NONE: break;
		default: return REI_SHAPE_POINTER;
		}
	}
	if (s->bar_hover_output) {
		if (s->bar_hover_ws > 0 || s->start_hover || s->mode_hover || rei_tray_hovering(s) ||
		    rei_toolbar_wifi_at(s, s->cursor_x, s->cursor_y) || rei_toolbar_volume_at(s, s->cursor_x, s->cursor_y))
			return REI_SHAPE_POINTER;
		return REI_SHAPE_DEFAULT;
	}
	return REI_SHAPE_NONE;
}

/* ---- Deciding ---- */

static struct wl_client *pointer_client(struct rei_server *s)
{
	struct rei_input *in = s->input;
	struct rei_surface *f = in ? (in->grab_surface ? in->grab_surface : in->pointer_focus) : NULL;
	return f && f->resource ? wl_resource_get_client(f->resource) : NULL;
}

/* Fills `out` with what to draw and returns false when the pointer is to be hidden. */
static bool decide(struct rei_server *s, struct rei_cursor_image *out, const struct rei_cursor_image **src)
{
	*src = NULL;
	struct rei_pointer_cursor *p = s->pcursor;
	enum rei_cursor_shape shape = ui_shape(s);
	if (shape == REI_SHAPE_NONE && p->req != REQ_NONE && p->req_client && p->req_client == pointer_client(s)) {
		switch (p->req) {
		case REQ_HIDDEN:
			return false;
		case REQ_SHAPE:
			shape = p->req_shape;
			break;
		case REQ_SURFACE: {
			const struct rei_surface *sf = p->req_surface;
			if (!sf)
				return false;
			if (sf->dmabuf) {   /* a GPU buffer as a pointer: not worth supporting; the arrow instead */
				shape = REI_SHAPE_DEFAULT;
				break;
			}
			if (!sf->content)
				return false;   /* no buffer attached: the client wants no pointer */
			out->pixbuf = sf->content;
			out->width = sf->width > 0 ? sf->width : sf->content->width;
			out->height = sf->height > 0 ? sf->height : sf->content->height;
			out->hotspot_x = p->req_hot_x;
			out->hotspot_y = p->req_hot_y;
			return true;
		}
		default:
			break;
		}
	}
	if (shape == REI_SHAPE_NONE)
		shape = REI_SHAPE_DEFAULT;
	*src = rei_cursor_theme_shape(p->theme, shape);
	*out = **src;
	return true;
}

static void on_anim_timer(evutil_socket_t fd, short what, void *arg);

static void anim_stop(struct rei_pointer_cursor *p)
{
	if (p->anim_timer)
		evtimer_del(p->anim_timer);
	p->anim_base = p->anim_cur = NULL;
}

void rei_pointer_cursor_refresh(struct rei_server *s)
{
	struct rei_pointer_cursor *p = s->pcursor;
	if (!p || !p->theme)
		return;
	struct rei_cursor_image img = { 0 };
	const struct rei_cursor_image *src;
	bool visible = decide(s, &img, &src);
	if (visible && src && src->next) {
		/* An animation: keep playing it if it is the one already running, else start it from the top. */
		if (p->anim_base != src) {
			anim_stop(p);
			p->anim_base = p->anim_cur = src;
			if (!p->anim_timer)
				p->anim_timer = evtimer_new(s->base, on_anim_timer, s);
			struct timeval tv = { src->delay_ms / 1000, (src->delay_ms % 1000) * 1000 };
			evtimer_add(p->anim_timer, &tv);
		}
		img = *p->anim_cur;
	} else if (p->anim_base) {
		anim_stop(p);
	}
	bool changed;
	if (!visible) {
		changed = !p->hidden;
		p->hidden = true;
		p->shown = (struct rei_cursor_image){ 0 };
		p->shown_id = 0;
	} else {
		changed = p->hidden || p->shown_id != img.pixbuf->id || p->shown.width != img.width ||
			  p->shown.height != img.height || p->shown.hotspot_x != img.hotspot_x ||
			  p->shown.hotspot_y != img.hotspot_y;
		p->hidden = false;
		p->shown = img;
		p->shown_id = img.pixbuf->id;
	}
	if (changed && s->damage_event)   /* (not yet, while the server is still starting up) */
		rei_server_cursor_moved(s);   /* redraws it, or re-uploads the hardware cursor */
}

static void on_anim_timer(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_server *s = arg;
	struct rei_pointer_cursor *p = s->pcursor;
	if (!p || !p->anim_cur)
		return;
	p->anim_cur = p->anim_cur->next;
	struct timeval tv = { p->anim_cur->delay_ms / 1000, (p->anim_cur->delay_ms % 1000) * 1000 };
	evtimer_add(p->anim_timer, &tv);
	rei_pointer_cursor_refresh(s);
}

const struct rei_cursor_image *rei_pointer_cursor_image(struct rei_server *s)
{
	struct rei_pointer_cursor *p = s->pcursor;
	if (!p)
		return NULL;
	if (!p->hidden && !p->shown.pixbuf)
		rei_pointer_cursor_refresh(s);
	return p->hidden ? NULL : &p->shown;
}

/* ---- Requests ---- */

static bool accepts_request(struct rei_server *s, struct wl_client *client)
{
	return s->pcursor && client && client == pointer_client(s);
}

void rei_pointer_cursor_request_shape(struct rei_server *s, struct wl_client *client, enum rei_cursor_shape shape)
{
	if (!accepts_request(s, client)) {
		rei_debug("cursor: a shape request from a program the pointer is not in: ignored");
		return;
	}
	struct rei_pointer_cursor *p = s->pcursor;
	drop_surface(p);
	p->req = REQ_SHAPE;
	p->req_client = client;
	p->req_shape = shape;
	rei_pointer_cursor_refresh(s);
}

void rei_pointer_cursor_request_surface(struct rei_server *s, struct wl_client *client,
					struct rei_surface *surface, int32_t hot_x, int32_t hot_y)
{
	if (!accepts_request(s, client)) {
		rei_debug("cursor: a %s request from a program the pointer is not in: ignored", surface ? "picture" : "hide");
		return;
	}
	struct rei_pointer_cursor *p = s->pcursor;
	drop_surface(p);
	p->req_client = client;
	if (!surface) {
		p->req = REQ_HIDDEN;
	} else {
		p->req = REQ_SURFACE;
		p->req_surface = surface;
		p->req_hot_x = hot_x;
		p->req_hot_y = hot_y;
		p->req_surface_destroy.notify = on_surface_destroyed;
		wl_signal_add(&surface->destroy_signal, &p->req_surface_destroy);
		surface->role_commit = on_cursor_surface_commit;
		surface->role_data = s;
	}
	rei_pointer_cursor_refresh(s);
}

void rei_pointer_cursor_focus_changed(struct rei_server *s)
{
	struct rei_pointer_cursor *p = s->pcursor;
	if (!p || p->req == REQ_NONE)
		return;
	/* Moving between two windows of the same program (Xwayland's, above all): its request still stands until it
	 * says otherwise; it is another client's, or nobody's, that has to start over. */
	if (p->req_client && p->req_client == pointer_client(s))
		return;
	drop_surface(p);
	p->req = REQ_NONE;
	p->req_client = NULL;
}

/* ---- Set-up ---- */

static void export_env(struct rei_pointer_cursor *p)
{
	/* Programs started from here and Xwayland take their pointer from these. */
	setenv("XCURSOR_THEME", rei_cursor_theme_name(p->theme), 1);
	char sz[16];
	snprintf(sz, sizeof sz, "%d", p->size);
	setenv("XCURSOR_SIZE", sz, 1);
	/* Themes in /usr/share/cursors are not on the standard search path of the X cursor library. */
	const char *user = getenv("XCURSOR_PATH");
	if (!user || !user[0]) {
		char *d = rei_cursor_default_path();
		setenv("XCURSOR_PATH", d, 1);
		free(d);
	}
}

void rei_pointer_cursor_reconfigure(struct rei_server *s)
{
	struct rei_pointer_cursor *p = s->pcursor;
	if (!p)
		return;
	const char *want = s->config->cursor_theme ? s->config->cursor_theme : "";
	double max_scale = 1.0;
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link)
		if (o->scale > max_scale)
			max_scale = o->scale;
	int scale = (int)ceil(max_scale);
	int size = s->config->cursor_size;
	if (p->theme && !strcmp(p->theme_setting, want) && p->size == size && p->pixel_scale == scale)
		return;

	/* The pictures of the old theme go with it, so nothing may still point at them. */
	anim_stop(p);
	struct rei_cursor_theme *old = p->theme;
	p->theme = rei_cursor_theme_open(want, size, scale);
	snprintf(p->theme_setting, sizeof p->theme_setting, "%s", want);
	p->size = size;
	p->pixel_scale = scale;
	p->shown = (struct rei_cursor_image){ 0 };
	p->shown_id = 0;
	p->hidden = false;
	rei_pointer_cursor_refresh(s);
	rei_cursor_theme_close(old);
	export_env(p);
	rei_info("cursor: theme '%s', size %d", rei_cursor_theme_name(p->theme), size);
	if (s->damage_event)
		rei_server_schedule_damage(s);
}

bool rei_pointer_cursor_init(struct rei_server *s)
{
	s->pcursor = rei_xcalloc(1, sizeof *s->pcursor);
	rei_pointer_cursor_reconfigure(s);
	return true;
}

void rei_pointer_cursor_finish(struct rei_server *s)
{
	struct rei_pointer_cursor *p = s->pcursor;
	if (!p)
		return;
	drop_surface(p);
	anim_stop(p);
	if (p->anim_timer)
		event_free(p->anim_timer);
	rei_cursor_theme_close(p->theme);
	free(p);
	s->pcursor = NULL;
}
