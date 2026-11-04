/*
 * Regression test for a crash seen on real hardware: four windows of one application, closed one
 * by one, took the compositor down (heap corruption) on the second close. Closes them both ways:
 * the client destroying its window, and the notch's close button.
 */
#define SOCKET "rei-close-test"
#include "testlib.h"

#define OUT_W2 1000
#define OUT_H2 600

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-close-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[400];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W2, OUT_H2);
	start_compositor(argv[1], config, true);

	/* Four terminals, each its own process: so each is its own connection, which simply
	 * disappears when the terminal exits (as foot does after the x button). */
	struct client cl[4];
	struct window w[4];
	g_app_id = "foot";
	for (int i = 0; i < 4; i++) {
		client_connect(&cl[i]);
		g_clients[i] = &cl[i];
		g_clients[i + 1] = NULL;
		client_bind_keyboard(&cl[i]);
		char title[16];
		snprintf(title, sizeof title, "term%d", i);
		open_win(&cl[i], &w[i], title, 0x00606060);
	}
	park_pointer();

	for (int i = 3; i >= 1; i--) {
		/* The process dies: every object it had goes at once, in id order. */
		untrack(&w[i]);
		wl_display_disconnect(cl[i].d);
		g_clients[i] = NULL;
		sleep_ms(100);
		settle_all();
		CHECK(waitpid(g_pid, &(int){ 0 }, WNOHANG) == 0);   /* the compositor is still alive */
	}

	/* The last one closes the polite way. */
	untrack(&w[0]);
	xdg_toplevel_destroy(w[0].tl);
	xdg_surface_destroy(w[0].xs);
	wl_surface_destroy(w[0].surface);
	CHECK(wl_display_roundtrip(cl[0].d) >= 0);
	CHECK(waitpid(g_pid, &(int){ 0 }, WNOHANG) == 0);

	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int tt = 0;; tt += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && tt < 5000);
		sleep_ms(10);
	}
	wl_display_disconnect(cl[0].d);
	puts("close sequence: ok");
	return 0;
}
