#ifndef REI_CURSOR_SHAPE_H
#define REI_CURSOR_SHAPE_H

#include <stdbool.h>

struct rei_server;

/*
 * wp_cursor_shape_v1: a client names the pointer it wants ("text", "pointer", "ew-resize"...) instead of
 * drawing it, and the compositor shows that shape of the current theme. Toolkits that support it (Qt,
 * GTK4, winit, ...) use it in preference to loading a theme themselves.
 */
bool rei_cursor_shape_init(struct rei_server *server);

#endif
