#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "config.h"

static struct rei_config *parse_ok(const char *text)
{
	char err[256];
	struct rei_config *c = rei_config_parse(text, err, sizeof err);
	if (!c)
		fprintf(stderr, "unexpected config error: %s\n", err);
	CHECK(c);
	return c;
}

static void parse_fails(const char *text, const char *expect_in_msg)
{
	char err[256];
	struct rei_config *c = rei_config_parse(text, err, sizeof err);
	if (c) {
		fprintf(stderr, "expected config failure for: %s\n", text);
		exit(1);
	}
	if (!strstr(err, expect_in_msg)) {
		fprintf(stderr, "error '%s' lacks '%s'\n", err, expect_in_msg);
		exit(1);
	}
}

static void test_defaults(void)
{
	struct rei_config *c = rei_config_defaults();
	CHECK_STR(c->backend, "auto");
	CHECK_STR(c->composition_key, "super");
	CHECK(c->gap_outer == 10 && c->gap_inner == 10);
	CHECK(c->n_outputs == 0);
	CHECK(c->wallpaper_path == NULL);
	CHECK_STR(c->wallpaper_mode, "fill");
	CHECK(c->background_rgb == 0x1b2733);
	rei_config_free(c);

	/* An empty file means all defaults. */
	c = parse_ok("");
	CHECK_STR(c->backend, "auto");
	rei_config_free(c);
}

/* The file we write on first run must itself load, and mean "defaults". */
static void test_shipped_default_text_parses(void)
{
	struct rei_config *c = parse_ok(rei_config_default_text());
	CHECK_STR(c->backend, "auto");
	CHECK_STR(c->composition_key, "super");
	CHECK(c->gap_outer == 10 && c->gap_inner == 10);
	CHECK(c->n_outputs == 0);
	rei_config_free(c);
}

static void test_overrides(void)
{
	struct rei_config *c = parse_ok(
		"[general]\nbackend = \"headless\"\ncomposition_key = \"alt\"\n"
		"[layout]\ngap_outer = 4\ngap_inner = 0\n"
		"[[output]]\nname = \"L\"\nwidth = 2560\nheight = 1440\nrefresh = 143.98\nscale = 2\n"
		"[[output]]\nx = 3000\ny = -200\n");
	CHECK_STR(c->backend, "headless");
	CHECK_STR(c->composition_key, "alt");
	CHECK(c->gap_outer == 4 && c->gap_inner == 0);
	CHECK(c->n_outputs == 2);
	CHECK_STR(c->outputs[0].name, "L");
	CHECK(c->outputs[0].width == 2560 && c->outputs[0].height == 1440);
	CHECK(c->outputs[0].refresh_mhz == 143980);
	CHECK(c->outputs[0].scale == 2.0);  /* integer accepted for a float key */
	CHECK(!c->outputs[0].has_position);
	CHECK(c->outputs[1].name == NULL);
	CHECK(c->outputs[1].width == 1920);  /* defaults filled in */
	CHECK(c->outputs[1].has_position && c->outputs[1].x == 3000 && c->outputs[1].y == -200);
	CHECK(toml_lookup(c->root, "layout.gap_outer") != NULL);  /* raw doc retained */
	rei_config_free(c);
}

static void test_wallpaper(void)
{
	setenv("HOME", "/home/tester", 1);
	struct rei_config *c = parse_ok(
		"[wallpaper]\npath = \"~/Pictures/w.png\"\nmode = \"tile\"\ncolor = \"#FF00aa\"\n");
	CHECK_STR(c->wallpaper_path, "/home/tester/Pictures/w.png");
	CHECK_STR(c->wallpaper_mode, "tile");
	CHECK(c->background_rgb == 0xff00aa);
	rei_config_free(c);

	/* An empty path means "no wallpaper", as the shipped default says. */
	c = parse_ok("[wallpaper]\npath = \"\"\n");
	CHECK(c->wallpaper_path == NULL);
	rei_config_free(c);

	parse_fails("[wallpaper]\nmode = \"zoom\"\n", "wallpaper.mode");
	parse_fails("[wallpaper]\ncolor = \"red\"\n", "wallpaper.color");
	parse_fails("[wallpaper]\ncolor = \"#12345\"\n", "wallpaper.color");
	parse_fails("[wallpaper]\ncolor = \"#12345g\"\n", "wallpaper.color");
	parse_fails("[wallpaper]\npath = 3\n", "wallpaper.path");
}

