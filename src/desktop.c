#include "desktop.h"
#include "xwayland.h"

#include <math.h>
#include <stdlib.h>

#include "input/input.h"
#include "proto/xdg_shell.h"
#include "toolbar.h"
#include "tray.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define CASCADE_STEP 30
#define CASCADE_SLOTS 8
#define FLOAT_NUDGE 40  /* Pixels a floating window moves per "move <dir>". */

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static struct rei_rect output_rect(const struct rei_output *o)
{
	int32_t w, h;
	rei_output_logical_size(o, &w, &h);
	return (struct rei_rect){ o->x, o->y, w, h };
}

/* The part of an output windows may use: the output less the toolbar's edge. */
static struct rei_rect output_usable(const struct rei_output *o)
{
	return rei_toolbar_usable(o);
}

static struct rei_rect inset(struct rei_rect r, int d)
{
	r.x += d;
	r.y += d;
	r.w -= 2 * d;
	r.h -= 2 * d;
	if (r.w < 1) r.w = 1;
	if (r.h < 1) r.h = 1;
	return r;
}

/* ---- A window is either an xdg toplevel or an X11 window ---- */

/* A window that is on screen if its workspace is: mapped and not minimized. */
#define SHOWN(v) ((v)->mapped && !(v)->minimized)

static struct rei_rect view_geometry(const struct rei_view *v)
{
	return v->xwin ? rei_xwindow_geometry(v->xwin) : rei_xdg_toplevel_geometry(v->toplevel);
}

static struct rei_surface *view_surface(const struct rei_view *v)
{
	return v->xwin ? rei_xwindow_surface(v->xwin) : rei_xdg_toplevel_surface(v->toplevel);
}

static bool view_wants_fullscreen(const struct rei_view *v)
{
	return v->xwin ? rei_xwindow_wants_fullscreen(v->xwin) : v->toplevel->want_fullscreen;
}

void rei_wm_close_view(struct rei_view *v)
{
	if (v->xwin)
		rei_xwindow_close(v->xwin);
	else
		rei_xdg_toplevel_close(v->toplevel);
}

bool rei_wm_surface_rect(struct rei_surface *root, struct rei_rect *out)
{
	return rei_xdg_surface_rect(root, out) || rei_xwayland_surface_rect(root, out);
}

bool rei_wm_surface_hidden(struct rei_surface *root)
{
	return rei_xdg_surface_hidden(root) || rei_xwayland_surface_hidden(root);
}

bool rei_view_visible(const struct rei_view *v)
{
	return !v->minimized && v->ws && v->ws->output && v->ws->output->workspace == v->ws;
}

struct rei_rect rei_view_rect(const struct rei_view *v)
{
	struct rei_rect g = view_geometry(v);
	return (struct rei_rect){ v->x, v->y, v->w > 0 ? v->w : g.w, v->h > 0 ? v->h : g.h };
}

static struct rei_workspace *ws_find(struct rei_server *s, int number)
{
	struct rei_workspace *ws;
	wl_list_for_each(ws, &s->workspaces, link) {
		if (ws->number == number)
			return ws;
	}
	return NULL;
}

static struct rei_workspace *ws_create(struct rei_server *s, int number, struct rei_output *output)
{
	struct rei_workspace *ws = rei_xcalloc(1, sizeof *ws);
	ws->number = number;
	ws->output = output;
	ws->tree = rei_tree_create(REI_LAYOUT_SPLIT_H);
	wl_list_init(&ws->floating);
	wl_list_init(&ws->focus);

	/* Keep the list sorted by number (the toolbar shows them in order). */
	struct wl_list *pos = &s->workspaces;
	struct rei_workspace *it;
	wl_list_for_each(it, &s->workspaces, link) {
		if (it->number > number)
			break;
		pos = &it->link;
	}
	wl_list_insert(pos, &ws->link);
	return ws;
}

static bool ws_empty(const struct rei_workspace *ws)
{
	/* Minimized windows are in `focus` but not in the tree. */
	return rei_tree_leaf_count(ws->tree) == 0 && wl_list_empty(&ws->floating) && wl_list_empty(&ws->focus);
}

static void ws_destroy(struct rei_workspace *ws)
{
	wl_list_remove(&ws->link);
	rei_tree_destroy(ws->tree);
	free(ws);
}

static int lowest_free_number(struct rei_server *s)
{
	for (int n = 1;; n++) {
		if (!ws_find(s, n))
			return n;
	}
}

/* ------------------------------------------------------------------------ */
/* Arranging: turn the tree and floating windows into rectangles + configures */
/* ------------------------------------------------------------------------ */

/* The highest a floating window's top edge may be: low enough that its notches
 * (which hover above it) stay on the screen. */
static int32_t min_window_y(const struct rei_output *o)
{
	return output_usable(o).y + o->server->config->notch_height;
}

static void apply_to_view(struct rei_view *v, struct rei_rect r, bool tiled, bool fullscreen)
{
	v->x = r.x;
	v->y = r.y;
	v->w = r.w;
	v->h = r.h;
	if (v->xwin)
		rei_xwindow_configure(v->xwin, r.x, r.y, r.w, r.h, fullscreen);
	else
		rei_xdg_toplevel_set_state(v->toplevel, r.w, r.h, tiled, fullscreen);
}

