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

int rei_bar_start_extent(enum rei_bar_position pos, int thickness)
{
	return rei_bar_vertical(pos) ? thickness : 68;
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


/* ---- The battery: an icon and a percentage, just before the volume (or the clock) ---- */

#define BATT_BODY_W 24.0
#define BATT_BODY_H 12.0
#define BATT_NUB_W 2.5
#define BATT_ICON_W (BATT_BODY_W + BATT_NUB_W)
#define BATT_GAP 5.0     /* icon to text */
#define BATT_LOW 15

static void battery_text(const struct rei_bar_state *st, char *out, size_t n)
{
	snprintf(out, n, "%d%%", st->battery_percent);
}

/* Where the battery widget goes, and the size of its text. */
static void battery_layout(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st,
			   double *x, double *y, double *w, double *h, double *tw_out, double *th_out)
{
	bool vertical = rei_bar_vertical(st->position);
	int lw = vertical ? st->thickness : st->length;
	int lh = vertical ? st->length : st->thickness;
	char text[16];
	battery_text(st, text, sizeof text);
	PangoLayout *l = make_layout(cr, font, text);
	int tw, th;
	pango_layout_get_pixel_size(l, &tw, &th);
	g_object_unref(l);
	*tw_out = tw;
	*th_out = th;

	/* Whatever is already at the end: the volume if shown, else the clock. */
	double cw, ch, cl;
	clock_extent(cr, font, st, &cw, &ch, &cl);
	double edge;  /* The near edge of what is already there, along the bar. */
	if (st->volume[0]) {
		double vx, vy, vw, vh;
		volume_layout(cr, font, st, &vx, &vy, &vw, &vh);
		edge = vertical ? vy : vx;
	} else {
		edge = vertical ? lh - BAR_PAD - ch : lw - BAR_PAD - cw;
	}
	bool anything = st->volume[0] || st->clock[0];
	if (!vertical) {
		*w = BATT_ICON_W + BATT_GAP + tw;
		*h = th > BATT_BODY_H ? th : BATT_BODY_H;
		*x = edge - (anything ? VOLUME_GAP : 0) - *w;
		*y = (lh - *h) / 2.0;
	} else {
		*w = tw > BATT_ICON_W ? tw : BATT_ICON_W;
		*h = BATT_BODY_H + 3 + th;
		*x = (lw - *w) / 2.0;
		*y = edge - (anything ? VOLUME_GAP : 0) - *h;
	}
}

/* ---- Styles: one drawing path for every themed element ---- */

static void set_rgba(cairo_t *cr, uint32_t rgb, double a)
{
	cairo_set_source_rgba(cr, (double)((rgb >> 16) & 0xff) / 255.0, (double)((rgb >> 8) & 0xff) / 255.0,
			      (double)(rgb & 0xff) / 255.0, a);
}

/* The outline of a box in the style's shape. */
static void shape_path(cairo_t *cr, enum rei_shape shape, double x, double y, double w, double h,
		       double radius)
{
	double maxr = fmin(w, h) / 2.0;
	double r = shape == REI_SHAPE_SQUARE ? 0 : shape == REI_SHAPE_PILL ? maxr : radius;
	if (r > maxr)
		r = maxr;
	if (r < 0)
		r = 0;
	cairo_new_path(cr);
	if (r <= 0.01) {
		cairo_rectangle(cr, x, y, w, h);
	} else if (shape == REI_SHAPE_BEVEL) {
		cairo_move_to(cr, x + r, y);
		cairo_line_to(cr, x + w - r, y);
		cairo_line_to(cr, x + w, y + r);
		cairo_line_to(cr, x + w, y + h - r);
		cairo_line_to(cr, x + w - r, y + h);
		cairo_line_to(cr, x + r, y + h);
		cairo_line_to(cr, x, y + h - r);
		cairo_line_to(cr, x, y + r);
		cairo_close_path(cr);
	} else {
		rounded_rect(cr, x, y, w, h, r);
	}
}

/* A soft shadow: layers growing outward, each faint, adding up to `opacity` at the centre. */
static void draw_shadow(cairo_t *cr, const struct rei_style *st, double x, double y, double w, double h,
			double mult)
{
	const struct rei_shadow *sh = &st->shadow;
	if (sh->opacity <= 0.001f || mult <= 0)
		return;
	int n = sh->blur < 1.0f ? 1 : (int)ceil(sh->blur);
	double total = sh->opacity * mult;
	double layer = 1.0 - pow(1.0 - (total > 0.98 ? 0.98 : total), 1.0 / n);
	for (int j = 0; j < n; j++) {
		double grow = sh->blur / 2.0 - j * ((double)sh->blur / n);
		shape_path(cr, st->shape, x + sh->dx - grow, y + sh->dy - grow, w + 2 * grow, h + 2 * grow,
			   st->radius + grow);
		set_rgba(cr, sh->color, layer);
		cairo_fill(cr);
	}
}

/* Shadow, fill and border of one element. `mult` is any extra transparency (a fade, a drag). */
static void draw_box(cairo_t *cr, const struct rei_style *st, double x, double y, double w, double h,
		     uint32_t fill, double mult)
{
	draw_shadow(cr, st, x, y, w, h, mult);
	shape_path(cr, st->shape, x, y, w, h, st->radius);
	set_rgba(cr, fill, st->opacity * mult);
	cairo_fill(cr);
	if (st->border_width > 0.01f) {
		double bw = st->border_width;
		shape_path(cr, st->shape, x + bw / 2, y + bw / 2, w - bw, h - bw, st->radius - bw / 2);
		cairo_set_line_width(cr, bw);
		set_rgba(cr, st->border_color, mult);
		cairo_stroke(cr);
	}
}

static void resolve_theme(const struct rei_bar_state *st, struct rei_theme *t)
{
	if (st->styled)
		*t = st->theme;
	else
		rei_theme_defaults(t, st->background, st->foreground, st->highlight);
}

/* ---- The application tray ---- */

#define TRAY_MARGIN 3.0      /* From the bar's edges to a tile. */
#define TRAY_STACK_STEP 4.0  /* How far each tile of a stack peeks out. */
#define TRAY_MAX_LAYERS 4
#define TRAY_NAT_NAME_W 150.0   /* A tile's natural width with its name. */
#define TRAY_NAT_ICON_W 30.0    /* ...and icon only. */
#define TRAY_FLOOR_W 18.0       /* Tiles never shrink below this, whatever the percentage says. */
#define TRAY_OVERFLOW_W 34.0
#define TRAY_GAP 6.0

static int tray_layers(const struct rei_bar_tray_item *it)
{
	return it->count < 1 ? 1 : it->count > TRAY_MAX_LAYERS ? TRAY_MAX_LAYERS : it->count;
}

static double tray_extra(const struct rei_bar_tray_item *it)
{
	return (tray_layers(it) - 1) * TRAY_STACK_STEP;
}

/* The free space at either end of the bar (workspace buttons; clock and volume). */
static void tray_bounds(const struct rei_bar_state *st, double *start, double *end)
{
	bool vertical = rei_bar_vertical(st->position);
	double length = vertical ? st->length : st->length;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(surface);
	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");
	*start = st->start_w + st->mode_w + st->n_buttons * (double)st->thickness + 12;
	*end = length;
	double cw, ch, cl;
	clock_extent(cr, font, st, &cw, &ch, &cl);
	*end -= BAR_PAD + (vertical ? ch : cw);
	if (st->volume[0]) {
		double vx, vy, vw, vh;
		volume_layout(cr, font, st, &vx, &vy, &vw, &vh);
		*end = vertical ? vy : vx;
	}
	if (st->battery_present) {
		double bx, by, bw, bh, btw, bth;
		battery_layout(cr, font, st, &bx, &by, &bw, &bh, &btw, &bth);
		*end = vertical ? by : bx;
	}
	*end -= 12;
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

void rei_bar_tray_layout(const struct rei_bar_state *st, struct rei_tray_layout *out)
{
	memset(out, 0, sizeof *out);
	out->drop_index = -1;
	if (st->tray_mode == REI_TRAY_OFF || (st->n_tray <= 0 && st->tray_extra_windows <= 0))
		return;

	bool vertical = rei_bar_vertical(st->position);
	double length = st->length;
	double tile = st->thickness - 2 * TRAY_MARGIN;  /* Across the bar. */

	/* The tray lives in the middle three fifths of the bar, and never on top of the ends. */
	double lo, hi;
	tray_bounds(st, &lo, &hi);
	double rs = fmax(lo, 0.2 * length), re = fmin(hi, 0.8 * length);
	double avail = re - rs;
	if (avail < TRAY_FLOOR_W)
		return;

	double nat = (!vertical && st->tray_mode == REI_TRAY_ICONS_NAMES) ? TRAY_NAT_NAME_W : TRAY_NAT_ICON_W;
	int pct = st->tray_min_percent < 10 ? 10 : st->tray_min_percent > 100 ? 100 : st->tray_min_percent;
	double minw = fmax(TRAY_FLOOR_W, nat * pct / 100.0);
	if (minw > nat)
		minw = nat;

	size_t n = (size_t)st->n_tray < REI_BAR_MAX_TRAY ? (size_t)st->n_tray : REI_BAR_MAX_TRAY;
	bool overflow = st->tray_extra_windows > 0;
	double sumx = 0;
	for (size_t i = 0; i < n; i++)
		sumx += tray_extra(&st->tray[i]);

	/* Total length of k tiles of width w (the first k groups), plus the overflow tile if shown. */
	#define NEED(k, w, ovf) ({                                                                      \
		double _t = 0;                                                                          \
		for (size_t _i = 0; _i < (k); _i++)                                                     \
			_t += (w) + tray_extra(&st->tray[_i]) + (_i ? TRAY_GAP : 0);                    \
		if (ovf)                                                                                \
			_t += TRAY_OVERFLOW_W + ((k) ? TRAY_GAP : 0);                                   \
		_t; })

	double w = nat;
	size_t k = n;
	if (NEED(n, nat, overflow) > avail) {
		double fixed = sumx + TRAY_GAP * (n ? n - 1 : 0) + (overflow ? TRAY_OVERFLOW_W + (n ? TRAY_GAP : 0) : 0);
		double fit = n ? (avail - fixed) / (double)n : nat;
		if (fit >= minw) {
			w = fit;
		} else {
			/* Even the narrowest tiles do not all fit: the newest go under an overflow tile. */
			w = minw;
			overflow = true;
			k = n;
			while (k > 0 && NEED(k, minw, true) > avail)
				k--;
		}
	}
	double total = NEED(k, w, overflow);
	#undef NEED

	int overflow_windows = st->tray_extra_windows;
	for (size_t i = k; i < n; i++)
		overflow_windows += st->tray[i].count;
	out->n_visible = k;
	out->has_overflow = overflow;
	out->overflow_windows = overflow_windows;

	double centre = length / 2.0;
	double pos = centre - total / 2.0;
	if (pos + total > re)
		pos = re - total;
	if (pos < rs)
		pos = rs;

	/* Dragging: the grabbed tile follows the pointer, and the others make room where it would land. */
	size_t order[REI_BAR_MAX_TRAY];
	size_t no = 0;
	int drag = (st->drag_index >= 0 && (size_t)st->drag_index < k) ? st->drag_index : -1;
	if (drag >= 0) {
		double p = pos;
		int drop = 0;
		for (size_t i = 0; i < k; i++) {
			double ext = w + tray_extra(&st->tray[i]);
			if ((int)i != drag && p + ext / 2.0 < st->drag_pos)
				drop++;
			p += ext + TRAY_GAP;
		}
		out->drop_index = drop;
		for (size_t i = 0; i < k; i++)
			if ((int)i != drag)
				order[no++] = i;
		/* Insert the dragged group at `drop` among the others. */
		for (size_t j = no; j > (size_t)drop; j--)
			order[j] = order[j - 1];
		order[drop] = (size_t)drag;
		no++;
	} else {
		for (size_t i = 0; i < k; i++)
			order[no++] = i;
	}

	double p = pos;
	for (size_t j = 0; j < no; j++) {
		size_t g = order[j];
		double ext = w + tray_extra(&st->tray[g]);
		if (!vertical)
			out->tile[g] = (struct rei_rect){ (int)floor(p), (int)TRAY_MARGIN, (int)ceil(ext), (int)tile };
		else
			out->tile[g] = (struct rei_rect){ (int)TRAY_MARGIN, (int)floor(p), (int)tile, (int)ceil(ext) };
		p += ext + TRAY_GAP;
	}
	if (overflow) {
		if (!vertical)
			out->overflow = (struct rei_rect){ (int)floor(p), (int)TRAY_MARGIN, (int)TRAY_OVERFLOW_W, (int)tile };
		else
			out->overflow = (struct rei_rect){ (int)TRAY_MARGIN, (int)floor(p), (int)tile, (int)TRAY_OVERFLOW_W };
	}
	if (drag >= 0) {
		/* The grabbed tile hovers under the pointer, kept inside the tray's range. */
		struct rei_rect *r = &out->tile[drag];
		double ext = vertical ? r->h : r->w;
		double at = st->drag_pos - ext / 2.0;
		if (at < rs)
			at = rs;
		if (at + ext > re)
			at = re - ext;
		if (vertical)
			r->y = (int)round(at);
		else
			r->x = (int)round(at);
	}
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

/* One tile (with its stack, if it has several windows). `lift` > 0 draws it picked up. */
static void draw_tile(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st,
		      const struct rei_theme *theme, const struct rei_bar_tray_item *it, struct rei_rect r,
		      bool hovered, double lift)
{
	const struct rei_style *ts = &theme->e[REI_ELEM_TRAY_TILE];
	const struct rei_style *bar = &theme->e[REI_ELEM_TOOLBAR];
	bool vertical = rei_bar_vertical(st->position);
	double tile = st->thickness - 2 * TRAY_MARGIN;
	int layers = tray_layers(it);
	double main_w = vertical ? tile : r.w - (layers - 1) * TRAY_STACK_STEP;
	double main_h = vertical ? r.h - (layers - 1) * TRAY_STACK_STEP : tile;
	double ry = r.y - lift;

	uint32_t fill = it->active ? ts->active_color : it->minimized ? ts->inactive_color : ts->color;
	uint32_t text = it->active ? ts->active_text_color : it->minimized ? ts->inactive_text_color : ts->text_color;
	if (hovered)  /* The highlight starts the moment the pointer touches the tile. */
		fill = rei_bar_mix(fill, ts->text_color, 0.16);

	/* The tiles behind, peeking out along the bar, back to front. */
	for (int k = layers - 1; k >= 1; k--) {
		double dx = vertical ? 0 : k * TRAY_STACK_STEP, dy = vertical ? k * TRAY_STACK_STEP : 0;
		double inset = k * 1.5;
		uint32_t back = it->active ? rei_bar_mix(bar->color, fill, 0.62 - 0.12 * (k - 1))
					   : rei_bar_mix(bar->color, ts->text_color, 0.26 - 0.05 * (k - 1));
		struct rei_style flat = *ts;
		flat.shadow.opacity = 0;
		flat.border_width = 0;
		if (!vertical)
			draw_box(cr, &flat, r.x + dx, ry + inset, main_w, main_h - 2 * inset, back, 1.0);
		else
			draw_box(cr, &flat, r.x + inset, ry + dy, main_w - 2 * inset, main_h, back, 1.0);
	}

	draw_box(cr, ts, r.x, ry, main_w, main_h, fill, lift > 0 ? 0.94 : 1.0);

	double icon = (vertical ? main_w : main_h) - 6;
	double ix = r.x + 3, iy = ry + 3;
	if (!vertical)
		iy = ry + (main_h - icon) / 2.0;
	else if (main_w < icon + 6)
		icon = main_w - 6;
	if (!vertical && main_w < icon + 6)
		icon = main_w - 6;
	draw_icon(cr, it->icon, ix, iy, icon, it->minimized ? 0.5 : 1.0);

	if (!vertical && st->tray_mode == REI_TRAY_ICONS_NAMES && main_w > tile + 14) {
		PangoLayout *l = make_layout(cr, font, it->label);
		double tx = r.x + 3 + icon + 5;
		pango_layout_set_width(l, (int)((main_w - (tx - r.x) - 4) * PANGO_SCALE));
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
		int tw, th;
		pango_layout_get_pixel_size(l, &tw, &th);
		set_color(cr, text);
		cairo_move_to(cr, tx, ry + (main_h - th) / 2.0);
		pango_cairo_show_layout(cr, l);
		g_object_unref(l);
	}
}

static void draw_overflow(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st,
			  const struct rei_theme *theme, struct rei_rect r, int windows, bool hovered)
{
	const struct rei_style *os = &theme->e[REI_ELEM_TRAY_OVERFLOW];
	uint32_t fill = os->color;
	if (hovered)
		fill = rei_bar_mix(fill, os->text_color, 0.16);
	draw_box(cr, os, r.x, r.y, r.w, r.h, fill, 1.0);
	char label[16];
	snprintf(label, sizeof label, "+%d", windows);
	set_color(cr, os->text_color);
	draw_centered(cr, font, label, r.x + r.w / 2.0, r.y + r.h / 2.0);
	(void)st;
}

static void draw_tray(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st,
		      const struct rei_theme *theme)
{
	struct rei_tray_layout lay;
	rei_bar_tray_layout(st, &lay);

	/* The grabbed tile is drawn last, on top. */
	for (size_t i = 0; i < lay.n_visible; i++) {
		if ((int)i == st->drag_index)
			continue;
		draw_tile(cr, font, st, theme, &st->tray[i], lay.tile[i], st->tray_hover == (int)i, 0);
	}
	if (lay.has_overflow)
		draw_overflow(cr, font, st, theme, lay.overflow, lay.overflow_windows,
			      st->tray_hover == REI_TRAY_HOVER_OVERFLOW);
	if (st->drag_index >= 0 && (size_t)st->drag_index < lay.n_visible)
		draw_tile(cr, font, st, theme, &st->tray[st->drag_index], lay.tile[st->drag_index], true, 2.0);
}

/* The battery: a horizontal cell that fills in proportion to the charge, then the percentage. */
static void draw_battery(cairo_t *cr, PangoFontDescription *font, const struct rei_bar_state *st,
			 const struct rei_theme *theme)
{
	const struct rei_style *bs = &theme->e[REI_ELEM_BATTERY];
	const struct rei_style *bar = &theme->e[REI_ELEM_TOOLBAR];
	bool vertical = rei_bar_vertical(st->position);
	double x, y, w, h, tw, th;
	battery_layout(cr, font, st, &x, &y, &w, &h, &tw, &th);

	int pct = st->battery_percent < 0 ? 0 : st->battery_percent > 100 ? 100 : st->battery_percent;
	bool low = pct <= BATT_LOW && !st->battery_charging;
	uint32_t level = st->battery_charging ? bs->active_color : low ? bs->inactive_color : bs->color;

	double ix = vertical ? x + (w - BATT_ICON_W) / 2.0 : x;
	double iy = vertical ? y : y + (h - BATT_BODY_H) / 2.0;
	double bw = bs->border_width > 0.01f ? bs->border_width : 0.0;

	/* The body (outline), and the little terminal on its right end. */
	if (bw > 0) {
		shape_path(cr, bs->shape, ix + bw / 2, iy + bw / 2, BATT_BODY_W - bw, BATT_BODY_H - bw, bs->radius - bw / 2);
		cairo_set_line_width(cr, bw);
		set_rgba(cr, bs->border_color, 1.0);
		cairo_stroke(cr);
	}
	set_rgba(cr, bs->border_color, 1.0);
	cairo_rectangle(cr, ix + BATT_BODY_W, iy + 3.5, BATT_NUB_W, BATT_BODY_H - 7);
	cairo_fill(cr);

	/* The level: grows from the left, inside the outline. */
	double pad = bw + 1.5;
	double full_w = BATT_BODY_W - 2 * pad, fh = BATT_BODY_H - 2 * pad;
	double fw = full_w * pct / 100.0;
	if (pct > 0 && fw < 1.5)
		fw = 1.5;  /* A nearly empty battery still shows a sliver. */
	if (fw > 0) {
		struct rei_style cell = *bs;
		cell.shadow.opacity = 0;
		cell.border_width = 0;
		cell.radius = fmax(0.0, bs->radius - pad);
		draw_box(cr, &cell, ix + pad, iy + pad, fw, fh, level, 1.0);
	}

	/* Charging: a small bolt over the cell. */
	if (st->battery_charging) {
		double cx = ix + BATT_BODY_W / 2.0, cy = iy + BATT_BODY_H / 2.0;
		cairo_new_path(cr);
		cairo_move_to(cr, cx + 1.5, cy - 4.5);
		cairo_line_to(cr, cx - 3.0, cy + 0.8);
		cairo_line_to(cr, cx - 0.2, cy + 0.8);
		cairo_line_to(cr, cx - 1.5, cy + 4.5);
		cairo_line_to(cr, cx + 3.0, cy - 0.8);
		cairo_line_to(cr, cx + 0.2, cy - 0.8);
		cairo_close_path(cr);
		set_rgba(cr, bar->color, 0.9);
		cairo_fill(cr);
	}

	/* The percentage. */
	char text[16];
	battery_text(st, text, sizeof text);
	set_color(cr, low ? bs->inactive_color : bs->text_color);
	PangoLayout *l = make_layout(cr, font, text);
	if (!vertical)
		cairo_move_to(cr, ix + BATT_ICON_W + BATT_GAP, y + (h - th) / 2.0);
	else
		cairo_move_to(cr, x + (w - tw) / 2.0, y + BATT_BODY_H + 3);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

/* ---- Tooltip ---- */

#define TIP_PAD_X 9
#define TIP_PAD_Y 5
#define TIP_MAX_W 420

static int shadow_margin(const struct rei_style *s)
{
	if (s->shadow.opacity <= 0.001f)
		return 0;
	double reach = s->shadow.blur / 2.0 + fmax(fabs(s->shadow.dx), fabs(s->shadow.dy)) + 1;
	return (int)ceil(reach);
}

struct rei_pixbuf *rei_bar_tooltip_render(const char *text, const char *font_desc,
					  const struct rei_style *style, int scale, int *box_w,
					  int *box_h, int *margin)
{
	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *pcr = cairo_create(probe);
	PangoFontDescription *font = pango_font_description_from_string(font_desc && font_desc[0] ? font_desc : "Sans 11");
	PangoLayout *l = make_layout(pcr, font, text);
	pango_layout_set_width(l, (TIP_MAX_W - 2 * TIP_PAD_X) * PANGO_SCALE);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	int tw, th;
	pango_layout_get_pixel_size(l, &tw, &th);
	g_object_unref(l);
	cairo_destroy(pcr);
	cairo_surface_destroy(probe);

	int bw = tw + 2 * TIP_PAD_X, bh = th + 2 * TIP_PAD_Y;
	int m = shadow_margin(style);
	*box_w = bw;
	*box_h = bh;
	*margin = m;
	if (scale < 1)
		scale = 1;
	int pw = (bw + 2 * m) * scale, ph = (bh + 2 * m) * scale;
	struct rei_pixbuf *pb = rei_pixbuf_create(pw, ph, false);
	if (!pb) {
		pango_font_description_free(font);
		return NULL;
	}
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	draw_box(cr, style, m, m, bw, bh, style->color, 1.0);
	PangoLayout *label = make_layout(cr, font, text);
	pango_layout_set_width(label, (TIP_MAX_W - 2 * TIP_PAD_X) * PANGO_SCALE);
	pango_layout_set_ellipsize(label, PANGO_ELLIPSIZE_END);
	set_color(cr, style->text_color);
	cairo_move_to(cr, m + TIP_PAD_X, m + TIP_PAD_Y);
	pango_cairo_show_layout(cr, label);
	g_object_unref(label);
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

/* ---- The Run prompt ---- */

#define PROMPT_PAD_X 26
#define PROMPT_PAD_Y 18

static PangoLayout *prompt_layout(cairo_t *cr, PangoFontDescription *font, const struct rei_prompt_state *st,
				  const char *text)
{
	PangoLayout *l = make_layout(cr, font, text);
	pango_layout_set_width(l, (st->box_w - 2 * PROMPT_PAD_X) * PANGO_SCALE);
	pango_layout_set_wrap(l, PANGO_WRAP_CHAR);   /* A long command with no spaces still wraps. */
	return l;
}

int rei_bar_prompt_height(const struct rei_prompt_state *st)
{
	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(probe);
	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 26");
	/* The cursor sits after the text, so a trailing space counts toward the last line. */
	PangoLayout *l = prompt_layout(cr, font, st, st->text[0] ? st->text : "M");
	int tw, th;
	pango_layout_get_pixel_size(l, &tw, &th);
	g_object_unref(l);
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(probe);
	return th + 2 * PROMPT_PAD_Y;
}

struct rei_pixbuf *rei_bar_prompt_render(const struct rei_prompt_state *st, int *box_h, int *margin)
{
	int bh = rei_bar_prompt_height(st);
	int m = shadow_margin(&st->style);
	*box_h = bh;
	*margin = m;
	int scale = st->scale < 1 ? 1 : st->scale;
	int pw = (st->box_w + 2 * m) * scale, ph = (bh + 2 * m) * scale;
	struct rei_pixbuf *pb = rei_pixbuf_create(pw, ph, false);
	if (!pb)
		return NULL;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	draw_box(cr, &st->style, m, m, st->box_w, bh, st->style.color, 1.0);

	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 26");
	bool empty = st->text[0] == '\0';
	PangoLayout *l = prompt_layout(cr, font, st, empty ? "Run a command" : st->text);
	set_color(cr, empty ? st->style.inactive_text_color : st->style.text_color);
	cairo_move_to(cr, m + PROMPT_PAD_X, m + PROMPT_PAD_Y);
	pango_cairo_show_layout(cr, l);

	if (st->cursor_on) {
		/* The cursor is at the end of the text (the start of the placeholder). */
		PangoRectangle strong;
		pango_layout_get_cursor_pos(l, empty ? 0 : (int)strlen(st->text), &strong, NULL);
		set_color(cr, st->style.text_color);
		double cx = m + PROMPT_PAD_X + (double)strong.x / PANGO_SCALE;
		double cy = m + PROMPT_PAD_Y + (double)strong.y / PANGO_SCALE;
		cairo_rectangle(cr, cx + (empty ? 0 : 1), cy, 2.5, (double)strong.height / PANGO_SCALE);
		cairo_fill(cr);
	}
	g_object_unref(l);
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

/* ---- The confirmation dialog ---- */

void rei_bar_confirm_buttons(struct rei_rect out[2])
{
	const int bw = 130, bh = 38, gap = 14, pad = 24;
	out[1] = (struct rei_rect){ REI_DIALOG_W - pad - bw, REI_DIALOG_H - pad - bh, bw, bh };
	out[0] = (struct rei_rect){ out[1].x - gap - bw, out[1].y, bw, bh };
}

struct rei_pixbuf *rei_bar_confirm_render(const struct rei_confirm_state *st, int *margin_out)
{
	int m = shadow_margin(&st->box);
	*margin_out = m;
	int scale = st->scale < 1 ? 1 : st->scale;
	int pw = (REI_DIALOG_W + 2 * m) * scale, ph = (REI_DIALOG_H + 2 * m) * scale;
	struct rei_pixbuf *pb = rei_pixbuf_create(pw, ph, false);
	if (!pb)
		return NULL;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	draw_box(cr, &st->box, m, m, REI_DIALOG_W, REI_DIALOG_H, st->box.color, 1.0);

	const char *fname = st->font[0] ? st->font : "Sans 11";
	PangoFontDescription *font = pango_font_description_from_string(fname);
	PangoFontDescription *big = pango_font_description_copy(font);
	pango_font_description_set_size(big, pango_font_description_get_size(font) * 3 / 2);
	pango_font_description_set_weight(big, PANGO_WEIGHT_BOLD);

	set_color(cr, st->box.text_color);
	PangoLayout *t = make_layout(cr, big, st->title);
	cairo_move_to(cr, m + 24, m + 22);
	pango_cairo_show_layout(cr, t);
	g_object_unref(t);

	PangoLayout *msg = make_layout(cr, font, st->message);
	pango_layout_set_width(msg, (REI_DIALOG_W - 48) * PANGO_SCALE);
	pango_layout_set_wrap(msg, PANGO_WRAP_WORD_CHAR);
	set_color(cr, st->box.inactive_text_color ? st->box.inactive_text_color : st->box.text_color);
	cairo_move_to(cr, m + 24, m + 62);
	pango_cairo_show_layout(cr, msg);
	g_object_unref(msg);

	struct rei_rect b[2];
	rei_bar_confirm_buttons(b);
	const char *labels[2] = { "Cancel", st->confirm_label };
	for (int i = 0; i < 2; i++) {
		uint32_t fill = i == 1 ? st->button.active_color : st->button.color;
		if (st->hover == i)
			fill = rei_bar_mix(fill, st->button.text_color, 0.15);
		draw_box(cr, &st->button, m + b[i].x, m + b[i].y, b[i].w, b[i].h, fill, 1.0);
		if (st->selected == i) {   /* The button Enter will press gets a ring. */
			set_color(cr, i == 1 ? st->button.active_text_color : st->button.text_color);
			cairo_set_line_width(cr, 1.5);
			cairo_rectangle(cr, m + b[i].x + 3.5, m + b[i].y + 3.5, b[i].w - 7, b[i].h - 7);
			cairo_stroke(cr);
		}
		set_color(cr, i == 1 ? st->button.active_text_color : st->button.text_color);
		draw_centered(cr, font, labels[i], m + b[i].x + b[i].w / 2.0, m + b[i].y + b[i].h / 2.0);
	}
	pango_font_description_free(big);
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

/* ---- The window list ---- */

#define MENU_ROW_H 26
#define MENU_PAD_X 10
#define MENU_MIN_W 180
#define MENU_MAX_W 400

void rei_bar_menu_size(const struct rei_menu_state *st, int *w, int *h, int *row_h, int *margin)
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
	int bw = (int)ceil(st->menu.border_width);
	int width = widest + 2 * MENU_PAD_X + 2 * bw;
	*w = width < MENU_MIN_W ? MENU_MIN_W : width > MENU_MAX_W ? MENU_MAX_W : width;
	*row_h = MENU_ROW_H;
	int gaps = 0;
	for (int i = 0; i < st->n_rows; i++)
		gaps += st->rows[i].gap_before;
	*h = st->n_rows * MENU_ROW_H + gaps + 2 * bw;
	*margin = shadow_margin(&st->menu);
}

int rei_bar_menu_row_at(const struct rei_menu_state *st, double y)
{
	double top = ceil(st->menu.border_width);
	double pos = top;
	for (int i = 0; i < st->n_rows; i++) {
		pos += st->rows[i].gap_before;
		if (y >= pos && y < pos + MENU_ROW_H)
			return i;
		pos += MENU_ROW_H;
	}
	return -1;
}

struct rei_pixbuf *rei_bar_menu_render(const struct rei_menu_state *st)
{
	int lw, lh, row_h, m;
	rei_bar_menu_size(st, &lw, &lh, &row_h, &m);
	int scale = st->scale < 1 ? 1 : st->scale;
	int pw = (lw + 2 * m) * scale, ph = (lh + 2 * m) * scale;
	struct rei_pixbuf *pb = rei_pixbuf_create(pw, ph, false);
	if (!pb)
		return NULL;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	const struct rei_style *ms = &st->menu, *rs = &st->row;
	draw_box(cr, ms, m, m, lw, lh, ms->color, 1.0);
	double bw = ceil(ms->border_width);

	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");
	double y = m + bw;
	for (int i = 0; i < st->n_rows; i++) {
		if (st->rows[i].gap_before > 0) {
			/* A group starts here: a thin line in the middle of the gap. */
			double ly = y + st->rows[i].gap_before / 2.0;
			set_rgba(cr, ms->border_color, 0.7);
			cairo_rectangle(cr, m + bw + 6, ly - 0.5, lw - 2 * bw - 12, 1);
			cairo_fill(cr);
			y += st->rows[i].gap_before;
		}
		if (i == st->hover || st->rows[i].emphasis) {
			struct rei_style hover = *rs;
			hover.shadow.opacity = 0;
			draw_box(cr, &hover, m + bw, y, lw - 2 * bw, row_h, rs->active_color, 1.0);
		}
		PangoLayout *l = make_layout(cr, font, st->rows[i].label);
		pango_layout_set_width(l, (int)((lw - 2 * MENU_PAD_X - 2 * bw) * PANGO_SCALE));
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
		int tw, th;
		pango_layout_get_pixel_size(l, &tw, &th);
		uint32_t text = (i == st->hover || st->rows[i].emphasis) ? rs->active_text_color
					       : st->rows[i].minimized ? rs->inactive_text_color : rs->text_color;
		set_color(cr, text);
		cairo_move_to(cr, m + bw + MENU_PAD_X, y + (row_h - th) / 2.0);
		pango_cairo_show_layout(cr, l);
		g_object_unref(l);
		y += row_h;
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

	struct rei_theme theme;
	resolve_theme(st, &theme);
	const struct rei_style *bar = &theme.e[REI_ELEM_TOOLBAR];
	const struct rei_style *wsb = &theme.e[REI_ELEM_WORKSPACE_BUTTON];

	bool translucent = bar->opacity < 0.999f;
	struct rei_pixbuf *pb = rei_pixbuf_create(lw * scale, lh * scale, !translucent);
	if (!pb)
		return NULL;

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, lw * scale, lh * scale);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);  /* Everything below is in logical pixels. */

	set_rgba(cr, bar->color, bar->opacity);
	cairo_paint(cr);

	PangoFontDescription *font = pango_font_description_from_string(st->font[0] ? st->font : "Sans 11");

	/* The Start button, at the very start of the bar. */
	const struct rei_style *sbs = &theme.e[REI_ELEM_START_BUTTON];
	int sw = st->start_w;
	if (sw > 0) {
		struct rei_rect cell = vertical ? (struct rei_rect){ 0, 0, lw, sw } : (struct rei_rect){ 0, 0, sw, lh };
		uint32_t fill = st->start_open ? sbs->active_color : sbs->color;
		if (st->start_hover && !st->start_open)
			fill = rei_bar_mix(fill, sbs->text_color, 0.15);
		draw_box(cr, sbs, cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 4, fill, 1.0);
		set_color(cr, st->start_open ? sbs->active_text_color : sbs->text_color);
		if (!vertical) {
			draw_centered(cr, font, st->start_label, cell.x + cell.w / 2.0, cell.y + cell.h / 2.0);
		} else {
			/* No room for the word on a narrow bar: the three-bar "menu" mark. */
			double cx = cell.x + cell.w / 2.0, cy = cell.y + cell.h / 2.0;
			for (int k = -1; k <= 1; k++) {
				cairo_rectangle(cr, cx - 6, cy + k * 5 - 1, 12, 2);
				cairo_fill(cr);
			}
		}
	}

	/* The tiled/floating switch: a small square. A grid of panes when new windows tile; two
	 * overlapping windows, lit, when they float. */
	int mw = st->mode_w;
	if (mw > 0) {
		struct rei_rect cell = vertical ? (struct rei_rect){ 0, sw, lw, mw } : (struct rei_rect){ sw, 0, mw, lh };
		double side = fmin(cell.w, cell.h) - 12;
		double bx = cell.x + (cell.w - side) / 2.0, by = cell.y + (cell.h - side) / 2.0;
		uint32_t fill = st->mode_float ? sbs->active_color : sbs->color;
		if (st->mode_hover)
			fill = rei_bar_mix(fill, sbs->text_color, 0.15);
		draw_box(cr, sbs, bx, by, side, side, fill, 1.0);
		set_color(cr, st->mode_float ? sbs->active_text_color : sbs->text_color);
		double in = side * 0.22, u = side - 2 * in;
		if (!st->mode_float) {   /* tiled: a 2x2 grid */
			double g = 1.5, c = (u - g) / 2.0;
			for (int a = 0; a < 2; a++)
				for (int b = 0; b < 2; b++) {
					cairo_rectangle(cr, bx + in + a * (c + g), by + in + b * (c + g), c, c);
					cairo_fill(cr);
				}
		} else {   /* floating: two overlapping windows */
			double w = u * 0.65;
			cairo_set_line_width(cr, 1.5);
			cairo_rectangle(cr, bx + in + 0.75, by + in + 0.75, w, w);
			cairo_stroke(cr);
			cairo_rectangle(cr, bx + in + u - w, by + in + u - w, w, w);
			cairo_fill(cr);
		}
	}
	sw += mw;

	/* Workspace buttons, after it. */
	struct rei_rect barr = vertical ? (struct rei_rect){ 0, sw, lw, lh - sw } : (struct rei_rect){ sw, 0, lw - sw, lh };
	for (int i = 0; i < st->n_buttons; i++) {
		const struct rei_bar_button *b = &st->buttons[i];
		struct rei_rect cell = rei_bar_button_rect(barr, st->position, i);

		bool filled = true;
		uint32_t fill;
		if (b->kind == REI_BAR_BUTTON_FOCUSED)
			fill = wsb->active_color;
		else if (b->kind == REI_BAR_BUTTON_VISIBLE)
			fill = wsb->inactive_color;
		else if (i == st->hover)
			fill = rei_bar_mix(wsb->color, wsb->text_color, 0.18);
		else
			filled = false;
		/* Hovering lightens any button a little. */
		if (filled && i == st->hover && b->kind != REI_BAR_BUTTON_HIDDEN)
			fill = rei_bar_mix(fill, wsb->text_color, 0.15);

		if (filled)
			draw_box(cr, wsb, cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 4, fill, 1.0);

		char label[16];
		snprintf(label, sizeof label, "%d", b->number);
		set_color(cr, b->kind == REI_BAR_BUTTON_FOCUSED ? wsb->active_text_color : wsb->text_color);
		draw_centered(cr, font, label, cell.x + cell.w / 2.0, cell.y + cell.h / 2.0);
	}

	draw_tray(cr, font, st, &theme);
	if (st->battery_present)
		draw_battery(cr, font, st, &theme);

	/* The volume, just before the clock. */
	if (st->volume[0]) {
		double vx, vy, vw, vh;
		volume_layout(cr, font, st, &vx, &vy, &vw, &vh);
		set_color(cr, st->volume_muted ? bar->inactive_text_color : bar->text_color);
		PangoLayout *layout = make_layout(cr, font, st->volume);
		cairo_move_to(cr, vx, vy);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}

	/* The clock, at the end. A vertical bar is too narrow for "14:32", so it is
	 * broken into stacked parts ("14" over "32"). */
	if (st->clock[0]) {
		set_color(cr, bar->text_color);
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
