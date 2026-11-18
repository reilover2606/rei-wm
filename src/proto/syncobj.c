#include "proto/syncobj.h"

#include <errno.h>
#include <event2/event.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server.h>

#include "linux-drm-syncobj-v1-server-protocol.h"
#include "proto/linux_dmabuf.h"
#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_syncobj {
	struct rei_server *server;
	const struct rei_syncobj_ops *ops;
	void *ctx;
	struct wl_global *global;
};

struct rei_syncobj_timeline {
	int refs;
	struct rei_syncobj *sync;
	uint32_t handle;
};

struct rei_syncobj_surface {
	struct rei_syncobj *sync;
	struct wl_resource *resource;
	struct rei_surface *surface;   /* NULL once the wl_surface is gone */
	struct wl_listener surface_destroy;
	struct rei_syncobj_timeline *acquire_tl, *release_tl;
	uint64_t acquire, release;
	bool has_acquire, has_release;
};

/* ------------------------------------------------------------------------ */
/* Timelines                                                                */
/* ------------------------------------------------------------------------ */

struct rei_syncobj_timeline *rei_syncobj_timeline_ref(struct rei_syncobj_timeline *t)
{
	if (t)
		t->refs++;
	return t;
}

void rei_syncobj_timeline_unref(struct rei_syncobj_timeline *t)
{
	if (!t || --t->refs > 0)
		return;
	if (t->sync)
		t->sync->ops->release_handle(t->sync->ctx, t->handle);
	free(t);
}

static void timeline_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
	.destroy = timeline_destroy_request,
};

static void timeline_resource_destroy(struct wl_resource *r)
{
	rei_syncobj_timeline_unref(wl_resource_get_user_data(r));
}

/* ------------------------------------------------------------------------ */
/* Waiting for an acquire point                                             */
/* ------------------------------------------------------------------------ */

struct rei_sync_wait {
	struct event *ev;
	int fd;
	void (*ready)(void *data);
	void *data;
};

static void on_wait_ready(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_sync_wait *w = arg;
	void (*ready)(void *) = w->ready;
	void *data = w->data;
	event_free(w->ev);
	close(w->fd);
	free(w);
	ready(data);
}

struct rei_sync_wait *rei_sync_wait_start(struct rei_server *server, struct rei_syncobj_timeline *tl, uint64_t point,
					  void (*ready)(void *data), void *data)
{
	struct rei_syncobj *sync = server->syncobj;
	if (!sync || !tl)
		return NULL;
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0)
		return NULL;
	if (sync->ops->wait_eventfd(sync->ctx, tl->handle, point, fd) != 0) {
		close(fd);
		return NULL;
	}
	struct rei_sync_wait *w = rei_xcalloc(1, sizeof *w);
	w->fd = fd;
	w->ready = ready;
	w->data = data;
	w->ev = event_new(server->base, fd, EV_READ, on_wait_ready, w);
	event_add(w->ev, NULL);
	return w;
}

void rei_sync_wait_cancel(struct rei_sync_wait *w)
{
	if (!w)
		return;
	event_free(w->ev);
	close(w->fd);
	free(w);
}

/* ------------------------------------------------------------------------ */
/* Points                                                                   */
/* ------------------------------------------------------------------------ */

void rei_sync_points_clear(struct rei_sync_points *p)
{
	rei_syncobj_timeline_unref(p->acquire_tl);
	rei_syncobj_timeline_unref(p->release_tl);
	*p = (struct rei_sync_points){ 0 };
}

void rei_sync_points_signal_release(struct rei_server *server, struct rei_sync_points *p)
{
	struct rei_syncobj *sync = server->syncobj;
	if (sync && p->release_tl && sync->ops->signal(sync->ctx, p->release_tl->handle, p->release) != 0)
		rei_debug("syncobj: cannot signal a release point: %s", strerror(errno));
	rei_syncobj_timeline_unref(p->release_tl);   /* (the acquire half, if any, is still wanted) */
	p->release_tl = NULL;
	p->release = 0;
}

/* ------------------------------------------------------------------------ */
/* wp_linux_drm_syncobj_surface_v1                                          */
/* ------------------------------------------------------------------------ */

static void sync_surface_clear_pending(struct rei_syncobj_surface *ss)
{
	rei_syncobj_timeline_unref(ss->acquire_tl);
	rei_syncobj_timeline_unref(ss->release_tl);
	ss->acquire_tl = ss->release_tl = NULL;
	ss->has_acquire = ss->has_release = false;
}

