#include "desktop.h"

#include <stdlib.h>

#include "input/input.h"
#include "proto/xdg_shell.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define CASCADE_STEP 30
#define CASCADE_SLOTS 8

struct rei_view *rei_view_create(struct rei_server *server, struct rei_xdg_toplevel *toplevel)
{
	struct rei_view *v = rei_xcalloc(1, sizeof *v);
	v->server = server;
	wl_list_init(&v->link);
	v->toplevel = toplevel;
	return v;
}

struct rei_view *rei_desktop_top_view(struct rei_server *s)
{
	if (wl_list_empty(&s->views))
		return NULL;
	struct rei_view *v;
	return wl_container_of(s->views.prev, v, link);
}

void rei_view_map(struct rei_view *v)
{
	struct rei_server *s = v->server;
	struct rei_rect g = rei_xdg_toplevel_geometry(v->toplevel);

	int32_t ox = 0, oy = 0, ow = 1920, oh = 1080;
	if (!wl_list_empty(&s->outputs)) {
		struct rei_output *o = wl_container_of(s->outputs.next, o, link);
		ox = o->x;
		oy = o->y;
		rei_output_logical_size(o, &ow, &oh);
	}

	int mapped = 0;
	struct rei_view *it;
	wl_list_for_each(it, &s->views, link)
		mapped++;
	int32_t step = (mapped % CASCADE_SLOTS) * CASCADE_STEP;

	v->x = ox + (ow - g.w) / 2 + step;
	v->y = oy + (oh - g.h) / 2 + step;

	struct rei_view *prev = rei_desktop_top_view(s);
	wl_list_insert(s->views.prev, &v->link);
	if (prev)
		rei_xdg_toplevel_set_activated(prev->toplevel, false);
	rei_xdg_toplevel_set_activated(v->toplevel, true);
	rei_input_refocus(s);
	rei_server_schedule_damage(s);
}

void rei_view_destroy(struct rei_view *v)
{
	struct rei_server *s = v->server;
	if (s->input && s->input->move_view == v)
		s->input->move_view = NULL;

	bool was_top = !wl_list_empty(&v->link) && s->views.prev == &v->link;
	if (!wl_list_empty(&v->link))
		wl_list_remove(&v->link);
	free(v);

	if (was_top) {
		struct rei_view *next = rei_desktop_top_view(s);
		if (next)
			rei_xdg_toplevel_set_activated(next->toplevel, true);
	}
	rei_input_refocus(s);
	rei_server_schedule_damage(s);
}

void rei_view_activate(struct rei_view *v)
{
	struct rei_server *s = v->server;
	struct rei_view *top = rei_desktop_top_view(s);
	if (top != v) {
		if (top)
			rei_xdg_toplevel_set_activated(top->toplevel, false);
		wl_list_remove(&v->link);
		wl_list_insert(s->views.prev, &v->link);
		rei_xdg_toplevel_set_activated(v->toplevel, true);
		rei_server_schedule_damage(s);
	}
	rei_input_refocus(s);
}

void rei_view_move_to(struct rei_view *v, int32_t x, int32_t y)
{
	if (v->x == x && v->y == y)
		return;
	v->x = x;
	v->y = y;
	rei_server_schedule_damage(v->server);
}

void rei_desktop_cycle(struct rei_server *s, bool next)
{
	if (wl_list_empty(&s->views) || s->views.next == s->views.prev)
		return;  /* Zero or one window: nothing to cycle. */

	struct rei_view *v;
	if (next) {
		v = wl_container_of(s->views.next, v, link);  /* Bottom. */
		rei_view_activate(v);
	} else {
		struct rei_view *top = rei_desktop_top_view(s);
		rei_xdg_toplevel_set_activated(top->toplevel, false);
		wl_list_remove(&top->link);
		wl_list_insert(&s->views, &top->link);  /* To the bottom. */
		struct rei_view *new_top = rei_desktop_top_view(s);
		rei_xdg_toplevel_set_activated(new_top->toplevel, true);
		rei_input_refocus(s);
		rei_server_schedule_damage(s);
	}
}

struct rei_view *rei_desktop_view_for_surface(struct rei_server *s, struct rei_surface *surface)
{
	(void)s;
	return rei_xdg_view_of(surface);
}

struct rei_surface *rei_desktop_keyboard_surface(struct rei_server *s)
{
	struct rei_view *top = rei_desktop_top_view(s);
	return top ? rei_xdg_toplevel_surface(top->toplevel) : NULL;
}

bool rei_desktop_pick(struct rei_server *s, double x, double y,
		      struct rei_surface **surface, double *sx, double *sy)
{
	/* The list is bottom to top, so walk it backwards. */
	const struct rei_view *v;
	wl_list_for_each_reverse(v, &s->views, link) {
		if (rei_xdg_pick_view(v, x, y, surface, sx, sy))
			return true;
	}
	*surface = NULL;
	return false;
}
