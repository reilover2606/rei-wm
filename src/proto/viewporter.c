#include <math.h>
#include <stdlib.h>
#include <wayland-server.h>

#include "proto/surface.h"
#include "proto/viewporter.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "util/wlsignal.h"
#include "viewporter-server-protocol.h"

/*
 * wp_viewporter: lets a client say what logical size its buffer is shown at (the destination) and
 * which part of the buffer is shown (the source). A fractional-scale client draws 1.4x the pixels
 * and uses the destination to say "this is 1371 logical pixels wide". The destination is honoured;
 * the source rectangle is only used for its size when there is no destination (no cropping).
 */

struct rei_viewport {
	struct wl_resource *resource;
	struct rei_surface *surface;     /* NULL once the wl_surface is gone */
	struct wl_listener surface_destroy;
};

static void on_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_viewport *v = wl_container_of(l, v, surface_destroy);
	wl_list_remove(&v->surface_destroy.link);
	wl_list_init(&v->surface_destroy.link);
	v->surface = NULL;
}

static void viewport_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void viewport_set_source(struct wl_client *c, struct wl_resource *r, wl_fixed_t x, wl_fixed_t y,
				wl_fixed_t w, wl_fixed_t h)
{
	(void)c;
	struct rei_viewport *v = wl_resource_get_user_data(r);
	if (!v->surface) {
		wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE, "the wl_surface was destroyed");
		return;
	}
	struct rei_surface_state *p = &v->surface->pending;
	if (x == wl_fixed_from_int(-1) && y == wl_fixed_from_int(-1) && w == wl_fixed_from_int(-1) &&
	    h == wl_fixed_from_int(-1)) {
		p->src_w = p->src_h = -1;
	} else if (x < 0 || y < 0 || w <= 0 || h <= 0) {
		wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid source rectangle");
		return;
	} else {
		p->src_w = (int32_t)lround(wl_fixed_to_double(w));
		p->src_h = (int32_t)lround(wl_fixed_to_double(h));
	}
	if (!p->viewport_set) {   /* keep the current destination unless this commit changes it */
		p->dst_w = v->surface->dst_w;
		p->dst_h = v->surface->dst_h;
	}
	p->viewport_set = true;
}

static void viewport_set_destination(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
	(void)c;
	struct rei_viewport *v = wl_resource_get_user_data(r);
	if (!v->surface) {
		wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE, "the wl_surface was destroyed");
		return;
	}
	struct rei_surface_state *p = &v->surface->pending;
	if (w == -1 && h == -1) {
		p->dst_w = p->dst_h = -1;
	} else if (w <= 0 || h <= 0) {
		wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid destination size");
		return;
	} else {
		p->dst_w = w;
		p->dst_h = h;
	}
	if (!p->viewport_set) {
		p->src_w = v->surface->src_w;
		p->src_h = v->surface->src_h;
	}
	p->viewport_set = true;
}

static const struct wp_viewport_interface viewport_impl = {
	.destroy = viewport_destroy_request,
	.set_source = viewport_set_source,
	.set_destination = viewport_set_destination,
};

static void viewport_resource_destroy(struct wl_resource *r)
{
	struct rei_viewport *v = wl_resource_get_user_data(r);
	if (v->surface) {
		/* Destroying the viewport unsets both, from the next commit. */
		struct rei_surface_state *p = &v->surface->pending;
		p->dst_w = p->dst_h = p->src_w = p->src_h = -1;
		p->viewport_set = true;
		v->surface->viewport = NULL;
		wl_list_remove(&v->surface_destroy.link);
	}
	free(v);
}

static void viewporter_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void viewporter_get_viewport(struct wl_client *client, struct wl_resource *r, uint32_t id,
				    struct wl_resource *surface_res)
{
	struct rei_surface *s = rei_surface_from_resource(surface_res);
	if (s->viewport) {
		wl_resource_post_error(r, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
				       "the surface already has a viewport");
		return;
	}
	struct wl_resource *res = wl_resource_create(client, &wp_viewport_interface, wl_resource_get_version(r), id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_viewport *v = rei_xcalloc(1, sizeof *v);
	v->resource = res;
	v->surface = s;
	v->surface_destroy.notify = on_surface_destroyed;
	wl_signal_add(&s->destroy_signal, &v->surface_destroy);
	s->viewport = res;
	wl_resource_set_implementation(res, &viewport_impl, v, viewport_resource_destroy);
}

static const struct wp_viewporter_interface viewporter_impl = {
	.destroy = viewporter_destroy_request,
	.get_viewport = viewporter_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &wp_viewporter_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &viewporter_impl, data, NULL);
}

bool rei_viewporter_init(struct rei_server *server)
{
	return wl_global_create(server->display, &wp_viewporter_interface, 1, server, viewporter_bind) != NULL;
}
