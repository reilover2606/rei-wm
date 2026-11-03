#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "config.h"
#include "toolbar.h"

static struct rei_rect R(int x, int y, int w, int h) { return (struct rei_rect){ x, y, w, h }; }

static void rect_eq(struct rei_rect a, int x, int y, int w, int h)
{
	if (a.x != x || a.y != y || a.w != w || a.h != h) {
		fprintf(stderr, "rect (%d,%d %dx%d), expected (%d,%d %dx%d)\n", a.x, a.y, a.w, a.h, x, y, w, h);
		exit(1);
	}
}

static void test_geometry(void)
{
	struct rei_rect out = R(100, 50, 800, 600);

	rect_eq(rei_bar_rect(out, REI_BAR_TOP, 28), 100, 50, 800, 28);
	rect_eq(rei_bar_usable(out, REI_BAR_TOP, 28), 100, 78, 800, 572);
	rect_eq(rei_bar_rect(out, REI_BAR_BOTTOM, 28), 100, 622, 800, 28);
	rect_eq(rei_bar_usable(out, REI_BAR_BOTTOM, 28), 100, 50, 800, 572);
	rect_eq(rei_bar_rect(out, REI_BAR_LEFT, 28), 100, 50, 28, 600);
	rect_eq(rei_bar_usable(out, REI_BAR_LEFT, 28), 128, 50, 772, 600);
	rect_eq(rei_bar_rect(out, REI_BAR_RIGHT, 28), 872, 50, 28, 600);
	rect_eq(rei_bar_usable(out, REI_BAR_RIGHT, 28), 100, 50, 772, 600);

	/* The bar and what is left for windows always partition the output exactly. */
	for (int pos = 0; pos < 4; pos++) {
		for (int t = 16; t <= 96; t += 8) {
			struct rei_rect b = rei_bar_rect(out, (enum rei_bar_position)pos, t);
			struct rei_rect u = rei_bar_usable(out, (enum rei_bar_position)pos, t);
			CHECK(b.w * b.h + u.w * u.h == out.w * out.h);
			bool disjoint = b.x + b.w <= u.x || u.x + u.w <= b.x || b.y + b.h <= u.y || u.y + u.h <= b.y;
			CHECK(disjoint);
			CHECK(b.x >= out.x && b.y >= out.y && b.x + b.w <= out.x + out.w && b.y + b.h <= out.y + out.h);
			CHECK(u.x >= out.x && u.y >= out.y && u.x + u.w <= out.x + out.w && u.y + u.h <= out.y + out.h);
		}
	}
	CHECK(!rei_bar_vertical(REI_BAR_TOP) && !rei_bar_vertical(REI_BAR_BOTTOM));
	CHECK(rei_bar_vertical(REI_BAR_LEFT) && rei_bar_vertical(REI_BAR_RIGHT));

	/* Buttons are squares laid from the start: left to right, or top to bottom. */
	struct rei_rect top = rei_bar_rect(out, REI_BAR_TOP, 28);
	rect_eq(rei_bar_button_rect(top, REI_BAR_TOP, 0), 100, 50, 28, 28);
	rect_eq(rei_bar_button_rect(top, REI_BAR_TOP, 2), 156, 50, 28, 28);
	struct rei_rect right = rei_bar_rect(out, REI_BAR_RIGHT, 28);
	rect_eq(rei_bar_button_rect(right, REI_BAR_RIGHT, 0), 872, 50, 28, 28);
	rect_eq(rei_bar_button_rect(right, REI_BAR_RIGHT, 3), 872, 134, 28, 28);

	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 3, 110, 60) == 0);
	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 3, 128, 60) == 1);    /* first pixel of button 1 */
	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 3, 127.9, 60) == 0);
	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 3, 183, 77) == 2);
	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 3, 184, 60) == -1);   /* past the last button */
	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 3, 110, 78) == -1);   /* below the bar */
	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 3, 110, 49) == -1);   /* above it */
	CHECK(rei_bar_button_at(top, REI_BAR_TOP, 0, 110, 60) == -1);   /* no buttons at all */
	CHECK(rei_bar_button_at(right, REI_BAR_RIGHT, 5, 880, 100) == 1);
	CHECK(rei_bar_button_at(right, REI_BAR_RIGHT, 5, 871, 100) == -1);
}

