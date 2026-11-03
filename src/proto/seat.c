#include "proto/seat.h"

#include <stdlib.h>
#include <wayland-server-protocol.h>

#include "desktop.h"
#include "input/input.h"
#include "input/xkb.h"
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

static void device_resource_destroy(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

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
	struct rei_seat *seat = wl_resource_get_user_data(seat_res);
	wl_resource_set_implementation(r, &pointer_impl, seat, device_resource_destroy);
	wl_list_insert(&seat->pointers, wl_resource_get_link(r));

	/* A client that binds its pointer while the pointer is already over its
	 * window still needs the enter event, or it would never see the pointer
	 * until it crossed a boundary. */
	struct rei_server *server = seat->server;
	struct rei_surface *focus = server->input ? server->input->pointer_focus : NULL;
	if (focus && wl_resource_get_client(focus->resource) == client) {
		struct rei_surface *hit = NULL;
		double sx = 0, sy = 0;
		if (rei_wm_pick(server, server->cursor_x, server->cursor_y, &hit, &sx, &sy) && hit == focus) {
			uint32_t serial = wl_display_next_serial(server->display);
			wl_pointer_send_enter(r, serial, focus->resource,
					      wl_fixed_from_double(sx), wl_fixed_from_double(sy));
			if (wl_resource_get_version(r) >= WL_POINTER_FRAME_SINCE_VERSION)
				wl_pointer_send_frame(r);
		}
	}
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
	struct rei_seat *seat = wl_resource_get_user_data(seat_res);
	wl_resource_set_implementation(r, &keyboard_impl, seat, device_resource_destroy);
	wl_list_insert(&seat->keyboards, wl_resource_get_link(r));

	struct rei_input *in = seat->server->input;
	wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, in->xkb->keymap_fd,
				in->xkb->keymap_size);
	if (wl_resource_get_version(r) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
		wl_keyboard_send_repeat_info(r, seat->server->config->repeat_rate,
					     seat->server->config->repeat_delay);

	/* A client that binds its keyboard while it already has focus still needs
	 * the enter event, or it would never see keys. */
	if (in->keyboard_focus && wl_resource_get_client(in->keyboard_focus->resource) == client) {
		struct wl_array keys;
		wl_array_init(&keys);
		for (size_t i = 0; i < in->n_pressed; i++)
			*(uint32_t *)wl_array_add(&keys, sizeof(uint32_t)) = in->pressed[i];
		uint32_t serial = wl_display_next_serial(seat->server->display);
		wl_keyboard_send_enter(r, serial, in->keyboard_focus->resource, &keys);
		wl_array_release(&keys);
		struct rei_xkb_mods m = rei_xkb_serialize(in->xkb);
		wl_keyboard_send_modifiers(r, serial, m.depressed, m.latched, m.locked, m.group);
	}
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

void rei_seat_keyboards_reconfigured(struct rei_seat *seat, bool keymap_changed)
{
	struct rei_server *server = seat->server;
	struct rei_input *in = server->input;
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->keyboards) {
		if (keymap_changed)
			wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, in->xkb->keymap_fd,
						in->xkb->keymap_size);
		if (wl_resource_get_version(r) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
			wl_keyboard_send_repeat_info(r, server->config->repeat_rate,
						     server->config->repeat_delay);
	}
	if (keymap_changed && in->keyboard_focus)
		rei_seat_keyboard_modifiers(seat, in->keyboard_focus);
}

