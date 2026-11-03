#ifndef REI_UTIL_H
#define REI_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REI_UNUSED __attribute__((unused))
#define REI_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Monotonic clock in nanoseconds. */
uint64_t rei_now_ns(void);

/* Allocation helpers that abort on out-of-memory. A compositor has no sane way
 * to continue without memory, and checking every call site adds noise. */
void *rei_xcalloc(size_t n, size_t size);
void *rei_xrealloc(void *ptr, size_t size);
char *rei_xstrndup(const char *s, size_t n);
char *rei_xstrdup(const char *s);

/* Parses "1920x1080", "1920x1080@60" or "2560x1440@143.912". Returns false
 * on anything else. `refresh_mhz` is 0 when no rate was given. */
bool rei_parse_mode(const char *text, int32_t *width, int32_t *height, int32_t *refresh_mhz);

#endif