static void arrange(struct rei_output *o)
{
	struct rei_server *s = o->server;
	if (!o->workspace || s->shutting_down)
		return;
	struct rei_workspace *ws = o->workspace;
	const struct rei_config *cfg = s->config;

	/* Room for the notches: the focused window's tabs hover above it, so every
	 * tiled window keeps at least that much space above it, whether or not it is
	 * focused. That makes focus changes cost nothing in layout. */
	int notch = cfg->notch_height;
	struct rei_rect area = inset(output_usable(o), cfg->gap_outer);
	if (notch > cfg->gap_outer) {
		area.y += notch - cfg->gap_outer;
		area.h -= notch - cfg->gap_outer;
		if (area.h < 1)
			area.h = 1;
	}
	rei_tree_layout_xy(ws->tree, area, cfg->gap_inner, notch > cfg->gap_inner ? notch : cfg->gap_inner);

	size_t n = rei_tree_leaf_count(ws->tree);
	struct rei_node **leaves = rei_xcalloc(n ? n : 1, sizeof *leaves);
	rei_tree_leaves(ws->tree, leaves, n);
	for (size_t i = 0; i < n; i++) {
		struct rei_view *v = leaves[i]->data;
		apply_to_view(v, leaves[i]->rect, true, false);
	}
	free(leaves);

	struct rei_view *v;
	wl_list_for_each(v, &ws->floating, float_link) {
		if (v->minimized)
			continue;
		/* A floating window keeps whatever size its client chose until the user
		 * resizes it; then we dictate it. */
		/* If the notches would stick out above the screen, pull the window back
		 * down; the corrected position is what gets remembered. */
		int32_t min_y = min_window_y(o);
		if (notch > 0 && o->y + v->float_rect.y < min_y)
			v->float_rect.y = min_y - o->y;
		struct rei_rect r = { o->x + v->float_rect.x, o->y + v->float_rect.y,
				      v->float_sized ? v->float_rect.w : 0,
				      v->float_sized ? v->float_rect.h : 0 };
		apply_to_view(v, r, false, false);
	}

	if (ws->fullscreen)
		apply_to_view(ws->fullscreen, output_rect(o), false, true);

	rei_server_schedule_damage(s);
}

void rei_wm_arrange_all(struct rei_server *s)
{
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link)
		arrange(o);
}

static void arrange_ws(struct rei_workspace *ws)
{
	if (ws && ws->output && ws->output->workspace == ws)
		arrange(ws->output);
}

/* ------------------------------------------------------------------------ */
/* Focus                                                                    */
/* ------------------------------------------------------------------------ */

struct rei_view *rei_wm_active_view(struct rei_server *s)
{
	if (!s->focused_output || !s->focused_output->workspace)
		return NULL;
	struct rei_view *v;
	wl_list_for_each(v, &s->focused_output->workspace->focus, focus_link) {
		if (SHOWN(v))
			return v;
	}
	return NULL;
}

/* Exactly one window, the active one, shows as activated. */
static void update_activation(struct rei_server *s)
{
	struct rei_view *active = rei_wm_active_view(s);
	struct rei_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->xwin)
			rei_xwindow_set_activated(v->xwin, v == active);
		else
			rei_xdg_toplevel_set_activated(v->toplevel, v == active);
	rei_input_refocus(s);
	rei_server_schedule_damage(s);
}

void rei_wm_focus_view(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (!rei_view_visible(v) || !SHOWN(v))
		return;

	s->focused_output = v->ws->output;
	wl_list_remove(&v->focus_link);
	wl_list_insert(&v->ws->focus, &v->focus_link);
	if (v->floating) {  /* Focusing a floating window raises it. */
		wl_list_remove(&v->float_link);
		wl_list_insert(v->ws->floating.prev, &v->float_link);
	}
	update_activation(s);
}

