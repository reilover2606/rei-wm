#ifndef REI_XDG_SHELL_H
#define REI_XDG_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "proto/surface.h"
#include "snapshot.h"

struct rei_server;
struct rei_view;
struct rei_xdg_surface;

struct rei_xdg_toplevel {
	struct wl_resource *resource;
	struct rei_xdg_surface *xs;
	struct rei_view *view;  /* Non-NULL exactly while mapped. */
	char *title, *app_id;
	int32_t min_w, min_h, max_w, max_h;
	bool has_parent;       /* A dialog: the window manager floats it. */
	bool want_fullscreen;  /* The client asked, possibly before it had a view. */
	bool activated;

	/* What the window manager wants the client told, and what it was last told. */
	struct { int32_t w, h; bool tiled, fullscreen; } want;
	struct { bool valid; int32_t w, h; bool activated, tiled, fullscreen; } sent;
};

bool rei_xdg_shell_init(struct rei_server *server);

/* Window geometry (the part of the surface that is "the window", excluding
 * client-drawn shadows), in surface coordinates. */
struct rei_rect rei_xdg_toplevel_geometry(const struct rei_xdg_toplevel *tl);

void rei_xdg_toplevel_set_activated(struct rei_xdg_toplevel *tl, bool activated);

/*
 * Tells the client what size and state the window manager gives it. A size of
 * 0x0 leaves the choice to the client. Sent immediately if it differs from what
 * the client already has.
 */
void rei_xdg_toplevel_set_state(struct rei_xdg_toplevel *tl, int32_t width, int32_t height,
				bool tiled, bool fullscreen);
void rei_xdg_toplevel_close(struct rei_xdg_toplevel *tl);

/* The wl_surface of a toplevel; NULL if it has been destroyed. */
struct rei_surface *rei_xdg_toplevel_surface(const struct rei_xdg_toplevel *tl);

/* The window a surface belongs to, whether it is the toplevel itself, one of its
 * subsurfaces, or a popup (or a popup's subsurface). NULL if none. */
struct rei_view *rei_xdg_view_of(struct rei_surface *surface);

/* Hit-tests the window's own surfaces (not its popups: see rei_xdg_pick_popups). */
bool rei_xdg_pick_view(const struct rei_view *view, double x, double y,
		       struct rei_surface **out, double *sx, double *sy);

/* Hit-tests every visible popup, newest first. Popups sit above all windows. */
bool rei_xdg_pick_popups(struct rei_server *server, double x, double y,
			 struct rei_surface **out, double *sx, double *sy);

/* The surface of the topmost mapped popup that holds a grab (menus), or NULL.
 * Such a popup takes keyboard focus while it is open. */
struct rei_surface *rei_xdg_topmost_grab_surface(struct rei_server *server);

/*
 * A button was pressed on `clicked` (NULL for empty desktop). Tells grabbing
 * popups to close unless the click landed inside one of them; a click on a
 * popup closes only the popups opened after it.
 */
void rei_xdg_dismiss_popups(struct rei_server *server, struct rei_surface *clicked);

/* Appends the window's own surfaces to `snap`. */
void rei_xdg_emit_view(const struct rei_view *view, struct rei_scene_snapshot *snap);

/* Appends every visible popup, above all windows. */
void rei_xdg_emit_popups(struct rei_server *server, struct rei_scene_snapshot *snap);

/*
 * Where an xdg root surface (toplevel or popup) is on screen, in global
 * coordinates. False if it isn't a mapped xdg surface.
 */
bool rei_xdg_surface_rect(struct rei_surface *root, struct rei_rect *out);

/* A mapped xdg surface whose window is on a workspace that isn't showing. */
bool rei_xdg_surface_hidden(struct rei_surface *root);

#endif
