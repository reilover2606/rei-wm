#ifndef REI_DESKTOP_H
#define REI_DESKTOP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

struct rei_server;
struct rei_surface;
struct rei_xdg_toplevel;

/*
 * A window on the desktop.
 *
 * PLACEHOLDER: until the real window manager (milestone 5) arrives, windows
 * are centred on the first output, cascaded 30px per window, with the most
 * recently activated one on top. The top window is the active one and holds
 * keyboard focus. There is no tiling and no workspaces yet.
 */
struct rei_view {
	struct wl_list link;  /* rei_server.views, bottom to top */
	struct rei_server *server;
	struct rei_xdg_toplevel *toplevel;
	int32_t x, y;  /* Global position of the window geometry's top-left. */
};

struct rei_view *rei_view_create(struct rei_server *server, struct rei_xdg_toplevel *toplevel);

/* Places the window, stacks it on top and activates it. */
void rei_view_map(struct rei_view *view);

void rei_view_destroy(struct rei_view *view);

/* Raises the window to the top and makes it the active, keyboard-focused one. */
void rei_view_activate(struct rei_view *view);

void rei_view_move_to(struct rei_view *view, int32_t x, int32_t y);

/* The active (topmost) window, or NULL. */
struct rei_view *rei_desktop_top_view(struct rei_server *server);

/* Alt-Tab: next brings the bottom window to the front; !next sends the front
 * one to the back. */
void rei_desktop_cycle(struct rei_server *server, bool next);

/* The window a surface (or one of its subsurfaces or popups) belongs to. */
struct rei_view *rei_desktop_view_for_surface(struct rei_server *server, struct rei_surface *surface);

/* The surface that should have keyboard focus absent a popup grab. */
struct rei_surface *rei_desktop_keyboard_surface(struct rei_server *server);

/* The topmost surface accepting input at global (x, y), with the point in that
 * surface's coordinates. */
bool rei_desktop_pick(struct rei_server *server, double x, double y,
		      struct rei_surface **surface, double *sx, double *sy);

#endif
