#ifndef REI_PROTO_LINUX_DMABUF_H
#define REI_PROTO_LINUX_DMABUF_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "dmabuf.h"

struct rei_server;
struct wl_resource;

/*
 * zwp_linux_dmabuf_v1: lets GPU clients hand over buffers without copying. The global is created only when a backend
 * knows which formats the renderer can sample (rei_linux_dmabuf_init); without it clients fall back to wl_shm.
 *
 * With a `main_device` (the dev_t of the GPU's render node) the global is version 4 and answers get_default_feedback /
 * get_surface_feedback: clients are told which GPU to allocate on and which format/modifier pairs they may use, so a GPU
 * program (Firefox, a game) on a machine with more than one GPU allocates buffers the compositor can really use. Without
 * one it stays version 3 (a flat list of pairs).
 */
bool rei_linux_dmabuf_init(struct rei_server *server, const struct rei_format_mod *formats,
			   size_t n_formats, dev_t main_device);
/* The usable pairs changed (a GPU came or went): replaces the list and tells the clients that asked for feedback. */
void rei_linux_dmabuf_update_formats(struct rei_server *server, const struct rei_format_mod *formats, size_t n_formats);
void rei_linux_dmabuf_finish(struct rei_server *server);

/* The dma-buf behind a wl_buffer, or NULL if it is not one. Borrowed. */
struct rei_dmabuf *rei_dmabuf_from_buffer(struct wl_resource *buffer);

/*
 * Releasing dma-bufs. A client may not touch a buffer until we say so, and the
 * GPU may still be reading it for any frame already sent to a render thread. So a
 * surface holds a "lease" on the wl_buffer it shows; when the content is
 * replaced the lease is retired, and wl_buffer.release goes out only once every
 * output has presented a frame built after the replacement.
 */
struct rei_buffer_lease;
struct rei_sync_points;
struct rei_buffer_lease *rei_buffer_lease_new(struct wl_resource *buffer);
/* Explicit synchronization: when the lease is released the release point is signaled (and no wl_buffer.release is sent).
 * Takes the release timeline out of `points`. */
void rei_buffer_lease_set_release(struct rei_buffer_lease *lease, struct rei_sync_points *points);
/* Takes ownership of `lease` (NULL is fine). */
void rei_buffer_lease_retire(struct rei_server *server, struct rei_buffer_lease *lease);
/* Sends the releases that are now due. Call after a frame is presented. */
void rei_buffer_leases_check(struct rei_server *server);

#endif
