/*
 * Shared plumbing for the end-to-end tests: start the real rei-wm headless in a
 * temp directory, talk to it as a Wayland client, and read its screenshots.
 *
 * A test defines SOCKET (the compositor's socket name) before including this,
 * and may define OUT_W/OUT_H. Each test process runs one compositor, tracked in
 * globals, which keeps the tests themselves short.
 */
#ifndef REI_TESTLIB_H
#define REI_TESTLIB_H

/* Each test uses only some of these helpers. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "check.h"
#include "xdg-shell-client-protocol.h"

#define OUT_W 800
#define OUT_H 600

/* ------------------------------------------------------------------------ */
/* Harness                                                                  */
/* ------------------------------------------------------------------------ */

/* Under LeakSanitizer, don't report a test client's own undestroyed proxies and
 * buffers: the test exits right after, and they're not what is being tested.
 * The compositor runs as a separate process and is still fully leak-checked. */
extern void __lsan_disable(void) __attribute__((weak));
static void quiet_client_leaks(void)
{
	if (__lsan_disable)
		__lsan_disable();
	/* Writing to a compositor that has already exited must be an error we can
	 * report, not a fatal signal that hides what happened. */
	signal(SIGPIPE, SIG_IGN);
}

static char g_dir[128], g_log[192], g_shot[192], g_fifo[192];
static pid_t g_pid;

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *buf = calloc(1, 1 << 20);
	size_t n = fread(buf, 1, (1 << 20) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static bool log_has(const char *needle)
{
	char *log = slurp(g_log);
	bool found = log && strstr(log, needle);
	free(log);
	return found;
}

static void write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	CHECK(f);
	fputs(text, f);
	fclose(f);
}

/* A failing CHECK exits immediately; don't leave the compositor running (it
 * would also hold the test runner's output pipe open). */
static void kill_compositor_at_exit(void)
{
	if (g_pid > 0)
		kill(g_pid, SIGKILL);
}

static void start_compositor(const char *binary, const char *config_text, bool with_fifo)
{
	char cfg[192];
	snprintf(cfg, sizeof cfg, "%s/config.toml", g_dir);
	snprintf(g_log, sizeof g_log, "%s/log.txt", g_dir);
	snprintf(g_shot, sizeof g_shot, "%s/rei-wm-OUT.ppm", g_dir);
	snprintf(g_fifo, sizeof g_fifo, "%s/input.fifo", g_dir);
	write_text(cfg, config_text);
	if (with_fifo)
		CHECK(mkfifo(g_fifo, 0600) == 0);

	g_pid = fork();
	CHECK(g_pid >= 0);
	if (g_pid > 0)
		atexit(kill_compositor_at_exit);
	if (g_pid == 0) {
		setenv("XDG_RUNTIME_DIR", g_dir, 1);
		unsetenv("WAYLAND_DISPLAY");
		if (with_fifo)
			setenv("REI_HEADLESS_INPUT", g_fifo, 1);
		int fd = open(g_log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		dup2(fd, STDERR_FILENO);
		execl(binary, binary, "-c", cfg, "-b", "headless", "-s", SOCKET, "-v", (char *)NULL);
		_exit(127);
	}

	char sock[192];
	snprintf(sock, sizeof sock, "%s/%s", g_dir, SOCKET);
	struct stat st;
	for (int t = 0; stat(sock, &st) != 0; t += 20) {
		int status;
		CHECK(waitpid(g_pid, &status, WNOHANG) == 0);
		CHECK(t < 5000);
		sleep_ms(20);
	}

	/* Rendering needs OpenGL ES; without it there is nothing to verify. */
	for (int t = 0;; t += 20) {
		if (log_has("rendering with"))
			break;
		if (log_has("OpenGL ES unavailable")) {
			fprintf(stderr, "test_render: no OpenGL ES here, skipping\n");
			kill(g_pid, SIGTERM);
			waitpid(g_pid, NULL, 0);
			exit(77);
		}
		CHECK(t < 8000);
		sleep_ms(20);
	}
}

static int stop_compositor(void)
{
	CHECK(kill(g_pid, SIGTERM) == 0);
	int status = 0;
	for (int t = 0;; t += 10) {
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid)
			break;
		CHECK(r == 0);
		CHECK(t < 5000);
		sleep_ms(10);
	}
	g_pid = 0;  /* Reaped: nothing left for the atexit handler to kill. */
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* ------------------------------------------------------------------------ */
/* Screenshots                                                              */
/* ------------------------------------------------------------------------ */

struct ppm {
	int w, h;
	uint8_t *rgb;
};

/* A screenshot of the named output (the compositor writes one per output). */
static struct ppm screenshot_output(const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "%s/rei-wm-%s.ppm", g_dir, name);
	unlink(path);
	CHECK(kill(g_pid, SIGUSR2) == 0);
	struct stat st;
	for (int t = 0; stat(path, &st) != 0; t += 20) {
		CHECK(t < 5000);
		sleep_ms(20);
	}
	FILE *f = fopen(path, "rb");
	CHECK(f);
	struct ppm img;
	int maxv;
	CHECK(fscanf(f, "P6 %d %d %d", &img.w, &img.h, &maxv) == 3 && maxv == 255);
	CHECK(fgetc(f) == '\n');
	img.rgb = malloc((size_t)img.w * (size_t)img.h * 3);
	CHECK(fread(img.rgb, 1, (size_t)img.w * (size_t)img.h * 3, f) == (size_t)img.w * (size_t)img.h * 3);
	fclose(f);
	return img;
}

