/*
 * End-to-end toolbar test. Run once per mode: top, bottom, left, right (one
 * 800x600 output) and "multi" (a top bar on two outputs).
 *
 * The bar is 28px thick and takes an edge of the output. Workspace buttons are
 * 28px squares from the start of the bar (left of a horizontal bar, top of a
 * vertical one); the clock is at the end. Windows get what is left, less the usual
 * gaps (10) and, at the top of that area, room for the notches (22).
 */
#define SOCKET "rei-bar-test"
#include "testlib.h"

#define KEY_1 2
#define KEY_2 3
#define KEY_F 33
#define BTN_LEFT 272

#define RED 0x00ff0000
#define GREEN 0x0000ff00

/* Bar colours (the defaults) as RGB triples. */
#define BAR_BG 22, 28, 38
#define BAR_FOCUSED 60, 110, 190
#define BAR_VISIBLE 39, 65, 106    /* highlight mixed 45% into the background */
#define BAR_HOVER 57, 63, 73       /* foreground mixed 18% into the background */

#define T 28
#define OUT2_W 640
#define OUT2_H 480

enum pos { TOP, BOTTOM, LEFT, RIGHT };
static enum pos g_pos;
static bool g_multi;

static const char *const pos_names[] = { "top", "bottom", "left", "right" };

struct box {
	int x, y, w, h;
};

static bool vertical(void) { return g_pos == LEFT || g_pos == RIGHT; }

/* The bar of the output whose top-left is (ox, 0) and size ow x oh. */
static struct box bar_of(int ox, int ow, int oh)
{
	switch (g_pos) {
	case TOP: return (struct box){ ox, 0, ow, T };
	case BOTTOM: return (struct box){ ox, oh - T, ow, T };
	case LEFT: return (struct box){ ox, 0, T, oh };
	default: return (struct box){ ox + ow - T, 0, T, oh };
	}
}

/* Button `i`'s cell. */
static struct box button_of(struct box bar, int i)
{
	return vertical() ? (struct box){ bar.x, bar.y + i * T, T, T}
			  : (struct box){ bar.x + i * T, bar.y, T, T };
}

static void probe(const struct ppm *img, struct box b, int r, int g, int bl, const char *what)
{
	expect_px(img, b.x + 4, b.y + 4, r, g, bl, 2, what);  /* inside the fill, clear of the number */
}

static void click_at(int x, int y)
{
	char buf[128];
	snprintf(buf, sizeof buf, "motion_abs %d %d\nbutton 272 1\nbutton 272 0\n", x, y);
	inject_lines(buf);
	settle_all();
}

static void move_to(int x, int y)
{
	inject("motion_abs %d %d", x, y);
	settle_all();
}

/* Pixels in a region that differ from the bar's plain background, i.e. text. */
static int ink(const struct ppm *img, int x0, int y0, int x1, int y1)
{
	int n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			n += !near_px(img, x, y, 22, 28, 38, 3);
	return n;
}

/* The clock's region: the end of the bar. */
static struct box clock_region(struct box bar)
{
	if (vertical())
		return (struct box){ bar.x + 2, bar.y + bar.h - 100, bar.w - 4, 98 };
	return (struct box){ bar.x + bar.w - 100, bar.y + 2, 98, bar.h - 4 };
}

static int clock_ink(const struct ppm *img, struct box bar)
{
	struct box c = clock_region(bar);
	return ink(img, c.x, c.y, c.x + c.w, c.y + c.h);
}

/* ------------------------------------------------------------------------ */