void rei_wm_focus_output_at(struct rei_server *s, double x, double y)
{
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct rei_rect r = output_rect(o);
		if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
			if (s->focused_output != o) {
				s->focused_output = o;
				update_activation(s);
			}
			return;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Attaching windows to workspaces                                          */
/* ------------------------------------------------------------------------ */

/* The leaf a new tiled window opens next to: the active window's, if it is tiled. */
static struct rei_node *insertion_point(struct rei_workspace *ws)
{
	struct rei_view *v;
	wl_list_for_each(v, &ws->focus, focus_link) {
		if (SHOWN(v) && v->node)
			return v->node;
	}
	return NULL;
}

static void attach(struct rei_view *v, struct rei_workspace *ws, bool floating)
{
	v->ws = ws;
	wl_list_insert(ws->focus.prev, &v->focus_link);  /* Least recent until focused. */
	v->floating = floating;
	if (floating) {
		wl_list_insert(ws->floating.prev, &v->float_link);
	} else {
		v->node = rei_tree_add_leaf(ws->tree, insertion_point(ws), v);
	}
}

static void detach(struct rei_view *v)
{
	struct rei_workspace *ws = v->ws;
	if (!ws)
		return;
	if (v->node) {
		rei_tree_remove_leaf(ws->tree, v->node);
		v->node = NULL;
	}
	if (v->floating)
		wl_list_remove(&v->float_link);
	wl_list_remove(&v->focus_link);
	wl_list_init(&v->float_link);
	wl_list_init(&v->focus_link);
	if (ws->fullscreen == v)
		ws->fullscreen = NULL;
	v->fullscreen = false;
	v->ws = NULL;
}

/* Dialogs, and windows that can't be resized, are better off floating. */
static bool wants_float(struct rei_server *s, const struct rei_view *v)
{
	if (v->xwin)
		return s->config->float_new_windows || rei_xwindow_wants_float(v->xwin);
	const struct rei_xdg_toplevel *tl = v->toplevel;
	bool fixed_size = tl->min_w > 0 && tl->min_h > 0 &&
			  tl->min_w == tl->max_w && tl->min_h == tl->max_h;
	return s->config->float_new_windows || tl->has_parent || fixed_size;
}

struct rei_view *rei_view_create_x(struct rei_server *s, struct rei_xwindow *xwin)
{
	struct rei_view *v = rei_view_create(s, NULL);
	v->xwin = xwin;
	return v;
}

struct rei_view *rei_view_create(struct rei_server *s, struct rei_xdg_toplevel *toplevel)
{
	struct rei_view *v = rei_xcalloc(1, sizeof *v);
	v->server = s;
	v->toplevel = toplevel;
	wl_list_init(&v->float_link);
	wl_list_init(&v->focus_link);
	wl_list_insert(s->views.prev, &v->link);
	return v;
}

void rei_wm_add_view(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (!s->focused_output || !s->focused_output->workspace)
		return;  /* No output: adopted when one appears. */

	attach(v, s->focused_output->workspace, wants_float(s, v));
	if (view_wants_fullscreen(v))
		rei_wm_set_fullscreen(v, true);
	else
		arrange_ws(v->ws);
}

void rei_view_map(struct rei_view *v)
{
	struct rei_server *s = v->server;
	v->mapped = true;
	if (!v->ws)
		return;

	if (v->floating && !v->float_sized && v->float_rect.w == 0) {
		/* First appearance: centre it on its output, cascading a little so
		 * several new windows don't land exactly on top of each other. */
		struct rei_output *o = v->ws->output;
		struct rei_rect g = view_geometry(v);
		struct rei_rect or = output_usable(o);
		or.x -= o->x;  /* Floating positions are relative to the output. */
		or.y -= o->y;
		int count = 0;
		struct rei_view *it;
		wl_list_for_each(it, &v->ws->floating, float_link)
			count += it != v && it->mapped;
		int step = (count % CASCADE_SLOTS) * CASCADE_STEP;
		v->float_rect = (struct rei_rect){ or.x + (or.w - g.w) / 2 + step,
						   or.y + (or.h - g.h) / 2 + step, g.w, g.h };
		arrange(o);
	}

	if (rei_view_visible(v))
		rei_wm_focus_view(v);  /* New windows take the focus. */
	else
		rei_input_refocus(s);
	rei_server_schedule_damage(s);
}

void rei_view_destroy(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (s->input) {
		if (s->input->move_view == v)
			s->input->move_view = NULL;
		if (s->input->resize_view == v)
			s->input->resize_view = NULL;
	}

	if (s->notch_hover_view == v) {
		s->notch_hover_view = NULL;
		s->notch_hover_part = REI_NOTCH_NONE;
	}
	if (s->input && s->input->notch_view == v) {
		s->input->notch_view = NULL;
		s->input->notch_press = REI_NOTCH_NONE;
	}
	s->drop_hint.active = false;
	rei_tray_menu_view_gone(s, v);

	struct rei_workspace *ws = v->ws;
	detach(v);
	wl_list_remove(&v->link);
	struct rei_view *other;
	wl_list_for_each(other, &s->views, link)
		if (other->restore_anchor == v)
			other->restore_anchor = NULL;
	free(v);

	if (s->shutting_down)
		return;
	arrange_ws(ws);
	update_activation(s);  /* Focus falls back to the next most recent window. */
}

/* ------------------------------------------------------------------------ */
/* Outputs                                                                  */
/* ------------------------------------------------------------------------ */

void rei_wm_init(struct rei_server *s)
{
	wl_list_init(&s->workspaces);
}

void rei_wm_finish(struct rei_server *s)
{
	struct rei_workspace *ws, *tmp;
	wl_list_for_each_safe(ws, tmp, &s->workspaces, link)
		ws_destroy(ws);
}

void rei_wm_output_added(struct rei_server *s, struct rei_output *o)
{
	/* A workspace left without an output (all outputs were unplugged) comes
	 * back first; otherwise the output starts on the lowest free number. */
	struct rei_workspace *ws = NULL, *it;
	wl_list_for_each(it, &s->workspaces, link) {
		if (!it->output) {
			ws = it;
			break;
		}
	}
	if (!ws)
		ws = ws_create(s, lowest_free_number(s), o);
	ws->output = o;
	o->workspace = ws;
	if (!s->focused_output)
		s->focused_output = o;

	/* Windows that opened while there was no output. */
	struct rei_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!v->ws && s->focused_output->workspace)
			attach(v, s->focused_output->workspace, wants_float(s, v));
	}

	arrange(o);
	update_activation(s);
}

