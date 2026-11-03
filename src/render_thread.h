#ifndef REI_RENDER_THREAD_H
#define REI_RENDER_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "snapshot.h"

struct rei_server;
struct rei_render;

/*
 * Hooks a backend (or later, the renderer) supplies. All of them are called on
 * the render thread, never the event thread. This is where EGL context
 * creation, drawing and page flips will live.
 */
struct rei_render_ops {
	bool (*init)(void *data);  /* Optional. */
	void (*present)(void *data, const struct rei_scene_snapshot *snap,
			int32_t width, int32_t height);  /* Optional. */
	void (*fini)(void *data);  /* Optional. */
};

struct rei_render_params {
	uint32_t output_id;
	const char *name;  /* For the thread's log label. */
	int32_t width, height;
	int32_t refresh_mhz;
	struct rei_render_ops ops;
	void *ops_data;
};

/*
 * One render thread per output, so a slow or differently-clocked monitor
 * can't stall the others. Must be created on the event thread.
 *
 * The thread draws only when handed a new snapshot, and then no faster than
 * the output's refresh rate (it simulates vblank with a timer until a real
 * backend supplies one). After each frame it posts REI_EMSG_FRAME to the
 * server's inbox.
 */
struct rei_render *rei_render_create(struct rei_server *server,
				     const struct rei_render_params *params);

/* Hands the thread a snapshot (takes its own reference). Thread-safe. */
void rei_render_submit(struct rei_render *r, struct rei_scene_snapshot *snap);
void rei_render_resize(struct rei_render *r, int32_t width, int32_t height);

/* Stops and joins the thread. */
void rei_render_destroy(struct rei_render *r);

#endif