static struct ppm screenshot(void)
{
	return screenshot_output("OUT");
}

static void expect_px(const struct ppm *img, int x, int y, int r, int g, int b, int tol,
		      const char *what)
{
	const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
	if (abs(p[0] - r) > tol || abs(p[1] - g) > tol || abs(p[2] - b) > tol) {
		fprintf(stderr, "pixel (%d,%d) [%s]: got (%d,%d,%d), expected (%d,%d,%d) +-%d\n",
			x, y, what, p[0], p[1], p[2], r, g, b, tol);
		exit(1);
	}
}

static bool near_px(const struct ppm *img, int x, int y, int r, int g, int b, int tol)
{
	const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
	return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol;
}

/* ------------------------------------------------------------------------ */
/* Client                                                                   */
/* ------------------------------------------------------------------------ */

struct global_info {
	char name[48];
	uint32_t version;
};

/* One recorded wl_pointer / wl_keyboard event. */
enum ev_type {
	EV_PTR_ENTER = 1, EV_PTR_LEAVE, EV_PTR_MOTION, EV_PTR_BUTTON, EV_PTR_AXIS,
	EV_KB_KEYMAP, EV_KB_REPEAT, EV_KB_ENTER, EV_KB_LEAVE, EV_KB_KEY, EV_KB_MODS,
};
struct ev {
	enum ev_type type;
	struct wl_surface *surface;
	double x, y;
	uint32_t a, b, c, d;
};

struct client {
	struct wl_display *d;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct wl_seat *seat;
	struct wl_data_device_manager *ddm;
	struct wl_output *output;
	struct global_info globals[32];
	int n_globals;

	/* wl_output events */
	int out_x, out_y, out_w, out_h, out_refresh, out_scale, out_mode_flags;
	char out_name[32];
	bool out_done;
	char seat_name[32];
	uint32_t seat_caps;

	/* Input objects and everything they were sent, in order. */
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct ev events[512];
	int n_events;
};

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
			 int32_t sub, const char *make, const char *model, int32_t tr)
{
	(void)o; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr;
	struct client *c = d;
	c->out_x = x;
	c->out_y = y;
}
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
	(void)o;
	struct client *c = d;
	c->out_mode_flags = (int)flags;
	c->out_w = w;
	c->out_h = h;
	c->out_refresh = r;
}
static void out_done(void *d, struct wl_output *o) { (void)o; ((struct client *)d)->out_done = true; }
static void out_scale(void *d, struct wl_output *o, int32_t s) { (void)o; ((struct client *)d)->out_scale = s; }
static void out_name(void *d, struct wl_output *o, const char *n)
{
	(void)o;
	snprintf(((struct client *)d)->out_name, 32, "%s", n);
}
static void out_desc(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static const struct wl_output_listener out_listener = {
	out_geometry, out_mode, out_done, out_scale, out_name, out_desc,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) { (void)s; ((struct client *)d)->seat_caps = caps; }
static void seat_name(void *d, struct wl_seat *s, const char *n)
{
	(void)s;
	snprintf(((struct client *)d)->seat_name, 32, "%s", n);
}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	struct client *c = data;
	CHECK(c->n_globals < 32);
	snprintf(c->globals[c->n_globals].name, sizeof c->globals[0].name, "%s", iface);
	c->globals[c->n_globals++].version = version;

	if (!strcmp(iface, "wl_compositor"))
		c->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version);
	else if (!strcmp(iface, "wl_subcompositor"))
		c->subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, version);
	else if (!strcmp(iface, "wl_shm"))
		c->shm = wl_registry_bind(reg, name, &wl_shm_interface, version);
	else if (!strcmp(iface, "xdg_wm_base")) {
		c->wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, version);
		xdg_wm_base_add_listener(c->wm, &wm_listener, c);
	} else if (!strcmp(iface, "wl_seat")) {
		c->seat = wl_registry_bind(reg, name, &wl_seat_interface, version);
		wl_seat_add_listener(c->seat, &seat_listener, c);
	} else if (!strcmp(iface, "wl_data_device_manager"))
		c->ddm = wl_registry_bind(reg, name, &wl_data_device_manager_interface, version);
	else if (!strcmp(iface, "wl_output") && !c->output) {
		c->output = wl_registry_bind(reg, name, &wl_output_interface, version);
		wl_output_add_listener(c->output, &out_listener, c);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void client_connect(struct client *c)
{
	memset(c, 0, sizeof *c);
	setenv("XDG_RUNTIME_DIR", g_dir, 1);
	c->d = wl_display_connect(SOCKET);
	CHECK(c->d);
	c->registry = wl_display_get_registry(c->d);
	wl_registry_add_listener(c->registry, &reg_listener, c);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	CHECK(wl_display_roundtrip(c->d) >= 0);  /* second trip: events of the bound objects */
}

static uint32_t global_version(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++) {
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].version;
	}
	return 0;
}

