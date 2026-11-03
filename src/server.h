#ifndef REI_SERVER_H
#define REI_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

#include "config.h"
#include "output.h"
#include "util/msgq.h"

struct event;
struct event_base;
struct wl_display;
struct wl_event_loop;
struct rei_backend;
struct rei_seat;
struct rei_input;
struct rei_image;

/*
 * The compositor core. Everything here lives on the event thread: the
 * libevent loop, the wl_display, and (in later milestones) all surface,
 * window-manager and input state. Render threads talk to it only through
 * `inbox`.
 */
struct rei_server {
	const struct rei_config *config;

	struct event_base *base;
	struct wl_display *display;
	struct wl_event_loop *wl_loop;
	char *socket_name;

	struct event *wl_event;     /* wl_event_loop fd -> libevent */
	struct event *flush_event;  /* coalesced "flush clients" request */
	struct event *sigint_event;
	struct event *sigterm_event;
	struct event *sigusr1_event;
	struct event *sigusr2_event;
	struct event *damage_event;  /* Coalesced "scene changed". */
	bool damage_pending;
	bool shutting_down;

	struct rei_msgq inbox;  /* render threads -> event thread */

	struct rei_backend *backend;
	struct wl_list outputs;
	uint32_t next_output_id;
	uint64_t scene_serial;
	int exit_code;

	/* Protocol state. */
	struct wl_list surfaces;    /* rei_surface */
	struct wl_list views;       /* rei_view, bottom to top (placeholder WM) */
	struct wl_list popups;      /* xdg popups, creation order */
	struct wl_list wl_outputs;  /* rei_wl_output, including ones being retired */
	struct rei_seat *seat;
	struct rei_input *input;

	/* Look. */
	struct rei_image *wallpaper_image;
	int wallpaper_mode;
	double cursor_x, cursor_y;  /* Global logical coordinates. */
	bool cursor_placed;
};

struct rei_server_options {
	const char *socket_name;      /* NULL: pick wayland-N automatically. */
	const char *backend_override; /* NULL: use the config's choice. */
};

struct rei_server *rei_server_create(const struct rei_config *config,
				     const struct rei_server_options *opts);

/* Runs the event loop until rei_server_stop(). Returns a process exit code. */
int rei_server_run(struct rei_server *server);

/* Safe to call from the event thread at any time, including from handlers. */
void rei_server_stop(struct rei_server *server);

/* Like rei_server_stop(), but the process will exit with status 1. */
void rei_server_fail(struct rei_server *server);

void rei_server_destroy(struct rei_server *server);

/*
 * libwayland has its own idle sources and queues events for clients, but only
 * services them when its fd is readable. Anything that changes client-visible
 * state from a non-Wayland libevent callback (input, a render-thread message, a
 * timer) must call this so the change is dispatched and flushed promptly.
 * Calls are coalesced.
 */
void rei_server_schedule_flush(struct rei_server *server);

/*
 * Marks the scene as changed. A new snapshot is built and sent to every
 * output once the current batch of events has been handled, so any number of
 * changes in one pass produce a single snapshot. Accepts NULL.
 */
void rei_server_schedule_damage(struct rei_server *server);

/* Builds and sends the snapshot now if one is scheduled. */
void rei_server_flush_damage(struct rei_server *server);

/* Asks every output to write a PPM of what it is showing to
 * $REI_SCREENSHOT_DIR (default: $XDG_RUNTIME_DIR). A debugging and testing
 * aid, also triggered by SIGUSR2. */
void rei_server_screenshot(struct rei_server *server);

struct rei_output *rei_server_add_output(struct rei_server *server,
					 const struct rei_output_params *params);
void rei_server_remove_output(struct rei_server *server, struct rei_output *output);
void rei_server_set_output_mode(struct rei_server *server, struct rei_output *output,
				int32_t width, int32_t height);
struct rei_output *rei_server_output_by_id(struct rei_server *server, uint32_t id);

#endif
