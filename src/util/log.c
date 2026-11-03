#include "util/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "util/util.h"

static atomic_int g_level = REI_LOG_INFO;
static _Thread_local const char *t_thread_name;
static atomic_uint_fast64_t g_start_ns;

static const char *const level_names[] = {
	[REI_LOG_ERROR] = "ERROR",
	[REI_LOG_WARN] = "WARN ",
	[REI_LOG_INFO] = "INFO ",
	[REI_LOG_DEBUG] = "DEBUG",
};

void rei_log_set_level(enum rei_log_level level)
{
	atomic_store(&g_level, level);
}

enum rei_log_level rei_log_get_level(void)
{
	return atomic_load(&g_level);
}

int rei_log_parse_level(const char *name)
{
	static const char *const names[] = { "error", "warn", "info", "debug" };
	for (size_t i = 0; i < REI_ARRAY_LEN(names); i++) {
		if (strcmp(name, names[i]) == 0)
			return (int)i;
	}
	return -1;
}

void rei_log_set_thread_name(const char *name)
{
	t_thread_name = name;
}

void rei_log(enum rei_log_level level, const char *fmt, ...)
{
	if ((int)level > atomic_load(&g_level))
		return;

	uint64_t now = rei_now_ns();
	uint64_t zero = 0;
	atomic_compare_exchange_strong(&g_start_ns, &zero, now);
	uint64_t rel = now - atomic_load(&g_start_ns);

	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	/* One fprintf call per line: stdio locks the stream, so lines from
	 * different threads never interleave. */
	fprintf(stderr, "[%5llu.%03llu] %s [%s] %s\n",
		(unsigned long long)(rel / 1000000000ull),
		(unsigned long long)((rel / 1000000ull) % 1000ull),
		level_names[level],
		t_thread_name ? t_thread_name : "?",
		msg);
}
