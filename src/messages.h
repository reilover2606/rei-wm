#ifndef REI_MESSAGES_H
#define REI_MESSAGES_H

#include <stdint.h>

/*
 * Cross-thread message vocabulary. The event thread owns all protocol and
 * window-manager state; render threads only ever see immutable data handed to
 * them in these messages, and report back the same way.
 */

/* event thread -> render thread */
enum rei_render_msg {
	REI_RMSG_SNAPSHOT = 1,  /* data: struct rei_scene_snapshot * (one ref) */
	REI_RMSG_GEOMETRY,      /* data: struct rei_geometry_msg * */
	REI_RMSG_SCREENSHOT,    /* data: char * path (PPM), malloc'd */
	REI_RMSG_VBLANK,        /* data: struct rei_vblank_msg *; a queued page flip completed */
	REI_RMSG_PAUSE,         /* data: struct rei_pause_msg *; the display is lost or regained */
	REI_RMSG_STOP,          /* no data */
};

/* render thread -> event thread */
enum rei_event_msg {
	REI_EMSG_FRAME = 1,     /* data: struct rei_frame_event * */
	REI_EMSG_RENDER_FAILED, /* data: struct rei_frame_event * (output_id only) */
	REI_EMSG_SCREENSHOT,    /* data: struct rei_screenshot_event * */
};

/* Where an output sits and how big it is. Sent whenever any of it changes. */
struct rei_geometry_msg {
	int32_t width, height;  /* Physical pixels. */
	int32_t x, y;           /* Position in the global logical layout. */
	double scale;
};

/* The display hardware finished a flip we queued. */
struct rei_vblank_msg {
	uint64_t present_ns;  /* CLOCK_MONOTONIC, as the kernel stamped it (0: unknown). */
};

struct rei_pause_msg {
	int paused;  /* 1: stop drawing (e.g. VT switched away); 0: resume. */
};

/* A frame reached the screen. This is what drives wl_surface frame callbacks
 * and, later, presentation feedback. */
struct rei_frame_event {
	uint32_t output_id;
	uint64_t seq;           /* Per-output frame counter. */
	uint64_t present_ns;    /* CLOCK_MONOTONIC. */
	uint64_t scene_serial;  /* Serial of the snapshot that was presented. */
};

struct rei_screenshot_event {
	uint32_t output_id;
	int ok;
	char path[256];
};

#endif