static void test_mix(void)
{
	CHECK(rei_bar_mix(0x000000, 0xffffff, 0.0) == 0x000000);
	CHECK(rei_bar_mix(0x000000, 0xffffff, 1.0) == 0xffffff);
	CHECK(rei_bar_mix(0x000000, 0xffffff, 0.5) == 0x808080);
	CHECK(rei_bar_mix(0xff0000, 0x0000ff, 0.5) == 0x800080);
	CHECK(rei_bar_mix(0x102030, 0x102030, 0.7) == 0x102030);
}

#define BG 0x161c26
#define FG 0xd6dde8
#define HL 0x3c6ebe

static struct rei_bar_state base_state(enum rei_bar_position pos, int length)
{
	struct rei_bar_state st;
	memset(&st, 0, sizeof st);
	st.position = pos;
	st.thickness = 28;
	st.length = length;
	st.scale = 1;
	st.hover = -1;
	snprintf(st.font, sizeof st.font, "Sans 11");
	st.background = BG;
	st.foreground = FG;
	st.highlight = HL;
	st.n_buttons = 3;
	st.buttons[0] = (struct rei_bar_button){ 1, REI_BAR_BUTTON_FOCUSED };
	st.buttons[1] = (struct rei_bar_button){ 2, REI_BAR_BUTTON_VISIBLE };
	st.buttons[2] = (struct rei_bar_button){ 3, REI_BAR_BUTTON_HIDDEN };
	return st;
}

static uint32_t pixel(const struct rei_pixbuf *p, int x, int y)
{
	const uint8_t *d = p->data + ((size_t)y * (size_t)p->width + (size_t)x) * 4;  /* B G R A */
	CHECK(d[3] == 255);
	return (uint32_t)d[2] << 16 | (uint32_t)d[1] << 8 | d[0];
}

static void expect_color(uint32_t got, uint32_t want, const char *what)
{
	for (int shift = 16; shift >= 0; shift -= 8) {
		int g = (int)((got >> shift) & 0xff), w = (int)((want >> shift) & 0xff);
		if (abs(g - w) > 2) {
			fprintf(stderr, "%s: got #%06x, expected #%06x\n", what, got, want);
			exit(1);
		}
	}
}

/* How many pixels differ between two renderings within a rectangle. */
static int diff_pixels(const struct rei_pixbuf *a, const struct rei_pixbuf *b, int x0, int y0, int x1, int y1)
{
	int n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			n += pixel(a, x, y) != pixel(b, x, y);
	return n;
}

