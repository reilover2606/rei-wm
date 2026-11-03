#ifndef REI_TEST_CHECK_H
#define REI_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* Minimal assertion helpers; a failing check prints its location and exits. */

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
				__LINE__, #cond);                            \
			exit(1);                                             \
		}                                                            \
	} while (0)

#define CHECK_STR(a, b)                                                      \
	do {                                                                 \
		const char *_a = (a), *_b = (b);                             \
		if (!_a || !_b || strcmp(_a, _b) != 0) {                     \
			fprintf(stderr, "%s:%d: CHECK_STR failed: '%s' != '%s'\n", \
				__FILE__, __LINE__, _a ? _a : "(null)",      \
				_b ? _b : "(null)");                         \
			exit(1);                                             \
		}                                                            \
	} while (0)

#endif
