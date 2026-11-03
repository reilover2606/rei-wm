/*
 * End-to-end window-manager test: tiling, gaps, splits, moving, floating,
 * fullscreen, workspaces, dialogs, mouse move/resize and two outputs.
 *
 * The clients behave like real applications: whenever the compositor sends a
 * new size in a configure they commit a buffer of that size. Geometry is then
 * checked from screenshots, so the test sees exactly what a user would.
 *
 * Setup: output OUT is 800x600 at (0,0), OUT2 is 640x480 at (800,0). Gaps are
 * the defaults (10 outer, 10 inner), so a lone window on OUT fills
 * (10,10)-(790,590). Workspace 1 starts on OUT and 2 on OUT2. The pointer is
 * parked at (795,595), over nothing, so it never steals focus.
 */
#define SOCKET "rei-wm-test"
#include "testlib.h"

#define OUT2_W 640
#define OUT2_H 480

/* evdev key codes */
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_Q 16
#define KEY_E 18
#define KEY_F 33
#define KEY_H 35
#define KEY_L 38
#define KEY_V 47
#define KEY_SPACE 57
#define KEY_SHIFT 42
#define KEY_SUPER 125
#define BTN_LEFT 272
#define BTN_RIGHT 273

/* Colours, as XRGB pixels and as the RGB triples to expect on screen. */
#define RED 0x00ff0000
#define GREEN 0x0000ff00
#define BLUE 0x000000ff
#define YELLOW 0x00ffff00
#define CYAN 0x0000ffff
#define BG 27, 39, 51  /* the default background colour #1b2733 */

static struct client *g_clients[4];
static struct window *g_wins[16];
static int g_nwins;

static void track(struct client *c, struct window *w)
{
	(void)c;
	CHECK(g_nwins < 16);
	g_wins[g_nwins++] = w;
}

static void untrack(struct window *w)
{
	for (int i = 0; i < g_nwins; i++) {
		if (g_wins[i] == w) {
			g_wins[i] = g_wins[--g_nwins];
			return;
		}
	}
}

static struct client *owner_of(struct window *w) { return w->c; }

/*
 * Waits for the compositor to finish reacting to everything so far, and lets
 * every client answer any new size with a new buffer, until nothing changes.
 */
static void settle_all(void)
{
	for (int round = 0; round < 8; round++) {
		input_sync();
		for (int i = 0; g_clients[i]; i++) {
			CHECK(wl_display_roundtrip(g_clients[i]->d) >= 0);
			CHECK(wl_display_roundtrip(g_clients[i]->d) >= 0);
		}
		bool changed = false;
		for (int i = 0; i < g_nwins; i++)
			changed |= window_follow(owner_of(g_wins[i]), g_wins[i]);
		if (!changed) {
			for (int i = 0; g_clients[i]; i++)
				CHECK(wl_display_roundtrip(g_clients[i]->d) >= 0);
			return;
		}
	}
	fprintf(stderr, "windows never settled\n");
	exit(1);
}

/* Presses modifiers and a key together, as one atomic write. */
static void chord(int mod1, int mod2, int key)
{
	char buf[256];
	int n = 0;
	n += snprintf(buf + n, sizeof buf - (size_t)n, "key %d 1\n", mod1);
	if (mod2)
		n += snprintf(buf + n, sizeof buf - (size_t)n, "key %d 1\n", mod2);
	n += snprintf(buf + n, sizeof buf - (size_t)n, "key %d 1\nkey %d 0\n", key, key);
	if (mod2)
		n += snprintf(buf + n, sizeof buf - (size_t)n, "key %d 0\n", mod2);
	snprintf(buf + n, sizeof buf - (size_t)n, "key %d 0\n", mod1);
	inject_lines(buf);
}
static void mod(int key) { chord(KEY_SUPER, 0, key); }
static void mod_shift(int key) { chord(KEY_SUPER, KEY_SHIFT, key); }

static void open_win(struct client *c, struct window *w, const char *title, uint32_t color)
{
	toplevel_create(c, w, title);
	w->color = color;
	w->natural_w = 200;
	w->natural_h = 120;
	track(c, w);
	window_follow(c, w);
	settle_all();
}

