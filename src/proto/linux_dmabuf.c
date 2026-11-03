#include "proto/linux_dmabuf.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>

#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define DMABUF_VERSION 3

/* ------------------------------------------------------------------------ */
/* wl_buffer                                                                */
/* ------------------------------------------------------------------------ */

struct dmabuf_buffer {
	struct rei_dmabuf *dmabuf;
};

static void buffer_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wl_buffer_interface buffer_impl = {
	.destroy = buffer_destroy_request,
};

static void buffer_resource_destroy(struct wl_resource *r)
{
	struct dmabuf_buffer *b = wl_resource_get_user_data(r);
	rei_dmabuf_unref(b->dmabuf);
	free(b);
}

struct rei_dmabuf *rei_dmabuf_from_buffer(struct wl_resource *buffer)
{
	if (!buffer || !wl_resource_instance_of(buffer, &wl_buffer_interface, &buffer_impl))
		return NULL;
	struct dmabuf_buffer *b = wl_resource_get_user_data(buffer);
	return b->dmabuf;
}

/* ------------------------------------------------------------------------ */
/* Leases                                                                   */
/* ------------------------------------------------------------------------ */

struct rei_buffer_lease {
	struct wl_list link;  /* rei_server.buffer_leases, once retired */
	struct wl_resource *buffer;  /* NULL once the client destroyed it */
	struct wl_listener destroy;
	uint64_t release_after;  /* Serial that must have been presented everywhere. */
};

static void lease_buffer_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_buffer_lease *lease = wl_container_of(l, lease, destroy);
	lease->buffer = NULL;
	wl_list_remove(&lease->destroy.link);
	wl_list_init(&lease->destroy.link);
}

struct rei_buffer_lease *rei_buffer_lease_new(struct wl_resource *buffer)
{
	struct rei_buffer_lease *lease = rei_xcalloc(1, sizeof *lease);
	wl_list_init(&lease->link);
	lease->buffer = buffer;
	lease->destroy.notify = lease_buffer_destroyed;
	wl_resource_add_destroy_listener(buffer, &lease->destroy);
	return lease;
}

static void lease_free(struct rei_buffer_lease *lease)
{
	if (lease->buffer)
		wl_list_remove(&lease->destroy.link);
	free(lease);
}

void rei_buffer_lease_retire(struct rei_server *server, struct rei_buffer_lease *lease)
{
	if (!lease)
		return;
	if (!lease->buffer) {
		lease_free(lease);
		return;
	}
	/* The next snapshot built is the first that cannot contain this content. */
	lease->release_after = server->scene_serial + 1;
	wl_list_insert(server->buffer_leases.prev, &lease->link);
}

void rei_buffer_leases_check(struct rei_server *server)
{
	uint64_t done = UINT64_MAX;
	struct rei_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->last_presented_serial < done)
			done = o->last_presented_serial;
	}

	struct rei_buffer_lease *lease, *tmp;
	wl_list_for_each_safe(lease, tmp, &server->buffer_leases, link) {
		if (lease->release_after > done)
			continue;
		wl_list_remove(&lease->link);
		if (lease->buffer)
			wl_buffer_send_release(lease->buffer);
		lease_free(lease);
	}
}

/* ------------------------------------------------------------------------ */
/* zwp_linux_buffer_params_v1                                               */
/* ------------------------------------------------------------------------ */

struct params {
	struct rei_server *server;
	struct wl_resource *resource;
	bool used;
	int fd[REI_DMABUF_MAX_PLANES];
	uint32_t offset[REI_DMABUF_MAX_PLANES];
	uint32_t stride[REI_DMABUF_MAX_PLANES];
	uint64_t modifier[REI_DMABUF_MAX_PLANES];
	bool has_modifier;
};

static void params_close_fds(struct params *p)
{
	for (int i = 0; i < REI_DMABUF_MAX_PLANES; i++) {
		if (p->fd[i] >= 0)
			close(p->fd[i]);
		p->fd[i] = -1;
	}
}

