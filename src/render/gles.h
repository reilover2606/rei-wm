#ifndef REI_GLES_H
#define REI_GLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"

/*
 * OpenGL ES 3 renderer. One instance belongs to one render thread: it makes an
 * EGL context current on the thread that creates it and must only be used from
 * there.
 *
 * It draws into an offscreen framebuffer. Backends decide what happens next:
 * the headless and nested backends read the pixels back; the DRM backend will
 * scan the buffer out.
 *
 * The context is created on EGL's surfaceless platform, so it needs no display
 * server and also runs on software rasterisers (Mesa llvmpipe).
 */
struct rei_gles;

struct rei_gles_target {
	int32_t width, height;  /* Physical pixels. */
	int32_t origin_x, origin_y;  /* Top-left of the output in logical coordinates. */
	double scale;
};

/* NULL if EGL/GLES is unavailable; the reason is logged. */
struct rei_gles *rei_gles_create(void);
void rei_gles_destroy(struct rei_gles *g);

/* Renders `snap` as seen by one output. Returns false on GL error. */
bool rei_gles_draw(struct rei_gles *g, const struct rei_scene_snapshot *snap,
		   const struct rei_gles_target *target);

/*
 * Copies the most recently drawn frame to `dst`: width*height*4 bytes, tightly
 * packed, top row first, byte order R,G,B,A.
 */
bool rei_gles_read_pixels(struct rei_gles *g, uint8_t *dst);

const char *rei_gles_renderer_name(const struct rei_gles *g);

#endif
