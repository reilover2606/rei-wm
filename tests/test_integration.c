/*
 * End-to-end test: starts the real rei-wm binary on the headless backend,
 * connects to it as a Wayland client, forces redraws through SIGUSR1, then
 * shuts it down with SIGTERM and inspects its log.
 *
 * This exercises the libevent <-> wl_display bridge, the event-thread to
 * render-thread snapshot path, the render-to-event frame path, multi-output
 * layout and clean shutdown, all in one process tree.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "check.h"

#define SOCKET "rei-test"

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	CHECK(f);
	char *buf = calloc(1, 1 << 20);
	CHECK(buf);
	size_t n = fread(buf, 1, (1 << 20) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	CHECK(f);
	fputs(text, f);
	fclose(f);
}

static void registry_global(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v)
{
	(void)d; (void)r; (void)n; (void)i; (void)v;
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { registry_global, registry_remove };

/* A round trip only completes if the compositor's event loop is alive and
 * servicing the socket. */
static void roundtrip(struct wl_display *d)
{
	struct wl_registry *reg = wl_display_get_registry(d);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	CHECK(wl_display_roundtrip(d) >= 0);
	wl_registry_destroy(reg);
}

/* Polls the compositor's log until `needle` shows up. */
static void wait_for_log(const char *logpath, const char *needle, int timeout_ms)
{
	for (int waited = 0; waited < timeout_ms; waited += 20) {
		char *log = slurp(logpath);
		bool found = strstr(log, needle) != NULL;
		free(log);
		if (found)
			return;
		sleep_ms(20);
	}
	fprintf(stderr, "timed out waiting for log line: %s\n", needle);
	exit(1);
}

static unsigned long long summary_field(const char *log, const char *output, const char *field)
{
	char needle[64];
	snprintf(needle, sizeof needle, "summary output=%s ", output);
	const char *line = strstr(log, needle);
	CHECK(line);
	snprintf(needle, sizeof needle, "%s=", field);
	const char *f = strstr(line, needle);
	CHECK(f);
	return strtoull(f + strlen(needle), NULL, 10);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	const char *binary = argv[1];

	char dir[] = "/tmp/rei-wm-test-XXXXXX";
	CHECK(mkdtemp(dir));
	chmod(dir, 0700);

	char cfg[256], logpath[256], sockpath[256];
	snprintf(cfg, sizeof cfg, "%s/config.toml", dir);
	snprintf(logpath, sizeof logpath, "%s/log.txt", dir);
	snprintf(sockpath, sizeof sockpath, "%s/%s", dir, SOCKET);
	write_file(cfg,
		"[[output]]\nname = \"A\"\nwidth = 800\nheight = 600\n"
		"[[output]]\nname = \"B\"\nwidth = 1024\nheight = 768\nrefresh = 75\n");

	pid_t pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		setenv("XDG_RUNTIME_DIR", dir, 1);
		unsetenv("WAYLAND_DISPLAY");
		int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		dup2(fd, STDERR_FILENO);
		execl(binary, binary, "-c", cfg, "-b", "headless", "-s", SOCKET, "-v", (char *)NULL);
		_exit(127);
	}

	/* Wait for the socket to appear (or the child to die trying). */
	struct stat st;
	int waited = 0;
	while (stat(sockpath, &st) != 0) {
		int status;
		CHECK(waitpid(pid, &status, WNOHANG) == 0);  /* still running */
		CHECK(waited < 5000);
		sleep_ms(20);
		waited += 20;
	}

	setenv("XDG_RUNTIME_DIR", dir, 1);
	struct wl_display *c1 = wl_display_connect(SOCKET);
	struct wl_display *c2 = wl_display_connect(SOCKET);
	CHECK(c1 && c2);
	roundtrip(c1);
	roundtrip(c2);

	/* Each render thread takes a moment to bring up its GL context. Redraws
	 * requested before the first frame legitimately merge into it, so wait
	 * for both outputs to have shown something before counting frames. */
	wait_for_log(logpath, "frame output=A seq=1", 5000);
	wait_for_log(logpath, "frame output=B seq=1", 5000);

	const int redraws = 5;
	for (int i = 0; i < redraws; i++) {
		CHECK(kill(pid, SIGUSR1) == 0);
		sleep_ms(100);
		roundtrip(c1);  /* compositor stays responsive under frame load */
	}

	/* A client disconnecting must not disturb the compositor. */
	wl_display_disconnect(c2);
	roundtrip(c1);
	wl_display_disconnect(c1);
	sleep_ms(50);

	CHECK(kill(pid, SIGTERM) == 0);
	int status = 0, polls = 0;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid)
			break;
		CHECK(r == 0);
		CHECK(polls++ < 500);  /* 5 s: shutdown must not hang */
		sleep_ms(10);
	}
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	char *log = slurp(logpath);
	CHECK(!strstr(log, "ERROR"));
	CHECK(strstr(log, "output added: name=A id=1 mode=800x600@60.000 pos=0,0"));
	CHECK(strstr(log, "output added: name=B id=2 mode=1024x768@75.000 pos=800,0"));
	CHECK(strstr(log, "listening on WAYLAND_DISPLAY=" SOCKET " (backend: headless)"));

	/* Scene serial: one for the outputs added at startup (changes made in the same
	 * pass share a snapshot) plus one per SIGUSR1. Both render
	 * threads must have presented the final snapshot. */
	unsigned long long final_serial = 1 + redraws;
	CHECK(summary_field(log, "A", "last_serial") == final_serial);
	CHECK(summary_field(log, "B", "last_serial") == final_serial);
	CHECK(summary_field(log, "A", "frames") >= (unsigned long long)redraws);
	CHECK(summary_field(log, "B", "frames") >= (unsigned long long)redraws);

	free(log);
	unlink(cfg);
	unlink(logpath);
	rmdir(dir);
	puts("test_integration: ok");
	return 0;
}
