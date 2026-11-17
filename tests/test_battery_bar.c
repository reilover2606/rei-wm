/*
 * The battery on the toolbar, end to end: with a fake /sys/class/power_supply of two batteries
 * the widget appears with the combined percentage, and it follows changes (here: going green
 * when the charger is plugged in). Without a battery nothing appears.
 */
#define SOCKET "rei-battbar-test"
#include "testlib.h"

#define OUT_W 800
#define OUT_H 600

static char g_supply[256];

static void put(const char *supply, const char *file, const char *text)
{
	char dir[300], path[400];
	snprintf(dir, sizeof dir, "%s/%s", g_supply, supply);
	mkdir(dir, 0755);
	snprintf(path, sizeof path, "%s/%s", dir, file);
	FILE *f = fopen(path, "w");
	CHECK(f);
	fprintf(f, "%s\n", text);
	fclose(f);
}

/* Counts pixels in the bar's right half that look like the battery's "level" colour. */
static int count_level_to(const struct ppm *img, bool green, int x1)
{
	int n = 0;
	for (int y = 2; y < 26; y++)
		for (int x = 500; x < x1; x++) {
			const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
			if (green ? (p[1] > 150 && p[0] < 160 && p[2] < 160) : (p[0] > 180 && p[1] > 180 && p[2] > 180))
				n++;
		}
	return n;
}

static int count_level(const struct ppm *img, bool green)
{
	return count_level_to(img, green, OUT_W - 90);
}

static void reload_config(const char *text)
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
	settle_all();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-battbar-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_supply, sizeof g_supply, "%s/power_supply", g_dir);
	mkdir(g_supply, 0755);
	put("AC", "type", "Mains");
	put("BAT0", "type", "Battery");
	put("BAT0", "energy_now", "30000000");
	put("BAT0", "energy_full", "60000000");
	put("BAT0", "status", "Discharging");
	put("BAT1", "type", "Battery");
	put("BAT1", "energy_now", "20000000");
	put("BAT1", "energy_full", "20000000");
	put("BAT1", "status", "Discharging");
	setenv("REI_POWER_SUPPLY_DIR", g_supply, 1);
	setenv("REI_BATTERY_POLL_SECONDS", "1", 1);
	setenv("TZ", "Asia/Kolkata", 1);

	char config[300];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\nsysinfo = false\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	CHECK(log_has("battery: 2 found, 63%"));
	sleep_ms(300);
	struct ppm img = screenshot();
	int level = count_level(&img, false);
	CHECK(level > 40);                         /* a filled cell and the percentage text */
	CHECK(count_level(&img, true) == 0);       /* not charging: no green */
	free(img.rgb);

	/* Plug the charger in: the poll notices and the icon turns green. */
	put("BAT1", "status", "Charging");
	for (int t = 0;; t += 100) {
		sleep_ms(100);
		img = screenshot();
		int green = count_level(&img, true);
		free(img.rgb);
		if (green > 20)
			break;
		CHECK(t < 6000);
	}

	/* Hovering the battery (it is among the last items on the bar) shows its exact percentage after half a second. */
	for (int x = OUT_W - 20; x > 500 && !log_has("bar tooltip: Battery"); x -= 6) {
		char buf[64];
		snprintf(buf, sizeof buf, "motion_abs %d 14\n", x);
		inject_lines(buf);
		settle_all();
		sleep_ms(560);
		settle_all();
	}
	CHECK(log_has("bar tooltip: Battery: 63%"));
	if (getenv("REI_SHOT_DIR")) {
		sleep_ms(400);
		struct ppm im = screenshot();
		free(im.rgb);
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/battery-tip.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}
	inject_lines("motion_abs 400 400\n");
	settle_all();

	/* The clock's tooltip: the date and time to the second, and the zone. */
	for (int x = OUT_W - 20; x > 300 && !log_has("+05:30"); x -= 12) {
		char buf[64];
		snprintf(buf, sizeof buf, "motion_abs %d 14\n", x);
		inject_lines(buf);
		settle_all();
		sleep_ms(560);
		settle_all();
	}
	CHECK(log_has("+05:30"));
	if (getenv("REI_SHOT_DIR")) {
		sleep_ms(400);
		struct ppm im = screenshot();
		free(im.rgb);
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/clock-tip.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}
	CHECK(log_has("Asia/Kolkata") || log_has("IST"));
	inject_lines("motion_abs 400 400\n");
	settle_all();

	/* Icon only: the percentage text goes (the bar picture changes), and the battery is still there. */
	struct ppm with = screenshot();
	if (getenv("REI_SHOT_DIR")) {
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/battery-full.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}
	reload_config("[toolbar]\nenabled = true\nsysinfo = false\nbattery_percent = false\n[[output]]\nname = \"OUT\"\n"
		      "width = 800\nheight = 600\n");
	struct ppm icon = screenshot();
	if (getenv("REI_SHOT_DIR")) {
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/battery-icon.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}
	int w_all = count_level_to(&with, false, OUT_W - 2), i_all = count_level_to(&icon, false, OUT_W - 2);
	CHECK(i_all > 5 && i_all < w_all);        /* still there, but without the digits */
	free(with.rgb);
	free(icon.rgb);

	/* Disabled in the config and reloaded: the battery really goes (it used to stay). */
	reload_config("[toolbar]\nenabled = true\nsysinfo = false\nbattery = false\n[[output]]\nname = \"OUT\"\n"
		      "width = 800\nheight = 600\n");
	img = screenshot();
	CHECK(log_has("battery: stopped"));
	free(img.rgb);

	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int t = 0;; t += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && t < 5000);
		sleep_ms(10);
	}
	puts("battery bar: ok");
	return 0;
}
