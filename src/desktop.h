#ifndef REI_DESKTOP_H
#define REI_DESKTOP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

struct rei_server;
struct rei_xdg_toplevel;

/*
 * A window on the desktop.
 *
 * PLACEHOLDER: until the real window manager (milestone 5) arrives, windows
 * are centred on the first output, cascaded 30px per window, with the most
 * recently mapped one on top and activated. There is no tiling, no
 * workspaces and no focus handling yet.
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

#endif
