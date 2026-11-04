#include <string.h>

#include "check.h"
#include "config.h"
#include "theme.h"

static struct rei_config *parse_ok(const char *text)
{
	char err[300];
	struct rei_config *c = rei_config_parse(text, err, sizeof err);
	if (!c) {
		fprintf(stderr, "unexpected config error: %s\n", err);
		exit(1);
	}
	return c;
}

static void parse_fails(const char *text, const char *needle)
{
	char err[300];
	struct rei_config *c = rei_config_parse(text, err, sizeof err);
	if (c || !strstr(err, needle)) {
		fprintf(stderr, "expected an error mentioning '%s', got: %s\n", needle, c ? "(success)" : err);
		exit(1);
	}
}

int main(void)
{
	CHECK_STR(rei_element_name(REI_ELEM_TRAY_TILE), "tray_tile");
	CHECK(rei_shape_parse("pill") == REI_SHAPE_PILL && rei_shape_parse("hexagon") < 0);
	CHECK(rei_color_mix(0x000000, 0xffffff, 0.0) == 0x000000);
	CHECK(rei_color_mix(0x000000, 0xffffff, 1.0) == 0xffffff);
	CHECK(rei_color_mix(0x102030, 0x304050, 0.5) == 0x203040);

	/* The defaults come from the three base colours. */
	struct rei_config *c = parse_ok("[toolbar]\nbackground = \"#101010\"\nhighlight = \"#ff0000\"\n");
	const struct rei_style *tile = &c->theme.e[REI_ELEM_TRAY_TILE];
	CHECK(tile->active_color == 0xff0000);
	CHECK(c->theme.e[REI_ELEM_TOOLBAR].color == 0x101010);
	CHECK(tile->shape == REI_SHAPE_ROUNDED && tile->radius == 4 && tile->opacity == 1.0f);
	CHECK(c->theme.e[REI_ELEM_TOOLTIP].shadow.opacity > 0);
	CHECK(c->theme.e[REI_ELEM_TOOLBAR].shadow.opacity == 0);
	rei_config_free(c);

	/* [theme.*] overrides single properties and leaves the rest. */
	c = parse_ok("[theme.tray_tile]\ncolor = \"#223344\"\nshape = \"pill\"\nopacity = 0.5\n"
		     "shadow_opacity = 0.4\nshadow_blur = 8\nshadow_offset_y = 3\nborder_width = 2\n"
		     "[theme.menu_row]\nradius = 6\nshape = \"bevel\"\n");
	tile = &c->theme.e[REI_ELEM_TRAY_TILE];
	CHECK(tile->color == 0x223344 && tile->shape == REI_SHAPE_PILL && tile->opacity == 0.5f);
	CHECK(tile->shadow.opacity == 0.4f && tile->shadow.blur == 8 && tile->shadow.dy == 3);
	CHECK(tile->border_width == 2);
	CHECK(tile->active_color == 0x3c6ebe);  /* untouched */
	CHECK(c->theme.e[REI_ELEM_MENU_ROW].shape == REI_SHAPE_BEVEL && c->theme.e[REI_ELEM_MENU_ROW].radius == 6);
	CHECK(c->theme.e[REI_ELEM_TRAY_OVERFLOW].color != 0x223344);  /* elements are independent */
	rei_config_free(c);

	/* Glow: white by default on hovered rows, dialogs and the Run prompt; colour and size are editable. */
	c = parse_ok("");
	CHECK(c->theme.e[REI_ELEM_MENU_ROW].glow.color == 0xffffff && c->theme.e[REI_ELEM_MENU_ROW].glow.size > 0);
	CHECK(c->theme.e[REI_ELEM_DIALOG].glow.size > 0 && c->theme.e[REI_ELEM_RUN_PROMPT].glow.size > 0);
	CHECK(c->theme.e[REI_ELEM_TOOLBAR].glow.size == 0);
	rei_config_free(c);
	c = parse_ok("[theme.dialog]\nglow_color = \"#ff8800\"\nglow_size = 20\nglow_opacity = 0.9\n"
		     "[theme.menu_row]\nglow_size = 0\n");
	CHECK(c->theme.e[REI_ELEM_DIALOG].glow.color == 0xff8800 && c->theme.e[REI_ELEM_DIALOG].glow.size == 20);
	CHECK(c->theme.e[REI_ELEM_DIALOG].glow.opacity == 0.9f && c->theme.e[REI_ELEM_MENU_ROW].glow.size == 0);
	rei_config_free(c);
	parse_fails("[theme.dialog]\nglow_size = 100\n", "theme.dialog.glow_size");

	/* Typos are errors that say where. */
	parse_fails("[theme.tray_tyle]\ncolor = \"#000000\"\n", "theme.tray_tyle: unknown element");
	parse_fails("[theme.tray_tile]\ncolour = \"#000000\"\n", "theme.tray_tile.colour: unknown property");
	parse_fails("[theme.tray_tile]\ncolor = \"red\"\n", "theme.tray_tile.color: expected a colour");
	parse_fails("[theme.tray_tile]\nopacity = 2.0\n", "theme.tray_tile.opacity");
	parse_fails("[theme.tray_tile]\nshape = \"star\"\n", "theme.tray_tile.shape");
	parse_fails("[theme.tray_tile]\nradius = \"big\"\n", "theme.tray_tile.radius: expected a number");
	parse_fails("[theme]\ntray_tile = 3\n", "theme.tray_tile: expected a table");

	puts("theme: ok");
	return 0;
}
