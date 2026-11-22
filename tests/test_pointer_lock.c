/*
 * Pointer constraints and relative motion (what games and Xwayland use) and wheel steps:
 *   - relative_motion events carry what the mouse moved, before and after "acceleration" alike;
 *   - a locked pointer: the cursor stays, the program gets relative motion only; destroying the lock with a cursor
 *     position hint moves the cursor there (that is XWarpPointer);
 *   - a confined pointer cannot leave its surface;
 *   - a wheel notch arrives as axis_value120 (or axis_discrete on older clients) beside the axis value.
 */
#define SOCKET "rei-pointer-lock-test"
#include "testlib.h"

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

static int g_rel_n;
static double g_rel_dx, g_rel_dy, g_rel_udx, g_rel_udy;
static bool g_locked, g_confined;
static int g_unlocked, g_unconfined;

static void rel_motion(void *d, struct zwp_relative_pointer_v1 *r, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy,
		       wl_fixed_t udx, wl_fixed_t udy)
{
	(void)d; (void)r; (void)hi; (void)lo;
	g_rel_n++;
	g_rel_dx += wl_fixed_to_double(dx);
	g_rel_dy += wl_fixed_to_double(dy);
	g_rel_udx += wl_fixed_to_double(udx);
	g_rel_udy += wl_fixed_to_double(udy);
}
static const struct zwp_relative_pointer_v1_listener rel_listener = { .relative_motion = rel_motion };

static void locked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; g_locked = true; }
static void unlocked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; g_locked = false; g_unlocked++; }
static const struct zwp_locked_pointer_v1_listener locked_listener = { .locked = locked, .unlocked = unlocked };
static void confined(void *d, struct zwp_confined_pointer_v1 *l) { (void)d; (void)l; g_confined = true; }
static void unconfined(void *d, struct zwp_confined_pointer_v1 *l) { (void)d; (void)l; g_confined = false; g_unconfined++; }
static const struct zwp_confined_pointer_v1_listener confined_listener = { .confined = confined, .unconfined = unconfined };

static uint32_t global_id(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].id;
	CHECK(!"global missing");
	return 0;
}

static void flush_all(struct client *c)
{
	settle_all();
	wl_display_roundtrip(c->d);
}

