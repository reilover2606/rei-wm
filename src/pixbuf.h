#ifndef REI_PIXBUF_H
#define REI_PIXBUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * An immutable (once published), reference-counted block of 32-bit pixels.
 *
 * Pixels are stored exactly as Wayland's ARGB8888 / XRGB8888 are in memory on
 * little-endian machines: bytes B, G, R, A, with colour premultiplied by alpha.
 * The event thread copies client shm buffers into these, so the render threads
 * never touch libwayland objects, and a client's buffer can be released the
 * moment it is committed.
 */
struct rei_pixbuf {
	atomic_int refs;
	uint64_t id;     /* Unique per pixbuf; renderers key their texture caches on it. */
	int32_t width, height;
	bool opaque;     /* Alpha channel is to be ignored (XRGB). */
	uint8_t data[];  /* width * height * 4 bytes, tightly packed. */
};

/* Zero-filled (fully transparent). Returns NULL if the size is unreasonable. */
struct rei_pixbuf *rei_pixbuf_create(int32_t width, int32_t height, bool opaque);
struct rei_pixbuf *rei_pixbuf_ref(struct rei_pixbuf *p);
void rei_pixbuf_unref(struct rei_pixbuf *p);

#endif
