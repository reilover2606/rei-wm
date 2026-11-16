/*
 * The message column: what a command typed into the Run prompt prints becomes a message (and no
 * output, no message); it shows at the right edge, fades away after its time; Mod+n opens the
 * whole stack with a backdrop; the wheel scrolls it; Escape closes it. Also the text clean-up.
 */
#define SOCKET "rei-msgbuf-test"
#include "testlib.h"

#include "msgbuf.h"

static int lum(const struct ppm *img, int x, int y)
{
	const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
	return p[0] + p[1] + p[2];
}

/* How many pixels in the column area differ from a reference picture. */
static int diff(const struct ppm *a, const struct ppm *b, int x0, int y0, int x1, int y1)
{
	int n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			if (abs(lum(a, x, y) - lum(b, x, y)) > 40)
				n++;
	return n;
}

static void type(const char *keys)   /* evdev codes, space separated */
{
	char buf[1024] = "";
	for (const char *p = keys; *p;) {
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

/* Types lowercase letters and spaces. */
static void type_text(const char *text)
{
	static const char *rows[] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
	static const int first[] = { 16, 30, 44 };
	for (const char *p = text; *p; p++) {
		char one[64];
		int code = *p == ' ' ? 57 : 0;
		for (int r = 0; r < 3 && !code; r++) {
			const char *at = strchr(rows[r], *p);
			if (at)
				code = first[r] + (int)(at - rows[r]);
		}
		CHECK(code);
		snprintf(one, sizeof one, "key %d 1\nkey %d 0\n", code, code);
		inject_lines(one);
	}
	settle_all();
}

static void run_command(const char *keys)
{
	inject_lines("key 125 1\nkey 19 1\nkey 19 0\nkey 125 0\n");   /* Mod+r */
	settle_all();
	type(keys);
	type("28");   /* Enter */
}

static void test_sanitize(void)
{
	char *s = rei_msg_sanitize("\x1b[31mred\x1b[0m text\r\n", 19);
	CHECK(s && strcmp(s, "red text") == 0);   /* colours and \r gone, trimmed */
	free(s);
	s = rei_msg_sanitize("a\tb", 3);
	CHECK(s && strcmp(s, "a    b") == 0);
	free(s);
	s = rei_msg_sanitize("bad \xff byte", 10);
	CHECK(s && strstr(s, "\xef\xbf\xbd"));   /* an invalid byte becomes U+FFFD, never invalid UTF-8 */
	free(s);
	CHECK(rei_msg_sanitize("  \n\t \r\n", 7) == NULL);   /* blank: nothing to show */
	CHECK(rei_msg_sanitize("", 0) == NULL);
	char lines[800] = "";
	for (int i = 0; i < 100; i++)
		strcat(lines, "line\n");
	s = rei_msg_sanitize(lines, strlen(lines));
	CHECK(s && strstr(s, "…"));   /* capped, and says so */
	int n = 0;
	for (const char *p = s; *p; p++)
		n += *p == '\n';
	CHECK(n < 45);
	free(s);
}

int main(int argc, char **argv)
{
	test_sanitize();
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-msgbuf-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	const char *config =
		"[toolbar]\nenabled = false\n"
		"[messages]\nduration = 1.6\nwidth = 300\n"
		"[theme.messages]\ncolor = \"#2040a0\"\nopacity = 1.0\n"
		"[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n";
	start_compositor(argv[1], config, true);
	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);
	settle_all();

	/* The column sits at the right edge, 10px in; its bottom is 10px above the screen's bottom. */
	const int x0 = 800 - 10 - 300 - 8, x1 = 800, y0 = 400, y1 = 600;
	struct ppm base = screenshot();

	/* A command with no output pushes nothing. */
	run_command("20 19 22 18");   /* "true" */
	sleep_ms(700);
	CHECK(!log_has("message:"));
	struct ppm quiet = screenshot();
	CHECK(diff(&base, &quiet, x0, y0, x1, y1) < 20);
	free(quiet.rgb);

	/* One with output does. */
	run_command("18 46 35 24 57 35 23");   /* "echo hi" */
	for (int t = 0; !log_has("message: hi"); t += 50) {
		CHECK(t < 5000);
		sleep_ms(50);
	}
	sleep_ms(400);   /* slid into place */
	struct ppm shown = screenshot();
	CHECK(diff(&base, &shown, x0, y0, x1, y1) > 15);   /* the text is on screen, at the right edge */
	CHECK(diff(&base, &shown, 0, 0, 400, 600) < 20);    /* and only there */
	free(shown.rgb);

	/* After its time it has faded away. */
	sleep_ms(1800);
	struct ppm gone = screenshot();
	CHECK(diff(&base, &gone, x0, y0, x1, y1) < 10);
	free(gone.rgb);

	/* The stack view: Mod+n shows everything, on a backdrop. */
	inject_lines("key 125 1\nkey 49 1\nkey 49 0\nkey 125 0\n");   /* Mod+n */
	settle_all();
	sleep_ms(300);
	struct ppm stack = screenshot();
	CHECK(diff(&base, &stack, x0 - 4, 30, x1, y1) > 3000);   /* a tall backdrop and the old message */
	/* The wheel over it scrolls without disturbing anything else; Escape closes it. */
	inject_lines("motion_abs 700 300\naxis v -1\naxis v 1\n");
	settle_all();
	inject_lines("key 1 1\nkey 1 0\nmotion_abs 100 100\n");   /* Escape; and the pointer out of the way */
	settle_all();
	sleep_ms(200);
	struct ppm closed = screenshot();
	CHECK(diff(&base, &closed, x0 - 4, 30, x1, y1) < 10);
	free(closed.rgb);
	free(stack.rgb);
	free(base.rgb);

	CHECK(stop_compositor() == 0);

	/* For looking at (only with REI_SHOT_DIR): several messages, then the stack, with the default look. */
	if (getenv("REI_SHOT_DIR")) {
		char cmd0[800];
		snprintf(cmd0, sizeof cmd0, "rm -f %s/input.fifo %s/%s %s/%s.lock", g_dir, g_dir, SOCKET, g_dir, SOCKET);
		CHECK(system(cmd0) == 0);
		g_pid = 0;
		close(g_fifo_fd);   /* the old compositor's input pipe */
		g_fifo_fd = -1;
		start_compositor(argv[1], "[toolbar]\nenabled = true\n[[output]]\nname = \"OUT\"\nwidth = 1000\nheight = 640\n", true);
		struct client c2;
		client_connect(&c2);
		g_clients[0] = &c2;
		client_bind_keyboard(&c2);
		settle_all();
		static const char *words[] = { "echo first message", "echo second one here", "echo third", "echo the fourth line is a good deal longer than the others so that it wraps around the column", "echo fifth and newest" };
		for (int i = 0; i < 5; i++) {
			inject_lines("key 125 1\nkey 19 1\nkey 19 0\nkey 125 0\n");
			settle_all();
			type_text(words[i]);
			type("28");
			sleep_ms(500);
		}
		sleep_ms(400);
		struct ppm a = screenshot();
		(void)a;
		char cmd[400];
		snprintf(cmd, sizeof cmd, "cp %s %s/messages-compact.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
		inject_lines("key 125 1\nkey 49 1\nkey 49 0\nkey 125 0\nmotion_abs 100 100\n");
		settle_all();
		sleep_ms(300);
		struct ppm b = screenshot();
		(void)b;
		snprintf(cmd, sizeof cmd, "cp %s %s/messages-stack.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
		CHECK(stop_compositor() == 0);
	}
	puts("msgbuf: ok");
	return 0;
}
