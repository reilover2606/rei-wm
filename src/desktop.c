#include "desktop.h"

#include <stdlib.h>

#include "input/input.h"
#include "proto/xdg_shell.h"
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

/* The part of an output windows may be tiled into, before the outer gap. The
 * toolbar will claim its edge from this later. */
static struct rei_rect output_usable(const struct rei_output *o)
{
	return output_rect(o);
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

bool rei_view_visible(const struct rei_view *v)
{
	return v->ws && v->ws->output && v->ws->output->workspace == v->ws;
}

struct rei_rect rei_view_rect(const struct rei_view *v)
{
	struct rei_rect g = rei_xdg_toplevel_geometry(v->toplevel);
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
	return rei_tree_leaf_count(ws->tree) == 0 && wl_list_empty(&ws->floating);
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

static void apply_to_view(struct rei_view *v, struct rei_rect r, bool tiled, bool fullscreen)
{
	v->x = r.x;
	v->y = r.y;
	v->w = r.w;
	v->h = r.h;
	rei_xdg_toplevel_set_state(v->toplevel, r.w, r.h, tiled, fullscreen);
}

static void arrange(struct rei_output *o)
{
	struct rei_server *s = o->server;
	if (!o->workspace || s->shutting_down)
		return;
	struct rei_workspace *ws = o->workspace;
	const struct rei_config *cfg = s->config;

	struct rei_rect area = inset(output_usable(o), cfg->gap_outer);
	rei_tree_layout(ws->tree, area, cfg->gap_inner);

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
		/* A floating window keeps whatever size its client chose until the user
		 * resizes it; then we dictate it. */
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
		if (v->mapped)
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
		rei_xdg_toplevel_set_activated(v->toplevel, v == active);
	rei_input_refocus(s);
	rei_server_schedule_damage(s);
}

void rei_wm_focus_view(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (!rei_view_visible(v) || !v->mapped)
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
		if (v->mapped && v->node)
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
	const struct rei_xdg_toplevel *tl = v->toplevel;
	bool fixed_size = tl->min_w > 0 && tl->min_h > 0 &&
			  tl->min_w == tl->max_w && tl->min_h == tl->max_h;
	return s->config->float_new_windows || tl->has_parent || fixed_size;
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
	if (v->toplevel->want_fullscreen)
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
		struct rei_rect g = rei_xdg_toplevel_geometry(v->toplevel);
		struct rei_rect or = output_rect(o);
		int count = 0;
		struct rei_view *it;
		wl_list_for_each(it, &v->ws->floating, float_link)
			count += it != v && it->mapped;
		int step = (count % CASCADE_SLOTS) * CASCADE_STEP;
		v->float_rect = (struct rei_rect){ (or.w - g.w) / 2 + step, (or.h - g.h) / 2 + step,
						   g.w, g.h };
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

	struct rei_workspace *ws = v->ws;
	detach(v);
	wl_list_remove(&v->link);
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
		if (v->mapped)
			out[n++] = v;
	}
	free(nodes);
	struct rei_view *v;
	wl_list_for_each(v, &ws->floating, float_link) {
		if (v->mapped && n < cap)
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
		/* Keep the size it had in the grid, so the window doesn't jump. */
		struct rei_rect r = rei_view_rect(v);
		v->float_rect = (struct rei_rect){ r.x - ws->output->x, r.y - ws->output->y, r.w, r.h };
		v->float_sized = true;
		rei_tree_remove_leaf(ws->tree, v->node);
		v->node = NULL;
		v->floating = true;
		wl_list_insert(ws->floating.prev, &v->float_link);
	}
	arrange_ws(ws);
	rei_wm_focus_view(v);
}

void rei_wm_set_fullscreen(struct rei_view *v, bool on)
{
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
	arrange(o);
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
/* Scene and hit-testing                                                    */
/* ------------------------------------------------------------------------ */

static void emit_view(struct rei_view *v, struct rei_scene_snapshot *snap, bool clip)
{
	if (!v->mapped)
		return;
	size_t first = snap->n_items;
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

bool rei_wm_pick(struct rei_server *s, double x, double y,
		 struct rei_surface **surface, double *sx, double *sy)
{
	if (rei_xdg_pick_popups(s, x, y, surface, sx, sy))
		return true;

	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct rei_workspace *ws = o->workspace;
		if (!ws)
			continue;
		if (ws->fullscreen) {
			if (rei_xdg_pick_view(ws->fullscreen, x, y, surface, sx, sy))
				return true;
			continue;
		}
		struct rei_view *v;
		wl_list_for_each_reverse(v, &ws->floating, float_link) {
			if (v->mapped && rei_xdg_pick_view(v, x, y, surface, sx, sy))
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
			if (tv->mapped && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h)
				hit = rei_xdg_pick_view(tv, x, y, surface, sx, sy);
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
	return v ? rei_xdg_toplevel_surface(v->toplevel) : NULL;
}

struct rei_view *rei_wm_view_for_surface(struct rei_server *s, struct rei_surface *surface)
{
	(void)s;
	return rei_xdg_view_of(surface);
}
