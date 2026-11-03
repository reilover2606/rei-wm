#include "util/toml.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* Data model                                                               */
/* ------------------------------------------------------------------------ */

static void value_free(struct toml_value *v);

static void table_free(struct toml_table *t)
{
	if (!t)
		return;
	for (size_t i = 0; i < t->len; i++) {
		free(t->entries[i].key);
		value_free(t->entries[i].val);
	}
	free(t->entries);
	free(t);
}

static void value_free(struct toml_value *v)
{
	if (!v)
		return;
	switch (v->type) {
	case TOML_STRING:
		free(v->u.s);
		break;
	case TOML_ARRAY:
		for (size_t i = 0; i < v->u.arr.len; i++)
			value_free(v->u.arr.items[i]);
		free(v->u.arr.items);
		break;
	case TOML_TABLE:
		table_free(v->u.tbl);
		break;
	default:
		break;
	}
	free(v);
}

void toml_free(struct toml_table *t)
{
	table_free(t);
}

static struct toml_table *table_new(void)
{
	return rei_xcalloc(1, sizeof(struct toml_table));
}

static struct toml_value *value_new(enum toml_type type)
{
	struct toml_value *v = rei_xcalloc(1, sizeof *v);
	v->type = type;
	if (type == TOML_TABLE)
		v->u.tbl = table_new();
	return v;
}

static struct toml_value *table_find(const struct toml_table *t, const char *key)
{
	for (size_t i = 0; i < t->len; i++) {
		if (strcmp(t->entries[i].key, key) == 0)
			return t->entries[i].val;
	}
	return NULL;
}

/* Takes ownership of key and val. */
static void table_add(struct toml_table *t, char *key, struct toml_value *val)
{
	if (t->len == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 8;
		t->entries = rei_xrealloc(t->entries, t->cap * sizeof *t->entries);
	}
	t->entries[t->len].key = key;
	t->entries[t->len].val = val;
	t->len++;
}

static void array_push(struct toml_value *arr, struct toml_value *item)
{
	if (arr->u.arr.len == arr->u.arr.cap) {
		arr->u.arr.cap = arr->u.arr.cap ? arr->u.arr.cap * 2 : 4;
		arr->u.arr.items = rei_xrealloc(arr->u.arr.items,
			arr->u.arr.cap * sizeof *arr->u.arr.items);
	}
	arr->u.arr.items[arr->u.arr.len++] = item;
}

const struct toml_value *toml_table_get(const struct toml_table *t, const char *key)
{
	return t ? table_find(t, key) : NULL;
}

const struct toml_value *toml_lookup(const struct toml_table *t, const char *path)
{
	const struct toml_value *v = NULL;
	char *copy = rei_xstrdup(path);
	char *save = NULL;

	for (char *part = strtok_r(copy, ".", &save); part; part = strtok_r(NULL, ".", &save)) {
		if (v) {
			if (v->type != TOML_TABLE) {
				v = NULL;
				break;
			}
			t = v->u.tbl;
		}
		v = toml_table_get(t, part);
		if (!v)
			break;
	}
	free(copy);
	return v;
}

const char *toml_type_name(enum toml_type type)
{
	switch (type) {
	case TOML_STRING: return "string";
	case TOML_INT: return "integer";
	case TOML_FLOAT: return "float";
	case TOML_BOOL: return "boolean";
	case TOML_ARRAY: return "array";
	case TOML_TABLE: return "table";
	}
	return "unknown";
}

/* ------------------------------------------------------------------------ */
/* Parser                                                                   */
/* ------------------------------------------------------------------------ */

struct parser {
	const char *p;
	int line;
	char *err;
	size_t errlen;
	bool failed;
};

static void perr(struct parser *ps, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void perr(struct parser *ps, const char *fmt, ...)
{
	if (ps->failed)
		return;
	ps->failed = true;

	int n = snprintf(ps->err, ps->errlen, "line %d: ", ps->line);
	if (n < 0 || (size_t)n >= ps->errlen)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ps->err + n, ps->errlen - (size_t)n, fmt, ap);
	va_end(ap);
}

static void skip_ws(struct parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t')
		ps->p++;
}

