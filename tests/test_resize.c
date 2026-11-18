/*
 * Resize mode and the workspace key that goes back:
 *   - Mod+Shift+r enters resize mode: i j k l move the focused tile's dividers (up, left, down, right), the toolbar's mode
 *     switch shows an R, other keys are typed as usual, and Escape (or Enter) leaves it, after which i j k l are typed;
 *   - Mod+<n> on workspace n goes back to the previous workspace, so one key switches between two.
 */
#define SOCKET "rei-resize-test"
#include "testlib.h"

#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_R 19
#define KEY_I 23
#define KEY_J 36
#define KEY_K 37
#define KEY_L 38
#define RED 0x00ff0000
#define GREEN 0x0000ff00

static void key(int code)
{
	inject("key %d 1", code);
	inject("key %d 0", code);
	settle_all();
}

static void expect_within(int got, int want, int tol, const char *what)
{
	if (abs(got - want) > tol) {
		fprintf(stderr, "%s: got %d, expected %d +-%d\n", what, got, want, tol);
		exit(1);
	}
}

/* The part of the bar left of the workspace buttons: the Start button and the mode switch. */
static struct ppm bar_left(void)
{
	return screenshot();
}

static int differing(const struct ppm *a, const struct ppm *b)
{
	int n = 0;
	for (int y = 0; y < 28; y++)
		for (int x = 0; x < 100; x++) {
			const uint8_t *p = a->rgb + ((size_t)y * (size_t)a->w + (size_t)x) * 3;
			const uint8_t *q = b->rgb + ((size_t)y * (size_t)b->w + (size_t)x) * 3;
			n += abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]) > 30;
		}
	return n;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-resize-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1],
			 "[toolbar]\nenabled = true\nsysinfo = false\nbattery = false\nwifi = false\n"
			 "[general]\nfocus_follows_mouse = false\n"
			 "[layout]\ngap_outer = 4\ngap_inner = 4\nresize_step = 40\n"
			 "[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n",
			 true);
	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);
	inject("motion_abs 700 500");
	settle_all();

	struct window w1, w2;
	open_win(&c, &w1, "W1", RED);
	open_win(&c, &w2, "W2", GREEN);
	CHECK(w2.activated);
	int a0 = w1.cfg_w, b0 = w2.cfg_w;
	expect_within(a0, b0, 2, "two equal tiles");
	struct ppm before = bar_left();
	if (getenv("REI_SHOT_DIR")) {
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/resize-normal.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}

	/* ---- Resize mode ---- */
	chord(KEY_SUPER, KEY_SHIFT, KEY_R);
	settle_all();
	struct ppm during = bar_left();
	if (getenv("REI_SHOT_DIR")) {
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/resize-mode.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}
	CHECK(differing(&before, &during) > 20);   /* the mode switch shows an R instead of its icon */
	free(during.rgb);

	ev_clear(&c);
	key(KEY_J);   /* W2 (right): its left divider moves left: W2 grows, W1 shrinks, by one step (40 px) */
	expect_within(w1.cfg_w, a0 - 40, 3, "W1 after j");
	expect_within(w2.cfg_w, b0 + 40, 3, "W2 after j");
	key(KEY_L);   /* and back: W2's left divider moves right */
	expect_within(w1.cfg_w, a0, 3, "W1 after l");
	expect_within(w2.cfg_w, b0, 3, "W2 after l");
	int h = w2.cfg_h;
	key(KEY_I);   /* no horizontal divider: nothing moves (and nothing is typed) */
	key(KEY_K);
	CHECK(w2.cfg_h == h && w1.cfg_w == w1.cfg_w);
	CHECK(ev_count(&c, EV_KB_KEY) == 0);   /* the keys were the mode's */
	/* Shift: three steps. */
	inject("key 42 1");
	key(KEY_J);
	inject("key 42 0");
	settle_all();
	expect_within(w2.cfg_w, b0 + 120, 4, "W2 after Shift+j");
	key(KEY_L);
	key(KEY_L);
	key(KEY_L);
	expect_within(w2.cfg_w, b0, 4, "W2 back after three steps");

	/* Escape leaves the mode: the indicator goes, and the keys are typed again. */
	key(KEY_ESC);
	struct ppm after = bar_left();
	CHECK(differing(&before, &after) == 0);
	free(after.rgb);
	free(before.rgb);
	ev_clear(&c);
	key(KEY_J);
	CHECK(ev_count(&c, EV_KB_KEY) >= 2);   /* typed into the window */
	expect_within(w2.cfg_w, b0, 3, "no resizing outside the mode");

	/* ---- The workspace key goes back ---- */
	chord(KEY_SUPER, 0, KEY_2);   /* to workspace 2 (empty: the windows are hidden) */
	settle_all();
	CHECK(!w2.activated);
	chord(KEY_SUPER, 0, KEY_2);   /* again: back to workspace 1 */
	settle_all();
	CHECK(w2.activated);
	chord(KEY_SUPER, 0, KEY_1);   /* on workspace 1: back to the previous one, 2 */
	settle_all();
	CHECK(!w2.activated);
	chord(KEY_SUPER, 0, KEY_2);   /* on 2: back to 1 */
	settle_all();
	CHECK(w2.activated);

	CHECK(stop_compositor() == 0);
	puts("resize: ok");
	return 0;
}
