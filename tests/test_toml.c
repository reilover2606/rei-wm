#include <math.h>
#include <string.h>

#include "check.h"
#include "util/toml.h"

static struct toml_table *parse_ok(const char *src)
{
	char err[256];
	struct toml_table *t = toml_parse(src, err, sizeof err);
	if (!t)
		fprintf(stderr, "unexpected parse error: %s\n  in: %s\n", err, src);
	CHECK(t);
	return t;
}

static void parse_fails(const char *src, const char *expect_in_msg)
{
	char err[256];
	struct toml_table *t = toml_parse(src, err, sizeof err);
	if (t) {
		fprintf(stderr, "expected failure for: %s\n", src);
		toml_free(t);
		exit(1);
	}
	if (!strstr(err, expect_in_msg)) {
		fprintf(stderr, "error '%s' lacks '%s' (input: %s)\n", err, expect_in_msg, src);
		exit(1);
	}
}

static void test_scalars(void)
{
	struct toml_table *t = parse_ok(
		"s = \"hi\\n\\u00e9\\\"\"\n"
		"lit = 'C:\\path'\n"
		"i = 42\n"
		"neg = -17\n"
		"big = 1_000_000\n"
		"hex = 0xFF_ff\n"
		"oct = 0o17\n"
		"bin = 0b101\n"
		"f = 3.14\n"
		"e = 1e3\n"
		"nf = -0.5\n"
		"yes = true\n"
		"no = false  # trailing comment\n");

	CHECK_STR(toml_lookup(t, "s")->u.s, "hi\n\xc3\xa9\"");
	CHECK_STR(toml_lookup(t, "lit")->u.s, "C:\\path");
	CHECK(toml_lookup(t, "i")->u.i == 42);
	CHECK(toml_lookup(t, "neg")->u.i == -17);
	CHECK(toml_lookup(t, "big")->u.i == 1000000);
	CHECK(toml_lookup(t, "hex")->u.i == 0xFFFF);
	CHECK(toml_lookup(t, "oct")->u.i == 15);
	CHECK(toml_lookup(t, "bin")->u.i == 5);
	CHECK(fabs(toml_lookup(t, "f")->u.d - 3.14) < 1e-9);
	CHECK(toml_lookup(t, "e")->type == TOML_FLOAT && toml_lookup(t, "e")->u.d == 1000.0);
	CHECK(fabs(toml_lookup(t, "nf")->u.d + 0.5) < 1e-9);
	CHECK(toml_lookup(t, "yes")->u.b == true);
	CHECK(toml_lookup(t, "no")->u.b == false);
	CHECK(toml_lookup(t, "missing") == NULL);
	toml_free(t);
}

static void test_int_limits(void)
{
	struct toml_table *t = parse_ok("max = 9223372036854775807\nmin = -9223372036854775808\n");
	CHECK(toml_lookup(t, "max")->u.i == INT64_MAX);
	CHECK(toml_lookup(t, "min")->u.i == INT64_MIN);
	toml_free(t);
	parse_fails("x = 9223372036854775808\n", "out-of-range");
	parse_fails("x = -9223372036854775809\n", "out-of-range");
}

static void test_tables(void)
{
	struct toml_table *t = parse_ok(
		"top = 1\n"
		"[a]\n"
		"x = 1\n"
		"[a.b]\n"
		"y = 2\n"
		"[c]\n"
		"d.e.f = 3\n"
		"\"quoted key\" = 4\n");

	CHECK(toml_lookup(t, "top")->u.i == 1);
	CHECK(toml_lookup(t, "a.x")->u.i == 1);
	CHECK(toml_lookup(t, "a.b.y")->u.i == 2);
	CHECK(toml_lookup(t, "c.d.e.f")->u.i == 3);
	const struct toml_value *c = toml_lookup(t, "c");
	CHECK(toml_table_get(c->u.tbl, "quoted key")->u.i == 4);
	toml_free(t);
}