static void test_exclusive_zone_and_buttons(struct client *c, struct window *w1)
{
	struct box bar = bar_of(0, OUT_W, OUT_H);
	struct ppm img = screenshot();
	probe(&img, button_of(bar, 0), BAR_FOCUSED, "workspace 1 is highlighted");
	/* The middle of the bar, away from buttons and clock, is plain. */
	expect_px(&img, bar.x + bar.w / 2, bar.y + bar.h / 2, BAR_BG, 2, "bar background");
	/* The clock is drawn at the end of the bar. */
	CHECK(clock_ink(&img, bar) > 10);
	free(img.rgb);

	open_win(c, w1, "W1", RED);
	/* The bar's edge is not available: top/bottom lose height, left/right width. */
	if (vertical())
		expect_size(w1, 752, 568, true, "window area beside a vertical bar");
	else
		expect_size(w1, 780, 540, true, "window area beside a horizontal bar");

	img = screenshot();
	struct box win;
	switch (g_pos) {
	case TOP: win = (struct box){ 10, 50, 780, 540 }; break;
	case BOTTOM: win = (struct box){ 10, 22, 780, 540 }; break;
	case LEFT: win = (struct box){ 38, 22, 752, 568 }; break;
	default: win = (struct box){ 10, 22, 752, 568 }; break;
	}
	expect_px(&img, win.x, win.y, 255, 0, 0, 0, "window's top-left pixel");
	expect_px(&img, win.x + win.w - 1, win.y + win.h - 1, 255, 0, 0, 0, "window's bottom-right pixel");
	expect_px(&img, win.x - 1, win.y + 100, 27, 39, 51, 2, "the gap just outside the window is plain desktop");
	/* The window never overlaps the bar. */
	probe(&img, button_of(bar, 0), BAR_FOCUSED, "bar untouched by the window");
	free(img.rgb);
}

static void test_workspaces(struct client *c, struct window *w1, struct window *w2)
{
	struct box bar = bar_of(0, OUT_W, OUT_H);

	/* A new workspace with a window of its own: two buttons, the new one lit. */
	mod(KEY_2);
	settle_all();
	open_win(c, w2, "W2", GREEN);
	struct ppm img = screenshot();
	probe(&img, button_of(bar, 0), BAR_BG, "workspace 1 has windows but isn't showing: no fill");
	probe(&img, button_of(bar, 1), BAR_FOCUSED, "workspace 2 is showing");
	free(img.rgb);
	CHECK(w2->activated && !w1->activated);

	/* Click a button: switch to that workspace. */
	struct box b0 = button_of(bar, 0);
	click_at(b0.x + T / 2, b0.y + T / 2);
	CHECK(w1->activated && !w2->activated);
	move_to(OUT_W / 2, OUT_H / 2);   /* the pointer rests on the button, which would show it hovered */
	img = screenshot();
	probe(&img, button_of(bar, 0), BAR_FOCUSED, "back on workspace 1");
	probe(&img, button_of(bar, 1), BAR_BG, "workspace 2 now hidden");
	free(img.rgb);

	/* The click didn't reach any client. */
	/* Hovering a button highlights it. */
	struct box b1 = button_of(bar, 1);
	move_to(b1.x + T / 2, b1.y + T / 2);
	img = screenshot();
	probe(&img, button_of(bar, 1), BAR_HOVER, "hovered button");
	probe(&img, button_of(bar, 0), BAR_FOCUSED, "other buttons unchanged");
	free(img.rgb);
	move_to(OUT_W / 2, OUT_H / 2);
	img = screenshot();
	probe(&img, button_of(bar, 1), BAR_BG, "highlight gone when the pointer leaves");
	free(img.rgb);

	/* The wheel over the bar steps through workspaces, stopping at the ends. */
	move_to(bar.x + bar.w / 2, bar.y + bar.h / 2);
	inject("axis v 10");
	settle_all();
	CHECK(w2->activated);
	inject("axis v 10");   /* already at the last one */
	settle_all();
	CHECK(w2->activated);
	inject("axis v -10");
	settle_all();
	CHECK(w1->activated);
	inject("axis v -10");  /* already at the first one */
	settle_all();
	CHECK(w1->activated);
	move_to(OUT_W / 2, OUT_H / 2);
}

static void test_fullscreen_covers_bar(struct window *w1)
{
	CHECK(w1->activated);
	struct box bar = bar_of(0, OUT_W, OUT_H);
	mod(KEY_F);
	settle_all();
	struct ppm img = screenshot();
	expect_px(&img, bar.x + bar.w / 2, bar.y + bar.h / 2, 255, 0, 0, 0, "a fullscreen window covers the bar");
	free(img.rgb);
	/* ...and a click there reaches the window, not a workspace button. */
	struct box b = button_of(bar, 1);
	click_at(b.x + T / 2, b.y + T / 2);
	CHECK(w1->activated);
	mod(KEY_F);
	settle_all();
	img = screenshot();
	probe(&img, button_of(bar, 0), BAR_FOCUSED, "the bar is back");
	free(img.rgb);
}

