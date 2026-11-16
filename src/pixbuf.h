#ifndef REI_PIXBUF_H
#define REI_PIXBUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fx.h"

/*
 * An immutable (once published), reference-counted block of 32-bit pixels.
 *
 * Pixels are stored exactly as Wayland's ARGB8888 / XRGB8888 are in memory on
 * little-endian machines: bytes B, G, R, A, with colour premultiplied by alpha.
 * The event thread copies client shm buffers into these, so the render threads
 * never touch libwayland objects, and a client's buffer can be released the
 * moment it is committed.
 */
/* A picture drawn over a pixbuf, at the same place and size, through a shader: the text of a themed element
 * whose text has an effect (glow, blink, shine) is kept out of the element's own picture and put in one. */
struct rei_pixbuf_layer {
	struct rei_pixbuf *pix;
	struct rei_fx fx;
};

struct rei_pixbuf {
	atomic_int refs;
	uint64_t id;     /* Unique per pixbuf; renderers key their texture caches on it. */
	int32_t width, height;
	bool opaque;     /* Alpha channel is to be ignored (XRGB). */
	struct rei_pixbuf_layer *layers;   /* Owned; set while the pixbuf is being made, never after it is shared. */
	int n_layers;
	uint8_t data[];  /* width * height * 4 bytes, tightly packed. */
};

/* The most a single surface may be: 8192x8192 pixels' worth (256 MiB). A bigger one is refused rather
 * than allowed to stall the whole compositor (and the machine) while it is allocated and copied. */
#define REI_PIXBUF_MAX_BYTES ((size_t)256 << 20)

/* Like rei_pixbuf_create, but the pixels are NOT cleared: the caller must write all of them. */
struct rei_pixbuf *rei_pixbuf_create_uninit(int32_t width, int32_t height, bool opaque);

/* Zero-filled (fully transparent). Returns NULL if the size is unreasonable. */
struct rei_pixbuf *rei_pixbuf_create(int32_t width, int32_t height, bool opaque);
struct rei_pixbuf *rei_pixbuf_ref(struct rei_pixbuf *p);
void rei_pixbuf_unref(struct rei_pixbuf *p);

/* Adds a layer (taking over the caller's reference to it). Only before `p` is handed to anyone else. */
void rei_pixbuf_add_layer(struct rei_pixbuf *p, struct rei_pixbuf *layer, const struct rei_fx *fx);

#endif
