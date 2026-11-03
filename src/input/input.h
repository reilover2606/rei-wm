#ifndef REI_INPUT_H
#define REI_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "decoration.h"
#include "geom.h"

struct rei_server;
struct rei_surface;
struct rei_view;
struct rei_output;
struct rei_xkb;
struct rei_bindings;

#define REI_MAX_PRESSED 64
#define REI_MAX_BUTTONS 16

/*
 * Input state and routing. Backends feed raw events in through the rei_input_*
 * functions below (evdev keycodes and button codes, so a libinput backend can
 * pass events straight through); this decides what each one means:
 *
 *   keys     -> keybindings first, otherwise the keyboard-focused client
 *   pointer  -> moves the cursor, hit-tests surfaces, drives focus and grabs
 *   buttons  -> click-to-focus, popup dismissal, then the client
 *
 * Everything runs on the event thread.
 */
struct rei_input {
	struct rei_server *server;
	struct rei_xkb *xkb;
	struct rei_bindings *bindings;

	uint32_t pressed[REI_MAX_PRESSED];  /* Keys held down (evdev codes). */
	size_t n_pressed;
	uint32_t swallowed[REI_MAX_PRESSED];  /* Held keys a binding consumed. */
	size_t n_swallowed;

	struct rei_surface *keyboard_focus;
	struct wl_listener keyboard_focus_destroy;

	struct rei_surface *pointer_focus;
	struct wl_listener pointer_focus_destroy;
	uint32_t buttons[REI_MAX_BUTTONS];  /* Pointer buttons held down. */
	size_t n_buttons;

	/* While any button is held, the pointer stays "in" the surface it was
	 * pressed on, so drags and selections keep working outside its edges. */
	struct rei_surface *grab_surface;
	struct wl_listener grab_destroy;
	double grab_origin_x, grab_origin_y;  /* Global position of that surface's top-left. */

	uint32_t last_button_serial;

	/* A toolbar button pressed and not yet released. */
	bool bar_press;
	int bar_press_ws;  /* The workspace button pressed (0: the bar itself). */
	bool bar_press_volume;  /* The press was on the volume text. */
	struct rei_output *bar_press_output;

	/* A notch button pressed and not yet released (the click is the release). */
	enum rei_notch_part notch_press;
	struct rei_view *notch_view;
	double drag_start_x, drag_start_y;  /* Where a handle drag began, to tell a drag from a click. */

	/* Interactive move and resize of a floating window. */
	struct rei_view *move_view;
	double move_cursor_x, move_cursor_y;
	int32_t move_view_x, move_view_y;
	struct rei_view *resize_view;
	uint32_t resize_edges;  /* xdg_toplevel resize edge bits */
	struct rei_rect resize_start;
};

/* Config hot reload, in two steps so that a bad config changes nothing. */
struct rei_input_reload {
	struct rei_bindings *bindings;
	struct rei_xkb *xkb;  /* NULL unless the keyboard layout changed. */
	bool keymap_changed;
};
struct rei_config;
bool rei_input_prepare_reload(struct rei_server *server, const struct rei_config *cfg,
			      struct rei_input_reload *out, char *err, size_t errlen);
void rei_input_commit_reload(struct rei_server *server, struct rei_input_reload *r);

/* The server's [input]/[keybindings] config must be loaded. */
bool rei_input_init(struct rei_server *server, char *err, size_t errlen);
void rei_input_finish(struct rei_server *server);

/* ---- Raw events from backends. Times are milliseconds. ---- */
void rei_input_key(struct rei_server *server, uint32_t time_ms, uint32_t evdev_code, bool pressed);
void rei_input_pointer_motion(struct rei_server *server, uint32_t time_ms, double dx, double dy);
/* Position in global layout coordinates. */
void rei_input_pointer_motion_abs(struct rei_server *server, uint32_t time_ms, double x, double y);
void rei_input_pointer_button(struct rei_server *server, uint32_t time_ms, uint32_t button, bool pressed);
void rei_input_pointer_axis(struct rei_server *server, uint32_t time_ms, bool horizontal, double value);

/* Releases every held key and button, as if the user let go. A backend calls this
 * when it loses the input focus (e.g. the nested window is deactivated), so no
 * key stays "stuck down". */
void rei_input_release_all(struct rei_server *server, uint32_t time_ms);

/* ---- Called by the rest of the compositor. ---- */

/* Re-evaluates which surface should have keyboard focus: the topmost popup
 * holding a grab, else the active window. Cheap; call after any change. */
void rei_input_refocus(struct rei_server *server);

/* Keeps the cursor on a screen after outputs change. */
void rei_input_clamp_cursor(struct rei_server *server);

/* Starts moving `view` with the pointer, if the button press `serial` is the
 * latest one and a button is held. Returns false (and does nothing) otherwise. */
bool rei_input_begin_move(struct rei_server *server, struct rei_view *view, uint32_t serial);

/* Likewise for resizing from the given xdg_toplevel resize edges. */
bool rei_input_begin_resize(struct rei_server *server, struct rei_view *view, uint32_t serial,
			    uint32_t edges);

#endif
