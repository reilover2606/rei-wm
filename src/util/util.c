#include <math.h>
#include "util/util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"

uint64_t rei_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void oom(void)
{
	fputs("rei-wm: out of memory\n", stderr);
	abort();
}

void *rei_xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		oom();
	return p;
}

void *rei_xrealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size ? size : 1);
	if (!p)
		oom();
	return p;
}

char *rei_xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p)
		oom();
	return p;
}

bool rei_parse_mode(const char *text, int32_t *width, int32_t *height, int32_t *refresh_mhz)
{
	if (!text)
		return false;
	char *end;
	long w = strtol(text, &end, 10);
	if (end == text || *end != 'x' || w <= 0 || w > 32768)
		return false;
	const char *p = end + 1;
	long h = strtol(p, &end, 10);
	if (end == p || h <= 0 || h > 32768)
		return false;

	int32_t mhz = 0;
	if (*end == '@') {
		p = end + 1;
		double hz = strtod(p, &end);
		if (end == p || hz <= 0 || hz > 1000)
			return false;
		mhz = (int32_t)llround(hz * 1000.0);
	}
	if (*end != '\0')
		return false;
	*width = (int32_t)w;
	*height = (int32_t)h;
	*refresh_mhz = mhz;
	return true;
}

char *rei_xstrndup(const char *s, size_t n)
{
	char *out = rei_xcalloc(n + 1, 1);
	memcpy(out, s, n);
	return out;
}

void rei_utf8_trim(char *s)
{
	size_t n = strlen(s);
	if (n == 0)
		return;
	size_t i = n;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80)   /* continuation bytes */
		i--;
	if (i == 0)
		return;
	unsigned char lead = (unsigned char)s[i - 1];
	size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	if (need > 1 && n - (i - 1) < need)
		s[i - 1] = '\0';   /* the last character is incomplete */
}

void rei_utf8_prefix(char *buf, const char *s, size_t max)
{
	size_t n = strlen(s);
	if (n > max)
		n = max;
	memcpy(buf, s, n);
	buf[n] = '\0';
	rei_utf8_trim(buf);
}

void rei_child_reset_signals(void)
{
	static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGCHLD, SIGPIPE, SIGQUIT };
	for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		signal(sigs[i], SIG_DFL);
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
}

bool rei_wait_child(int pid, int term_ms, const char *what)
{
	if (pid <= 0)
		return true;
	for (int phase = 0; phase < 2; phase++) {
		int limit = phase == 0 ? term_ms : 1000;
		for (int waited = 0; waited <= limit; waited += 10) {
			int status;
			pid_t r = waitpid((pid_t)pid, &status, WNOHANG);
			if (r == (pid_t)pid || (r < 0 && errno == ECHILD))
				return true;
			struct timespec nap = { 0, 10 * 1000000L };
			nanosleep(&nap, NULL);
		}
		if (phase == 0) {
			rei_warn("%s (pid %d) did not exit within %d ms of SIGTERM; killing it", what, pid, term_ms);
			kill((pid_t)pid, SIGKILL);
		}
	}
	rei_warn("%s (pid %d) cannot be killed (stuck in the kernel?); not waiting for it any longer", what, pid);
	return false;
}

bool rei_app_list_matches(const char *list, const char *app_id)
{
	if (!app_id)
		app_id = "";
	for (const char *p = list ? list : ""; *p;) {
		while (*p == ' ' || *p == ',')
			p++;
		const char *end = p;
		while (*end && *end != ',')
			end++;
		size_t len = (size_t)(end - p);
		while (len > 0 && p[len - 1] == ' ')
			len--;
		if (len == 1 && p[0] == '*')
			return true;
		if (len > 0 && strlen(app_id) == len && strncasecmp(app_id, p, len) == 0)
			return true;
		p = end;
	}
	return false;
}
