/*
 * Thin mode: Mod+t narrows the tiling grid of the current workspace by [layout] thin_shrink percent and centres it, so
 * the wallpaper shows at the sides. Floating windows are not affected, other workspaces are not affected, and pressing
 * the key again restores the grid.
 */
#define SOCKET "rei-thin-test"
#include "testlib.h"

#define KEY_2 3
#define KEY_T 20
#define KEY_SPACE 57
#define RED 0x00ff0000
#define GREEN 0x0000ff00
#define BLUE 0x000000ff

static void expect_within(int got, int want, int tol, const char *what)
{
	if (abs(got - want) > tol) {
		fprintf(stderr, "%s: got %d, expected %d +-%d\n", what, got, want, tol);
		exit(1);
	}
}

static bool is_red(const struct ppm *p, int x, int y)
{
	const uint8_t *q = p->rgb + ((size_t)y * (size_t)p->w + (size_t)x) * 3;
	return q[0] > 200 && q[1] < 60 && q[2] < 60;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-thin-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1],
			 "[toolbar]\nenabled = true\nsysinfo = false\nbattery = false\nwifi = false\n"
			 "[general]\nfocus_follows_mouse = false\n"
			 "[layout]\ngap_outer = 4\ngap_inner = 4\nthin_shrink = 50\n"
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
	int full = w1.cfg_w + w2.cfg_w;
	expect_within(full, 800 - 8 - 4, 3, "the grid fills the screen");
	struct ppm img = screenshot();
	CHECK(is_red(&img, 12, 300));   /* the left tile reaches the left edge */
	free(img.rgb);

	mod(KEY_T);
	settle_all();
	expect_within(w1.cfg_w + w2.cfg_w, (800 - 8) / 2 - 4, 4, "thin mode halves the grid (shrink 50)");
	CHECK(w1.cfg_h > 300);   /* heights are untouched */
	img = screenshot();
	CHECK(!is_red(&img, 12, 300));          /* the wallpaper shows at the left... */
	CHECK(!is_red(&img, 790, 300));         /* ...and the right */
	CHECK(is_red(&img, 800 / 2 - 60, 300)); /* the column is in the middle */
	free(img.rgb);

	/* Another workspace is not thin. */
	mod(KEY_2);
	settle_all();
	struct window w3;
	open_win(&c, &w3, "W3", BLUE);
	expect_within(w3.cfg_w, 800 - 8, 3, "other workspaces are not thin");
	mod(KEY_2);   /* back to workspace 1 (back and forth) */
	settle_all();
	expect_within(w1.cfg_w + w2.cfg_w, (800 - 8) / 2 - 4, 4, "workspace 1 is still thin");

	/* A floating window keeps its size while the grid changes. */
	mod_shift(KEY_SPACE);   /* W2 floats; W1 is alone in the grid */
	settle_all();
	expect_within(w1.cfg_w, (800 - 8) / 2, 4, "a lone tile fills the thin column");
	mod(KEY_T);
	settle_all();
	expect_within(w1.cfg_w, 800 - 8, 3, "thin mode off: the grid is full width again");
	puts("thin: ok");
	return 0;
}