void rei_wm_output_removed(struct rei_server *s, struct rei_output *o)
{
	rei_tray_menu_close(s);
	struct rei_output *dest = NULL, *it;
	wl_list_for_each(it, &s->outputs, link) {
		if (it != o) {
			dest = it;
			break;
		}
	}

	struct rei_workspace *ws, *tmp;
	wl_list_for_each_safe(ws, tmp, &s->workspaces, link) {
		if (ws->output != o)
			continue;
		if (!dest) {
			ws->output = NULL;  /* Parked until an output returns. */
		} else if (ws_empty(ws)) {
			ws_destroy(ws);
		} else {
			ws->output = dest;  /* Floating positions are output-relative, so they follow. */
		}
	}
	o->workspace = NULL;
	if (s->focused_output == o)
		s->focused_output = dest;

	if (dest) {
		arrange(dest);
		update_activation(s);
	}
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                 */
/* ------------------------------------------------------------------------ */

/* The mapped windows of a workspace in a stable order: tiles, then floating. */
static size_t collect_views(struct rei_workspace *ws, struct rei_view **out, size_t cap)
{
	size_t n = 0;
	size_t leaves = rei_tree_leaf_count(ws->tree);
	struct rei_node **nodes = rei_xcalloc(leaves ? leaves : 1, sizeof *nodes);
	rei_tree_leaves(ws->tree, nodes, leaves);
	for (size_t i = 0; i < leaves && n < cap; i++) {
		struct rei_view *v = nodes[i]->data;
		if (SHOWN(v))
			out[n++] = v;
	}
	free(nodes);
	struct rei_view *v;
	wl_list_for_each(v, &ws->floating, float_link) {
		if (SHOWN(v) && n < cap)
			out[n++] = v;
	}
	return n;
}

/* The output nearest to `from` in a direction, or NULL. */
static struct rei_output *output_in_direction(struct rei_server *s, struct rei_output *from,
					      enum rei_direction dir)
{
	struct rei_rect fr = output_rect(from);
	struct rei_output *best = NULL, *o;
	double best_score = 0;
	wl_list_for_each(o, &s->outputs, link) {
		if (o == from)
			continue;
		struct rei_rect r = output_rect(o);
		double sc = rei_rect_direction_score(&fr, &r, dir);
		if (sc >= 0 && (!best || sc < best_score)) {
			best = o;
			best_score = sc;
		}
	}
	return best;
}

void rei_wm_focus_direction(struct rei_server *s, enum rei_direction dir)
{
	struct rei_view *active = rei_wm_active_view(s);
	if (active && !active->fullscreen) {
		struct rei_view *vs[256];
		size_t n = collect_views(active->ws, vs, 256);
		struct rei_rect from = rei_view_rect(active);
		struct rei_view *best = NULL;
		double best_score = 0;
		for (size_t i = 0; i < n; i++) {
			if (vs[i] == active)
				continue;
			struct rei_rect r = rei_view_rect(vs[i]);
			double sc = rei_rect_direction_score(&from, &r, dir);
			if (sc >= 0 && (!best || sc < best_score)) {
				best = vs[i];
				best_score = sc;
			}
		}
		if (best) {
			rei_wm_focus_view(best);
			return;
		}
	}

	/* Nothing further that way on this output: continue onto the next one. */
	if (s->focused_output) {
		struct rei_output *o = output_in_direction(s, s->focused_output, dir);
		if (o) {
			s->focused_output = o;
			update_activation(s);
		}
	}
}

void rei_wm_cycle(struct rei_server *s, bool next)
{
	struct rei_view *active = rei_wm_active_view(s);
	if (!active)
		return;
	struct rei_view *vs[256];
	size_t n = collect_views(active->ws, vs, 256);
	if (n < 2)
		return;
	size_t i = 0;
	while (i < n && vs[i] != active)
		i++;
	rei_wm_focus_view(vs[(i + (next ? 1 : n - 1)) % n]);
}

void rei_wm_split(struct rei_server *s, enum rei_layout layout)
{
	struct rei_view *v = rei_wm_active_view(s);
	if (!v || !v->node)
		return;
	rei_tree_split(v->ws->tree, v->node, layout);
	/* The window's own slot is unchanged until a neighbour arrives, but a lone
	 * window's parent may have changed direction. */
	arrange_ws(v->ws);
}

static void move_view_to_workspace(struct rei_view *v, struct rei_workspace *dest)
{
	struct rei_workspace *old = v->ws;
	bool floating = v->floating;
	bool was_mapped = v->mapped;
	detach(v);
	attach(v, dest, floating);
	/* Where it sits among the destination's windows follows the move. */
	if (was_mapped && rei_view_visible(v)) {
		wl_list_remove(&v->focus_link);
		wl_list_insert(&dest->focus, &v->focus_link);
	}
	arrange_ws(old);
	arrange_ws(dest);
}

void rei_wm_move_direction(struct rei_server *s, enum rei_direction dir)
{
	struct rei_view *v = rei_wm_active_view(s);
	if (!v)
		return;

	if (v->floating) {
		int dx = dir == REI_DIR_LEFT ? -FLOAT_NUDGE : dir == REI_DIR_RIGHT ? FLOAT_NUDGE : 0;
		int dy = dir == REI_DIR_UP ? -FLOAT_NUDGE : dir == REI_DIR_DOWN ? FLOAT_NUDGE : 0;
		rei_wm_float_move(v, v->x + dx, v->y + dy);
		rei_wm_float_settle(v);
		return;
	}
	if (v->fullscreen)
		return;

	if (rei_tree_move(v->ws->tree, v->node, dir)) {
		arrange_ws(v->ws);
		update_activation(s);
		return;
	}

	/* Already at the edge: hand the window to the neighbouring output. */
	struct rei_output *o = output_in_direction(s, v->ws->output, dir);
	if (o && o->workspace) {
		move_view_to_workspace(v, o->workspace);
		s->focused_output = o;
		rei_wm_focus_view(v);
	}
}

/* Takes a tiled window out of the grid, keeping its size and place. */
static void make_floating(struct rei_view *v)
{
	struct rei_workspace *ws = v->ws;
	struct rei_rect r = rei_view_rect(v);
	v->float_rect = (struct rei_rect){ r.x - ws->output->x, r.y - ws->output->y, r.w, r.h };
	v->float_sized = true;
	rei_tree_remove_leaf(ws->tree, v->node);
	v->node = NULL;
	v->floating = true;
	wl_list_insert(ws->floating.prev, &v->float_link);
}

void rei_wm_toggle_floating(struct rei_server *s)
{
	struct rei_view *v = rei_wm_active_view(s);
	if (!v || v->fullscreen)
		return;
	struct rei_workspace *ws = v->ws;

	if (v->floating) {
		wl_list_remove(&v->float_link);
		wl_list_init(&v->float_link);
		v->floating = false;
		v->node = rei_tree_add_leaf(ws->tree, insertion_point(ws), v);
	} else {
		make_floating(v);
	}
	arrange_ws(ws);
	rei_wm_focus_view(v);
}

void rei_wm_set_fullscreen(struct rei_view *v, bool on)
{
	if (v->xwin)
		rei_xwindow_set_fullscreen(v->xwin, on);
	else
		v->toplevel->want_fullscreen = on;
	if (!v->ws)
		return;
	struct rei_workspace *ws = v->ws;
	if (on) {
		if (ws->fullscreen && ws->fullscreen != v)
			ws->fullscreen->fullscreen = false;
		ws->fullscreen = v;
		v->fullscreen = true;
	} else if (ws->fullscreen == v) {
		ws->fullscreen = NULL;
		v->fullscreen = false;
	}
	arrange_ws(ws);
}

void rei_wm_toggle_fullscreen(struct rei_server *s)
{
	struct rei_view *v = rei_wm_active_view(s);
	if (v)
		rei_wm_set_fullscreen(v, !v->fullscreen);
}

void rei_wm_goto_workspace(struct rei_server *s, int number)
{
	struct rei_output *out = s->focused_output;
	if (!out)
		return;

	struct rei_workspace *ws = ws_find(s, number);
	if (ws && ws->output && ws->output->workspace == ws) {
		/* Already showing somewhere: just go there. */
		s->focused_output = ws->output;
		update_activation(s);
		return;
	}
	if (!ws)
		ws = ws_create(s, number, out);
	struct rei_output *target = ws->output ? ws->output : out;
	ws->output = target;

	struct rei_workspace *old = target->workspace;
	target->workspace = ws;
	s->focused_output = target;
	/* Like i3, an empty workspace vanishes once you leave it. */
	if (old && old != ws && ws_empty(old))
		ws_destroy(old);

	arrange(target);
	update_activation(s);
}

void rei_wm_move_to_workspace(struct rei_server *s, int number)
{
	struct rei_view *v = rei_wm_active_view(s);
	if (!v || !s->focused_output)
		return;
	struct rei_workspace *dest = ws_find(s, number);
	if (!dest)
		dest = ws_create(s, number, s->focused_output);  /* Hidden until visited. */
	if (dest == v->ws)
		return;
	move_view_to_workspace(v, dest);
	update_activation(s);
}

/* ------------------------------------------------------------------------ */
/* Minimize and restore                                                     */
/* ------------------------------------------------------------------------ */

static struct rei_node *edge_leaf(struct rei_node *n, bool last)
{
	while (n->is_split) {
		struct wl_list *e = last ? n->children.prev : n->children.next;
		if (e == &n->children)
			return NULL;
		n = wl_container_of(e, n, link);
	}
	return n;
}

/* Forgets everything that points at a window that is about to be hidden. */
static void drop_interaction(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (s->input) {
		if (s->input->move_view == v)
			s->input->move_view = NULL;
		if (s->input->resize_view == v)
			s->input->resize_view = NULL;
		if (s->input->notch_view == v) {
			s->input->notch_view = NULL;
			s->input->notch_press = REI_NOTCH_NONE;
		}
	}
	if (s->notch_hover_view == v) {
		s->notch_hover_view = NULL;
		s->notch_hover_part = REI_NOTCH_NONE;
	}
	s->drop_hint.active = false;
}

void rei_wm_minimize_view(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (!v->ws || !v->mapped || v->minimized)
		return;
	struct rei_workspace *ws = v->ws;
	drop_interaction(v);

	v->restore_anchor = NULL;
	v->restore_tiled = v->node != NULL;
	if (v->node) {
		/* Remember a neighbour and the side, so the window can go back to the same spot. */
		struct rei_node *n = v->node, *p = n->parent, *sib = NULL;
		bool before = false;
		if (p && n->link.prev != &p->children) {
			sib = wl_container_of(n->link.prev, sib, link);
			before = true;
		} else if (p && n->link.next != &p->children) {
			sib = wl_container_of(n->link.next, sib, link);
		}
		struct rei_node *leaf = sib ? edge_leaf(sib, before) : NULL;
		if (leaf && leaf->data) {
			v->restore_anchor = leaf->data;
			bool horizontal = p->layout == REI_LAYOUT_SPLIT_H;
			v->restore_side = horizontal ? (before ? REI_DIR_RIGHT : REI_DIR_LEFT)
						     : (before ? REI_DIR_DOWN : REI_DIR_UP);
		}
		rei_tree_remove_leaf(ws->tree, n);
		v->node = NULL;
	}
	if (ws->fullscreen == v) {
		ws->fullscreen = NULL;
		v->fullscreen = false;
	}
	/* The least recently focused, so something else takes the focus. */
	wl_list_remove(&v->focus_link);
	wl_list_insert(ws->focus.prev, &v->focus_link);
	v->minimized = true;

	arrange_ws(ws);
	update_activation(s);
	rei_server_schedule_damage(s);
}

static void restore_view(struct rei_view *v)
{
	struct rei_workspace *ws = v->ws;
	v->minimized = false;
	if (!v->floating && v->restore_tiled) {
		struct rei_view *a = v->restore_anchor;
		if (a && a->ws == ws && a->node && !a->minimized)
			v->node = rei_tree_insert_beside(ws->tree, a->node, v, v->restore_side);
		else
			v->node = rei_tree_add_leaf(ws->tree, insertion_point(ws), v);
	}
	v->restore_anchor = NULL;
	arrange_ws(ws);
}

void rei_wm_activate_view(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (!v->ws || !v->mapped)
		return;
	if (v->minimized)
		restore_view(v);
	struct rei_workspace *ws = v->ws;
	if (!(ws->output && ws->output->workspace == ws))
		rei_wm_goto_workspace(s, ws->number);
	rei_wm_focus_view(v);
	rei_server_schedule_damage(s);
}

void rei_wm_minimize_active(struct rei_server *s)
{
	struct rei_view *v = rei_wm_active_view(s);
	if (v)
		rei_wm_minimize_view(v);
}

/* ------------------------------------------------------------------------ */
/* Floating windows                                                         */
/* ------------------------------------------------------------------------ */

void rei_wm_float_move(struct rei_view *v, int32_t x, int32_t y)
{
	if (!v->floating || !v->ws)
		return;
	struct rei_output *o = v->ws->output;
	struct rei_rect cur = rei_view_rect(v);
	/* Remember a size so later toggles and resizes know what the window was. */
	if (v->float_rect.w == 0) {
		v->float_rect.w = cur.w;
		v->float_rect.h = cur.h;
	}
	/* The pull-back rule: dragging a window up past the point where its notches
	 * would leave the screen just stops it there. */
	if (v->server->config->notch_height > 0 && y < min_window_y(o))
		y = min_window_y(o);
	v->float_rect.x = x - o->x;
	v->float_rect.y = y - o->y;
	v->x = x;
	v->y = y;
	rei_server_schedule_damage(v->server);
}

void rei_wm_float_set_rect(struct rei_view *v, int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (!v->floating || !v->ws)
		return;
	struct rei_output *o = v->ws->output;
	v->float_rect = (struct rei_rect){ x - o->x, y - o->y, w, h };
	v->float_sized = true;
	arrange(o);  /* Also applies the pull-back rule. */
}

void rei_wm_float_settle(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (!v->floating || !v->ws)
		return;
	struct rei_rect r = rei_view_rect(v);
	double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;

	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct rei_rect or = output_rect(o);
		if (cx < or.x || cy < or.y || cx >= or.x + or.w || cy >= or.y + or.h)
			continue;
		if (o != v->ws->output && o->workspace) {
			/* Keep the same screen position; only the owner changes. */
			struct rei_rect keep = r;
			bool sized = v->float_sized;
			move_view_to_workspace(v, o->workspace);
			v->float_rect = (struct rei_rect){ keep.x - o->x, keep.y - o->y, keep.w, keep.h };
			v->float_sized = sized;
			s->focused_output = o;
			arrange(o);
			rei_wm_focus_view(v);
		}
		return;
	}
}

