#ifndef REI_CURSOR_H
#define REI_CURSOR_H

#include "pixbuf.h"

/*
 * rei-wm draws the one and only pointer image, for every client (native
 * Wayland and X11 alike); clients' own cursor requests are ignored. For now
 * it is the classic arrow; themes and animation come later.
 */
struct rei_cursor_image {
	struct rei_pixbuf *pixbuf;
	int32_t hotspot_x, hotspot_y;
};

/* Returns a shared image; the caller must not unref the pixbuf. */
const struct rei_cursor_image *rei_cursor_default(void);

#endif
