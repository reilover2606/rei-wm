#include "proto/xdg_decoration.h"

#include <stdlib.h>
#include <wayland-server.h>

#include "config.h"
#include "proto/xdg_shell.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"

struct rei_xdg_decoration {
	struct rei_server *server;
	struct wl_resource *resource;
	struct rei_xdg_toplevel *toplevel;   /* NULL once the toplevel is gone */
	struct wl_listener toplevel_destroy;
	struct wl_list link;                 /* rei_server.xdg_decorations */
	int sent;                            /* the mode last sent, 0 = none */
};

static int wanted_mode(const struct rei_server *s)
{
	return s->config->decoration_client_titlebars ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
						      : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

/* Says the mode (if it is not what the client was last told) and, with `configure`, sends the xdg configure that
 * makes it take effect. */
static void send_mode(struct rei_xdg_decoration *d, bool configure)
{
	int mode = wanted_mode(d->server);
	if (d->sent == mode)
		return;
	d->sent = mode;
	zxdg_toplevel_decoration_v1_send_configure(d->resource, (uint32_t)mode);
	if (configure && d->toplevel)
		rei_xdg_toplevel_reconfigure(d->toplevel);
}

static void on_toplevel_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_xdg_decoration *d = wl_container_of(l, d, toplevel_destroy);
	wl_list_remove(&d->toplevel_destroy.link);
	wl_list_init(&d->toplevel_destroy.link);
	d->toplevel = NULL;
}

static void decoration_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

/* The client has a preference; the compositor's setting decides. (The answer is sent again even if the mode is
 * unchanged, as the protocol wants a configure in reply.) */
static void decoration_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode)
{
	(void)c;
	(void)mode;
	struct rei_xdg_decoration *d = wl_resource_get_user_data(r);
	d->sent = 0;
	send_mode(d, true);
}

static void decoration_unset_mode(struct wl_client *c, struct wl_resource *r)
{
	decoration_set_mode(c, r, 0);
}

static const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
	.destroy = decoration_destroy_request,
	.set_mode = decoration_set_mode,
	.unset_mode = decoration_unset_mode,
};

static void decoration_resource_destroyed(struct wl_resource *r)
{
	struct rei_xdg_decoration *d = wl_resource_get_user_data(r);
	wl_list_remove(&d->toplevel_destroy.link);
	wl_list_remove(&d->link);
	free(d);
}

static void manager_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void manager_get_toplevel_decoration(struct wl_client *client, struct wl_resource *manager, uint32_t id,
					    struct wl_resource *toplevel_res)
{
	struct rei_server *s = wl_resource_get_user_data(manager);
	struct rei_xdg_toplevel *tl = wl_resource_get_user_data(toplevel_res);
	struct rei_xdg_decoration *other;
	wl_list_for_each(other, &s->xdg_decorations, link) {
		if (other->toplevel == tl) {
			wl_resource_post_error(manager, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
					       "the toplevel already has a decoration object");
			return;
		}
	}
	struct rei_xdg_decoration *d = rei_xcalloc(1, sizeof *d);
	d->server = s;
	d->toplevel = tl;
	d->resource = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(manager), id);
	if (!d->resource) {
		free(d);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(d->resource, &decoration_impl, d, decoration_resource_destroyed);
	d->toplevel_destroy.notify = on_toplevel_destroyed;
	wl_resource_add_destroy_listener(toplevel_res, &d->toplevel_destroy);
	wl_list_insert(&s->xdg_decorations, &d->link);
	/* Sent now; it goes out with the first configure, or with the next one if the window is already mapped. */
	send_mode(d, true);
}

static const struct zxdg_decoration_manager_v1_interface manager_impl = {
	.destroy = manager_destroy_request,
	.get_toplevel_decoration = manager_get_toplevel_decoration,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

bool rei_xdg_decoration_init(struct rei_server *s)
{
	wl_list_init(&s->xdg_decorations);
	return wl_global_create(s->display, &zxdg_decoration_manager_v1_interface, 1, s, bind_manager) != NULL;
}

void rei_xdg_decoration_apply(struct rei_server *s)
{
	struct rei_xdg_decoration *d;
	wl_list_for_each(d, &s->xdg_decorations, link)
		send_mode(d, true);
}
