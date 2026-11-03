/*
 * End-to-end test of the window notches: the drag handle and the minimize /
 * maximize / close tabs that hover above the focused window.
 *
 * Setup: one 800x600 output, gaps of 4, notch height 22 (the default). With the
 * notch reserving room, the tiling area is (4,22)-(796,596): the top gap is 22
 * (not 4) and stacked windows are 22 apart. Notch tabs sit directly above a
 * window's top edge: the buttons are the rightmost 84px, the handle the leftmost
 * 48px, each 22px tall.
 */
#define SOCKET "rei-notch-test"
#include "testlib.h"

#define KEY_SPACE 57
#define KEY_H 35
#define KEY_F 33
#define KEY_E 18
#define BTN_LEFT 272

#define RED 0x00ff0000
#define GREEN 0x0000ff00
#define BG 27, 39, 51

/* A notch tab's body, blended over the default background: (12,16,24) at 94%. */
#define TAB 13, 17, 26

static void check_near(int got, int want, int tol, const char *what)
{
	if (abs(got - want) > tol) {
		fprintf(stderr, "%s: got %d, expected %d +-%d\n", what, got, want, tol);
		exit(1);
	}
}

static const uint8_t *rgb(const struct ppm *img, int x, int y)
{
	return img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
}

static void test_appearance(struct client *c, struct window *w1, struct window *w2)
{
	open_win(c, w1, "W1", RED);
	expect_size(w1, 792, 574, true, "a lone window: top gap is the notch height, not the 4px gap");

	struct ppm img = screenshot();
	expect_px(&img, 400, 22, 255, 0, 0, 0, "window's top row is at y=22");
	expect_px(&img, 400, 21, BG, 0, "just above the window: empty, no title bar");
	expect_px(&img, 4, 100, 255, 0, 0, 0, "left gap is the configured 4px");
	expect_px(&img, 3, 100, BG, 0, "left gap is the configured 4px");
	/* The focused window shows both tabs, entirely above its top edge. */
	expect_px(&img, 722, 5, TAB, 4, "button tab, minimize cell");
	expect_px(&img, 772, 5, TAB, 4, "button tab, close cell");
	expect_px(&img, 14, 6, TAB, 4, "drag-handle tab");
	expect_px(&img, 722, 22, 255, 0, 0, 0, "the tabs take nothing from the window");
	expect_px(&img, 400, 10, BG, 0, "no tab between the handle and the buttons");
	/* Rounded top corners: the very corner pixel of a tab is not drawn. */
	expect_px(&img, 796 - 1, 0, BG, 0, "tab's rounded corner");
	free(img.rgb);

	/* A second window: the tabs belong to the focused window only. */
	open_win(c, w2, "W2", GREEN);
	expect_size(w1, 394, 574, true, "W1 beside W2");
	expect_size(w2, 394, 574, true, "W2 beside W1");
	CHECK(w2->activated && !w1->activated);
	img = screenshot();
	expect_px(&img, 722, 5, TAB, 4, "W2's buttons (x 712..796)");
	expect_px(&img, 412, 6, TAB, 4, "W2's handle (x 402..450)");
	expect_px(&img, 324, 5, BG, 0, "W1 is not focused: no tab above it");
	expect_px(&img, 14, 6, BG, 0, "W1 is not focused: no handle");
	free(img.rgb);

	/* Moving focus moves the tabs, and never resizes anything. */
	mod(KEY_H);
	settle_all();
	CHECK(w1->activated);
	expect_size(w1, 394, 574, true, "W1 after gaining focus");
	expect_size(w2, 394, 574, true, "W2 after losing focus");
	img = screenshot();
	expect_px(&img, 324, 5, TAB, 4, "W1's buttons (x 314..398)");
	expect_px(&img, 14, 6, TAB, 4, "W1's handle");
	expect_px(&img, 722, 5, BG, 0, "W2's tab is gone");
	free(img.rgb);
}

