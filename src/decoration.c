#include "decoration.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Geometry                                                                 */
/* ------------------------------------------------------------------------ */

void rei_notch_geometry(struct rei_rect w, int h, struct rei_notch_geometry *g)
{
	memset(g, 0, sizeof *g);
	if (h <= 0)
		return;

	g->has_buttons = w.w >= REI_NOTCH_BUTTONS_W;
	if (g->has_buttons) {
		g->buttons = (struct rei_rect){ w.x + w.w - REI_NOTCH_BUTTONS_W, w.y - h,
						REI_NOTCH_BUTTONS_W, h };
		g->minimize = (struct rei_rect){ g->buttons.x, g->buttons.y, REI_NOTCH_BUTTON_W, h };
		g->maximize = (struct rei_rect){ g->buttons.x + REI_NOTCH_BUTTON_W, g->buttons.y,
						 REI_NOTCH_BUTTON_W, h };
		g->close = (struct rei_rect){ g->buttons.x + 2 * REI_NOTCH_BUTTON_W, g->buttons.y,
					      REI_NOTCH_BUTTON_W, h };
	}

	/* A little air between the two tabs, so they never touch or overlap. */
	int handle_room = w.w - (g->has_buttons ? REI_NOTCH_BUTTONS_W : 0) - 8;
	g->has_handle = handle_room >= REI_NOTCH_HANDLE_W;
	if (g->has_handle)
		g->handle = (struct rei_rect){ w.x, w.y - h, REI_NOTCH_HANDLE_W, h };
}

