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
	REI_RMSG_RESIZE,        /* data: struct rei_resize_msg * */
	REI_RMSG_STOP,          /* no data */
};

/* render thread -> event thread */
enum rei_event_msg {
	REI_EMSG_FRAME = 1,     /* data: struct rei_frame_event * */
	REI_EMSG_RENDER_FAILED, /* data: struct rei_frame_event * (output_id only) */
};

struct rei_resize_msg {
	int32_t width, height;
};

/* A frame reached the screen. This is what later drives wl_surface frame
 * callbacks and wp_presentation feedback, and what returns wl_buffers to
 * their clients. */
struct rei_frame_event {
	uint32_t output_id;
	uint64_t seq;           /* Per-output frame counter. */
	uint64_t present_ns;    /* CLOCK_MONOTONIC. */
	uint64_t scene_serial;  /* Serial of the snapshot that was presented. */
};

#endif
