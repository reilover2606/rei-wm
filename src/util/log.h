#ifndef REI_LOG_H
#define REI_LOG_H

enum rei_log_level {
	REI_LOG_ERROR = 0,
	REI_LOG_WARN,
	REI_LOG_INFO,
	REI_LOG_DEBUG,
};

void rei_log_set_level(enum rei_log_level level);
enum rei_log_level rei_log_get_level(void);

/* Parses "error", "warn", "info" or "debug". Returns -1 if unrecognised. */
int rei_log_parse_level(const char *name);

/* Labels the calling thread in log output (e.g. "main", "render:DP-1").
 * The string must outlive the thread's last log call. */
void rei_log_set_thread_name(const char *name);

void rei_log(enum rei_log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define rei_error(...) rei_log(REI_LOG_ERROR, __VA_ARGS__)
#define rei_warn(...) rei_log(REI_LOG_WARN, __VA_ARGS__)
#define rei_info(...) rei_log(REI_LOG_INFO, __VA_ARGS__)
#define rei_debug(...) rei_log(REI_LOG_DEBUG, __VA_ARGS__)

#endif
