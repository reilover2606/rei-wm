#ifndef REI_TOML_H
#define REI_TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A small TOML reader.
 *
 * Supported: tables, dotted keys, arrays of tables, inline tables, arrays,
 * basic and literal strings, integers (decimal/hex/octal/binary, underscores),
 * floats, booleans, comments.
 * Not supported (reported as a parse error): multi-line strings, date/time
 * values, inf/nan.
 */

enum toml_type {
	TOML_STRING,
	TOML_INT,
	TOML_FLOAT,
	TOML_BOOL,
	TOML_ARRAY,
	TOML_TABLE,
};

struct toml_table;

struct toml_value {
	enum toml_type type;
	bool aot;           /* Array built from [[name]] headers. */
	bool explicit_def;  /* Table defined by an explicit [header]. */
	union {
		char *s;
		int64_t i;
		double d;
		bool b;
		struct {
			struct toml_value **items;
			size_t len, cap;
		} arr;
		struct toml_table *tbl;
	} u;
};

struct toml_entry {
	char *key;
	struct toml_value *val;
};

struct toml_table {
	struct toml_entry *entries;  /* In document order. */
	size_t len, cap;
};

/* Returns NULL on error and writes "line N: message" to err. */
struct toml_table *toml_parse(const char *src, char *err, size_t errlen);
void toml_free(struct toml_table *t);

/* Single-key lookup. */
const struct toml_value *toml_table_get(const struct toml_table *t, const char *key);

/* Lookup by dotted path ("layout.gap_inner"). Keys containing dots can't be
 * addressed this way; use toml_table_get() on each level instead. */
const struct toml_value *toml_lookup(const struct toml_table *t, const char *path);

const char *toml_type_name(enum toml_type type);

#endif