static void skip_comment(struct parser *ps)
{
	if (*ps->p == '#') {
		while (*ps->p && *ps->p != '\n')
			ps->p++;
	}
}

/* Skips whitespace, comments and newlines. */
static void skip_blank(struct parser *ps)
{
	for (;;) {
		skip_ws(ps);
		skip_comment(ps);
		if (*ps->p == '\r' && ps->p[1] == '\n')
			ps->p++;
		if (*ps->p == '\n') {
			ps->line++;
			ps->p++;
			continue;
		}
		return;
	}
}

/* After a value or header: only whitespace/comment may remain on the line. */
static bool expect_eol(struct parser *ps)
{
	skip_ws(ps);
	skip_comment(ps);
	if (*ps->p == '\r' && ps->p[1] == '\n')
		ps->p++;
	if (*ps->p == '\n') {
		ps->line++;
		ps->p++;
		return true;
	}
	if (*ps->p == '\0')
		return true;
	perr(ps, "unexpected '%c' after value", *ps->p);
	return false;
}

static size_t utf8_encode(uint32_t cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

struct strbuf {
	char *s;
	size_t len, cap;
};

static void sb_put(struct strbuf *sb, const char *data, size_t n)
{
	if (sb->len + n + 1 > sb->cap) {
		sb->cap = (sb->len + n + 1) * 2;
		sb->s = rei_xrealloc(sb->s, sb->cap);
	}
	memcpy(sb->s + sb->len, data, n);
	sb->len += n;
	sb->s[sb->len] = '\0';
}

/* Parses a "basic" or 'literal' string starting at the opening quote. */
static char *parse_string(struct parser *ps)
{
	char quote = *ps->p;
	if (ps->p[1] == quote && ps->p[2] == quote) {
		perr(ps, "multi-line strings are not supported");
		return NULL;
	}
	ps->p++;

	struct strbuf sb = { 0 };
	sb_put(&sb, "", 0);

	for (;;) {
		char c = *ps->p;
		if (c == '\0' || c == '\n' || (c == '\r' && ps->p[1] == '\n')) {
			perr(ps, "unterminated string");
			goto fail;
		}
		ps->p++;
		if (c == quote)
			return sb.s;

		if (quote == '"' && c == '\\') {
			char e = *ps->p++;
			switch (e) {
			case 'b': sb_put(&sb, "\b", 1); break;
			case 't': sb_put(&sb, "\t", 1); break;
			case 'n': sb_put(&sb, "\n", 1); break;
			case 'f': sb_put(&sb, "\f", 1); break;
			case 'r': sb_put(&sb, "\r", 1); break;
			case '"': sb_put(&sb, "\"", 1); break;
			case '\\': sb_put(&sb, "\\", 1); break;
			case 'u':
			case 'U': {
				int digits = (e == 'u') ? 4 : 8;
				uint32_t cp = 0;
				for (int i = 0; i < digits; i++) {
					char h = *ps->p;
					int d;
					if (h >= '0' && h <= '9') d = h - '0';
					else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
					else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
					else {
						perr(ps, "bad unicode escape");
						goto fail;
					}
					cp = cp * 16 + (uint32_t)d;
					ps->p++;
				}
				if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0) {
					perr(ps, "invalid unicode code point");
					goto fail;
				}
				char buf[4];
				sb_put(&sb, buf, utf8_encode(cp, buf));
				break;
			}
			default:
				perr(ps, "invalid escape '\\%c'", e ? e : '?');
				goto fail;
			}
			continue;
		}
		sb_put(&sb, &c, 1);
	}
fail:
	free(sb.s);
	return NULL;
}

static bool is_bare_key_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '-';
}

struct keypath {
	char **parts;
	size_t n;
};

static void keypath_free(struct keypath *k)
{
	for (size_t i = 0; i < k->n; i++)
		free(k->parts[i]);
	free(k->parts);
	k->parts = NULL;
	k->n = 0;
}

