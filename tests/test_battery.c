/* Battery detection and aggregation, against a fake /sys/class/power_supply. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "battery.h"
#include "check.h"

static char g_root[] = "/tmp/rei-battery-test-XXXXXX";

static void put(const char *supply, const char *file, const char *text)
{
	char dir[300], path[400];
	snprintf(dir, sizeof dir, "%s/%s", g_root, supply);
	mkdir(dir, 0755);
	snprintf(path, sizeof path, "%s/%s", dir, file);
	FILE *f = fopen(path, "w");
	CHECK(f);
	fprintf(f, "%s\n", text);
	fclose(f);
}

static void clear(void)
{
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf %s/*", g_root);
	CHECK(system(cmd) == 0);
}

int main(void)
{
	CHECK(mkdtemp(g_root));
	struct rei_battery_info b;

	/* A desktop: only a mains supply, no battery. */
	put("AC", "type", "Mains");
	put("AC", "online", "1");
	CHECK(!rei_battery_read(g_root, &b) && !b.present);
	CHECK(!rei_battery_read("/nonexistent/power_supply", &b) && !b.present);

	/* One laptop battery in energy units: 30 / 60 Wh = 50%. */
	put("BAT0", "type", "Battery");
	put("BAT0", "energy_now", "30000000");
	put("BAT0", "energy_full", "60000000");
	put("BAT0", "status", "Discharging");
	CHECK(rei_battery_read(g_root, &b) && b.present);
	CHECK(b.count == 1 && b.percent == 50 && b.discharging && !b.charging);

	/* Two batteries are added up by capacity, not averaged. */
	put("BAT1", "type", "Battery");
	put("BAT1", "energy_now", "20000000");
	put("BAT1", "energy_full", "20000000");
	put("BAT1", "status", "Charging");
	CHECK(rei_battery_read(g_root, &b));
	CHECK(b.count == 2);
	CHECK(b.percent == 63);                   /* (30 + 20) / (60 + 20) = 62.5%, not (50 + 100) / 2 = 75% */
	CHECK(b.charging && b.discharging);

	/* A battery measured in charge (µAh) is converted with its voltage: 2 Ah at 10 V = 20 Wh. */
	clear();
	put("BAT0", "type", "Battery");
	put("BAT0", "energy_now", "30000000");
	put("BAT0", "energy_full", "60000000");
	put("BAT1", "type", "Battery");
	put("BAT1", "charge_now", "1000000");
	put("BAT1", "charge_full", "2000000");
	put("BAT1", "voltage_min_design", "10000000");
	CHECK(rei_battery_read(g_root, &b) && b.count == 2);
	CHECK(b.percent == 50);                      /* (30 + 10) / (60 + 20) Wh */

	/* Only a percentage known: it counts as a battery of its own weight. */
	clear();
	put("BAT0", "type", "Battery");
	put("BAT0", "capacity", "42");
	CHECK(rei_battery_read(g_root, &b) && b.count == 1 && b.percent == 42);

	/* Things that are not the machine's battery are ignored. */
	clear();
	put("hid-mouse-battery", "type", "Battery");
	put("hid-mouse-battery", "scope", "Device");
	put("hid-mouse-battery", "capacity", "5");
	put("BAT0", "type", "Battery");
	put("BAT0", "present", "0");                    /* an empty bay */
	put("BAT0", "energy_now", "1");
	put("BAT0", "energy_full", "100");
	put("ucsi-source-psy", "type", "USB");
	CHECK(!rei_battery_read(g_root, &b) && !b.present);

	/* Charging, full, and "not charging" (plugged in at a charge limit). */
	clear();
	put("BAT0", "type", "Battery");
	put("BAT0", "capacity", "80");
	put("BAT0", "status", "Not charging");
	CHECK(rei_battery_read(g_root, &b) && !b.charging && !b.discharging);
	put("BAT0", "status", "Charging");
	CHECK(rei_battery_read(g_root, &b) && b.charging && !b.discharging);

	/* Out-of-range and unreadable values do not produce nonsense. */
	clear();
	put("BAT0", "type", "Battery");
	put("BAT0", "energy_now", "90000000");          /* more than the capacity says */
	put("BAT0", "energy_full", "60000000");
	CHECK(rei_battery_read(g_root, &b) && b.percent == 100);
	put("BAT0", "energy_full", "0");
	CHECK(!rei_battery_read(g_root, &b));

	clear();
	rmdir(g_root);
	puts("battery: ok");
	return 0;
}