/* Pumps events for up to timeout_ms; returns once `*flag` is set. */
static bool wait_for(struct client *c, const bool *flag, int timeout_ms)
{
	for (int t = 0; t < timeout_ms && !*flag; t += 10) {
		while (wl_display_prepare_read(c->d) != 0)
			wl_display_dispatch_pending(c->d);
		wl_display_flush(c->d);
		struct pollfd pfd = { wl_display_get_fd(c->d), POLLIN, 0 };
		if (poll(&pfd, 1, 10) > 0)
			wl_display_read_events(c->d);
		else
			wl_display_cancel_read(c->d);
		wl_display_dispatch_pending(c->d);
	}
	return *flag;
}

/* ---- buffers --------------------------------------------------------- */

static struct wl_buffer *make_buffer(struct client *c, int w, int h, uint32_t format, uint32_t pixel)
{
	size_t size = (size_t)w * (size_t)h * 4;
	int fd = memfd_create("test-buf", MFD_CLOEXEC);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(px != MAP_FAILED);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = pixel;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

/* ---- windows --------------------------------------------------------- */

struct window {
	struct client *c;
	struct wl_surface *surface;
	struct xdg_surface *xs;
	struct xdg_toplevel *tl;
	struct xdg_popup *popup;
	bool configured, activated, frame_done, popup_configured;
	bool close_requested, popup_dismissed;
	/* What the compositor last told this toplevel (0x0 = pick your own size). */
	int32_t cfg_w, cfg_h;
	bool cfg_tiled, cfg_fullscreen;
	/* What the test client is showing, and how it behaves when resized. */
	uint32_t color;
	int32_t natural_w, natural_h, buf_w, buf_h;
	bool has_buffer;
	uint32_t serial;
	int n_configures;
	int popup_x, popup_y, popup_w, popup_h;
};

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	struct window *w = d;
	w->serial = serial;
	w->configured = true;
	w->n_configures++;
	xdg_surface_ack_configure(xs, serial);
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t wd, int32_t h, struct wl_array *states)
{
	(void)t;
	struct window *w = d;
	w->cfg_w = wd;
	w->cfg_h = h;
	w->activated = w->cfg_tiled = w->cfg_fullscreen = false;
	uint32_t *s;
	wl_array_for_each(s, states) {
		if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
			w->activated = true;
		if (*s == XDG_TOPLEVEL_STATE_TILED_LEFT)
			w->cfg_tiled = true;
		if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN)
			w->cfg_fullscreen = true;
	}
}
static void tl_close(void *d, struct xdg_toplevel *t)
{
	(void)t;
	((struct window *)d)->close_requested = true;
}
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t a, int32_t b) { (void)d; (void)t; (void)a; (void)b; }
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *a) { (void)d; (void)t; (void)a; }
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close, tl_bounds, tl_caps };

static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t wd, int32_t h)
{
	(void)p;
	struct window *w = d;
	w->popup_x = x; w->popup_y = y; w->popup_w = wd; w->popup_h = h;
	w->popup_configured = true;
}
static void popup_done(void *d, struct xdg_popup *p)
{
	(void)p;
	((struct window *)d)->popup_dismissed = true;
}
static void popup_repositioned(void *d, struct xdg_popup *p, uint32_t t) { (void)d; (void)p; (void)t; }
static const struct xdg_popup_listener popup_listener = { popup_configure, popup_done, popup_repositioned };