static bool parse_keypath(struct parser *ps, struct keypath *out)
{
	out->parts = NULL;
	out->n = 0;
	size_t cap = 0;

	for (;;) {
		skip_ws(ps);
		char *part;
		if (*ps->p == '"' || *ps->p == '\'') {
			part = parse_string(ps);
			if (!part)
				goto fail;
		} else {
			const char *start = ps->p;
			while (is_bare_key_char(*ps->p))
				ps->p++;
			if (ps->p == start) {
				perr(ps, "expected a key");
				goto fail;
			}
			part = strndup(start, (size_t)(ps->p - start));
			if (!part)
				abort();
		}
		if (out->n == cap) {
			cap = cap ? cap * 2 : 4;
			out->parts = rei_xrealloc(out->parts, cap * sizeof *out->parts);
		}
		out->parts[out->n++] = part;

		skip_ws(ps);
		if (*ps->p == '.') {
			ps->p++;
			continue;
		}
		return true;
	}
fail:
	keypath_free(out);
	return false;
}

/* Finds or creates the sub-table `key` of `t`. For arrays of tables, descends
 * into the most recently added element. */
static struct toml_table *descend(struct parser *ps, struct toml_table *t, const char *key)
{
	struct toml_value *v = table_find(t, key);
	if (!v) {
		v = value_new(TOML_TABLE);
		table_add(t, rei_xstrdup(key), v);
		return v->u.tbl;
	}
	if (v->type == TOML_TABLE)
		return v->u.tbl;
	if (v->type == TOML_ARRAY && v->aot && v->u.arr.len > 0)
		return v->u.arr.items[v->u.arr.len - 1]->u.tbl;
	perr(ps, "key '%s' is already defined as a %s, not a table", key, toml_type_name(v->type));
	return NULL;
}

static struct toml_value *parse_value(struct parser *ps);

static bool is_value_end(char c)
{
	return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == ',' || c == ']' || c == '}' || c == '#';
}

static struct toml_value *parse_number(struct parser *ps)
{
	const char *start = ps->p;
	while (!is_value_end(*ps->p))
		ps->p++;
	size_t len = (size_t)(ps->p - start);
	if (len == 0 || len > 63) {
		perr(ps, "invalid value");
		return NULL;
	}

	char buf[64];
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		if (start[i] == '_') {
			/* Underscores must sit between two digits (hex digits for 0x). */
			bool ok = i > 0 && i + 1 < len &&
				  isxdigit((unsigned char)start[i - 1]) &&
				  isxdigit((unsigned char)start[i + 1]);
			if (!ok) {
				perr(ps, "invalid underscore in number");
				return NULL;
			}
			continue;
		}
		buf[n++] = start[i];
	}
	buf[n] = '\0';

	const char *digits = buf;
	bool neg = false;
	if (*digits == '+' || *digits == '-') {
		neg = (*digits == '-');
		digits++;
	}

	int base = 10;
	if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'o' || digits[1] == 'b')) {
		if (digits != buf) {
			perr(ps, "sign not allowed on prefixed integers");
			return NULL;
		}
		base = digits[1] == 'x' ? 16 : digits[1] == 'o' ? 8 : 2;
		digits += 2;
	}

	bool is_float = false;
	if (base == 10) {
		for (const char *c = digits; *c; c++) {
			if (*c == '.' || *c == 'e' || *c == 'E')
				is_float = true;
			else if (*c == '+' || *c == '-') {
				/* A sign is only valid at the start or right after an
				 * exponent marker; anywhere else this is a date. */
				if (c != digits && c[-1] != 'e' && c[-1] != 'E') {
					perr(ps, "unsupported value '%.*s' (dates and times are not supported)",
					     (int)len, start);
					return NULL;
				}
			} else if (!(*c >= '0' && *c <= '9')) {
				perr(ps, "unsupported or invalid value '%.*s'", (int)len, start);
				return NULL;
			}
		}
	}

	errno = 0;
	char *end = NULL;
	if (is_float) {
		if (digits[0] == '.' || digits[strlen(digits) - 1] == '.' ||
		    strstr(digits, ".e") || strstr(digits, ".E") ||
		    strstr(digits, "e.") || strstr(digits, "E.")) {
			perr(ps, "invalid float '%.*s'", (int)len, start);
			return NULL;
		}
		double d = strtod(buf, &end);
		if (*end != '\0' || errno == ERANGE) {
			perr(ps, "invalid float '%.*s'", (int)len, start);
			return NULL;
		}
		struct toml_value *v = value_new(TOML_FLOAT);
		v->u.d = d;
		return v;
	}

	if (*digits == '\0' || (base == 10 && digits[0] == '0' && digits[1] != '\0')) {
		perr(ps, "invalid integer '%.*s'", (int)len, start);
		return NULL;
	}
	if (*digits == '+' || *digits == '-') {
		perr(ps, "invalid integer '%.*s'", (int)len, start);
		return NULL;
	}
	unsigned long long mag = strtoull(digits, &end, base);
	if (*end != '\0' || errno == ERANGE ||
	    mag > (neg ? (unsigned long long)INT64_MAX + 1ull : (unsigned long long)INT64_MAX)) {
		perr(ps, "invalid or out-of-range integer '%.*s'", (int)len, start);
		return NULL;
	}
	struct toml_value *v = value_new(TOML_INT);
	v->u.i = neg ? (int64_t)(0 - mag) : (int64_t)mag;
	return v;
}

