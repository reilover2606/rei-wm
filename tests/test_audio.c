/*
 * End-to-end test of the volume control against a real PipeWire daemon: the media
 * keys change the volume and mute state of the default sink, and the toolbar
 * follows. Starts its own PipeWire (no WirePlumber is needed: with no "default"
 * metadata, rei-wm falls back to the highest-priority sink) with one null sink.
 * Skipped (exit 77) when PipeWire's tools are not installed.
 */
#define SOCKET "rei-audio-test"
#include "testlib.h"

#include <math.h>

#define KEY_MUTE 113
#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP 115

static pid_t g_pw;

static void kill_pipewire(void)
{
	if (g_pw > 0)
		kill(g_pw, SIGKILL);
}

static bool have_tool(const char *name)
{
	char cmd[128];
	snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
	return system(cmd) == 0;
}

/* The sink's channel volume (linear) and mute flag, read with pw-dump. */
static bool read_sink(double *linear, bool *muted)
{
	FILE *p = popen("pw-dump 2>/dev/null", "r");
	CHECK(p);
	char *buf = NULL;
	size_t cap = 0, len = 0;
	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) {
		if (len + n + 1 > cap) {
			cap = (len + n + 1) * 2;
			buf = realloc(buf, cap);
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	pclose(p);
	if (!buf)
		return false;
	buf[len] = '\0';

	bool ok = false;
	const char *sink = strstr(buf, "\"node.name\": \"test-sink\"");
	/* The key also appears as a name in PropInfo; the value is the one followed by ": [". */
	const char *cv = sink;
	const char *b = NULL;
	while (cv && (cv = strstr(cv, "\"channelVolumes\"")) != NULL) {
		const char *q = cv + strlen("\"channelVolumes\"");
		while (*q == ' ' || *q == '\n' || *q == '\t')
			q++;
		if (*q == ':') {
			q++;
			while (*q == ' ' || *q == '\n' || *q == '\t')
				q++;
			if (*q == '[') {
				b = q;
				break;
			}
		}
		cv += 5;
	}
	const char *mute = sink;
	const char *mv = NULL;
	while (mute && (mute = strstr(mute, "\"mute\"")) != NULL) {
		const char *q = mute + strlen("\"mute\"");
		while (*q == ' ' || *q == '\n' || *q == '\t')
			q++;
		if (*q == ':') {
			q++;
			while (*q == ' ')
				q++;
			if (!strncmp(q, "true", 4) || !strncmp(q, "false", 5)) {
				mv = q;
				break;
			}
		}
		mute += 4;
	}
	if (b && mv) {
		*linear = strtod(b + 1, NULL);
		*muted = !strncmp(mv, "true", 4);
		ok = true;
	}
	free(buf);
	return ok;
}

static void expect_volume(int percent, bool muted, const char *what)
{
	for (int t = 0;; t += 50) {
		double lin;
		bool m;
		if (read_sink(&lin, &m)) {
			int got = (int)lround(cbrt(lin) * 100.0);
			if (abs(got - percent) <= 1 && m == muted)
				return;
			if (t >= 4000) {
				fprintf(stderr, "%s: volume %d%% muted=%d, expected %d%% muted=%d\n", what, got, m,
					percent, muted);
				exit(1);
			}
		} else if (t >= 4000) {
			fprintf(stderr, "%s: cannot read the sink\n", what);
			exit(1);
		}
		sleep_ms(50);
	}
}

static void key(int code)
{
	char buf[64];
	snprintf(buf, sizeof buf, "key %d 1\nkey %d 0\n", code, code);
	inject_lines(buf);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!have_tool("pipewire") || !have_tool("pw-cli") || !have_tool("pw-dump")) {
		fprintf(stderr, "PipeWire tools are not installed: skipping\n");
		return 77;
	}
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-audio-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	setenv("XDG_RUNTIME_DIR", g_dir, 1);
	setenv("PIPEWIRE_RUNTIME_DIR", g_dir, 1);

	g_pw = fork();
	CHECK(g_pw >= 0);
	if (g_pw == 0) {
		int fd = open("/dev/null", O_WRONLY);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		execlp("pipewire", "pipewire", (char *)NULL);
		_exit(127);
	}
	atexit(kill_pipewire);
	char sock[300];
	snprintf(sock, sizeof sock, "%s/pipewire-0", g_dir);
	struct stat st;
	for (int t = 0; stat(sock, &st) != 0; t += 50) {
		CHECK(t < 10000);
		sleep_ms(50);
	}
	CHECK(system("pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=test-sink "
		     "media.class=Audio/Sink object.linger=true audio.position=[FL FR] }' >/dev/null 2>&1") == 0);
	{
		double lin;
		bool m;
		for (int t = 0; !read_sink(&lin, &m); t += 50) {
			CHECK(t < 8000);
			sleep_ms(50);
		}
	}

	char config[300];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\n[audio]\nvolume_step = 5\nmax_volume = 100\n"
		 "[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n");
	start_compositor(argv[1], config, true);
	for (int t = 0; !log_has("audio: sink 'test-sink'") || !log_has("volume 100%"); t += 50) {
		CHECK(t < 8000);
		sleep_ms(50);
	}

	key(KEY_VOLUMEDOWN);
	key(KEY_VOLUMEDOWN);
	expect_volume(90, false, "two presses of volume down lower it by 2 steps");
	key(KEY_MUTE);
	expect_volume(90, true, "mute");
	key(KEY_MUTE);
	expect_volume(90, false, "mute again un-mutes");
	key(KEY_MUTE);
	key(KEY_VOLUMEUP);
	expect_volume(95, false, "raising the volume un-mutes");
	for (int i = 0; i < 4; i++)
		key(KEY_VOLUMEUP);
	expect_volume(100, false, "the volume stops at max_volume");

	/* The toolbar shows it: with the volume text present the bar differs from one without. */
	struct ppm with = screenshot();
	key(KEY_MUTE);
	expect_volume(100, true, "mute for the picture");
	sleep_ms(300);
	struct ppm muted = screenshot();
	int differing = 0;
	for (int x = 600; x < 800; x++)
		for (int y = 0; y < 28; y++)
			differing += !near_px(&muted, x, y, with.rgb[(y * with.w + x) * 3],
					      with.rgb[(y * with.w + x) * 3 + 1],
					      with.rgb[(y * with.w + x) * 3 + 2], 0);
	CHECK(differing > 20);  /* "100%" became "muted", in a dimmer colour */
	free(with.rgb);
	free(muted.rgb);

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
	puts("audio: ok");
	return 0;
}