/* ------------------------------------------------------------------------ */
/* Notches                                                                  */
/* ------------------------------------------------------------------------ */

/* Only the focused window shows notches (and never when fullscreen). */
static struct rei_view *notch_view(struct rei_server *s)
{
	if (s->config->notch_height <= 0)
		return NULL;
	struct rei_view *v = rei_wm_active_view(s);
	return (v && !v->fullscreen && rei_view_visible(v)) ? v : NULL;
}

static void notch_geom(const struct rei_view *v, struct rei_notch_geometry *g)
{
	rei_notch_geometry(rei_view_rect(v), v->server->config->notch_height, g);
}

bool rei_wm_notch_at(struct rei_server *s, double x, double y,
		     struct rei_view **view, enum rei_notch_part *part)
{
	struct rei_view *v = notch_view(s);
	if (!v)
		return false;
	struct rei_notch_geometry g;
	notch_geom(v, &g);
	enum rei_notch_part p = rei_notch_hit(&g, x, y);
	if (p == REI_NOTCH_NONE)
		return false;
	*view = v;
	*part = p;
	return true;
}

void rei_wm_set_notch_hover(struct rei_server *s, struct rei_view *v, enum rei_notch_part part)
{
	if (s->notch_hover_view == v && s->notch_hover_part == part)
		return;
	s->notch_hover_view = v;
	s->notch_hover_part = part;
	rei_server_schedule_damage(s);
}

