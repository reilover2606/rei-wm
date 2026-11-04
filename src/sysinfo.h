#ifndef REI_SYSINFO_H
#define REI_SYSINFO_H

#include <stdbool.h>
#include <stdint.h>

struct rei_server;

/*
 * CPU and memory use, for the toolbar. CPU is the busy share of all cores since the previous
 * reading (from /proc/stat); memory is (MemTotal - MemAvailable) / MemTotal (from /proc/meminfo).
 */

struct rei_cpu_sample {
	uint64_t busy, total;   /* jiffies, summed over every core */
};

/* Reads the "cpu" line of <proc_dir>/stat. */
bool rei_sysinfo_read_cpu(const char *proc_dir, struct rei_cpu_sample *out);
/* The busy percentage (0..100) between two samples; -1 if no time passed. */
int rei_sysinfo_cpu_percent(const struct rei_cpu_sample *before, const struct rei_cpu_sample *after);
/* Memory in use, 0..100, from <proc_dir>/meminfo; -1 if unreadable. */
int rei_sysinfo_read_mem_percent(const char *proc_dir);

/* Polls every 2 seconds if [toolbar] sysinfo is on. */
void rei_sysinfo_start(struct rei_server *server);
void rei_sysinfo_stop(struct rei_server *server);
/* The last readings (0..100); false when the widget is off or /proc is unreadable. */
bool rei_sysinfo_state(const struct rei_server *server, int *cpu, int *mem);

#endif
