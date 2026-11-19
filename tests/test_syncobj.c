/*
 * Explicit synchronization (wp_linux_drm_syncobj_v1), against the fake kernel of the headless backend (REI_TEST_SYNCOBJ):
 * a timeline is a memfd whose first 8 bytes are the last signaled point.
 *   - a commit with an acquire point is held until the point is signaled, and not shown before;
 *   - commits stay in order: a later one waits for an earlier one that is still waiting;
 *   - the release point is signaled once the compositor is done with the buffer (here: at once, it copies shm buffers);
 *   - the protocol's rules are enforced (no acquire point, no buffer, conflicting points, a second object).
 */
#define SOCKET "rei-syncobj-test"
#include "testlib.h"

#include <sys/mman.h>

#include "linux-drm-syncobj-v1-client-protocol.h"

#define RED 0x00ff0000
#define GREEN 0x0000ff00
#define BLUE 0x000000ff
#define YELLOW 0x00ffff00

static uint32_t global_id(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].id;
	CHECK(!"global missing");
	return 0;
}

static int new_timeline(void)
{
	int fd = memfd_create("timeline", MFD_CLOEXEC);
	CHECK(fd >= 0 && ftruncate(fd, 8) == 0);
	uint64_t zero = 0;
	CHECK(pwrite(fd, &zero, 8, 0) == 8);
	return fd;
}

static void signal_point(int fd, uint64_t v)
{
	CHECK(pwrite(fd, &v, 8, 0) == 8);
}

static uint64_t read_point(int fd)
{
	uint64_t v = 0;
	CHECK(pread(fd, &v, 8, 0) == 8);
	return v;
}

/* The colour in the middle of the window, polled until it is `want` (or for a while). */
static bool shows(uint32_t rgb, int ms)
{
	for (int t = 0;; t += 50) {
		struct ppm img = screenshot();
		bool ok = near_px(&img, 200, 150, (int)(rgb >> 16 & 0xff), (int)(rgb >> 8 & 0xff), (int)(rgb & 0xff), 2);
		free(img.rgb);
		if (ok || t >= ms)
			return ok;
		sleep_ms(50);
	}
}

static void roundtrips(struct client *c)
{
	for (int i = 0; i < 3; i++)
		CHECK(wl_display_roundtrip(c->d) >= 0);
}

struct session {
	struct client c;
	struct wp_linux_drm_syncobj_manager_v1 *mgr;
	struct window w;
};

static void open_session(struct session *s, uint32_t color)
{
	client_connect(&s->c);
	s->mgr = wl_registry_bind(s->c.registry, global_id(&s->c, "wp_linux_drm_syncobj_manager_v1"),
				  &wp_linux_drm_syncobj_manager_v1_interface, 1);
	CHECK(s->mgr);
	g_clients[0] = &s->c;
	open_win(&s->c, &s->w, "W", color);
}

static struct wl_buffer *colour_buffer(struct session *s, uint32_t rgb)
{
	return make_buffer(&s->c, s->w.buf_w, s->w.buf_h, WL_SHM_FORMAT_XRGB8888, rgb);
}

