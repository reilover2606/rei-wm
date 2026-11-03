/*
 * The tray's interaction: the hover highlight (immediate), the tooltip (after half a second,
 * fading in), dragging tiles to reorder them, the overflow tile when the middle three fifths
 * are full, and the 32-window cap that sends new windows to the next workspace.
 *
 * Output 800x600 (the tray's range is x 160..640), toolbar on top, new windows float.
 */
#define SOCKET "rei-trayui-test"
#include "testlib.h"

#define OUT_W 800
#define OUT_H 600

struct tile {
	int x, y, w, h, count;
	char label[64];
};

struct tray {
	struct tile tiles[32];
	int n;
	bool overflow;
	int ox, oy, ow, oh, owindows;
};

static struct tray read_tray(void)
{
	sleep_ms(200);
	struct tray t = { 0 };
	char *log = slurp(g_log);
	CHECK(log);
	char *line = NULL;
	for (char *p = log; (p = strstr(p, "tray OUT:")); p += 9)
		line = p;
	if (line) {
		char *end = strchr(line, '\n');
		if (end)
			*end = '\0';
		for (char *p = line; (p = strchr(p, '[')) && t.n < 32; p++) {
			struct tile *tl = &t.tiles[t.n];
			int idx, active, min;
			if (sscanf(p, "[%d %d,%d %dx%d count=%d active=%d min=%d '", &idx, &tl->x, &tl->y, &tl->w, &tl->h,
				   &tl->count, &active, &min) == 8) {
				char *q = strchr(p, '\'');
				char *e2 = q ? strchr(q + 1, '\'') : NULL;
				if (e2) {
					size_t len = (size_t)(e2 - q - 1);
					memcpy(tl->label, q + 1, len < 63 ? len : 63);
				}
				t.n++;
			}
		}
		const char *o = strstr(line, "{overflow ");
		if (o && sscanf(o, "{overflow %d,%d %dx%d windows=%d}", &t.ox, &t.oy, &t.ow, &t.oh, &t.owindows) == 5)
			t.overflow = true;
	}
	free(log);
	return t;
}

static void move_to(int x, int y)
{
	char buf[64];
	snprintf(buf, sizeof buf, "motion_abs %d %d\n", x, y);
	inject_lines(buf);
	settle_all();
}

static void click_at(int x, int y)
{
	char buf[128];
	snprintf(buf, sizeof buf, "motion_abs %d %d\nbutton 272 1\nbutton 272 0\n", x, y);
	inject_lines(buf);
	settle_all();
}

static int lum(const struct ppm *img, int x, int y)
{
	const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
	return p[0] + p[1] + p[2];
}

static int g_nwin;
static struct window g_win[48];
static char g_names[48][16];

static struct window *open_app(const char *app, const char *title)
{
	CHECK(g_nwin < 48);
	g_app_id = app;
	struct window *w = &g_win[g_nwin++];
	open_win(g_clients[0], w, title, 0x00808080);
	return w;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	setenv("REI_TRAY_FADE_MS", "1600", 1);  /* a slow fade, so a half-faded frame can be seen */
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-trayui-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[500];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\nthickness = 28\n[layout]\nnew_windows = \"float\"\n"
		 "[theme.tooltip]\ncolor = \"#ffffff\"\nopacity = 1.0\nshadow_opacity = 0.0\nborder_width = 0\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);
	client_bind_pointer(&c);

	struct window *wa = open_app("app-a", "AA");
	struct window *wb = open_app("app-b", "BB");
	struct window *wc = open_app("app-c", "CC");
	park_pointer();
	struct tray t = read_tray();
	CHECK(t.n == 3 && !t.overflow);
	CHECK(strstr(t.tiles[0].label, "AA") && strstr(t.tiles[1].label, "BB") && strstr(t.tiles[2].label, "CC"));
	CHECK(t.tiles[1].w == 150);   /* natural width: nothing has needed to shrink */

	/* ---- Hover: the highlight is immediate, the tooltip is not, and it fades in ---- */
	struct tile b = t.tiles[1];
	int fill_x = b.x + b.w - 10, fill_y = b.y + 4;      /* the tile's own fill, clear of its icon and label */
	int tip_x = b.x + b.w / 2, tip_y = 28 + 4 + 10;     /* inside the tooltip's box, below the bar */
	struct ppm base = screenshot();
	int fill0 = lum(&base, fill_x, fill_y), tip0 = lum(&base, tip_x, tip_y);

	move_to(b.x + b.w / 2, b.y + b.h / 2);
	struct ppm early = screenshot();               /* well under half a second */
	CHECK(lum(&early, fill_x, fill_y) > fill0 + 12);   /* lighter at once */
	CHECK(lum(&early, tip_x, tip_y) == tip0);          /* no tooltip yet */
	CHECK(!log_has("tray tooltip:"));
	free(early.rgb);

	sleep_ms(1100);                                    /* 0.5 s delay + about 0.4 s into the 1.6 s fade */
	struct ppm mid = screenshot();
	int mid_diff = abs(lum(&mid, tip_x, tip_y) - tip0);
	free(mid.rgb);
	CHECK(log_has("tray tooltip: BB"));
	sleep_ms(2200);                                    /* fully faded in */
	struct ppm full = screenshot();
	int full_diff = abs(lum(&full, tip_x, tip_y) - tip0);
	free(full.rgb);
	CHECK(full_diff > 300);                            /* a (white) tooltip is there */
	CHECK(mid_diff > 30 && mid_diff < full_diff - 60);  /* and it was partway when we looked */

