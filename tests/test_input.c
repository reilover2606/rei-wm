/*
 * End-to-end input test: keyboard, pointer, focus, keybindings, popup grabs,
 * interactive move and the clipboard, driven through the headless backend's
 * virtual input FIFO and observed from real Wayland clients.
 *
 * Linux evdev codes used: A=30 Q=16 T=20 LEFTSHIFT=42 LEFTMETA(Super)=125
 * BTN_LEFT=272.
 *
 * Layout (800x600 output, placeholder window manager: first window centred,
 * each further one cascaded 30px; the cursor starts at the output centre):
 *   W1 200x100 at (300,250)   moved later by the test
 */
#define SOCKET "rei-input-test"
#include "testlib.h"

#define KEY_A 30
#define KEY_Q 16
#define KEY_T 20
#define KEY_SHIFT 42
#define KEY_SUPER 125
#define BTN_LEFT 272

#define NEAR(a, b) (fabs((a) - (b)) < 0.01)

static void check_near(double got, double want, const char *what)
{
	if (!NEAR(got, want)) {
		fprintf(stderr, "%s: got %g, expected %g\n", what, got, want);
		exit(1);
	}
}

static bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static void wait_file(const char *path)
{
	for (int t = 0; !file_exists(path); t += 10) {
		CHECK(t < 5000);
		sleep_ms(10);
	}
	sleep_ms(30);  /* The shell may still be writing. */
}

/* Maps a window with a solid-colour buffer and settles. */
static void open_window(struct client *c, struct window *w, const char *title,
			int bw, int bh, uint32_t pixel)
{
	toplevel_create(c, w, title);
	window_show(w, make_buffer(c, bw, bh, WL_SHM_FORMAT_XRGB8888, pixel), false);
	CHECK(wl_display_roundtrip(c->d) >= 0);
}

/* ---- data device listeners (clipboard) ------------------------------- */

struct clip {
	bool selection_seen, has_offer;
	int n_mimes;
	char mimes[4][64];
	struct wl_data_offer *offer;
	bool cancelled;
	bool send_requested;
	char send_mime[64];
	int send_fd;
};

static void off_offer(void *d, struct wl_data_offer *o, const char *mime)
{
	(void)o;
	struct clip *cl = d;
	if (cl->n_mimes < 4)
		snprintf(cl->mimes[cl->n_mimes++], 64, "%s", mime);
}
static void off_source_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void off_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_listener = { off_offer, off_source_actions, off_action };

static void dd_data_offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
	(void)dev;
	struct clip *cl = d;
	cl->n_mimes = 0;
	wl_data_offer_add_listener(o, &offer_listener, cl);
}
static void dd_enter(void *d, struct wl_data_device *dev, uint32_t s, struct wl_surface *sf, wl_fixed_t x,
		     wl_fixed_t y, struct wl_data_offer *o)
{
	(void)d; (void)dev; (void)s; (void)sf; (void)x; (void)y; (void)o;
}
static void dd_leave(void *d, struct wl_data_device *dev) { (void)d; (void)dev; }
static void dd_motion(void *d, struct wl_data_device *dev, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)dev; (void)t; (void)x; (void)y;
}
static void dd_drop(void *d, struct wl_data_device *dev) { (void)d; (void)dev; }
static void dd_selection(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
	(void)dev;
	struct clip *cl = d;
	cl->selection_seen = true;
	cl->has_offer = o != NULL;
	cl->offer = o;
}
static const struct wl_data_device_listener dd_listener = {
	dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection,
};

