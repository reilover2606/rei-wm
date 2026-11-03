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
#define SOCKET "rei-render-test"
#include "testlib.h"

/* Writes the config for this test: a 2x2 PNM wallpaper (red, green / blue,
 * white; stretched, the corners keep their exact colour) and one 800x600 output. */
static void render_config(char *text, size_t len)
{
	char wall[192];
	snprintf(wall, sizeof wall, "%s/wall.ppm", g_dir);
	FILE *f = fopen(wall, "wb");
	CHECK(f);
	fputs("P6\n2 2\n255\n", f);
	const uint8_t px[] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 };
	fwrite(px, 1, sizeof px, f);
	fclose(f);

	snprintf(text, len,
		 "[toolbar]\nenabled = false\n"
		 "[layout]\nnew_windows = \"float\"\n"
		 "[wallpaper]\npath = \"%s\"\nmode = \"stretch\"\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", wall, OUT_W, OUT_H);
}

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
	CHECK(c->seat_caps == (WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD));
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

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-render-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[1024];
	render_config(config, sizeof config);
	start_compositor(argv[1], config, false);

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
