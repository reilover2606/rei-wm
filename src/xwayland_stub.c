#include "xwayland.h"

#include "desktop.h"
#include "proto/surface.h"
#include "server.h"

/* Built when libxcb is missing: X11 windows never exist, so these are inert. */

void rei_xwayland_start(struct rei_server *server) { (void)server; }
void rei_xwayland_stop(struct rei_server *server) { (void)server; }
const char *rei_xwayland_display_name(const struct rei_server *server) { (void)server; return NULL; }
struct rei_rect rei_xwindow_geometry(const struct rei_xwindow *xw) { (void)xw; return (struct rei_rect){ 0 }; }
void rei_xwindow_configure(struct rei_xwindow *xw, int32_t x, int32_t y, int32_t w, int32_t h, bool f)
{ (void)xw; (void)x; (void)y; (void)w; (void)h; (void)f; }
void rei_xwindow_set_activated(struct rei_xwindow *xw, bool a) { (void)xw; (void)a; }
void rei_xwindow_close(struct rei_xwindow *xw) { (void)xw; }
struct rei_surface *rei_xwindow_surface(const struct rei_xwindow *xw) { (void)xw; return NULL; }
bool rei_xwindow_wants_float(const struct rei_xwindow *xw) { (void)xw; return false; }
bool rei_xwindow_wants_fullscreen(const struct rei_xwindow *xw) { (void)xw; return false; }
void rei_xwindow_set_fullscreen(struct rei_xwindow *xw, bool on) { (void)xw; (void)on; }
void rei_xwindow_emit(const struct rei_view *v, struct rei_scene_snapshot *s) { (void)v; (void)s; }
bool rei_xwindow_pick(const struct rei_view *v, double x, double y, struct rei_surface **o, double *sx, double *sy)
{ (void)v; (void)x; (void)y; (void)o; (void)sx; (void)sy; return false; }
struct rei_view *rei_xwayland_view_of(struct rei_surface *s) { (void)s; return NULL; }
bool rei_xwayland_surface_rect(struct rei_surface *r, struct rei_rect *o) { (void)r; (void)o; return false; }
bool rei_xwayland_surface_hidden(struct rei_surface *r) { (void)r; return false; }
void rei_xwayland_emit_unmanaged(struct rei_server *s, struct rei_scene_snapshot *n) { (void)s; (void)n; }
bool rei_xwayland_pick_unmanaged(struct rei_server *s, double x, double y, struct rei_surface **o, double *sx, double *sy)
{ (void)s; (void)x; (void)y; (void)o; (void)sx; (void)sy; return false; }
const char *rei_xwindow_title(const struct rei_xwindow *xw) { (void)xw; return NULL; }
const char *rei_xwindow_class(const struct rei_xwindow *xw) { (void)xw; return NULL; }