static void close_win(struct window *w)
{
	struct client *c = w->c;
	untrack(w);
	xdg_toplevel_destroy(w->tl);
	xdg_surface_destroy(w->xs);
	wl_surface_destroy(w->surface);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	settle_all();
}

static void park_pointer(void)
{
	inject("motion_abs 795 595");
	settle_all();
}

static void expect_size(const struct window *w, int cw, int ch, bool tiled, const char *what)
{
	if (w->cfg_w != cw || w->cfg_h != ch || w->cfg_tiled != tiled) {
		fprintf(stderr, "%s: configured %dx%d tiled=%d, expected %dx%d tiled=%d\n", what,
			w->cfg_w, w->cfg_h, w->cfg_tiled, cw, ch, tiled);
		exit(1);
	}
}

static int active_count(void)
{
	int n = 0;
	for (int i = 0; i < g_nwins; i++)
		n += g_wins[i]->activated;
	return n;
}

/* ------------------------------------------------------------------------ */

static void test_tiling(struct client *c, struct window *w1, struct window *w2)
{
	open_win(c, w1, "W1", RED);
	expect_size(w1, 780, 580, true, "lone window fills the area inside the outer gap");
	CHECK(w1->activated);

	struct ppm img = screenshot();
	expect_px(&img, 400, 100, 255, 0, 0, 0, "window");
	expect_px(&img, 10, 10, 255, 0, 0, 0, "window's top-left pixel");
	expect_px(&img, 789, 589, 255, 0, 0, 0, "window's bottom-right pixel");
	expect_px(&img, 9, 9, BG, 0, "outer gap, top-left");
	expect_px(&img, 5, 100, BG, 0, "outer gap, left");
	expect_px(&img, 400, 595, BG, 0, "outer gap, bottom");
	expect_px(&img, 790, 100, BG, 0, "outer gap, right");
	free(img.rgb);

	/* A second window splits the row: equal halves with the inner gap between. */
	open_win(c, w2, "W2", GREEN);
	expect_size(w1, 385, 580, true, "W1 after W2 opens");
	expect_size(w2, 385, 580, true, "W2");
	CHECK(w2->activated && !w1->activated && active_count() == 1);

	img = screenshot();
	expect_px(&img, 200, 100, 255, 0, 0, 0, "W1");
	expect_px(&img, 394, 100, 255, 0, 0, 0, "W1's right edge");
	expect_px(&img, 395, 100, BG, 0, "inner gap, left edge");
	expect_px(&img, 404, 100, BG, 0, "inner gap, right edge");
	expect_px(&img, 405, 100, 0, 255, 0, 0, "W2's left edge");
	expect_px(&img, 600, 100, 0, 255, 0, 0, "W2");
	free(img.rgb);
}

static void test_focus_and_move(struct window *w1, struct window *w2)
{
	mod(KEY_H);
	settle_all();
	CHECK(w1->activated && !w2->activated);
	mod(KEY_L);
	settle_all();
	CHECK(w2->activated && !w1->activated);

	/* With focus_follows_mouse the pointer chooses too. */
	inject("motion_abs 200 300");
	settle_all();
	CHECK(w1->activated && active_count() == 1);
	inject("motion_abs 600 300");
	settle_all();
	CHECK(w2->activated && active_count() == 1);
	park_pointer();
	CHECK(w2->activated);  /* Moving off every window changes nothing. */

	/* Move W1 right, past W2: the columns swap. */
	mod(KEY_H);
	mod_shift(KEY_L);
	settle_all();
	CHECK(w1->activated);
	struct ppm img = screenshot();
	expect_px(&img, 200, 100, 0, 255, 0, 0, "W2 now on the left");
	expect_px(&img, 600, 100, 255, 0, 0, 0, "W1 now on the right");
	free(img.rgb);
	expect_size(w1, 385, 580, true, "W1 after the move");
}

