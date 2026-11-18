/*
 * Copy and paste with X11 programs (Xwayland does not bridge selections; rei-wm, as its window manager, does):
 *   - an X11 terminal highlights text (it takes the X11 PRIMARY selection): a Wayland program gets the primary
 *     selection and, because the window being typed into is a terminal (WM_CLASS XTerm), the clipboard too;
 *   - a Wayland program sets the clipboard: an X11 program reads it as CLIPBOARD;
 *   - and a Wayland program's primary selection is the X11 PRIMARY.
 * The X11 programs are xclip and a window made here with libxcb. Skipped (77) without Xwayland or xclip.
 */
#define SOCKET "rei-xsel-test"
#include "testlib.h"

#include <xcb/xcb.h>

#include "primary-selection-unstable-v1-client-protocol.h"
#include "selclient.h"

static bool have_program(const char *name)
{
	const char *path = getenv("PATH");
	if (!path)
		return false;
	char *copy = strdup(path), *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		char full[600];
		snprintf(full, sizeof full, "%s/%s", dir, name);
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

/* Runs `cmd` with DISPLAY set, returning what it printed. */
static void run(const char *display, const char *cmd, char *out, size_t n)
{
	char full[1200];
	snprintf(full, sizeof full, "DISPLAY=%s %s", display, cmd);
	FILE *f = popen(full, "r");
	CHECK(f);
	size_t got = out ? fread(out, 1, n - 1, f) : 0;
	if (out)
		out[got] = '\0';
	pclose(f);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!have_program("Xwayland") || !have_program("xclip")) {
		puts("xselection: skipped (needs Xwayland and xclip)");
		return 77;
	}
	quiet_client_leaks();
	setenv("REI_TEST_XWAYLAND", "1", 1);
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-xsel-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1],
			 "[toolbar]\nenabled = false\n[general]\nfocus_follows_mouse = false\n"
			 "[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n",
			 true);
	wait_log("xwayland: ready on DISPLAY=", 40000);
	char display[16] = ":0";
	{
		char *text = slurp(g_log);
		const char *p = text ? strstr(text, "xwayland: ready on DISPLAY=") : NULL;
		if (p)
			sscanf(p + strlen("xwayland: ready on DISPLAY="), "%15s", display);
		free(text);
	}

	struct client b;
	client_connect(&b);
	g_clients[0] = &b;
	client_bind_keyboard(&b);
	static struct seen sb;
	sb.c = &b;
	struct wl_data_device *ddb = wl_data_device_manager_get_data_device(b.ddm, b.seat);
	wl_data_device_add_listener(ddb, &dd_listener, &sb);
	struct zwp_primary_selection_device_manager_v1 *pmb = wl_registry_bind(
		b.registry, global_id(&b, "zwp_primary_selection_device_manager_v1"), &zwp_primary_selection_device_manager_v1_interface, 1);
	struct zwp_primary_selection_device_v1 *pdb = zwp_primary_selection_device_manager_v1_get_device(pmb, b.seat);
	zwp_primary_selection_device_v1_add_listener(pdb, &pd_listener, &sb);

	/* An X11 "terminal": a window of class XTerm, focused as soon as it maps. */
	xcb_connection_t *x = xcb_connect(display, NULL);
	CHECK(x && !xcb_connection_has_error(x));
	xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(x)).data;
	xcb_window_t win = xcb_generate_id(x);
	xcb_create_window(x, XCB_COPY_FROM_PARENT, win, scr->root, 20, 20, 300, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->root_visual, 0, NULL);
	xcb_change_property(x, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 12, "xterm\0XTerm");
	xcb_atom_t net_wm_name = xcb_intern_atom_reply(x, xcb_intern_atom(x, 0, 12, "_NET_WM_NAME"), NULL)->atom;
	xcb_atom_t utf8 = xcb_intern_atom_reply(x, xcb_intern_atom(x, 0, 11, "UTF8_STRING"), NULL)->atom;
	xcb_change_property(x, XCB_PROP_MODE_REPLACE, win, net_wm_name, utf8, 8, 5, "xterm");
	xcb_map_window(x, win);
	xcb_flush(x);
	{   /* (Xwayland shows a window once it has drawn something) */
		uint32_t red = 0x204060;
		xcb_gcontext_t gc = xcb_generate_id(x);
		xcb_create_gc(x, gc, win, XCB_GC_FOREGROUND, &red);
		xcb_rectangle_t all = { 0, 0, 4000, 4000 };
		for (int i = 0; i < 20 && !log_has("x11 window mapped: \"xterm\""); i++) {
			xcb_poly_fill_rectangle(x, win, gc, 1, &all);
			xcb_flush(x);
			sleep_ms(100);
		}
	}
	wait_log("x11 window mapped: \"xterm\"", 8000);
	sleep_ms(300);

	/* ---- Highlighted text in the X11 terminal (PRIMARY), taken by an X11 program (xclip) on its behalf ---- */
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s/highlight.txt", g_dir);
	FILE *hf = fopen(tmp, "w");
	fputs("highlighted in an X11 terminal", hf);
	fclose(hf);
	char cmd[800];
	/* First a large one (a megabyte and a half arrives in pieces: INCR), then another: each replaces the last, and the
	 * clipboard follows. */
	char big[600];
	snprintf(big, sizeof big, "%s/big.txt", g_dir);
	FILE *bf = fopen(big, "w");
	for (int i = 0; i < 150000; i++)
		fputs("0123456789", bf);
	fclose(bf);
	snprintf(cmd, sizeof cmd, "xclip -i -selection primary %s >/dev/null 2>&1", big);
	run(display, cmd, NULL, 0);
	wait_log("xselection: an X11 program owns the primary selection", 5000);
	for (int t = 0; log_count("highlighted in a terminal are now the clipboard") < 1; t += 50) {
		CHECK(t < 8000);
		sleep_ms(50);
	}
	snprintf(cmd, sizeof cmd, "xclip -o -selection clipboard | wc -c");
	char count[64];
	run(display, cmd, count, sizeof count);
	CHECK(atoi(count) == 1500000);   /* an X11 program reads the whole of it as the clipboard */

	snprintf(cmd, sizeof cmd, "xclip -i -selection primary %s >/dev/null 2>&1", tmp);
	run(display, cmd, NULL, 0);
	for (int t = 0; log_count("highlighted in a terminal are now the clipboard") < 2; t += 50) {
		CHECK(t < 8000);
		sleep_ms(50);
	}
	char xclip_got[200];
	run(display, "xclip -o -selection clipboard", xclip_got, sizeof xclip_got);
	CHECK_STR(xclip_got, "highlighted in an X11 terminal");   /* the newer highlight replaced the older */

	/* A Wayland program that gets the focus later has the primary selection and the clipboard. */
	struct window wb;
	open_win(&b, &wb, "wayland program", 0xff406020);
	for (int i = 0; i < 4; i++)
		CHECK(wl_display_roundtrip(b.d) >= 0);
	CHECK(wb.activated);
	CHECK(sb.prim_offer && sb.clip_offer);
	char got[200];
	read_clip(&b, &b, sb.clip, "text/plain;charset=utf-8", got, sizeof got);
	CHECK_STR(got, "highlighted in an X11 terminal");
	read_prim(&b, &b, sb.prim, "text/plain", got, sizeof got);   /* a middle click: fetched from the X11 owner */
	CHECK_STR(got, "highlighted in an X11 terminal");

	/* ---- A Wayland program's clipboard, read by an X11 program ---- */
	static struct seen sw;
	sw.c = &b;
	sw.text = "copied in a Wayland program";
	/* (the primary selection is the simpler Wayland source here; the clipboard direction is the mirror above) */
	struct zwp_primary_selection_source_v1 *ps = zwp_primary_selection_device_manager_v1_create_source(pmb);
	zwp_primary_selection_source_v1_add_listener(ps, &ps_listener, &sw);
	zwp_primary_selection_source_v1_offer(ps, "text/plain;charset=utf-8");
	zwp_primary_selection_source_v1_offer(ps, "text/plain");
	zwp_primary_selection_device_v1_set_selection(pdb, ps, 0);
	CHECK(wl_display_roundtrip(b.d) >= 0);
	/* the X11 program's read is answered by rei-wm from the Wayland source; pump the Wayland client meanwhile */
	snprintf(cmd, sizeof cmd, "xclip -o -selection primary");
	char full[1200];
	snprintf(full, sizeof full, "DISPLAY=%s %s", display, cmd);
	FILE *pf = popen(full, "r");
	CHECK(pf);
	for (int i = 0; i < 40 && !sw.prim_send; i++) {
		CHECK(wl_display_roundtrip(b.d) >= 0);
		sleep_ms(25);
	}
	char xgot[200] = { 0 };
	size_t n = fread(xgot, 1, sizeof xgot - 1, pf);
	pclose(pf);
	(void)n;
	CHECK(sw.prim_send);
	CHECK_STR(xgot, "copied in a Wayland program");

	xcb_disconnect(x);
	CHECK(stop_compositor() == 0);
	puts("xselection: ok");
	return 0;
}
