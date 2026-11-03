/*
 * End-to-end test of Xwayland support: rei-wm starts Xwayland, becomes its window
 * manager, and X11 windows join the same tiling/floating tree as native ones. The
 * X11 client here is plain libxcb. Skipped (exit 77) when Xwayland is not installed.
 *
 * Output: 800x600, toolbar off, gaps 4, notch height 22 (so the tiling area is
 * (4,22)-(796,596)).
 */
#define SOCKET "rei-xwl-test"
#include "testlib.h"

#include <xcb/xcb.h>

#define OUT_W 800
#define OUT_H 600

static xcb_connection_t *g_x;
static xcb_screen_t *g_scr;
static xcb_atom_t a_protocols, a_delete, a_name, a_utf8, a_hints, a_state, a_fullscreen;

static xcb_atom_t atom(const char *name)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
		g_x, xcb_intern_atom(g_x, 0, (uint16_t)strlen(name), name), NULL);
	CHECK(r);
	xcb_atom_t a = r->atom;
	free(r);
	return a;
}

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

/* An X11 window with a solid background. `fixed` gives it a fixed size, which the
 * window manager floats; otherwise it is tiled. */
static xcb_window_t make_window(int w, int h, uint32_t color, bool fixed, bool override_redirect,
				const char *title)
{
	xcb_window_t win = xcb_generate_id(g_x);
	uint32_t values[2] = { color, override_redirect };
	xcb_create_window(g_x, XCB_COPY_FROM_PARENT, win, g_scr->root, 20, 20, (uint16_t)w, (uint16_t)h, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT, g_scr->root_visual,
			  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values);
	if (title)
		xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, win, a_name, a_utf8, 8,
				    (uint32_t)strlen(title), title);
	if (fixed) {
		uint32_t hints[18] = { 16 | 32, 0, 0, 0, 0, (uint32_t)w, (uint32_t)h, (uint32_t)w, (uint32_t)h };
		xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, win, a_hints, a_hints, 32, 18, hints);
	}
	xcb_atom_t proto = a_delete;
	xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, win, a_protocols, XCB_ATOM_ATOM, 32, 1, &proto);
	xcb_flush(g_x);
	return win;
}

/* Paints the whole window, as a real client does when it gets an Expose event. */
static void paint(xcb_window_t win, uint32_t color)
{
	xcb_gcontext_t gc = xcb_generate_id(g_x);
	xcb_create_gc(g_x, gc, win, XCB_GC_FOREGROUND, &color);
	xcb_rectangle_t all = { 0, 0, 4000, 4000 };
	xcb_poly_fill_rectangle(g_x, win, gc, 1, &all);
	xcb_free_gc(g_x, gc);
	xcb_flush(g_x);
}

static void geometry(xcb_window_t win, int *x, int *y, int *w, int *h)
{
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(g_x, xcb_get_geometry(g_x, win), NULL);
	CHECK(g);
	xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(
		g_x, xcb_translate_coordinates(g_x, win, g_scr->root, 0, 0), NULL);
	CHECK(t);
	*x = t->dst_x;
	*y = t->dst_y;
	*w = g->width;
	*h = g->height;
	free(g);
	free(t);
}

static bool g_got_delete;
static void pump_x(void)
{
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_event(g_x))) {
		if ((ev->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
			const xcb_client_message_event_t *m = (void *)ev;
			if (m->type == a_protocols && m->data.data32[0] == a_delete)
				g_got_delete = true;
		}
		free(ev);
	}
}

static xcb_window_t active_window(void)
{
	xcb_atom_t a = atom("_NET_ACTIVE_WINDOW");
	xcb_get_property_reply_t *r = xcb_get_property_reply(
		g_x, xcb_get_property(g_x, 0, g_scr->root, a, XCB_ATOM_WINDOW, 0, 1), NULL);
	xcb_window_t w = 0;
	if (r && xcb_get_property_value_length(r) >= 4)
		w = *(xcb_window_t *)xcb_get_property_value(r);
	free(r);
	return w;
}

/* Polls screenshots until a pixel has the expected colour: Xwayland shows a window
 * before the X server has painted its background, so the first frame can be black. */
static struct ppm wait_px(int x, int y, int r, int g, int b, const char *what)
{
	struct ppm img;
	for (int t = 0;; t += 100) {
		img = screenshot();
		if (near_px(&img, x, y, r, g, b, 0) || t >= 4000)
			break;
		free(img.rgb);
		sleep_ms(100);
	}
	expect_px(&img, x, y, r, g, b, 0, what);
	return img;
}

