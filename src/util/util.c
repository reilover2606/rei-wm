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
