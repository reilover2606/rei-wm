/*
 * xdg-decoration: who draws a window's title bar. With [decoration] client_titlebars = true (the default) the
 * compositor answers "client side", with false "server side" (rei-wm draws no title bars: programs leave theirs out).
 * A change of the setting, on a config reload, reaches windows that already asked, in a new configure; and a
 * program's own request does not override it.
 */
#define SOCKET "rei-xdgdeco-test"
#include "testlib.h"

#include "xdg-decoration-unstable-v1-client-protocol.h"

static uint32_t global_id(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].id;
	CHECK(!"global missing");
	return 0;
}

struct mode_log {
	int n;
	uint32_t last;
};

static void on_mode(void *d, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode)
{
	(void)deco;
	struct mode_log *m = d;
	m->n++;
	m->last = mode;
}
static const struct zxdg_toplevel_decoration_v1_listener deco_listener = { on_mode };

static void reload(const char *text)
{
	char path[500], tmp[520];
	snprintf(path, sizeof path, "%s/config.toml", g_dir);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	write_text(tmp, text);
	int ok = log_count("config reloaded");
	CHECK(rename(tmp, path) == 0);
	CHECK(kill(g_pid, SIGHUP) == 0);
	for (int t = 0; log_count("config reloaded") <= ok; t += 20) {
		CHECK(t < 5000);
		sleep_ms(20);
	}
}

static void roundtrips(struct client *c)
{
	for (int i = 0; i < 3; i++)
		CHECK(wl_display_roundtrip(c->d) >= 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-xdgdeco-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	const char *tail = "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 600\nheight = 400\n";
	char config[600];
	snprintf(config, sizeof config, "%s", tail);   /* the default: client side */
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	struct zxdg_decoration_manager_v1 *mgr = wl_registry_bind(c.registry, global_id(&c, "zxdg_decoration_manager_v1"),
								   &zxdg_decoration_manager_v1_interface, 1);
	CHECK(mgr);
	struct window w;
	toplevel_create(&c, &w, "D");
	struct mode_log log = { 0, 0 };
	struct zxdg_toplevel_decoration_v1 *deco = zxdg_decoration_manager_v1_get_toplevel_decoration(mgr, w.tl);
	zxdg_toplevel_decoration_v1_add_listener(deco, &deco_listener, &log);
	roundtrips(&c);
	CHECK(log.n >= 1 && log.last == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);

	/* The program asks for server side; the setting says client side, and that is the answer. */
	zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	roundtrips(&c);
	CHECK(log.last == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);

	/* Hiding the title bars: the window that asked is told, with a configure. */
	int cfgs = w.n_configures, seen = log.n;
	snprintf(config, sizeof config, "[decoration]\nclient_titlebars = false\n%s", tail);
	reload(config);
	roundtrips(&c);
	CHECK(log.n > seen && log.last == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	CHECK(w.n_configures > cfgs);

	/* A window made afterwards starts with it. */
	struct window w2;
	toplevel_create(&c, &w2, "E");
	struct mode_log log2 = { 0, 0 };
	struct zxdg_toplevel_decoration_v1 *deco2 = zxdg_decoration_manager_v1_get_toplevel_decoration(mgr, w2.tl);
	zxdg_toplevel_decoration_v1_add_listener(deco2, &deco_listener, &log2);
	roundtrips(&c);
	CHECK(log2.n >= 1 && log2.last == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

	/* Back: both are told. */
	snprintf(config, sizeof config, "%s", tail);
	reload(config);
	roundtrips(&c);
	CHECK(log.last == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE && log2.last == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);

	zxdg_toplevel_decoration_v1_destroy(deco2);
	zxdg_toplevel_decoration_v1_destroy(deco);
	zxdg_decoration_manager_v1_destroy(mgr);
	CHECK(stop_compositor() == 0);
	puts("xdg decoration: ok");
	return 0;
}
