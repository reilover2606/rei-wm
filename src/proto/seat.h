#ifndef REI_SEAT_H
#define REI_SEAT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct rei_server;
struct rei_data_source;

/*
 * The one seat ("seat0"): protocol objects and event delivery. Which surface
 * has focus and what the events are is decided by input.c; this file just puts
 * them on the wire, to the right client.
 *
 * It advertises no capabilities until a backend reports an input source with
 * rei_seat_set_capabilities().
 */
struct rei_seat {
	struct rei_server *server;
	struct wl_global *global;
	uint32_t capabilities;       /* WL_SEAT_CAPABILITY_* currently offered. */
	uint32_t ever_capabilities;  /* Ever offered (a request is legal if so). */
	struct wl_list resources;    /* Bound wl_seat objects. */
	struct wl_list data_devices; /* wl_data_device objects, all clients. */
	struct wl_list pointers;     /* wl_pointer objects, all clients. */
	struct wl_list keyboards;    /* wl_keyboard objects, all clients. */

	/* The clipboard. Delivery to the focused client arrives with keyboard
	 * focus in milestone 4. */
	struct rei_data_source *selection;
	struct wl_listener selection_destroy;
};

bool rei_seat_init(struct rei_server *server);
void rei_seat_finish(struct rei_server *server);
void rei_seat_set_capabilities(struct rei_seat *seat, uint32_t capabilities);

struct rei_surface;

/* Pointer events go to the pointer objects of the client that owns `surface`.
 * Each call is one complete wl_pointer frame. Coordinates are surface-local. */
uint32_t rei_seat_pointer_enter(struct rei_seat *seat, struct rei_surface *surface, double sx, double sy);
void rei_seat_pointer_leave(struct rei_seat *seat, struct rei_surface *surface);
void rei_seat_pointer_motion(struct rei_seat *seat, struct rei_surface *surface,
			     uint32_t time_ms, double sx, double sy);
uint32_t rei_seat_pointer_button(struct rei_seat *seat, struct rei_surface *surface,
				 uint32_t time_ms, uint32_t button, bool pressed);
void rei_seat_pointer_axis(struct rei_seat *seat, struct rei_surface *surface,
			   uint32_t time_ms, bool horizontal, double value);

/* Keyboard events likewise. `keys` are the evdev codes held down on entry. */
void rei_seat_keyboard_enter(struct rei_seat *seat, struct rei_surface *surface,
			     const uint32_t *keys, size_t n_keys);
void rei_seat_keyboard_leave(struct rei_seat *seat, struct rei_surface *surface);
void rei_seat_keyboard_key(struct rei_seat *seat, struct rei_surface *surface,
			   uint32_t time_ms, uint32_t key, bool pressed);
/* Sends the current xkb modifier state. */
void rei_seat_keyboard_modifiers(struct rei_seat *seat, struct rei_surface *surface);

#endif
