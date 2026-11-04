/* CPU / memory readings, against a fake /proc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "sysinfo.h"

static char g_root[] = "/tmp/rei-sysinfo-test-XXXXXX";

static void put(const char *file, const char *text)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", g_root, file);
	FILE *f = fopen(path, "w");
	CHECK(f);
	fputs(text, f);
	fclose(f);
}

int main(void)
{
	CHECK(mkdtemp(g_root));
	struct rei_cpu_sample a, b;

	CHECK(!rei_sysinfo_read_cpu(g_root, &a));   /* nothing there */
	CHECK(rei_sysinfo_read_mem_percent(g_root) < 0);

	/* user nice system idle iowait irq softirq steal */
	put("stat", "cpu  100 0 100 700 100 0 0 0\ncpu0 50 0 50 350 50 0 0 0\nintr 1\n");
	CHECK(rei_sysinfo_read_cpu(g_root, &a));
	CHECK(a.total == 1000 && a.busy == 200);   /* idle and iowait are not busy */

	put("stat", "cpu  200 0 200 750 150 0 0 0\n");
	CHECK(rei_sysinfo_read_cpu(g_root, &b));
	CHECK(rei_sysinfo_cpu_percent(&a, &b) == 67);   /* 200 busy of 300 more jiffies */
	CHECK(rei_sysinfo_cpu_percent(&b, &b) == -1);   /* no time passed */

	put("meminfo", "MemTotal:       8000000 kB\nMemFree: 100 kB\nMemAvailable:   6000000 kB\n");
	CHECK(rei_sysinfo_read_mem_percent(g_root) == 25);
	put("meminfo", "MemTotal: 1000 kB\n");   /* no MemAvailable: unusable */
	CHECK(rei_sysinfo_read_mem_percent(g_root) < 0);

	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
	CHECK(system(cmd) == 0);
	printf("sysinfo: ok\n");
	return 0;
}