bool rei_seat_init(struct rei_server *server)
{
	struct rei_seat *seat = rei_xcalloc(1, sizeof *seat);
	seat->server = server;
	wl_list_init(&seat->resources);
	wl_list_init(&seat->data_devices);
	wl_list_init(&seat->pointers);
	wl_list_init(&seat->keyboards);
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

/* ------------------------------------------------------------------------ */
/* Event delivery                                                           */
/* ------------------------------------------------------------------------ */

static struct wl_client *client_of(const struct rei_surface *surface)
{
	return wl_resource_get_client(surface->resource);
}

/* Runs `fn` for every pointer object belonging to the surface's client. */
#define FOR_CLIENT_POINTERS(seat, surface, res)                                \
	struct wl_resource *res;                                               \
	wl_resource_for_each(res, &(seat)->pointers)                           \
		if (wl_resource_get_client(res) == client_of(surface))

#define FOR_CLIENT_KEYBOARDS(seat, surface, res)                               \
	struct wl_resource *res;                                               \
	wl_resource_for_each(res, &(seat)->keyboards)                          \
		if (wl_resource_get_client(res) == client_of(surface))

static void pointer_frame(struct wl_resource *r)
{
	if (wl_resource_get_version(r) >= WL_POINTER_FRAME_SINCE_VERSION)
		wl_pointer_send_frame(r);
}

uint32_t rei_seat_pointer_enter(struct rei_seat *seat, struct rei_surface *surface, double sx, double sy)
{
	uint32_t serial = wl_display_next_serial(seat->server->display);
	FOR_CLIENT_POINTERS(seat, surface, r) {
		wl_pointer_send_enter(r, serial, surface->resource,
				      wl_fixed_from_double(sx), wl_fixed_from_double(sy));
		pointer_frame(r);
	}
	return serial;
}

void rei_seat_pointer_leave(struct rei_seat *seat, struct rei_surface *surface)
{
	uint32_t serial = wl_display_next_serial(seat->server->display);
	FOR_CLIENT_POINTERS(seat, surface, r) {
		wl_pointer_send_leave(r, serial, surface->resource);
		pointer_frame(r);
	}
}

void rei_seat_pointer_motion(struct rei_seat *seat, struct rei_surface *surface,
			     uint32_t time_ms, double sx, double sy)
{
	FOR_CLIENT_POINTERS(seat, surface, r) {
		wl_pointer_send_motion(r, time_ms, wl_fixed_from_double(sx), wl_fixed_from_double(sy));
		pointer_frame(r);
	}
}

uint32_t rei_seat_pointer_button(struct rei_seat *seat, struct rei_surface *surface,
				 uint32_t time_ms, uint32_t button, bool pressed)
{
	uint32_t serial = wl_display_next_serial(seat->server->display);
	FOR_CLIENT_POINTERS(seat, surface, r) {
		wl_pointer_send_button(r, serial, time_ms, button,
				       pressed ? WL_POINTER_BUTTON_STATE_PRESSED
					       : WL_POINTER_BUTTON_STATE_RELEASED);
		pointer_frame(r);
	}
	return serial;
}

void rei_seat_pointer_axis(struct rei_seat *seat, struct rei_surface *surface,
			   uint32_t time_ms, bool horizontal, double value)
{
	FOR_CLIENT_POINTERS(seat, surface, r) {
		if (wl_resource_get_version(r) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
			wl_pointer_send_axis_source(r, WL_POINTER_AXIS_SOURCE_WHEEL);
		uint32_t axis = horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
					   : WL_POINTER_AXIS_VERTICAL_SCROLL;
		if (value == 0.0) {
			if (wl_resource_get_version(r) >= WL_POINTER_AXIS_STOP_SINCE_VERSION)
				wl_pointer_send_axis_stop(r, time_ms, axis);
		} else {
			wl_pointer_send_axis(r, time_ms, axis, wl_fixed_from_double(value));
		}
		pointer_frame(r);
	}
}

void rei_seat_keyboard_enter(struct rei_seat *seat, struct rei_surface *surface,
			     const uint32_t *keys, size_t n_keys)
{
	struct wl_array arr;
	wl_array_init(&arr);
	for (size_t i = 0; i < n_keys; i++)
		*(uint32_t *)wl_array_add(&arr, sizeof(uint32_t)) = keys[i];

	uint32_t serial = wl_display_next_serial(seat->server->display);
	struct rei_xkb_mods m = rei_xkb_serialize(seat->server->input->xkb);
	FOR_CLIENT_KEYBOARDS(seat, surface, r) {
		wl_keyboard_send_enter(r, serial, surface->resource, &arr);
		wl_keyboard_send_modifiers(r, serial, m.depressed, m.latched, m.locked, m.group);
	}
	wl_array_release(&arr);
}

void rei_seat_keyboard_leave(struct rei_seat *seat, struct rei_surface *surface)
{
	uint32_t serial = wl_display_next_serial(seat->server->display);
	FOR_CLIENT_KEYBOARDS(seat, surface, r)
		wl_keyboard_send_leave(r, serial, surface->resource);
}

void rei_seat_keyboard_key(struct rei_seat *seat, struct rei_surface *surface,
			   uint32_t time_ms, uint32_t key, bool pressed)
{
	uint32_t serial = wl_display_next_serial(seat->server->display);
	FOR_CLIENT_KEYBOARDS(seat, surface, r)
		wl_keyboard_send_key(r, serial, time_ms, key,
				     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
					     : WL_KEYBOARD_KEY_STATE_RELEASED);
}

void rei_seat_keyboard_modifiers(struct rei_seat *seat, struct rei_surface *surface)
{
	uint32_t serial = wl_display_next_serial(seat->server->display);
	struct rei_xkb_mods m = rei_xkb_serialize(seat->server->input->xkb);
	FOR_CLIENT_KEYBOARDS(seat, surface, r)
		wl_keyboard_send_modifiers(r, serial, m.depressed, m.latched, m.locked, m.group);
}
