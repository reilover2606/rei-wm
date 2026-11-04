#ifndef REI_TOOLBAR_H
#define REI_TOOLBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "geom.h"
#include "pixbuf.h"
#include "snapshot.h"
#include "theme.h"

struct rei_server;
struct rei_output;
struct rei_tray_set;
struct rei_workspace;

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

#define REI_TRAY_HOVER_OVERFLOW 1000

/* How the application tray in the middle of the bar looks. */
enum rei_tray_mode {
	REI_TRAY_OFF,
	REI_TRAY_ICONS,          /* icons only */
	REI_TRAY_ICONS_NAMES,    /* icon and name */
};

/* Whose windows the tray lists. */
enum rei_tray_scope {
	REI_TRAY_SCOPE_WORKSPACE,  /* the workspace the monitor is showing */
	REI_TRAY_SCOPE_OUTPUT,     /* every workspace on the monitor */
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
	int tray_extra_windows;  /* Windows beyond what the list holds: always under the overflow tile. */
	int tray_min_percent;    /* Tiles shrink to this share of their natural width before overflowing. */
	int tray_hover;          /* The tile under the pointer: a group index, REI_TRAY_HOVER_OVERFLOW or -1. */
	int drag_index;          /* The tile being dragged, or -1. */
	int drag_pos;            /* Where the pointer is along the bar (bar-local), while dragging. */
	bool styled;             /* `theme` is set; otherwise the legacy colours below give the default look. */
	struct rei_theme theme;
	bool battery_present;   /* The machine has a battery (the widget shows). */
	int battery_percent;    /* 0..100 */
	bool battery_charging;
	char volume[16];  /* "73%", "muted"; empty when there is no audio sink. */
	bool volume_muted;
	uint32_t background, foreground, highlight;  /* 0xRRGGBB */
};

/* Where the volume text sits in the bar (bar-local logical pixels), for hit-testing.
 * False when the bar shows no volume. */
bool rei_bar_volume_rect(const struct rei_bar_state *state, struct rei_rect *out);

/*
 * Where the tray's tiles sit (bar-local logical pixels, including the peeking edges of a
 * stack). The tray lives in the middle three fifths of the bar. Tiles shrink as it fills
 * (down to `tray_min_percent` of their natural width); past that the newest groups go
 * under an overflow tile. While a tile is dragged, the others make room for it.
 */
struct rei_tray_layout {
	struct rei_rect tile[REI_BAR_MAX_TRAY];  /* By group index; valid for index < n_visible. */
	size_t n_visible;       /* Groups 0..n_visible-1 have a tile; the rest are under the overflow tile. */
	bool has_overflow;
	struct rei_rect overflow;
	int overflow_windows;   /* Windows under it. */
	int drop_index;         /* While dragging: where the tile would land among the visible ones, else -1. */
};

void rei_bar_tray_layout(const struct rei_bar_state *state, struct rei_tray_layout *out);

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
	struct rei_style menu, row;
};

/* Logical size of the menu box and of one row, and the extra room its shadow needs around it:
 * the rendered picture is (w + 2*margin) x (h + 2*margin), with the box in the middle. */
void rei_bar_menu_size(const struct rei_menu_state *state, int *w, int *h, int *row_h, int *margin);
struct rei_pixbuf *rei_bar_menu_render(const struct rei_menu_state *state);

/* A tooltip: `text` in a box styled by `style`. Returns the picture, which includes the box's
 * shadow; the box itself is box_w x box_h with `margin` pixels of picture around it. */
struct rei_pixbuf *rei_bar_tooltip_render(const char *text, const char *font, const struct rei_style *style,
					  int scale, int *box_w, int *box_h, int *margin);

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

enum { REI_TRAY_TARGET_TILE = 1, REI_TRAY_TARGET_OVERFLOW = 2 };

/* The bar's rectangle on an output, global logical coordinates. */
struct rei_rect rei_toolbar_rect(const struct rei_server *server, const struct rei_output *output);
/*
 * Finds what the tray shows at (x, y): a tile (kind REI_TRAY_TARGET_TILE, `index` = its group's
 * index) or the overflow tile. `tile` is its rectangle in global coordinates.
 */
bool rei_toolbar_tray_target(struct rei_server *server, double x, double y, struct rei_output **output,
			     int *kind, int *index, struct rei_rect *tile);
/* How many of `set`'s groups have a tile (the rest are under the overflow tile). */
size_t rei_toolbar_tray_visible(struct rei_server *server, struct rei_output *output,
				const struct rei_tray_set *set);
/* While a tile is dragged: where it would land among the visible ones; also the visible count. */
int rei_toolbar_tray_drop_index(struct rei_server *server, struct rei_output *output, size_t *visible);
/* Windows under the overflow tile if `workspace` were the one showing on `output`. */
int rei_toolbar_overflow_windows(struct rei_server *server, struct rei_output *output,
				 const struct rei_workspace *workspace);

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