static void test_render(void)
{
	struct rei_bar_state st = base_state(REI_BAR_TOP, 300);
	struct rei_pixbuf *pb = rei_bar_render(&st);
	CHECK(pb && pb->width == 300 && pb->height == 28 && pb->opaque);

	/* The three kinds of button, away from their numbers. */
	expect_color(pixel(pb, 4, 4), HL, "focused workspace");
	expect_color(pixel(pb, 28 + 4, 4), rei_bar_mix(BG, HL, 0.45), "visible on another output");
	expect_color(pixel(pb, 56 + 4, 4), BG, "hidden workspace has no fill");
	expect_color(pixel(pb, 150, 14), BG, "empty bar");
	/* Rounded corners: the extreme corner of a button's fill is not painted. */
	expect_color(pixel(pb, 2, 2), BG, "button corner is rounded");
	rei_pixbuf_unref(pb);

	/* Hover lightens a hidden button, and any other a little. */
	st.hover = 2;
	pb = rei_bar_render(&st);
	expect_color(pixel(pb, 56 + 4, 4), rei_bar_mix(BG, FG, 0.18), "hovered hidden workspace");
	expect_color(pixel(pb, 4, 4), HL, "unhovered buttons unchanged");
	rei_pixbuf_unref(pb);
	st.hover = 0;
	pb = rei_bar_render(&st);
	expect_color(pixel(pb, 4, 4), rei_bar_mix(HL, FG, 0.15), "hovered focused workspace");
	rei_pixbuf_unref(pb);
	st.hover = -1;

	/* The clock is drawn at the end (right) of a horizontal bar, nowhere else. */
	struct rei_pixbuf *plain = rei_bar_render(&st);
	snprintf(st.clock, sizeof st.clock, "88:88");
	struct rei_pixbuf *clocked = rei_bar_render(&st);
	int in_clock = diff_pixels(plain, clocked, 200, 0, 300, 28);
	if (in_clock == 0) {
		fprintf(stderr, "test_toolbar: no usable font here, skipping text checks\n");
	} else {
		CHECK(in_clock > 20);
		CHECK(diff_pixels(plain, clocked, 0, 0, 200, 28) == 0);   /* nothing drawn left of it */
		/* Right-aligned: the ink touches the right padding, not the edge. */
		CHECK(diff_pixels(plain, clocked, 292, 0, 300, 28) == 0);
	}
	rei_pixbuf_unref(plain);
	rei_pixbuf_unref(clocked);

	/* The numbers are drawn inside their buttons. */
	struct rei_bar_state nums = base_state(REI_BAR_TOP, 300);
	nums.n_buttons = 3;
	struct rei_pixbuf *with = rei_bar_render(&nums);
	for (int i = 0; i < 3; i++)
		nums.buttons[i].number = 8 + i;
	struct rei_pixbuf *other = rei_bar_render(&nums);
	if (diff_pixels(with, other, 0, 0, 84, 28) > 0)
		CHECK(diff_pixels(with, other, 84, 0, 300, 28) == 0);  /* only the buttons changed */
	rei_pixbuf_unref(with);
	rei_pixbuf_unref(other);

	/* A vertical bar: buttons run down from the top, the clock stacks at the bottom. */
	struct rei_bar_state v = base_state(REI_BAR_LEFT, 240);
	pb = rei_bar_render(&v);
	CHECK(pb && pb->width == 28 && pb->height == 240);
	expect_color(pixel(pb, 4, 4), HL, "first button, top of a vertical bar");
	expect_color(pixel(pb, 4, 28 + 4), rei_bar_mix(BG, HL, 0.45), "second button below it");
	expect_color(pixel(pb, 4, 84 + 10), BG, "below the buttons");
	snprintf(v.clock, sizeof v.clock, "14:32");
	struct rei_pixbuf *vc = rei_bar_render(&v);
	int stacked = diff_pixels(pb, vc, 0, 150, 28, 240);
	if (stacked > 0) {
		CHECK(stacked > 20);
		CHECK(diff_pixels(pb, vc, 0, 0, 28, 150) == 0);   /* at the end, not the start */
	}
	rei_pixbuf_unref(pb);
	rei_pixbuf_unref(vc);

	/* High-density outputs get a proportionally larger, sharp image. */
	struct rei_bar_state hd = base_state(REI_BAR_BOTTOM, 300);
	hd.scale = 2;
	pb = rei_bar_render(&hd);
	CHECK(pb && pb->width == 600 && pb->height == 56);
	expect_color(pixel(pb, 8, 8), HL, "scale 2");
	rei_pixbuf_unref(pb);

	/* Degenerate sizes are refused rather than crashing. */
	struct rei_bar_state bad = base_state(REI_BAR_TOP, 0);
	CHECK(rei_bar_render(&bad) == NULL);
}

static struct rei_config *parse_ok(const char *text)
{
	char err[300];
	struct rei_config *c = rei_config_parse(text, err, sizeof err);
	if (!c)
		fprintf(stderr, "unexpected config error: %s\n", err);
	CHECK(c);
	return c;
}

static void parse_fails(const char *text, const char *expect)
{
	char err[300];
	struct rei_config *c = rei_config_parse(text, err, sizeof err);
	if (c || !strstr(err, expect)) {
		fprintf(stderr, "config:\n%s\nexpected error containing '%s', got '%s'\n", text, expect,
			c ? "(success)" : err);
		exit(1);
	}
}

