#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "toolbar.h"
#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* Geometry                                                                 */
/* ------------------------------------------------------------------------ */

bool rei_bar_vertical(enum rei_bar_position pos)
{
	return pos == REI_BAR_LEFT || pos == REI_BAR_RIGHT;
}

struct rei_rect rei_bar_rect(struct rei_rect o, enum rei_bar_position pos, int t)
{
	switch (pos) {
	case REI_BAR_TOP: return (struct rei_rect){ o.x, o.y, o.w, t };
	case REI_BAR_BOTTOM: return (struct rei_rect){ o.x, o.y + o.h - t, o.w, t };
	case REI_BAR_LEFT: return (struct rei_rect){ o.x, o.y, t, o.h };
	default: return (struct rei_rect){ o.x + o.w - t, o.y, t, o.h };
	}
}

struct rei_rect rei_bar_usable(struct rei_rect o, enum rei_bar_position pos, int t)
{
	switch (pos) {
	case REI_BAR_TOP: return (struct rei_rect){ o.x, o.y + t, o.w, o.h - t };
	case REI_BAR_BOTTOM: return (struct rei_rect){ o.x, o.y, o.w, o.h - t };
	case REI_BAR_LEFT: return (struct rei_rect){ o.x + t, o.y, o.w - t, o.h };
	default: return (struct rei_rect){ o.x, o.y, o.w - t, o.h };
	}
}

struct rei_rect rei_bar_button_rect(struct rei_rect bar, enum rei_bar_position pos, int index)
{
	if (rei_bar_vertical(pos))
		return (struct rei_rect){ bar.x, bar.y + index * bar.w, bar.w, bar.w };
	return (struct rei_rect){ bar.x + index * bar.h, bar.y, bar.h, bar.h };
}

