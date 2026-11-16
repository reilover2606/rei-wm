#include "watchdog.h"

#include <dirent.h>
#include <errno.h>
#include <event2/event.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"

static atomic_llong g_beat_ms;
static atomic_bool g_stop;
static atomic_int g_suspended;   /* >0: the event loop is knowingly in a call that may take a while */
static pthread_t g_thread;
static bool g_running;
static struct event *g_timer;
static int g_limit_s;
static atomic_flag g_dump_lock = ATOMIC_FLAG_INIT;

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* REI_WATCHDOG_SELFTEST=1: two seconds in, the event loop spins forever, to see the report. */
static void on_selftest(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what; (void)arg;
	const char *mode = getenv("REI_WATCHDOG_SELFTEST");
	if (mode && !strcmp(mode, "suspended")) {
		/* The same stall, but declared as an allowed wait (like a VT switch): the watchdog must leave it alone. */
		rei_watchdog_suspend();
		struct timespec nap = { 4, 0 };
		nanosleep(&nap, NULL);
		rei_watchdog_resume();
		return;
	}
	for (volatile unsigned long i = 0;; i++)
		;
}

static void on_beat(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what; (void)arg;
	atomic_store(&g_beat_ms, now_ms());
}

/* Each thread, told to print its own backtrace (debugging only: not async-signal-safe). */
static void on_dump_signal(int sig)
{
	(void)sig;
	while (atomic_flag_test_and_set(&g_dump_lock))
		;
	char name[32] = "?";
	prctl(PR_GET_NAME, name, 0, 0, 0);
	void *frames[48];
	int n = backtrace(frames, 48);
	dprintf(STDERR_FILENO, "\n--- thread '%s' (tid %ld) is at:\n", name, (long)syscall(SYS_gettid));
	backtrace_symbols_fd(frames, n, STDERR_FILENO);
	atomic_flag_clear(&g_dump_lock);
}

static void slurp_line(const char *path, char *out, size_t n)
{
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(out, (int)n, f)) {
		size_t l = strlen(out);
		if (l && out[l - 1] == '\n')
			out[l - 1] = '\0';
	}
	fclose(f);
}

static void report_and_exit(void)
{
	dprintf(STDERR_FILENO, "\nrei-wm watchdog: the event loop has not run for %d seconds. Threads:\n", g_limit_s);
	DIR *d = opendir("/proc/self/task");
	pid_t self = getpid();
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			char path[300], comm[64], wchan[96], stat[256];
			snprintf(path, sizeof path, "/proc/self/task/%s/comm", de->d_name);
			slurp_line(path, comm, sizeof comm);
			snprintf(path, sizeof path, "/proc/self/task/%s/wchan", de->d_name);
			slurp_line(path, wchan, sizeof wchan);
			snprintf(path, sizeof path, "/proc/self/task/%s/stat", de->d_name);
			slurp_line(path, stat, sizeof stat);
			char *rp = strrchr(stat, ')');
			dprintf(STDERR_FILENO, "  tid %s '%s': state %c, kernel wait: %s\n", de->d_name, comm,
				rp && rp[1] == ' ' ? rp[2] : '?', wchan[0] ? wchan : "-");
		}
		rewinddir(d);
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			syscall(SYS_tgkill, self, atoi(de->d_name), SIGUSR2);
		}
		closedir(d);
	}
	struct timespec nap = { 1, 0 };   /* let the dumps finish */
	nanosleep(&nap, NULL);
	dprintf(STDERR_FILENO, "\nrei-wm watchdog: exiting, so the console is released.\n");
	_exit(70);
}

static void *watch(void *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, "rei-watchdog", 0, 0, 0);
	sigset_t all;   /* only the event loop's thread and the others take signals */
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);
	long long last_tick = now_ms();
	while (!atomic_load(&g_stop)) {
		struct timespec nap = { 0, 250 * 1000000L };
		nanosleep(&nap, NULL);
		long long now = now_ms();
		/* If this thread itself was not scheduled for seconds, the whole machine stalled (a firmware
		 * or GPU hiccup, suspend, ...): the event loop was frozen along with us, not hung. Give it a
		 * fresh start instead of declaring it dead. */
		if (now - last_tick > 2000) {
			rei_warn("watchdog: the machine stalled for %lld ms (not rei-wm's doing); carrying on", now - last_tick);
			atomic_store(&g_beat_ms, now);
		}
		last_tick = now;
		if (atomic_load(&g_suspended) > 0) {   /* a call that is allowed to wait: not a hang */
			atomic_store(&g_beat_ms, now);
			continue;
		}
		if (now - atomic_load(&g_beat_ms) > (long long)g_limit_s * 1000)
			report_and_exit();
	}
	return NULL;
}

void rei_watchdog_start(struct event_base *base)
{
	g_limit_s = 10;
	const char *e = getenv("REI_WATCHDOG_SECONDS");
	if (e && e[0])
		g_limit_s = atoi(e);
	if (g_limit_s <= 0 || g_running)
		return;
	struct sigaction sa = { 0 };
	sa.sa_handler = on_dump_signal;
	sigaction(SIGUSR2, &sa, NULL);
	void *warm[2];
	backtrace(warm, 2);   /* loads libgcc now, not inside the handler */

	atomic_store(&g_beat_ms, now_ms());
	g_timer = event_new(base, -1, EV_PERSIST, on_beat, NULL);
	struct timeval tv = { 0, 500000 };
	event_add(g_timer, &tv);
	const char *st = getenv("REI_WATCHDOG_SELFTEST");
	if (st && (st[0] == '1' || !strcmp(st, "suspended"))) {
		struct timeval later = { 2, 0 };
		evtimer_add(evtimer_new(base, on_selftest, NULL), &later);
	}
	atomic_store(&g_stop, false);
	if (pthread_create(&g_thread, NULL, watch, NULL) == 0)
		g_running = true;
}

void rei_watchdog_suspend(void)
{
	atomic_fetch_add(&g_suspended, 1);
}

void rei_watchdog_resume(void)
{
	atomic_store(&g_beat_ms, now_ms());   /* the loop gets a fresh start */
	atomic_fetch_sub(&g_suspended, 1);
}

void rei_watchdog_stop(void)
{
	if (g_running) {
		atomic_store(&g_stop, true);
		pthread_join(g_thread, NULL);
		g_running = false;
	}
	if (g_timer) {
		event_free(g_timer);
		g_timer = NULL;
	}
}