static void test_clock_ticks(void)
{
	/* The test's clock shows seconds, so it must change by itself. */
	struct box bar = bar_of(0, OUT_W, OUT_H);
	struct ppm a = screenshot();
	sleep_ms(2200);
	struct ppm b = screenshot();
	struct box c = clock_region(bar);
	int diff = 0;
	for (int y = c.y; y < c.y + c.h; y++)
		for (int x = c.x; x < c.x + c.w; x++)
			diff += !near_px(&a, x, y, b.rgb[((size_t)y * (size_t)b.w + (size_t)x) * 3],
					 b.rgb[((size_t)y * (size_t)b.w + (size_t)x) * 3 + 1],
					 b.rgb[((size_t)y * (size_t)b.w + (size_t)x) * 3 + 2], 0);
	CHECK(diff > 5);
	free(a.rgb);
	free(b.rgb);
}

static void test_multi_output(struct client *c)
{
	struct box bar1 = bar_of(0, OUT_W, OUT_H), bar2 = bar_of(0, OUT2_W, OUT2_H);

	/* Each output has its own bar listing its own workspaces: workspace 1 on the
	 * first (focused), workspace 2 on the second (showing, but not focused). */
	struct ppm img = screenshot();
	probe(&img, button_of(bar1, 0), BAR_FOCUSED, "OUT: workspace 1, focused");
	free(img.rgb);
	struct ppm img2 = screenshot_output("OUT2");
	CHECK(img2.w == OUT2_W && img2.h == OUT2_H);
	probe(&img2, button_of(bar2, 0), BAR_VISIBLE, "OUT2: workspace 2, showing but not focused");
	CHECK(clock_ink(&img2, bar2) > 10);   /* every output has a clock */
	free(img2.rgb);

	/* Clicking the other output's button moves focus there. Its bar is at x >= 800. */
	click_at(OUT_W + T / 2, T / 2);
	move_to(OUT_W / 2, OUT_H / 2);   /* off the hovered button */
	img = screenshot();
	probe(&img, button_of(bar1, 0), BAR_VISIBLE, "OUT: no longer focused");
	free(img.rgb);
	img2 = screenshot_output("OUT2");
	probe(&img2, button_of(bar2, 0), BAR_FOCUSED, "OUT2: now focused");
	free(img2.rgb);

	/* New windows open on the focused output, sized for ITS area and bar. */
	struct window w;
	open_win(c, &w, "W", RED);
	expect_size(&w, OUT2_W - 20, OUT2_H - T - 20 - 12, true, "window on the second output");
	img2 = screenshot_output("OUT2");
	expect_px(&img2, 10, T + 22, 255, 0, 0, 0, "window starts below OUT2's bar");
	free(img2.rgb);

	/* Back to workspace 1: it is showing on the first output, so focus goes there. */
	mod(KEY_1);
	settle_all();
	img = screenshot();
	probe(&img, button_of(bar1, 0), BAR_FOCUSED, "OUT focused again");
	free(img.rgb);
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	quiet_client_leaks();
	g_multi = strcmp(argv[2], "multi") == 0;
	g_pos = TOP;
	for (int i = 0; i < 4; i++)
		if (strcmp(argv[2], pos_names[i]) == 0)
			g_pos = (enum pos)i;
	CHECK(g_multi || strcmp(argv[2], pos_names[g_pos]) == 0);

	snprintf(g_dir, sizeof g_dir, "/tmp/rei-bar-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[600];
	int n = snprintf(config, sizeof config,
			 "[toolbar]\nposition = \"%s\"\nclock_format = \"%%S\"\n"
			 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n",
			 pos_names[g_pos], OUT_W, OUT_H);
	if (g_multi) {
		snprintf(config + n, sizeof config - (size_t)n,
			 "[[output]]\nname = \"OUT2\"\nwidth = %d\nheight = %d\n", OUT2_W, OUT2_H);
		g_n_outputs = 2;
	}
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);
	client_bind_pointer(&c);
	move_to(OUT_W / 2, OUT_H / 2);

	struct window w1, w2;
	if (g_multi) {
		test_multi_output(&c);
	} else {
		test_exclusive_zone_and_buttons(&c, &w1);
		test_workspaces(&c, &w1, &w2);
		test_fullscreen_covers_bar(&w1);
		if (g_pos == TOP)
			test_clock_ticks();
	}

	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int t = 0;; t += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && t < 5000);
		sleep_ms(10);
	}
	wl_display_disconnect(c.d);
	char *log = slurp(g_log);
	CHECK(log && !strstr(log, "ERROR"));
	free(log);
	puts("test_bar: ok");
	return 0;
}