/* The protocol error code the compositor killed this client with, or 0. */
static uint32_t error_of(struct client *c, const struct wl_interface *want)
{
	wl_display_roundtrip(c->d);
	const struct wl_interface *iface = NULL;
	uint32_t id = 0;
	uint32_t code = wl_display_get_protocol_error(c->d, &iface, &id);
	if (code && want)
		CHECK(iface == want);
	return code;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	setenv("REI_TEST_SYNCOBJ", "1", 1);
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-syncobj-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1], "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 600\nheight = 400\n", true);

	/* ---- The rules ---- */
	{   /* a second object on the surface */
		static struct session b;
		open_session(&b, RED);
		struct wp_linux_drm_syncobj_surface_v1 *first = wp_linux_drm_syncobj_manager_v1_get_surface(b.mgr, b.w.surface);
		struct wp_linux_drm_syncobj_surface_v1 *again = wp_linux_drm_syncobj_manager_v1_get_surface(b.mgr, b.w.surface);
		(void)first;
		(void)again;
		CHECK(error_of(&b.c, &wp_linux_drm_syncobj_manager_v1_interface) == WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS);
		wl_display_disconnect(b.c.d);
		g_clients[0] = NULL;
		g_nwins = 0;   /* (its window is gone) */
	}
	const struct {
		const char *what;
		bool attach, set_acq, set_rel, same_timeline;
		uint32_t code;
	} cases[] = {
		{ "no acquire point", true, false, true, false, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT },
		{ "no release point", true, true, false, false, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT },
		{ "points without a buffer", false, true, true, false, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER },
		{ "acquire not before release", true, true, true, true, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		static struct session b;
		open_session(&b, RED);
		int ta = new_timeline(), tr = new_timeline();
		struct wp_linux_drm_syncobj_timeline_v1 *xa = wp_linux_drm_syncobj_manager_v1_import_timeline(b.mgr, ta);
		struct wp_linux_drm_syncobj_timeline_v1 *xr = cases[i].same_timeline ? xa : wp_linux_drm_syncobj_manager_v1_import_timeline(b.mgr, tr);
		struct wp_linux_drm_syncobj_surface_v1 *obj = wp_linux_drm_syncobj_manager_v1_get_surface(b.mgr, b.w.surface);
		if (cases[i].attach) {
			wl_surface_attach(b.w.surface, colour_buffer(&b, GREEN), 0, 0);
			wl_surface_damage(b.w.surface, 0, 0, 1 << 20, 1 << 20);
		}
		if (cases[i].set_acq)
			wp_linux_drm_syncobj_surface_v1_set_acquire_point(obj, xa, 0, 5);
		if (cases[i].set_rel)
			wp_linux_drm_syncobj_surface_v1_set_release_point(obj, xr, 0, 5);
		wl_surface_commit(b.w.surface);
		uint32_t code = error_of(&b.c, &wp_linux_drm_syncobj_surface_v1_interface);
		if (code != cases[i].code) {
			fprintf(stderr, "%s: error %u, expected %u\n", cases[i].what, code, cases[i].code);
			return 1;
		}
		wl_display_disconnect(b.c.d);
		g_clients[0] = NULL;
		g_nwins = 0;
		close(ta);
		close(tr);
	}

	static struct session a;
	open_session(&a, RED);
	CHECK(shows(RED, 2000));

	int acq = new_timeline(), rel = new_timeline();
	struct wp_linux_drm_syncobj_timeline_v1 *tl_acq = wp_linux_drm_syncobj_manager_v1_import_timeline(a.mgr, acq);
	struct wp_linux_drm_syncobj_timeline_v1 *tl_rel = wp_linux_drm_syncobj_manager_v1_import_timeline(a.mgr, rel);
	struct wp_linux_drm_syncobj_surface_v1 *so = wp_linux_drm_syncobj_manager_v1_get_surface(a.mgr, a.w.surface);

	/* ---- A commit waits for its acquire point ---- */
	wl_surface_attach(a.w.surface, colour_buffer(&a, GREEN), 0, 0);
	wl_surface_damage(a.w.surface, 0, 0, 1 << 20, 1 << 20);
	wp_linux_drm_syncobj_surface_v1_set_acquire_point(so, tl_acq, 0, 1);
	wp_linux_drm_syncobj_surface_v1_set_release_point(so, tl_rel, 0, 1);
	wl_surface_commit(a.w.surface);
	roundtrips(&a.c);
	CHECK(!shows(GREEN, 400));          /* held: still the old content */
	CHECK(shows(RED, 100));
	CHECK(read_point(rel) >= 1);        /* the compositor copied it already, so the release point is signaled */
	signal_point(acq, 1);               /* the "GPU" is done */
	CHECK(shows(GREEN, 3000));

	/* ---- Commits stay in order ---- */
	wl_surface_attach(a.w.surface, colour_buffer(&a, BLUE), 0, 0);
	wl_surface_damage(a.w.surface, 0, 0, 1 << 20, 1 << 20);
	wp_linux_drm_syncobj_surface_v1_set_acquire_point(so, tl_acq, 0, 2);
	wp_linux_drm_syncobj_surface_v1_set_release_point(so, tl_rel, 0, 2);
	wl_surface_commit(a.w.surface);
	wl_surface_attach(a.w.surface, colour_buffer(&a, YELLOW), 0, 0);
	wl_surface_damage(a.w.surface, 0, 0, 1 << 20, 1 << 20);
	wp_linux_drm_syncobj_surface_v1_set_acquire_point(so, tl_acq, 0, 3);
	wp_linux_drm_syncobj_surface_v1_set_release_point(so, tl_rel, 0, 3);
	wl_surface_commit(a.w.surface);
	roundtrips(&a.c);
	CHECK(!shows(BLUE, 300) && !shows(YELLOW, 100));   /* both wait */
	signal_point(acq, 2);
	CHECK(shows(BLUE, 3000));                           /* the first, not the second */
	CHECK(!shows(YELLOW, 300));
	signal_point(acq, 3);
	CHECK(shows(YELLOW, 3000));
	CHECK(read_point(rel) >= 3);

	/* A point already signaled: the commit takes effect at once. */
	wl_surface_attach(a.w.surface, colour_buffer(&a, GREEN), 0, 0);
	wl_surface_damage(a.w.surface, 0, 0, 1 << 20, 1 << 20);
	wp_linux_drm_syncobj_surface_v1_set_acquire_point(so, tl_acq, 0, 2);
	wp_linux_drm_syncobj_surface_v1_set_release_point(so, tl_rel, 0, 4);
	wl_surface_commit(a.w.surface);
	roundtrips(&a.c);
	CHECK(shows(GREEN, 3000));

	/* The compositor is still fine, and a surface that is destroyed with a commit still waiting does not leave it
	 * stuck. */
	wl_surface_attach(a.w.surface, colour_buffer(&a, BLUE), 0, 0);
	wl_surface_damage(a.w.surface, 0, 0, 1 << 20, 1 << 20);
	wp_linux_drm_syncobj_surface_v1_set_acquire_point(so, tl_acq, 0, 99);
	wp_linux_drm_syncobj_surface_v1_set_release_point(so, tl_rel, 0, 99);
	wl_surface_commit(a.w.surface);
	roundtrips(&a.c);
	CHECK(error_of(&a.c, NULL) == 0);
	wl_display_disconnect(a.c.d);
	sleep_ms(200);
	CHECK(kill(g_pid, 0) == 0);
	g_clients[0] = NULL;
	CHECK(stop_compositor() == 0);
	puts("syncobj: ok");
	return 0;
}
