#ifndef REI_BACKTRACE_H
#define REI_BACKTRACE_H

/* Crash and watchdog backtraces use glibc's <execinfo.h>. A libc without it (musl) needs libexecinfo; when neither
 * is there the build still works and the traces just say nothing (REI_HAVE_BACKTRACE is set by meson.build). */
#ifdef REI_HAVE_BACKTRACE
#include <execinfo.h>
#define rei_backtrace(frames, n) backtrace((frames), (n))
#define rei_backtrace_symbols_fd(frames, n, fd) backtrace_symbols_fd((frames), (n), (fd))
#else
static inline int rei_backtrace(void **frames, int n)
{
	(void)frames;
	(void)n;
	return 0;
}
static inline void rei_backtrace_symbols_fd(void *const *frames, int n, int fd)
{
	(void)frames;
	(void)n;
	(void)fd;
}
#endif

#endif
