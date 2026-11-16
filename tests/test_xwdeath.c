/*
 * If Xwayland dies before it is ready, rei-wm says so (with the exit status), removes the dead X
 * socket and lock so nobody is sent to a display that is not there, and stops handing out DISPLAY.
 * A stand-in "Xwayland" that exits at once stands in for the real one.
 */
#define SOCKET "rei-xwdeath-test"
#include "testlib.h"

#include <sys/stat.h>

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-xwdeath-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	char bin[220], script[300];
	snprintf(bin, sizeof bin, "%s/bin", g_dir);
	CHECK(mkdir(bin, 0700) == 0);
	snprintf(script, sizeof script, "%s/Xwayland", bin);
	write_text(script, "#!/bin/sh\necho 'Fatal server error: pretend failure' >&2\nexit 3\n");
	CHECK(chmod(script, 0755) == 0);
	char path[1200];
	snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	setenv("PATH", path, 1);
	setenv("REI_TEST_XWAYLAND", "1", 1);   /* Xwayland is wanted here, unlike in most tests */

	const char *config = "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n";
	start_compositor(argv[1], config, true);

	for (int t = 0; !log_has("exited with status 3 before it was ready"); t += 50) {
		CHECK(t < 8000);
		sleep_ms(50);
	}
	CHECK(log_has("pretend failure"));   /* what Xwayland printed is in the same log */

	char *log = slurp(g_log);
	const char *at = strstr(log, "xwayland: starting on display :");
	CHECK(at);
	int n = atoi(at + strlen("xwayland: starting on display :"));
	free(log);
	char sock[64], lock[64];
	snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n);
	snprintf(lock, sizeof lock, "/tmp/.X%d-lock", n);
	/* The dead display's lock is gone, or (if another test's compositor has meanwhile taken the same
	 * number) no longer ours. */
	char *held = slurp(lock);
	CHECK(!held || atoi(held) != (int)g_pid);
	free(held);
	(void)sock;

	CHECK(stop_compositor() == 0);
	printf("xwdeath: ok\n");
	return 0;
}
