#ifndef REI_DMABUF_H
#define REI_DMABUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A GPU buffer shared as a Linux dma-buf: up to four planes, each a file
 * descriptor plus offset and stride, in a pixel format and layout (modifier)
 * named by DRM fourcc codes. Clients hand these over through linux-dmabuf; the
 * compositor allocates its own for rendering and scanout.
 *
 * Immutable and reference-counted, like rei_pixbuf, so the event thread and the
 * render threads can share one without locks. The descriptors are owned: they
 * close when the last reference goes.
 */

/* The DRM fourcc codes and modifiers this code needs, defined here so the core
 * does not depend on libdrm's headers. (They are stable kernel ABI.) */
#define REI_FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define REI_FORMAT_ARGB8888 REI_FOURCC('A', 'R', '2', '4')
#define REI_FORMAT_XRGB8888 REI_FOURCC('X', 'R', '2', '4')
#define REI_FORMAT_ABGR8888 REI_FOURCC('A', 'B', '2', '4')
#define REI_FORMAT_XBGR8888 REI_FOURCC('X', 'B', '2', '4')
#define REI_MOD_LINEAR 0ull
#define REI_MOD_INVALID 0x00ffffffffffffffull

#define REI_DMABUF_MAX_PLANES 4

struct rei_dmabuf {
	atomic_int refs;
	uint64_t id;  /* Unique; renderers key their caches on it. */
	int32_t width, height;
	uint32_t format;     /* DRM fourcc */
	uint64_t modifier;
	int n_planes;
	int fd[REI_DMABUF_MAX_PLANES];
	uint32_t offset[REI_DMABUF_MAX_PLANES];
	uint32_t stride[REI_DMABUF_MAX_PLANES];
	bool y_invert;       /* Rows are stored bottom to top (linux-dmabuf flag). */
	bool opaque;         /* The format has no alpha channel. */
};

/* A pixel format together with a memory layout it can be used in. */
struct rei_format_mod {
	uint32_t format;
	uint64_t modifier;
};

/* Takes ownership of the descriptors in `proto` (n_planes of them); on failure
 * returns NULL and closes them. The id, refcount and `opaque` are filled in. */
struct rei_dmabuf *rei_dmabuf_create(const struct rei_dmabuf *proto);
struct rei_dmabuf *rei_dmabuf_ref(struct rei_dmabuf *d);
void rei_dmabuf_unref(struct rei_dmabuf *d);

/* Whether a fourcc has no alpha channel, and so draws opaque. */
bool rei_format_is_opaque(uint32_t format);

/* Bytes per pixel for the formats this compositor handles (0 if unknown). */
int rei_format_bpp(uint32_t format);

bool rei_format_mod_supported(const struct rei_format_mod *list, size_t n,
			      uint32_t format, uint64_t modifier);

/* Pairs present in both lists. Returns the count; *out is caller-freed. */
size_t rei_format_mod_intersect(const struct rei_format_mod *a, size_t na,
				const struct rei_format_mod *b, size_t nb,
				struct rei_format_mod **out);

#endif
