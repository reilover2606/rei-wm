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

/* ---- The volume text: a fixed place just before the clock ---- */

static const double BAR_PAD = 8, VOLUME_GAP = 14;

/* Size of the clock's text block, for placing the volume next to it. */
static void clock_extent(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st,
			 double *w, double *h, double *line_h)
{
	*w = *h = *line_h = 0;
	if (!st->clock[0])
		return;
	if (!rei_bar_vertical(st->position)) {
		PangoLayout *l = make_layout(cr, font, st->clock);
		int iw, ih;
		pango_layout_get_pixel_size(l, &iw, &ih);
		g_object_unref(l);
		*w = iw;
		*h = *line_h = ih;
		return;
	}
	char copy[64];
	snprintf(copy, sizeof copy, "%s", st->clock);
	int n = 0;
	for (char *tok = strtok(copy, ": "); tok && n < 4; tok = strtok(NULL, ": ")) {
		PangoLayout *l = make_layout(cr, font, tok);
		int iw, ih;
		pango_layout_get_pixel_size(l, &iw, &ih);
		g_object_unref(l);
		*w = iw > *w ? iw : *w;
		*line_h = ih > *line_h ? ih : *line_h;
		n++;
	}
	*h = *line_h * n;
}

/* Where the volume text goes, and its size. */
static void volume_layout(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st,
			  double *x, double *y, double *w, double *h)
{
	bool vertical = rei_bar_vertical(st->position);
	int lw = vertical ? st->thickness : st->length;
	int lh = vertical ? st->length : st->thickness;
	double cw, ch, cl;
	clock_extent(cr, font, st, &cw, &ch, &cl);

	PangoLayout *l = make_layout(cr, font, st->volume);
	int iw, ih;
	pango_layout_get_pixel_size(l, &iw, &ih);
	g_object_unref(l);
	*w = iw;
	*h = ih;
	if (!vertical) {
		*x = lw - BAR_PAD - cw - (cw > 0 ? VOLUME_GAP : 0) - iw;
		*y = (lh - ih) / 2.0;
	} else {
		*x = (lw - iw) / 2.0;
		*y = lh - BAR_PAD - ch - (ch > 0 ? VOLUME_GAP : 0) - ih;
	}
}

bool rei_bar_volume_rect(const struct rei_bar_state *st, struct rei_rect *out)
{
	if (!st->volume[0])
		return false;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(surface);
	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");
	double x, y, w, h;
	volume_layout(cr, font, st, &x, &y, &w, &h);
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	/* A generous target: the whole height (or width) of the bar, a little wider than the text. */
	if (!rei_bar_vertical(st->position))
		*out = (struct rei_rect){ (int)floor(x) - 6, 0, (int)ceil(w) + 12, st->thickness };
	else
		*out = (struct rei_rect){ 0, (int)floor(y) - 3, st->thickness, (int)ceil(h) + 6 };
	return true;
}


/* ---- The application tray ---- */

#define TRAY_MARGIN 3.0      /* From the bar's edges to a tile. */
#define TRAY_STACK_STEP 4.0  /* How far each tile of a stack peeks out. */
#define TRAY_MAX_LAYERS 4
#define TRAY_NAME_W 150.0    /* A tile's width with its name. */
#define TRAY_GAP 6.0

static int tray_layers(const struct rei_bar_tray_item *it)
{
	return it->count < 1 ? 1 : it->count > TRAY_MAX_LAYERS ? TRAY_MAX_LAYERS : it->count;
}

size_t rei_bar_tray_layout(const struct rei_bar_state *st, struct rei_rect *out)
{
	if (st->tray_mode == REI_TRAY_OFF || st->n_tray <= 0)
		return 0;
	bool vertical = rei_bar_vertical(st->position);
	int lw = vertical ? st->thickness : st->length;
	int lh = vertical ? st->length : st->thickness;
	double tile = st->thickness - 2 * TRAY_MARGIN;  /* Height of a horizontal tile, width of a vertical one. */

	/* What is already at the ends of the bar. */
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(surface);
	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");
	double start = st->n_buttons * (double)st->thickness + 12;
	double end = vertical ? lh : lw;
	double cw, ch, cl;
	clock_extent(cr, font, st, &cw, &ch, &cl);
	end -= BAR_PAD + (vertical ? ch : cw);
	if (st->volume[0]) {
		double vx, vy, vw, vh;
		volume_layout(cr, font, st, &vx, &vy, &vw, &vh);
		end = vertical ? vy : vx;
	}
	end -= 12;
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	/* Widths along the bar. With names, tiles shrink (to icon-only) before any are dropped. */
	size_t n = (size_t)st->n_tray < REI_BAR_MAX_TRAY ? (size_t)st->n_tray : REI_BAR_MAX_TRAY;
	double name_w = (!vertical && st->tray_mode == REI_TRAY_ICONS_NAMES) ? TRAY_NAME_W - tile : 0;
	double avail = end - start;
	double total = 0;
	for (int pass = 0; pass < 2; pass++) {
		total = 0;
		for (size_t i = 0; i < n; i++)
			total += tile + name_w + (tray_layers(&st->tray[i]) - 1) * TRAY_STACK_STEP +
				 (i ? TRAY_GAP : 0);
		if (total <= avail || name_w <= 0)
			break;
		/* Shrink the names evenly; never below icon-only. */
		double over = total - avail;
		name_w -= over / (double)n;
		if (name_w < 0)
			name_w = 0;
	}
	/* Still too much: show as many as fit. */
	while (n > 0 && total > avail) {
		n--;
		total -= tile + name_w + (tray_layers(&st->tray[n]) - 1) * TRAY_STACK_STEP + (n ? TRAY_GAP : 0);
	}
	if (n == 0)
		return 0;

	/* Centred on the bar, kept clear of the ends. */
	double pos = (vertical ? lh : lw) / 2.0 - total / 2.0;
	if (pos < start)
		pos = start;
	if (pos + total > end)
		pos = end - total;
	for (size_t i = 0; i < n; i++) {
		double extent = tile + name_w + (tray_layers(&st->tray[i]) - 1) * TRAY_STACK_STEP;
		if (!vertical)
			out[i] = (struct rei_rect){ (int)floor(pos), (int)TRAY_MARGIN, (int)ceil(extent), (int)tile };
		else
			out[i] = (struct rei_rect){ (int)TRAY_MARGIN, (int)floor(pos), (int)tile, (int)ceil(extent) };
		pos += extent + TRAY_GAP;
	}
	(void)lw;
	return n;
}

