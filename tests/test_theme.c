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

	/* The wifi icon: the text colour when connected, red when not. */
	c = parse_ok("");
	CHECK_STR(rei_element_name(REI_ELEM_WIFI), "wifi");
	CHECK(c->theme.e[REI_ELEM_WIFI].inactive_color == 0xe5534b && c->theme.e[REI_ELEM_WIFI].color != 0xe5534b);
	CHECK(c->toolbar_wifi && !strcmp(c->toolbar_wifi_command, "wpa_gui"));
	CHECK(c->brightness_step == 5 && !c->brightness_device[0] && !c->brightness_command[0]);
	rei_config_free(c);
	c = parse_ok("[toolbar]\nwifi = false\nwifi_command = \"nm-connection-editor\"\n[theme.wifi]\ninactive_color = \"#ff0000\"\n"
		     "[brightness]\nstep = 10\ndevice = \"intel_backlight\"\ncommand = \"brightnessctl set {percent}%\"\n");
	CHECK(!c->toolbar_wifi && !strcmp(c->toolbar_wifi_command, "nm-connection-editor"));
	CHECK(c->theme.e[REI_ELEM_WIFI].inactive_color == 0xff0000);
	CHECK(c->brightness_step == 10 && !strcmp(c->brightness_device, "intel_backlight") && !strcmp(c->brightness_command, "brightnessctl set {percent}%"));
	rei_config_free(c);
	parse_fails("[brightness]\nstep = 0\n", "brightness.step");

	/* The music controls are an element of their own: a dialog-like box, a little see-through. */
	c = parse_ok("");
	CHECK_STR(rei_element_name(REI_ELEM_MPD), "mpd");
	CHECK(c->theme.e[REI_ELEM_MPD].opacity < c->theme.e[REI_ELEM_DIALOG].opacity);
	CHECK(c->theme.e[REI_ELEM_MPD].shape == REI_SHAPE_ROUNDED && c->theme.e[REI_ELEM_MPD].glow.size > 0);
	CHECK(c->mpd_enabled && c->mpd_announce && c->mpd_port == 6600 && !strcmp(c->mpd_start_command, "mpd"));
	CHECK(c->messages_blur_radius == 12 && c->messages_blur_dim > 0.29 && c->messages_blur_dim < 0.31);
	rei_config_free(c);
	c = parse_ok("[theme.mpd]\ncolor = \"#123456\"\nradius = 3\n[mpd]\nenabled = false\nhost = \"/run/mpd.sock\"\n"
		     "port = 6601\nstart_command = \"mpd --no-daemon\"\nannounce = false\n[messages]\nblur_radius = 0\nblur_dim = 0.5\n");
	CHECK(c->theme.e[REI_ELEM_MPD].color == 0x123456 && c->theme.e[REI_ELEM_MPD].radius == 3);
	CHECK(!c->mpd_enabled && !c->mpd_announce && c->mpd_port == 6601 && !strcmp(c->mpd_host, "/run/mpd.sock"));
	CHECK(!strcmp(c->mpd_start_command, "mpd --no-daemon") && c->messages_blur_radius == 0 && c->messages_blur_dim == 0.5);
	rei_config_free(c);
	parse_fails("[mpd]\nport = 0\n", "mpd.port");
	parse_fails("[messages]\nblur_radius = 100\n", "messages.blur_radius");

	/* Text effects: all off by default, except that terminal windows glow a little. */
	c = parse_ok("");
	const struct rei_text_fx *fx = &c->theme.e[REI_ELEM_TOOLBAR].text_fx;
	CHECK(fx->glow_size == 0 && fx->blink == 0 && fx->shine == 0);
	CHECK(fx->glow_strength == 1 && fx->glow_color == 0xffffff && fx->shine_color == 0xffffff);
	CHECK(c->theme.e[REI_ELEM_WINDOW_TEXT].text_fx.glow_size > 0);
	CHECK_STR(c->theme.window_apps, "URxvt");
	CHECK_STR(c->theme.window_shader, "glow");
	CHECK(rei_theme_window_matches(&c->theme, "URxvt") && rei_theme_window_matches(&c->theme, "urxvt"));
	CHECK(!rei_theme_window_matches(&c->theme, "foot") && !rei_theme_window_matches(&c->theme, ""));
	CHECK(!rei_theme_window_matches(&c->theme, NULL));
	rei_config_free(c);
	c = parse_ok("[theme.tray_tile]\ntext_glow_size = 6\ntext_glow_strength = 2.5\ntext_glow_color = \"#ff8800\"\n"
		     "text_glow_tint = 0.5\ntext_blink = 1.5\ntext_shine = 0.7\ntext_shine_color = \"#ffeeaa\"\n"
		     "[theme.window_text]\napps = \" foot , Alacritty,URxvt \"\ntext_glow_size = 9\nglow_threshold = 0.6\n");
	fx = &c->theme.e[REI_ELEM_TRAY_TILE].text_fx;
	CHECK(fx->glow_size == 6 && fx->glow_strength == 2.5f && fx->glow_color == 0xff8800 && fx->glow_tint == 0.5f);
	CHECK(fx->blink == 1.5f && fx->shine == 0.7f && fx->shine_color == 0xffeeaa);
	CHECK(c->theme.e[REI_ELEM_TOOLBAR].text_fx.glow_size == 0);   /* elements are independent */
	CHECK(c->theme.window_area_limit == 0.5f);
	CHECK(c->theme.window_threshold == 0.6f && c->theme.e[REI_ELEM_WINDOW_TEXT].text_fx.glow_size == 9);
	CHECK(rei_theme_window_matches(&c->theme, "foot") && rei_theme_window_matches(&c->theme, "alacritty"));
	CHECK(rei_theme_window_matches(&c->theme, "URxvt") && !rei_theme_window_matches(&c->theme, "foo"));
	rei_config_free(c);
	c = parse_ok("[theme.window_text]\napps = \"*\"\n");
	CHECK(rei_theme_window_matches(&c->theme, "anything") && rei_theme_window_matches(&c->theme, ""));
	rei_config_free(c);
	c = parse_ok("[theme.window_text]\ntext_glow_size = 0\n");   /* off */
	CHECK(!rei_theme_window_matches(&c->theme, "URxvt"));
	rei_config_free(c);
	c = parse_ok("[theme.window_text]\ntext_glow_size = 0\ntext_blink = 2\n");   /* blinking alone counts */
	CHECK(rei_theme_window_matches(&c->theme, "URxvt"));
	rei_config_free(c);
	c = parse_ok("[theme.window_text]\nglow_area_limit = 0.7\n");
	CHECK(c->theme.window_area_limit == 0.7f);
	rei_config_free(c);
	parse_fails("[theme.window_text]\nglow_area_limit = 2\n", "theme.window_text.glow_area_limit");
	parse_fails("[theme.toolbar]\nglow_area_limit = 0.5\n", "theme.toolbar.glow_area_limit: unknown property");
	parse_fails("[theme.tray_tile]\ntext_glow_size = 100\n", "theme.tray_tile.text_glow_size");
	parse_fails("[theme.tray_tile]\ntext_shine = 2\n", "theme.tray_tile.text_shine");
	parse_fails("[theme.tray_tile]\ntext_blink = \"fast\"\n", "theme.tray_tile.text_blink: expected a number");
	parse_fails("[theme.tray_tile]\napps = \"x\"\n", "theme.tray_tile.apps: unknown property");
	parse_fails("[theme.tray_tile]\nglow_threshold = 0.5\n", "theme.tray_tile.glow_threshold: unknown property");
	parse_fails("[theme.window_text]\nshader = \"/no/such/shader.glsl\"\n", "theme.window_text.shader: cannot read");
	parse_fails("[theme.window_text]\napps = 3\n", "theme.window_text.apps: expected a string");

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