void rei_wm_notch_activate(struct rei_server *s, struct rei_view *v, enum rei_notch_part part)
{
	switch (part) {
	case REI_NOTCH_CLOSE:
		rei_wm_close_view(v);
		break;
	case REI_NOTCH_MINIMIZE:
		rei_wm_minimize_view(v);
		break;
	case REI_NOTCH_MAXIMIZE:
		rei_debug("notch: maximize is not implemented yet");
		break;
	default:
		break;
	}
	(void)s;
}

static void emit_tab(struct rei_scene_snapshot *snap, struct rei_pixbuf *art, struct rei_rect r)
{
	if (art)
		rei_snapshot_add(snap, art, (float)r.x, (float)r.y, (float)r.w, (float)r.h, false);
}

void rei_wm_emit_decorations(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	if (s->drop_hint.active && rei_drop_hint_art()) {
		const struct rei_rect *r = &s->drop_hint.rect;
		rei_snapshot_add(snap, rei_drop_hint_art(), (float)r->x, (float)r->y,
				 (float)r->w, (float)r->h, false);
	}

	struct rei_view *v = notch_view(s);
	if (!v)
		return;
	struct rei_notch_geometry g;
	notch_geom(v, &g);
	int h = s->config->notch_height;
	int scale = (int)ceil(v->ws->output->scale);
	enum rei_notch_part hover = s->notch_hover_view == v ? s->notch_hover_part : REI_NOTCH_NONE;

	if (g.has_buttons)
		emit_tab(snap, rei_notch_art_buttons(h, scale, hover), g.buttons);
	if (g.has_handle)
		emit_tab(snap, rei_notch_art_handle(h, scale, hover), g.handle);
}

