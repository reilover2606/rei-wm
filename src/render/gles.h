#ifndef REI_GLES_H
#define REI_GLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dmabuf.h"
#include "snapshot.h"

/*
 * OpenGL ES 3 renderer. One instance belongs to one render thread: it makes an
 * EGL context current on the thread that creates it and must only be used from
 * there.
 *
 * It draws into an offscreen framebuffer: either its own (the headless and
 * nested backends read the pixels back) or one backed by a dma-buf (the DRM
 * backend scans those out). The context needs no display server: it lives on
 * EGL's surfaceless platform, or on a GBM device for real hardware.
 */
struct rei_gles;
struct rei_gles_buffer;

enum rei_gles_platform {
	REI_GLES_SURFACELESS,  /* Software or any GPU, no display (default). */
	REI_GLES_GBM,          /* A GBM device, for scanout buffers and dma-buf import. */
};

struct rei_gles_options {
	enum rei_gles_platform platform;
	void *native_display;  /* The gbm_device, for REI_GLES_GBM. */
};

struct rei_gles_target {
	int32_t width, height;  /* Physical pixels. */
	int32_t origin_x, origin_y;  /* Top-left of the output in logical coordinates. */
	double scale;
	bool draw_cursor;       /* False if a hardware cursor shows the pointer. */
};

/* NULL if EGL/GLES is unavailable; the reason is logged. `opts` may be NULL. */
struct rei_gles *rei_gles_create(const struct rei_gles_options *opts);
void rei_gles_destroy(struct rei_gles *g);

/* Whether client dma-bufs can be drawn and our own buffers rendered into
 * (EGL_EXT_image_dma_buf_import and the GLES image extensions). */
bool rei_gles_supports_dmabuf(const struct rei_gles *g);

/*
 * A render target backed by a dma-buf (from GBM, for scanout). The renderer
 * imports it as an EGL image bound to a framebuffer; the buffer's own memory is
 * what ends up on screen. NULL if the import fails (the reason is logged).
 */
struct rei_gles_buffer *rei_gles_buffer_import(struct rei_gles *g, const struct rei_dmabuf *dmabuf);
void rei_gles_buffer_destroy(struct rei_gles *g, struct rei_gles_buffer *buf);

/*
 * Renders `snap` as seen by one output into `buf`, or into the renderer's own
 * framebuffer (readable with rei_gles_read_pixels) if `buf` is NULL. Returns
 * false on GL error. Flushes: when this returns the commands are submitted, and
 * the result is safe to scan out.
 */
bool rei_gles_draw_into(struct rei_gles *g, const struct rei_scene_snapshot *snap,
			const struct rei_gles_target *target, struct rei_gles_buffer *buf);

/* rei_gles_draw_into(g, snap, target, NULL). */
bool rei_gles_draw(struct rei_gles *g, const struct rei_scene_snapshot *snap,
		   const struct rei_gles_target *target);

/*
 * Copies the most recently drawn frame of the renderer's own framebuffer to
 * `dst`: width*height*4 bytes, tightly packed, top row first, byte order R,G,B,A.
 */
bool rei_gles_read_pixels(struct rei_gles *g, uint8_t *dst);

const char *rei_gles_renderer_name(const struct rei_gles *g);

/*
 * The (format, modifier) pairs the renderer can sample from as ordinary 2D
 * textures, for advertising through linux-dmabuf. Needs only a GBM device, not a
 * context. Returns the number written to *out (malloc'd; free() it), or 0.
 */
int rei_gles_query_dmabuf_formats(void *gbm_device, struct rei_format_mod **out);

#endif