int rei_bar_button_at(struct rei_rect bar, enum rei_bar_position pos, int count, double x, double y)
{
	if (x < bar.x || y < bar.y || x >= bar.x + bar.w || y >= bar.y + bar.h)
		return -1;
	int cell = rei_bar_vertical(pos) ? bar.w : bar.h;
	if (cell <= 0)
		return -1;
	double along = rei_bar_vertical(pos) ? y - bar.y : x - bar.x;
	int index = (int)(along / cell);
	return index >= 0 && index < count ? index : -1;
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                */
/* ------------------------------------------------------------------------ */

uint32_t rei_bar_mix(uint32_t a, uint32_t b, double t)
{
	uint32_t out = 0;
	for (int shift = 16; shift >= 0; shift -= 8) {
		double ca = (double)((a >> shift) & 0xff), cb = (double)((b >> shift) & 0xff);
		out |= (uint32_t)lround(ca + (cb - ca) * t) << shift;
	}
	return out;
}

static void set_color(cairo_t *cr, uint32_t rgb)
{
	cairo_set_source_rgb(cr, (double)((rgb >> 16) & 0xff) / 255.0,
			     (double)((rgb >> 8) & 0xff) / 255.0, (double)(rgb & 0xff) / 255.0);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static PangoLayout *make_layout(cairo_t *cr, const PangoFontDescription *font, const char *text)
{
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text, -1);
	return layout;
}

/* Draws `text` centred on (cx, cy). */
static void draw_centered(cairo_t *cr, const PangoFontDescription *font, const char *text,
			  double cx, double cy)
{
	PangoLayout *layout = make_layout(cr, font, text);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	cairo_move_to(cr, cx - w / 2.0, cy - h / 2.0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

struct rei_pixbuf *rei_bar_render(const struct rei_bar_state *st)
{
	bool vertical = rei_bar_vertical(st->position);
	int lw = vertical ? st->thickness : st->length;
	int lh = vertical ? st->length : st->thickness;
	int scale = st->scale < 1 ? 1 : st->scale;
	if (lw <= 0 || lh <= 0)
		return NULL;

	struct rei_pixbuf *pb = rei_pixbuf_create(lw * scale, lh * scale, true);
	if (!pb)
		return NULL;

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, lw * scale, lh * scale);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);  /* Everything below is in logical pixels. */

	set_color(cr, st->background);
	cairo_paint(cr);

	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");

	/* Workspace buttons, from the start of the bar. */
	struct rei_rect bar = { 0, 0, lw, lh };
	for (int i = 0; i < st->n_buttons; i++) {
		const struct rei_bar_button *b = &st->buttons[i];
		struct rei_rect cell = rei_bar_button_rect(bar, st->position, i);

		bool filled = true;
		uint32_t fill;
		if (b->kind == REI_BAR_BUTTON_FOCUSED)
			fill = st->highlight;
		else if (b->kind == REI_BAR_BUTTON_VISIBLE)
			fill = rei_bar_mix(st->background, st->highlight, 0.45);
		else if (i == st->hover)
			fill = rei_bar_mix(st->background, st->foreground, 0.18);
		else
			filled = false;
		/* Hovering lightens any button a little. */
		if (filled && i == st->hover && b->kind != REI_BAR_BUTTON_HIDDEN)
			fill = rei_bar_mix(fill, st->foreground, 0.15);

		if (filled) {
			set_color(cr, fill);
			rounded_rect(cr, cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 4, 4);
			cairo_fill(cr);
		}

		char label[16];
		snprintf(label, sizeof label, "%d", b->number);
		set_color(cr, st->foreground);
		draw_centered(cr, font, label, cell.x + cell.w / 2.0, cell.y + cell.h / 2.0);
	}

	/* The clock, at the end. A vertical bar is too narrow for "14:32", so it is
	 * broken into stacked parts ("14" over "32"). */
	if (st->clock[0]) {
		set_color(cr, st->foreground);
		const double pad = 8;
		if (!vertical) {
			PangoLayout *layout = make_layout(cr, font, st->clock);
			int w, h;
			pango_layout_get_pixel_size(layout, &w, &h);
			cairo_move_to(cr, lw - pad - w, (lh - h) / 2.0);
			pango_cairo_show_layout(cr, layout);
			g_object_unref(layout);
		} else {
			char parts[4][32];
			int n = 0;
			char copy[64];
			snprintf(copy, sizeof copy, "%s", st->clock);
			for (char *tok = strtok(copy, ": "); tok && n < 4; tok = strtok(NULL, ": "))
				snprintf(parts[n++], sizeof parts[0], "%s", tok);

			double line_h = 0;
			for (int i = 0; i < n; i++) {
				PangoLayout *layout = make_layout(cr, font, parts[i]);
				int w, h;
				pango_layout_get_pixel_size(layout, &w, &h);
				line_h = h > line_h ? h : line_h;
				g_object_unref(layout);
			}
			double y = lh - pad - line_h * n;
			for (int i = 0; i < n; i++)
				draw_centered(cr, font, parts[i], lw / 2.0, y + line_h * (i + 0.5));
		}
	}

	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_flush(surface);

	/* Cairo's ARGB32 is premultiplied BGRA in memory, exactly our pixel format. */
	const uint8_t *src = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	for (int y = 0; y < pb->height; y++)
		memcpy(pb->data + (size_t)y * (size_t)pb->width * 4, src + (size_t)y * (size_t)stride,
		       (size_t)pb->width * 4);
	cairo_surface_destroy(surface);
	return pb;
}

void rei_bar_render_cleanup(void)
{
	/* Pango keeps every font it has loaded, with its fontconfig data, in a
	 * process-wide font map, and fontconfig keeps its parsed configuration. Releasing
	 * both frees those allocations, which otherwise show up as leaks in leak checkers.
	 * (cairo_debug_reset_static_data() is deliberately not called: it asserts if any
	 * cairo object is still alive, and Pango frees fonts lazily, so it failed
	 * intermittently.) */
	pango_cairo_font_map_set_default(NULL);
	FcFini();
}