static void test_split_and_float(struct client *c, struct window *w1, struct window *w2,
				 struct window *w3)
{
	/* "split v", then a new window stacks under the active one (W1, on the right). */
	mod(KEY_V);
	open_win(c, w3, "W3", BLUE);
	expect_size(w1, 385, 285, true, "W1 stacked");
	expect_size(w3, 385, 285, true, "W3 stacked");
	expect_size(w2, 385, 580, true, "W2 untouched");
	struct ppm img = screenshot();
	expect_px(&img, 600, 100, 255, 0, 0, 0, "W1 on top");
	expect_px(&img, 600, 450, 0, 0, 255, 0, "W3 underneath");
	expect_px(&img, 600, 295, BG, 0, "gap between the stacked windows");
	expect_px(&img, 600, 304, BG, 0, "gap between the stacked windows");
	expect_px(&img, 200, 300, 0, 255, 0, 0, "W2 spans the full height");
	free(img.rgb);

	/* Float W3: it keeps its size and place, and W1 takes the space back. */
	mod_shift(KEY_SPACE);
	settle_all();
	expect_size(w3, 385, 285, false, "W3 floating keeps its size");
	expect_size(w1, 385, 580, true, "W1 reclaims the column");
	img = screenshot();
	expect_px(&img, 600, 450, 0, 0, 255, 0, "floating W3 above W1");
	expect_px(&img, 600, 300, 255, 0, 0, 0, "W1 visible above the floating window");
	free(img.rgb);

	/* Back into the grid: three columns. */
	mod_shift(KEY_SPACE);
	settle_all();
	expect_size(w3, 253, 580, true, "W3 tiled again");
	expect_size(w1, 254, 580, true, "W1, three columns (253+254+253 = 760)");
	expect_size(w2, 253, 580, true, "W2");
	img = screenshot();
	expect_px(&img, 100, 100, 0, 255, 0, 0, "column 1");
	expect_px(&img, 268, 100, BG, 0, "gap between columns 1 and 2");
	expect_px(&img, 400, 100, 255, 0, 0, 0, "column 2");
	expect_px(&img, 532, 100, BG, 0, "gap between columns 2 and 3");
	expect_px(&img, 650, 100, 0, 0, 255, 0, "column 3");
	free(img.rgb);
}

static void test_fullscreen(struct window *w3)
{
	CHECK(w3->activated);
	mod(KEY_F);
	settle_all();
	CHECK(w3->cfg_w == 800 && w3->cfg_h == 600 && w3->cfg_fullscreen && !w3->cfg_tiled);
	struct ppm img = screenshot();
	expect_px(&img, 5, 5, 0, 0, 255, 0, "fullscreen covers the gaps, top-left");
	expect_px(&img, 400, 100, 0, 0, 255, 0, "fullscreen");
	expect_px(&img, 5, 595, 0, 0, 255, 0, "fullscreen covers the gaps, bottom-left");
	free(img.rgb);

	mod(KEY_F);
	settle_all();
	CHECK(!w3->cfg_fullscreen && w3->cfg_tiled && w3->cfg_w == 253);
}

static void test_workspaces(struct client *c, struct window *w1, struct window *w2, struct window *w3)
{
	(void)c; (void)w1; (void)w2;
	/* Workspace 3 is new and empty on OUT; the windows on 1 disappear from view. */
	mod(KEY_3);
	settle_all();
	struct ppm img = screenshot();
	expect_px(&img, 100, 100, BG, 0, "empty workspace");
	expect_px(&img, 600, 450, BG, 0, "empty workspace");
	free(img.rgb);

	mod(KEY_1);
	settle_all();
	img = screenshot();
	expect_px(&img, 100, 100, 0, 255, 0, 0, "workspace 1 is back");
	free(img.rgb);

	/* Send the active window (W3) to workspace 3 and follow it there. */
	CHECK(w3->activated);
	mod_shift(KEY_3);
	settle_all();
	CHECK(!w3->activated);
	img = screenshot();
	expect_px(&img, 200, 100, 0, 255, 0, 0, "two windows are left on workspace 1");
	expect_px(&img, 600, 100, 255, 0, 0, 0, "two windows are left on workspace 1");
	free(img.rgb);
	CHECK(active_count() == 1);  /* focus fell back to another window */

	mod(KEY_3);
	settle_all();
	CHECK(w3->activated && w3->cfg_w == 780 && w3->cfg_h == 580 && w3->cfg_tiled);
	img = screenshot();
	expect_px(&img, 400, 100, 0, 0, 255, 0, "W3 alone on workspace 3");
	free(img.rgb);

	mod(KEY_1);
	settle_all();
}

