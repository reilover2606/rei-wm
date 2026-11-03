#include <math.h>
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