static void test_config(void)
{
	struct rei_config *c = rei_config_defaults();
	CHECK(c->toolbar_enabled && c->toolbar_position == REI_BAR_TOP && c->toolbar_thickness == 28);
	CHECK(!strcmp(c->toolbar_font, "Sans 11") && !strcmp(c->toolbar_clock_format, "%H:%M:%S"));
	CHECK(c->toolbar_background == 0x161c26 && c->toolbar_foreground == 0xd6dde8 &&
	      c->toolbar_highlight == 0x3c6ebe);
	rei_config_free(c);

	/* The first-run file says exactly the defaults. */
	c = parse_ok(rei_config_default_text());
	CHECK(c->toolbar_enabled && c->toolbar_position == REI_BAR_TOP && c->toolbar_thickness == 28);
	CHECK(!strcmp(c->toolbar_clock_format, "%H:%M:%S"));
	rei_config_free(c);

	c = parse_ok("[toolbar]\nenabled = false\nposition = \"left\"\nthickness = 40\n"
		     "font = \"Monospace 9\"\nclock_format = \"\"\nbackground = \"#000000\"\n"
		     "foreground = \"#FFFFFF\"\nhighlight = \"#ff0000\"\n");
	CHECK(!c->toolbar_enabled && c->toolbar_position == REI_BAR_LEFT && c->toolbar_thickness == 40);
	CHECK(!strcmp(c->toolbar_font, "Monospace 9") && c->toolbar_clock_format[0] == '\0');
	CHECK(c->toolbar_background == 0 && c->toolbar_foreground == 0xffffff && c->toolbar_highlight == 0xff0000);
	rei_config_free(c);

	const char *positions[] = { "top", "bottom", "left", "right" };
	for (int i = 0; i < 4; i++) {
		char text[100];
		snprintf(text, sizeof text, "[toolbar]\nposition = \"%s\"\n", positions[i]);
		c = parse_ok(text);
		CHECK((int)c->toolbar_position == i);
		rei_config_free(c);
	}

	parse_fails("[toolbar]\nposition = \"middle\"\n", "toolbar.position");
	parse_fails("[toolbar]\nthickness = 8\n", "toolbar.thickness");
	parse_fails("[toolbar]\nthickness = 200\n", "toolbar.thickness");
	parse_fails("[toolbar]\nenabled = \"yes\"\n", "toolbar.enabled");
	parse_fails("[toolbar]\nbackground = \"blue\"\n", "toolbar.background");
	parse_fails("[toolbar]\nhighlight = \"#12345\"\n", "toolbar.highlight");
	parse_fails("[toolbar]\nfont = 12\n", "toolbar.font");
}

static struct rei_bar_state tray_state(enum rei_bar_position pos, int length, int n_items, int counts[])
{
	struct rei_bar_state st;
	memset(&st, 0, sizeof st);
	st.position = pos;
	st.thickness = 28;
	st.length = length;
	st.scale = 1;
	snprintf(st.font, sizeof st.font, "Sans 11");
	snprintf(st.clock, sizeof st.clock, "12:34:56");
	st.n_buttons = 2;
	st.hover = -1;
	st.tray_hover = -1;
	st.drag_index = -1;
	st.tray_min_percent = 50;
	st.tray_mode = REI_TRAY_ICONS_NAMES;
	st.n_tray = n_items;
	for (int i = 0; i < n_items; i++) {
		snprintf(st.tray[i].label, sizeof st.tray[i].label, "Window %d", i);
		st.tray[i].count = counts ? counts[i] : 1;
	}
	return st;
}

