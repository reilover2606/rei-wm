#include "sysinfo.h"

#include <event2/event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "util/log.h"
#include "util/util.h"

bool rei_sysinfo_read_cpu(const char *dir, struct rei_cpu_sample *out)
{
	char path[600];
	snprintf(path, sizeof path, "%s/stat", dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char line[512];
	bool ok = false;
	if (fgets(line, sizeof line, f) && strncmp(line, "cpu ", 4) == 0) {
		unsigned long long v[8] = { 0 };   /* user nice system idle iowait irq softirq steal */
		int n = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
			       &v[5], &v[6], &v[7]);
		if (n >= 4) {
			uint64_t total = 0;
			for (int i = 0; i < 8; i++)
				total += v[i];
			out->total = total;
			out->busy = total - v[3] - v[4];   /* not idle, not waiting for I/O */
			ok = true;
		}
	}
	fclose(f);
	return ok;
}

int rei_sysinfo_cpu_percent(const struct rei_cpu_sample *a, const struct rei_cpu_sample *b)
{
	if (b->total <= a->total)
		return -1;
	double p = 100.0 * (double)(b->busy - a->busy) / (double)(b->total - a->total);
	return p < 0 ? 0 : p > 100 ? 100 : (int)(p + 0.5);
}

bool rei_sysinfo_read_mem(const char *dir, long long *used, long long *total_out)
{
	char path[600];
	snprintf(path, sizeof path, "%s/meminfo", dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char line[256];
	long long total = -1, avail = -1;
	while (fgets(line, sizeof line, f)) {
		sscanf(line, "MemTotal: %lld", &total);
		sscanf(line, "MemAvailable: %lld", &avail);
	}
	fclose(f);
	if (total <= 0 || avail < 0 || avail > total)
		return false;
	*used = total - avail;
	*total_out = total;
	return true;
}

void rei_sysinfo_format_mem(long long used_kb, long long total_kb, char *out, unsigned long n)
{
	snprintf(out, n, "%.1f/%.1fGB", (double)used_kb / 1048576.0, (double)total_kb / 1048576.0);
}

/* ------------------------------------------------------------------------ */

struct rei_sysinfo {
	struct rei_server *server;
	char dir[512];
	struct event *timer;
	struct rei_cpu_sample last;
	int cpu;
	long long mem_used, mem_total;
};

static int poll_ms(void)
{
	const char *e = getenv("REI_SYSINFO_POLL_MS");   /* A test hook. */
	return e && atoi(e) > 0 ? atoi(e) : 2000;
}

static void poll(struct rei_sysinfo *s)
{
	struct rei_cpu_sample now;
	int cpu = s->cpu;
	long long used = s->mem_used, total = s->mem_total;
	rei_sysinfo_read_mem(s->dir, &used, &total);   /* keeps the old values if unreadable */
	if (rei_sysinfo_read_cpu(s->dir, &now)) {
		int p = rei_sysinfo_cpu_percent(&s->last, &now);
		if (p >= 0)
			cpu = p;
		s->last = now;
	}
	if (cpu != s->cpu || used != s->mem_used || total != s->mem_total) {
		s->cpu = cpu;
		s->mem_used = used;
		s->mem_total = total;
		rei_server_schedule_damage(s->server);
	}
}

static void on_timer(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_sysinfo *s = arg;
	poll(s);
	int ms = poll_ms();
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	evtimer_add(s->timer, &tv);
}

void rei_sysinfo_start(struct rei_server *server)
{
	if (!server->config->toolbar_sysinfo)
		return;
	const char *dir = getenv("REI_PROC_DIR");   /* A test hook. */
	struct rei_sysinfo *s = rei_xcalloc(1, sizeof *s);
	s->server = server;
	snprintf(s->dir, sizeof s->dir, "%s", dir && dir[0] ? dir : "/proc");
	struct rei_cpu_sample first;
	if (!rei_sysinfo_read_cpu(s->dir, &first) || !rei_sysinfo_read_mem(s->dir, &s->mem_used, &s->mem_total)) {
		rei_info("sysinfo: %s is not readable; the toolbar shows no CPU/RAM", s->dir);
		free(s);
		return;
	}
	s->last = first;
	s->cpu = 0;
	s->timer = evtimer_new(server->base, on_timer, s);
	int ms = poll_ms();
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	evtimer_add(s->timer, &tv);
	server->sysinfo = s;
}

void rei_sysinfo_stop(struct rei_server *server)
{
	struct rei_sysinfo *s = server->sysinfo;
	if (!s)
		return;
	server->sysinfo = NULL;
	if (s->timer)
		event_free(s->timer);
	free(s);
}

bool rei_sysinfo_state(const struct rei_server *server, int *cpu, long long *mem_used, long long *mem_total)
{
	if (!server->sysinfo)
		return false;
	*cpu = server->sysinfo->cpu;
	*mem_used = server->sysinfo->mem_used;
	*mem_total = server->sysinfo->mem_total;
	return true;
}