static void test_hover_and_clicks(struct client *c, struct window *w1)
{
	/* W1's buttons: minimize 314..342, maximize 342..370, close 370..398. */
	client_bind_pointer(c);
	inject("motion_abs 200 300");
	settle_all();
	CHECK(ev_count(c, EV_PTR_ENTER) >= 1);

	ev_clear(c);
	inject("motion_abs 384 10");   /* onto the close button */
	settle_all();
	CHECK(ev_count(c, EV_PTR_LEAVE) == 1);  /* the client's pointer left: the notch is ours */
	struct ppm img = screenshot();
	const uint8_t *p = rgb(&img, 374, 5);
	CHECK(p[0] > 170 && p[1] < 110 && p[2] < 100);  /* hovered close turns red */
	free(img.rgb);

	inject("motion_abs 356 10");   /* onto maximize: close returns to normal */
	settle_all();
	img = screenshot();
	expect_px(&img, 374, 5, TAB, 4, "close button no longer hovered");
	p = rgb(&img, 346, 5);
	CHECK(p[0] > 13 + 25);          /* maximize is highlighted (lighter) */
	free(img.rgb);
	CHECK(ev_count(c, EV_PTR_MOTION) == 0 && ev_count(c, EV_PTR_ENTER) == 0);  /* nothing leaked */

	/* A click is press and release on the same button, and the client never
	 * sees it. */
	ev_clear(c);
	inject_lines("motion_abs 384 10\nbutton 272 1\nbutton 272 0\n");
	settle_all();
	CHECK(w1->close_requested);
	CHECK(ev_count(c, EV_PTR_BUTTON) == 0);
	w1->close_requested = false;

	/* Press on close, slide off, release elsewhere: cancelled, nothing happens. */
	inject_lines("motion_abs 384 10\nbutton 272 1\nmotion_abs 200 300\nbutton 272 0\n");
	settle_all();
	CHECK(!w1->close_requested);
	CHECK(ev_count(c, EV_PTR_BUTTON) == 0);

	/* The maximize button is still a placeholder: clickable and harmless. (Minimize is tested
	 * with the tray.) */
	inject_lines("motion_abs 356 10\nbutton 272 1\nbutton 272 0\n");
	settle_all();
	CHECK(!w1->close_requested && w1->activated);
	expect_size(w1, 394, 574, true, "unchanged after clicking the placeholder");
	inject("motion_abs 200 300");
	settle_all();
}

static void test_float_drag_and_pull_back(struct client *c, struct window *w1, struct window *w2)
{
	(void)c; (void)w2;
	mod_shift(KEY_SPACE);   /* float W1; it keeps its size and place (4,22 394x574) */
	settle_all();
	expect_size(w1, 394, 574, false, "W1 floating");

	/* Drag by the handle (4..52, 0..22): press at (20,10), move by (+100,+200). */
	inject_lines("motion_abs 20 10\nbutton 272 1\nmotion_abs 120 210\nbutton 272 0\n");
	/* The pointer rests on the handle, which would show it highlighted; move it
	 * away so the plain tab can be checked. */
	inject("motion_abs 700 500");
	settle_all();
	struct ppm img = screenshot();
	expect_px(&img, 110, 230, 255, 0, 0, 0, "W1 dragged to (104,222)");
	expect_px(&img, 104, 222, 255, 0, 0, 0, "W1's top-left corner");
	/* The tab is 94% opaque, so a little of W2 (green) behind it shows through. */
	expect_px(&img, 114, 206, 11, 30, 23, 4, "the handle moved with the window");
	free(img.rgb);

	/* The pull-back rule: drag it up past the top of the screen. The pointer
	 * stops at the screen edge (y=0), which would put the window at y=12, so it
	 * is held at 22: its notches stay on screen. */
	inject_lines("motion_abs 120 210\nbutton 272 1\nmotion_abs 120 -300\nbutton 272 0\n");
	inject("motion_abs 700 500");
	settle_all();
	img = screenshot();
	expect_px(&img, 150, 22, 255, 0, 0, 0, "window held at y=22");
	expect_px(&img, 150, 40, 255, 0, 0, 0, "window held at y=22");
	expect_px(&img, 114, 6, TAB, 4, "its handle is fully on screen");
	free(img.rgb);

	/* The same rule for keyboard moves ("move up" nudges 40px at a time). */
	mod_shift(KEY_H);
	mod_shift(KEY_H);
	settle_all();
	img = screenshot();
	expect_px(&img, 60, 22, 255, 0, 0, 0, "still at y=22 after moving left");
	free(img.rgb);
}

