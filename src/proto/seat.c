#include "proto/seat.h"

#include <stdlib.h>
#include <wayland-server-protocol.h>

#include "proto/data_device.h"
#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

/* ---- wl_pointer / wl_keyboard / wl_touch --------------------------------- */

static void release_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial,
			       struct wl_resource *surface_res, int32_t hx, int32_t hy)
{
	(void)c; (void)serial; (void)hx; (void)hy;
	/* rei-wm draws the cursor itself, for every client, so the image a client
	 * asks for is deliberately ignored. The surface still takes the cursor
	 * role so the client's state stays consistent. */
	if (!surface_res)
		return;
	struct rei_surface *s = rei_surface_from_resource(surface_res);
	if (!rei_surface_set_role(s, REI_ROLE_CURSOR))
		wl_resource_post_error(r, WL_POINTER_ERROR_ROLE, "surface already has a role");
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = pointer_set_cursor,
	.release = release_request,
};

static const struct wl_keyboard_interface keyboard_impl = {
	.release = release_request,
};

static const struct wl_touch_interface touch_impl = {
	.release = release_request,
};

/* ---- wl_seat ------------------------------------------------------------- */

static bool check_capability(struct wl_resource *seat_res, uint32_t cap, const char *what)
{
	struct rei_seat *seat = wl_resource_get_user_data(seat_res);
	if (seat->ever_capabilities & cap)
		return true;
	wl_resource_post_error(seat_res, WL_SEAT_ERROR_MISSING_CAPABILITY,
			       "the seat has never had the %s capability", what);
	return false;
}

static void seat_get_pointer(struct wl_client *client, struct wl_resource *seat_res, uint32_t id)
{
	if (!check_capability(seat_res, WL_SEAT_CAPABILITY_POINTER, "pointer"))
		return;
	struct wl_resource *r = wl_resource_create(client, &wl_pointer_interface,
						   wl_resource_get_version(seat_res), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &pointer_impl, wl_resource_get_user_data(seat_res), NULL);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *seat_res, uint32_t id)
{
	if (!check_capability(seat_res, WL_SEAT_CAPABILITY_KEYBOARD, "keyboard"))
		return;
	struct wl_resource *r = wl_resource_create(client, &wl_keyboard_interface,
						   wl_resource_get_version(seat_res), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &keyboard_impl, wl_resource_get_user_data(seat_res), NULL);
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *seat_res, uint32_t id)
{
	if (!check_capability(seat_res, WL_SEAT_CAPABILITY_TOUCH, "touch"))
		return;
	struct wl_resource *r = wl_resource_create(client, &wl_touch_interface,
						   wl_resource_get_version(seat_res), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &touch_impl, wl_resource_get_user_data(seat_res), NULL);
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = release_request,
};

static void seat_resource_destroy(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct rei_seat *seat = data;
	struct wl_resource *r = wl_resource_create(client, &wl_seat_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &seat_impl, seat, seat_resource_destroy);
	wl_list_insert(&seat->resources, wl_resource_get_link(r));

	wl_seat_send_capabilities(r, seat->capabilities);
	if (version >= WL_SEAT_NAME_SINCE_VERSION)
		wl_seat_send_name(r, "seat0");
}

bool rei_seat_init(struct rei_server *server)
{
	struct rei_seat *seat = rei_xcalloc(1, sizeof *seat);
	seat->server = server;
	wl_list_init(&seat->resources);
	wl_list_init(&seat->data_devices);
	seat->global = wl_global_create(server->display, &wl_seat_interface, 7, seat, seat_bind);
	if (!seat->global) {
		free(seat);
		return false;
	}
	server->seat = seat;
	return rei_data_device_init(server);
}

void rei_seat_finish(struct rei_server *server)
{
	if (!server->seat)
		return;
	free(server->seat);
	server->seat = NULL;
}

void rei_seat_set_capabilities(struct rei_seat *seat, uint32_t capabilities)
{
	seat->capabilities = capabilities;
	seat->ever_capabilities |= capabilities;
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->resources)
		wl_seat_send_capabilities(r, capabilities);
}
