/* An X11 window with a 32-bit ARGB visual (what urxvt's depth/background transparency uses) is blended
 * with what is behind it, not drawn opaque. */
#define SOCKET "rei-xwalpha-test"
#include "testlib.h"

#include <xcb/xcb.h>

#define OUT_W 800
#define OUT_H 600

static bool have_xwayland(void)
{
	const char *path = getenv("PATH");
	if (!path)
		return false;
	char *copy = strdup(path), *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		char full[600];
		snprintf(full, sizeof full, "%s/Xwayland", dir);
		if (access(full, X_OK) == 0) {
			free(copy);
			return true;
		}
	}
	free(copy);
	return false;
}

static void wait_log(const char *needle, int timeout_ms)
{
	for (int t = 0; !log_has(needle); t += 20) {
		if (t >= timeout_ms) {
			fprintf(stderr, "timed out waiting for log line '%s'\n", needle);
			exit(1);
		}
		sleep_ms(20);
	}
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!have_xwayland()) {
		fprintf(stderr, "Xwayland is not installed: skipping\n");
		return 77;
	}
	quiet_client_leaks();
	setenv("REI_TEST_XWAYLAND", "1", 1);
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-xwalpha-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[300];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	/* A stand-in xrdb records how it is called; HOME holds an .Xresources for it to be given. */
	{
		char bin[220], file[300], body[500], path[1200];
		snprintf(bin, sizeof bin, "%s/bin", g_dir);
		CHECK(mkdir(bin, 0700) == 0);
		snprintf(file, sizeof file, "%s/xrdb", bin);
		snprintf(body, sizeof body, "#!/bin/sh\necho \"$DISPLAY $*\" > \"%s/xrdb.out\"\n", g_dir);
		write_text(file, body);
		CHECK(chmod(file, 0755) == 0);
		snprintf(file, sizeof file, "%s/.Xresources", g_dir);
		write_text(file, "URxvt.depth: 32\nURxvt.background: [80]#000000\n");
		snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
		setenv("PATH", path, 1);
		setenv("HOME", g_dir, 1);
	}
	start_compositor(argv[1], config, true);

	wait_log("xwayland: ready on DISPLAY=", 40000);
	char *log = slurp(g_log);
	const char *at = strstr(log, "DISPLAY=:");
	CHECK(at);
	char display[32];
	CHECK(sscanf(at, "DISPLAY=%31s", display) == 1);
	free(log);

	/* The X resources were loaded into this display with xrdb -merge, as a session script would. */
	{
		char out[300], *got = NULL;
		snprintf(out, sizeof out, "%s/xrdb.out", g_dir);
		for (int t = 0; t < 40 && !got; t++) {
			got = slurp(out);
			if (!got)
				sleep_ms(100);
		}
		CHECK(got);
		char expect[400];
		snprintf(expect, sizeof expect, "%s -merge %s/.Xresources", display, g_dir);
		CHECK(strncmp(got, expect, strlen(expect)) == 0);
		free(got);
	}

	int screen = 0;
	xcb_connection_t *x = xcb_connect(display, &screen);
	CHECK(x && !xcb_connection_has_error(x));
	xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(x)).data;

	/* The wallpaper is published as _XROOTPMAP_ID, for pseudo-transparency (urxvt.transparent). */
	{
		xcb_intern_atom_reply_t *ar = xcb_intern_atom_reply(x, xcb_intern_atom(x, 0, 13, "_XROOTPMAP_ID"), NULL);
		CHECK(ar);
		xcb_get_property_reply_t *pr = NULL;
		for (int t = 0; t < 40; t++) {
			pr = xcb_get_property_reply(x, xcb_get_property(x, 0, scr->root, ar->atom, XCB_ATOM_PIXMAP, 0, 1), NULL);
			if (pr && xcb_get_property_value_length(pr) == 4)
				break;
			free(pr);
			pr = NULL;
			sleep_ms(100);
		}
		free(ar);
		CHECK(pr);
		xcb_pixmap_t pm = *(xcb_pixmap_t *)xcb_get_property_value(pr);
		free(pr);
		xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(x, xcb_get_geometry(x, pm), NULL);
		CHECK(g && g->width == 800 && g->height == 600);
		free(g);
		xcb_get_image_reply_t *im = xcb_get_image_reply(
			x, xcb_get_image(x, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, 100, 100, 1, 1, ~0u), NULL);
		CHECK(im && xcb_get_image_data_length(im) >= 3);
		const uint8_t *d = xcb_get_image_data(im);
		CHECK(d[2] == 27 && d[1] == 39 && d[0] == 51);   /* the plain desktop colour (B,G,R order) */
		free(im);
	}

	/* Find a 32-bit TrueColor visual. */
	xcb_visualid_t vis = 0;
	for (xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(scr); di.rem && !vis; xcb_depth_next(&di)) {
		if (di.data->depth != 32)
			continue;
		for (xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data); vi.rem; xcb_visualtype_next(&vi))
			if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
				vis = vi.data->visual_id;
				break;
			}
	}
	CHECK(vis);   /* Xwayland offers an ARGB visual */

	xcb_colormap_t cmap = xcb_generate_id(x);
	xcb_create_colormap(x, XCB_COLORMAP_ALLOC_NONE, cmap, scr->root, vis);
	xcb_window_t win = xcb_generate_id(x);
	uint32_t values[3] = { 0, 0, cmap };   /* back pixel (transparent), border pixel, colormap */
	xcb_create_window(x, 32, win, scr->root, 20, 20, 300, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, vis,
			  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP, values);
	const char *title = "alpha";
	xcb_atom_t net_wm_name = 0, utf8 = 0;
	{
		xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(x, xcb_intern_atom(x, 0, 12, "_NET_WM_NAME"), NULL);
		net_wm_name = r->atom;
		free(r);
		r = xcb_intern_atom_reply(x, xcb_intern_atom(x, 0, 11, "UTF8_STRING"), NULL);
		utf8 = r->atom;
		free(r);
	}
	xcb_change_property(x, XCB_PROP_MODE_REPLACE, win, net_wm_name, utf8, 8, (uint32_t)strlen(title), title);
	{
		xcb_generic_error_t *e0 = xcb_request_check(x, xcb_create_colormap_checked(x, XCB_COLORMAP_ALLOC_NONE, xcb_generate_id(x), scr->root, vis));
		if (e0) { fprintf(stderr, "X error %d on colormap\n", e0->error_code); free(e0); }
	}
	xcb_map_window(x, win);
	xcb_flush(x);
	wait_log("x11 window mapped: \"alpha\"", 8000);

	/* Half-transparent red, premultiplied ARGB: alpha 0x80, red 0x80. */
	xcb_gcontext_t gc = xcb_generate_id(x);
	uint32_t fg = 0x80800000;
	xcb_create_gc(x, gc, win, XCB_GC_FOREGROUND, &fg);
	xcb_rectangle_t all = { 0, 0, 4000, 4000 };
	struct ppm img = { 0 };
	for (int t = 0; t < 4000; t += 250) {   /* it appears once Xwayland has committed the painted window */
		xcb_void_cookie_t ck = xcb_poly_fill_rectangle_checked(x, win, gc, 1, &all);
		xcb_generic_error_t *err = xcb_request_check(x, ck);
		if (err) {
			fprintf(stderr, "X error %d on poly_fill\n", err->error_code);
			free(err);
		}
		sleep_ms(250);
		img = screenshot();
		const uint8_t *q = img.rgb + ((size_t)100 * (size_t)img.w + 100) * 3;
		if (q[0] != 0 || q[1] != 0 || q[2] != 0)
			break;
		free(img.rgb);
		img.rgb = NULL;
	}
	if (!img.rgb)
		img = screenshot();
	int px = 0, py = 0;
	/* Somewhere inside the window: the middle of the screen is within the centred 300x200 floater. */
	px = 100; py = 100;   /* (the cursor sits in the middle of the screen) */
	const uint8_t *p = img.rgb + ((size_t)py * (size_t)img.w + (size_t)px) * 3;
	/* The desktop is (27,39,51); half-transparent red over it is about (141,19,25), neither opaque red
	 * (255,0,0) nor the bare desktop. */
	CHECK(p[0] > 100 && p[0] < 200);
	CHECK(p[1] < 45 && p[2] < 60);
	CHECK(!(p[0] == 27 && p[1] == 39 && p[2] == 51));
	free(img.rgb);
	xcb_disconnect(x);

	CHECK(stop_compositor() == 0);
	printf("xwalpha: ok\n");
	return 0;
}