static struct toml_value *parse_array(struct parser *ps)
{
	ps->p++; /* '[' */
	struct toml_value *arr = value_new(TOML_ARRAY);

	for (;;) {
		skip_blank(ps);
		if (*ps->p == ']') {
			ps->p++;
			return arr;
		}
		struct toml_value *item = parse_value(ps);
		if (!item)
			goto fail;
		array_push(arr, item);

		skip_blank(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == ']') {
			ps->p++;
			return arr;
		}
		perr(ps, "expected ',' or ']' in array");
		goto fail;
	}
fail:
	value_free(arr);
	return NULL;
}

static struct toml_value *parse_inline_table(struct parser *ps)
{
	ps->p++; /* '{' */
	struct toml_value *tv = value_new(TOML_TABLE);
	skip_ws(ps);
	if (*ps->p == '}') {
		ps->p++;
		return tv;
	}

	for (;;) {
		struct keypath kp;
		if (!parse_keypath(ps, &kp))
			goto fail;
		struct toml_table *t = tv->u.tbl;
		for (size_t i = 0; i + 1 < kp.n; i++) {
			t = descend(ps, t, kp.parts[i]);
			if (!t) {
				keypath_free(&kp);
				goto fail;
			}
		}
		skip_ws(ps);
		if (*ps->p != '=') {
			perr(ps, "expected '=' after key");
			keypath_free(&kp);
			goto fail;
		}
		ps->p++;
		skip_ws(ps);
		if (table_find(t, kp.parts[kp.n - 1])) {
			perr(ps, "duplicate key '%s'", kp.parts[kp.n - 1]);
			keypath_free(&kp);
			goto fail;
		}
		struct toml_value *val = parse_value(ps);
		if (!val) {
			keypath_free(&kp);
			goto fail;
		}
		table_add(t, kp.parts[kp.n - 1], val);
		kp.parts[kp.n - 1] = NULL;
		keypath_free(&kp);

		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			skip_ws(ps);
			continue;
		}
		if (*ps->p == '}') {
			ps->p++;
			return tv;
		}
		perr(ps, "expected ',' or '}' in inline table");
		goto fail;
	}
fail:
	value_free(tv);
	return NULL;
}

static bool match_word(struct parser *ps, const char *word)
{
	size_t n = strlen(word);
	if (strncmp(ps->p, word, n) == 0 && is_value_end(ps->p[n])) {
		ps->p += n;
		return true;
	}
	return false;
}

static struct toml_value *parse_value(struct parser *ps)
{
	switch (*ps->p) {
	case '"':
	case '\'': {
		char *s = parse_string(ps);
		if (!s)
			return NULL;
		struct toml_value *v = value_new(TOML_STRING);
		v->u.s = s;
		return v;
	}
	case '[':
		return parse_array(ps);
	case '{':
		return parse_inline_table(ps);
	case 't':
		if (match_word(ps, "true")) {
			struct toml_value *v = value_new(TOML_BOOL);
			v->u.b = true;
			return v;
		}
		break;
	case 'f':
		if (match_word(ps, "false")) {
			struct toml_value *v = value_new(TOML_BOOL);
			v->u.b = false;
			return v;
		}
		break;
	case '\0':
	case '\n':
	case '\r':
	case '#':
		perr(ps, "expected a value");
		return NULL;
	default:
		break;
	}

