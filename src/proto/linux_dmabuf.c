#include "proto/linux_dmabuf.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server.h>

#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "proto/syncobj.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define DMABUF_VERSION_NO_FEEDBACK 3
#define DMABUF_VERSION_FEEDBACK 4

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
	struct rei_sync_points sync;   /* explicit synchronization: the release point to signal, if any */
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

void rei_buffer_lease_set_release(struct rei_buffer_lease *lease, struct rei_sync_points *points)
{
	lease->sync.has = true;
	lease->sync.release_tl = points->release_tl;
	lease->sync.release = points->release;
	points->release_tl = NULL;
}

static void lease_free(struct rei_buffer_lease *lease)
{
	if (lease->buffer)
		wl_list_remove(&lease->destroy.link);
	rei_sync_points_clear(&lease->sync);
	free(lease);
}

void rei_buffer_lease_retire(struct rei_server *server, struct rei_buffer_lease *lease)
{
	if (!lease)
		return;
	if (!lease->buffer && !lease->sync.has) {   /* nothing to tell: no wl_buffer left, no point to signal */
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
		if (lease->sync.has)
			rei_sync_points_signal_release(server, &lease->sync);   /* explicit sync: the point, not wl_buffer.release */
		else if (lease->buffer)
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

/* ---- feedback (version 4) ---- */

struct feedback {
	struct rei_server *server;
	struct wl_resource *resource;
	struct wl_list link;   /* rei_server.dmabuf_feedbacks */
};

/* The format table: 16 bytes per pair (the format, 4 bytes of padding, the modifier), in a memfd the client maps. */
static int make_format_table(const struct rei_format_mod *formats, size_t n, size_t *size)
{
	*size = n * 16;
	int fd = memfd_create("rei-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0 || ftruncate(fd, (off_t)*size) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	uint8_t *map = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		uint32_t format = formats[i].format, pad = 0;
		uint64_t modifier = formats[i].modifier;
		memcpy(map + i * 16, &format, 4);
		memcpy(map + i * 16 + 4, &pad, 4);
		memcpy(map + i * 16 + 8, &modifier, 8);
	}
	munmap(map, *size);
	fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
	return fd;
}

/* One complete feedback: the main device, one tranche for it with every usable pair, done. */
static void send_feedback(struct rei_server *s, struct wl_resource *r)
{
	size_t size;
	int fd = make_format_table(s->dmabuf_formats, s->n_dmabuf_formats, &size);
	if (fd < 0) {
		wl_resource_post_no_memory(r);
		return;
	}
	zwp_linux_dmabuf_feedback_v1_send_format_table(r, fd, (uint32_t)size);
	close(fd);

	struct wl_array dev;
	wl_array_init(&dev);
	dev_t *d = wl_array_add(&dev, sizeof(dev_t));
	*d = s->dmabuf_main_device;
	zwp_linux_dmabuf_feedback_v1_send_main_device(r, &dev);

	/* The pairs, as indices into the table (16-bit each: a list longer than 65535 is cut, which no GPU comes near). */
	struct wl_array idx;
	wl_array_init(&idx);
	size_t n = s->n_dmabuf_formats < 65535 ? s->n_dmabuf_formats : 65535;
	uint16_t *ix = wl_array_add(&idx, n * sizeof(uint16_t));
	for (size_t i = 0; i < n; i++)
		ix[i] = (uint16_t)i;
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(r, &dev);
	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(r, 0);
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(r, &idx);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(r);
	zwp_linux_dmabuf_feedback_v1_send_done(r);
	wl_array_release(&idx);
	wl_array_release(&dev);
}

static void feedback_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
	.destroy = feedback_destroy_request,
};

static void feedback_resource_destroy(struct wl_resource *r)
{
	struct feedback *f = wl_resource_get_user_data(r);
	wl_list_remove(&f->link);
	free(f);
}

static void make_feedback(struct wl_client *client, struct wl_resource *dmabuf_res, uint32_t id)
{
	struct rei_server *s = wl_resource_get_user_data(dmabuf_res);
	struct feedback *f = rei_xcalloc(1, sizeof *f);
	f->server = s;
	f->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
					 wl_resource_get_version(dmabuf_res), id);
	if (!f->resource) {
		free(f);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(f->resource, &feedback_impl, f, feedback_resource_destroy);
	wl_list_insert(&s->dmabuf_feedbacks, &f->link);
	send_feedback(s, f->resource);
}

static void dmabuf_get_default_feedback(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	make_feedback(client, resource, id);
}

/* Every surface gets the same: the compositor draws every window on the same device(s). */
static void dmabuf_get_surface_feedback(struct wl_client *client, struct wl_resource *resource, uint32_t id,
					struct wl_resource *surface)
{
	(void)surface;
	make_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = dmabuf_destroy_request,
	.create_params = dmabuf_create_params,
	.get_default_feedback = dmabuf_get_default_feedback,
	.get_surface_feedback = dmabuf_get_surface_feedback,
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

	/* Version 4 clients ask for feedback instead of listening for these (and are not sent them). */
	if (version >= DMABUF_VERSION_FEEDBACK)
		return;

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

void rei_linux_dmabuf_update_formats(struct rei_server *server, const struct rei_format_mod *formats, size_t n_formats)
{
	if (!server->dmabuf_global || n_formats == 0)
		return;
	free(server->dmabuf_formats);
	server->dmabuf_formats = rei_xcalloc(n_formats, sizeof *formats);
	memcpy(server->dmabuf_formats, formats, n_formats * sizeof *formats);
	server->n_dmabuf_formats = n_formats;
	struct feedback *f;
	wl_list_for_each(f, &server->dmabuf_feedbacks, link)
		send_feedback(server, f->resource);
	rei_info("linux-dmabuf: now advertising %zu format/modifier pairs", n_formats);
}

bool rei_linux_dmabuf_init(struct rei_server *server, const struct rei_format_mod *formats,
			   size_t n_formats, dev_t main_device)
{
	if (server->dmabuf_global || n_formats == 0)
		return n_formats != 0 || server->dmabuf_global;
	server->dmabuf_formats = rei_xcalloc(n_formats, sizeof *formats);
	memcpy(server->dmabuf_formats, formats, n_formats * sizeof *formats);
	server->n_dmabuf_formats = n_formats;
	server->dmabuf_main_device = main_device;
	wl_list_init(&server->dmabuf_feedbacks);
	server->dmabuf_global = wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface,
						 main_device ? DMABUF_VERSION_FEEDBACK : DMABUF_VERSION_NO_FEEDBACK,
						 server, dmabuf_bind);
	if (!server->dmabuf_global) {
		free(server->dmabuf_formats);
		server->dmabuf_formats = NULL;
		server->n_dmabuf_formats = 0;
		return false;
	}
	rei_info("linux-dmabuf: advertising %zu format/modifier pairs%s", n_formats,
		 main_device ? " (version 4, with feedback naming the main device)" : "");
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