static void test_close_and_dialog(struct client *c, struct window *w1, struct window *w2)
{
	/* Focus W1 (right) with the pointer, then close it with the keyboard. */
	inject("motion_abs 600 300");
	settle_all();
	CHECK(w1->activated);
	park_pointer();
	mod_shift(KEY_Q);
	settle_all();
	CHECK(w1->close_requested && !w2->close_requested);
	close_win(w1);
	expect_size(w2, 780, 580, true, "the survivor takes the whole area");

	/* A dialog (fixed size) floats, at its own size, centred. */
	struct window dlg;
	memset(&dlg, 0, sizeof dlg);
	dlg.c = c;
	dlg.surface = wl_compositor_create_surface(c->compositor);
	dlg.xs = xdg_wm_base_get_xdg_surface(c->wm, dlg.surface);
	xdg_surface_add_listener(dlg.xs, &xs_listener, &dlg);
	dlg.tl = xdg_surface_get_toplevel(dlg.xs);
	xdg_toplevel_add_listener(dlg.tl, &tl_listener, &dlg);
	xdg_toplevel_set_min_size(dlg.tl, 300, 200);
	xdg_toplevel_set_max_size(dlg.tl, 300, 200);
	wl_surface_commit(dlg.surface);
	CHECK(wait_for(c, &dlg.configured, 3000));
	dlg.color = YELLOW;
	dlg.natural_w = 300;
	dlg.natural_h = 200;
	track(c, &dlg);
	window_follow(c, &dlg);
	settle_all();
	expect_size(&dlg, 0, 0, false, "a fixed-size window floats and keeps its own size");
	expect_size(w2, 780, 580, true, "the tiled window is unaffected by the dialog");
	struct ppm img = screenshot();
	expect_px(&img, 260, 210, 255, 255, 0, 0, "dialog, centred at (250,200)");
	expect_px(&img, 549, 399, 255, 255, 0, 0, "dialog's far corner");
	expect_px(&img, 240, 190, 0, 255, 0, 0, "tiled window around it");
	free(img.rgb);

	/* Mod+left-drag moves it (100,50); the click never reaches the client. */
	struct client *cl = c;
	ev_clear(cl);
	inject_lines("key 125 1\nmotion_abs 400 300\nbutton 272 1\nmotion_abs 500 350\nbutton 272 0\nkey 125 0\n");
	settle_all();
	img = screenshot();
	expect_px(&img, 360, 260, 255, 255, 0, 0, "dialog after being dragged to (350,250)");
	expect_px(&img, 260, 210, 0, 255, 0, 0, "its old place is the tiled window again");
	expect_px(&img, 649, 449, 255, 255, 0, 0, "dialog's new far corner");
	free(img.rgb);
	CHECK(dlg.activated);

	/* Mod+right-drag from its bottom-right quadrant resizes it by (+60,+40). */
	inject_lines("key 125 1\nmotion_abs 640 440\nbutton 273 1\nmotion_abs 700 480\nbutton 273 0\nkey 125 0\n");
	settle_all();
	expect_size(&dlg, 360, 240, false, "dialog resized from its corner");
	img = screenshot();
	/* The dialog is now (350,250)-(710,490); stay clear of the cursor at (700,480). */
	expect_px(&img, 680, 460, 255, 255, 0, 0, "dialog's enlarged corner");
	expect_px(&img, 690, 489, 255, 255, 0, 0, "dialog's bottom edge, past the old size");
	expect_px(&img, 709, 300, 255, 255, 0, 0, "dialog's right edge, past the old size");
	expect_px(&img, 711, 300, 0, 255, 0, 0, "tiled window beyond the dialog's right edge");
	free(img.rgb);
	park_pointer();

	close_win(&dlg);
	CHECK(w2->activated);  /* focus returns to the window underneath */
}

