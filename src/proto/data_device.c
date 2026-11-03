#include "proto/data_device.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#include "input/input.h"
#include "proto/seat.h"
#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "util/wlsignal.h"

struct rei_data_source {
	struct wl_resource *resource;
	struct wl_array mime_types;  /* NUL-terminated strings, concatenated */
	size_t n_mimes;
	struct wl_signal destroy_signal;
};

struct rei_data_offer {
	struct wl_resource *resource;
	struct rei_data_source *source;  /* NULL once the source is gone */
	struct wl_listener source_destroy;
};

/* ---- wl_data_offer ------------------------------------------------------- */

static void offer_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *mime)
{
	(void)c; (void)r; (void)serial; (void)mime;
}

static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
	(void)c;
	struct rei_data_offer *offer = wl_resource_get_user_data(r);
	if (offer->source)
		wl_data_source_send_send(offer->source->resource, mime, fd);
	close(fd);
}

static void offer_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void offer_finish(struct wl_client *c, struct wl_resource *r)
{
	(void)c; (void)r;
}

static void offer_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t actions, uint32_t preferred)
{
	(void)c; (void)r; (void)actions; (void)preferred;
}

static const struct wl_data_offer_interface offer_impl = {
	.accept = offer_accept,
	.receive = offer_receive,
	.destroy = offer_destroy_request,
	.finish = offer_finish,
	.set_actions = offer_set_actions,
};

static void offer_on_source_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_data_offer *offer = wl_container_of(l, offer, source_destroy);
	wl_list_remove(&offer->source_destroy.link);
	wl_list_init(&offer->source_destroy.link);
	offer->source = NULL;
}

static void offer_resource_destroy(struct wl_resource *r)
{
	struct rei_data_offer *offer = wl_resource_get_user_data(r);
	if (!wl_list_empty(&offer->source_destroy.link))
		wl_list_remove(&offer->source_destroy.link);
	free(offer);
}

/* ---- wl_data_source ------------------------------------------------------ */

static void source_offer(struct wl_client *c, struct wl_resource *r, const char *mime)
{
	(void)c;
	struct rei_data_source *src = wl_resource_get_user_data(r);
	size_t len = strlen(mime) + 1;
	char *dst = wl_array_add(&src->mime_types, len);
	memcpy(dst, mime, len);
	src->n_mimes++;
}

static void source_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void source_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t actions)
{
	(void)c; (void)r; (void)actions;
}

static const struct wl_data_source_interface source_impl = {
	.offer = source_offer,
	.destroy = source_destroy_request,
	.set_actions = source_set_actions,
};

static void source_resource_destroy(struct wl_resource *r)
{
	struct rei_data_source *src = wl_resource_get_user_data(r);
	rei_signal_emit(&src->destroy_signal, src);
	wl_array_release(&src->mime_types);
	free(src);
}

/* ---- wl_data_device ------------------------------------------------------ */

static void device_start_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *source_res,
			      struct wl_resource *origin, struct wl_resource *icon, uint32_t serial)
{
	(void)c; (void)r; (void)origin; (void)icon; (void)serial;
	/* No pointer yet, so a drag can never proceed. Cancelling is the defined
	 * way to refuse. */
	if (source_res)
		wl_data_source_send_cancelled(source_res);
}

static void selection_on_source_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_seat *seat = wl_container_of(l, seat, selection_destroy);
	wl_list_remove(&seat->selection_destroy.link);
	wl_list_init(&seat->selection_destroy.link);
	seat->selection = NULL;
}

static void device_set_selection(struct wl_client *c, struct wl_resource *r,
				 struct wl_resource *source_res, uint32_t serial)
{
	(void)serial;
	struct rei_seat *seat = wl_resource_get_user_data(r);
	struct rei_data_source *src = source_res ? wl_resource_get_user_data(source_res) : NULL;

