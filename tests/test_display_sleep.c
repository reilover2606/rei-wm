/*
 * Display sleep: after a while without input the displays are turned off (here the headless backend only logs it); a
 * key press or click that wakes them is swallowed, pointer motion wakes them and passes; an idle inhibitor keeps them
 * on; and the setting can be turned off.
 */
#define SOCKET "rei-displaysleep-test"
#include "testlib.h"

#include "idle-inhibit-unstable-v1-client-protocol.h"

static uint32_t global_id(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].id;
	CHECK(!"global missing");
	return 0;
}

static int key_events(const struct client *c)
{
	int n = 0;
	for (int i = 0; i < c->n_events; i++)
		n += c->events[i].type == EV_KB_KEY;
	return n;
}

static void wait_log(const char *needle, int count, int max_ms)
{
	for (int t = 0; log_count(needle) < count; t += 25) {
		CHECK(t < max_ms);
		sleep_ms(25);
	}
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-displaysleep-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	setenv("REI_DISPLAY_SLEEP_MS", "700", 1);
	char config[400];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", 400, 300);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	client_bind_keyboard(&c);
	struct idle_inhibit_manager_holder { struct zwp_idle_inhibit_manager_v1 *m; } h;
	h.m = wl_registry_bind(c.registry, global_id(&c, "zwp_idle_inhibit_manager_v1"),
			       &zwp_idle_inhibit_manager_v1_interface, 1);
	CHECK(h.m);
	struct window w;
	open_win(&c, &w, "W", 0xff2080c0);
	inject("motion_abs 200 150");
	settle_all();
	CHECK(w.activated);

	/* Typing goes through while awake. */
	inject("key 30 1");
	inject("key 30 0");
	settle_all();
	int typed = key_events(&c);
	CHECK(typed >= 2);

	/* Idle for 700 ms: the display goes off. */
	CHECK(log_count("display power: off") == 0);
	wait_log("display power: off", 1, 4000);

	/* A key press wakes it and is not typed; its release is not either. */
	inject("key 30 1");
	inject("key 30 0");
	settle_all();
	CHECK(log_count("display power: on") == 1);
	CHECK(key_events(&c) == typed);
	/* Awake again: keys arrive. */
	inject("key 30 1");
	inject("key 30 0");
	settle_all();
	CHECK(key_events(&c) > typed);

	/* Asleep again; pointer motion wakes it and carries on. */
	wait_log("display power: off", 2, 4000);
	inject("motion_abs 210 160");
	settle_all();
	CHECK(log_count("display power: on") == 2);

	/* A click that wakes it does not click. */
	wait_log("display power: off", 3, 4000);
	int buttons = 0;
	for (int i = 0; i < c.n_events; i++)
		buttons += c.events[i].type == EV_PTR_BUTTON;
	inject("button 272 1");
	inject("button 272 0");
	settle_all();
	int after = 0;
	for (int i = 0; i < c.n_events; i++)
		after += c.events[i].type == EV_PTR_BUTTON;
	CHECK(log_count("display power: on") == 3 && after == buttons);

	/* With an idle inhibitor the display stays on, however long. */
	struct zwp_idle_inhibitor_v1 *inh = zwp_idle_inhibit_manager_v1_create_inhibitor(h.m, w.surface);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(log_has("display: 1 idle inhibitor"));
	sleep_ms(2200);
	CHECK(log_count("display power: off") == 3);
	/* Released: a new interval starts, and then it sleeps. */
	zwp_idle_inhibitor_v1_destroy(inh);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	wait_log("display power: off", 4, 5000);

	zwp_idle_inhibit_manager_v1_destroy(h.m);
	CHECK(stop_compositor() == 0);
	puts("display sleep: ok");
	return 0;
}
