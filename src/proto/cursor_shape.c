#include "proto/cursor_shape.h"

#include <wayland-server.h>

#include "cursor.h"
#include "cursor-shape-v1-server-protocol.h"
#include "pointer_cursor.h"
#include "server.h"

static void destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void device_set_shape(struct wl_client *client, struct wl_resource *r, uint32_t serial, uint32_t shape)
{
	(void)serial;
	if (shape < 1 || shape > WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE) {
		wl_resource_post_error(r, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE, "invalid shape %u", shape);
		return;
	}
	struct rei_server *s = wl_resource_get_user_data(r);
	rei_pointer_cursor_request_shape(s, client, (enum rei_cursor_shape)shape);
}

static const struct wp_cursor_shape_device_v1_interface device_impl = {
	.destroy = destroy_request,
	.set_shape = device_set_shape,
};

static void make_device(struct wl_client *client, struct wl_resource *mgr, uint32_t id, struct rei_server *s)
{
	struct wl_resource *r = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
						   wl_resource_get_version(mgr), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &device_impl, s, NULL);
}

static void manager_get_pointer(struct wl_client *client, struct wl_resource *mgr, uint32_t id,
				struct wl_resource *pointer)
{
	(void)pointer;
	make_device(client, mgr, id, wl_resource_get_user_data(mgr));
}

static void manager_get_tablet_tool(struct wl_client *client, struct wl_resource *mgr, uint32_t id,
				    struct wl_resource *tool)
{
	(void)tool;   /* No tablets are offered, so this cannot name a real tool; the device does nothing. */
	make_device(client, mgr, id, wl_resource_get_user_data(mgr));
}

static const struct wp_cursor_shape_manager_v1_interface manager_impl = {
	.destroy = destroy_request,
	.get_pointer = manager_get_pointer,
	.get_tablet_tool_v2 = manager_get_tablet_tool,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &wp_cursor_shape_manager_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

bool rei_cursor_shape_init(struct rei_server *s)
{
	return wl_global_create(s->display, &wp_cursor_shape_manager_v1_interface, 2, s, manager_bind) != NULL;
}
