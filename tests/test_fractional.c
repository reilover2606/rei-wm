/*
 * wp_fractional_scale_v1 and wp_viewporter: a client is told the exact scale (in 120ths), the
 * viewport destination sets the surface's logical size, the scale follows a live change, and a
 * second viewport on one surface is an error.
 */
#define SOCKET "rei-fractional-test"
#include "testlib.h"

#include <errno.h>
#include <signal.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

static uint32_t g_preferred;

static void on_preferred(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale)
{
	(void)data; (void)f;
	g_preferred = scale;
}

static const struct wp_fractional_scale_v1_listener frac_listener = { on_preferred };

static uint32_t global_id(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].id;
	CHECK(!"global missing");
	return 0;
}

static void write_config(double scale)
{
	char text[400], path[300], tmp[320];
	snprintf(text, sizeof text,
		 "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\nscale = %g\n",
		 OUT_W, OUT_H, scale);
	snprintf(path, sizeof path, "%s/config.toml", g_dir);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	write_text(tmp, text);
	CHECK(rename(tmp, path) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-fractional-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	char config[400];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\nscale = 1.5\n",
		 OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);

	struct wp_viewporter *vp = wl_registry_bind(c.registry, global_id(&c, "wp_viewporter"),
						    &wp_viewporter_interface, 1);
	struct wp_fractional_scale_manager_v1 *mgr = wl_registry_bind(
		c.registry, global_id(&c, "wp_fractional_scale_manager_v1"),
		&wp_fractional_scale_manager_v1_interface, 1);
	CHECK(vp && mgr);

	struct window w;
	toplevel_create(&c, &w, "V");

	/* The scale arrives at once: 1.5 is 180/120 (wl_output still says 2, rounded up). */
	struct wp_fractional_scale_v1 *frac = wp_fractional_scale_manager_v1_get_fractional_scale(mgr, w.surface);
	wp_fractional_scale_v1_add_listener(frac, &frac_listener, NULL);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(g_preferred == 180);

	/* The viewport destination is the logical size, whatever the buffer's pixels. */
	struct wp_viewport *view = wp_viewporter_get_viewport(vp, w.surface);
	wp_viewport_set_destination(view, 120, 90);
	struct wl_buffer *buf = make_buffer(&c, 240, 180, WL_SHM_FORMAT_XRGB8888, 0x3a7bd5);
	window_show(&w, buf, false);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	settle_all();
	CHECK(log_has("window mapped: \"V\" (rei-test) 120x90"));

	/* A live scale change reaches the client. */
	g_preferred = 0;
	write_config(2.0);
	CHECK(kill(g_pid, SIGHUP) == 0);
	for (int t = 0; g_preferred != 240; t += 50) {
		CHECK(t < 5000);
		CHECK(wl_display_roundtrip(c.d) >= 0);
		sleep_ms(50);
	}

	/* Closing a window that has a fractional-scale object must not touch the freed window (it did). */
	{
		struct window w2;
		toplevel_create(&c, &w2, "W2");
		struct wp_fractional_scale_v1 *f2 = wp_fractional_scale_manager_v1_get_fractional_scale(mgr, w2.surface);
		wp_fractional_scale_v1_add_listener(f2, &frac_listener, NULL);
		struct wl_buffer *b2 = make_buffer(&c, 40, 40, WL_SHM_FORMAT_XRGB8888, 0xaa5500);
		window_show(&w2, b2, false);
		CHECK(wl_display_roundtrip(c.d) >= 0);
		settle_all();
		xdg_toplevel_destroy(w2.tl);
		xdg_surface_destroy(w2.xs);
		wl_surface_destroy(w2.surface);
		CHECK(wl_display_roundtrip(c.d) >= 0);
		CHECK(log_has("window destroyed: \"W2\""));
		wp_fractional_scale_v1_destroy(f2);
		CHECK(wl_display_roundtrip(c.d) >= 0);
	}

	/* A second viewport on the same surface is a protocol error. */
	struct wp_viewport *second = wp_viewporter_get_viewport(vp, w.surface);
	(void)second;
	CHECK(wl_display_roundtrip(c.d) < 0);
	CHECK(wl_display_get_error(c.d) == EPROTO);

	printf("fractional: ok\n");
	return 0;
}