static void surface_obj_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static bool set_point(struct wl_resource *r, struct wl_resource *timeline_res, uint32_t hi, uint32_t lo, bool acquire)
{
	struct rei_syncobj_surface *ss = wl_resource_get_user_data(r);
	if (!ss->surface) {
		wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE, "the wl_surface was destroyed");
		return false;
	}
	struct rei_syncobj_timeline *tl = wl_resource_get_user_data(timeline_res);
	uint64_t point = ((uint64_t)hi << 32) | lo;
	if (acquire) {
		rei_syncobj_timeline_unref(ss->acquire_tl);
		ss->acquire_tl = rei_syncobj_timeline_ref(tl);
		ss->acquire = point;
		ss->has_acquire = true;
	} else {
		rei_syncobj_timeline_unref(ss->release_tl);
		ss->release_tl = rei_syncobj_timeline_ref(tl);
		ss->release = point;
		ss->has_release = true;
	}
	return true;
}

static void surface_obj_set_acquire_point(struct wl_client *c, struct wl_resource *r, struct wl_resource *timeline,
					  uint32_t hi, uint32_t lo)
{
	(void)c;
	set_point(r, timeline, hi, lo, true);
}

static void surface_obj_set_release_point(struct wl_client *c, struct wl_resource *r, struct wl_resource *timeline,
					  uint32_t hi, uint32_t lo)
{
	(void)c;
	set_point(r, timeline, hi, lo, false);
}

static const struct wp_linux_drm_syncobj_surface_v1_interface surface_obj_impl = {
	.destroy = surface_obj_destroy_request,
	.set_acquire_point = surface_obj_set_acquire_point,
	.set_release_point = surface_obj_set_release_point,
};

static void on_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_syncobj_surface *ss = wl_container_of(l, ss, surface_destroy);
	wl_list_remove(&ss->surface_destroy.link);
	wl_list_init(&ss->surface_destroy.link);
	ss->surface = NULL;
}

static void surface_obj_resource_destroy(struct wl_resource *r)
{
	struct rei_syncobj_surface *ss = wl_resource_get_user_data(r);
	if (ss->surface) {
		wl_list_remove(&ss->surface_destroy.link);
		ss->surface->sync_surface = NULL;
	}
	sync_surface_clear_pending(ss);
	free(ss);
}

bool rei_syncobj_commit_begin(struct rei_surface *surface, struct rei_sync_points *out)
{
	*out = (struct rei_sync_points){ 0 };
	struct rei_syncobj_surface *ss = surface->sync_surface;
	if (!ss)
		return true;
	struct wl_resource *r = ss->resource;
	struct rei_server *server = surface->server;
	bool buffer = surface->pending.buffer_set && surface->pending.buffer != NULL;

	if (!buffer) {
		if (ss->has_acquire || ss->has_release) {
			wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
					       "a timeline point was set without a buffer");
			return false;
		}
		return true;
	}
	if (!ss->has_acquire) {
		wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT, "no acquire point was set");
		return false;
	}
	if (!ss->has_release) {
		wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT, "no release point was set");
		return false;
	}
	bool dmabuf = rei_dmabuf_from_buffer(surface->pending.buffer) != NULL;
	if (!dmabuf && !(server->syncobj && server->syncobj->ops->allow_shm)) {
		wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER,
				       "only dma-buf buffers support explicit synchronization");
		return false;
	}
	if (ss->acquire_tl == ss->release_tl && ss->acquire >= ss->release) {
		wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
				       "the acquire point must be less than the release point on the same timeline");
		return false;
	}
	out->has = true;
	out->acquire_tl = ss->acquire_tl;
	out->release_tl = ss->release_tl;
	out->acquire = ss->acquire;
	out->release = ss->release;
	ss->acquire_tl = ss->release_tl = NULL;   /* moved */
	ss->has_acquire = ss->has_release = false;
	return true;
}

/* ------------------------------------------------------------------------ */
/* The manager                                                              */
/* ------------------------------------------------------------------------ */

static void manager_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void manager_get_surface(struct wl_client *client, struct wl_resource *manager, uint32_t id,
				struct wl_resource *surface_res)
{
	struct rei_syncobj *sync = wl_resource_get_user_data(manager);
	struct rei_surface *surface = wl_resource_get_user_data(surface_res);
	if (surface->sync_surface) {
		wl_resource_post_error(manager, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
				       "the surface already has a synchronization object");
		return;
	}
	struct rei_syncobj_surface *ss = rei_xcalloc(1, sizeof *ss);
	ss->sync = sync;
	ss->surface = surface;
	ss->resource = wl_resource_create(client, &wp_linux_drm_syncobj_surface_v1_interface,
					  wl_resource_get_version(manager), id);
	if (!ss->resource) {
		free(ss);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(ss->resource, &surface_obj_impl, ss, surface_obj_resource_destroy);
	ss->surface_destroy.notify = on_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &ss->surface_destroy);
	surface->sync_surface = ss;
}

