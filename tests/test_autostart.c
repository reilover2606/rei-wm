/*
 * [audio] autostart: with no PipeWire socket, rei-wm starts pipewire, then wireplumber and
 * pipewire-pulse; it stops them when it exits; and with autostart off it starts nothing.
 * The three programs are stand-in scripts that only leave marks.
 */
#define SOCKET "rei-autostart-test"
#include "testlib.h"

#include <signal.h>
#include <sys/stat.h>

static char g_marks[200];

static bool exists(const char *name)
{
	char path[300];
	snprintf(path, sizeof path, "%s/%s", g_marks, name);
	struct stat st;
	return stat(path, &st) == 0;
}

static void script(const char *dir, const char *name, const char *extra)
{
	char path[300], body[600];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	snprintf(body, sizeof body, "#!/bin/sh\necho $$ > \"%s/%s\"\n%s\nexec sleep 60\n", g_marks, name, extra);
	write_text(path, body);
	CHECK(chmod(path, 0755) == 0);
}

static pid_t pid_of(const char *name)
{
	char path[300];
	snprintf(path, sizeof path, "%s/%s", g_marks, name);
	char *s = slurp(path);
	pid_t p = s ? (pid_t)atoi(s) : 0;
	free(s);
	return p;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-autostart-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_marks, sizeof g_marks, "%s/marks", g_dir);
	CHECK(mkdir(g_marks, 0700) == 0);
	char bin[220];
	snprintf(bin, sizeof bin, "%s/bin", g_dir);
	CHECK(mkdir(bin, 0700) == 0);

	/* "pipewire" creates its socket (a plain file is enough for the existence check). */
	char sockcmd[300];
	snprintf(sockcmd, sizeof sockcmd, "touch \"%s/pipewire-0\"", g_dir);
	script(bin, "pipewire", sockcmd);
	script(bin, "wireplumber", "");
	script(bin, "pipewire-pulse", "");
	char path[1200];
	snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	setenv("PATH", path, 1);
	setenv("REI_AUDIO_AUTOSTART", "1", 1);

	const char *config = "[audio]\nenabled = true\n[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n";
	start_compositor(argv[1], config, true);

	for (int t = 0; !(exists("pipewire") && exists("wireplumber") && exists("pipewire-pulse")); t += 50) {
		CHECK(t < 15000);
		sleep_ms(50);
	}
	CHECK(log_has("audio: autostart: started pipewire"));
	CHECK(log_has("audio: autostart: started wireplumber"));
	CHECK(log_has("audio: autostart: started pipewire-pulse"));
	pid_t pw = pid_of("pipewire"), wp = pid_of("wireplumber"), pp = pid_of("pipewire-pulse");
	CHECK(pw > 0 && wp > 0 && pp > 0);
	CHECK(kill(pw, 0) == 0 && kill(wp, 0) == 0 && kill(pp, 0) == 0);

	/* Quitting rei-wm stops what it started. */
	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int t = 0;; t += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && t < 8000);
		sleep_ms(10);
	}
	for (int t = 0; kill(pw, 0) == 0 || kill(wp, 0) == 0 || kill(pp, 0) == 0; t += 50) {
		CHECK(t < 5000);   /* (the scripts exec sleep, which dies on SIGTERM; zombies reaped by init) */
		sleep_ms(50);
	}

	/* With autostart off, nothing is started. */
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -f %s/*; rm -f %s/pipewire-0", g_marks, g_dir);
	CHECK(system(cmd) == 0);
	setenv("REI_AUDIO_AUTOSTART", "0", 1);
	g_pid = 0;
	/* a second compositor needs its own socket name and directory state */
	snprintf(cmd, sizeof cmd, "rm -f %s/%s %s/%s.lock", g_dir, SOCKET, g_dir, SOCKET);
	CHECK(system(cmd) == 0);
	start_compositor(argv[1], config, false);
	sleep_ms(1500);
	CHECK(!exists("pipewire") && !exists("wireplumber") && !exists("pipewire-pulse"));
	CHECK(stop_compositor() == 0);

	puts("autostart: ok");
	return 0;
}
