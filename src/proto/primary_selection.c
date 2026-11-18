#include "proto/primary_selection.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>

#include "clipboard.h"
#include "input/input.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "proto/seat.h"
#include "proto/selection.h"
#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "util/wlsignal.h"
#include "xwayland.h"

struct rei_primary_offer {
	struct wl_resource *resource;
	struct rei_selection_source *source;   /* NULL once the source is gone */
	struct wl_listener source_destroy;
};

/* ---- the offer ---- */

static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
	(void)c;
	struct rei_primary_offer *offer = wl_resource_get_user_data(r);
	if (offer->source)
		rei_selection_source_send(offer->source, mime, fd);
	close(fd);
}

static void offer_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct zwp_primary_selection_offer_v1_interface offer_impl = {
	.receive = offer_receive,
	.destroy = offer_destroy_request,
};

static void offer_on_source_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_primary_offer *offer = wl_container_of(l, offer, source_destroy);
	wl_list_remove(&offer->source_destroy.link);
	wl_list_init(&offer->source_destroy.link);
	offer->source = NULL;
}

static void offer_resource_destroy(struct wl_resource *r)
{
	struct rei_primary_offer *offer = wl_resource_get_user_data(r);
	if (!wl_list_empty(&offer->source_destroy.link))
		wl_list_remove(&offer->source_destroy.link);
	free(offer);
}

/* ---- the source ---- */

static void source_offer(struct wl_client *c, struct wl_resource *r, const char *mime)
{
	(void)c;
	rei_selection_source_add_mime(wl_resource_get_user_data(r), mime);
}

static void source_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct zwp_primary_selection_source_v1_interface source_impl = {
	.offer = source_offer,
	.destroy = source_destroy_request,
};

static void source_resource_destroy(struct wl_resource *r)
{
	struct rei_selection_source *src = wl_resource_get_user_data(r);
	rei_signal_emit(&src->destroy_signal, src);
	wl_array_release(&src->mime_types);
	free(src);
}

/* ---- the device ---- */

static void primary_on_source_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_seat *seat = wl_container_of(l, seat, primary_destroy);
	wl_list_remove(&seat->primary_destroy.link);
	wl_list_init(&seat->primary_destroy.link);
	seat->primary = NULL;
	rei_xsel_wayland_changed(seat->server, REI_SEL_PRIMARY);
}

static void replace_primary(struct rei_server *server, struct rei_selection_source *src)
{
	struct rei_seat *seat = server->seat;
	if (seat->primary == src)
		return;
	if (seat->primary) {
		struct rei_selection_source *old = seat->primary;
		wl_list_remove(&seat->primary_destroy.link);
		wl_list_init(&seat->primary_destroy.link);
		seat->primary = NULL;
		rei_selection_source_cancel(old);
	}
	seat->primary = src;
	if (src) {
		seat->primary_destroy.notify = primary_on_source_destroyed;
		wl_signal_add(&src->destroy_signal, &seat->primary_destroy);
	}
	rei_xsel_wayland_changed(server, REI_SEL_PRIMARY);   /* X11 programs follow */
}

static void device_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *source_res,
				 uint32_t serial)
{
	(void)serial;
	struct rei_seat *seat = wl_resource_get_user_data(r);
	struct rei_selection_source *src = source_res ? wl_resource_get_user_data(source_res) : NULL;

	/* As for the clipboard: only the client being typed into may take it. */
	struct rei_surface *focus = seat->server->input ? seat->server->input->keyboard_focus : NULL;
	if (!focus || wl_resource_get_client(focus->resource) != c) {
		rei_debug("primary selection refused: not from the client being typed into");
		if (src)
			zwp_primary_selection_source_v1_send_cancelled(source_res);
		return;
	}
	rei_debug("primary selection %s", src ? "set" : "cleared");
	replace_primary(seat->server, src);
	rei_primary_selection_send(seat->server, c);
	if (src)
		rei_clipboard_primary_set(seat->server, src);
}

void rei_primary_selection_set(struct rei_server *server, struct rei_selection_source *src)
{
	replace_primary(server, src);
	struct rei_surface *focus = server->input ? server->input->keyboard_focus : NULL;
	if (focus)
		rei_primary_selection_send(server, wl_resource_get_client(focus->resource));
}

static void device_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct zwp_primary_selection_device_v1_interface device_impl = {
	.set_selection = device_set_selection,
	.destroy = device_destroy_request,
};

static void device_resource_destroy(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

void rei_primary_selection_send(struct rei_server *server, struct wl_client *client)
{
	struct rei_seat *seat = server->seat;
	struct wl_resource *dev;
	wl_resource_for_each(dev, &seat->primary_devices) {
		if (wl_resource_get_client(dev) != client)
			continue;
		struct rei_selection_source *src = seat->primary;
		if (!src) {
			zwp_primary_selection_device_v1_send_selection(dev, NULL);
			continue;
		}
		struct wl_resource *offer_res = wl_resource_create(client, &zwp_primary_selection_offer_v1_interface,
								   wl_resource_get_version(dev), 0);
		if (!offer_res) {
			wl_client_post_no_memory(client);
			return;
		}
		struct rei_primary_offer *offer = rei_xcalloc(1, sizeof *offer);
		offer->resource = offer_res;
		offer->source = src;
		offer->source_destroy.notify = offer_on_source_destroyed;
		wl_signal_add(&src->destroy_signal, &offer->source_destroy);
		wl_resource_set_implementation(offer_res, &offer_impl, offer, offer_resource_destroy);

		zwp_primary_selection_device_v1_send_data_offer(dev, offer_res);
		const char *mime = src->mime_types.data;
		for (size_t i = 0; i < src->n_mimes; i++) {
			zwp_primary_selection_offer_v1_send_offer(offer_res, mime);
			mime += strlen(mime) + 1;
		}
		zwp_primary_selection_device_v1_send_selection(dev, offer_res);
	}
}

/* ---- the manager ---- */

static void manager_create_source(struct wl_client *client, struct wl_resource *mgr, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &zwp_primary_selection_source_v1_interface,
						   wl_resource_get_version(mgr), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_selection_source *src = rei_xcalloc(1, sizeof *src);
	src->server = wl_resource_get_user_data(mgr);
	src->kind = REI_SEL_PRIMARY;
	src->resource = r;
	wl_array_init(&src->mime_types);
	wl_signal_init(&src->destroy_signal);
	wl_resource_set_implementation(r, &source_impl, src, source_resource_destroy);
}

static void manager_get_device(struct wl_client *client, struct wl_resource *mgr, uint32_t id,
			       struct wl_resource *seat_res)
{
	(void)seat_res;
	struct wl_resource *r = wl_resource_create(client, &zwp_primary_selection_device_v1_interface,
						   wl_resource_get_version(mgr), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_server *server = wl_resource_get_user_data(mgr);
	wl_resource_set_implementation(r, &device_impl, server->seat, device_resource_destroy);
	wl_list_insert(&server->seat->primary_devices, wl_resource_get_link(r));
}

static void manager_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct zwp_primary_selection_device_manager_v1_interface manager_impl = {
	.create_source = manager_create_source,
	.get_device = manager_get_device,
	.destroy = manager_destroy_request,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &zwp_primary_selection_device_manager_v1_interface,
						   (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

bool rei_primary_selection_init(struct rei_server *server)
{
	return wl_global_create(server->display, &zwp_primary_selection_device_manager_v1_interface, 1, server,
				manager_bind) != NULL;
}