/* The newest pointer motion the client saw, in surface coordinates. */
static bool last_motion(const struct client *c, double *x, double *y)
{
	for (int i = c->n_events - 1; i >= 0; i--)
		if (c->events[i].type == EV_PTR_MOTION) {
			*x = c->events[i].x;
			*y = c->events[i].y;
			return true;
		}
	return false;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-pointer-lock-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1],
			 "[toolbar]\nenabled = false\n[layout]\ngap_outer = 0\ngap_inner = 0\n"
			 "[general]\nfocus_follows_mouse = false\n"
			 "[[output]]\nname = \"OUT\"\nwidth = 400\nheight = 300\n",
			 true);
	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	client_bind_keyboard(&c);
	struct zwp_pointer_constraints_v1 *pcm = wl_registry_bind(c.registry, global_id(&c, "zwp_pointer_constraints_v1"),
								   &zwp_pointer_constraints_v1_interface, 1);
	struct zwp_relative_pointer_manager_v1 *rm = wl_registry_bind(
		c.registry, global_id(&c, "zwp_relative_pointer_manager_v1"), &zwp_relative_pointer_manager_v1_interface, 1);
	CHECK(pcm && rm);
	struct zwp_relative_pointer_v1 *rp = zwp_relative_pointer_manager_v1_get_relative_pointer(rm, c.pointer);
	zwp_relative_pointer_v1_add_listener(rp, &rel_listener, NULL);

	struct window w;
	open_win(&c, &w, "W", 0xff2080c0);
	inject("motion_abs 200 150");
	flush_all(&c);
	CHECK(ev_count(&c, EV_PTR_ENTER) >= 1);
	/* The window's top-left on the screen (the notches take some room above it). */
	double ox = 200, oy = 150;
	for (int i = 0; i < c.n_events; i++)
		if (c.events[i].type == EV_PTR_ENTER) {
			ox = 200 - c.events[i].x;
			oy = 150 - c.events[i].y;
		}

	/* ---- Relative motion ---- */
	ev_clear(&c);
	inject("motion 5 3");
	flush_all(&c);
	CHECK(g_rel_n == 1 && g_rel_dx == 5 && g_rel_dy == 3 && g_rel_udx == 5 && g_rel_udy == 3);
	double mx, my;
	CHECK(last_motion(&c, &mx, &my));
	CHECK(mx == 205 - ox && my == 153 - oy);   /* and the cursor moved as usual */

	/* ---- A locked pointer ---- */
	struct zwp_locked_pointer_v1 *lock =
		zwp_pointer_constraints_v1_lock_pointer(pcm, w.surface, c.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	zwp_locked_pointer_v1_add_listener(lock, &locked_listener, NULL);
	flush_all(&c);
	CHECK(g_locked);   /* the surface has the pointer: it starts at once */
	ev_clear(&c);
	g_rel_n = 0;
	g_rel_dx = g_rel_dy = 0;
	inject("motion 10 -4");
	inject("motion 7 9");
	flush_all(&c);
	CHECK(g_rel_n == 2 && g_rel_dx == 17 && g_rel_dy == 5);   /* relative motion arrives... */
	CHECK(ev_count(&c, EV_PTR_MOTION) == 0);                    /* ...but the cursor does not move */
	zwp_locked_pointer_v1_destroy(lock);
	flush_all(&c);
	ev_clear(&c);
	inject("motion 1 1");
	flush_all(&c);
	CHECK(last_motion(&c, &mx, &my) && mx == 206 - ox && my == 154 - oy);   /* still where it was (205,153) + 1 */

	/* ---- A lock with a position hint, destroyed: the cursor warps there (XWarpPointer) ---- */
	lock = zwp_pointer_constraints_v1_lock_pointer(pcm, w.surface, c.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	zwp_locked_pointer_v1_add_listener(lock, &locked_listener, NULL);
	flush_all(&c);
	CHECK(g_locked);
	zwp_locked_pointer_v1_set_cursor_position_hint(lock, wl_fixed_from_int(50), wl_fixed_from_int(60));
	zwp_locked_pointer_v1_destroy(lock);
	flush_all(&c);
	ev_clear(&c);
	inject("motion 1 1");
	flush_all(&c);
	CHECK(last_motion(&c, &mx, &my) && mx == 51 && my == 61);

	/* ---- A confined pointer ---- */
	struct zwp_confined_pointer_v1 *conf = zwp_pointer_constraints_v1_confine_pointer(
		pcm, w.surface, c.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	zwp_confined_pointer_v1_add_listener(conf, &confined_listener, NULL);
	flush_all(&c);
	CHECK(g_confined);
	ev_clear(&c);
	inject("motion 900 900");
	flush_all(&c);
	CHECK(last_motion(&c, &mx, &my) && mx <= 399 - ox && my <= 299 - oy && mx >= 390 - ox && my >= 290 - oy && mx <= w.cfg_w - 1 && my <= w.cfg_h - 1);   /* it stopped at the edge */
	zwp_confined_pointer_v1_destroy(conf);
	flush_all(&c);

	/* ---- The wheel ---- */
	ev_clear(&c);
	inject("wheel v 15 120");
	inject("wheel v -15 -120");
	flush_all(&c);
	CHECK(ev_count(&c, EV_PTR_AXIS) == 2);
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
	CHECK(ev_count(&c, EV_PTR_AXIS120) == 2);
	bool seen_up = false;
	for (int i = 0; i < c.n_events; i++)
		if (c.events[i].type == EV_PTR_AXIS120 && (int32_t)c.events[i].b == -120)
			seen_up = true;
	CHECK(seen_up);
#endif

	zwp_relative_pointer_v1_destroy(rp);
	puts("pointer lock: ok");
	return 0;
}
