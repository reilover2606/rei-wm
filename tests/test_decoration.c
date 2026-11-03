#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "decoration.h"

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
	struct rei_notch_geometry g;
	/* A window at (100,200) 400 wide: tabs end exactly at its top edge, y = 200. */
	rei_notch_geometry(R(100, 200, 400, 300), 22, &g);
	CHECK(g.has_buttons && g.has_handle);
	rect_eq(g.buttons, 100 + 400 - 84, 178, 84, 22);   /* right-aligned */
	rect_eq(g.minimize, 416, 178, 28, 22);
	rect_eq(g.maximize, 444, 178, 28, 22);
	rect_eq(g.close, 472, 178, 28, 22);                 /* rightmost, flush with the window */
	rect_eq(g.handle, 100, 178, 48, 22);                /* left-aligned */
	/* The tabs are entirely above the window: they take no space from it. */
	CHECK(g.buttons.y + g.buttons.h == 200 && g.handle.y + g.handle.h == 200);

	/* Hit-testing. */
	CHECK(rei_notch_hit(&g, 490, 190) == REI_NOTCH_CLOSE);
	CHECK(rei_notch_hit(&g, 460, 190) == REI_NOTCH_MAXIMIZE);
	CHECK(rei_notch_hit(&g, 420, 190) == REI_NOTCH_MINIMIZE);
	CHECK(rei_notch_hit(&g, 110, 190) == REI_NOTCH_HANDLE);
	CHECK(rei_notch_hit(&g, 300, 190) == REI_NOTCH_NONE);   /* between the tabs */
	CHECK(rei_notch_hit(&g, 490, 200) == REI_NOTCH_NONE);   /* the window itself is not a notch */
	CHECK(rei_notch_hit(&g, 490, 177) == REI_NOTCH_NONE);   /* above the tab */
	CHECK(rei_notch_hit(&g, 500, 190) == REI_NOTCH_NONE);   /* right edge is exclusive */
	CHECK(rei_notch_hit(&g, 472, 178) == REI_NOTCH_CLOSE);  /* top-left pixel of a cell */

	/* A narrow window drops the handle first, then everything. */
	rei_notch_geometry(R(0, 100, 120, 80), 22, &g);
	CHECK(g.has_buttons && !g.has_handle);
	CHECK(rei_notch_hit(&g, 5, 90) == REI_NOTCH_NONE);
	/* Too narrow for the buttons (84) but wide enough for the handle: the window
	 * can still be dragged. */
	rei_notch_geometry(R(0, 100, 83, 80), 22, &g);
	CHECK(!g.has_buttons && g.has_handle);
	CHECK(rei_notch_hit(&g, 80, 90) == REI_NOTCH_NONE);
	CHECK(rei_notch_hit(&g, 10, 90) == REI_NOTCH_HANDLE);
	rei_notch_geometry(R(0, 100, 55, 80), 22, &g);   /* handle (48) + 8 of air doesn't fit */
	CHECK(!g.has_buttons && !g.has_handle);

	/* Never overlapping or past the window's sides, whatever the width. */
	for (int w = 1; w < 400; w++) {
		rei_notch_geometry(R(50, 60, w, 100), 22, &g);
		if (g.has_buttons)
			CHECK(g.buttons.x >= 50 && g.buttons.x + g.buttons.w == 50 + w);
		if (g.has_handle) {
			CHECK(g.handle.x == 50 && g.handle.x + g.handle.w <= 50 + w);
			if (g.has_buttons)
				CHECK(g.handle.x + g.handle.w <= g.buttons.x);
		}
	}

	/* Height 0 disables the notches entirely. */
	rei_notch_geometry(R(0, 100, 400, 80), 0, &g);
	CHECK(!g.has_buttons && !g.has_handle);
	CHECK(rei_notch_hit(&g, 390, 99) == REI_NOTCH_NONE);
}

static const uint8_t *px(const struct rei_pixbuf *p, int x, int y)
{
	return p->data + ((size_t)y * (size_t)p->width + (size_t)x) * 4;  /* B G R A */
}