static void test_tiled_drag_and_drop(struct client *c, struct window *w1, struct window *w2)
{
	(void)c;
	/* Back into the grid: W2 on the left, W1 on the right. */
	mod_shift(KEY_SPACE);
	settle_all();
	expect_size(w1, 394, 574, true, "W1 tiled again");
	expect_size(w2, 394, 574, true, "W2");

	/* A click on a tiled window's handle (or a tiny wobble) must not pull it out. */
	inject_lines("motion_abs 420 10\nbutton 272 1\nmotion_abs 423 12\nbutton 272 0\n");
	settle_all();
	expect_size(w1, 394, 574, true, "a click on the handle leaves it tiled");

	/* Drag W1 (handle at x 402..450) down to the bottom edge of W2, but don't
	 * let go yet: the drop hint appears over W2's lower half. */
	inject_lines("motion_abs 420 10\nbutton 272 1\nmotion_abs 400 580\n");
	settle_all();
	CHECK(!w1->cfg_tiled);   /* lifted out of the grid while dragging */
	expect_size(w2, 792, 574, true, "W2 closes up while W1 is lifted out");
	struct ppm img = screenshot();
	/* W2 is green; the hint is a 45% translucent blue over its lower half
	 * (y 309..596): (0,255,0)*0.55 + (31,67,103) ~= (32,208,104). */
	expect_px(&img, 700, 400, 32, 208, 104, 8, "drop hint over W2's lower half");
	expect_px(&img, 700, 200, 0, 255, 0, 0, "no hint over W2's upper half");
	free(img.rgb);

	/* Let go: W1 lands below W2, and the column splits in two. */
	inject("button 272 0");
	settle_all();
	expect_size(w2, 792, 276, true, "W2 on top");
	expect_size(w1, 792, 276, true, "W1 below it");
	CHECK(w1->activated);
	img = screenshot();
	expect_px(&img, 400, 100, 0, 255, 0, 0, "W2 above");
	expect_px(&img, 400, 450, 255, 0, 0, 0, "W1 below");
	expect_px(&img, 400, 310, BG, 0, "22px gap between them, room for W1's notch");
	expect_px(&img, 14, 303, TAB, 4, "W1's handle sits in that gap, above W1");
	free(img.rgb);

	/* Drag it out onto the outer gap instead: it stays floating where it is. */
	inject_lines("motion_abs 20 310\nbutton 272 1\nmotion_abs 2 300\nbutton 272 0\n");
	settle_all();
	CHECK(!w1->cfg_tiled && w1->cfg_w == 792 && w1->cfg_h == 276);
	expect_size(w2, 792, 574, true, "W2 takes the whole area back");
	mod_shift(KEY_SPACE);   /* tile it again */
	settle_all();
	expect_size(w1, 792, 276, true, "tiled again");
}

static void test_fullscreen_hides_notches(struct window *w1)
{
	CHECK(w1->activated);
	mod(KEY_F);
	settle_all();
	struct ppm img = screenshot();
	expect_px(&img, 748, 10, 255, 0, 0, 0, "fullscreen: no notches, the window covers everything");
	expect_px(&img, 14, 6, 255, 0, 0, 0, "fullscreen: no handle");
	free(img.rgb);
	mod(KEY_F);
	settle_all();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-notch-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[500];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n"
		 "[layout]\ngap_outer = 4\ngap_inner = 4\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);

	struct window w1, w2;
	test_appearance(&c, &w1, &w2);
	test_hover_and_clicks(&c, &w1);
	test_float_drag_and_pull_back(&c, &w1, &w2);
	test_tiled_drag_and_drop(&c, &w1, &w2);
	test_fullscreen_hides_notches(&w1);

	(void)check_near;
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
	puts("test_notch: ok");
	return 0;
}