/* ------------------------------------------------------------------------ */
/* Dragging a tiled window by its handle                                    */
/* ------------------------------------------------------------------------ */

bool rei_wm_drag_float(struct rei_view *v)
{
	if (v->floating)
		return true;
	if (v->fullscreen || !v->ws || !v->node)
		return false;
	struct rei_workspace *ws = v->ws;
	make_floating(v);
	v->drag_from_tiled = true;
	arrange_ws(ws);  /* The others close up around the gap. */
	rei_wm_focus_view(v);
	return true;
}

/* The tiled window under (x, y) and the side of it nearest the pointer. */
static struct rei_view *find_drop_target(struct rei_server *s, const struct rei_view *dragged,
					 double x, double y, enum rei_direction *dir,
					 struct rei_rect *hint)
{
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct rei_rect or = output_rect(o);
		if (x < or.x || y < or.y || x >= or.x + or.w || y >= or.y + or.h)
			continue;
		struct rei_workspace *ws = o->workspace;
		if (!ws || ws->fullscreen)
			return NULL;

		size_t n = rei_tree_leaf_count(ws->tree);
		struct rei_node **leaves = rei_xcalloc(n ? n : 1, sizeof *leaves);
		rei_tree_leaves(ws->tree, leaves, n);
		struct rei_view *found = NULL;
		for (size_t i = 0; i < n && !found; i++) {
			struct rei_view *tv = leaves[i]->data;
			struct rei_rect r = leaves[i]->rect;
			if (tv == dragged || !SHOWN(tv))
				continue;
			if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h)
				continue;

			/* The nearest edge decides the side, measured relative to the size
			 * so wide and narrow windows behave alike. */
			double fx = (x - r.x) / r.w, fy = (y - r.y) / r.h;
			double dl = fx, dr = 1 - fx, dt = fy, db = 1 - fy;
			double m = fmin(fmin(dl, dr), fmin(dt, db));
			if (m == dl) { *dir = REI_DIR_LEFT; *hint = (struct rei_rect){ r.x, r.y, r.w / 2, r.h }; }
			else if (m == dr) { *dir = REI_DIR_RIGHT; *hint = (struct rei_rect){ r.x + r.w / 2, r.y, r.w - r.w / 2, r.h }; }
			else if (m == dt) { *dir = REI_DIR_UP; *hint = (struct rei_rect){ r.x, r.y, r.w, r.h / 2 }; }
			else { *dir = REI_DIR_DOWN; *hint = (struct rei_rect){ r.x, r.y + r.h / 2, r.w, r.h - r.h / 2 }; }
			found = tv;
		}
		free(leaves);
		return found;
	}
	return NULL;
}

