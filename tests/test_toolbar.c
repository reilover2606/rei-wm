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

int main(void)
{
	test_geometry();
	test_mix();
	test_render();
	test_config();
	rei_bar_render_cleanup();
	puts("test_toolbar: ok");
	return 0;
}
