#ifndef REI_PROTO_LINUX_DMABUF_H
#define REI_PROTO_LINUX_DMABUF_H

#include <stdbool.h>
#include <stddef.h>

#include "dmabuf.h"

struct rei_server;
struct wl_resource;

/*
 * zwp_linux_dmabuf_v1 (version 3): lets GPU clients hand over buffers without
 * copying. The global is created only when a backend knows which formats the
 * renderer can sample (rei_linux_dmabuf_init); without it clients fall back to
 * wl_shm.
 */
bool rei_linux_dmabuf_init(struct rei_server *server, const struct rei_format_mod *formats,
			   size_t n_formats);
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
struct rei_buffer_lease *rei_buffer_lease_new(struct wl_resource *buffer);
/* Takes ownership of `lease` (NULL is fine). */
void rei_buffer_lease_retire(struct rei_server *server, struct rei_buffer_lease *lease);
/* Sends the releases that are now due. Call after a frame is presented. */
void rei_buffer_leases_check(struct rei_server *server);

#endif
