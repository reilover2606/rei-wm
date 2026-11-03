#ifndef REI_DESKTOP_H
#define REI_DESKTOP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

#include "geom.h"
#include "layout.h"
#include "snapshot.h"

struct rei_server;
struct rei_output;
struct rei_surface;
struct rei_xdg_toplevel;

/*
 * The window manager.
 *
 * Every output shows one workspace at a time. A workspace has a number (global,
 * like i3: "workspace 3" is the same workspace wherever you are), belongs to one
 * output, and holds that output's windows in two groups: a tiling tree of
 * windows laid out in the grid with gaps, and a stack of floating windows drawn
 * above it. One window may be fullscreen.
 *
 * A window ("view") is created when its client first commits (so its first
 * configure can already carry the right size) and is destroyed when it unmaps.
 * It exists, and holds a slot, before it is mapped (has content).
 *
 * Everything here runs on the event thread.
 */

struct rei_workspace {
	struct wl_list link;         /* rei_server.workspaces, sorted by number */
	int number;
	struct rei_output *output;   /* NULL only while there are no outputs */
	struct rei_tree *tree;       /* tiled windows (leaves hold rei_view) */
	struct wl_list floating;     /* rei_view.float_link, bottom to top */
	struct wl_list focus;        /* rei_view.focus_link, most recently focused first */
	struct rei_view *fullscreen;
};

struct rei_view {
	struct wl_list link;         /* rei_server.views: every window */
	struct rei_server *server;
	struct rei_xdg_toplevel *toplevel;

	struct rei_workspace *ws;    /* NULL if no output existed to put it on */
	struct rei_node *node;       /* Its leaf in ws->tree when tiled. */
	struct wl_list float_link;
	struct wl_list focus_link;
	bool mapped;
	bool floating;
	bool fullscreen;

	/* Floating geometry, relative to the workspace's output so that windows
	 * follow their output when the layout changes. w/h are only used once the
	 * user has resized it (float_sized); before that the client picks its size. */
	struct rei_rect float_rect;
	bool float_sized;

	/* Where it currently is, in global coordinates. w/h of 0 means "the
	 * client's own size". */
	int32_t x, y, w, h;
};

void rei_wm_init(struct rei_server *server);
void rei_wm_finish(struct rei_server *server);

/* ---- Outputs ---- */
void rei_wm_output_added(struct rei_server *server, struct rei_output *output);
void rei_wm_output_removed(struct rei_server *server, struct rei_output *output);
/* An output's size or position changed. */
void rei_wm_arrange_all(struct rei_server *server);

/* ---- Window lifecycle, driven by the xdg-shell code ---- */
struct rei_view *rei_view_create(struct rei_server *server, struct rei_xdg_toplevel *toplevel);
/* The client's first commit: finds the window a slot and sends its first size. */
void rei_wm_add_view(struct rei_view *view);
/* The window now has content: show it and give it the focus. */
void rei_view_map(struct rei_view *view);
void rei_view_destroy(struct rei_view *view);

/* ---- Focus ---- */
struct rei_view *rei_wm_active_view(struct rei_server *server);
void rei_wm_focus_view(struct rei_view *view);
/* Makes the output under (x, y) the focused one. */
void rei_wm_focus_output_at(struct rei_server *server, double x, double y);
bool rei_view_visible(const struct rei_view *view);

/* ---- Commands (keybindings) ---- */
void rei_wm_focus_direction(struct rei_server *server, enum rei_direction dir);
void rei_wm_move_direction(struct rei_server *server, enum rei_direction dir);
void rei_wm_cycle(struct rei_server *server, bool next);
void rei_wm_split(struct rei_server *server, enum rei_layout layout);
void rei_wm_toggle_floating(struct rei_server *server);
void rei_wm_set_fullscreen(struct rei_view *view, bool fullscreen);
void rei_wm_toggle_fullscreen(struct rei_server *server);
void rei_wm_goto_workspace(struct rei_server *server, int number);
void rei_wm_move_to_workspace(struct rei_server *server, int number);

/* ---- Floating windows (mouse) ---- */
/* The window's rectangle as it is on screen right now. */
struct rei_rect rei_view_rect(const struct rei_view *view);
void rei_wm_float_move(struct rei_view *view, int32_t x, int32_t y);
void rei_wm_float_set_rect(struct rei_view *view, int32_t x, int32_t y, int32_t w, int32_t h);
/* After a drag: a window dropped on another output belongs to that output. */
void rei_wm_float_settle(struct rei_view *view);

/* ---- Scene and hit-testing ---- */
void rei_wm_emit_views(struct rei_server *server, struct rei_output *output,
		       struct rei_scene_snapshot *snap);
bool rei_wm_pick(struct rei_server *server, double x, double y,
		 struct rei_surface **surface, double *sx, double *sy);
struct rei_surface *rei_wm_keyboard_surface(struct rei_server *server);
struct rei_view *rei_wm_view_for_surface(struct rei_server *server, struct rei_surface *surface);

#endif
