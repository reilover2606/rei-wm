#include <stdlib.h>
#include <wayland-server.h>

#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xwayland-shell-v1-server-protocol.h"
#include "xwayland.h"

/*
 * xwayland_shell_v1: how Xwayland tells us which wl_surface is which X11 window.
 * Xwayland gives each surface a serial number and sends the same number to us as
 * a client message on the X11 side; the two halves are matched in xwayland.c.
 * Only the Xwayland process we started may bind this global.
 */

static void surface_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void surface_set_serial(struct wl_client *c, struct wl_resource *r, uint32_t lo, uint32_t hi)
{
	struct rei_xsurface *xs = wl_resource_get_user_data(r);
	if (!xs) {
		return;
	}
	(void)c;
	rei_xsurface_set_serial(xs, ((uint64_t)hi << 32) | lo);
}

static const struct xwayland_surface_v1_interface xsurface_impl = {
	.destroy = surface_destroy_request,
	.set_serial = surface_set_serial,
};

static void xsurface_resource_destroy(struct wl_resource *r)
{
	struct rei_xsurface *xs = wl_resource_get_user_data(r);
	if (xs)
		rei_xsurface_destroy(xs);
}

static void shell_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void shell_get_xwayland_surface(struct wl_client *client, struct wl_resource *shell,
				       uint32_t id, struct wl_resource *surface_res)
{
	struct rei_server *server = wl_resource_get_user_data(shell);
	struct rei_surface *surface = rei_surface_from_resource(surface_res);

	if (surface->role != REI_ROLE_NONE) {
		wl_resource_post_error(shell, XWAYLAND_SHELL_V1_ERROR_ROLE,
				       "the surface already has a role");
		return;
	}
	struct wl_resource *r = wl_resource_create(client, &xwayland_surface_v1_interface,
						   wl_resource_get_version(shell), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_xsurface *xs = rei_xsurface_create(server, surface);
	wl_resource_set_implementation(r, &xsurface_impl, xs, xsurface_resource_destroy);
}

static const struct xwayland_shell_v1_interface shell_impl = {
	.destroy = shell_destroy_request,
	.get_xwayland_surface = shell_get_xwayland_surface,
};

static void shell_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct rei_server *server = data;
	struct wl_resource *r = wl_resource_create(client, &xwayland_shell_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &shell_impl, server, NULL);
}

static bool shell_filter(const struct wl_client *client, const struct wl_global *global, void *data)
{
	struct rei_server *server = data;
	if (wl_global_get_interface(global) != &xwayland_shell_v1_interface)
		return true;  /* Every other global is open to everyone. */
	return client == server->xwayland_client;
}

bool rei_xwayland_shell_init(struct rei_server *server)
{
	wl_display_set_global_filter(server->display, shell_filter, server);
	return wl_global_create(server->display, &xwayland_shell_v1_interface, 1, server, shell_bind) != NULL;
}
