#ifndef REI_PROTO_SYNCOBJ_H
#define REI_PROTO_SYNCOBJ_H

#include <stdbool.h>
#include <stdint.h>

struct rei_server;
struct rei_surface;
struct event;

/*
 * wp_linux_drm_syncobj_v1: explicit synchronization. A client that renders with the GPU hands over, with each buffer, a
 * DRM sync object timeline point that is signaled when its rendering is done (acquire) and one the compositor signals
 * when it has finished reading the buffer (release), instead of relying on the kernel's implicit ordering of
 * buffer access. Some drivers have no implicit synchronization (NVIDIA before its 555 series), where without
 * this a client's buffer can be shown half drawn or overwritten while it is still being read; the others support
 * it, and benefit as well (no wl_buffer.release round trip).
 *
 * How it is done here, with only the sync object calls and no GPU fences:
 *   - acquire: a commit that carries an acquire point is held (with every later commit of that surface, in order) until
 *     the point is signaled; only then does it take effect, so nothing is ever sampled early;
 *   - release: the point is signaled when the buffer's lease is released, that is once a frame without the buffer has been
 *     presented on every output, by which time the GPU has long finished reading (a frame is complete before the page flip
 *     that shows it). wl_buffer.release is not sent for such buffers.
 *
 * The kernel calls are behind `rei_syncobj_ops`, so the protocol logic can be tested without a GPU.
 */
struct rei_syncobj_ops {
	/* Imports a client's sync object (a file descriptor, not consumed) as a handle; 0 on success. */
	int (*import)(void *ctx, int fd, uint32_t *handle);
	void (*release_handle)(void *ctx, uint32_t handle);
	/* Makes `eventfd` readable once `point` of the timeline is signaled (at once if it already is); 0 on success. */
	int (*wait_eventfd)(void *ctx, uint32_t handle, uint64_t point, int eventfd);
	int (*signal)(void *ctx, uint32_t handle, uint64_t point);
	/* Tests only: wl_shm buffers count as supporting explicit synchronization (the real kernel path is for dma-bufs). */
	bool allow_shm;
};

/* Creates the global. `ctx` is passed to the ops. Returns false if it could not. */
bool rei_syncobj_init(struct rei_server *server, const struct rei_syncobj_ops *ops, void *ctx);
void rei_syncobj_finish(struct rei_server *server);

#ifdef REI_HAVE_DRM
/* The real kernel backend on a DRM device fd, if the device and kernel can do it (timeline sync objects and
 * DRM_IOCTL_SYNCOBJ_EVENTFD, Linux 6.6+). Returns false (and logs why) if not: the global is then not offered, and
 * clients use implicit synchronization as before. */
bool rei_syncobj_init_drm(struct rei_server *server, int drm_fd);
#endif

/* ---- used by the surface commit code ---- */

struct rei_syncobj_timeline;
struct rei_syncobj_timeline *rei_syncobj_timeline_ref(struct rei_syncobj_timeline *t);
void rei_syncobj_timeline_unref(struct rei_syncobj_timeline *t);

/* What a commit carries. */
struct rei_sync_points {
	bool has;                                /* explicit synchronization applies to this commit's buffer */
	struct rei_syncobj_timeline *acquire_tl, *release_tl;
	uint64_t acquire, release;
};
/* Called at the start of a commit on a surface that has a synchronization object: checks the rules (raising the protocol
 * errors) and moves the pending points into `out`. False if an error was raised and the commit must be dropped. */
bool rei_syncobj_commit_begin(struct rei_surface *surface, struct rei_sync_points *out);
void rei_sync_points_clear(struct rei_sync_points *p);   /* drops the references */
/* Signals the release point (the buffer is free) and drops its reference; the acquire half is left alone. */
void rei_sync_points_signal_release(struct rei_server *server, struct rei_sync_points *p);

/* Waits for a point; `ready(data)` runs on the event loop once it is signaled. NULL (and `ready` is not called) if the wait
 * cannot be set up: the caller then treats the point as signaled. */
struct rei_sync_wait;
struct rei_sync_wait *rei_sync_wait_start(struct rei_server *server, struct rei_syncobj_timeline *tl, uint64_t point,
					  void (*ready)(void *data), void *data);
void rei_sync_wait_cancel(struct rei_sync_wait *w);

#endif
