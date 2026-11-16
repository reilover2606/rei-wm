#include "proto/fractional_scale.h"

#include <math.h>
#include <stdlib.h>
#include <wayland-server.h>

#include "desktop.h"
#include "fractional-scale-v1-server-protocol.h"
#include "output.h"
#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_fractional {
	struct rei_server *server;
	struct wl_resource *resource;
	struct rei_surface *surface;   /* NULL once the wl_surface is gone */
	struct wl_listener surface_destroy;
	struct wl_list link;           /* rei_server.fractional_scales */
	uint32_t sent;                 /* the last preferred_scale, 120ths; 0 = none yet */
};

/* The scale of the output a surface is on: its window's output, else the focused one. */
static double scale_for(struct rei_server *s, struct rei_surface *surface)
{
	struct rei_view *v = rei_wm_view_for_surface(s, rei_surface_root(surface));
	const struct rei_output *o = v && v->ws ? v->ws->output : NULL;
	if (!o)
		o = s->focused_output;
	if (!o && !wl_list_empty(&s->outputs))
		o = wl_container_of(s->outputs.next, o, link);
	return o && o->scale > 0 ? o->scale : 1.0;
}

static void send_if_changed(struct rei_fractional *f)
{
	if (!f->surface)
		return;
	uint32_t want = (uint32_t)lround(scale_for(f->server, f->surface) * 120.0);
	if (want == f->sent)
		return;
	f->sent = want;
	wp_fractional_scale_v1_send_preferred_scale(f->resource, want);
}

void rei_fractional_scale_update_all(struct rei_server *s)
{
	struct rei_fractional *f;
	wl_list_for_each(f, &s->fractional_scales, link)
		send_if_changed(f);
}

static void on_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_fractional *f = wl_container_of(l, f, surface_destroy);
	wl_list_remove(&f->surface_destroy.link);
	wl_list_init(&f->surface_destroy.link);
	f->surface = NULL;
}

static void fractional_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wp_fractional_scale_v1_interface fractional_impl = {
	.destroy = fractional_destroy_request,
};

static void fractional_resource_destroy(struct wl_resource *r)
{
	struct rei_fractional *f = wl_resource_get_user_data(r);
	wl_list_remove(&f->link);
	if (f->surface)
		wl_list_remove(&f->surface_destroy.link);
	free(f);
}

static void manager_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void manager_get_fractional_scale(struct wl_client *client, struct wl_resource *manager, uint32_t id,
					 struct wl_resource *surface_res)
{
	struct rei_server *server = wl_resource_get_user_data(manager);
	struct rei_surface *s = rei_surface_from_resource(surface_res);
	struct rei_fractional *other;
	wl_list_for_each(other, &server->fractional_scales, link) {
		if (other->surface == s) {
			wl_resource_post_error(manager, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
					       "the surface already has a fractional scale object");
			return;
		}
	}
	struct wl_resource *res = wl_resource_create(client, &wp_fractional_scale_v1_interface,
						     wl_resource_get_version(manager), id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_fractional *f = rei_xcalloc(1, sizeof *f);
	f->server = server;
	f->resource = res;
	f->surface = s;
	f->surface_destroy.notify = on_surface_destroyed;
	wl_signal_add(&s->destroy_signal, &f->surface_destroy);
	wl_list_insert(&server->fractional_scales, &f->link);
	wl_resource_set_implementation(res, &fractional_impl, f, fractional_resource_destroy);
	send_if_changed(f);   /* the client learns the scale right away */
}

static const struct wp_fractional_scale_manager_v1_interface manager_impl = {
	.destroy = manager_destroy_request,
	.get_fractional_scale = manager_get_fractional_scale,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &wp_fractional_scale_manager_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

bool rei_fractional_scale_init(struct rei_server *server)
{
	wl_list_init(&server->fractional_scales);
	return wl_global_create(server->display, &wp_fractional_scale_manager_v1_interface, 1, server,
				manager_bind) != NULL;
}
