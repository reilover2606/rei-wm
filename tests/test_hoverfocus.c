/*
 * Focus by hovering does not raise a floating window; clicking does. (A small dialog, like the colour picker of
 * the settings app, must not get covered by its parent just because the pointer passed over the parent.)
 *
 * Two floating windows overlap: A (red) and B (green, opened later, so on top). The pointer is moved over A's
 * uncovered part: A has the focus, and B is still in front. A click on A brings it to the front.
 */
#define SOCKET "rei-hoverfocus-test"
#include "testlib.h"

/* The bounding box of the pixels of a colour. */
static bool bbox(const struct ppm *img, int r, int g, int b, int *x0, int *y0, int *x1, int *y1)
{
	*x0 = img->w;
	*y0 = img->h;
	*x1 = *y1 = -1;
	for (int y = 0; y < img->h; y++)
		for (int x = 0; x < img->w; x++)
			if (near_px(img, x, y, r, g, b, 2)) {
				*x0 = x < *x0 ? x : *x0;
				*x1 = x > *x1 ? x : *x1;
				*y0 = y < *y0 ? y : *y0;
				*y1 = y > *y1 ? y : *y1;
			}
	return *x1 >= 0;
}

static bool is(int x, int y, int r, int g, int b)
{
	struct ppm img = screenshot();
	bool ok = near_px(&img, x, y, r, g, b, 4);
	free(img.rgb);
	return ok;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-hoverfocus-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	char config[400];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n[general]\nfocus_follows_mouse = true\n[layout]\nnew_windows = \"float\"\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	client_bind_keyboard(&c);
	park_pointer();

	struct window a, b;
	open_win(&c, &a, "A", 0xffff0000);
	open_win(&c, &b, "B", 0xff00ff00);
	park_pointer();   /* (over nothing: nothing is focused) */

	struct ppm img = screenshot();
	int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
	CHECK(bbox(&img, 255, 0, 0, &ax0, &ay0, &ax1, &ay1));
	CHECK(bbox(&img, 0, 255, 0, &bx0, &by0, &bx1, &by1));
	free(img.rgb);
	fprintf(stderr, "A (visible red) %d,%d-%d,%d; B %d,%d-%d,%d\n", ax0, ay0, ax1, ay1, bx0, by0, bx1, by1);
	/* B is over A's corner: the cascade offsets them. */
	CHECK(bx0 > ax0 || by0 > ay0);

	/* A point of A that B does not cover (and one both would cover, were A in front: where they overlap). */
	int ox = bx0 + 3, oy = by0 + 3;   /* inside B, and inside A's rectangle if they overlap there */
	int px = ax0 + 3, py = ay0 + 3;   /* A's own corner, uncovered */
	CHECK(is(px, py, 255, 0, 0));
	CHECK(is(ox, oy, 0, 255, 0));

	/* Hover over A: it is focused ... */
	inject("motion_abs %d %d", px, py);
	settle_all();
	CHECK(a.activated && !b.activated);
	/* ... and B is still in front of it. */
	CHECK(is(ox, oy, 0, 255, 0));
	/* Back over B and again over A: still no change in the stacking. */
	inject("motion_abs %d %d", ox, oy);
	settle_all();
	CHECK(b.activated && !a.activated);
	inject("motion_abs %d %d", px, py);
	settle_all();
	CHECK(a.activated);
	CHECK(is(ox, oy, 0, 255, 0));

	/* A click on A raises it. */
	inject("button 272 1");
	inject("button 272 0");
	settle_all();
	CHECK(a.activated);
	CHECK(is(ox, oy, 255, 0, 0));   /* where B was in front of it, A is now */

	/* And hovering B does not raise B back. */
	inject("motion_abs %d %d", bx1 - 2, by1 - 2);
	settle_all();
	CHECK(b.activated && !a.activated);
	CHECK(is(ox, oy, 255, 0, 0));

	CHECK(stop_compositor() == 0);
	puts("hoverfocus: ok");
	return 0;
}
