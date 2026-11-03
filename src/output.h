#ifndef REI_OUTPUT_H
#define REI_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

#include "render_thread.h"

struct rei_render;
struct rei_wl_output;
struct rei_pixbuf;

/* What a backend supplies when it adds a monitor. */
struct rei_output_params {
	const char *name;
	int32_t width, height;  /* Physical pixels. */
	int32_t refresh_mhz;
	double scale;
	bool has_position;
	int32_t x, y;           /* Used only if has_position. */
	struct rei_render_ops render_ops;
	void *render_ops_data;
	void *backend_data;
};

/*
 * A monitor. Lives on the event thread; the render thread gets its own copy of
 * the mode and refers back to the output by `id` only, so an output can be
 * removed without leaving the render thread holding a dangling pointer.
 */
struct rei_output {
	struct wl_list link;  /* rei_server.outputs */
	uint32_t id;          /* Never reused within a run. */
	char *name;

	int32_t width, height;  /* Physical pixels. */
	int32_t refresh_mhz;
	double scale;

	bool has_position;      /* Position pinned by configuration. */
	int32_t x, y;           /* Position in the global (logical) layout. */

	struct rei_render *render;
	void *backend_data;

	struct rei_pixbuf *wallpaper;     /* Rendered for this output's size. */
	struct rei_wl_output *wl_output;  /* The protocol-visible twin. */

	/* What the render thread and clients were last told, to send only changes. */
	int32_t sent_x, sent_y, sent_width, sent_height;
	double sent_scale;

	uint64_t frames_presented;
	uint64_t last_presented_serial;
};

/* Size in the global layout's coordinate space (physical size / scale). */
void rei_output_logical_size(const struct rei_output *o, int32_t *w, int32_t *h);

/*
 * Assigns x/y to every output. Outputs with a pinned position keep it; the
 * rest are placed left to right, in list order, after the rightmost edge of
 * everything placed so far.
 */
void rei_outputs_layout(struct wl_list *outputs);

#endif