static void draw_icon(cairo_t *cr, const struct rei_pixbuf *icon, double x, double y, double size,
		      double alpha)
{
	if (!icon || icon->width <= 0 || icon->height <= 0)
		return;
	cairo_surface_t *src = cairo_image_surface_create_for_data((unsigned char *)icon->data,
								   CAIRO_FORMAT_ARGB32, icon->width,
								   icon->height, icon->width * 4);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size / icon->width, size / icon->height);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
	cairo_surface_destroy(src);
}

static void draw_tray(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st)
{
	struct rei_rect rects[REI_BAR_MAX_TRAY];
	size_t n = rei_bar_tray_layout(st, rects);
	bool vertical = rei_bar_vertical(st->position);
	double tile = st->thickness - 2 * TRAY_MARGIN;

	for (size_t i = 0; i < n; i++) {
		const struct rei_bar_tray_item *it = &st->tray[i];
		const struct rei_rect *r = &rects[i];
		int layers = tray_layers(it);
		double main_w = vertical ? tile : r->w - (layers - 1) * TRAY_STACK_STEP;
		double main_h = vertical ? r->h - (layers - 1) * TRAY_STACK_STEP : tile;

		/* The tiles behind, peeking out along the bar, back to front. */
		for (int k = layers - 1; k >= 1; k--) {
			double dx = vertical ? 0 : k * TRAY_STACK_STEP, dy = vertical ? k * TRAY_STACK_STEP : 0;
			double inset = k * 1.5;
			/* Clearly lighter than the bar, and fading with depth; tinted when the stack is focused. */
			uint32_t back = it->active ? rei_bar_mix(st->background, st->highlight, 0.62 - 0.12 * (k - 1))
						   : rei_bar_mix(st->background, st->foreground, 0.26 - 0.05 * (k - 1));
			set_color(cr, back);
			if (!vertical)
				rounded_rect(cr, r->x + dx, r->y + inset, main_w, main_h - 2 * inset, 4);
			else
				rounded_rect(cr, r->x + inset, r->y + dy, main_w - 2 * inset, main_h, 4);
			cairo_fill(cr);
		}

		/* The front tile. */
		uint32_t fill = it->active ? st->highlight
					   : rei_bar_mix(st->background, st->foreground, it->minimized ? 0.06 : 0.13);
		set_color(cr, fill);
		rounded_rect(cr, r->x, r->y, main_w, main_h, 4);
		cairo_fill(cr);

		double icon = (vertical ? main_w : main_h) - 6;
		double ix = r->x + 3, iy = r->y + 3;
		if (!vertical)
			iy = r->y + (main_h - icon) / 2.0;
		draw_icon(cr, it->icon, ix, iy, icon, it->minimized ? 0.5 : 1.0);

		if (!vertical && st->tray_mode == REI_TRAY_ICONS_NAMES && main_w > tile + 8) {
			PangoLayout *l = make_layout(cr, font, it->label);
			double tx = r->x + 3 + icon + 5;
			pango_layout_set_width(l, (int)((main_w - (tx - r->x) - 4) * PANGO_SCALE));
			pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
			pango_layout_set_height(l, 0);
			int tw, th;
			pango_layout_get_pixel_size(l, &tw, &th);
			set_color(cr, it->minimized ? rei_bar_mix(st->background, st->foreground, 0.55)
						    : st->foreground);
			cairo_move_to(cr, tx, r->y + (main_h - th) / 2.0);
			pango_cairo_show_layout(cr, l);
			g_object_unref(l);
		}
	}
}

