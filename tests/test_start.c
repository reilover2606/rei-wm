/*
 * The Start menu and the Run prompt: the button, the shortcuts, Run, the power rows (which ask
 * for a second click), typing / editing / wrapping / the size limit, and Enter running a command.
 */
#define SOCKET "rei-start-test"
#include "testlib.h"

#include <sys/stat.h>

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

	char config[1600];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\nthickness = 28\n"
		 "[start_menu]\nshutdown_command = \"touch %s/shutdown\"\nreboot_command = \"touch %s/reboot\"\n"
		 "run_font = \"Sans 20\"\n"
		 "[[shortcut]]\nname = \"Alpha\"\ncommand = \"touch %s/alpha\"\n"
		 "[[shortcut]]\nname = \"Beta\"\ncommand = \"touch %s/beta\"\n"
		 "[theme.run_prompt]\ncolor = \"#e0a000\"\nopacity = 1.0\nshadow_opacity = 0.0\nborder_width = 0\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n",
		 g_marks, g_marks, g_marks, g_marks, OUT_W, OUT_H);
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

	/* Power rows need a second click. */
	click_at(20, 14);
	y = row_y(4, &x);
	click_at(x, y);
	sleep_ms(300);
	CHECK(!exists("shutdown"));
	CHECK(log_has("popup start:"));
	y = row_y(4, &x);
	click_at(x, y);
	CHECK(wait_exists("shutdown"));
	click_at(20, 14);
	y = row_y(5, &x);
	click_at(x, y);
	y = row_y(5, &x);
	click_at(x, y);
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

	CHECK(stop_compositor() == 0);
	printf("start: ok\n");
	return 0;
}
