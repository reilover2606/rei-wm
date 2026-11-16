/*
 * The brightness keys, against a fake backlight directory (REI_BACKLIGHT_DIR): XF86MonBrightnessUp/Down change the
 * file by [brightness] step of the range (never below 1% or above the maximum), quick presses add up, the result is
 * one message that updates in place; the finest backlight is chosen, or the named one; a command of your own can
 * replace the write; and when the file cannot be written logind is asked through dbus-send.
 */
#define SOCKET "rei-brightness-test"
#include "testlib.h"

#include <signal.h>

#include "brightness.h"

static char g_bl[400];

static void make_device(const char *name, const char *type, int cur, int max)
{
	char dir[600], path[700];
	snprintf(dir, sizeof dir, "%s/%s", g_bl, name);
	CHECK(mkdir(dir, 0700) == 0);
	char text[32];
	snprintf(path, sizeof path, "%s/brightness", dir);
	snprintf(text, sizeof text, "%d\n", cur);
	write_text(path, text);
	snprintf(path, sizeof path, "%s/max_brightness", dir);
	snprintf(text, sizeof text, "%d\n", max);
	write_text(path, text);
	snprintf(path, sizeof path, "%s/type", dir);
	snprintf(text, sizeof text, "%s\n", type);
	write_text(path, text);
}

static int value_of(const char *name)
{
	char path[700];
	snprintf(path, sizeof path, "%s/%s/brightness", g_bl, name);
	char *t = slurp(path);
	CHECK(t);
	int v = atoi(t);
	free(t);
	return v;
}

static void key(int code)
{
	inject("key %d 1", code);
	inject("key %d 0", code);
	settle_all();
	sleep_ms(30);
}

static void wait_value(const char *name, int want)
{
	for (int t = 0; value_of(name) != want; t += 20) {
		if (t > 3000) {
			fprintf(stderr, "%s is %d, expected %d\n", name, value_of(name), want);
			exit(1);
		}
		sleep_ms(20);
	}
}

