#ifndef REI_XWAYLAND_H
#define REI_XWAYLAND_H

#include <stdbool.h>
#include <stdint.h>

#include "geom.h"
#include "snapshot.h"

struct rei_server;
struct rei_surface;
struct rei_view;
struct rei_xwindow;

/*
 * Xwayland support: rei-wm runs Xwayland (the X server that draws into Wayland
 * surfaces) and acts as its window manager over an X11 connection. Each X11
 * toplevel becomes a window in the same tiling/floating tree as native ones.
 *
 * X11 coordinates are the compositor's global layout coordinates (one pixel is
 * one pixel; no scaling is applied to X11 windows yet).
 *
 * Everything here runs on the event thread. If Xwayland is missing or fails to
 * start, rei-wm carries on without it.
 */

/* Starts Xwayland when the config allows it and the binary exists. */
void rei_xwayland_start(struct rei_server *server);
void rei_xwayland_stop(struct rei_server *server);

/* "DISPLAY" value for programs we launch (":1"), or NULL while it is not running. */
const char *rei_xwayland_display_name(const struct rei_server *server);

/* ---- The xwayland-shell protocol global (src/proto/xwayland_shell.c) ---- */
bool rei_xwayland_shell_init(struct rei_server *server);

/* ---- Called by the window manager for views that are X11 windows ---- */
struct rei_rect rei_xwindow_geometry(const struct rei_xwindow *xw);
/* Moves and resizes the window (global coordinates). */
void rei_xwindow_configure(struct rei_xwindow *xw, int32_t x, int32_t y, int32_t w, int32_t h,
			   bool fullscreen);
void rei_xwindow_set_activated(struct rei_xwindow *xw, bool activated);
void rei_xwindow_close(struct rei_xwindow *xw);
struct rei_surface *rei_xwindow_surface(const struct rei_xwindow *xw);
const char *rei_xwindow_title(const struct rei_xwindow *xw);
/* WM_CLASS's class part, the X11 counterpart of an app id. */
const char *rei_xwindow_class(const struct rei_xwindow *xw);
bool rei_xwindow_wants_float(const struct rei_xwindow *xw);
bool rei_xwindow_wants_fullscreen(const struct rei_xwindow *xw);
void rei_xwindow_set_fullscreen(struct rei_xwindow *xw, bool on);
void rei_xwindow_emit(const struct rei_view *view, struct rei_scene_snapshot *snap);
bool rei_xwindow_pick(const struct rei_view *view, double x, double y, struct rei_surface **out,
		      double *sx, double *sy);

/* The managed window a surface belongs to, or NULL. */
struct rei_view *rei_xwayland_view_of(struct rei_surface *surface);
/* Where an X11 root surface is on screen / whether its window is on a hidden workspace. */
bool rei_xwayland_surface_rect(struct rei_surface *root, struct rei_rect *out);
bool rei_xwayland_surface_hidden(struct rei_surface *root);

/* Menus, tooltips and other override-redirect windows: not managed, drawn at the
 * position X11 gave them, above all windows. */
void rei_xwayland_emit_unmanaged(struct rei_server *server, struct rei_scene_snapshot *snap);
bool rei_xwayland_pick_unmanaged(struct rei_server *server, double x, double y,
				 struct rei_surface **out, double *sx, double *sy);

/* ---- Surface association, used by the protocol file ---- */
struct rei_xsurface;
struct rei_xsurface *rei_xsurface_create(struct rei_server *server, struct rei_surface *surface);
void rei_xsurface_set_serial(struct rei_xsurface *xs, uint64_t serial);
void rei_xsurface_destroy(struct rei_xsurface *xs);

#endif
