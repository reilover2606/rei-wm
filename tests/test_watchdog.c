/*
 * The watchdog: a stalled event loop is reported and rei-wm exits (so a frozen screen gives the console
 * back), but a wait the loop declares allowed (a VT switch through logind) is left alone.
 */
#define SOCKET "rei-watchdog-test"
#include "testlib.h"

#include <signal.h>

static const char *g_wd_seconds = "1";

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
	setenv("REI_WATCHDOG_SECONDS", g_wd_seconds, 1);
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

	/* A program rei-wm started and that ignores SIGTERM (a stuck pipewire) must not keep it from exiting: it is
	 * waited for a few seconds, then killed. The shutdown used to wait for it for good. */
	{
		char dir[300], script[400], pidfile[400], path[900];
		snprintf(dir, sizeof dir, "%s/fakebin", g_dir);
		CHECK(mkdir(dir, 0700) == 0);
		snprintf(script, sizeof script, "%s/pipewire", dir);
		snprintf(pidfile, sizeof pidfile, "%s/fake.pid", g_dir);
		char body[800];
		snprintf(body, sizeof body, "#!/bin/sh\necho $$ > %s\ntrap '' TERM\nwhile :; do sleep 1; done\n", pidfile);
		write_text(script, body);
		CHECK(chmod(script, 0700) == 0);
		snprintf(path, sizeof path, "%s:%s", dir, getenv("PATH"));
		setenv("PATH", path, 1);
		setenv("REI_AUDIO_AUTOSTART", "1", 1);
		g_wd_seconds = "30";   /* (the watchdog is not what is being tested here) */
		setenv("REI_WATCHDOG_FORCE", "0", 1);
		fresh_start(argv[1], "");
		for (int t = 0; !log_has("started pipewire"); t += 50) {
			CHECK(t < 5000);
			sleep_ms(50);
		}
		char *pidtext = NULL;
		for (int t = 0; !pidtext || !pidtext[0]; t += 50) {
			CHECK(t < 5000);
			sleep_ms(50);
			free(pidtext);
			pidtext = slurp(pidfile);
		}
		int fake = atoi(pidtext);
		free(pidtext);
		CHECK(fake > 1 && kill(fake, 0) == 0);
		CHECK(kill(g_pid, SIGTERM) == 0);
		for (int t = 0;; t += 100) {
			pid_t r = waitpid(g_pid, &status, WNOHANG);
			if (r == g_pid)
				break;
			CHECK(t < 8000);   /* 2 s for the program, then SIGKILL */
			sleep_ms(100);
		}
		g_pid = 0;
		CHECK(log_has("did not exit within") && log_has("killing it"));
		sleep_ms(200);
		CHECK(kill(fake, 0) != 0);   /* and it is gone */
		setenv("REI_AUDIO_AUTOSTART", "0", 1);
		g_wd_seconds = "1";
	}

	/* A shutdown that hangs is reported too, and ends the process (it used to hang for good). */
	fresh_start(argv[1], "shutdown");
	CHECK(kill(g_pid, SIGTERM) == 0);
	for (int t = 0;; t += 100) {
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid)
			break;
		CHECK(t < 15000);
		sleep_ms(100);
	}
	g_pid = 0;
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 70);
	CHECK(log_has("shutting down has not finished"));

	puts("watchdog: ok");
	return 0;
}