static void test_notch_height(void)
{
	struct rei_config *c = rei_config_defaults();
	CHECK(c->notch_height == 22);
	rei_config_free(c);

	c = parse_ok("[decoration]\nnotch_height = 30\n");
	CHECK(c->notch_height == 30);
	rei_config_free(c);
	c = parse_ok("[decoration]\nnotch_height = 0\n");   /* 0 turns the notches off */
	CHECK(c->notch_height == 0);
	rei_config_free(c);

	parse_fails("[decoration]\nnotch_height = 65\n", "decoration.notch_height");
	parse_fails("[decoration]\nnotch_height = -1\n", "decoration.notch_height");
	parse_fails("[decoration]\nnotch_height = \"tall\"\n", "expected integer");

	/* The file written on first run lists the default explicitly. */
	c = parse_ok(rei_config_default_text());
	CHECK(c->notch_height == 22);
	rei_config_free(c);
}

static void test_validation(void)
{
	parse_fails("[general]\nbackend = \"drm\"\n", "general.backend");
	parse_fails("[general]\nbackend = 3\n", "expected string");
	parse_fails("[general]\ncomposition_key = \"hyper\"\n", "general.composition_key");
	parse_fails("[layout]\ngap_inner = -1\n", "out of range");
	parse_fails("[layout]\ngap_outer = \"big\"\n", "expected integer");
	parse_fails("layout = 3\n", "expected table");
	parse_fails("[[output]]\nwidth = 0\n", "output[0].width");
	parse_fails("[[output]]\nrefresh = 0\n", "output[0].refresh");
	parse_fails("[[output]]\nscale = 100\n", "output[0].scale");
	parse_fails("[[output]]\nx = 5\n", "x and y must be given together");
	parse_fails("[[output]]\nname = \"\"\n", "must not be empty");
	parse_fails("[[output]]\n[[output]]\nheight = true\n", "output[1].height");
	parse_fails("output = 3\n", "expected array");
	parse_fails("[general\n", "line 1");  /* TOML syntax errors surface too */
}

static void test_first_run_creates_default_file(void)
{
	char root[] = "/tmp/rei-cfg-test-XXXXXX";
	CHECK(mkdtemp(root));
	char path[256];
	snprintf(path, sizeof path, "%s/nested/dirs/config.toml", root);
	char err[256];

	/* An explicitly requested file that doesn't exist is an error. */
	CHECK(rei_config_load(path, false, err, sizeof err) == NULL);
	CHECK(strstr(err, "cannot read"));

	/* The default location is created, parents included, and then loads. */
	struct rei_config *c = rei_config_load(path, true, err, sizeof err);
	CHECK(c);
	CHECK_STR(c->path, path);
	CHECK_STR(c->backend, "auto");
	rei_config_free(c);

	FILE *f = fopen(path, "rb");
	CHECK(f);
	char buf[8192] = { 0 };
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	CHECK(n > 0);
	CHECK_STR(buf, rei_config_default_text());

	/* Loading again reads the file; user edits are honoured, not overwritten. */
	f = fopen(path, "w");
	fputs("[layout]\ngap_inner = 3\n", f);
	fclose(f);
	c = rei_config_load(path, true, err, sizeof err);
	CHECK(c && c->gap_inner == 3);
	rei_config_free(c);

	/* A syntax error names the file and the line. */
	f = fopen(path, "w");
	fputs("ok = 1\nbroken = \n", f);
	fclose(f);
	CHECK(rei_config_load(path, true, err, sizeof err) == NULL);
	CHECK(strstr(err, path) && strstr(err, "line 2"));

	unlink(path);
	snprintf(path, sizeof path, "%s/nested/dirs", root);
	rmdir(path);
	snprintf(path, sizeof path, "%s/nested", root);
	rmdir(path);
	rmdir(root);
}

int main(void)
{
	test_defaults();
	test_shipped_default_text_parses();
	test_overrides();
	test_wallpaper();
	test_notch_height();
	test_validation();
	test_first_run_creates_default_file();
	puts("test_config: ok");
	return 0;
}
