#ifndef REI_POINTER_CURSOR_H
#define REI_POINTER_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "cursor.h"

struct rei_server;
struct rei_surface;
struct wl_client;

/*
 * What the pointer looks like. Whoever knows best decides, in this order:
 *   1. the compositor, over its own things (a button, a text box, a window being moved or resized);
 *   2. the client under the pointer, if it asked for a named shape (wp_cursor_shape_v1), gave a picture
 *      (wl_pointer.set_cursor with a surface: Xwayland, GTK, ...) or asked to hide it;
 *   3. the theme's ordinary arrow.
 * A client's request lasts until the pointer enters another surface.
 */
bool rei_pointer_cursor_init(struct rei_server *server);
void rei_pointer_cursor_finish(struct rei_server *server);

/* (Re)reads [cursor] theme and size and the outputs' scales, and exports XCURSOR_THEME, XCURSOR_SIZE
 * and XCURSOR_PATH so the programs rei-wm starts (and Xwayland) use the same theme. */
void rei_pointer_cursor_reconfigure(struct rei_server *server);

/* The image to draw now; NULL when the pointer is hidden. Valid until the next call that can change it. */
const struct rei_cursor_image *rei_pointer_cursor_image(struct rei_server *server);

/* Re-decides what to show (cheap) and redraws if it changed. */
/* The picture of the plain arrow in the current theme (valid until the theme changes), or NULL. */
const struct rei_cursor_image *rei_pointer_cursor_default_image(struct rei_server *server);
void rei_pointer_cursor_refresh(struct rei_server *server);

/* Client requests. They only count while that client has the pointer. */
void rei_pointer_cursor_request_shape(struct rei_server *server, struct wl_client *client, enum rei_cursor_shape shape);
/* `surface` NULL hides the pointer. */
void rei_pointer_cursor_request_surface(struct rei_server *server, struct wl_client *client,
					struct rei_surface *surface, int32_t hot_x, int32_t hot_y);
/* The pointer entered another surface (or none): the previous client's request ends. */
void rei_pointer_cursor_focus_changed(struct rei_server *server);

#endif
