#ifndef REI_TOOLBAR_H
#define REI_TOOLBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "geom.h"
#include "pixbuf.h"
#include "snapshot.h"

struct rei_server;
struct rei_output;

/*
 * The toolbar: a strip along one edge of every output, drawn by the compositor.
 * At its start (the left of a horizontal bar, the top of a vertical one) are
 * buttons for that output's workspaces; at its end is a clock.
 *
 * Whatever edge it occupies is removed from the area windows may use.
 *
 * This file has two layers. The first (geometry, state, rendering) is pure and is
 * tested on its own. The second (the rei_toolbar_* functions taking a server)
 * glues it to outputs, workspaces, the clock and input.
 */

enum rei_bar_position {
	REI_BAR_TOP,
	REI_BAR_BOTTOM,
	REI_BAR_LEFT,
	REI_BAR_RIGHT,
};

#define REI_BAR_MAX_BUTTONS 32

/* ---- Geometry (pure) ---- */

bool rei_bar_vertical(enum rei_bar_position pos);

/* The bar's rectangle along an edge of `output`, `thickness` pixels thick. */
struct rei_rect rei_bar_rect(struct rei_rect output, enum rei_bar_position pos, int thickness);

/* What is left of `output` for windows once the bar has taken its edge. */
struct rei_rect rei_bar_usable(struct rei_rect output, enum rei_bar_position pos, int thickness);

/* Button cells are squares of side `thickness`, laid end to end from the start of
 * the bar. This is the rectangle of button `index`. */
struct rei_rect rei_bar_button_rect(struct rei_rect bar, enum rei_bar_position pos, int index);

/* The button under (x, y), or -1 (including when there are only `count` buttons). */
int rei_bar_button_at(struct rei_rect bar, enum rei_bar_position pos, int count, double x, double y);

/* ---- State and rendering (pure) ---- */

enum rei_bar_button_kind {
	REI_BAR_BUTTON_HIDDEN,    /* A workspace with windows that isn't showing. */
	REI_BAR_BUTTON_VISIBLE,   /* Showing on this output, which isn't the focused one. */
	REI_BAR_BUTTON_FOCUSED,   /* Showing on the focused output. */
};

struct rei_bar_button {
	int number;
	enum rei_bar_button_kind kind;
};

/* Everything that determines how a bar looks. Compared byte for byte to decide
 * whether it needs redrawing, so it is always built from a zeroed struct. */
struct rei_bar_state {
	enum rei_bar_position position;
	int thickness;  /* Logical pixels. */
	int length;     /* Logical size along the bar (the output's width or height). */
	int scale;      /* Integer output scale. */
	int n_buttons;
	struct rei_bar_button buttons[REI_BAR_MAX_BUTTONS];
	int hover;      /* Button under the pointer, or -1. */
	char clock[64];
	char font[96];  /* Pango font description, e.g. "Sans 11". */
	uint32_t background, foreground, highlight;  /* 0xRRGGBB */
};

/* An opaque pixbuf of the bar at its physical resolution; NULL on failure. */
struct rei_pixbuf *rei_bar_render(const struct rei_bar_state *state);

/* Releases the font caches Pango, Cairo and fontconfig keep for the life of the
 * process. Call once, at exit, after the last rei_bar_render(). */
void rei_bar_render_cleanup(void);

/* Mixes two colours: t = 0 gives `a`, t = 1 gives `b`. */
uint32_t rei_bar_mix(uint32_t a, uint32_t b, double t);

/* ---- Integration with the compositor (event thread) ---- */

/* Starts the clock. */
void rei_toolbar_init(struct rei_server *server);
void rei_toolbar_finish(struct rei_server *server);
void rei_toolbar_output_removed(struct rei_output *output);

/* The part of an output windows may use: the whole output, less the toolbar. */
struct rei_rect rei_toolbar_usable(const struct rei_output *output);

/* Appends the output's bar, redrawing it first if anything about it changed. */
void rei_toolbar_emit(struct rei_server *server, struct rei_output *output,
		      struct rei_scene_snapshot *snap);

/*
 * Whether (x, y) is over some output's bar (and not under a fullscreen window).
 * `workspace` is the number of the workspace button there, or 0 if the point is
 * on the bar but not on a button.
 */
bool rei_toolbar_hit(struct rei_server *server, double x, double y,
		     struct rei_output **output, int *workspace);

/* Highlights the button for `workspace` on `output` (0 / NULL for none). */
void rei_toolbar_set_hover(struct rei_server *server, struct rei_output *output, int workspace);

/* Mouse wheel over a bar: steps to the next (+1) or previous (-1) workspace of
 * that output. */
void rei_toolbar_scroll(struct rei_server *server, struct rei_output *output, int direction);

#endif
