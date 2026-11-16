/*
 * The watchdog: a stalled event loop is reported and rei-wm exits (so a frozen screen gives the console
 * back), but a wait the loop declares allowed (a VT switch through logind) is left alone.
 */
#define SOCKET "rei-watchdog-test"
#include "testlib.h"

#include <signal.h>

static void fresh_start(const char *binary, const char *selftest)
{
	char cmd[800];
	snprintf(cmd, sizeof cmd, "rm -f %s/input.fifo %s/%s %s/%s.lock %s/log.txt", g_dir, g_dir, SOCKET, g_dir, SOCKET, g_dir);
	CHECK(system(cmd) == 0);
	g_pid = 0;
	if (g_fifo_fd >= 0)
		close(g_fifo_fd);
	g_fifo_fd = -1;
	setenv("REI_WATCHDOG_FORCE", "1", 1);
	setenv("REI_WATCHDOG_SECONDS", "1", 1);
	setenv("REI_WATCHDOG_SELFTEST", selftest, 1);
	start_compositor(binary, "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n", true);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-watchdog-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);

	/* A real stall (the loop spins): reported, and rei-wm exits with status 70. */
	fresh_start(argv[1], "1");
	int status = 0;
	for (int t = 0;; t += 100) {
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid)
			break;
		CHECK(t < 15000);
		sleep_ms(100);
	}
	g_pid = 0;
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 70);
	CHECK(log_has("the event loop has not run"));

	/* A declared wait that outlasts the limit four times over: nothing is reported, rei-wm carries on. */
	fresh_start(argv[1], "suspended");
	sleep_ms(7000);
	CHECK(kill(g_pid, 0) == 0);
	CHECK(!log_has("the event loop has not run"));
	CHECK(stop_compositor() == 0);

	puts("watchdog: ok");
	return 0;
}
