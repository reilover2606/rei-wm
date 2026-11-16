#ifndef REI_CURSOR_H
#define REI_CURSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixbuf.h"

/*
 * Pointer images: XCursor themes (the files under <theme>/cursors/, as used by X11 and by
 * libwayland-cursor), found in the usual icon directories and in /usr/share/cursors, plus a built-in
 * arrow for when no theme can be found at all.
 *
 * The shapes are the ones of the wp_cursor_shape_v1 protocol (the same numbers), so a client's named
 * shape maps straight onto one; the compositor uses them for its own widgets too.
 */
enum rei_cursor_shape {
	REI_SHAPE_NONE = 0,
	REI_SHAPE_DEFAULT = 1,
	REI_SHAPE_CONTEXT_MENU,
	REI_SHAPE_HELP,
	REI_SHAPE_POINTER,
	REI_SHAPE_PROGRESS,
	REI_SHAPE_WAIT,
	REI_SHAPE_CELL,
	REI_SHAPE_CROSSHAIR,
	REI_SHAPE_TEXT,
	REI_SHAPE_VERTICAL_TEXT,
	REI_SHAPE_ALIAS,
	REI_SHAPE_COPY,
	REI_SHAPE_MOVE,
	REI_SHAPE_NO_DROP,
	REI_SHAPE_NOT_ALLOWED,
	REI_SHAPE_GRAB,
	REI_SHAPE_GRABBING,
	REI_SHAPE_E_RESIZE,
	REI_SHAPE_N_RESIZE,
	REI_SHAPE_NE_RESIZE,
	REI_SHAPE_NW_RESIZE,
	REI_SHAPE_S_RESIZE,
	REI_SHAPE_SE_RESIZE,
	REI_SHAPE_SW_RESIZE,
	REI_SHAPE_W_RESIZE,
	REI_SHAPE_EW_RESIZE,
	REI_SHAPE_NS_RESIZE,
	REI_SHAPE_NESW_RESIZE,
	REI_SHAPE_NWSE_RESIZE,
	REI_SHAPE_COL_RESIZE,
	REI_SHAPE_ROW_RESIZE,
	REI_SHAPE_ALL_SCROLL,
	REI_SHAPE_ZOOM_IN,
	REI_SHAPE_ZOOM_OUT,
	REI_SHAPE_DND_ASK,
	REI_SHAPE_ALL_RESIZE,
	REI_SHAPE_COUNT,
};

struct rei_cursor_image {
	struct rei_pixbuf *pixbuf;   /* Premultiplied; owned by whoever made the image. */
	int32_t width, height;       /* Logical size (the pixbuf may be larger on a scaled output). */
	int32_t hotspot_x, hotspot_y;   /* Logical, from the image's top-left. */
	/* An animated cursor is a ring of these: `next` is the following frame (NULL when there is only one) and
	 * `delay_ms` how long this one is shown. The theme owns the ring. */
	const struct rei_cursor_image *next;
	uint32_t delay_ms;
};

/* The name of a shape as the protocol and CSS spell it ("pointer", "ew-resize"...), or NULL. */
const char *rei_cursor_shape_name(enum rei_cursor_shape shape);
enum rei_cursor_shape rei_cursor_shape_from_name(const char *name);

/*
 * Parses an XCursor file (the Xcur format) and returns, as a new pixbuf, the first image of the
 * nominal size closest to `want_size`. `*nominal` is that size. NULL if the data is not valid.
 */
struct rei_pixbuf *rei_xcursor_parse(const uint8_t *data, size_t len, int want_size, int *nominal,
				     int32_t *hot_x, int32_t *hot_y);

/* Like rei_xcursor_parse, but every image of that size, in order (the frames of an animation, up to `max`),
 * with each one's delay in milliseconds. Returns how many; the caller unrefs the pixbufs. */
int rei_xcursor_parse_frames(const uint8_t *data, size_t len, int want_size, int *nominal, struct rei_pixbuf **pix,
			     int32_t *hot_x, int32_t *hot_y, uint32_t *delay_ms, int max);

/* The directories searched for themes: $XCURSOR_PATH, else ~/.local/share/icons:~/.icons:
 * /usr/share/icons:/usr/share/pixmaps:/usr/share/cursors:/usr/share/cursors/xorg-x11. The result is
 * a colon-separated string (the caller frees it). */
char *rei_cursor_search_path(void);
/* The same, with every user-set variable ignored (what is exported to launched programs). */
char *rei_cursor_default_path(void);

/* The installed cursor themes' names, sorted, without duplicates. Free with rei_cursor_free_names. */
char **rei_cursor_list_themes(size_t *count);
void rei_cursor_free_names(char **names, size_t count);

struct rei_cursor_theme;

/*
 * Opens a theme. `name` may be empty: then $XCURSOR_THEME, "default", "Adwaita", and then whatever is
 * installed are tried in turn. `size` is the pointer's size in logical pixels and `pixel_scale` the
 * output scale it should be sharp at (the pictures are picked for size * pixel_scale).
 * Never fails: with no theme at all every shape is the built-in arrow.
 */
struct rei_cursor_theme *rei_cursor_theme_open(const char *name, int size, int pixel_scale);
void rei_cursor_theme_close(struct rei_cursor_theme *t);
/* The theme actually in use ("" for the built-in arrow). */
const char *rei_cursor_theme_name(const struct rei_cursor_theme *t);
/* The image for a shape. Never NULL; a shape the theme lacks falls back to its default arrow.
 * Valid until the theme is closed. */
const struct rei_cursor_image *rei_cursor_theme_shape(struct rei_cursor_theme *t, enum rei_cursor_shape shape);

#endif