static void test_arrays(void)
{
	struct toml_table *t = parse_ok(
		"a = [1, 2, 3]\n"
		"empty = []\n"
		"multi = [\n"
		"  \"x\", # comment\n"
		"  \"y\",\n"
		"]\n"
		"nested = [[1], [2, 3]]\n");

	const struct toml_value *a = toml_lookup(t, "a");
	CHECK(a->type == TOML_ARRAY && a->u.arr.len == 3 && a->u.arr.items[2]->u.i == 3);
	CHECK(toml_lookup(t, "empty")->u.arr.len == 0);
	const struct toml_value *m = toml_lookup(t, "multi");
	CHECK(m->u.arr.len == 2);
	CHECK_STR(m->u.arr.items[1]->u.s, "y");
	CHECK(toml_lookup(t, "nested")->u.arr.items[1]->u.arr.len == 2);
	toml_free(t);
}

static void test_array_of_tables(void)
{
	struct toml_table *t = parse_ok(
		"[[output]]\n"
		"name = \"A\"\n"
		"[output.extra]\n"
		"k = 1\n"
		"[[output]]\n"
		"name = \"B\"\n");

	const struct toml_value *o = toml_lookup(t, "output");
	CHECK(o->type == TOML_ARRAY && o->u.arr.len == 2);
	CHECK_STR(toml_table_get(o->u.arr.items[0]->u.tbl, "name")->u.s, "A");
	CHECK_STR(toml_table_get(o->u.arr.items[1]->u.tbl, "name")->u.s, "B");
	/* [output.extra] attached to the first element, not the second. */
	CHECK(toml_table_get(o->u.arr.items[0]->u.tbl, "extra") != NULL);
	CHECK(toml_table_get(o->u.arr.items[1]->u.tbl, "extra") == NULL);
	toml_free(t);
}

static void test_inline_table(void)
{
	struct toml_table *t = parse_ok("bind = { key = \"Return\", mods = [\"super\"], n = 1 }\n");
	CHECK_STR(toml_lookup(t, "bind.key")->u.s, "Return");
	CHECK(toml_lookup(t, "bind.mods")->u.arr.len == 1);
	CHECK(toml_lookup(t, "bind.n")->u.i == 1);
	toml_free(t);
}

static void test_line_endings_and_bom(void)
{
	struct toml_table *t = parse_ok("\xEF\xBB\xBF" "a = 1\r\n[t]\r\nb = 2\r\n");
	CHECK(toml_lookup(t, "a")->u.i == 1);
	CHECK(toml_lookup(t, "t.b")->u.i == 2);
	toml_free(t);
}

static void test_errors(void)
{
	parse_fails("a = 1\na = 2\n", "line 2: duplicate key");
	parse_fails("[a]\n[a]\n", "defined more than once");
	parse_fails("a = 1\n[a]\n", "not a table");
	parse_fails("a = \n", "expected a value");
	parse_fails("a = \"open\n", "unterminated");
	parse_fails("a = 1 2\n", "unexpected");
	parse_fails("= 1\n", "expected a key");
	parse_fails("a = [1, 2\n", "expected ',' or ']'");
	parse_fails("a = 01\n", "invalid integer");
	parse_fails("a = 1__0\n", "underscore");
	parse_fails("a = 1.\n", "invalid float");
	parse_fails("a = nan\n", "unsupported");
	parse_fails("a = 1979-05-27\n", "unsupported");
	parse_fails("a = \"\"\"multi\"\"\"\n", "multi-line");
	parse_fails("a = \"\\q\"\n", "invalid escape");
	parse_fails("a = \"\\ud800\"\n", "invalid unicode");
	parse_fails("[a\nb=1\n", "unterminated table header");
	parse_fails("[[a]]\n[a]\n", "not a table");
	parse_fails("a = [1]\n[[a]]\n", "not an array of tables");
	parse_fails("\n\n  bad line here\n", "line 3");
	parse_fails("t = { a = 1,\n b = 2 }\n", "expected");  /* inline tables are single-line */
}

int main(void)
{
	test_scalars();
	test_int_limits();
	test_tables();
	test_arrays();
	test_array_of_tables();
	test_inline_table();
	test_line_endings_and_bom();
	test_errors();
	puts("test_toml: ok");
	return 0;
}