static void params_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void params_add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
		       uint32_t plane_idx, uint32_t offset, uint32_t stride,
		       uint32_t modifier_hi, uint32_t modifier_lo)
{
	(void)client;
	struct params *p = wl_resource_get_user_data(resource);
	if (p->used) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
				       "the params object was already used to create a buffer");
		return;
	}
	if (plane_idx >= REI_DMABUF_MAX_PLANES) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
				       "plane index %u is out of range", plane_idx);
		return;
	}
	if (p->fd[plane_idx] >= 0) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
				       "plane %u was already set", plane_idx);
		return;
	}
	uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
	if (p->has_modifier) {
		for (int i = 0; i < REI_DMABUF_MAX_PLANES; i++) {
			if (p->fd[i] >= 0 && p->modifier[i] != modifier) {
				close(fd);
				wl_resource_post_error(resource,
					ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
					"all planes must use the same modifier");
				return;
			}
		}
	}
	p->fd[plane_idx] = fd;
	p->offset[plane_idx] = offset;
	p->stride[plane_idx] = stride;
	p->modifier[plane_idx] = modifier;
	p->has_modifier = true;
}

/* Checks the buffer and builds it. Returns NULL after posting a protocol error,
 * or with *failed set when the buffer is merely unusable (the client is told
 * through 'failed' rather than disconnected). */
static struct rei_dmabuf *params_build(struct params *p, int32_t width, int32_t height,
				       uint32_t format, uint32_t flags, bool *failed)
{
	struct wl_resource *r = p->resource;
	*failed = false;

	if (p->used) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
				       "the params object was already used to create a buffer");
		return NULL;
	}
	p->used = true;

	int n_planes = 0;
	while (n_planes < REI_DMABUF_MAX_PLANES && p->fd[n_planes] >= 0)
		n_planes++;
	if (n_planes == 0) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
				       "no planes were added");
		return NULL;
	}
	for (int i = n_planes; i < REI_DMABUF_MAX_PLANES; i++) {
		if (p->fd[i] >= 0) {
			wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
					       "plane %d was added but plane %d was not", i, n_planes);
			return NULL;
		}
	}
	if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
				       "invalid buffer size %dx%d", width, height);
		return NULL;
	}
	if (flags & ~(unsigned)(ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT |
				ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED |
				ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST)) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
				       "unknown flags 0x%x", flags);
		return NULL;
	}
	if (flags & (ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED |
		     ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST)) {
		*failed = true;  /* Valid, but not something we can display. */
		return NULL;
	}

	uint64_t modifier = p->modifier[0];
	if (!rei_format_mod_supported(p->server->dmabuf_formats, p->server->n_dmabuf_formats,
					   format, modifier)) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
				       "format %.4s with modifier 0x%llx is not supported",
				       (const char *)&format, (unsigned long long)modifier);
		return NULL;
	}

	/* Each plane must start inside its buffer. (Tiled layouts do not follow
	 * stride * height, so this is the strongest check that is always right.) */
	for (int i = 0; i < n_planes; i++) {
		off_t size = lseek(p->fd[i], 0, SEEK_END);
		if (size >= 0 && (uint64_t)p->offset[i] >= (uint64_t)size) {
			wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
					       "plane %d offset %u is past the end of its buffer",
					       i, p->offset[i]);
			return NULL;
		}
	}

	struct rei_dmabuf proto = {
		.width = width, .height = height, .format = format, .modifier = modifier,
		.n_planes = n_planes,
		.y_invert = (flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT) != 0,
	};
	for (int i = 0; i < REI_DMABUF_MAX_PLANES; i++) {
		proto.fd[i] = p->fd[i];
		proto.offset[i] = p->offset[i];
		proto.stride[i] = p->stride[i];
		p->fd[i] = -1;  /* Ownership moves to the dmabuf, even on failure. */
	}
	struct rei_dmabuf *d = rei_dmabuf_create(&proto);
	if (!d)
		*failed = true;
	return d;
}

static struct wl_resource *make_buffer(struct wl_client *client, struct wl_resource *params_res,
				       uint32_t id, struct rei_dmabuf *d)
{
	struct wl_resource *buf = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!buf) {
		rei_dmabuf_unref(d);
		wl_client_post_no_memory(client);
		return NULL;
	}
	(void)params_res;
	struct dmabuf_buffer *b = rei_xcalloc(1, sizeof *b);
	b->dmabuf = d;
	wl_resource_set_implementation(buf, &buffer_impl, b, buffer_resource_destroy);
	return buf;
}

