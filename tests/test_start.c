/*
 * The Start menu and the Run prompt: the button, the shortcuts, Run, the power rows (which ask
 * for a second click), typing / editing / wrapping / the size limit, and Enter running a command.
 */
#define SOCKET "rei-start-test"
#include "testlib.h"

#include <sys/stat.h>

#include "pngw.h"

#define OUT_W 800
#define OUT_H 600

static char g_marks[160];

static bool exists(const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "%s/%s", g_marks, name);
	struct stat st;
	return stat(path, &st) == 0;
}

static bool wait_exists(const char *name)
{
	for (int i = 0; i < 40; i++) {
		if (exists(name))
			return true;
		sleep_ms(100);
	}
	return false;
}

static void click_at(int x, int y)
{
	char buf[128];
	snprintf(buf, sizeof buf, "motion_abs %d %d\nbutton 272 1\nbutton 272 0\n", x, y);
	inject_lines(buf);
	settle_all();
}

/* Row centres from the last "popup start:" log line. */
static int row_y(int row, int *x)
{
	char *log = slurp(g_log);
	CHECK(log);
	char *line = NULL;
	for (char *p = log; (p = strstr(p, "popup start:")); p += 12)
		line = p;
	CHECK(line);
	char *end = strchr(line, '\n');
	if (end)
		*end = '\0';
	int px, pw;
	CHECK(sscanf(line, "popup start: x=%d w=%d", &px, &pw) == 2);
	if (x)
		*x = px + pw / 2;
	char tag[16];
	snprintf(tag, sizeof tag, "[%d y=", row);
	char *r = strstr(line, tag);
	CHECK(r);
	int y = atoi(r + strlen(tag));
	free(log);
	return y;
}

static int lum(const struct ppm *img, int x, int y)
{
	const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
	return p[0] + p[1] + p[2];
}

/* The number of pixels that differ between two screenshots, within a box. */
static int diff_px(const struct ppm *a, const struct ppm *b, int x0, int y0, int x1, int y1)
{
	int n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			if (abs(lum(a, x, y) - lum(b, x, y)) > 30)
				n++;
	return n;
}

static void type(const char *keys)  /* evdev codes, space-separated */
{
	char buf[1024] = "";
	for (const char *p = keys; *p;) {
		if (strlen(buf) > 900) {   /* flush in pieces */
			inject_lines(buf);
			buf[0] = '\0';
		}
		int code, n;
		if (sscanf(p, "%d%n", &code, &n) != 1)
			break;
		p += n;
		char one[64];
		snprintf(one, sizeof one, "key %d 1\nkey %d 0\n", code, code);
		strcat(buf, one);
	}
	inject_lines(buf);
	settle_all();
}