static void test_floating_and_tiled(void)
{
	/* A fixed-size window floats, centred, at its own size. */
	xcb_window_t fl = make_window(300, 200, 0xff0000, true, false, "float");
	xcb_map_window(g_x, fl);
	xcb_flush(g_x);
	wait_log("x11 window mapped: \"float\"", 8000);
	paint(fl, 0xff0000);
	sleep_ms(200);

	struct ppm img = wait_px(300, 250, 255, 0, 0, "a floating X11 window is drawn, centred");
	expect_px(&img, 30, 300, 27, 39, 51, 0, "and leaves the rest of the desktop alone");
	free(img.rgb);

	int x, y, w, h;
	geometry(fl, &x, &y, &w, &h);
	CHECK(w == 300 && h == 200);
	CHECK(abs(x - (OUT_W - 300) / 2) <= 30 && y > 0 && y < OUT_H - 200);
	CHECK(active_window() == fl);  /* New windows take the focus. */

	/* A resizable window is tiled and given the slot: the tiling area is split with the floater out of it. */
	xcb_window_t tl = make_window(100, 100, 0x0000ff, false, false, "tiled");
	xcb_map_window(g_x, tl);
	xcb_flush(g_x);
	wait_log("x11 window mapped: \"tiled\"", 8000);
	paint(tl, 0x0000ff);
	sleep_ms(300);
	geometry(tl, &x, &y, &w, &h);
	CHECK(x == 4 && y == 22 && w == 792 && h == 574);
	CHECK(active_window() == tl);
	img = wait_px(100, 100, 0, 0, 255, "a tiled X11 window fills its slot");
	free(img.rgb);

	/* The notch's close button asks politely (WM_DELETE_WINDOW), as with native windows. */
	g_got_delete = false;
	inject("motion_abs 780 10");
	inject("button 272 1");
	inject("button 272 0");
	for (int t = 0; !g_got_delete && t < 5000; t += 20) {
		pump_x();
		sleep_ms(20);
	}
	CHECK(g_got_delete);

	/* Once the client destroys the window, it is gone from the screen. */
	xcb_destroy_window(g_x, tl);
	xcb_flush(g_x);
	sleep_ms(400);
	img = screenshot();
	CHECK(!near_px(&img, 100, 100, 0, 0, 255, 0));
	free(img.rgb);
	xcb_destroy_window(g_x, fl);
	xcb_flush(g_x);
}

static void test_override_redirect(void)
{
	/* Menus and tooltips (override-redirect) are not managed: drawn where they ask. */
	xcb_window_t base = make_window(300, 200, 0x00ff00, true, false, "base");
	xcb_map_window(g_x, base);
	xcb_flush(g_x);
	wait_log("x11 window mapped: \"base\"", 8000);

	xcb_window_t menu = make_window(60, 40, 0xffff00, false, true, NULL);
	uint32_t pos[2] = { 50, 50 };
	xcb_configure_window(g_x, menu, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
	xcb_map_window(g_x, menu);
	xcb_flush(g_x);
	wait_log("(override-redirect)", 8000);
	paint(menu, 0xffff00);
	sleep_ms(200);

	struct ppm img = wait_px(70, 70, 255, 255, 0, "an override-redirect window is drawn where X11 put it");
	expect_px(&img, 40, 70, 27, 39, 51, 0, "and is not tiled or decorated");
	free(img.rgb);

	xcb_unmap_window(g_x, menu);
	xcb_flush(g_x);
	sleep_ms(300);
	img = screenshot();
	CHECK(!near_px(&img, 70, 70, 255, 255, 0, 0));
	free(img.rgb);
	xcb_destroy_window(g_x, menu);
	xcb_destroy_window(g_x, base);
	xcb_flush(g_x);
}

static void test_fullscreen(void)
{
	xcb_window_t win = make_window(100, 100, 0xff00ff, false, false, "full");
	xcb_map_window(g_x, win);
	xcb_flush(g_x);
	wait_log("x11 window mapped: \"full\"", 8000);
	sleep_ms(200);

	/* The client asks for fullscreen through _NET_WM_STATE. */
	xcb_client_message_event_t m = { .response_type = XCB_CLIENT_MESSAGE, .format = 32, .window = win,
					 .type = a_state };
	m.data.data32[0] = 1;
	m.data.data32[1] = a_fullscreen;
	xcb_send_event(g_x, 0, g_scr->root,
		       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&m);
	xcb_flush(g_x);
	sleep_ms(500);
	int x, y, w, h;
	geometry(win, &x, &y, &w, &h);
	CHECK(x == 0 && y == 0 && w == OUT_W && h == OUT_H);
	xcb_destroy_window(g_x, win);
	xcb_flush(g_x);
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
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-xwl-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[500];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n"
		 "[layout]\ngap_outer = 4\ngap_inner = 4\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	wait_log("xwayland: ready on DISPLAY=", 40000);
	char *log = slurp(g_log);
	const char *at = strstr(log, "DISPLAY=:");
	CHECK(at);
	char display[32];
	CHECK(sscanf(at, "DISPLAY=%31s", display) == 1);
	free(log);

	int screen = 0;
	g_x = xcb_connect(display, &screen);
	CHECK(g_x && !xcb_connection_has_error(g_x));
	g_scr = xcb_setup_roots_iterator(xcb_get_setup(g_x)).data;
	a_protocols = atom("WM_PROTOCOLS");
	a_delete = atom("WM_DELETE_WINDOW");
	a_name = atom("_NET_WM_NAME");
	a_utf8 = atom("UTF8_STRING");
	a_hints = atom("WM_NORMAL_HINTS");
	a_state = atom("_NET_WM_STATE");
	a_fullscreen = atom("_NET_WM_STATE_FULLSCREEN");

	test_floating_and_tiled();
	test_override_redirect();
	test_fullscreen();

	xcb_disconnect(g_x);
	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int t = 0;; t += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && t < 8000);
		sleep_ms(10);
	}
	puts("xwayland: ok");
	return 0;
}
