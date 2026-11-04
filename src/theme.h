#ifndef REI_THEME_H
#define REI_THEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct toml_table;

/*
 * The look of the compositor's own UI, described once and applied everywhere.
 *
 * Every UI element (a toolbar tile, a tooltip, a menu row, ...) has a *style*: a
 * handful of properties that mean the same thing for all of them. A theme is a style per
 * element. The defaults are derived from the three base colours in `[toolbar]`
 * (background, foreground, highlight), so a config with no `[theme]` looks as it always
 * did; `[theme.<element>]` tables override individual properties.
 *
 * Which colour is used when:
 *   color          the normal state
 *   active_color   active / highlighted (the focused window, the open workspace, a hovered row)
 *   inactive_color dimmed (minimized windows, a workspace shown on another monitor)
 * and the same three for text (text_color, active_text_color, inactive_text_color).
 *
 * Not every property applies to every element (a tooltip has no "active" state); unused ones are
 * ignored. See docs/THEME.md for which element uses what.
 */

enum rei_shape {
	REI_SHAPE_SQUARE,   /* square corners */
	REI_SHAPE_ROUNDED,  /* corners of `radius` pixels */
	REI_SHAPE_PILL,     /* fully rounded ends */
	REI_SHAPE_BEVEL,    /* corners cut off at 45 degrees, `radius` pixels deep */
};

struct rei_shadow {
	uint32_t color;   /* 0xRRGGBB */
	float opacity;    /* 0 = no shadow */
	float dx, dy;     /* offset, pixels */
	float blur;       /* softness, pixels */
};

struct rei_style {
	uint32_t color, active_color, inactive_color;                  /* 0xRRGGBB */
	uint32_t text_color, active_text_color, inactive_text_color;
	uint32_t border_color;
	float border_width;   /* pixels; 0 = none */
	enum rei_shape shape;
	float radius;         /* pixels (rounded, bevel) */
	float opacity;        /* 0..1: how opaque the element's fill is (transparency) */
	struct rei_shadow shadow;
};

enum rei_element {
	REI_ELEM_TOOLBAR,           /* the bar's own background */
	REI_ELEM_WORKSPACE_BUTTON,  /* a workspace number */
	REI_ELEM_TRAY_TILE,         /* an application in the tray */
	REI_ELEM_TRAY_OVERFLOW,     /* the tile that holds what no longer fits */
	REI_ELEM_TOOLTIP,
	REI_ELEM_MENU,              /* a pop-up list (the tray's window list) */
	REI_ELEM_MENU_ROW,
	REI_ELEM_BATTERY,           /* the battery icon: color (level), active_color (charging), inactive_color (low) */
	REI_ELEM_COUNT,
};

struct rei_theme {
	struct rei_style e[REI_ELEM_COUNT];
};

const char *rei_element_name(enum rei_element e);   /* "tray_tile", ... */
const char *rei_shape_name(enum rei_shape s);
int rei_shape_parse(const char *name);              /* -1 if unknown */

/* Mixes two 0xRRGGBB colours: t = 0 gives a, t = 1 gives b. */
uint32_t rei_color_mix(uint32_t a, uint32_t b, double t);

/* Fills `t` with the built-in look for the given base colours. */
void rei_theme_defaults(struct rei_theme *t, uint32_t background, uint32_t foreground,
			uint32_t highlight);

/*
 * Applies the `[theme.<element>]` tables of a parsed config on top of `t`. Returns false
 * with a message if a value is invalid or an unknown element or key is named (typos are
 * errors, not silently ignored).
 */
bool rei_theme_read(struct rei_theme *t, const struct toml_table *root, char *err, size_t errlen);

#endif