static void manager_import_timeline(struct wl_client *client, struct wl_resource *manager, uint32_t id, int32_t fd)
{
	struct rei_syncobj *sync = wl_resource_get_user_data(manager);
	uint32_t handle = 0;
	int ret = sync->ops->import(sync->ctx, fd, &handle);
	close(fd);
	if (ret != 0) {
		wl_resource_post_error(manager, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
				       "the timeline could not be imported");
		return;
	}
	struct rei_syncobj_timeline *tl = rei_xcalloc(1, sizeof *tl);
	tl->refs = 1;
	tl->sync = sync;
	tl->handle = handle;
	struct wl_resource *r = wl_resource_create(client, &wp_linux_drm_syncobj_timeline_v1_interface,
						   wl_resource_get_version(manager), id);
	if (!r) {
		rei_syncobj_timeline_unref(tl);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &timeline_impl, tl, timeline_resource_destroy);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface manager_impl = {
	.destroy = manager_destroy_request,
	.get_surface = manager_get_surface,
	.import_timeline = manager_import_timeline,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &wp_linux_drm_syncobj_manager_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

bool rei_syncobj_init(struct rei_server *server, const struct rei_syncobj_ops *ops, void *ctx)
{
	if (server->syncobj)
		return true;
	struct rei_syncobj *sync = rei_xcalloc(1, sizeof *sync);
	sync->server = server;
	sync->ops = ops;
	sync->ctx = ctx;
	sync->global = wl_global_create(server->display, &wp_linux_drm_syncobj_manager_v1_interface, 1, sync, manager_bind);
	if (!sync->global) {
		free(sync);
		return false;
	}
	server->syncobj = sync;
	rei_info("explicit synchronization (wp_linux_drm_syncobj_v1) is offered");
	return true;
}

void rei_syncobj_finish(struct rei_server *server)
{
	struct rei_syncobj *sync = server->syncobj;
	if (!sync)
		return;
	server->syncobj = NULL;
	if (sync->global)
		wl_global_destroy(sync->global);
	free(sync);
}

/* ------------------------------------------------------------------------ */
/* The kernel's sync objects                                                */
/* ------------------------------------------------------------------------ */

#ifdef REI_HAVE_DRM
#include <xf86drm.h>

static int drm_import(void *ctx, int fd, uint32_t *handle)
{
	return drmSyncobjFDToHandle((int)(intptr_t)ctx, fd, handle);
}

static void drm_release_handle(void *ctx, uint32_t handle)
{
	drmSyncobjDestroy((int)(intptr_t)ctx, handle);
}

static int drm_wait_eventfd(void *ctx, uint32_t handle, uint64_t point, int efd)
{
#ifdef REI_HAVE_SYNCOBJ_EVENTFD
	return drmSyncobjEventfd((int)(intptr_t)ctx, handle, point, efd, 0);   /* 0: when signaled (not merely submitted) */
#else
	(void)ctx; (void)handle; (void)point; (void)efd;
	return -1;
#endif
}

static int drm_signal(void *ctx, uint32_t handle, uint64_t point)
{
	return drmSyncobjTimelineSignal((int)(intptr_t)ctx, &handle, &point, 1);
}

static const struct rei_syncobj_ops drm_ops = { drm_import, drm_release_handle, drm_wait_eventfd, drm_signal, false };

bool rei_syncobj_init_drm(struct rei_server *server, int drm_fd)
{
#ifndef REI_HAVE_SYNCOBJ_EVENTFD
	(void)server; (void)drm_fd;
	rei_info("explicit synchronization: not offered (libdrm has no drmSyncobjEventfd)");
	return false;
#else
	uint64_t cap = 0;
	if (drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) {
		rei_info("explicit synchronization: not offered (the kernel driver has no timeline sync objects)");
		return false;
	}
	/* The one call that needs a newer kernel (6.6): try it on a signaled sync object. */
	uint32_t handle = 0;
	int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (efd < 0 || drmSyncobjCreate(drm_fd, DRM_SYNCOBJ_CREATE_SIGNALED, &handle) != 0) {
		if (efd >= 0)
			close(efd);
		rei_info("explicit synchronization: not offered (cannot create a sync object)");
		return false;
	}
	int ret = drmSyncobjEventfd(drm_fd, handle, 0, efd, 0);
	close(efd);
	drmSyncobjDestroy(drm_fd, handle);
	if (ret != 0) {
		rei_info("explicit synchronization: not offered (the kernel has no sync object eventfd, Linux 6.6 or newer)");
		return false;
	}
	return rei_syncobj_init(server, &drm_ops, (void *)(intptr_t)drm_fd);
#endif
}
#endif