static bool inside(const struct rei_rect *r, double x, double y)
{
	return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

enum rei_notch_part rei_notch_hit(const struct rei_notch_geometry *g, double x, double y)
{
	if (g->has_buttons) {
		if (inside(&g->close, x, y)) return REI_NOTCH_CLOSE;
		if (inside(&g->maximize, x, y)) return REI_NOTCH_MAXIMIZE;
		if (inside(&g->minimize, x, y)) return REI_NOTCH_MINIMIZE;
	}
	if (g->has_handle && inside(&g->handle, x, y))
		return REI_NOTCH_HANDLE;
	return REI_NOTCH_NONE;
}

/* ------------------------------------------------------------------------ */
/* Artwork                                                                  */
/* ------------------------------------------------------------------------ */

struct color {
	float r, g, b, a;  /* straight (not premultiplied) */
};

static const struct color BG = { 12 / 255.0f, 16 / 255.0f, 24 / 255.0f, 0.94f };
static const struct color BG_HOVER = { 56 / 255.0f, 68 / 255.0f, 92 / 255.0f, 0.97f };
static const struct color BG_CLOSE = { 216 / 255.0f, 64 / 255.0f, 56 / 255.0f, 0.98f };
static const struct color ICON = { 214 / 255.0f, 221 / 255.0f, 232 / 255.0f, 1.0f };
static const struct color ICON_ON_CLOSE = { 1.0f, 1.0f, 1.0f, 1.0f };

#define CORNER_RADIUS 8.0f
#define SUB 4  /* Supersampling: SUB x SUB samples per pixel. */

/* Inside a rectangle w x h whose top corners are rounded (the bottom edge meets
 * the window, so it stays square). Coordinates in logical pixels. */
static bool in_tab_shape(float x, float y, float w, float h)
{
	if (x < 0 || y < 0 || x >= w || y >= h)
		return false;
	float r = CORNER_RADIUS < h ? CORNER_RADIUS : h;
	float cx = -1, cy = r;
	if (x < r) cx = r;
	else if (x > w - r) cx = w - r;
	if (cx >= 0 && y < r) {
		float dx = x - cx, dy = y - cy;
		return dx * dx + dy * dy <= r * r;
	}
	return true;
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
	float vx = bx - ax, vy = by - ay;
	float t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
	return sqrtf(dx * dx + dy * dy);
}

/* Is the point on the icon strokes of a cell? (x, y) are relative to the cell
 * centre, in logical pixels. */
static bool on_icon(enum rei_notch_part kind, float x, float y)
{
	const float half = 0.85f;  /* half the stroke width */
	switch (kind) {
	case REI_NOTCH_CLOSE:
		return seg_dist(x, y, -4, -4, 4, 4) < half || seg_dist(x, y, -4, 4, 4, -4) < half;
	case REI_NOTCH_MAXIMIZE: {
		float d = fmaxf(fabsf(x), fabsf(y));  /* square outline, 9x9 */
		return d <= 4.5f + half && d >= 4.5f - half;
	}
	case REI_NOTCH_MINIMIZE:
		return seg_dist(x, y, -4.5f, 3.5f, 4.5f, 3.5f) < half;
	default:
		return false;
	}
}

/* The drag handle: a grid of dots, two across and three down. */
static bool on_grip(float x, float y)
{
	for (int cx = -1; cx <= 1; cx += 2) {
		for (int cy = -1; cy <= 1; cy++) {
			float dx = x - (float)cx * 3.0f, dy = y - (float)cy * 4.0f;
			if (dx * dx + dy * dy <= 1.35f * 1.35f)
				return true;
		}
	}
	return false;
}

/* Writes one premultiplied BGRA pixel from coverage of the tab shape (`cov`),
 * of its icon (`icon_cov`, a subset), and the colours involved. */
static void put_pixel(uint8_t *p, float cov, float icon_cov, struct color bg, struct color icon)
{
	float bg_only = cov - icon_cov;
	float a = icon_cov * icon.a + bg_only * bg.a;
	float r = icon_cov * icon.a * icon.r + bg_only * bg.a * bg.r;
	float g = icon_cov * icon.a * icon.g + bg_only * bg.a * bg.g;
	float b = icon_cov * icon.a * icon.b + bg_only * bg.a * bg.b;
#define BYTE(v) ((uint8_t)(fminf(fmaxf((v) * 255.0f + 0.5f, 0.0f), 255.0f)))
	p[0] = BYTE(b);
	p[1] = BYTE(g);
	p[2] = BYTE(r);
	p[3] = BYTE(a);
#undef BYTE
}

/* Renders a tab `w` x `h` logical pixels at `scale`. `cell_of` maps a logical x
 * to (kind, cell centre x) so each cell can have its own icon and hover colour. */
struct cell {
	enum rei_notch_part kind;
	float x0, x1;  /* Horizontal extent, logical. */
};

static struct rei_pixbuf *render_tab(int w, int h, int scale, const struct cell *cells, int n_cells,
				     enum rei_notch_part hover, bool grip)
{
	struct rei_pixbuf *pb = rei_pixbuf_create(w * scale, h * scale, false);
	if (!pb)
		return NULL;

	for (int py = 0; py < h * scale; py++) {
		for (int px = 0; px < w * scale; px++) {
			int in_shape = 0, in_icon = 0;
			const struct cell *cell = NULL;
			float cell_cx = 0;
			for (int sy = 0; sy < SUB; sy++) {
				for (int sx = 0; sx < SUB; sx++) {
					float x = ((float)px + ((float)sx + 0.5f) / SUB) / (float)scale;
					float y = ((float)py + ((float)sy + 0.5f) / SUB) / (float)scale;
					if (!in_tab_shape(x, y, (float)w, (float)h))
						continue;
					in_shape++;
					for (int i = 0; i < n_cells; i++) {
						if (x >= cells[i].x0 && x < cells[i].x1) {
							cell = &cells[i];
							cell_cx = (cells[i].x0 + cells[i].x1) / 2.0f;
							break;
						}
					}
					float cy = (float)h / 2.0f;
					bool icon = grip ? on_grip(x - (float)w / 2.0f, y - cy)
							 : cell && on_icon(cell->kind, x - cell_cx, y - cy);
					in_icon += icon;
				}
			}
			float total = (float)(SUB * SUB);
			struct color bg = BG, ic = ICON;
			/* Colour by the cell the pixel centre is in. */
			float xc = ((float)px + 0.5f) / (float)scale;
			for (int i = 0; i < n_cells; i++) {
				if (xc >= cells[i].x0 && xc < cells[i].x1 && cells[i].kind == hover) {
					bg = cells[i].kind == REI_NOTCH_CLOSE ? BG_CLOSE : BG_HOVER;
					if (cells[i].kind == REI_NOTCH_CLOSE)
						ic = ICON_ON_CLOSE;
				}
			}
			if (grip && hover == REI_NOTCH_HANDLE)
				bg = BG_HOVER;
			put_pixel(pb->data + ((size_t)py * (size_t)pb->width + (size_t)px) * 4,
				  (float)in_shape / total, (float)in_icon / total, bg, ic);
		}
	}
	return pb;
}

#define MAX_SCALE 4
#define N_VARIANTS 5  /* indexed by rei_notch_part */

static struct rei_pixbuf *button_cache[REI_NOTCH_MAX_HEIGHT + 1][MAX_SCALE + 1][N_VARIANTS];
static struct rei_pixbuf *handle_cache[REI_NOTCH_MAX_HEIGHT + 1][MAX_SCALE + 1][2];

struct rei_pixbuf *rei_notch_art_buttons(int height, int scale, enum rei_notch_part hover)
{
	if (height <= 0 || height > REI_NOTCH_MAX_HEIGHT)
		return NULL;
	if (scale < 1) scale = 1;
	if (scale > MAX_SCALE) scale = MAX_SCALE;
	if (hover != REI_NOTCH_MINIMIZE && hover != REI_NOTCH_MAXIMIZE && hover != REI_NOTCH_CLOSE)
		hover = REI_NOTCH_NONE;

	struct rei_pixbuf **slot = &button_cache[height][scale][hover];
	if (!*slot) {
		const struct cell cells[3] = {
			{ REI_NOTCH_MINIMIZE, 0, REI_NOTCH_BUTTON_W },
			{ REI_NOTCH_MAXIMIZE, REI_NOTCH_BUTTON_W, 2 * REI_NOTCH_BUTTON_W },
			{ REI_NOTCH_CLOSE, 2 * REI_NOTCH_BUTTON_W, 3 * REI_NOTCH_BUTTON_W },
		};
		*slot = render_tab(REI_NOTCH_BUTTONS_W, height, scale, cells, 3, hover, false);
	}
	return *slot;
}

struct rei_pixbuf *rei_notch_art_handle(int height, int scale, enum rei_notch_part hover)
{
	if (height <= 0 || height > REI_NOTCH_MAX_HEIGHT)
		return NULL;
	if (scale < 1) scale = 1;
	if (scale > MAX_SCALE) scale = MAX_SCALE;
	int variant = hover == REI_NOTCH_HANDLE ? 1 : 0;

	struct rei_pixbuf **slot = &handle_cache[height][scale][variant];
	if (!*slot) {
		const struct cell cells[1] = { { REI_NOTCH_HANDLE, 0, REI_NOTCH_HANDLE_W } };
		*slot = render_tab(REI_NOTCH_HANDLE_W, height, scale, cells, 1,
				   variant ? REI_NOTCH_HANDLE : REI_NOTCH_NONE, true);
	}
	return *slot;
}

struct rei_pixbuf *rei_drop_hint_art(void)
{
	static struct rei_pixbuf *hint;
	if (!hint) {
		hint = rei_pixbuf_create(1, 1, false);
		if (hint) {
			/* Translucent blue, premultiplied: 45% opaque. */
			hint->data[0] = (uint8_t)(0.45f * 230);  /* B */
			hint->data[1] = (uint8_t)(0.45f * 150);  /* G */
			hint->data[2] = (uint8_t)(0.45f * 70);   /* R */
			hint->data[3] = (uint8_t)(0.45f * 255);  /* A */
		}
	}
	return hint;
}
