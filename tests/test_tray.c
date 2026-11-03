/*
 * End-to-end test of the application tray and minimize/restore: tiles per application
 * with a stack for several windows, click to focus / hide / restore, the window list for
 * a stacked tile, and that a restored window returns to its place in the grid (or its
 * floating position).
 *
 * One 800x600 output, toolbar on top (28 px), gaps 4, notch height 22.
 */
#define SOCKET "rei-tray-test"
#include "testlib.h"

#define KEY_M 50
#define KEY_ESC 1
#define KEY_SPACE 57
#define OUT_W 800
#define OUT_H 600

#define RED 0x00ff0000
#define GREEN 0x0000ff00
#define BLUE 0x000000ff

struct tile {
	int x, y, w, h, count;
	bool active, minimized;
	char label[64];
};

/* The newest "tray OUT:" line of the compositor's log, parsed. */
static int read_tray(struct tile *tiles, int max)
{
	sleep_ms(200);
	char *log = slurp(g_log);
	CHECK(log);
	char *line = NULL;
	for (char *p = log; (p = strstr(p, "tray OUT:")); p += 9)
		line = p;
	int n = 0;
	if (line) {
		char *end = strchr(line, '\n');
		if (end)
			*end = '\0';
		for (char *p = line; (p = strchr(p, '[')) && n < max; p++) {
			struct tile *t = &tiles[n];
			int idx;
			char label[64] = "";
			if (sscanf(p, "[%d %d,%d %dx%d count=%d active=%d min=%d '", &idx, &t->x, &t->y, &t->w,
				   &t->h, &t->count, (int *)&t->active, (int *)&t->minimized) == 8) {
				char *q = strchr(p, '\'');
				if (q) {
					char *e2 = strrchr(q + 1, '\'');
					if (e2 && e2 > q) {
						size_t len = (size_t)(e2 - q - 1);
						if (len >= sizeof label)
							len = sizeof label - 1;
						memcpy(label, q + 1, len);
						label[len] = '\0';
					}
				}
				snprintf(t->label, sizeof t->label, "%s", label);
				n++;
			}
		}
	}
	free(log);
	return n;
}

static int g_ptr_x = -1;

static void click_at(int x, int y)
{
	char buf[128];
	snprintf(buf, sizeof buf, "motion_abs %d %d\nbutton 272 1\nbutton 272 0\n", x, y);
	inject_lines(buf);
	settle_all();
	g_ptr_x = x;
}

static void click_tile(const struct tile *t)
{
	click_at(t->x + t->w / 2, t->y + t->h / 2);
}