/* ---- The window list ---- */

#define MENU_ROW_H 26
#define MENU_PAD_X 10
#define MENU_MIN_W 180
#define MENU_MAX_W 400

void rei_bar_menu_size(const struct rei_menu_state *st, int *w, int *h, int *row_h)
{
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(surface);
	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");
	int widest = 0;
	for (int i = 0; i < st->n_rows; i++) {
		PangoLayout *l = make_layout(cr, font, st->rows[i].label);
		int iw, ih;
		pango_layout_get_pixel_size(l, &iw, &ih);
		g_object_unref(l);
		widest = iw > widest ? iw : widest;
	}
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	int width = widest + 2 * MENU_PAD_X + 2;
	*w = width < MENU_MIN_W ? MENU_MIN_W : width > MENU_MAX_W ? MENU_MAX_W : width;
	*row_h = MENU_ROW_H;
	*h = st->n_rows * MENU_ROW_H + 2;
}

struct rei_pixbuf *rei_bar_menu_render(const struct rei_menu_state *st)
{
	int lw, lh, row_h;
	rei_bar_menu_size(st, &lw, &lh, &row_h);
	int scale = st->scale < 1 ? 1 : st->scale;
	struct rei_pixbuf *pb = rei_pixbuf_create(lw * scale, lh * scale, true);
	if (!pb)
		return NULL;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, lw * scale, lh * scale);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	set_color(cr, rei_bar_mix(st->background, st->foreground, 0.30));  /* the border */
	cairo_paint(cr);
	set_color(cr, st->background);
	cairo_rectangle(cr, 1, 1, lw - 2, lh - 2);
	cairo_fill(cr);

	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");
	for (int i = 0; i < st->n_rows; i++) {
		double y = 1 + i * row_h;
		if (i == st->hover) {
			set_color(cr, st->highlight);
			cairo_rectangle(cr, 1, y, lw - 2, row_h);
			cairo_fill(cr);
		}
		PangoLayout *l = make_layout(cr, font, st->rows[i].label);
		pango_layout_set_width(l, (lw - 2 * MENU_PAD_X - 2) * PANGO_SCALE);
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
		int tw, th;
		pango_layout_get_pixel_size(l, &tw, &th);
		set_color(cr, st->rows[i].minimized ? rei_bar_mix(st->background, st->foreground, 0.6)
						    : st->foreground);
		cairo_move_to(cr, 1 + MENU_PAD_X, y + (row_h - th) / 2.0);
		pango_cairo_show_layout(cr, l);
		g_object_unref(l);
	}
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_flush(surface);

	const uint8_t *src = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	for (int y = 0; y < pb->height; y++)
		memcpy(pb->data + (size_t)y * (size_t)pb->width * 4, src + (size_t)y * (size_t)stride,
		       (size_t)pb->width * 4);
	cairo_surface_destroy(surface);
	return pb;
}

/* A stand-in icon: a rounded square in a colour taken from the name, with its first letter. */
struct rei_pixbuf *rei_bar_letter_icon(const char *name)
{
	const int size = 48;
	struct rei_pixbuf *pb = rei_pixbuf_create(size, size, false);
	if (!pb)
		return NULL;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(surface);

	uint32_t h = 2166136261u;
	for (const char *p = name; p && *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	/* A pleasant range of hues at fixed lightness. */
	double hue = (double)(h % 360) / 360.0;
	double r = 0.5 + 0.35 * cos(6.2831853 * (hue + 0.0));
	double g = 0.5 + 0.35 * cos(6.2831853 * (hue + 0.33));
	double b = 0.5 + 0.35 * cos(6.2831853 * (hue + 0.67));
	cairo_set_source_rgb(cr, r * 0.8, g * 0.8, b * 0.8);
	rounded_rect(cr, 2, 2, size - 4, size - 4, 10);
	cairo_fill(cr);

	char letter[8] = "?";
	for (const char *p = name; p && *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
			letter[0] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
			letter[1] = '\0';
			break;
		}
	}
	PangoFontDescription *font = pango_font_description_from_string("Sans Bold 22");
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_centered(cr, font, letter, size / 2.0, size / 2.0);
	pango_font_description_free(font);

	cairo_destroy(cr);
	cairo_surface_flush(surface);
	const uint8_t *src = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	for (int y = 0; y < pb->height; y++)
		memcpy(pb->data + (size_t)y * (size_t)pb->width * 4, src + (size_t)y * (size_t)stride,
		       (size_t)pb->width * 4);
	cairo_surface_destroy(surface);
	return pb;
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

	draw_tray(cr, font, st);

	/* The volume, just before the clock. */
	if (st->volume[0]) {
		double vx, vy, vw, vh;
		volume_layout(cr, font, st, &vx, &vy, &vw, &vh);
		set_color(cr, st->volume_muted ? rei_bar_mix(st->background, st->foreground, 0.5) : st->foreground);
		PangoLayout *layout = make_layout(cr, font, st->volume);
		cairo_move_to(cr, vx, vy);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
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
