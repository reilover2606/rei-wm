#ifndef REI_POINTER_CONSTRAINTS_H
#define REI_POINTER_CONSTRAINTS_H

#include <stdbool.h>
#include <stdint.h>

struct rei_server;
struct rei_surface;

/*
 * zwp_pointer_constraints_v1 and zwp_relative_pointer_manager_v1: what games, 3D programs and Xwayland need. A program
 * that wants the mouse for itself (looking around in a game) *locks* the pointer: the cursor stops where it is and the
 * program gets relative motion instead. *Confining* keeps the cursor inside a surface. Xwayland builds XGrabPointer and
 * XWarpPointer (and so GLFW/SDL "disabled cursor" mode, Java games) out of these, and X11 raw motion out of relative
 * motion; without them the view spins as the program re-centres a cursor that cannot be moved.
 *
 * A constraint is active only while its surface has the pointer focus. input.c tells this module when the focus
 * changes and routes motion through it.
 */
bool rei_pointer_constraints_init(struct rei_server *server);

/* The pointer focus is now `surface` (or NULL), whose top-left is at (ox, oy) in global coordinates: the old one's
 * constraint stops, the new one's (if it asked for one) starts. A lock's cursor position hint is applied to the cursor
 * (server->cursor_x/y) when it stops. */
void rei_pointer_constraints_focus(struct rei_server *server, struct rei_surface *surface, double ox, double oy);

/* Whether the pointer is locked: the cursor must not move. */
bool rei_pointer_locked(const struct rei_server *server);

/* Keeps a position inside the active confinement (a surface whose top-left is at (ox, oy)), if there is one. */
void rei_pointer_confine(const struct rei_server *server, double ox, double oy, double *x, double *y);

/* Relative motion for the program with the pointer focus: dx, dy as the cursor would move, ux, uy before pointer
 * acceleration. Sent whether or not the pointer is locked. */
void rei_relative_pointer_motion(struct rei_server *server, struct rei_surface *surface, uint64_t time_usec, double dx,
				 double dy, double ux, double uy);

#endif
