/*
 * The music controls and the message stack view as the top layer, against a fake MPD:
 *   - not running: a "Start MPD" button, which starts it (and the controls then appear);
 *   - play, pause and skip send the right commands; Space toggles; the label follows the song;
 *   - a new song (changed by someone else) is announced as a message, the first one at connect is not;
 *   - the screen under the stack view is blurred and darkened, and gets sharp again when it closes;
 *   - while it is open nothing reaches a window: no motion, buttons, wheel or keys; releasing a key pressed
 *     before it opened still does; Escape and a click on the background close it.
 * The fake MPD is this program run as `test_mpd --fake-mpd SOCKET LOG`.
 */
#define SOCKET "rei-mpd-test"
#include "testlib.h"

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "toolbar.h"

/* ---- the fake MPD ---- */

static volatile sig_atomic_t g_advance, g_quieter;
static void on_usr1(int sig) { (void)sig; g_advance = 1; }
static void on_usr2(int sig) { (void)sig; g_quieter = 1; }   /* "mpc volume -10", from outside */

struct fclient {
	int fd;
	char buf[1024];
	size_t len;
	bool idle, dirty;
};

static const char *const g_artist[] = { "Ada", "Bob", "Cyd" };
static const char *const g_title[] = { "One", "Two", "Three" };

static void say(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int fd, const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	CHECK(write(fd, b, (size_t)n) == n);
}

static int fake_mpd(const char *path, const char *logpath)
{
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	char pidpath[450];
	snprintf(pidpath, sizeof pidpath, "%s.pid", path);
	FILE *pf = fopen(pidpath, "w");
	fprintf(pf, "%d\n", (int)getpid());
	fclose(pf);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	unlink(path);
	CHECK(bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 4) == 0);
	struct fclient cl[4];
	memset(cl, 0, sizeof cl);
	for (int i = 0; i < 4; i++)
		cl[i].fd = -1;
	int state = 0 /* 0 stop 1 play 2 pause */, song = 0, id = 100, volume = 80;
	int rnd = 0, rep = 0, single = 0;
	for (;;) {
		struct pollfd p[5];
		int n = 0;
		p[n++] = (struct pollfd){ ls, POLLIN, 0 };
		for (int i = 0; i < 4; i++)
			if (cl[i].fd >= 0)
				p[n++] = (struct pollfd){ cl[i].fd, POLLIN, 0 };
		poll(p, (nfds_t)n, 40);
		bool change = false;
		if (g_advance) {   /* someone else changes the song */
			g_advance = 0;
			song = (song + 1) % 3;
			id++;
			state = 1;
			change = true;
		}
		if (g_quieter) {
			g_quieter = 0;
			volume -= 10;
			change = true;
		}
		if (p[0].revents & POLLIN) {
			int fd = accept(ls, NULL, NULL);
			for (int i = 0; i < 4 && fd >= 0; i++)
				if (cl[i].fd < 0) {
					cl[i] = (struct fclient){ .fd = fd };
					say(fd, "OK MPD 0.23.5\n");
					fd = -1;
				}
			if (fd >= 0)
				close(fd);
		}
		for (int i = 0; i < 4; i++) {
			struct fclient *c = &cl[i];
			if (c->fd < 0 || !(poll(&(struct pollfd){ c->fd, POLLIN, 0 }, 1, 0) > 0))
				continue;
			ssize_t r = read(c->fd, c->buf + c->len, sizeof c->buf - c->len);
			if (r <= 0) {
				close(c->fd);
				c->fd = -1;
				continue;
			}
			c->len += (size_t)r;
			char *nl;
			while ((nl = memchr(c->buf, '\n', c->len))) {
				*nl = '\0';
				char line[256];
				snprintf(line, sizeof line, "%s", c->buf);
				memmove(c->buf, nl + 1, c->len - (size_t)(nl - c->buf) - 1);
				c->len -= (size_t)(nl - c->buf) + 1;
				if (strcmp(line, "status") && strcmp(line, "currentsong") && strncmp(line, "idle", 4) && strcmp(line, "noidle")) {
					FILE *lf = fopen(logpath, "a");
					fprintf(lf, "%s\n", line);
					fclose(lf);
				}
				if (!strcmp(line, "status")) {
					say(c->fd, "volume: %d\nrandom: %d\nrepeat: %d\nsingle: %d\nstate: %s\n", volume, rnd, rep, single,
					    state == 1 ? "play" : state == 2 ? "pause" : "stop");
					if (state)
						say(c->fd, "songid: %d\n", id);
					say(c->fd, "OK\n");
				} else if (!strcmp(line, "currentsong")) {
					if (state)
						say(c->fd, "file: music/%s.mp3\nArtist: %s\nTitle: %s\nId: %d\n", g_title[song], g_artist[song],
						    g_title[song], id);
					say(c->fd, "OK\n");
				} else if (!strncmp(line, "idle", 4)) {
					if (c->dirty) {
						c->dirty = false;
						say(c->fd, "changed: player\nOK\n");
					} else {
						c->idle = true;
					}
				} else if (!strcmp(line, "noidle")) {
					if (c->idle)
						say(c->fd, "OK\n");
					c->idle = false;
				} else {
					if (!strcmp(line, "pause 1") && state == 1)
						state = 2, change = true;
					else if (!strcmp(line, "pause 0") && state == 2)
						state = 1, change = true;
					else if (!strcmp(line, "previous") && state) {
						song = (song + 2) % 3;
						id++;
						change = true;
					} else if (!strncmp(line, "random ", 7))
						rnd = atoi(line + 7), change = true;
					else if (!strncmp(line, "repeat ", 7))
						rep = atoi(line + 7), change = true;
					else if (!strncmp(line, "single ", 7))
						single = atoi(line + 7), change = true;
					else if (!strncmp(line, "setvol ", 7))
						volume = atoi(line + 7), change = true;
					else if (!strcmp(line, "play"))
						state = 1, change = true;
					else if (!strcmp(line, "next") && state) {
						song = (song + 1) % 3;
						id++;
						change = true;
					}
					say(c->fd, "OK\n");
				}
			}
		}
		if (change)
			for (int i = 0; i < 4; i++)
				if (cl[i].fd >= 0) {
					if (cl[i].idle) {
						cl[i].idle = false;
						say(cl[i].fd, "changed: player\nOK\n");
					} else {
						cl[i].dirty = true;
					}
				}
	}
}