static void test_two_outputs(struct client *c, struct window *w2, struct window *w5)
{
	/* Workspace 2 is showing on OUT2; asking for it just moves focus there. */
	mod(KEY_2);
	settle_all();
	CHECK(!w2->activated);

	open_win(c, w5, "W5", CYAN);
	expect_size(w5, OUT2_W - 20, OUT2_H - 20, true, "a window on the smaller output");
	struct ppm img2 = screenshot_output("OUT2");
	CHECK(img2.w == OUT2_W && img2.h == OUT2_H);
	expect_px(&img2, 300, 200, 0, 255, 255, 0, "W5 on OUT2");
	expect_px(&img2, 5, 5, BG, 0, "OUT2's outer gap");
	free(img2.rgb);
	struct ppm img = screenshot();
	expect_px(&img, 300, 200, 0, 255, 0, 0, "OUT is untouched");
	free(img.rgb);

	/* Move it to workspace 1, which is showing on OUT: it changes screens. */
	mod_shift(KEY_1);
	settle_all();
	expect_size(w5, 385, 580, true, "W5 joins workspace 1 on OUT beside W2");
	img2 = screenshot_output("OUT2");
	expect_px(&img2, 300, 200, BG, 0, "OUT2 is empty again");
	free(img2.rgb);
	img = screenshot();
	expect_px(&img, 600, 100, 0, 255, 255, 0, "W5 is now on OUT");
	free(img.rgb);

	/* Back to workspace 1: it is already showing, so focus moves to OUT. */
	mod(KEY_1);
	settle_all();
}

/*
 * A client that is tiled next to others dies without cleaning up. Its slot must
 * be given back and focus must land on a surviving window (the destroy-order
 * problem of milestone 4, now with a layout tree involved).
 */
static void test_abrupt_disconnect_tiled(struct window *w2, struct window *w5)
{
	CHECK(w2->cfg_w == 385 && w5->cfg_w == 385);

	struct client doomed;
	client_connect(&doomed);
	g_clients[1] = &doomed;
	struct window wd;
	open_win(&doomed, &wd, "doomed", YELLOW);
	CHECK(wd.activated);
	CHECK(w2->cfg_w == 253 && w5->cfg_w == 254);  /* three columns now */

	untrack(&wd);
	wl_display_disconnect(doomed.d);  /* no destroy requests: the socket just closes */
	g_clients[1] = NULL;
	settle_all();

	CHECK(w2->cfg_w == 385 && w5->cfg_w == 385);  /* the slot was given back */
	CHECK(active_count() == 1);                     /* and focus landed somewhere */
	struct ppm img = screenshot();
	expect_px(&img, 200, 100, 0, 255, 0, 0, "W2 after its neighbour vanished");
	expect_px(&img, 600, 100, 0, 255, 255, 0, "W5 after its neighbour vanished");
	free(img.rgb);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-wm-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[600];
	snprintf(config, sizeof config,
		 "[general]\nfocus_follows_mouse = true\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n"
		 "[[output]]\nname = \"OUT2\"\nwidth = %d\nheight = %d\n",
		 OUT_W, OUT_H, OUT2_W, OUT2_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	client_bind_keyboard(&c);
	park_pointer();

	struct window w1, w2, w3, w5;
	test_tiling(&c, &w1, &w2);
	test_focus_and_move(&w1, &w2);
	test_split_and_float(&c, &w1, &w2, &w3);
	test_fullscreen(&w3);
	test_workspaces(&c, &w1, &w2, &w3);
	/* W2 is left of W1 here; W3 lives on workspace 3. */
	test_close_and_dialog(&c, &w1, &w2);
	test_two_outputs(&c, &w2, &w5);
	test_abrupt_disconnect_tiled(&w2, &w5);

	/* Windows on several workspaces and outputs are still open when the session
	 * ends, so shutdown tears outputs down under live windows. */
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
	puts("test_wm: ok");
	return 0;
}