	/* Only the client being typed into may take the clipboard; otherwise any
	 * background program could silently replace what the user copied. */
	struct rei_surface *focus = seat->server->input ? seat->server->input->keyboard_focus : NULL;
	if (!focus || wl_resource_get_client(focus->resource) != c) {
		if (src)
			wl_data_source_send_cancelled(source_res);
		return;
	}
	if (seat->selection == src)
		return;
	if (seat->selection) {
		wl_list_remove(&seat->selection_destroy.link);
		wl_list_init(&seat->selection_destroy.link);
		wl_data_source_send_cancelled(seat->selection->resource);
	}
	seat->selection = src;
	if (src) {
		seat->selection_destroy.notify = selection_on_source_destroyed;
		wl_signal_add(&src->destroy_signal, &seat->selection_destroy);
	}
	/* Everyone with a data device learns of the change only through focus, so
	 * tell the focused client (which is the one that set it, and any others it
	 * shares a connection with) right away. */
	rei_data_device_send_selection(seat->server, c);
}

static void device_resource_destroy(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

void rei_data_device_send_selection(struct rei_server *server, struct wl_client *client)
{
	struct rei_seat *seat = server->seat;
	struct wl_resource *dev;
	wl_resource_for_each(dev, &seat->data_devices) {
		if (wl_resource_get_client(dev) != client)
			continue;

		struct rei_data_source *src = seat->selection;
		if (!src) {
			wl_data_device_send_selection(dev, NULL);
			continue;
		}
		struct wl_resource *offer_res = wl_resource_create(client, &wl_data_offer_interface,
								   wl_resource_get_version(dev), 0);
		if (!offer_res) {
			wl_client_post_no_memory(client);
			return;
		}
		struct rei_data_offer *offer = rei_xcalloc(1, sizeof *offer);
		offer->resource = offer_res;
		offer->source = src;
		offer->source_destroy.notify = offer_on_source_destroyed;
		wl_signal_add(&src->destroy_signal, &offer->source_destroy);
		wl_resource_set_implementation(offer_res, &offer_impl, offer, offer_resource_destroy);

		wl_data_device_send_data_offer(dev, offer_res);
		const char *mime = src->mime_types.data;
		for (size_t i = 0; i < src->n_mimes; i++) {
			wl_data_offer_send_offer(offer_res, mime);
			mime += strlen(mime) + 1;
		}
		wl_data_device_send_selection(dev, offer_res);
	}
}

static void device_release(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wl_data_device_interface device_impl = {
	.start_drag = device_start_drag,
	.set_selection = device_set_selection,
	.release = device_release,
};

/* ---- wl_data_device_manager --------------------------------------------- */

static void manager_create_data_source(struct wl_client *client, struct wl_resource *mgr, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &wl_data_source_interface,
						   wl_resource_get_version(mgr), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_data_source *src = rei_xcalloc(1, sizeof *src);
	src->resource = r;
	wl_array_init(&src->mime_types);
	wl_signal_init(&src->destroy_signal);
	wl_resource_set_implementation(r, &source_impl, src, source_resource_destroy);
}

static void manager_get_data_device(struct wl_client *client, struct wl_resource *mgr, uint32_t id,
				    struct wl_resource *seat_res)
{
	(void)seat_res;
	struct wl_resource *r = wl_resource_create(client, &wl_data_device_interface,
						   wl_resource_get_version(mgr), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_server *server = wl_resource_get_user_data(mgr);
	wl_resource_set_implementation(r, &device_impl, server->seat, device_resource_destroy);
	wl_list_insert(&server->seat->data_devices, wl_resource_get_link(r));
}

static const struct wl_data_device_manager_interface manager_impl = {
	.create_data_source = manager_create_data_source,
	.get_data_device = manager_get_data_device,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &wl_data_device_manager_interface,
						   (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

bool rei_data_device_init(struct rei_server *server)
{
	return wl_global_create(server->display, &wl_data_device_manager_interface, 3, server,
				manager_bind) != NULL;
}
