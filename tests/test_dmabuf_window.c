/*
 * A window whose first (and only) buffer is a GPU buffer, as a GTK4 or Vulkan program's is, maps like any other. (A
 * memfd stands in for the dma-buf: REI_TEST_DMABUF has the headless backend advertise the protocol without a GPU, and
 * creating a buffer does not read it.)
 */
#define SOCKET "rei-dmabuf-win-test"
#include "testlib.h"

#include <sys/mman.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"

static uint32_t global_id(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].id;
	CHECK(!"global missing");
	return 0;
}

static uint32_t g_configure_serial;
static bool g_configured;
static void dw_xs_conf(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d; (void)xs;
	g_configure_serial = serial;
	g_configured = true;
}
static const struct xdg_surface_listener dw_xs_listener = { .configure = dw_xs_conf };
static void dw_tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void dw_tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void dw_tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void dw_tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *a) { (void)d; (void)t; (void)a; }
static const struct xdg_toplevel_listener dw_tl_listener = { dw_tl_conf, dw_tl_close, dw_tl_bounds, dw_tl_caps };

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	setenv("REI_TEST_DMABUF", "1", 1);
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-dmabuf-win-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1], "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 400\nheight = 300\n", true);
	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	struct zwp_linux_dmabuf_v1 *dmabuf =
		wl_registry_bind(c.registry, global_id(&c, "zwp_linux_dmabuf_v1"), &zwp_linux_dmabuf_v1_interface, 3);
	CHECK(dmabuf);

	struct wl_surface *surface = wl_compositor_create_surface(c.compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, surface);
	xdg_surface_add_listener(xs, &dw_xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &dw_tl_listener, NULL);
	xdg_toplevel_set_title(tl, "DMA");
	wl_surface_commit(surface);
	for (int i = 0; i < 100 && !g_configured; i++)
		wl_display_roundtrip(c.d);
	CHECK(g_configured);
	xdg_surface_ack_configure(xs, g_configure_serial);

	/* The first buffer is a GPU buffer. */
	const int w = 160, h = 100, stride = w * 4;
	int fd = memfd_create("fake-dmabuf", MFD_CLOEXEC);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)stride * h) == 0);
	struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
	zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, 0, 0);
	struct wl_buffer *buf = zwp_linux_buffer_params_v1_create_immed(params, w, h, 0x34325241 /* ARGB8888 */, 0);
	zwp_linux_buffer_params_v1_destroy(params);
	close(fd);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, w, h);
	wl_surface_commit(surface);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	settle_all();
	for (int t = 0; log_count("window mapped: \"DMA\"") < 1; t += 25) {
		CHECK(t < 3000);
		sleep_ms(25);
	}
	puts("dmabuf window: ok");
	return 0;
}