static void frame_done_cb(void *d, struct wl_callback *cb, uint32_t time)
{
	(void)time;
	((struct window *)d)->frame_done = true;
	wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { frame_done_cb };

/* Creates a toplevel and performs the initial commit/configure handshake, but
 * does not attach a buffer yet. */
static void toplevel_create(struct client *c, struct window *w, const char *title)
{
	memset(w, 0, sizeof *w);
	w->c = c;
	w->surface = wl_compositor_create_surface(c->compositor);
	w->xs = xdg_wm_base_get_xdg_surface(c->wm, w->surface);
	xdg_surface_add_listener(w->xs, &xs_listener, w);
	w->tl = xdg_surface_get_toplevel(w->xs);
	xdg_toplevel_add_listener(w->tl, &tl_listener, w);
	xdg_toplevel_set_title(w->tl, title);
	xdg_toplevel_set_app_id(w->tl, "rei-test");
	wl_surface_commit(w->surface);
	CHECK(wait_for(c, &w->configured, 3000));
}

static void window_show(struct window *w, struct wl_buffer *buf, bool want_frame)
{
	wl_surface_attach(w->surface, buf, 0, 0);
	wl_surface_damage(w->surface, 0, 0, 1 << 20, 1 << 20);
	if (want_frame) {
		w->frame_done = false;
		struct wl_callback *cb = wl_surface_frame(w->surface);
		wl_callback_add_listener(cb, &frame_listener, w);
	}
	wl_surface_commit(w->surface);
}

/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Input: recording listeners, injection, and two-client pumping             */
/* ------------------------------------------------------------------------ */

static struct ev *ev_push(struct client *c, enum ev_type type)
{
	CHECK(c->n_events < 512);
	struct ev *e = &c->events[c->n_events++];
	memset(e, 0, sizeof *e);
	e->type = type;
	return e;
}

static void ev_clear(struct client *c) { c->n_events = 0; }

static int ev_count(const struct client *c, enum ev_type t)
{
	int n = 0;
	for (int i = 0; i < c->n_events; i++)
		n += c->events[i].type == t;
	return n;
}

/* The nth (0-based) event of a type, or NULL. */
static const struct ev *ev_nth(const struct client *c, enum ev_type t, int n)
{
	for (int i = 0; i < c->n_events; i++) {
		if (c->events[i].type == t && n-- == 0)
			return &c->events[i];
	}
	return NULL;
}

static const struct ev *ev_last(const struct client *c, enum ev_type t)
{
	for (int i = c->n_events - 1; i >= 0; i--) {
		if (c->events[i].type == t)
			return &c->events[i];
	}
	return NULL;
}

/* Position of the first event of a type, to check ordering; -1 if absent. */
static int ev_pos(const struct client *c, enum ev_type t)
{
	for (int i = 0; i < c->n_events; i++) {
		if (c->events[i].type == t)
			return i;
	}
	return -1;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		    wl_fixed_t x, wl_fixed_t y)
{
	(void)p;
	struct ev *e = ev_push(d, EV_PTR_ENTER);
	e->surface = s; e->a = serial; e->x = wl_fixed_to_double(x); e->y = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)p;
	struct ev *e = ev_push(d, EV_PTR_LEAVE);
	e->surface = s; e->a = serial;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)p;
	struct ev *e = ev_push(d, EV_PTR_MOTION);
	e->a = time; e->x = wl_fixed_to_double(x); e->y = wl_fixed_to_double(y);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
		     uint32_t button, uint32_t state)
{
	(void)p;
	struct ev *e = ev_push(d, EV_PTR_BUTTON);
	e->a = button; e->b = state; e->c = serial; e->d = time;
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v)
{
	(void)p;
	struct ev *e = ev_push(d, EV_PTR_AXIS);
	e->a = axis; e->d = time; e->x = wl_fixed_to_double(v);
}
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener test_pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button,
	.axis = p_axis, .frame = p_frame, .axis_source = p_axis_source,
	.axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
	(void)k;
	struct ev *e = ev_push(d, EV_KB_KEYMAP);
	e->a = format; e->b = size;
	/* The keymap must really be readable text in the file we were given. */
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(map != MAP_FAILED);
	e->c = strncmp(map, "xkb_keymap", 10) == 0;
	munmap(map, size);
	close(fd);
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
	(void)k;
	struct ev *e = ev_push(d, EV_KB_ENTER);
	e->surface = s; e->a = serial; e->b = (uint32_t)(keys->size / sizeof(uint32_t));
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)k;
	struct ev *e = ev_push(d, EV_KB_LEAVE);
	e->surface = s; e->a = serial;
}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	(void)k;
	struct ev *e = ev_push(d, EV_KB_KEY);
	e->a = key; e->b = state; e->c = time; e->d = serial;
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat,
		   uint32_t lock, uint32_t group)
{
	(void)k; (void)serial;
	struct ev *e = ev_push(d, EV_KB_MODS);
	e->a = dep; e->b = lat; e->c = lock; e->d = group;
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)k;
	struct ev *e = ev_push(d, EV_KB_REPEAT);
	e->a = (uint32_t)rate; e->b = (uint32_t)delay;
}
static const struct wl_keyboard_listener test_keyboard_listener = {
	.keymap = k_keymap, .enter = k_enter, .leave = k_leave, .key = k_key,
	.modifiers = k_mods, .repeat_info = k_repeat,
};

