#include "proto/idle_inhibit.h"

#include <stdlib.h>
#include <wayland-server.h>

#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "idle.h"
#include "server.h"
#include "util/util.h"

struct inhibitor {
	struct rei_server *server;
	struct wl_resource *resource;
};

static void inhibitor_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {
	.destroy = inhibitor_destroy_request,
};

static void inhibitor_resource_destroyed(struct wl_resource *r)
{
	struct inhibitor *i = wl_resource_get_user_data(r);
	rei_idle_inhibit_add(i->server, -1);
	free(i);
}

static void manager_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void manager_create_inhibitor(struct wl_client *client, struct wl_resource *manager, uint32_t id,
				     struct wl_resource *surface)
{
	(void)surface;
	struct inhibitor *i = rei_xcalloc(1, sizeof *i);
	i->server = wl_resource_get_user_data(manager);
	i->resource = wl_resource_create(client, &zwp_idle_inhibitor_v1_interface, wl_resource_get_version(manager), id);
	if (!i->resource) {
		free(i);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(i->resource, &inhibitor_impl, i, inhibitor_resource_destroyed);
	rei_idle_inhibit_add(i->server, 1);
}

static const struct zwp_idle_inhibit_manager_v1_interface manager_impl = {
	.destroy = manager_destroy_request,
	.create_inhibitor = manager_create_inhibitor,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &zwp_idle_inhibit_manager_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

bool rei_idle_inhibit_init(struct rei_server *s)
{
	return wl_global_create(s->display, &zwp_idle_inhibit_manager_v1_interface, 1, s, bind_manager) != NULL;
}