	move_to(795, 595);                                 /* away: highlight and tooltip both go */
	struct ppm gone = screenshot();
	CHECK(lum(&gone, fill_x, fill_y) == fill0 && lum(&gone, tip_x, tip_y) == tip0);
	free(gone.rgb);
	free(base.rgb);

	/* ---- Dragging a tile reorders the tray without clicking it ---- */
	CHECK(wc->activated);
	t = read_tray();
	struct tile a = t.tiles[0], cc = t.tiles[2];
	move_to(a.x + a.w / 2, a.y + a.h / 2);
	inject_lines("button 272 1\n");
	settle_all();
	for (int x = a.x + a.w / 2; x <= cc.x + cc.w - 6; x += 30) {
		char buf[64];
		snprintf(buf, sizeof buf, "motion_abs %d %d\n", x, a.y + a.h / 2);
		inject_lines(buf);
	}
	settle_all();
	struct tray during = read_tray();
	inject_lines("button 272 0\n");
	settle_all();
	(void)during;
	t = read_tray();
	CHECK(t.n == 3);
	CHECK(strstr(t.tiles[0].label, "BB") && strstr(t.tiles[1].label, "CC") && strstr(t.tiles[2].label, "AA"));
	CHECK(wc->activated && !wa->activated);            /* it was a drag, not a click on AA */

	/* ...and back to the front. */
	struct tile last = t.tiles[2], first = t.tiles[0];
	move_to(last.x + last.w / 2, last.y + last.h / 2);
	inject_lines("button 272 1\n");
	settle_all();
	for (int x = last.x + last.w / 2; x >= first.x - 20; x -= 30) {
		char buf[64];
		snprintf(buf, sizeof buf, "motion_abs %d %d\n", x, last.y + last.h / 2);
		inject_lines(buf);
	}
	settle_all();
	inject_lines("button 272 0\n");
	settle_all();
	t = read_tray();
	CHECK(strstr(t.tiles[0].label, "AA") && strstr(t.tiles[1].label, "BB") && strstr(t.tiles[2].label, "CC"));
	park_pointer();
	(void)wb;

	/* ---- Overflow: eight applications do not fit in 160..640 even at half width ---- */
	const char *apps[] = { "app-d", "app-e", "app-f", "app-g", "app-h" };
	const char *titles[] = { "DD", "EE", "FF", "GG", "HH" };
	struct window *extra[5];
	for (int i = 0; i < 5; i++)
		extra[i] = open_app(apps[i], titles[i]);
	park_pointer();
	t = read_tray();
	CHECK(t.overflow && t.n == 5 && t.owindows == 3);
	CHECK(t.tiles[0].w == 75);                         /* shrunk to exactly half */
	CHECK(t.tiles[0].x >= 160 && t.ox + t.ow <= 640);  /* the middle three fifths */

	/* The overflow tile opens a list of what is under it; picking a row focuses that window. */
	struct ppm before = screenshot();
	click_at(t.ox + t.ow / 2, t.oy + t.oh / 2);
	struct ppm open = screenshot();
	int row1_y = 28 + 2 + 1 + 26 + 13;
	CHECK(abs(lum(&open, t.ox + 12, row1_y + 8) - lum(&before, t.ox + 12, row1_y + 8)) > 20);
	free(open.rgb);
	free(before.rgb);
	click_at(t.ox + 12, row1_y);                       /* second row: GG */
	CHECK(extra[3]->activated);

	/* Hovering the overflow tile explains it. */
	move_to(t.ox + t.ow / 2, t.oy + t.oh / 2);
	sleep_ms(800);
	CHECK(log_has("tray tooltip: 3 more windows"));
	park_pointer();

	/* ---- The cap: with 32 windows under the overflow tile, the next one goes to workspace 2 ---- */
	int n_app = 13;   /* five visible tiles + 32 overflowing = 37 applications in all; 8 already exist */
	(void)n_app;
	for (int i = 0; i < 29; i++) {
		snprintf(g_names[i], sizeof g_names[i], "app-x%d", i);
		open_app(g_names[i], g_names[i]);
	}
	park_pointer();
	t = read_tray();
	CHECK(t.overflow && t.owindows == 32);
	CHECK(!log_has("tray: workspace 1 is full"));

	snprintf(g_names[29], sizeof g_names[29], "app-last");
	struct window *pushed = open_app(g_names[29], "LAST");
	park_pointer();
	CHECK(log_has("tray: workspace 1 is full; the new window opens on workspace 2"));
	t = read_tray();
	CHECK(t.n == 1 && !t.overflow && strstr(t.tiles[0].label, "LAST"));   /* workspace 2 has its own tray */
	CHECK(pushed->activated);

	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int tt = 0;; tt += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && tt < 8000);
		sleep_ms(10);
	}
	wl_display_disconnect(c.d);
	puts("tray ui: ok");
	return 0;
}
