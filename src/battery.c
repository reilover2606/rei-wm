#include "battery.h"

#include <dirent.h>
#include <errno.h>
#include <event2/event.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define POLL_SECONDS_DEFAULT 30

/* REI_BATTERY_POLL_SECONDS shortens the interval, for tests. */
static int poll_seconds(void)
{
	const char *e = getenv("REI_BATTERY_POLL_SECONDS");
	return e && atoi(e) > 0 ? atoi(e) : POLL_SECONDS_DEFAULT;
}

/* Reads a small sysfs file into `out`, without the trailing newline. */
static bool read_attr(const char *dir, const char *name, char *out, size_t n)
{
	char path[600];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(out, (int)n, f) != NULL;
	fclose(f);
	if (ok)
		out[strcspn(out, "\r\n")] = '\0';
	return ok;
}

static bool read_num(const char *dir, const char *name, double *out)
{
	char buf[64];
	if (!read_attr(dir, name, buf, sizeof buf) || !buf[0])
		return false;
	char *end;
	double v = strtod(buf, &end);
	if (end == buf)
		return false;
	*out = v;
	return true;
}

/*
 * One battery's charge and capacity in the same (arbitrary but consistent) unit, so that
 * several can be added. Energy (µWh) is preferred; charge (µAh) is converted to energy with
 * the battery's voltage; as a last resort only a percentage is known, weighed as 100 units.
 */
static bool battery_amounts(const char *dir, double *now, double *full)
{
	double e_now, e_full;
	if (read_num(dir, "energy_now", &e_now) && read_num(dir, "energy_full", &e_full) && e_full > 0) {
		*now = e_now;
		*full = e_full;
		return true;
	}
	double c_now, c_full;
	if (read_num(dir, "charge_now", &c_now) && read_num(dir, "charge_full", &c_full) && c_full > 0) {
		double volts_uv;  /* µV */
		if (!read_num(dir, "voltage_min_design", &volts_uv) && !read_num(dir, "voltage_now", &volts_uv))
			volts_uv = 1e6;  /* Unknown: treat as 1 V; only the ratio between batteries is off. */
		*now = c_now * volts_uv / 1e6;
		*full = c_full * volts_uv / 1e6;
		return true;
	}
	double pct;
	if (read_num(dir, "capacity", &pct)) {
		*now = pct;
		*full = 100;
		return true;
	}
	return false;
}

bool rei_battery_read(const char *dir, struct rei_battery_info *info)
{
	memset(info, 0, sizeof *info);
	DIR *d = opendir(dir);
	if (!d)
		return false;

	double total_now = 0, total_full = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[600], buf[64];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);

		if (!read_attr(path, "type", buf, sizeof buf) || strcmp(buf, "Battery") != 0)
			continue;
		/* "Device" scope is a peripheral's battery (mouse, headset, phone), not the system's. */
		if (read_attr(path, "scope", buf, sizeof buf) && strcmp(buf, "Device") == 0)
			continue;
		double present;
		if (read_num(path, "present", &present) && present == 0)
			continue;  /* An empty bay. */

		double now, full;
		if (!battery_amounts(path, &now, &full))
			continue;
		total_now += now > full ? full : now;
		total_full += full;
		info->count++;

		if (read_attr(path, "status", buf, sizeof buf)) {
			if (strcmp(buf, "Charging") == 0)
				info->charging = true;
			else if (strcmp(buf, "Discharging") == 0)
				info->discharging = true;
		}
	}
	closedir(d);

	if (info->count == 0 || total_full <= 0)
		return false;
	double pct = total_now / total_full * 100.0;
	info->percent = (int)(pct + 0.5);
	if (info->percent < 0)
		info->percent = 0;
	if (info->percent > 100)
		info->percent = 100;
	info->present = true;
	return true;
}

/* ------------------------------------------------------------------------ */

/*
 * The batteries are read by a thread of their own. Reading sysfs power-supply files goes through
 * the firmware (ACPI / the embedded controller) and on some laptops a read can block for seconds;
 * done on the event thread that froze the whole compositor (the watchdog caught it inside
 * rei_battery_read). The thread tells the event loop through a pipe when what it shows changed.
 */