	if ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '+' || *ps->p == '-')
		return parse_number(ps);

	perr(ps, "unsupported or invalid value");
	return NULL;
}

static bool parse_header(struct parser *ps, struct toml_table *root, struct toml_table **current)
{
	bool is_aot = false;
	ps->p++; /* '[' */
	if (*ps->p == '[') {
		is_aot = true;
		ps->p++;
	}

	struct keypath kp;
	if (!parse_keypath(ps, &kp))
		return false;

	skip_ws(ps);
	if (*ps->p != ']' || (is_aot && ps->p[1] != ']')) {
		perr(ps, "unterminated table header");
		keypath_free(&kp);
		return false;
	}
	ps->p += is_aot ? 2 : 1;

	struct toml_table *t = root;
	for (size_t i = 0; i + 1 < kp.n; i++) {
		t = descend(ps, t, kp.parts[i]);
		if (!t) {
			keypath_free(&kp);
			return false;
		}
	}

	const char *last = kp.parts[kp.n - 1];
	struct toml_value *v = table_find(t, last);
	bool ok = false;

	if (is_aot) {
		if (!v) {
			v = value_new(TOML_ARRAY);
			v->aot = true;
			table_add(t, rei_xstrdup(last), v);
		} else if (v->type != TOML_ARRAY || !v->aot) {
			perr(ps, "key '%s' is already defined and is not an array of tables", last);
			goto out;
		}
		struct toml_value *elem = value_new(TOML_TABLE);
		array_push(v, elem);
		*current = elem->u.tbl;
		ok = true;
	} else {
		if (!v) {
			v = value_new(TOML_TABLE);
			v->explicit_def = true;
			table_add(t, rei_xstrdup(last), v);
		} else if (v->type != TOML_TABLE) {
			perr(ps, "key '%s' is already defined and is not a table", last);
			goto out;
		} else if (v->explicit_def) {
			perr(ps, "table '%s' is defined more than once", last);
			goto out;
		} else {
			v->explicit_def = true;
		}
		*current = v->u.tbl;
		ok = true;
	}
out:
	keypath_free(&kp);
	return ok && expect_eol(ps);
}

static bool parse_keyval(struct parser *ps, struct toml_table *current)
{
	struct keypath kp;
	if (!parse_keypath(ps, &kp))
		return false;

	struct toml_table *t = current;
	for (size_t i = 0; i + 1 < kp.n; i++) {
		t = descend(ps, t, kp.parts[i]);
		if (!t) {
			keypath_free(&kp);
			return false;
		}
	}

	skip_ws(ps);
	if (*ps->p != '=') {
		perr(ps, "expected '=' after key");
		keypath_free(&kp);
		return false;
	}
	ps->p++;
	skip_ws(ps);

	if (table_find(t, kp.parts[kp.n - 1])) {
		perr(ps, "duplicate key '%s'", kp.parts[kp.n - 1]);
		keypath_free(&kp);
		return false;
	}

	/* Arrays may span lines, so the line counter can advance in here. */
	struct toml_value *val = parse_value(ps);
	if (!val) {
		keypath_free(&kp);
		return false;
	}
	table_add(t, kp.parts[kp.n - 1], val);
	kp.parts[kp.n - 1] = NULL;
	keypath_free(&kp);
	return expect_eol(ps);
}

struct toml_table *toml_parse(const char *src, char *err, size_t errlen)
{
	struct parser ps = { .p = src, .line = 1, .err = err, .errlen = errlen };
	if (errlen)
		err[0] = '\0';

	/* Skip a UTF-8 byte order mark. */
	if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB &&
	    (unsigned char)ps.p[2] == 0xBF)
		ps.p += 3;

	struct toml_table *root = table_new();
	struct toml_table *current = root;

	for (;;) {
		skip_blank(&ps);
		if (*ps.p == '\0')
			break;
		bool ok = (*ps.p == '[') ? parse_header(&ps, root, &current)
					 : parse_keyval(&ps, current);
		if (!ok) {
			toml_free(root);
			return NULL;
		}
	}
	return root;
}