/* With REI_SHOT_DIR set, saves the screen as a PPM there (for looking at, not for checking). */
static void shot(const char *name)
{
	const char *dir = getenv("REI_SHOT_DIR");
	if (!dir)
		return;
	struct ppm img = screenshot();
	char path[256];
	snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
	FILE *f = fopen(path, "wb");
	CHECK(f);
	fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
	fwrite(img.rgb, 3, (size_t)img.w * (size_t)img.h, f);
	fclose(f);
	free(img.rgb);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	setenv("REI_PROMPT_BLINK_MS", "250", 1);
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-start-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_marks, sizeof g_marks, "%s/marks", g_dir);
	CHECK(mkdir(g_marks, 0700) == 0);

	/* A fake /proc so the CPU/RAM box has known numbers. */
	char proc[200];
	snprintf(proc, sizeof proc, "%s/proc", g_dir);
	CHECK(mkdir(proc, 0700) == 0);
	{
		char path[260];
		snprintf(path, sizeof path, "%s/stat", proc);
		write_text(path, "cpu  100 0 100 700 100 0 0 0\n");
		snprintf(path, sizeof path, "%s/meminfo", proc);
		write_text(path, "MemTotal: 16777216 kB\nMemAvailable: 11324621 kB\n");
	}
	setenv("REI_PROC_DIR", proc, 1);
	setenv("REI_SYSINFO_POLL_MS", "200", 1);

	/* Flavor icons: two solid, opaque 32x32 images (magenta, cyan), and two that must be rejected. */
	crc_init();
	char flavor[200];
	snprintf(flavor, sizeof flavor, "%s/flavor", g_dir);
	CHECK(mkdir(flavor, 0700) == 0);
	{
		char path[260];
		snprintf(path, sizeof path, "%s/magenta.png", flavor);
		write_png(path, 32, 32, 255, 0, 255, 255);
		snprintf(path, sizeof path, "%s/cyan.png", flavor);
		write_png(path, 32, 32, 0, 255, 255, 255);
		snprintf(path, sizeof path, "%s/toobig.png", flavor);
		write_png(path, 128, 128, 255, 255, 0, 255);
		snprintf(path, sizeof path, "%s/notsquare.png", flavor);
		write_png(path, 32, 20, 255, 255, 0, 255);
	}

	char config[1900];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\nthickness = 28\n"
		 "[start_menu]\nshutdown_command = \"touch %s/shutdown\"\nreboot_command = \"touch %s/reboot\"\n"
		 "run_font = \"Sans 20\"\n"
		 "[[shortcut]]\nname = \"Alpha\"\ncommand = \"touch %s/alpha\"\n"
		 "[[shortcut]]\nname = \"Beta\"\ncommand = \"touch %s/beta\"\n"
		 "[theme]\nflavor_icons = \"%s\"\n"
		 "[theme.run_prompt]\ncolor = \"#e0a000\"\nopacity = 1.0\nshadow_opacity = 0.0\nborder_width = 0\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n",
		 g_marks, g_marks, g_marks, g_marks, flavor, OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);
	client_bind_pointer(&c);
	settle_all();

	/* Shortcuts first, then Run, then the power unit: Alpha, Beta, Run, Logout, Shutdown, Reboot. */
	struct ppm closed = screenshot();
	click_at(20, 14);
	CHECK(log_has("popup start:"));
	struct ppm open = screenshot();
	CHECK(diff_px(&closed, &open, 0, 28, 300, 300) > 500);   /* a menu appeared under the button */
	free(closed.rgb);
	free(open.rgb);

	int x;
	click_at(0, 0);   /* a press outside closes it and does nothing else */
	CHECK(!exists("alpha"));

	/* A shortcut runs its command and closes the menu. */
	click_at(20, 14);
	click_at(0 + 0, 0);
	click_at(20, 14);
	int y = row_y(0, &x);
	click_at(x, y);
	CHECK(wait_exists("alpha"));
	CHECK(!exists("beta"));

	click_at(20, 14);
	y = row_y(1, &x);
	click_at(x, y);
	CHECK(wait_exists("beta"));

	/* Power rows ask in a dialog. Cancel is highlighted at first, so Enter alone is safe. */
	int dlg_confirm_x = 511, dlg_cancel_x = 367, dlg_y = 342;
	click_at(20, 14);
	y = row_y(4, &x);
	click_at(x, y);
	CHECK(log_count("confirm: Shut down?") == 1);
	type("28");                                  /* Enter */
	sleep_ms(300);
	CHECK(!exists("shutdown"));
	CHECK(log_has("confirm: cancelled"));

	click_at(20, 14);
	y = row_y(4, &x);
	click_at(x, y);
	type("1");                                   /* Escape */
	CHECK(log_count("confirm: cancelled") == 2);

	click_at(20, 14);
	y = row_y(4, &x);
	click_at(x, y);
	click_at(dlg_cancel_x, dlg_y);               /* the Cancel button */
	CHECK(log_count("confirm: cancelled") == 3);
	CHECK(!exists("shutdown"));

	click_at(20, 14);
	y = row_y(4, &x);
	click_at(x, y);
	type("15");                                  /* Tab: highlights the confirm button */
	type("28");                                  /* Enter */
	CHECK(wait_exists("shutdown"));

	click_at(20, 14);
	y = row_y(5, &x);
	click_at(x, y);
	CHECK(log_count("confirm: Restart?") == 1);
	click_at(dlg_confirm_x, dlg_y);              /* the Restart button */
	CHECK(wait_exists("reboot"));

	/* Run opens the prompt: a box in the middle that blinks. */
	struct ppm base = screenshot();
	click_at(20, 14);
	y = row_y(2, &x);
	click_at(x, y);
	CHECK(log_has("run prompt: open"));
	struct ppm p1 = screenshot();
	CHECK(diff_px(&base, &p1, 150, 250, 650, 350) > 2000);   /* the box */
	CHECK(near_px(&p1, 400, 260, 0xe0, 0xa0, 0x00, 4) || near_px(&p1, 400, 300, 0xe0, 0xa0, 0x00, 4));
	/* The cursor blinks: over a few frames the box differs. */
	bool changed = false;
	for (int i = 0; i < 8 && !changed; i++) {
		sleep_ms(130);
		struct ppm p2 = screenshot();
		changed = diff_px(&p1, &p2, 150, 250, 650, 350) > 10;
		free(p2.rgb);
	}
	CHECK(changed);
	free(p1.rgb);

	/* Typing "touch <dir>/ran" would need many keys; type a short command instead: `:` is a no-op.
	 * Check editing: type "ab", backspace, Ctrl+U, then escape closes without running. */
	type("30 48");           /* a b */
	type("14");              /* backspace */
	inject_lines("key 29 1\nkey 22 1\nkey 22 0\nkey 29 0\n");  /* Ctrl+U */
	settle_all();
	type("1");               /* Escape */
	struct ppm gone = screenshot();
	CHECK(diff_px(&base, &gone, 150, 250, 650, 350) < 50);
	free(gone.rgb);
	free(base.rgb);

	/* A command typed with Mod+r: "touch" needs a path; use the shell's `>` instead: not typeable
	 * by keycode simply, so run a two-letter command that creates nothing but must not crash. */
	inject_lines("key 125 1\nkey 19 1\nkey 19 0\nkey 125 0\n");
	settle_all();
	CHECK(log_count("run prompt: open") == 2);
	/* Fill it: holding 'm' wraps and grows the box, then stops accepting text. */
	char many[4096] = "";
	for (int i = 0; i < 400; i++)
		strcat(many, "50 ");
	type(many);
	struct ppm full = screenshot();
	int top = -1, bottom = -1;
	for (int yy = 0; yy < OUT_H; yy++)
		if (near_px(&full, 400, yy, 0xe0, 0xa0, 0x00, 4)) {
			if (top < 0)
				top = yy;
			bottom = yy;
		}
	CHECK(top >= 0);
	CHECK(bottom - top > 150);                       /* it grew past one line */
	CHECK(bottom - top < OUT_H * 6 / 10 + 40);       /* but stopped at the limit */
	CHECK(abs((top + bottom) / 2 - OUT_H / 2) < 30); /* and stayed centred */
	free(full.rgb);

	/* Enter runs it (as a shell command: it fails, harmlessly) and closes the prompt. */
	type("28");
	CHECK(log_has("run prompt: mmm"));
	sleep_ms(200);
	struct ppm after = screenshot();
	CHECK(!near_px(&after, 400, 300, 0xe0, 0xa0, 0x00, 4));
	free(after.rgb);

	/* Flavor icons: the rejected files are reported, the Start button and menu rows show an icon. */
	CHECK(log_has("flavor icons: toobig.png: 128x128 is larger than 64x64; skipped"));
	CHECK(log_has("flavor icons: notsquare.png: 32x20 is not square (1:1); skipped"));
	CHECK(log_has("flavor icons: 2 usable"));
	{
		struct ppm img = screenshot();
		bool m = false, c = false;
		for (int y = 4; y < 24; y++)
			for (int x = 2; x < 30; x++) {
				m |= near_px(&img, x, y, 255, 0, 255, 6);
				c |= near_px(&img, x, y, 0, 255, 255, 6);
			}
		CHECK(m || c);   /* the Start button's icon, at its front */
		free(img.rgb);
		click_at(20, 14);
		int rx, ry = row_y(0, &rx);
		img = screenshot();
		bool row_icon = false;
		for (int y = ry - 9; y < ry + 9; y++)
			for (int x = 6; x < 30; x++)
				row_icon |= near_px(&img, x, y, 255, 0, 255, 6) || near_px(&img, x, y, 0, 255, 255, 6);
		CHECK(row_icon);   /* ...and the first menu row's */
		free(img.rgb);
		click_at(0, 0);
	}

	/* The CPU/RAM box: it is drawn left of the clock, and follows /proc. */
	{
		struct ppm a = screenshot();
		char path[260];
		snprintf(path, sizeof path, "%s/stat", proc);
		write_text(path, "cpu  300 0 300 700 100 0 0 0\n");   /* 500 busy of 600 more: 83% */
		snprintf(path, sizeof path, "%s/meminfo", proc);
		write_text(path, "MemTotal: 16777216 kB\nMemAvailable: 2097152 kB\n");
		sleep_ms(700);
		struct ppm b = screenshot();
		CHECK(diff_px(&a, &b, 560, 2, 700, 26) > 20);   /* the numbers changed */
		/* The box has a fill that differs from the plain bar behind it. */
		CHECK(!near_px(&b, 600, 4, 22, 28, 38, 3));
		free(a.rgb);
		free(b.rgb);
	}

	/* Hover glow: a hovered menu row is ringed by light that fades outward. */
	{
		struct ppm quiet = screenshot();
		click_at(20, 14);
		int rx;
		int ry = row_y(1, &rx);
		inject_lines("motion_abs 90 1\n");   /* off the rows, still on the bar */
		settle_all();
		struct ppm before = screenshot();
		char mv[64];
		snprintf(mv, sizeof mv, "motion_abs %d %d\n", rx, ry);
		inject_lines(mv);
		settle_all();
		struct ppm hov = screenshot();
		/* Just outside the menu's right edge (x = 180), beside the hovered row, light has appeared. */
		CHECK(lum(&hov, 183, ry) > lum(&before, 183, ry) + 20);
		click_at(0, 0);
		free(quiet.rgb);
		free(before.rgb);
		free(hov.rgb);
	}

	/* Looking around (only with REI_SHOT_DIR): windows, the menu, the dialog, the prompt, fullscreen. */
	struct window wt, wf, wt2;
	open_win(&c, &wt, "T", 0x3a7bd5);
	CHECK(wt.cfg_tiled);
	open_win(&c, &wt2, "T2", 0x7bd53a);
	CHECK(wt2.cfg_tiled);
	click_at(68 + 14, 14);      /* The tiled/floating switch decides how new windows open. */
	CHECK(log_has("new windows: floating"));
	open_win(&c, &wf, "F", 0xd5a03a);
	CHECK(!wf.cfg_tiled);
	click_at(68 + 14, 14);
	CHECK(log_has("new windows: tiled"));
	struct window wt3;
	open_win(&c, &wt3, "T3", 0xaa55cc);
	CHECK(wt3.cfg_tiled);
	inject_lines("motion_abs 100 400\n");
	settle_all();
	shot("1-desktop");
	click_at(20, 14);
	{
		int hx, hy = row_y(1, &hx);
		char mv[64];
		snprintf(mv, sizeof mv, "motion_abs %d %d\n", hx, hy);
		inject_lines(mv);
		settle_all();
		shot("2-start-menu");
	}
	y = row_y(4, &x);
	click_at(x, y);
	shot("3-confirm-dialog");
	type("1");
	click_at(20, 14);
	y = row_y(2, &x);
	click_at(x, y);
	type("33 23 19 18 33 24 45 57 24 49 33");   /* "firefox ofe" */
	shot("4-run-prompt");
	type("1");
	inject_lines("motion_abs 400 300\n");
	settle_all();
	inject_lines("key 125 1\nkey 33 1\nkey 33 0\nkey 125 0\n");   /* Mod+f */
	settle_all();
	shot("5-fullscreen");
	inject_lines("key 125 1\nkey 33 1\nkey 33 0\nkey 125 0\n");
	settle_all();

	CHECK(stop_compositor() == 0);
	printf("start: ok\n");
	return 0;
}