static void params_create(struct wl_client *client, struct wl_resource *resource, int32_t width,
			  int32_t height, uint32_t format, uint32_t flags)
{
	struct params *p = wl_resource_get_user_data(resource);
	bool failed;
	struct rei_dmabuf *d = params_build(p, width, height, format, flags, &failed);
	if (!d) {
		if (failed)
			zwp_linux_buffer_params_v1_send_failed(resource);
		return;
	}
	struct wl_resource *buf = make_buffer(client, resource, 0, d);
	if (buf)
		zwp_linux_buffer_params_v1_send_created(resource, buf);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
				uint32_t buffer_id, int32_t width, int32_t height,
				uint32_t format, uint32_t flags)
{
	struct params *p = wl_resource_get_user_data(resource);
	bool failed;
	struct rei_dmabuf *d = params_build(p, width, height, format, flags, &failed);
	if (!d) {
		/* There is no 'failed' reply to a create_immed: it is a protocol error. */
		if (failed)
			wl_resource_post_error(resource,
					       ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
					       "the buffer cannot be imported");
		return;
	}
	make_buffer(client, resource, buffer_id, d);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
	.destroy = params_destroy_request,
	.add = params_add,
	.create = params_create,
	.create_immed = params_create_immed,
};

static void params_resource_destroy(struct wl_resource *r)
{
	struct params *p = wl_resource_get_user_data(r);
	params_close_fds(p);
	free(p);
}

/* ------------------------------------------------------------------------ */
/* zwp_linux_dmabuf_v1                                                      */
/* ------------------------------------------------------------------------ */

static void dmabuf_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct params *p = rei_xcalloc(1, sizeof *p);
	p->server = wl_resource_get_user_data(resource);
	for (int i = 0; i < REI_DMABUF_MAX_PLANES; i++)
		p->fd[i] = -1;
	p->resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
					 wl_resource_get_version(resource), id);
	if (!p->resource) {
		free(p);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(p->resource, &params_impl, p, params_resource_destroy);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = dmabuf_destroy_request,
	.create_params = dmabuf_create_params,
};

static void dmabuf_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct rei_server *server = data;
	struct wl_resource *r = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
						   (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &dmabuf_impl, server, NULL);

	/* v3 sends one 'modifier' event per pair; older clients get bare formats. */
	uint32_t last = 0;
	bool have_last = false;
	for (size_t i = 0; i < server->n_dmabuf_formats; i++) {
		const struct rei_format_mod *fm = &server->dmabuf_formats[i];
		if (version >= 3) {
			zwp_linux_dmabuf_v1_send_modifier(r, fm->format,
							  (uint32_t)(fm->modifier >> 32),
							  (uint32_t)(fm->modifier & 0xffffffffu));
		} else if (!have_last || fm->format != last) {
			bool seen = false;
			for (size_t j = 0; j < i; j++)
				seen = seen || server->dmabuf_formats[j].format == fm->format;
			if (!seen)
				zwp_linux_dmabuf_v1_send_format(r, fm->format);
			last = fm->format;
			have_last = true;
		}
	}
}

bool rei_linux_dmabuf_init(struct rei_server *server, const struct rei_format_mod *formats,
			   size_t n_formats)
{
	if (server->dmabuf_global || n_formats == 0)
		return n_formats != 0 || server->dmabuf_global;
	server->dmabuf_formats = rei_xcalloc(n_formats, sizeof *formats);
	memcpy(server->dmabuf_formats, formats, n_formats * sizeof *formats);
	server->n_dmabuf_formats = n_formats;
	server->dmabuf_global = wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface,
						 DMABUF_VERSION, server, dmabuf_bind);
	if (!server->dmabuf_global) {
		free(server->dmabuf_formats);
		server->dmabuf_formats = NULL;
		server->n_dmabuf_formats = 0;
		return false;
	}
	rei_info("linux-dmabuf: advertising %zu format/modifier pairs", n_formats);
	return true;
}

void rei_linux_dmabuf_finish(struct rei_server *server)
{
	struct rei_buffer_lease *lease, *tmp;
	wl_list_for_each_safe(lease, tmp, &server->buffer_leases, link) {
		wl_list_remove(&lease->link);
		lease_free(lease);
	}
	if (server->dmabuf_global)
		wl_global_destroy(server->dmabuf_global);
	server->dmabuf_global = NULL;
	free(server->dmabuf_formats);
	server->dmabuf_formats = NULL;
	server->n_dmabuf_formats = 0;
}
