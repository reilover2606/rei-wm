#include "desktop.h"

#include <stdlib.h>

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

static struct rei_view *top_view(struct rei_server *s)
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

	struct rei_view *prev = top_view(s);
	wl_list_insert(s->views.prev, &v->link);
	if (prev)
		rei_xdg_toplevel_set_activated(prev->toplevel, false);
	rei_xdg_toplevel_set_activated(v->toplevel, true);
	rei_server_schedule_damage(s);
}

void rei_view_destroy(struct rei_view *v)
{
	struct rei_server *s = v->server;
	bool was_top = !wl_list_empty(&v->link) && s->views.prev == &v->link;
	if (!wl_list_empty(&v->link))
		wl_list_remove(&v->link);
	free(v);

	if (was_top) {
		struct rei_view *next = top_view(s);
		if (next)
			rei_xdg_toplevel_set_activated(next->toplevel, true);
	}
	rei_server_schedule_damage(s);
}