static void test_tray_layout(void)
{
	struct rei_tray_layout lay;

	/* Off, or nothing to show: no tiles. */
	struct rei_bar_state st = tray_state(REI_BAR_TOP, 1200, 3, NULL);
	st.tray_mode = REI_TRAY_OFF;
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.n_visible == 0 && !lay.has_overflow);
	st = tray_state(REI_BAR_TOP, 1200, 0, NULL);
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.n_visible == 0 && !lay.has_overflow);

	/* Three tiles with names on a wide bar: natural size, centred, inside the middle 3/5 (240..960). */
	st = tray_state(REI_BAR_TOP, 1200, 3, NULL);
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.n_visible == 3 && !lay.has_overflow && lay.drop_index == -1);
	int left = lay.tile[0].x, right = lay.tile[2].x + lay.tile[2].w;
	CHECK(abs((left + right) / 2 - 600) <= 2);
	for (int i = 0; i < 3; i++) {
		CHECK(lay.tile[i].y == 3 && lay.tile[i].h == 22 && lay.tile[i].w == 150);
		if (i)
			CHECK(lay.tile[i].x >= lay.tile[i - 1].x + lay.tile[i - 1].w + 2);
	}

	/* The range is the middle three fifths: eight tiles shrink to fit it, and stay inside. */
	st = tray_state(REI_BAR_TOP, 1200, 8, NULL);
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.n_visible == 8 && !lay.has_overflow);
	CHECK(lay.tile[0].w < 150 && lay.tile[0].w >= 75);
	CHECK(lay.tile[0].x >= 240 && lay.tile[7].x + lay.tile[7].w <= 960);

	/* Twelve do not fit even at half width: the newest go under an overflow tile. */
	st = tray_state(REI_BAR_TOP, 1200, 12, NULL);
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.has_overflow && lay.n_visible < 12 && lay.n_visible >= 6);
	CHECK(lay.tile[0].w == 75);                                   /* shrunk exactly to 50% */
	CHECK(lay.overflow_windows == (int)(12 - lay.n_visible));
	CHECK(lay.overflow.x >= lay.tile[lay.n_visible - 1].x + lay.tile[lay.n_visible - 1].w);
	CHECK(lay.overflow.x >= 240 && lay.overflow.x + lay.overflow.w <= 960);

	/* A different floor: with 100% tiles cannot shrink, so overflow starts sooner. */
	st.tray_min_percent = 100;
	size_t full_floor = (rei_bar_tray_layout(&st, &lay), lay.n_visible);
	CHECK(lay.has_overflow && full_floor < 6 && lay.tile[0].w == 150);

	/* Windows beyond the list's capacity are always under the overflow tile. */
	st = tray_state(REI_BAR_TOP, 1200, 2, NULL);
	st.tray_extra_windows = 3;
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.n_visible == 2 && lay.has_overflow && lay.overflow_windows == 3);

	/* Icon-only tiles: natural 30 px wide, and never below 18 px. */
	st = tray_state(REI_BAR_TOP, 1200, 3, NULL);
	st.tray_mode = REI_TRAY_ICONS;
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.tile[0].w == 30 && lay.tile[0].h == 22);
	st = tray_state(REI_BAR_TOP, 1200, 24, NULL);
	st.tray_mode = REI_TRAY_ICONS;
	rei_bar_tray_layout(&st, &lay);
	for (size_t i = 0; i < lay.n_visible; i++)
		CHECK(lay.tile[i].w >= 18);

	/* A stack of windows peeks out by 4 px per extra tile, up to four tiles. */
	int counts[3] = { 3, 1, 9 };
	st = tray_state(REI_BAR_TOP, 1200, 3, counts);
	st.tray_mode = REI_TRAY_ICONS;
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.tile[0].w == 30 + 2 * 4 && lay.tile[1].w == 30 && lay.tile[2].w == 30 + 3 * 4);

	/* Dragging: the grabbed tile follows the pointer; the others make room where it would land. */
	st = tray_state(REI_BAR_TOP, 1200, 3, NULL);
	rei_bar_tray_layout(&st, &lay);
	struct rei_rect natural[3] = { lay.tile[0], lay.tile[1], lay.tile[2] };
	st.drag_index = 0;
	st.drag_pos = natural[2].x + natural[2].w - 10;   /* near the far end of the last tile */
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.drop_index == 2);
	CHECK(lay.tile[1].x < natural[1].x && lay.tile[2].x < natural[2].x);          /* made room: shifted left */
	CHECK(abs(lay.tile[0].x + lay.tile[0].w / 2 - st.drag_pos) <= 2);             /* under the pointer */
	st.drag_pos = natural[0].x - 100;                                             /* back past the start */
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.drop_index == 0);
	CHECK(lay.tile[0].x >= 240);                                                  /* held inside the range */

	/* A vertical bar: tiles stacked down it, icon-only, centred along its length (range 160..640). */
	st = tray_state(REI_BAR_LEFT, 800, 3, NULL);
	rei_bar_tray_layout(&st, &lay);
	CHECK(lay.n_visible == 3);
	CHECK(lay.tile[0].x == 3 && lay.tile[0].w == 22 && lay.tile[0].h == 30);
	CHECK(lay.tile[1].y >= lay.tile[0].y + lay.tile[0].h && lay.tile[2].y >= lay.tile[1].y + lay.tile[1].h);
	CHECK(abs((lay.tile[0].y + lay.tile[2].y + lay.tile[2].h) / 2 - 400) <= 2);
	CHECK(lay.tile[0].y >= 160 && lay.tile[2].y + lay.tile[2].h <= 640);
}