/* ---- the test ---- */

static char g_self[400], g_sock[400], g_cmdlog[400];
static char g_config[2400];

static char *commands(void)
{
	char *t = slurp(g_cmdlog);
	return t ? t : strdup("");
}

static int count_cmd(const char *cmd)
{
	char *t = commands();
	int n = 0;
	for (const char *p = t; (p = strstr(p, cmd)); p += strlen(cmd))
		if ((p == t || p[-1] == '\n') && p[strlen(cmd)] == '\n')
			n++;
	free(t);
	return n;
}

static void wait_log(const char *needle, int timeout_ms)
{
	for (int t = 0; !log_has(needle); t += 20) {
		if (t >= timeout_ms) {
			fprintf(stderr, "never logged: %s\n", needle);
			exit(1);
		}
		sleep_ms(20);
	}
}

static void wait_cmd(const char *cmd, int want)
{
	for (int t = 0; count_cmd(cmd) < want; t += 20) {
		if (t > 4000) {
			fprintf(stderr, "MPD never got '%s' (x%d)\n", cmd, want);
			exit(1);
		}
		sleep_ms(20);
	}
}

/* Where the controls' buttons are on the screen (800x600, no toolbar). */
static int g_w = OUT_W, g_h = OUT_H;
static int g_panel_dx;   /* how far the panel was pushed aside (a wide message column) */