static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd)
{
	(void)s;
	struct clip *cl = d;
	cl->send_requested = true;
	snprintf(cl->send_mime, sizeof cl->send_mime, "%s", mime);
	cl->send_fd = fd;
}
static void src_cancelled(void *d, struct wl_data_source *s) { (void)s; ((struct clip *)d)->cancelled = true; }
static void src_dnd_drop(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_dnd_finished(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener src_listener = {
	src_target, src_send, src_cancelled, src_dnd_drop, src_dnd_finished, src_action,
};

/* ------------------------------------------------------------------------ */

static void test_late_keyboard_bind_and_pointer_enter(struct client *c, struct window *w1)
{
	/* The window mapped under the stationary cursor (400,300): the client gets
	 * its pointer enter without the mouse having moved. */
	const struct ev *e = ev_nth(c, EV_PTR_ENTER, 0);
	CHECK(e && e->surface == w1->surface);
	check_near(e->x, 100, "enter x");
	check_near(e->y, 50, "enter y");

	/* A keyboard bound while the window is already focused still gets keymap,
	 * repeat info and the enter event. */
	ev_clear(c);
	client_bind_keyboard(c);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	const struct ev *km = ev_nth(c, EV_KB_KEYMAP, 0);
	CHECK(km && km->a == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && km->b > 100 && km->c == 1);
	const struct ev *rep = ev_nth(c, EV_KB_REPEAT, 0);
	CHECK(rep && rep->a == 25 && rep->b == 600);
	const struct ev *en = ev_nth(c, EV_KB_ENTER, 0);
	CHECK(en && en->surface == w1->surface);
	CHECK(ev_pos(c, EV_KB_KEYMAP) < ev_pos(c, EV_KB_ENTER));
}

static void test_pointer_motion(struct client *c, struct window *w1)
{
	ev_clear(c);
	inject("motion_abs 320.5 270.25");
	settle(c);
	const struct ev *m = ev_last(c, EV_PTR_MOTION);
	CHECK(m);
	check_near(m->x, 20.5, "motion x");
	check_near(m->y, 20.25, "motion y");
	CHECK(ev_count(c, EV_PTR_ENTER) == 0);  /* still inside: no new enter */

	/* Relative motion accumulates. */
	ev_clear(c);
	inject("motion 10 5");
	settle(c);
	m = ev_last(c, EV_PTR_MOTION);
	CHECK(m);
	check_near(m->x, 30.5, "relative x");
	check_near(m->y, 25.25, "relative y");

	/* Leaving the window. */
	ev_clear(c);
	inject("motion_abs 10 10");
	settle(c);
	const struct ev *l = ev_nth(c, EV_PTR_LEAVE, 0);
	CHECK(l && l->surface == w1->surface);
	CHECK(ev_count(c, EV_PTR_MOTION) == 0);

	/* Coming back in: enter, with the right coordinates. */
	ev_clear(c);
	inject("motion_abs 350 300");
	settle(c);
	const struct ev *e = ev_nth(c, EV_PTR_ENTER, 0);
	CHECK(e && e->surface == w1->surface);
	check_near(e->x, 50, "re-enter x");
	check_near(e->y, 50, "re-enter y");
}

static void test_keys_and_modifiers(struct client *c)
{
	ev_clear(c);
	inject("key %d 1", KEY_A);
	inject("key %d 0", KEY_A);
	settle(c);
	CHECK(ev_count(c, EV_KB_KEY) == 2);
	const struct ev *k0 = ev_nth(c, EV_KB_KEY, 0), *k1 = ev_nth(c, EV_KB_KEY, 1);
	CHECK(k0->a == KEY_A && k0->b == WL_KEYBOARD_KEY_STATE_PRESSED);
	CHECK(k1->a == KEY_A && k1->b == WL_KEYBOARD_KEY_STATE_RELEASED);
	CHECK(k1->c >= k0->c);  /* timestamps don't go backwards */
	CHECK(ev_count(c, EV_KB_MODS) == 0);  /* a letter changes no modifiers */

	/* Shift: the key is forwarded, then the modifier state follows. */
	ev_clear(c);
	inject("key %d 1", KEY_SHIFT);
	settle(c);
	CHECK(ev_count(c, EV_KB_KEY) == 1 && ev_count(c, EV_KB_MODS) == 1);
	CHECK(ev_pos(c, EV_KB_KEY) < ev_pos(c, EV_KB_MODS));
	CHECK(ev_last(c, EV_KB_MODS)->a != 0);
	ev_clear(c);
	inject("key %d 0", KEY_SHIFT);
	settle(c);
	CHECK(ev_last(c, EV_KB_MODS) && ev_last(c, EV_KB_MODS)->a == 0);

	/* A repeated press (already down) or stray release is dropped: clients
	 * must always see a consistent down/up sequence. */
	ev_clear(c);
	inject("key %d 1", KEY_A);
	inject("key %d 1", KEY_A);
	inject("key %d 0", KEY_A);
	inject("key %d 0", KEY_A);
	settle(c);
	CHECK(ev_count(c, EV_KB_KEY) == 2);
}

static void test_exec_binding(struct client *c, const char *marker)
{
	unlink(marker);
	ev_clear(c);
	inject("key %d 1", KEY_SUPER);
	inject("key %d 1", KEY_T);
	inject("key %d 0", KEY_T);
	inject("key %d 0", KEY_SUPER);
	settle(c);

	/* Super itself is forwarded (it's a modifier); the bound key is not, and
	 * neither is its release. */
	int super_events = 0;
	for (int i = 0; i < c->n_events; i++) {
		if (c->events[i].type != EV_KB_KEY)
			continue;
		CHECK(c->events[i].a != KEY_T);
		super_events += c->events[i].a == KEY_SUPER;
	}
	CHECK(super_events == 2);
	CHECK(ev_last(c, EV_KB_MODS) && ev_last(c, EV_KB_MODS)->a == 0);

	/* The command ran, with WAYLAND_DISPLAY pointing at this compositor. */
	wait_file(marker);
	char *text = slurp(marker);
	CHECK(text);
	CHECK_STR(text, SOCKET "\n");
	free(text);
}

static void test_close_binding_and_exact_modifiers(struct client *c, struct window *w1)
{
	/* Mod+q (no shift) is NOT bound: the client sees the keys. */
	ev_clear(c);
	inject("key %d 1", KEY_SUPER);
	inject("key %d 1", KEY_Q);
	inject("key %d 0", KEY_Q);
	inject("key %d 0", KEY_SUPER);
	settle(c);
	CHECK(!w1->close_requested);
	int q_events = 0;
	for (int i = 0; i < c->n_events; i++)
		q_events += c->events[i].type == EV_KB_KEY && c->events[i].a == KEY_Q;
	CHECK(q_events == 2);

	/* Mod+Shift+q closes the focused window and is consumed. */
	ev_clear(c);
	inject("key %d 1", KEY_SUPER);
	inject("key %d 1", KEY_SHIFT);
	inject("key %d 1", KEY_Q);
	inject("key %d 0", KEY_Q);
	inject("key %d 0", KEY_SHIFT);
	inject("key %d 0", KEY_SUPER);
	settle(c);
	CHECK(w1->close_requested);
	for (int i = 0; i < c->n_events; i++)
		CHECK(!(c->events[i].type == EV_KB_KEY && c->events[i].a == KEY_Q));
	w1->close_requested = false;  /* The test keeps the window open. */
}

static void test_axis(struct client *c)
{
	ev_clear(c);
	inject("axis v 15");
	inject("axis h -5");
	settle(c);
	CHECK(ev_count(c, EV_PTR_AXIS) == 2);
	const struct ev *v = ev_nth(c, EV_PTR_AXIS, 0), *h = ev_nth(c, EV_PTR_AXIS, 1);
	CHECK(v->a == WL_POINTER_AXIS_VERTICAL_SCROLL);
	check_near(v->x, 15, "vertical scroll");
	CHECK(h->a == WL_POINTER_AXIS_HORIZONTAL_SCROLL);
	check_near(h->x, -5, "horizontal scroll");
}

static void test_interactive_move(struct client *c, struct window *w1)
{
	inject("motion_abs 350 300");
	settle(c);
	ev_clear(c);
	inject("button %d 1", BTN_LEFT);
	settle(c);
	const struct ev *press = ev_nth(c, EV_PTR_BUTTON, 0);
	CHECK(press && press->a == BTN_LEFT && press->b == WL_POINTER_BUTTON_STATE_PRESSED);

	/* The client answers the press by asking for a move (what a client-side
	 * title bar does). */
	xdg_toplevel_move(w1->tl, c->seat, press->c);
	CHECK(wl_display_roundtrip(c->d) >= 0);

	/* While moving, the client sees no pointer events at all. */
	ev_clear(c);
	inject("motion_abs 400 340");
	settle(c);
	CHECK(ev_count(c, EV_PTR_MOTION) == 0 && ev_count(c, EV_PTR_ENTER) == 0 &&
	      ev_count(c, EV_PTR_LEAVE) == 0);

	/* Releasing ends the move and the pointer re-enters the window at its
	 * new position: moved by (+50,+40), so the cursor is at local (50,50). */
	inject("button %d 0", BTN_LEFT);
	settle(c);
	CHECK(ev_count(c, EV_PTR_BUTTON) == 0);
	const struct ev *e = ev_nth(c, EV_PTR_ENTER, 0);
	CHECK(e && e->surface == w1->surface);
	check_near(e->x, 50, "post-move enter x");
	check_near(e->y, 50, "post-move enter y");

	struct ppm img = screenshot();
	expect_px(&img, 355, 295, 255, 128, 0, 2, "window drawn at its new position");
	CHECK(!near_px(&img, 305, 255, 255, 128, 0, 20));
	free(img.rgb);

	/* A stale serial must not start a move. */
	inject("motion_abs 360 300");
	settle(c);
	xdg_toplevel_move(w1->tl, c->seat, press->c);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	ev_clear(c);
	inject("motion_abs 380 300");
	settle(c);
	CHECK(ev_count(c, EV_PTR_MOTION) == 1);  /* still delivered: no move began */
}

static void test_input_region_passthrough_and_click_focus(struct client *c, struct window *w1,
							  struct window *w2)
{
	/* W2 (100x100, XRGB) lies over W1 but accepts no input. */
	toplevel_create(c, w2, "W2");
	struct wl_region *empty = wl_compositor_create_region(c->compositor);
	wl_surface_set_input_region(w2->surface, empty);
	wl_region_destroy(empty);
	ev_clear(c);
	window_show(w2, make_buffer(c, 100, 100, WL_SHM_FORMAT_XRGB8888, 0x0000ff00), false);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	CHECK(wl_display_roundtrip(c->d) >= 0);

	/* The new window took keyboard focus, in this order. */
	CHECK(w2->activated && !w1->activated);
	const struct ev *leave = ev_nth(c, EV_KB_LEAVE, 0), *enter = ev_nth(c, EV_KB_ENTER, 0);
	CHECK(leave && leave->surface == w1->surface);
	CHECK(enter && enter->surface == w2->surface);
	CHECK(ev_pos(c, EV_KB_LEAVE) < ev_pos(c, EV_KB_ENTER));

	/* The pointer (at (380,300), over both) stays with W1: W2 is transparent
	 * to input, so there is no enter/leave. */
	CHECK(ev_count(c, EV_PTR_ENTER) == 0 && ev_count(c, EV_PTR_LEAVE) == 0);

	/* Clicking there hits W1, which is raised and takes keyboard focus. */
	ev_clear(c);
	inject("button %d 1", BTN_LEFT);
	inject("button %d 0", BTN_LEFT);
	settle(c);
	CHECK(w1->activated && !w2->activated);
	leave = ev_nth(c, EV_KB_LEAVE, 0);
	enter = ev_nth(c, EV_KB_ENTER, 0);
	CHECK(leave && leave->surface == w2->surface);
	CHECK(enter && enter->surface == w1->surface);
	CHECK(ev_count(c, EV_PTR_BUTTON) == 2);  /* the click reached the client too */
}

static void test_popup_grab(struct client *c, struct window *w1, struct window *popup)
{
	/* A menu: grabs the keyboard, and closes on a click outside it. */
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(c->wm);
	xdg_positioner_set_size(pos, 50, 40);
	xdg_positioner_set_anchor_rect(pos, 0, 0, 10, 10);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);

	memset(popup, 0, sizeof *popup);
	popup->c = c;
	popup->surface = wl_compositor_create_surface(c->compositor);
	popup->xs = xdg_wm_base_get_xdg_surface(c->wm, popup->surface);
	xdg_surface_add_listener(popup->xs, &xs_listener, popup);
	popup->popup = xdg_surface_get_popup(popup->xs, w1->xs, pos);
	xdg_popup_add_listener(popup->popup, &popup_listener, popup);
	xdg_popup_grab(popup->popup, c->seat, 0);
	wl_surface_commit(popup->surface);
	CHECK(wait_for(c, &popup->popup_configured, 3000));

	ev_clear(c);
	window_show(popup, make_buffer(c, 50, 40, WL_SHM_FORMAT_XRGB8888, 0x00ffff00), false);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	CHECK(wl_display_roundtrip(c->d) >= 0);

	/* Keyboard focus moved to the popup. */
	const struct ev *leave = ev_nth(c, EV_KB_LEAVE, 0), *enter = ev_nth(c, EV_KB_ENTER, 0);
	CHECK(leave && leave->surface == w1->surface);
	CHECK(enter && enter->surface == popup->surface);

	/* W1 is at (350,290) and the popup sits at its top-left corner:
	 * (350..400, 290..330). The cursor was already at (380,300), so the popup
	 * appeared under it and the pointer entered it without moving. */
	const struct ev *pe = ev_nth(c, EV_PTR_ENTER, 0);
	CHECK(pe && pe->surface == popup->surface);
	check_near(pe->x, 30, "popup enter x");
	check_near(pe->y, 10, "popup enter y");

	/* Motion inside it is reported in the popup's own coordinates, and a click
	 * there doesn't dismiss it. */
	ev_clear(c);
	inject("motion_abs 370 310");
	settle(c);
	const struct ev *pm = ev_last(c, EV_PTR_MOTION);
	CHECK(pm);
	check_near(pm->x, 20, "popup motion x");
	check_near(pm->y, 20, "popup motion y");
	inject("button %d 1", BTN_LEFT);
	inject("button %d 0", BTN_LEFT);
	settle(c);
	CHECK(!popup->popup_dismissed);

	/* A click on the empty desktop does. */
	inject("motion_abs 700 100");
	inject("button %d 1", BTN_LEFT);
	inject("button %d 0", BTN_LEFT);
	settle(c);
	CHECK(popup->popup_dismissed);

	/* The client destroys the popup; keyboard focus returns to the window. */
	ev_clear(c);
	xdg_popup_destroy(popup->popup);
	xdg_surface_destroy(popup->xs);
	wl_surface_destroy(popup->surface);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	CHECK(wl_display_roundtrip(c->d) >= 0);
	enter = ev_nth(c, EV_KB_ENTER, 0);
	CHECK(enter && enter->surface == w1->surface);
}

static void test_clipboard(struct client *a, struct client *b, struct window *w1)
{
	/* Static: the listeners registered below outlive this function (the
	 * compositor re-announces the clipboard on every later focus change). */
	static struct clip ca, cb;
	memset(&ca, 0, sizeof ca);
	memset(&cb, 0, sizeof cb);
	struct wl_data_device *dev_a = wl_data_device_manager_get_data_device(a->ddm, a->seat);
	struct wl_data_device *dev_b = wl_data_device_manager_get_data_device(b->ddm, b->seat);
	wl_data_device_add_listener(dev_a, &dd_listener, &ca);
	wl_data_device_add_listener(dev_b, &dd_listener, &cb);
	client_bind_keyboard(b);
	CHECK(wl_display_roundtrip(a->d) >= 0);
	CHECK(wl_display_roundtrip(b->d) >= 0);

	/* A has the keyboard, so A may take the clipboard. */
	struct wl_data_source *src = wl_data_device_manager_create_data_source(a->ddm);
	wl_data_source_add_listener(src, &src_listener, &ca);
	wl_data_source_offer(src, "text/plain");
	wl_data_source_offer(src, "text/plain;charset=utf-8");
	wl_data_device_set_selection(dev_a, src, 0);
	CHECK(wl_display_roundtrip(a->d) >= 0);
	CHECK(!ca.cancelled);

	/* B opens a window; focus moves to B and the clipboard is announced to it. */
	struct window w4;
	open_window(b, &w4, "B", 100, 100, 0x00ff00ff);
	CHECK(wl_display_roundtrip(b->d) >= 0);
	CHECK(wl_display_roundtrip(b->d) >= 0);
	CHECK(cb.selection_seen && cb.has_offer);
	CHECK(cb.n_mimes == 2);
	CHECK_STR(cb.mimes[0], "text/plain");
	CHECK_STR(cb.mimes[1], "text/plain;charset=utf-8");

	/* B reads it: the request reaches A, A writes, B reads the same bytes. */
	int pipefd[2];
	CHECK(pipe(pipefd) == 0);
	wl_data_offer_receive(cb.offer, "text/plain", pipefd[1]);
	close(pipefd[1]);
	CHECK(wait_for2(a, b, &ca.send_requested, 3000));
	CHECK_STR(ca.send_mime, "text/plain");
	const char *msg = "clipboard-text";
	CHECK(write(ca.send_fd, msg, strlen(msg)) == (ssize_t)strlen(msg));
	close(ca.send_fd);
	char buf[64] = { 0 };
	CHECK(read(pipefd[0], buf, sizeof buf - 1) == (ssize_t)strlen(msg));
	CHECK_STR(buf, msg);
	close(pipefd[0]);

	/* Focus goes back to A (click on the part of W1 nobody covers): now B is a
	 * background client and may not replace the clipboard. */
	inject("motion_abs 545 330");
	inject("button %d 1", BTN_LEFT);
	inject("button %d 0", BTN_LEFT);
	input_sync();
	CHECK(wl_display_roundtrip(a->d) >= 0);
	CHECK(wl_display_roundtrip(b->d) >= 0);
	CHECK(w1->activated);

	static struct clip cb2;
	memset(&cb2, 0, sizeof cb2);
	struct wl_data_source *evil = wl_data_device_manager_create_data_source(b->ddm);
	wl_data_source_add_listener(evil, &src_listener, &cb2);
	wl_data_source_offer(evil, "text/plain");
	wl_data_device_set_selection(dev_b, evil, 0);
	CHECK(wl_display_roundtrip(b->d) >= 0);
	CHECK(cb2.cancelled);
	CHECK(!ca.cancelled);  /* A's clipboard is untouched */

	xdg_toplevel_destroy(w4.tl);
	xdg_surface_destroy(w4.xs);
	wl_surface_destroy(w4.surface);
	CHECK(wl_display_roundtrip(b->d) >= 0);
}

/*
 * A client vanishes without tidying up while it holds keyboard and pointer
 * focus. libwayland then destroys its wl_surface *before* the xdg objects, so the
 * surface's destroy listeners run while the window is still mapped. This once
 * made the compositor spin forever (a listener list was modified mid-emit).
 */
static void test_abrupt_disconnect_while_focused(struct client *a, struct window *w1)
{
	struct client doomed;
	client_connect(&doomed);
	client_bind_pointer(&doomed);
	client_bind_keyboard(&doomed);

	/* Put its window under the pointer and give it the keyboard. */
	inject("motion_abs 500 340");
	input_sync();
	struct window wd;
	open_window(&doomed, &wd, "doomed", 300, 200, 0x000000ff);
	CHECK(wl_display_roundtrip(doomed.d) >= 0);
	CHECK(wl_display_roundtrip(doomed.d) >= 0);
	CHECK(ev_count(&doomed, EV_KB_ENTER) == 1 && ev_count(&doomed, EV_PTR_ENTER) >= 1);
	CHECK(wl_display_roundtrip(a->d) >= 0);
	CHECK(!w1->activated);

	/* Hold a button too, so the pointer grab is involved as well. */
	inject("button %d 1", BTN_LEFT);
	input_sync();

	ev_clear(a);
	wl_display_disconnect(doomed.d);  /* no destroy requests: the socket just closes */

	/* The compositor survives (input is still processed within the timeout)
	 * and focus falls back to the remaining window. */
	inject("button %d 0", BTN_LEFT);
	input_sync();
	CHECK(wl_display_roundtrip(a->d) >= 0);
	CHECK(w1->activated);
	const struct ev *enter = ev_nth(a, EV_KB_ENTER, 0);
	CHECK(enter && enter->surface == w1->surface);
}

static void test_release_all(struct client *c)
{
	inject("motion_abs 360 320");
	settle(c);
	ev_clear(c);
	inject("key %d 1", KEY_A);
	inject("button %d 1", BTN_LEFT);
	inject("release_all");
	settle(c);
	/* Everything held is released, so nothing is left stuck down. */
	const struct ev *kr = NULL, *br = NULL;
	for (int i = 0; i < c->n_events; i++) {
		if (c->events[i].type == EV_KB_KEY && c->events[i].b == 0 && c->events[i].a == KEY_A)
			kr = &c->events[i];
		if (c->events[i].type == EV_PTR_BUTTON && c->events[i].b == 0)
			br = &c->events[i];
	}
	CHECK(kr && br);
}

static void test_cursor_clamping(struct client *c)
{
	/* The cursor can't leave the output. Its outline pixel (the hotspot) shows
	 * where it is. */
	inject("motion_abs 5000 5000");
	settle(c);
	struct ppm img = screenshot();
	expect_px(&img, OUT_W - 1, OUT_H - 1, 0, 0, 0, 0, "cursor clamped to the bottom-right");
	free(img.rgb);

	inject("motion -10000 -10000");
	settle(c);
	img = screenshot();
	expect_px(&img, 0, 0, 0, 0, 0, 0, "cursor clamped to the top-left");
	free(img.rgb);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-input-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char marker[200], config[900];
	snprintf(marker, sizeof marker, "%s/exec-ran", g_dir);
	snprintf(config, sizeof config,
		 "[layout]\nnew_windows = \"float\"\n"
		 "[keybindings]\n\"Mod+t\" = \"exec echo $WAYLAND_DISPLAY > %s\"\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", marker, OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client a, b;
	client_connect(&a);
	CHECK(a.seat_caps == (WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD));
	client_bind_pointer(&a);

	struct window w1, w2, popup;
	open_window(&a, &w1, "W1", 200, 100, 0x00ff8000);  /* opaque orange */
	CHECK(wl_display_roundtrip(a.d) >= 0);

	test_late_keyboard_bind_and_pointer_enter(&a, &w1);
	test_pointer_motion(&a, &w1);
	test_keys_and_modifiers(&a);
	test_exec_binding(&a, marker);
	test_close_binding_and_exact_modifiers(&a, &w1);
	test_axis(&a);
	test_interactive_move(&a, &w1);          /* W1 now at (350,290) */
	test_input_region_passthrough_and_click_focus(&a, &w1, &w2);
	test_popup_grab(&a, &w1, &popup);

	client_connect(&b);
	test_clipboard(&a, &b, &w1);
	test_abrupt_disconnect_while_focused(&a, &w1);
	test_release_all(&a);
	test_cursor_clamping(&a);

	/* With no window at all, bindings still work and keys don't crash anything. */
	xdg_toplevel_destroy(w1.tl);
	xdg_surface_destroy(w1.xs);
	wl_surface_destroy(w1.surface);
	xdg_toplevel_destroy(w2.tl);
	xdg_surface_destroy(w2.xs);
	wl_surface_destroy(w2.surface);
	CHECK(wl_display_roundtrip(a.d) >= 0);
	unlink(marker);
	inject("key %d 1", KEY_A);
	inject("key %d 0", KEY_A);
	inject("key %d 1", KEY_SUPER);
	inject("key %d 1", KEY_T);
	inject("key %d 0", KEY_T);
	inject("key %d 0", KEY_SUPER);
	inject("button %d 1", BTN_LEFT);
	inject("button %d 0", BTN_LEFT);
	input_sync();
	wait_file(marker);

	/* A binding can end the session. */
	wl_display_disconnect(b.d);
	wl_display_disconnect(a.d);
	/* Mod+Shift+E, as ONE write: the compositor exits on the press, and writing
	 * the releases separately afterwards could hit a FIFO nobody reads. */
	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int t = 0;; t += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && t < 5000);
		sleep_ms(10);
	}

	char *log = slurp(g_log);
	CHECK(log && !strstr(log, "ERROR"));
	CHECK(strstr(log, "quit requested by keybinding"));
	free(log);

	puts("test_input: ok");
	return 0;
}
