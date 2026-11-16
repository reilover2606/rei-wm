/* Xwayland at an output scale of 2: its X screen is the *logical* size, so X pixels line up with ours. */
#define SOCKET "rei-xwscale-test"
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
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-xwscale-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char config[300];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\nscale = 2\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	wait_log("xwayland: ready on DISPLAY=", 40000);
	char *log = slurp(g_log);
	const char *at = strstr(log, "DISPLAY=:");
	CHECK(at);
	char display[32];
	CHECK(sscanf(at, "DISPLAY=%31s", display) == 1);
	free(log);

	int screen = 0;
	xcb_connection_t *x = xcb_connect(display, &screen);
	CHECK(x && !xcb_connection_has_error(x));
	xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(x)).data;
	/* 800x600 at scale 2 is 400x300 logical; X must see that, not the 800x600 of the mode. */
	CHECK(scr->width_in_pixels == OUT_W / 2 && scr->height_in_pixels == OUT_H / 2);
	xcb_disconnect(x);

	CHECK(stop_compositor() == 0);
	puts("xwscale: ok");
	return 0;
}
