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
#define REI_BAR_MAX_TRAY 24

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

/* How the application tray in the middle of the bar looks. */
enum rei_tray_mode {
	REI_TRAY_OFF,
	REI_TRAY_ICONS,          /* icons only */
	REI_TRAY_ICONS_NAMES,    /* icon and name */
};

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
struct rei_bar_tray_item {
	char label[48];
	int count;                  /* Windows of this application. */
	bool active;                /* One of them has the focus. */
	bool minimized;             /* All of them are hidden. */
	struct rei_pixbuf *icon;    /* Borrowed from the icon cache; never NULL. */
};

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
	/* The tray: one tile per application (a stack of tiles when it has several windows). */
	enum rei_tray_mode tray_mode;
	int n_tray;
	struct rei_bar_tray_item tray[REI_BAR_MAX_TRAY];
	char volume[16];  /* "73%", "muted"; empty when there is no audio sink. */
	bool volume_muted;
	uint32_t background, foreground, highlight;  /* 0xRRGGBB */
};

/* Where the volume text sits in the bar (bar-local logical pixels), for hit-testing.
 * False when the bar shows no volume. */
bool rei_bar_volume_rect(const struct rei_bar_state *state, struct rei_rect *out);

/*
 * Where each tray tile sits (bar-local logical pixels, including the peeking edges of
 * a stack), centred in the bar between the workspace buttons and the volume/clock.
 * Writes up to REI_BAR_MAX_TRAY rectangles and returns how many tiles fit; tiles
 * that do not fit are not drawn.
 */
size_t rei_bar_tray_layout(const struct rei_bar_state *state, struct rei_rect *out);

/* ---- The window list that opens from a tray tile ---- */

#define REI_MENU_MAX_ROWS 32

struct rei_menu_row {
	char label[96];
	bool minimized;
};

struct rei_menu_state {
	int n_rows;
	struct rei_menu_row rows[REI_MENU_MAX_ROWS];
	int hover;  /* Row under the pointer, or -1. */
	int scale;
	char font[96];
	uint32_t background, foreground, highlight;
};

/* Logical size of the menu and of one row. */
void rei_bar_menu_size(const struct rei_menu_state *state, int *w, int *h, int *row_h);
struct rei_pixbuf *rei_bar_menu_render(const struct rei_menu_state *state);

/* A coloured square with the first letter of `name`, for applications with no icon. */
struct rei_pixbuf *rei_bar_letter_icon(const char *name);

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
/* Whether (x, y) is on the volume text of an output's toolbar. */
bool rei_toolbar_volume_at(struct rei_server *server, double x, double y);

/* The tray tile under (x, y): sets the output and the index into that output's tray groups. */
bool rei_toolbar_tray_at(struct rei_server *server, double x, double y, struct rei_output **output,
			 int *index);
/* A tray tile was clicked: acts on its application (see tray.h). */
void rei_toolbar_tray_click(struct rei_server *server, struct rei_output *output, int index);

void rei_toolbar_set_hover(struct rei_server *server, struct rei_output *output, int workspace);

/* Mouse wheel over a bar: steps to the next (+1) or previous (-1) workspace of
 * that output. */
void rei_toolbar_scroll(struct rei_server *server, struct rei_output *output, int direction);

#endif
