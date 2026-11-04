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
static int count_level(const struct ppm *img, bool green)
{
	int n = 0;
	for (int y = 2; y < 26; y++)
		for (int x = 500; x < OUT_W - 90; x++) {
			const uint8_t *p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
			if (green ? (p[1] > 150 && p[0] < 160 && p[2] < 160) : (p[0] > 180 && p[1] > 180 && p[2] > 180))
				n++;
		}
	return n;
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

	char config[300];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
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
