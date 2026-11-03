#ifndef REI_DECORATION_H
#define REI_DECORATION_H

#include <stdbool.h>

#include "geom.h"
#include "pixbuf.h"

/*
 * Window notches.
 *
 * Windows have no border or title bar. The focused window gets two small tabs
 * that hover directly above its top edge, drawn by the compositor:
 *
 *      [ ⠿ ]                        [ – ][ □ ][ × ]      <- the notches
 *      ┌────────────────────────────────────────┐
 *      │                 window                 │
 *
 * Left: a drag handle (press and hold to move the window). Right: minimize,
 * maximize and close. They overlay the area above the window and never take
 * space from it, so showing or hiding them cannot resize anything.
 *
 * This file is geometry, hit-testing and artwork only; it knows nothing about
 * windows or Wayland, which keeps it simple to test.
 */

#define REI_NOTCH_BUTTON_W 28
#define REI_NOTCH_BUTTONS_W (3 * REI_NOTCH_BUTTON_W)
#define REI_NOTCH_HANDLE_W 48
#define REI_NOTCH_MAX_HEIGHT 64

enum rei_notch_part {
	REI_NOTCH_NONE = 0,
	REI_NOTCH_MINIMIZE,
	REI_NOTCH_MAXIMIZE,
	REI_NOTCH_CLOSE,
	REI_NOTCH_HANDLE,
};

struct rei_notch_geometry {
	bool has_handle, has_buttons;
	struct rei_rect handle;    /* The whole drag-handle tab. */
	struct rei_rect buttons;   /* The whole button tab. */
	struct rei_rect minimize, maximize, close;  /* Cells within `buttons`. */
};

/*
 * Where the notches go for a window occupying `window`, with tabs `height`
 * pixels tall. Both tabs end exactly at the window's top edge. The buttons take
 * priority; a window too narrow for both drops the handle, and one too narrow
 * for either gets no tabs. Tabs never extend past the window's sides or overlap
 * each other.
 */
void rei_notch_geometry(struct rei_rect window, int height, struct rei_notch_geometry *out);

/* Which part of the notches the point (x, y) is over. */
enum rei_notch_part rei_notch_hit(const struct rei_notch_geometry *g, double x, double y);

/*
 * Artwork, in logical size `w` x `height` drawn at integer `scale` (so the
 * pixbuf is larger on high-density outputs and stays sharp). Pixbufs are cached
 * and shared: callers must not unref or modify them. Event thread only.
 *
 * `hover` is the part under the pointer, to highlight it (REI_NOTCH_NONE for
 * none). For the button tab it may be minimize, maximize or close; for the
 * handle, only REI_NOTCH_HANDLE counts.
 */
struct rei_pixbuf *rei_notch_art_buttons(int height, int scale, enum rei_notch_part hover);
struct rei_pixbuf *rei_notch_art_handle(int height, int scale, enum rei_notch_part hover);

/* A translucent rectangle, stretched by the renderer: the highlight showing
 * where a dragged window will land. */
struct rei_pixbuf *rei_drop_hint_art(void);

#endif
