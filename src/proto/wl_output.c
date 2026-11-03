#include "proto/wl_output.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>

#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define REMOVAL_GRACE_MS 5000

/*
 * Outlives the rei_output it describes: after an output is unplugged, clients
 * may still bind or hold the global for a few seconds, so everything they can
 * ask about is copied here.
 */
struct rei_wl_output {
	struct wl_list link;  /* rei_server.wl_outputs */
	struct rei_server *server;
	struct wl_global *global;
	struct wl_list resources;
	struct wl_event_source *grace_timer;

	char *name;
	int32_t x, y, width, height, refresh_mhz;
	double scale;
};

static int32_t int_scale(double scale)
{
	int32_t s = (int32_t)ceil(scale);
	return s < 1 ? 1 : s;
}

static void send_state(struct wl_resource *r, const struct rei_wl_output *o)
{
	int version = wl_resource_get_version(r);

	wl_output_send_geometry(r, o->x, o->y, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
				"rei-wm", o->name, WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
			    o->width, o->height, o->refresh_mhz);
	if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
		wl_output_send_scale(r, int_scale(o->scale));
	if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
		wl_output_send_name(r, o->name);
		wl_output_send_description(r, o->name);
	}
	if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(r);
}

static void output_release(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wl_output_interface output_impl = {
	.release = output_release,
};

static void output_resource_destroy(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct rei_wl_output *o = data;
	struct wl_resource *r = wl_resource_create(client, &wl_output_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &output_impl, o, output_resource_destroy);
	wl_list_insert(&o->resources, wl_resource_get_link(r));
	send_state(r, o);
}

static void destroy_global(struct rei_wl_output *o)
{
	if (o->grace_timer)
		wl_event_source_remove(o->grace_timer);
	wl_global_destroy(o->global);
	/* Resources still held by clients are inert; make them forget us. */
	struct wl_resource *r, *tmp;
	wl_resource_for_each_safe(r, tmp, &o->resources) {
		wl_list_remove(wl_resource_get_link(r));
		wl_list_init(wl_resource_get_link(r));
	}
	wl_list_remove(&o->link);
	free(o->name);
	free(o);
}

static int on_grace_expired(void *data)
{
	destroy_global(data);
	return 0;
}

void rei_wl_output_add(struct rei_server *server, struct rei_output *output)
{
	struct rei_wl_output *o = rei_xcalloc(1, sizeof *o);
	o->server = server;
	wl_list_init(&o->resources);
	o->name = rei_xstrdup(output->name);
	o->x = output->x;
	o->y = output->y;
	o->width = output->width;
	o->height = output->height;
	o->refresh_mhz = output->refresh_mhz;
	o->scale = output->scale;
	o->global = wl_global_create(server->display, &wl_output_interface, 4, o, output_bind);
	wl_list_insert(&server->wl_outputs, &o->link);
	output->wl_output = o;
}

void rei_wl_output_update(struct rei_output *output)
{
	struct rei_wl_output *o = output->wl_output;
	if (!o)
		return;
	if (o->x == output->x && o->y == output->y && o->width == output->width &&
	    o->height == output->height && o->refresh_mhz == output->refresh_mhz &&
	    o->scale == output->scale)
		return;

	o->x = output->x;
	o->y = output->y;
	o->width = output->width;
	o->height = output->height;
	o->refresh_mhz = output->refresh_mhz;
	o->scale = output->scale;

	struct wl_resource *r;
	wl_resource_for_each(r, &o->resources)
		send_state(r, o);
}

void rei_wl_output_remove(struct rei_output *output)
{
	struct rei_wl_output *o = output->wl_output;
	if (!o)
		return;
	output->wl_output = NULL;
	wl_global_remove(o->global);
	o->grace_timer = wl_event_loop_add_timer(o->server->wl_loop, on_grace_expired, o);
	if (o->grace_timer)
		wl_event_source_timer_update(o->grace_timer, REMOVAL_GRACE_MS);
	else
		destroy_global(o);
}

void rei_wl_output_finish(struct rei_server *server)
{
	struct rei_wl_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &server->wl_outputs, link)
		destroy_global(o);
}