static const uint8_t *px4(const struct rei_pixbuf *pb, int x, int y)
{
	return pb->data + ((size_t)y * (size_t)pb->width + (size_t)x) * 4;  /* B, G, R, A */
}

/* The theme drives the look: shape, opacity, shadow and colours of a tray tile and the bar. */
static void test_theme_render(void)
{
	struct rei_bar_state st = tray_state(REI_BAR_TOP, 1200, 1, NULL);
	st.tray_mode = REI_TRAY_ICONS;
	st.styled = true;
	rei_theme_defaults(&st.theme, 0x101010, 0xe0e0e0, 0x3060c0);
	struct rei_style *tile = &st.theme.e[REI_ELEM_TRAY_TILE];
	tile->color = 0xff0000;
	tile->shape = REI_SHAPE_SQUARE;
	tile->radius = 0;
	struct rei_tray_layout lay;
	rei_bar_tray_layout(&st, &lay);
	struct rei_rect t = lay.tile[0];

	/* Square corners: the corner pixel of the tile is the tile's colour. */
	struct rei_pixbuf *pb = rei_bar_render(&st);
	CHECK(pb && pb->opaque);
	const uint8_t *c = px4(pb, t.x, t.y);
	CHECK(c[2] == 255 && c[1] == 0 && c[0] == 0);
	rei_pixbuf_unref(pb);

	/* Rounded corners: the same pixel is the bar's background instead. */
	tile->shape = REI_SHAPE_ROUNDED;
	tile->radius = 8;
	pb = rei_bar_render(&st);
	c = px4(pb, t.x, t.y);
	CHECK(c[2] == 0x10 && c[1] == 0x10 && c[0] == 0x10);
	rei_pixbuf_unref(pb);

	/* A shadow darkens the bar just below the tile; without one it is untouched. */
	tile->shadow = (struct rei_shadow){ 0x000000, 0.0f, 0, 3, 6 };
	pb = rei_bar_render(&st);
	int below_y = t.y + t.h + 1;
	uint8_t plain = px4(pb, t.x + t.w / 2, below_y)[2];
	rei_pixbuf_unref(pb);
	tile->shadow.opacity = 0.8f;
	st.theme.e[REI_ELEM_TOOLBAR].color = 0x808080;   /* a light bar, so a shadow shows */
	pb = rei_bar_render(&st);
	CHECK(plain == 0x10);
	CHECK(px4(pb, t.x + t.w / 2, below_y)[2] < 0x70);
	rei_pixbuf_unref(pb);

	/* Bar transparency: a translucent bar is not opaque and carries partial alpha. */
	tile->shadow.opacity = 0;
	st.theme.e[REI_ELEM_TOOLBAR].opacity = 0.5f;
	pb = rei_bar_render(&st);
	CHECK(pb && !pb->opaque);
	CHECK(abs(px4(pb, 1000, 2)[3] - 128) <= 2);
	rei_pixbuf_unref(pb);

	/* Without a theme (styled = false) the legacy colours give the default look. */
	struct rei_bar_state plain_st = tray_state(REI_BAR_TOP, 1200, 0, NULL);
	plain_st.background = 0x204060;
	pb = rei_bar_render(&plain_st);
	c = px4(pb, 600, 14);
	CHECK(c[2] == 0x20 && c[1] == 0x40 && c[0] == 0x60);
	rei_pixbuf_unref(pb);
}

int main(void)
{
	test_geometry();
	test_mix();
	test_render();
	test_config();
	test_tray_layout();
	test_theme_render();
	rei_bar_render_cleanup();
	puts("test_toolbar: ok");
	return 0;
}