static void reload(const char *extra)
{
	char text[1200], path[500], tmp[520];
	snprintf(text, sizeof text, "[toolbar]\nenabled = false\n%s[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", extra, OUT_W, OUT_H);
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
	settle_all();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-brightness-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_bl, sizeof g_bl, "%s/backlight", g_dir);
	CHECK(mkdir(g_bl, 0700) == 0);

	/* ---- Choosing the device (pure). ---- */
	make_device("acpi_video0", "firmware", 10, 15);
	make_device("intel_backlight", "raw", 480, 960);
	make_device("small_raw", "raw", 5, 100);
	struct rei_backlight b;
	CHECK(rei_backlight_find(g_bl, "", &b) && !strcmp(b.name, "intel_backlight") && b.cur == 480 && b.max == 960);
	CHECK(rei_backlight_find(g_bl, "acpi_video0", &b) && b.max == 15);
	CHECK(!rei_backlight_find(g_bl, "nope", &b));
	CHECK(!rei_backlight_find("/nonexistent-backlights", "", &b));

	setenv("REI_BACKLIGHT_DIR", g_bl, 1);
	start_compositor(argv[1], "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n", true);
	struct client c;
	client_connect(&c);
	g_clients[0] = &c;

	/* ---- The keys: 5% of 960 is 48. ---- */
	key(225);   /* KEY_BRIGHTNESSUP */
	wait_value("intel_backlight", 528);
	CHECK(log_has("message: Brightness: 55%"));
	CHECK(value_of("acpi_video0") == 10 && value_of("small_raw") == 5);   /* the others are left alone */
	key(224);   /* KEY_BRIGHTNESSDOWN */
	wait_value("intel_backlight", 480);
	/* A burst: each press starts from what the last asked for. */
	for (int i = 0; i < 4; i++) {
		inject("key 224 1");
		inject("key 224 0");
	}
	settle_all();
	wait_value("intel_backlight", 288);
	/* One note, updated in place, not one per press (a new message is logged, an update is not). */
	CHECK(log_count("message: Brightness:") == 1);
	/* Down to the floor: 1% of the range, never black. */
	for (int i = 0; i < 40; i++) {
		inject("key 224 1");
		inject("key 224 0");
	}
	settle_all();
	sleep_ms(300);
	CHECK(value_of("intel_backlight") == 9);   /* 960 / 100 */
	/* ... and up to the top, no further. */
	for (int i = 0; i < 40; i++) {
		inject("key 225 1");
		inject("key 225 0");
	}
	settle_all();
	sleep_ms(300);
	CHECK(value_of("intel_backlight") == 960);

	/* ---- The step, the device and a custom command. ---- */
	reload("[brightness]\nstep = 10\ndevice = \"small_raw\"\n");
	key(225);
	wait_value("small_raw", 7);           /* 10% of 100 is 10, but at the dim end the steps are a quarter: 2 */
	CHECK(value_of("intel_backlight") == 960);
	char out[600], cmd[900];
	snprintf(out, sizeof out, "%s/custom.txt", g_dir);
	snprintf(cmd, sizeof cmd, "[brightness]\ncommand = \"echo {device} {value} {percent} > %s\"\n", out);
	reload(cmd);
	sleep_ms(1600);   /* (earlier presses no longer count) */
	key(224);
	char *text = NULL;
	for (int t = 0; !text || !text[0]; t += 30) {
		CHECK(t < 4000);
		sleep_ms(30);
		free(text);
		text = slurp(out);
	}
	fprintf(stderr, "custom command wrote: %s", text);
	CHECK(strstr(text, "intel_backlight 912 95") != NULL);   /* 960 - 48 */
	free(text);

	/* ---- Not writable: logind is asked. ---- */
	char fake[600], script[700], args[600];
	snprintf(fake, sizeof fake, "%s/fakebin", g_dir);
	CHECK(mkdir(fake, 0700) == 0);
	snprintf(script, sizeof script, "%s/dbus-send", fake);
	snprintf(args, sizeof args, "%s/dbus-args.txt", g_dir);
	char body[900];
	snprintf(body, sizeof body, "#!/bin/sh\necho \"$@\" > %s\n", args);
	write_text(script, body);
	CHECK(chmod(script, 0700) == 0);
	/* (the compositor was started before PATH changed: restart it with the hook) */
	CHECK(stop_compositor() == 0);
	char cmd0[1500];
	snprintf(cmd0, sizeof cmd0, "rm -f %s/input.fifo %s/%s %s/%s.lock", g_dir, g_dir, SOCKET, g_dir, SOCKET);
	CHECK(system(cmd0) == 0);
	g_pid = 0;
	close(g_fifo_fd);
	g_fifo_fd = -1;
	g_nwins = 0;
	char path[1200];
	snprintf(path, sizeof path, "%s:%s", fake, getenv("PATH"));
	setenv("PATH", path, 1);
	setenv("REI_BACKLIGHT_NOWRITE", "1", 1);
	start_compositor(argv[1], "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n", true);
	struct client c2;
	client_connect(&c2);
	g_clients[0] = &c2;
	key(224);
	char *dargs = NULL;
	for (int t = 0; !dargs || !dargs[0]; t += 30) {
		CHECK(t < 4000);
		sleep_ms(30);
		free(dargs);
		dargs = slurp(args);
	}
	fprintf(stderr, "dbus-send got: %s", dargs);
	CHECK(strstr(dargs, "--system") && strstr(dargs, "org.freedesktop.login1.Session.SetBrightness"));
	CHECK(strstr(dargs, "string:backlight") && strstr(dargs, "string:intel_backlight") && strstr(dargs, "uint32:912"));
	free(dargs);
	CHECK(value_of("intel_backlight") == 960);   /* (the file was not touched: logind does that) */

	CHECK(stop_compositor() == 0);
	puts("brightness: ok");
	return 0;
}