static uint32_t px_at(const struct ppm *img, int x, int y)
{
	const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-tray-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[500];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\nthickness = 28\n"
		 "[layout]\ngap_outer = 4\ngap_inner = 4\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);

	/* Three windows of two applications: A1 and A2 are "alpha", B1 is "beta". */
	struct window a1, b1, a2;
	g_app_id = "alpha";
	open_win(&c, &a1, "A1", RED);
	g_app_id = "beta";
	open_win(&c, &b1, "B1", GREEN);
	g_app_id = "alpha";
	open_win(&c, &a2, "A2", BLUE);
	settle_all();
	park_pointer();

	struct tile t[8];
	int n = read_tray(t, 8);
	CHECK(n == 2);
	CHECK(t[0].count == 2 && !t[0].minimized);   /* alpha: a stack of two */
	CHECK(t[1].count == 1 && !t[1].minimized);   /* beta */
	CHECK(strstr(t[1].label, "B1"));
	CHECK(t[0].active && !t[1].active);          /* A2, the newest, has the focus */
	/* The tiles are centred on the bar, below nothing and above the windows. */
	CHECK(t[0].y >= 0 && t[0].y + t[0].h <= 28);
	int mid = (t[0].x + t[1].x + t[1].w) / 2;
	CHECK(abs(mid - OUT_W / 2) <= 40);
	/* The stack peeks out: alpha's tile is wider than beta's by the peeking edge. */
	CHECK(t[0].w == t[1].w + 4);

	/* The three tiled windows left to right: red, green, blue. Remember where green is. */
	struct ppm img = screenshot();
	if (getenv("TRAY_TEST_SHOT")) {  /* For documentation: keep a picture. */
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s/rei-wm-OUT.ppm %s.tiles.ppm", g_dir, getenv("TRAY_TEST_SHOT"));
		CHECK(system(cmd) == 0);
	}
	CHECK(px_at(&img, 100, 300) == RED);
	CHECK(px_at(&img, 400, 300) == GREEN);
	CHECK(px_at(&img, 700, 300) == BLUE);
	free(img.rgb);
	int b_w = b1.cfg_w;

	/* 1. Clicking beta's tile (it is not focused) focuses it. */
	click_tile(&t[1]);
	CHECK(b1.activated && !a1.activated && !a2.activated);

	/* 2. Clicking it again (now focused) hides it; the others take its space. */
	click_tile(&t[1]);
	settle_all();
	n = read_tray(t, 8);
	CHECK(n == 2 && t[1].minimized && !t[1].active);
	CHECK(a1.cfg_w > b_w + 100 && a2.cfg_w > b_w + 100);
	img = screenshot();
	CHECK(px_at(&img, 200, 300) == RED);
	CHECK(px_at(&img, 600, 300) == BLUE);
	free(img.rgb);

	/* 3. Clicking it once more brings it back to the same place in the grid. */
	click_tile(&t[1]);
	settle_all();
	n = read_tray(t, 8);
	CHECK(n == 2 && !t[1].minimized && t[1].active);
	CHECK(b1.activated);
	img = screenshot();
	CHECK(px_at(&img, 100, 300) == RED);
	CHECK(px_at(&img, 400, 300) == GREEN);   /* middle again, not at an end */
	CHECK(px_at(&img, 700, 300) == BLUE);
	free(img.rgb);
	CHECK(b1.cfg_w == b_w);

	/* 4. The stacked tile opens a list of its windows; picking one focuses it. */
	click_tile(&t[0]);
	img = screenshot();
	if (getenv("TRAY_TEST_SHOT")) {
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s/rei-wm-OUT.ppm %s.menu.ppm", g_dir, getenv("TRAY_TEST_SHOT"));
		CHECK(system(cmd) == 0);
	}
	int row0_y = 28 + 2 + 1 + 13, row1_y = 28 + 2 + 1 + 26 + 13;
	/* Menu background is the toolbar colour (22,28,38), not any window's colour. */
	CHECK(near_px(&img, t[0].x + 20, row0_y + 8, 22, 28, 38, 12) ||
	      near_px(&img, t[0].x + t[0].w - 6, row0_y + 8, 22, 28, 38, 12));
	free(img.rgb);
	click_at(t[0].x + 20, row1_y);     /* the second window: A2 */
	CHECK(a2.activated && !a1.activated);
	img = screenshot();
	CHECK(!near_px(&img, t[0].x + t[0].w - 6, row0_y + 8, 22, 28, 38, 12));   /* the list closed */
	free(img.rgb);

	click_tile(&t[0]);
	click_at(t[0].x + 20, row0_y);     /* the first window: A1 */
	CHECK(a1.activated && !a2.activated);

	/* 5. Escape closes the list without picking anything. */
	click_tile(&t[0]);
	inject_lines("key 1 1\nkey 1 0\n");
	settle_all();
	img = screenshot();
	CHECK(!near_px(&img, t[0].x + t[0].w - 6, row0_y + 8, 22, 28, 38, 12));
	free(img.rgb);
	CHECK(a1.activated);

	/* 6. Minimize from the keyboard, restore from the tray. */
	park_pointer();
	mod(KEY_M);
	settle_all();
	n = read_tray(t, 8);
	CHECK(n == 2 && t[0].count == 2 && !t[0].minimized);   /* only one of alpha's windows is hidden */
	CHECK(!a1.activated);
	click_tile(&t[0]);                                      /* opens the list: A1 is listed as minimized */
	click_at(t[0].x + 20, row0_y);
	settle_all();
	CHECK(a1.activated);

	/* 7. A floating window's position is remembered. */
	click_tile(&t[1]);
	settle_all();
	mod_shift(KEY_SPACE);              /* float B1: it keeps its size and place */
	settle_all();
	inject_lines("motion_abs 400 300\nbutton 272 1\nbutton 272 0\n");   /* nothing special; just focus B1 */
	settle_all();
	park_pointer();
	img = screenshot();
	uint32_t before = px_at(&img, 400, 300);
	free(img.rgb);
	CHECK(before == GREEN);
	mod(KEY_M);
	settle_all();
	n = read_tray(t, 8);
	CHECK(n == 2 && t[1].minimized);
	click_tile(&t[1]);
	settle_all();
	park_pointer();
	img = screenshot();
	CHECK(px_at(&img, 400, 300) == GREEN);   /* back where it was */
	free(img.rgb);

	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int tt = 0;; tt += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && tt < 5000);
		sleep_ms(10);
	}
	wl_display_disconnect(c.d);
	puts("tray: ok");
	return 0;
}
