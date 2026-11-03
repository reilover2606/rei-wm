#ifndef REI_RENDER_THREAD_H
#define REI_RENDER_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "render/gles.h"
#include "snapshot.h"

struct rei_server;
struct rei_render;
struct rei_gles;

/* What a vsync-driven render() did. */
enum rei_render_result {
	REI_RENDER_FAILED = 0,  /* Nothing was queued; the frame is dropped. */
	REI_RENDER_FLIP,        /* A flip is queued; the backend will call rei_render_notify_vblank(). */
	REI_RENDER_TIMED,       /* Shown at once, no hardware event: complete after one refresh period. */
};

/*
 * Hooks a backend supplies. All of them are called on the render thread,
 * never the event thread.
 *
 * Two pacing styles:
 *
 *  - Timer-driven (`render` is NULL): the thread simulates vblank with a timer,
 *    draws into its own framebuffer, and calls `present` so the backend can copy
 *    the pixels out. Used by headless and nested outputs.
 *
 *  - Vsync-driven (`render` set): the backend draws into its own buffers and
 *    queues a flip; the thread then waits for the flip to complete before it
 *    draws again, and only then reports the frame as presented. This is how real
 *    displays are paced.
 */
struct rei_render_ops {
	/* How to create the renderer. Zero means "surfaceless". */
	struct rei_gles_options gles;

	/* Called once, after the renderer exists and before the first frame
	 * (`gles` is NULL if rendering is unavailable). Optional. */
	bool (*init)(void *data, struct rei_gles *gles);
	/* Timer-driven: called after each frame has been drawn into the renderer's
	 * own framebuffer (`gles` is NULL if rendering is unavailable). Optional. */
	void (*present)(void *data, struct rei_gles *gles,
			const struct rei_scene_snapshot *snap,
			int32_t width, int32_t height);
	/* Vsync-driven: draw `snap` and queue it for display. `target` carries
	 * the output's geometry. Set draw_cursor to false in a copy of it when the
	 * hardware cursor shows the pointer. */
	enum rei_render_result (*render)(void *data, struct rei_gles *gles,
					 const struct rei_scene_snapshot *snap,
					 const struct rei_gles_target *target);
	/* The display was lost (paused) or came back. A pending flip is abandoned. */
	void (*set_paused)(void *data, bool paused);
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
/* A flip queued by the vsync-driven render() has reached the screen. `present_ns`
 * is the kernel's timestamp for it (CLOCK_MONOTONIC), or 0. */
void rei_render_notify_vblank(struct rei_render *r, uint64_t present_ns);
/* Stops drawing (VT switched away) or resumes, redrawing the latest scene. */
void rei_render_set_paused(struct rei_render *r, bool paused);
void rei_render_set_geometry(struct rei_render *r, int32_t width, int32_t height,
			     int32_t x, int32_t y, double scale);

/* Draws the latest snapshot and writes it to `path` as a binary PPM. The result
 * is reported with REI_EMSG_SCREENSHOT. */
void rei_render_screenshot(struct rei_render *r, const char *path);

/* Stops and joins the thread. */
void rei_render_destroy(struct rei_render *r);

#endif