static void test_art(void)
{
	struct rei_pixbuf *b = rei_notch_art_buttons(22, 1, REI_NOTCH_NONE);
	CHECK(b && b->width == 84 && b->height == 22 && !b->opaque);

	/* Rounded top corners are transparent; the middle of the tab is solid dark. */
	CHECK(px(b, 0, 0)[3] == 0);
	CHECK(px(b, 83, 0)[3] == 0);
	CHECK(px(b, 40, 4)[3] > 220);
	/* The bottom edge is square and opaque: it meets the window flush. */
	CHECK(px(b, 0, 21)[3] > 220 && px(b, 83, 21)[3] > 220);
	/* Premultiplied: no channel exceeds alpha. */
	for (int y = 0; y < 22; y++)
		for (int x = 0; x < 84; x++) {
			const uint8_t *p = px(b, x, y);
			CHECK(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3]);
		}

	/* Icons are drawn light on the dark tab: the close cross passes through the
	 * cell centre (x = 70, y = 11), the maximize square's outline is at +-4.5. */
	CHECK(px(b, 70, 11)[2] > 150);
	CHECK(px(b, 42 + 4, 11)[2] > 120 || px(b, 42 + 5, 11)[2] > 120);  /* square's right side */
	CHECK(px(b, 14, 14)[2] > 120);                                       /* minimize's bar */
	CHECK(px(b, 56, 11)[2] < 60);                                        /* square's centre is empty */

	/* Hovering lights up the right cell and only that one. */
	struct rei_pixbuf *hc = rei_notch_art_buttons(22, 1, REI_NOTCH_CLOSE);
	CHECK(hc != b);
	CHECK(px(hc, 66, 4)[2] > 180 && px(hc, 66, 4)[1] < 100);  /* close turns red */
	CHECK(px(hc, 14, 4)[2] == px(b, 14, 4)[2]);                /* other cells unchanged */
	struct rei_pixbuf *hm = rei_notch_art_buttons(22, 1, REI_NOTCH_MAXIMIZE);
	CHECK(px(hm, 50, 4)[2] > px(b, 50, 4)[2] + 20);           /* highlighted */
	CHECK(px(hm, 66, 4)[2] == px(b, 66, 4)[2]);                /* close is not */

	/* Cached: the same request returns the same pixbuf. */
	CHECK(rei_notch_art_buttons(22, 1, REI_NOTCH_CLOSE) == hc);

	/* High-density outputs get a proportionally larger, still-sharp image. */
	struct rei_pixbuf *b2 = rei_notch_art_buttons(22, 2, REI_NOTCH_NONE);
	CHECK(b2->width == 168 && b2->height == 44);
	CHECK(px(b2, 0, 0)[3] == 0 && px(b2, 80, 10)[3] > 220);

	/* The handle: dots on a dark tab, lighter when hovered. */
	struct rei_pixbuf *h = rei_notch_art_handle(22, 1, REI_NOTCH_NONE);
	struct rei_pixbuf *hh = rei_notch_art_handle(22, 1, REI_NOTCH_HANDLE);
	CHECK(h && h->width == 48 && hh != h);
	CHECK(px(h, 21, 7)[2] > 150);          /* a grip dot at (-3,-4) from the centre */
	CHECK(px(h, 24, 11)[2] < 60);          /* gap between dot columns */
	CHECK(px(hh, 10, 11)[2] > px(h, 10, 11)[2] + 20);

	/* Unreasonable sizes are refused rather than crashing. */
	CHECK(rei_notch_art_buttons(0, 1, 0) == NULL);
	CHECK(rei_notch_art_buttons(1000, 1, 0) == NULL);

	struct rei_pixbuf *hint = rei_drop_hint_art();
	CHECK(hint && hint->data[3] > 90 && hint->data[3] < 140);
}

int main(void)
{
	test_geometry();
	test_art();
	puts("test_decoration: ok");
	return 0;
}
