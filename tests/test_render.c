/*
 * End-to-end protocol + rendering test.
 *
 * Starts rei-wm headless with one 800x600 output and a 2x2 PNM wallpaper, then
 * acts as a Wayland client: checks the advertised globals and wl_output state,
 * maps windows (opaque, translucent), a subsurface and a popup, checks frame
 * callbacks and activation, provokes a protocol error from a second client, and
 * finally asks the compositor for a screenshot and verifies exact pixels.
 *
 * Exits 77 (meson's "skipped") if the machine has no usable OpenGL ES.
 */
#include <errno.h>
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

#define SOCKET "rei-render-test"
#define OUT_W 800
#define OUT_H 600

/* ------------------------------------------------------------------------ */
/* Harness                                                                  */
/* ------------------------------------------------------------------------ */

static char g_dir[128], g_log[192], g_shot[192];
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

static void start_compositor(const char *binary)
{
	char cfg[192], wall[192], text[1024];
	snprintf(cfg, sizeof cfg, "%s/config.toml", g_dir);
	snprintf(wall, sizeof wall, "%s/wall.ppm", g_dir);
	snprintf(g_log, sizeof g_log, "%s/log.txt", g_dir);
	snprintf(g_shot, sizeof g_shot, "%s/rei-wm-OUT.ppm", g_dir);

	/* 2x2 wallpaper: red, green / blue, white. Stretched, the corners keep
	 * their exact colour. */
	FILE *f = fopen(wall, "wb");
	CHECK(f);
	fputs("P6\n2 2\n255\n", f);
	const uint8_t px[] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 };
	fwrite(px, 1, sizeof px, f);
	fclose(f);

	snprintf(text, sizeof text,
		 "[wallpaper]\npath = \"%s\"\nmode = \"stretch\"\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", wall, OUT_W, OUT_H);
	write_text(cfg, text);

	g_pid = fork();
	CHECK(g_pid >= 0);
	if (g_pid == 0) {
		setenv("XDG_RUNTIME_DIR", g_dir, 1);
		unsetenv("WAYLAND_DISPLAY");
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
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* ------------------------------------------------------------------------ */
/* Screenshots                                                              */
/* ------------------------------------------------------------------------ */

struct ppm {
	int w, h;
	uint8_t *rgb;
};

static struct ppm screenshot(void)
{
	unlink(g_shot);
	CHECK(kill(g_pid, SIGUSR2) == 0);
	struct stat st;
	for (int t = 0; stat(g_shot, &st) != 0; t += 20) {
		CHECK(t < 5000);
		sleep_ms(20);
	}
	FILE *f = fopen(g_shot, "rb");
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
	(void)t; (void)wd; (void)h;
	struct window *w = d;
	w->activated = false;
	uint32_t *s;
	wl_array_for_each(s, states) {
		if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
			w->activated = true;
	}
}
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
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
static void popup_done(void *d, struct xdg_popup *p) { (void)d; (void)p; }
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

static void test_globals_and_output(struct client *c)
{
	CHECK(global_version(c, "wl_compositor") == 6);
	CHECK(global_version(c, "wl_subcompositor") == 1);
	CHECK(global_version(c, "wl_shm") >= 1);
	CHECK(global_version(c, "xdg_wm_base") == 5);
	CHECK(global_version(c, "wl_seat") == 7);
	CHECK(global_version(c, "wl_data_device_manager") == 3);
	CHECK(global_version(c, "wl_output") == 4);
	CHECK(c->compositor && c->subcompositor && c->shm && c->wm && c->seat && c->ddm && c->output);

	CHECK(c->out_done);
	CHECK(c->out_x == 0 && c->out_y == 0);
	CHECK(c->out_w == OUT_W && c->out_h == OUT_H);
	CHECK(c->out_refresh == 60000);
	CHECK(c->out_scale == 1);
	CHECK(c->out_mode_flags == (WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED));
	CHECK_STR(c->out_name, "OUT");

	CHECK_STR(c->seat_name, "seat0");
	CHECK(c->seat_caps == 0);  /* No input devices until milestone 4. */
}

/* A second client misbehaves; it is disconnected and nobody else notices. */
static void test_protocol_error_is_contained(struct client *good)
{
	struct client bad;
	client_connect(&bad);
	struct wl_surface *s = wl_compositor_create_surface(bad.compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(bad.wm, s);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	(void)tl;
	/* Attaching content before the initial configure is a protocol error. */
	struct wl_buffer *buf = make_buffer(&bad, 10, 10, WL_SHM_FORMAT_XRGB8888, 0);
	wl_surface_attach(s, buf, 0, 0);
	wl_surface_commit(s);
	wl_display_roundtrip(bad.d);  /* fails: the compositor killed us */

	const struct wl_interface *iface = NULL;
	uint32_t id, code;
	CHECK(wl_display_get_error(bad.d) == EPROTO);
	CHECK(wl_display_get_protocol_error(bad.d, &iface, &id) == XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER);
	code = XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER;
	(void)code;
	CHECK(iface && !strcmp(iface->name, "xdg_surface"));
	wl_display_disconnect(bad.d);

	int rt = wl_display_roundtrip(good->d);
	if (rt < 0) {
		const struct wl_interface *gi = NULL; uint32_t gid;
		uint32_t gc = wl_display_get_protocol_error(good->d, &gi, &gid);
		fprintf(stderr, "good client: errno=%d proto_error=%u iface=%s\n", wl_display_get_error(good->d), gc, gi ? gi->name : "-");
	}
	CHECK(rt >= 0);  /* the good client is unaffected */
}

/* Under LeakSanitizer, don't report this test client's own undestroyed proxies
 * and buffers: it exits right after, and they're not what is being tested. The
 * compositor runs as a separate process and is still fully leak-checked. */
extern void __lsan_disable(void) __attribute__((weak));

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (__lsan_disable)
		__lsan_disable();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-render-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	start_compositor(argv[1]);

	struct client c;
	client_connect(&c);
	test_globals_and_output(&c);

	/* ---- Window A: opaque orange 200x100 ------------------------------ */
	struct window a;
	toplevel_create(&c, &a, "A");
	CHECK(a.n_configures == 1 && !a.activated);  /* not mapped, not active yet */
	struct wl_buffer *buf_a = make_buffer(&c, 200, 100, WL_SHM_FORMAT_XRGB8888, 0x00ff8000);
	window_show(&a, buf_a, true);
	CHECK(wait_for(&c, &a.frame_done, 3000));  /* frame callback fires after a real frame */
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(a.activated);  /* the newest window is activated */

	/* ---- Subsurface of A: blue 20x20 at (10,10), synchronized ---------- */
	struct wl_surface *child = wl_compositor_create_surface(c.compositor);
	struct wl_subsurface *sub = wl_subcompositor_get_subsurface(c.subcompositor, child, a.surface);
	wl_subsurface_set_position(sub, 10, 10);
	struct wl_buffer *buf_child = make_buffer(&c, 20, 20, WL_SHM_FORMAT_XRGB8888, 0x000000ff);
	wl_surface_attach(child, buf_child, 0, 0);
	wl_surface_commit(child);  /* synchronized: held back until the parent commits */
	CHECK(wl_display_roundtrip(c.d) >= 0);
	struct ppm before = screenshot();
	CHECK(!near_px(&before, 315, 265, 0, 0, 255, 8));  /* not visible yet */
	free(before.rgb);
	wl_surface_commit(a.surface);  /* releases the child's state */
	CHECK(wl_display_roundtrip(c.d) >= 0);

	/* ---- Popup on A ------------------------------------------------------ */
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(c.wm);
	xdg_positioner_set_size(pos, 50, 40);
	xdg_positioner_set_anchor_rect(pos, 20, 20, 10, 10);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_offset(pos, 5, 5);
	struct window pw;
	memset(&pw, 0, sizeof pw);
	pw.c = &c;
	pw.surface = wl_compositor_create_surface(c.compositor);
	pw.xs = xdg_wm_base_get_xdg_surface(c.wm, pw.surface);
	xdg_surface_add_listener(pw.xs, &xs_listener, &pw);
	pw.popup = xdg_surface_get_popup(pw.xs, a.xs, pos);
	xdg_popup_add_listener(pw.popup, &popup_listener, &pw);
	wl_surface_commit(pw.surface);
	CHECK(wait_for(&c, &pw.popup_configured, 3000));
	CHECK(pw.popup_x == 25 && pw.popup_y == 25 && pw.popup_w == 50 && pw.popup_h == 40);
	struct wl_buffer *buf_popup = make_buffer(&c, 50, 40, WL_SHM_FORMAT_XRGB8888, 0x00ffff00);
	window_show(&pw, buf_popup, false);
	CHECK(wl_display_roundtrip(c.d) >= 0);

	/* ---- Window B: 50% translucent green, 100x100 ------------------------ */
	struct window b;
	toplevel_create(&c, &b, "B");
	/* Premultiplied: green 255 at alpha 128 is stored as G=128, A=128. */
	struct wl_buffer *buf_b = make_buffer(&c, 100, 100, WL_SHM_FORMAT_ARGB8888, 0x80008000);
	window_show(&b, buf_b, true);
	CHECK(wait_for(&c, &b.frame_done, 3000));
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(b.activated);
	CHECK(!a.activated);  /* activation moved to the new window */

	test_protocol_error_is_contained(&c);

	/* ---- Verify what was drawn ------------------------------------------- *
	 * Placement (the placeholder window manager): first window centred, each
	 * further one cascaded by 30px.
	 *   A: 200x100 at (300,250)   B: 100x100 at (380,280)
	 * The cursor sits at the output centre (400,300). */
	struct ppm img = screenshot();
	CHECK(img.w == OUT_W && img.h == OUT_H);

	expect_px(&img, 2, 2, 255, 0, 0, 3, "wallpaper top-left (red)");
	expect_px(&img, OUT_W - 3, 2, 0, 255, 0, 3, "wallpaper top-right (green)");
	expect_px(&img, 2, OUT_H - 3, 0, 0, 255, 3, "wallpaper bottom-left (blue)");
	expect_px(&img, OUT_W - 3, OUT_H - 3, 255, 255, 255, 3, "wallpaper bottom-right (white)");

	expect_px(&img, 305, 255, 255, 128, 0, 2, "window A, opaque");
	expect_px(&img, 485, 335, 255, 128, 0, 2, "window A, near its far corner");
	expect_px(&img, 315, 265, 0, 0, 255, 2, "subsurface of A, at its (10,10) offset");
	expect_px(&img, 330, 280, 255, 255, 0, 2, "popup, above its parent");
	/* B over A: 0.498*orange + premultiplied green. */
	expect_px(&img, 450, 320, 127, 192, 0, 3, "B blended over A");
	/* Cursor: black outline at the hotspot, white fill inside the arrow. */
	expect_px(&img, 400, 300, 0, 0, 0, 0, "cursor outline at hotspot");
	expect_px(&img, 401, 302, 255, 255, 255, 0, "cursor fill");
	free(img.rgb);

	/* ---- Close A (and its popup/subsurface): everything of A disappears ---- */
	xdg_popup_destroy(pw.popup);
	xdg_surface_destroy(pw.xs);
	wl_surface_destroy(pw.surface);
	wl_subsurface_destroy(sub);
	wl_surface_destroy(child);
	xdg_toplevel_destroy(a.tl);
	xdg_surface_destroy(a.xs);
	wl_surface_destroy(a.surface);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(b.activated);

	img = screenshot();
	CHECK(!near_px(&img, 305, 255, 255, 128, 0, 20));
	CHECK(!near_px(&img, 315, 265, 0, 0, 255, 20));
	CHECK(!near_px(&img, 330, 280, 255, 255, 0, 20));
	expect_px(&img, 400, 300, 0, 0, 0, 0, "cursor still drawn");
	free(img.rgb);

	wl_display_disconnect(c.d);
	CHECK(stop_compositor() == 0);

	char *log = slurp(g_log);
	CHECK(log);
	CHECK(!strstr(log, "ERROR"));
	CHECK(strstr(log, "window mapped: \"A\" (rei-test) 200x100"));
	CHECK(strstr(log, "window mapped: \"B\" (rei-test) 100x100"));
	free(log);

	puts("test_render: ok");
	return 0;
}