struct rei_battery {
	struct rei_server *server;
	char dir[512];
	struct event *wake;           /* the event loop's end of the pipe */
	int pipe_fd[2];
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool stop;
	struct rei_battery_info info;  /* guarded by `lock` */
};

static void on_wake(evutil_socket_t fd, short what, void *arg)
{
	(void)what;
	struct rei_battery *b = arg;
	char buf[64];
	while (read(fd, buf, sizeof buf) > 0)
		;
	rei_server_schedule_damage(b->server);  /* The toolbar redraws if what it shows changed. */
}

static void *battery_thread(void *arg)
{
	struct rei_battery *b = arg;
	pthread_mutex_lock(&b->lock);
	while (!b->stop) {
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += poll_seconds();
		while (!b->stop && pthread_cond_timedwait(&b->cond, &b->lock, &until) != ETIMEDOUT)
			;
		if (b->stop)
			break;
		pthread_mutex_unlock(&b->lock);
		struct rei_battery_info now;
		rei_battery_read(b->dir, &now);   /* may block; nothing is held */
		pthread_mutex_lock(&b->lock);
		if (memcmp(&now, &b->info, sizeof now) != 0) {
			b->info = now;
			ssize_t r = write(b->pipe_fd[1], "x", 1);
			(void)r;
		}
	}
	pthread_mutex_unlock(&b->lock);
	return NULL;
}

void rei_battery_start(struct rei_server *server)
{
	if (!server->config->toolbar_battery)
		return;
	const char *dir = getenv("REI_POWER_SUPPLY_DIR");  /* A test hook. */
	struct rei_battery_info probe;
	if (!rei_battery_read(dir && dir[0] ? dir : "/sys/class/power_supply", &probe)) {
		rei_info("battery: none found; the toolbar shows no battery");
		return;
	}
	struct rei_battery *b = rei_xcalloc(1, sizeof *b);
	b->server = server;
	snprintf(b->dir, sizeof b->dir, "%s", dir && dir[0] ? dir : "/sys/class/power_supply");
	b->info = probe;
	if (pipe2(b->pipe_fd, O_CLOEXEC | O_NONBLOCK) != 0) {
		free(b);
		return;
	}
	pthread_mutex_init(&b->lock, NULL);
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_REALTIME);
	pthread_cond_init(&b->cond, &attr);
	pthread_condattr_destroy(&attr);
	b->wake = event_new(server->base, b->pipe_fd[0], EV_READ | EV_PERSIST, on_wake, b);
	event_add(b->wake, NULL);
	if (pthread_create(&b->thread, NULL, battery_thread, b) != 0) {
		event_free(b->wake);
		close(b->pipe_fd[0]);
		close(b->pipe_fd[1]);
		free(b);
		return;
	}
	server->battery = b;
	rei_info("battery: %d found, %d%%%s", probe.count, probe.percent, probe.charging ? " (charging)" : "");
}

void rei_battery_stop(struct rei_server *server)
{
	struct rei_battery *b = server->battery;
	if (!b)
		return;
	server->battery = NULL;
	pthread_mutex_lock(&b->lock);
	b->stop = true;
	pthread_cond_signal(&b->cond);
	pthread_mutex_unlock(&b->lock);
	pthread_join(b->thread, NULL);   /* (a read stuck in the kernel would delay this; it cannot be helped) */
	event_free(b->wake);
	close(b->pipe_fd[0]);
	close(b->pipe_fd[1]);
	pthread_cond_destroy(&b->cond);
	pthread_mutex_destroy(&b->lock);
	free(b);
}

struct rei_battery_info rei_battery_state(const struct rei_server *server)
{
	struct rei_battery_info none = { 0 };
	struct rei_battery *b = server->battery;
	if (!b)
		return none;
	pthread_mutex_lock(&b->lock);
	struct rei_battery_info copy = b->info;
	pthread_mutex_unlock(&b->lock);
	return copy;
}
