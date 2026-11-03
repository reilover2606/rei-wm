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
	bool activated;
};

bool rei_xdg_shell_init(struct rei_server *server);

/* Window geometry (the part of the surface that is "the window", excluding
 * client-drawn shadows), in surface coordinates. */
struct rei_rect rei_xdg_toplevel_geometry(const struct rei_xdg_toplevel *tl);

void rei_xdg_toplevel_set_activated(struct rei_xdg_toplevel *tl, bool activated);
void rei_xdg_toplevel_close(struct rei_xdg_toplevel *tl);

/* Appends the view's surfaces, then its popups, to `snap`. */
void rei_xdg_emit_view(const struct rei_view *view, struct rei_scene_snapshot *snap);

/*
 * Where an xdg root surface (toplevel or popup) is on screen, in global
 * coordinates. False if it isn't a mapped xdg surface.
 */
bool rei_xdg_surface_rect(struct rei_surface *root, struct rei_rect *out);

#endif