static void client_bind_pointer(struct client *c)
{
	c->pointer = wl_seat_get_pointer(c->seat);
	wl_pointer_add_listener(c->pointer, &test_pointer_listener, c);
}

static void client_bind_keyboard(struct client *c)
{
	c->keyboard = wl_seat_get_keyboard(c->seat);
	wl_keyboard_add_listener(c->keyboard, &test_keyboard_listener, c);
}

/* ---- injecting input into the headless backend ----------------------- */

static int g_fifo_fd = -1;
static int g_sync_n;

static void inject(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void inject(const char *fmt, ...)
{
	if (g_fifo_fd < 0) {
		g_fifo_fd = open(g_fifo, O_WRONLY);
		CHECK(g_fifo_fd >= 0);
	}
	char line[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	line[n++] = '\n';
	CHECK(write(g_fifo_fd, line, (size_t)n) == n);
}

/* Writes several lines in one write(), so the compositor sees them together.
 * For input that ends the session: separate writes could land after it exited. */
static void inject_lines(const char *text)
{
	if (g_fifo_fd < 0) {
		g_fifo_fd = open(g_fifo, O_WRONLY);
		CHECK(g_fifo_fd >= 0);
	}
	size_t n = strlen(text);
	CHECK(write(g_fifo_fd, text, n) == (ssize_t)n);
}

/* Returns once the compositor has processed everything injected so far. */
static void input_sync(void)
{
	char needle[64];
	snprintf(needle, sizeof needle, "input sync t%dx", ++g_sync_n);
	inject("sync t%dx", g_sync_n);
	for (int t = 0; !log_has(needle); t += 5) {
		CHECK(t < 5000);
		sleep_ms(5);
	}
}

/* Injected input has been handled, and any resulting events are in the
 * client's queue and dispatched. */
static void settle(struct client *c)
{
	input_sync();
	CHECK(wl_display_roundtrip(c->d) >= 0);
	CHECK(wl_display_roundtrip(c->d) >= 0);
}

/* Like wait_for(), but services a second client too (e.g. for clipboard
 * transfers, where each side waits on the other). */
static bool wait_for2(struct client *a, struct client *b, const bool *flag, int timeout_ms)
{
	for (int t = 0; t < timeout_ms && !*flag; t += 10) {
		struct client *cs[2] = { a, b };
		struct pollfd pfd[2];
		for (int i = 0; i < 2; i++) {
			while (wl_display_prepare_read(cs[i]->d) != 0)
				wl_display_dispatch_pending(cs[i]->d);
			wl_display_flush(cs[i]->d);
			pfd[i] = (struct pollfd){ wl_display_get_fd(cs[i]->d), POLLIN, 0 };
		}
		poll(pfd, 2, 10);
		for (int i = 0; i < 2; i++) {
			if (pfd[i].revents & POLLIN)
				wl_display_read_events(cs[i]->d);
			else
				wl_display_cancel_read(cs[i]->d);
			wl_display_dispatch_pending(cs[i]->d);
		}
	}
	return *flag;
}

/* ---- a client that obeys its configure, like a real application ---------- */

/* Shows the window at the size the compositor asked for (or its natural size if
 * it was told 0x0). Returns true if it committed a new buffer. */
static bool window_follow(struct client *c, struct window *w)
{
	int32_t bw = w->cfg_w > 0 ? w->cfg_w : w->natural_w;
	int32_t bh = w->cfg_h > 0 ? w->cfg_h : w->natural_h;
	if (w->has_buffer && bw == w->buf_w && bh == w->buf_h)
		return false;
	window_show(w, make_buffer(c, bw, bh, WL_SHM_FORMAT_XRGB8888, w->color), false);
	w->buf_w = bw;
	w->buf_h = bh;
	w->has_buffer = true;
	return true;
}

#endif