static void button_center(bool running, int i, int *x, int *y)
{
	struct rei_rect b[REI_MPD_BUTTONS];
	int n = rei_bar_mpd_buttons(running, b);
	CHECK(i < n);
	int bx = (g_w - REI_MPDP_W) / 2 + g_panel_dx, by = g_h - 10 - REI_MPDP_H;
	*x = bx + b[i].x + b[i].w / 2;
	*y = by + b[i].y + b[i].h / 2;
}

static void keep(const char *name)
{
	if (!getenv("REI_SHOT_DIR"))
		return;
	struct ppm img = screenshot();
	free(img.rgb);
	char cmd[600];
	snprintf(cmd, sizeof cmd, "cp %s %s/%s.ppm", g_shot, getenv("REI_SHOT_DIR"), name);
	CHECK(system(cmd) == 0);
}

static void click(int x, int y)
{
	inject("motion_abs %d %d", x, y);
	inject("button 272 1");
	inject("button 272 0");
	settle_all();
}

static void open_stack(void) { chord(125, 0, 49); settle_all(); sleep_ms(80); }

/* A window's worth of 2-pixel vertical stripes: black, white. */
static struct wl_buffer *stripes(struct client *c, int w, int h)
{
	size_t size = (size_t)w * (size_t)h * 4;
	int fd = memfd_create("stripes", MFD_CLOEXEC);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			p[y * w + x] = (x / 2) % 2 ? 0xffffffff : 0xff000000;
	munmap(p, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static int lum(const struct ppm *img, int x, int y)
{
	const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
	return p[0] + p[1] + p[2];
}

/* Largest brightness step between neighbouring pixels along a row of the left part of the screen. */
static int contrast(const struct ppm *img, int y)
{
	int best = 0;
	for (int x = 30; x < 180; x++) {
		int d = abs(lum(img, x, y) - lum(img, x + 1, y));
		best = d > best ? d : best;
	}
	return best;
}

static int mean(const struct ppm *img, int y)
{
	long s = 0;
	for (int x = 30; x < 180; x++)
		s += lum(img, x, y);
	return (int)(s / 150);
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "--fake-mpd"))
		return fake_mpd(argv[2], argv[3]);
	CHECK(argc == 2);
	quiet_client_leaks();
	CHECK(readlink("/proc/self/exe", g_self, sizeof g_self - 1) > 0);
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-mpd-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_sock, sizeof g_sock, "%s/mpd.sock", g_dir);
	snprintf(g_cmdlog, sizeof g_cmdlog, "%s/mpd-commands.txt", g_dir);

	char *config = g_config;
	snprintf(config, sizeof g_config,
		 "[toolbar]\nenabled = false\n[mpd]\nhost = \"%s\"\nstart_command = \"%s --fake-mpd %s %s\"\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", g_sock, g_self, g_sock, g_cmdlog, OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	client_bind_keyboard(&c);
	struct window w;
	open_win(&c, &w, "music", 0xff808080);
	wl_surface_attach(w.surface, stripes(&c, w.buf_w, w.buf_h), 0, 0);
	wl_surface_damage(w.surface, 0, 0, w.buf_w, w.buf_h);
	wl_surface_commit(w.surface);
	inject("motion_abs 100 100");
	settle_all();

	/* ---- The stack view, MPD not running: the screen under it is blurred and dark. ---- */
	struct ppm sharp = screenshot();
	int sharp_contrast = contrast(&sharp, 300), sharp_mean = mean(&sharp, 300);
	free(sharp.rgb);
	CHECK(sharp_contrast > 600);   /* black next to white */
	open_stack();
	struct ppm view = screenshot();
	int blur_contrast = contrast(&view, 300), blur_mean = mean(&view, 300);
	free(view.rgb);
	fprintf(stderr, "stripes: sharp contrast %d mean %d; blurred contrast %d mean %d\n", sharp_contrast, sharp_mean,
		blur_contrast, blur_mean);
	CHECK(blur_contrast < sharp_contrast / 6);                   /* blurred */
	CHECK(blur_mean < sharp_mean && blur_mean > sharp_mean / 4);  /* darkened, but not black */
	keep("mpd-not-running");

	/* ---- Nothing reaches the window while it is open. ---- */
	ev_clear(&c);
	inject("motion_abs 120 120");
	inject("motion_abs 140 130");
	inject("axis vertical 10");
	inject("key 30 1");   /* 'a' */
	inject("key 30 0");
	settle_all();
	CHECK(ev_count(&c, EV_PTR_MOTION) == 0 && ev_count(&c, EV_PTR_AXIS) == 0 && ev_count(&c, EV_KB_KEY) == 0);

	/* ---- Start MPD. ---- */
	int bx, by;
	button_center(false, 0, &bx, &by);
	ev_clear(&c);
	click(bx, by);
	CHECK(ev_count(&c, EV_PTR_BUTTON) == 0 && ev_count(&c, EV_PTR_MOTION) == 0);   /* the click was the view's */
	wait_log("message: Starting MPD", 3000);
	wait_log("mpd: connected", 8000);
	sleep_ms(200);
	settle_all();

	char pidpath[450];
	snprintf(pidpath, sizeof pidpath, "%s.pid", g_sock);
	char *pidtext = slurp(pidpath);
	CHECK(pidtext);
	int fake = atoi(pidtext);
	free(pidtext);
	CHECK(fake > 1);

	/* ---- The controls. MPD was stopped when we connected: nothing is announced for that. ---- */
	CHECK(!log_has("message: Now playing"));
	keep("mpd-controls-stopped");
	int px, py;
	button_center(true, 1, &px, &py);   /* play (the middle button: one for play and pause) */
	click(px, py);
	wait_cmd("play", 1);
	wait_log("message: Now playing: Ada - One", 4000);
	settle_all();
	keep("mpd-controls-playing");
	button_center(true, 1, &px, &py);   /* the same button now pauses */
	click(px, py);
	wait_cmd("pause 1", 1);
	sleep_ms(250);                      /* (MPD tells us, then the button knows it is paused) */
	button_center(true, 1, &px, &py);   /* and resumes */
	click(px, py);
	wait_cmd("pause 0", 1);
	sleep_ms(250);
	CHECK(log_count("message: Now playing: Ada - One") == 1);   /* the same song resumed is not new */
	button_center(true, 2, &px, &py);   /* skip */
	click(px, py);
	wait_cmd("next", 1);
	wait_log("message: Now playing: Bob - Two", 4000);
	sleep_ms(200);
	button_center(true, 0, &px, &py);   /* previous */
	click(px, py);
	wait_cmd("previous", 1);
	wait_log("message: Now playing: Ada - One", 4000);   /* (Bob - Two back to Ada - One: announced again, it is new) */
	CHECK(log_count("message: Now playing: Ada - One") == 2);
	sleep_ms(200);
	button_center(true, 2, &px, &py);   /* skip again, to where it was */
	click(px, py);
	wait_cmd("next", 2);
	wait_log("message: Now playing: Bob - Two", 4000);
	sleep_ms(200);
	CHECK(kill(fake, SIGUSR1) == 0);    /* someone else changes the song */
	wait_log("message: Now playing: Cyd - Three", 4000);

	/* The toggles: shuffle, loop all, loop one (the two loops exclude each other; the lit one turns itself off). */
	sleep_ms(200);
	button_center(true, 3, &px, &py);
	click(px, py);
	wait_cmd("random 1", 1);
	sleep_ms(250);
	keep("mpd-controls-shuffle");
	button_center(true, 4, &px, &py);   /* loop all: repeat on, single off */
	click(px, py);
	wait_cmd("repeat 1", 1);
	wait_cmd("single 0", 1);
	sleep_ms(250);
	button_center(true, 5, &px, &py);   /* loop one: repeat and single */
	click(px, py);
	wait_cmd("single 1", 1);
	sleep_ms(250);
	keep("mpd-controls-loop-one");
	button_center(true, 5, &px, &py);   /* off again: no looping */
	click(px, py);
	wait_cmd("single 0", 2);
	wait_cmd("repeat 0", 1);
	sleep_ms(250);
	button_center(true, 3, &px, &py);   /* shuffle off */
	click(px, py);
	wait_cmd("random 0", 1);
	sleep_ms(250);
	inject("key 57 1");                 /* Space: pause */
	inject("key 57 0");
	settle_all();
	wait_cmd("pause 1", 2);
	sleep_ms(250);
	inject("key 57 1");                 /* and play */
	inject("key 57 0");
	settle_all();
	wait_cmd("pause 0", 2);
	ev_clear(&c);
	CHECK(ev_count(&c, EV_KB_KEY) == 0);

	/* ---- The volume keys follow MPD while it plays (it has a volume of its own). ---- */
	sleep_ms(250);
	CHECK(count_cmd("setvol 85") == 0);
	inject("key 115 1");   /* XF86AudioRaiseVolume */
	inject("key 115 0");
	settle_all();
	wait_cmd("setvol 85", 1);
	inject("key 115 1");   /* twice in a row, before MPD has answered the first: they add up */
	inject("key 115 0");
	inject("key 115 1");
	inject("key 115 0");
	settle_all();
	wait_cmd("setvol 95", 1);
	inject("key 114 1");   /* lower */
	inject("key 114 0");
	settle_all();
	wait_cmd("setvol 90", 1);
	sleep_ms(250);
	inject("key 113 1");   /* mute: MPD has none, so 0 ... */
	inject("key 113 0");
	settle_all();
	wait_cmd("setvol 0", 1);
	sleep_ms(250);
	inject("key 113 1");   /* ... and back to what it was */
	inject("key 113 0");
	settle_all();
	wait_cmd("setvol 90", 2);
	sleep_ms(250);
	CHECK(kill(fake, SIGUSR2) == 0);   /* someone runs "mpc volume -10": 80 */
	sleep_ms(400);
	inject("key 115 1");
	inject("key 115 0");
	settle_all();
	wait_cmd("setvol 85", 2);          /* 80 + 5: the keys know what mpc did */
	/* Stopped or paused, the keys are the sound system's again: MPD is left alone. */
	int sv = count_cmd("setvol 85");
	inject("key 57 1");                /* Space: pause */
	inject("key 57 0");
	settle_all();
	wait_cmd("pause 1", 3);
	sleep_ms(300);
	inject("key 115 1");
	inject("key 115 0");
	settle_all();
	sleep_ms(300);
	CHECK(count_cmd("setvol 85") == sv && count_cmd("setvol 90") == 2);
	inject("key 57 1");                /* and play again */
	inject("key 57 0");
	settle_all();
	wait_cmd("pause 0", 3);
	sleep_ms(300);

	/* volume_target = "sink": MPD is never touched. */
	{
		char text[3000], cfgpath[500], tmp[520];
		snprintf(text, sizeof text, "%s[audio]\nvolume_target = \"sink\"\n", g_config);
		snprintf(cfgpath, sizeof cfgpath, "%s/config.toml", g_dir);
		snprintf(tmp, sizeof tmp, "%s.new", cfgpath);
		write_text(tmp, text);
		int reloaded = log_count("config reloaded");
		CHECK(rename(tmp, cfgpath) == 0);
		CHECK(kill(g_pid, SIGHUP) == 0);
		for (int t = 0; log_count("config reloaded") <= reloaded; t += 20) {
			CHECK(t < 5000);
			sleep_ms(20);
		}
		settle_all();
		int total = count_cmd("setvol 85") + count_cmd("setvol 90") + count_cmd("setvol 95") + count_cmd("setvol 0");
		inject("key 115 1");
		inject("key 115 0");
		settle_all();
		sleep_ms(300);
		CHECK(count_cmd("setvol 85") + count_cmd("setvol 90") + count_cmd("setvol 95") + count_cmd("setvol 0") == total);
	}

	/* ---- Escape puts the view away: sharp again, and the window has its pointer back. ---- */
	inject("key 1 1");
	inject("key 1 0");
	settle_all();
	sleep_ms(150);
	struct ppm after = screenshot();
	int after_contrast = contrast(&after, 300);
	free(after.rgb);
	CHECK(after_contrast > sharp_contrast * 3 / 4);
	ev_clear(&c);
	inject("motion_abs 110 110");
	settle_all();
	CHECK(ev_count(&c, EV_PTR_ENTER) + ev_count(&c, EV_PTR_MOTION) >= 1);

	/* ---- A key pressed before the view opened still gets its release, so nothing stays down. ---- */
	ev_clear(&c);
	inject("key 48 1");   /* 'b' */
	settle_all();
	open_stack();
	inject("key 48 0");
	settle_all();
	int b_press = 0, b_release = 0;
	for (int i = 0; i < c.n_events; i++)
		if (c.events[i].type == EV_KB_KEY && c.events[i].a == 48) {
			if (c.events[i].b)
				b_press++;
			else
				b_release++;
		}
	CHECK(b_press == 1 && b_release == 1);
	/* While it is open, a click on the blurred background puts it away and is not the window's. */
	ev_clear(&c);
	click(110, 110);
	CHECK(ev_count(&c, EV_PTR_BUTTON) == 0);
	sleep_ms(150);
	struct ppm closed = screenshot();
	int closed_contrast = contrast(&closed, 300);
	free(closed.rgb);
	CHECK(closed_contrast > sharp_contrast * 3 / 4);

	CHECK(kill(fake, SIGTERM) == 0);
	CHECK(stop_compositor() == 0);

	/* ---- A wider screen: the controls stay centred unless the message column is in the way, and then they
	 * move aside just far enough (the column is 512 wide with its backdrop, 10 from the right edge). ---- */
	char cmd0[2000];
	snprintf(cmd0, sizeof cmd0, "rm -f %s/input.fifo %s/%s %s/%s.lock %s", g_dir, g_dir, SOCKET, g_dir, SOCKET, g_sock);
	CHECK(system(cmd0) == 0);
	g_pid = 0;
	close(g_fifo_fd);
	g_fifo_fd = -1;
	g_nwins = 0;
	g_w = 1366;
	g_h = 768;
	pid_t fake2 = fork();
	CHECK(fake2 >= 0);
	if (fake2 == 0)
		_exit(fake_mpd(g_sock, g_cmdlog));
	for (int t = 0; access(g_sock, F_OK) != 0; t += 20) {
		CHECK(t < 3000);
		sleep_ms(20);
	}
	snprintf(config, sizeof g_config,
		 "[toolbar]\nenabled = false\n[mpd]\nhost = \"%s\"\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n",
		 g_sock, g_w, g_h);
	start_compositor(argv[1], config, true);
	struct client c2;
	client_connect(&c2);
	g_clients[0] = &c2;
	client_bind_pointer(&c2);
	wait_log("mpd: connected", 5000);
	int column_x = g_w - 10 - (480 + 2 * REI_MSG_PAD + 2 * 12);
	g_panel_dx = (column_x - 12 - REI_MPDP_W) - (g_w - REI_MPDP_W) / 2;
	CHECK(g_panel_dx < 0 && g_panel_dx > -60);   /* a small step left, not a jump */
	open_stack();
	int before = count_cmd("play");
	button_center(true, 1, &px, &py);
	click(px, py);
	wait_cmd("play", before + 1);   /* the button was where it was drawn */
	keep("mpd-wide");
	CHECK(kill(fake2, SIGTERM) == 0);
	CHECK(stop_compositor() == 0);
	puts("mpd: ok");
	return 0;
}