void rei_wm_drag_update(struct rei_view *v, double x, double y)
{
	struct rei_server *s = v->server;
	if (!v->drag_from_tiled)
		return;
	enum rei_direction dir;
	struct rei_rect hint;
	bool was = s->drop_hint.active;
	struct rei_view *target = find_drop_target(s, v, x, y, &dir, &hint);
	s->drop_hint.active = target != NULL;
	if (target)
		s->drop_hint.rect = hint;
	if (was || target)
		rei_server_schedule_damage(s);
}

void rei_wm_drag_finish(struct rei_view *v, double x, double y)
{
	struct rei_server *s = v->server;
	s->drop_hint.active = false;
	rei_server_schedule_damage(s);

	if (!v->floating)
		return;  /* The drag never began (a click), or the window went away. */

	if (v->drag_from_tiled) {
		v->drag_from_tiled = false;
		enum rei_direction dir;
		struct rei_rect hint;
		struct rei_view *target = find_drop_target(s, v, x, y, &dir, &hint);
		if (target && target->ws && target->node) {
			/* Into the grid, beside the window it was dropped on. */
			struct rei_workspace *old = v->ws, *dest = target->ws;
			detach(v);
			v->ws = dest;
			v->floating = false;
			wl_list_insert(dest->focus.prev, &v->focus_link);
			v->node = rei_tree_insert_beside(dest->tree, target->node, v, dir);
			arrange_ws(old);
			arrange_ws(dest);
			s->focused_output = dest->output;
			rei_wm_focus_view(v);
			return;
		}
		/* Dropped on empty space or a gap: it stays floating where it is. */
	}
	rei_wm_float_settle(v);
}

/* ------------------------------------------------------------------------ */
/* Scene and hit-testing                                                    */
/* ------------------------------------------------------------------------ */

static void emit_view(struct rei_view *v, struct rei_scene_snapshot *snap, bool clip)
{
	if (!SHOWN(v))
		return;
	size_t first = snap->n_items;
	if (v->xwin)
		rei_xwindow_emit(v, snap);
	else
		rei_xdg_emit_view(v, snap);
	if (clip) {
		/* A tiled window must stay inside its slot even while its client is
		 * still redrawing at the old size. */
		struct rei_rect r = rei_view_rect(v);
		rei_snapshot_clip_from(snap, first, (float)r.x, (float)r.y, (float)r.w, (float)r.h);
	}
}

void rei_wm_emit_views(struct rei_server *s, struct rei_output *o, struct rei_scene_snapshot *snap)
{
	(void)s;
	struct rei_workspace *ws = o->workspace;
	if (!ws)
		return;

	size_t n = rei_tree_leaf_count(ws->tree);
	struct rei_node **leaves = rei_xcalloc(n ? n : 1, sizeof *leaves);
	rei_tree_leaves(ws->tree, leaves, n);
	for (size_t i = 0; i < n; i++) {
		struct rei_view *v = leaves[i]->data;
		if (v != ws->fullscreen)
			emit_view(v, snap, true);
	}
	free(leaves);

	struct rei_view *v;
	wl_list_for_each(v, &ws->floating, float_link) {
		if (v != ws->fullscreen)
			emit_view(v, snap, false);
	}
	if (ws->fullscreen)
		emit_view(ws->fullscreen, snap, true);
}

static bool view_pick(const struct rei_view *v, double x, double y, struct rei_surface **out,
		      double *sx, double *sy)
{
	return v->xwin ? rei_xwindow_pick(v, x, y, out, sx, sy) : rei_xdg_pick_view(v, x, y, out, sx, sy);
}

bool rei_wm_pick(struct rei_server *s, double x, double y,
		 struct rei_surface **surface, double *sx, double *sy)
{
	if (rei_xdg_pick_popups(s, x, y, surface, sx, sy) ||
	    rei_xwayland_pick_unmanaged(s, x, y, surface, sx, sy))
		return true;

	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct rei_workspace *ws = o->workspace;
		if (!ws)
			continue;
		if (ws->fullscreen) {
			if (view_pick(ws->fullscreen, x, y, surface, sx, sy))
				return true;
			continue;
		}
		struct rei_view *v;
		wl_list_for_each_reverse(v, &ws->floating, float_link) {
			if (SHOWN(v) && view_pick(v, x, y, surface, sx, sy))
				return true;
		}
		size_t n = rei_tree_leaf_count(ws->tree);
		struct rei_node **leaves = rei_xcalloc(n ? n : 1, sizeof *leaves);
		rei_tree_leaves(ws->tree, leaves, n);
		bool hit = false;
		for (size_t i = 0; i < n && !hit; i++) {
			struct rei_view *tv = leaves[i]->data;
			/* Only the visible part: a tiled window is clipped to its slot. */
			struct rei_rect r = rei_view_rect(tv);
			if (SHOWN(tv) && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h)
				hit = view_pick(tv, x, y, surface, sx, sy);
		}
		free(leaves);
		if (hit)
			return true;
	}
	*surface = NULL;
	return false;
}

struct rei_surface *rei_wm_keyboard_surface(struct rei_server *s)
{
	struct rei_view *v = rei_wm_active_view(s);
	return v ? view_surface(v) : NULL;
}

struct rei_view *rei_wm_view_for_surface(struct rei_server *s, struct rei_surface *surface)
{
	(void)s;
	struct rei_view *v = rei_xdg_view_of(surface);
	return v ? v : rei_xwayland_view_of(surface);
}
