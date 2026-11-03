#ifndef REI_RENDER_THREAD_H
#define REI_RENDER_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "snapshot.h"

struct rei_server;
struct rei_render;
struct rei_gles;

/*
 * Hooks a backend supplies. All of them are called on the render thread,
 * never the event thread.
 */
struct rei_render_ops {
	/* Called once, before the first frame. Optional. */
	bool (*init)(void *data);
	/* Called after each frame has been drawn into the renderer's framebuffer
	 * (`gles` is NULL if rendering is unavailable). This is where a backend
	 * gets the pixels to the screen. Optional. */
	void (*present)(void *data, struct rei_gles *gles,
			const struct rei_scene_snapshot *snap,
			int32_t width, int32_t height);
	/* Called once, when the thread is shutting down. Optional. */
	void (*fini)(void *data);
};

struct rei_render_params {
	uint32_t output_id;
	const char *name;  /* For the thread's log label and screenshots. */
	int32_t width, height;
	int32_t x, y;
	double scale;
	int32_t refresh_mhz;
	struct rei_render_ops ops;
	void *ops_data;
};

/*
 * One render thread per output, so a slow or differently-clocked monitor can't
 * stall the others. Must be created on the event thread.
 *
 * The thread draws only when handed a new snapshot, and then no faster than the
 * output's refresh rate (it simulates vblank with a timer until a real backend
 * supplies one). After each frame it posts REI_EMSG_FRAME to the server's
 * inbox.
 *
 * If the OpenGL renderer cannot start, the thread still paces and reports
 * frames, so clients keep working; it just doesn't draw anything.
 */
struct rei_render *rei_render_create(struct rei_server *server,
				     const struct rei_render_params *params);

/* All of these are thread-safe. */
void rei_render_submit(struct rei_render *r, struct rei_scene_snapshot *snap);
void rei_render_set_geometry(struct rei_render *r, int32_t width, int32_t height,
			     int32_t x, int32_t y, double scale);

/* Draws the latest snapshot and writes it to `path` as a binary PPM. The result
 * is reported with REI_EMSG_SCREENSHOT. */
void rei_render_screenshot(struct rei_render *r, const char *path);

/* Stops and joins the thread. */
void rei_render_destroy(struct rei_render *r);

#endif
