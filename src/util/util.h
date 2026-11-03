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
char *rei_xstrdup(const char *s);

#endif
