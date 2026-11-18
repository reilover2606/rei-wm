#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "input/keybindings.h"
#include "util/log.h"
#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* Typed accessors. Absent keys leave the default untouched; present keys   */
/* of the wrong type or out of range are errors, so typos are never         */
/* silently ignored.                                                        */
/* ------------------------------------------------------------------------ */

static bool parse_color(const char *s, uint32_t *out);

struct reader {
	char *err;
	size_t errlen;
	bool failed;
};

static void fail(struct reader *r, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void fail(struct reader *r, const char *fmt, ...)
{
	if (r->failed)
		return;
	r->failed = true;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->err, r->errlen, fmt, ap);
	va_end(ap);
}

static const struct toml_value *get(struct reader *r, const struct toml_table *t,
				    const char *prefix, const char *key,
				    enum toml_type want)
{
	const struct toml_value *v = toml_table_get(t, key);
	if (!v)
		return NULL;
	if (v->type != want) {
		fail(r, "%s%s: expected %s, got %s", prefix, key,
		     toml_type_name(want), toml_type_name(v->type));
		return NULL;
	}
	return v;
}

static void read_int(struct reader *r, const struct toml_table *t, const char *prefix,
		     const char *key, int32_t *out, int32_t min, int32_t max)
{
	const struct toml_value *v = get(r, t, prefix, key, TOML_INT);
	if (!v)
		return;
	if (v->u.i < min || v->u.i > max) {
		fail(r, "%s%s: %lld is out of range (%d to %d)", prefix, key,
		     (long long)v->u.i, min, max);
		return;
	}
	*out = (int32_t)v->u.i;
}

/* Accepts integers where a float is expected ("scale = 2"). */
static bool read_double(struct reader *r, const struct toml_table *t, const char *prefix,
			const char *key, double *out, double min, double max)
{
	const struct toml_value *v = toml_table_get(t, key);
	if (!v)
		return false;
	double d;
	if (v->type == TOML_FLOAT) {
		d = v->u.d;
	} else if (v->type == TOML_INT) {
		d = (double)v->u.i;
	} else {
		fail(r, "%s%s: expected float, got %s", prefix, key, toml_type_name(v->type));
		return false;
	}
	if (!(d >= min && d <= max)) {
		fail(r, "%s%s: %g is out of range (%g to %g)", prefix, key, d, min, max);
		return false;
	}
	*out = d;
	return true;
}

static void read_string(struct reader *r, const struct toml_table *t, const char *prefix,
			const char *key, char **out)
{
	const struct toml_value *v = get(r, t, prefix, key, TOML_STRING);
	if (!v)
		return;
	free(*out);
	*out = rei_xstrdup(v->u.s);
}

static void read_color(struct reader *r, const struct toml_table *t, const char *prefix,
		       const char *key, uint32_t *out)
{
	const struct toml_value *v = get(r, t, prefix, key, TOML_STRING);
	if (v && !parse_color(v->u.s, out))
		fail(r, "%s%s: '%s' is not a colour like \"#1b2733\"", prefix, key, v->u.s);
}

static void read_choice(struct reader *r, const struct toml_table *t, const char *prefix,
			const char *key, char **out, const char *const *choices)
{
	const struct toml_value *v = get(r, t, prefix, key, TOML_STRING);
	if (!v)
		return;
	for (const char *const *c = choices; *c; c++) {
		if (strcmp(v->u.s, *c) == 0) {
			free(*out);
			*out = rei_xstrdup(v->u.s);
			return;
		}
	}

	char list[128] = "";
	for (const char *const *c = choices; *c; c++) {
		if (list[0])
			strncat(list, ", ", sizeof list - strlen(list) - 1);
		strncat(list, *c, sizeof list - strlen(list) - 1);
	}
	fail(r, "%s%s: '%s' is not one of: %s", prefix, key, v->u.s, list);
}

static const struct toml_table *get_table(struct reader *r, const struct toml_table *t,
					  const char *key)
{
	const struct toml_value *v = get(r, t, "", key, TOML_TABLE);
	return v ? v->u.tbl : NULL;
}

/* ------------------------------------------------------------------------ */

/* "~/pic.png" -> "$HOME/pic.png". */
static char *expand_home(const char *path)
{
	const char *home = getenv("HOME");
	if (path[0] == '~' && (path[1] == '/' || path[1] == '\0') && home) {
		char *out = NULL;
		if (asprintf(&out, "%s%s", home, path + 1) >= 0)
			return out;
	}
	return rei_xstrdup(path);
}

static bool parse_color(const char *s, uint32_t *out)
{
	if (s[0] != '#' || strlen(s) != 7)
		return false;
	uint32_t v = 0;
	for (int i = 1; i < 7; i++) {
		char c = s[i];
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return false;
		v = v * 16 + (uint32_t)d;
	}
	*out = v;
	return true;
}

static void read_output(struct reader *r, const struct toml_table *t, size_t index,
			struct rei_output_config *out)
{
	char prefix[32];
	snprintf(prefix, sizeof prefix, "output[%zu].", index);

	out->width = 1920;
	out->height = 1080;
	out->refresh_mhz = 60000;
	out->scale = 1.0;
	out->enabled = true;

	const struct toml_value *name = get(r, t, prefix, "name", TOML_STRING);
	if (name) {
		if (name->u.s[0] == '\0')
			fail(r, "%sname: must not be empty", prefix);
		out->name = rei_xstrdup(name->u.s);
	}

	read_int(r, t, prefix, "width", &out->width, 1, 16384);
	read_int(r, t, prefix, "height", &out->height, 1, 16384);

	double hz;
	if (read_double(r, t, prefix, "refresh", &hz, 1.0, 1000.0))
		out->refresh_mhz = (int32_t)lround(hz * 1000.0);

	out->has_mode = toml_table_get(t, "width") || toml_table_get(t, "height") ||
			toml_table_get(t, "refresh");
	/* mode = "2560x1440@144" is the same thing in one string. */
	const struct toml_value *mode = get(r, t, prefix, "mode", TOML_STRING);
	if (mode) {
		int32_t w, h, mhz;
		if (!rei_parse_mode(mode->u.s, &w, &h, &mhz)) {
			fail(r, "%smode: '%s' is not a mode like \"1920x1080\" or \"2560x1440@144\"",
			     prefix, mode->u.s);
		} else {
			out->width = w;
			out->height = h;
			if (mhz)
				out->refresh_mhz = mhz;
			out->has_mode = true;
		}
	}
	const struct toml_value *enabled = get(r, t, prefix, "enabled", TOML_BOOL);
	if (enabled)
		out->enabled = enabled->u.b;

	read_double(r, t, prefix, "scale", &out->scale, 0.25, 8.0);

	bool has_x = toml_table_get(t, "x") != NULL;
	bool has_y = toml_table_get(t, "y") != NULL;
	if (has_x != has_y)
		fail(r, "%sx and y must be given together", prefix);
	if (has_x && has_y) {
		read_int(r, t, prefix, "x", &out->x, -100000, 100000);
		read_int(r, t, prefix, "y", &out->y, -100000, 100000);
		out->has_position = true;
	}
}

struct rei_config *rei_config_defaults(void)
{
	struct rei_config *cfg = rei_xcalloc(1, sizeof *cfg);
	cfg->backend = rei_xstrdup("auto");
	cfg->composition_key = rei_xstrdup("super");
	cfg->gap_outer = 10;
	cfg->gap_inner = 10;
	cfg->kb_rules = rei_xstrdup("");
	cfg->kb_model = rei_xstrdup("");
	cfg->kb_layout = rei_xstrdup("");
	cfg->kb_variant = rei_xstrdup("");
	cfg->kb_options = rei_xstrdup("");
	cfg->toolbar_enabled = true;
	cfg->toolbar_position = REI_BAR_TOP;
	cfg->toolbar_thickness = 28;
	cfg->toolbar_tray = REI_TRAY_ICONS_NAMES;
	cfg->toolbar_battery = true;
	cfg->toolbar_battery_percent = true;
	cfg->toolbar_wifi = true;
	cfg->toolbar_wifi_command = rei_xstrdup("wpa_gui");
	cfg->toolbar_sysinfo = true;
	cfg->start_enabled = true;
	cfg->focus_follows_mouse = true;
	cfg->display_sleep_minutes = 5;
	cfg->cursor_theme = rei_xstrdup("");
	cfg->volume_target = REI_VOLUME_AUTO;
	cfg->brightness_step = 5;
	cfg->brightness_device = rei_xstrdup("");
	cfg->brightness_command = rei_xstrdup("");
	cfg->mpd_enabled = true;
	cfg->mpd_host = rei_xstrdup("");
	cfg->mpd_port = 6600;
	cfg->mpd_start_command = rei_xstrdup("mpd");
	cfg->mpd_announce = true;
	cfg->mpd_album_art = true;
	cfg->cursor_size = 24;
	cfg->start_label = rei_xstrdup("Start");
	cfg->start_shutdown_command = rei_xstrdup("loginctl poweroff || systemctl poweroff || poweroff");
	cfg->start_reboot_command = rei_xstrdup("loginctl reboot || systemctl reboot || reboot");
	cfg->start_logout_command = rei_xstrdup("");
	cfg->start_confirm_power = true;
	cfg->start_run_font = rei_xstrdup("Sans 26");
	cfg->messages_enabled = true;
	cfg->messages_right = true;
	cfg->messages_duration = 10.0;
	cfg->messages_max_visible = 6;
	cfg->messages_width = 480;
	cfg->messages_history = 200;
	cfg->messages_font = rei_xstrdup("Sans 12");
	cfg->messages_blur_radius = 12;
	cfg->messages_blur_dim = 0.3;
	cfg->toolbar_tray_scope = REI_TRAY_SCOPE_WORKSPACE;
	cfg->toolbar_tray_min_width = 50;
	cfg->toolbar_font = rei_xstrdup("Sans 11");
	cfg->flavor_icons = rei_xstrdup("");
	cfg->toolbar_clock_format = rei_xstrdup("%H:%M:%S");
	cfg->toolbar_background = 0x161c26;
	cfg->toolbar_foreground = 0xd6dde8;
	cfg->toolbar_highlight = 0x3c6ebe;
	rei_theme_defaults(&cfg->theme, cfg->toolbar_background, cfg->toolbar_foreground, cfg->toolbar_highlight);
	cfg->notch_height = 22;
	cfg->decoration_client_titlebars = true;
	cfg->clipboard_terminals = rei_xstrdup("URxvt, XTerm, UXTerm, rxvt, Eterm, mlterm, st, st-256color, foot, footclient, "
					       "kitty, Alacritty, org.wezfurlong.wezterm, wezterm, konsole, org.kde.konsole, "
					       "xfce4-terminal, gnome-terminal, gnome-terminal-server, org.gnome.Terminal, "
					       "terminator, tilix, lxterminal, qterminal, sakura, termite, cool-retro-term, Guake");
	cfg->clipboard_terminal_to_clipboard = true;
	cfg->repeat_rate = 25;
	cfg->repeat_delay = 600;
	cfg->tap_to_click = true;
	cfg->xwayland = true;
	cfg->xresources = rei_xstrdup("~/.Xresources");
	cfg->audio_enabled = true;
	cfg->audio_autostart = true;
	cfg->volume_step = 5;
	cfg->max_volume = 100;
	cfg->wallpaper_mode = rei_xstrdup("fill");
	cfg->background_rgb = 0x1b2733;
	cfg->root = toml_parse("", NULL, 0);
	return cfg;
}

struct rei_config *rei_config_parse(const char *text, char *err, size_t errlen)
{
	struct toml_table *root = toml_parse(text, err, errlen);
	if (!root)
		return NULL;

	struct rei_config *cfg = rei_config_defaults();
	toml_free(cfg->root);
	cfg->root = root;

	struct reader r = { .err = err, .errlen = errlen };
	if (errlen)
		err[0] = '\0';

	static const char *const backends[] = { "auto", "headless", "nested", "drm", NULL };
	static const char *const mod_keys[] = { "super", "alt", "ctrl", "shift", NULL };

	const struct toml_table *general = get_table(&r, root, "general");
	if (general) {
		read_choice(&r, general, "general.", "backend", &cfg->backend, backends);
		read_choice(&r, general, "general.", "composition_key",
			    &cfg->composition_key, mod_keys);
		read_string(&r, general, "general.", "xresources", &cfg->xresources);
		const struct toml_value *xw = get(&r, general, "general.", "xwayland", TOML_BOOL);
		if (xw)
			cfg->xwayland = xw->u.b;
		static const char *const buses[] = { "inherit", "private", NULL };
		char *bus = rei_xstrdup("inherit");
		read_choice(&r, general, "general.", "session_bus", &bus, buses);
		cfg->session_bus_private = strcmp(bus, "private") == 0;
		free(bus);
		read_int(&r, general, "general.", "display_sleep", &cfg->display_sleep_minutes, 0, 1440);
		const struct toml_value *ffm = get(&r, general, "general.", "focus_follows_mouse", TOML_BOOL);
		if (ffm)
			cfg->focus_follows_mouse = ffm->u.b;
	}

	const struct toml_table *br = get_table(&r, root, "brightness");
	if (br) {
		read_int(&r, br, "brightness.", "step", &cfg->brightness_step, 1, 50);
		read_string(&r, br, "brightness.", "device", &cfg->brightness_device);
		read_string(&r, br, "brightness.", "command", &cfg->brightness_command);
	}

	const struct toml_table *mpd = get_table(&r, root, "mpd");
	if (mpd) {
		const struct toml_value *en = get(&r, mpd, "mpd.", "enabled", TOML_BOOL);
		if (en)
			cfg->mpd_enabled = en->u.b;
		const struct toml_value *an = get(&r, mpd, "mpd.", "announce", TOML_BOOL);
		if (an)
			cfg->mpd_announce = an->u.b;
		const struct toml_value *art = get(&r, mpd, "mpd.", "album_art", TOML_BOOL);
		if (art)
			cfg->mpd_album_art = art->u.b;
		read_string(&r, mpd, "mpd.", "host", &cfg->mpd_host);
		read_int(&r, mpd, "mpd.", "port", &cfg->mpd_port, 1, 65535);
		read_string(&r, mpd, "mpd.", "start_command", &cfg->mpd_start_command);
	}

	const struct toml_table *cursor = get_table(&r, root, "cursor");
	if (cursor) {
		read_string(&r, cursor, "cursor.", "theme", &cfg->cursor_theme);
		read_int(&r, cursor, "cursor.", "size", &cfg->cursor_size, 8, 256);
	}

	const struct toml_table *layout = get_table(&r, root, "layout");
	if (layout) {
		read_int(&r, layout, "layout.", "gap_outer", &cfg->gap_outer, 0, 500);
		read_int(&r, layout, "layout.", "gap_inner", &cfg->gap_inner, 0, 500);
		static const char *const modes[] = { "tile", "float", NULL };
		char *mode = rei_xstrdup("tile");
		read_choice(&r, layout, "layout.", "new_windows", &mode, modes);
		cfg->float_new_windows = strcmp(mode, "float") == 0;
		free(mode);
	}

	const struct toml_table *input = get_table(&r, root, "input");
	if (input) {
		read_string(&r, input, "input.", "keyboard_rules", &cfg->kb_rules);
		read_string(&r, input, "input.", "keyboard_model", &cfg->kb_model);
		read_string(&r, input, "input.", "keyboard_layout", &cfg->kb_layout);
		read_string(&r, input, "input.", "keyboard_variant", &cfg->kb_variant);
		read_string(&r, input, "input.", "keyboard_options", &cfg->kb_options);
		read_int(&r, input, "input.", "repeat_rate", &cfg->repeat_rate, 0, 1000);
		read_int(&r, input, "input.", "repeat_delay", &cfg->repeat_delay, 0, 10000);
		const struct toml_value *tap = get(&r, input, "input.", "tap_to_click", TOML_BOOL);
		if (tap)
			cfg->tap_to_click = tap->u.b;
		const struct toml_value *nat = get(&r, input, "input.", "natural_scroll", TOML_BOOL);
		if (nat)
			cfg->natural_scroll = nat->u.b;
		const struct toml_value *lh = get(&r, input, "input.", "left_handed", TOML_BOOL);
		if (lh)
			cfg->left_handed = lh->u.b;
		read_double(&r, input, "input.", "pointer_accel", &cfg->pointer_accel, -1.0, 1.0);
	}

	const struct toml_table *kb = get_table(&r, root, "keybindings");
	if (kb && !r.failed) {
		cfg->bindings = rei_xcalloc(kb->len ? kb->len : 1, sizeof *cfg->bindings);
		for (size_t i = 0; i < kb->len && !r.failed; i++) {
			const struct toml_entry *e = &kb->entries[i];
			if (e->val->type != TOML_STRING) {
				fail(&r, "keybindings.\"%s\": expected a string like \"close\" or "
				     "\"exec foot\", got %s", e->key, toml_type_name(e->val->type));
				break;
			}
			/* Validate now so a typo is reported at startup, naming the entry. */
			uint32_t mods;
			xkb_keysym_t sym;
			struct rei_action action;
			char detail[200];
			if (!rei_combo_parse(e->key, cfg->composition_key, &mods, &sym, detail, sizeof detail)) {
				fail(&r, "keybindings: %s", detail);
				break;
			}
			if (!rei_action_parse(e->val->u.s, &action, detail, sizeof detail)) {
				fail(&r, "keybindings.\"%s\": %s", e->key, detail);
				break;
			}
			rei_action_free(&action);
			cfg->bindings[cfg->n_bindings].combo = rei_xstrdup(e->key);
			cfg->bindings[cfg->n_bindings].action = rei_xstrdup(e->val->u.s);
			cfg->n_bindings++;
		}
	}

	const struct toml_table *sm = get_table(&r, root, "start_menu");
	if (sm) {
		const struct toml_value *en = get(&r, sm, "start_menu.", "enabled", TOML_BOOL);
		if (en)
			cfg->start_enabled = en->u.b;
		read_string(&r, sm, "start_menu.", "label", &cfg->start_label);
		read_string(&r, sm, "start_menu.", "shutdown_command", &cfg->start_shutdown_command);
		read_string(&r, sm, "start_menu.", "reboot_command", &cfg->start_reboot_command);
		read_string(&r, sm, "start_menu.", "logout_command", &cfg->start_logout_command);
		read_string(&r, sm, "start_menu.", "run_font", &cfg->start_run_font);
		const struct toml_value *cp = get(&r, sm, "start_menu.", "confirm_power", TOML_BOOL);
		if (cp)
			cfg->start_confirm_power = cp->u.b;
		if (cfg->start_label[0] == '\0')
			fail(&r, "start_menu.label: must not be empty (use enabled = false to remove the button)");
	}
	const struct toml_table *ms = get_table(&r, root, "messages");
	if (ms) {
		const struct toml_value *en = get(&r, ms, "messages.", "enabled", TOML_BOOL);
		if (en)
			cfg->messages_enabled = en->u.b;
		static const char *const sides[] = { "right", "left", NULL };
		char *side = rei_xstrdup("right");
		read_choice(&r, ms, "messages.", "position", &side, sides);
		cfg->messages_right = strcmp(side, "left") != 0;
		free(side);
		read_double(&r, ms, "messages.", "duration", &cfg->messages_duration, 0.5, 600.0);
		read_int(&r, ms, "messages.", "max_visible", &cfg->messages_max_visible, 1, 20);
		read_int(&r, ms, "messages.", "width", &cfg->messages_width, 120, 2000);
		read_int(&r, ms, "messages.", "history", &cfg->messages_history, 1, 5000);
		read_string(&r, ms, "messages.", "font", &cfg->messages_font);
		read_int(&r, ms, "messages.", "blur_radius", &cfg->messages_blur_radius, 0, 64);
		read_double(&r, ms, "messages.", "blur_dim", &cfg->messages_blur_dim, 0.0, 0.9);
	}
	const struct toml_value *shorts = get(&r, root, "", "shortcut", TOML_ARRAY);
	if (shorts) {
		if (shorts->u.arr.len > 20)
			fail(&r, "shortcut: at most 20 shortcuts (the menu would not fit on a screen)");
		cfg->shortcuts = rei_xcalloc(shorts->u.arr.len ? shorts->u.arr.len : 1, sizeof *cfg->shortcuts);
		for (size_t i = 0; i < shorts->u.arr.len && !r.failed; i++) {
			const struct toml_value *item = shorts->u.arr.items[i];
			if (item->type != TOML_TABLE) {
				fail(&r, "shortcut[%zu]: expected a table with name and command", i);
				break;
			}
			char prefix[32];
			snprintf(prefix, sizeof prefix, "shortcut[%zu].", i);
			const struct toml_value *name = get(&r, item->u.tbl, prefix, "name", TOML_STRING);
			const struct toml_value *cmd = get(&r, item->u.tbl, prefix, "command", TOML_STRING);
			if (!name || !cmd || !name->u.s[0] || !cmd->u.s[0]) {
				fail(&r, "%sname and command are both required", prefix);
				break;
			}
			cfg->shortcuts[cfg->n_shortcuts].name = rei_xstrdup(name->u.s);
			cfg->shortcuts[cfg->n_shortcuts].command = rei_xstrdup(cmd->u.s);
			cfg->n_shortcuts++;
		}
	}

	const struct toml_table *audio = get_table(&r, root, "audio");
	if (audio) {
		const struct toml_value *en = get(&r, audio, "audio.", "enabled", TOML_BOOL);
		if (en)
			cfg->audio_enabled = en->u.b;
		const struct toml_value *au = get(&r, audio, "audio.", "autostart", TOML_BOOL);
		if (au)
			cfg->audio_autostart = au->u.b;
		read_int(&r, audio, "audio.", "volume_step", &cfg->volume_step, 1, 100);
		read_int(&r, audio, "audio.", "max_volume", &cfg->max_volume, 10, 150);
		static const char *const targets[] = { "auto", "sink", "mpd", "both", NULL };
		char *tgt = rei_xstrdup("auto");
		read_choice(&r, audio, "audio.", "volume_target", &tgt, targets);
		cfg->volume_target = !strcmp(tgt, "sink") ? REI_VOLUME_SINK : !strcmp(tgt, "mpd") ? REI_VOLUME_MPD
				   : !strcmp(tgt, "both") ? REI_VOLUME_BOTH : REI_VOLUME_AUTO;
		free(tgt);
	}

	const struct toml_table *tb = get_table(&r, root, "toolbar");
	if (tb) {
		static const char *const positions[] = { "top", "bottom", "left", "right", NULL };
		const struct toml_value *en = get(&r, tb, "toolbar.", "enabled", TOML_BOOL);
		if (en)
			cfg->toolbar_enabled = en->u.b;
		char *pos = rei_xstrdup("top");
		read_choice(&r, tb, "toolbar.", "position", &pos, positions);
		for (int i = 0; positions[i]; i++) {
			if (strcmp(pos, positions[i]) == 0)
				cfg->toolbar_position = (enum rei_bar_position)i;
		}
		free(pos);
		read_int(&r, tb, "toolbar.", "thickness", &cfg->toolbar_thickness, 16, 96);
		static const char *const trays[] = { "off", "icon", "icon_and_name", NULL };
		char *tray = rei_xstrdup("icon_and_name");
		read_choice(&r, tb, "toolbar.", "tray", &tray, trays);
		for (int i = 0; trays[i]; i++) {
			if (strcmp(tray, trays[i]) == 0)
				cfg->toolbar_tray = (enum rei_tray_mode)i;
		}
		free(tray);
		static const char *const scopes[] = { "workspace", "output", NULL };
		char *scope = rei_xstrdup("workspace");
		read_choice(&r, tb, "toolbar.", "tray_scope", &scope, scopes);
		cfg->toolbar_tray_scope = strcmp(scope, "output") == 0 ? REI_TRAY_SCOPE_OUTPUT : REI_TRAY_SCOPE_WORKSPACE;
		free(scope);
		const struct toml_value *sysi = get(&r, tb, "toolbar.", "sysinfo", TOML_BOOL);
		if (sysi)
			cfg->toolbar_sysinfo = sysi->u.b;
		const struct toml_value *bp = get(&r, tb, "toolbar.", "battery_percent", TOML_BOOL);
		if (bp)
			cfg->toolbar_battery_percent = bp->u.b;
		const struct toml_value *wf = get(&r, tb, "toolbar.", "wifi", TOML_BOOL);
		if (wf)
			cfg->toolbar_wifi = wf->u.b;
		read_string(&r, tb, "toolbar.", "wifi_command", &cfg->toolbar_wifi_command);
		const struct toml_value *bat = get(&r, tb, "toolbar.", "battery", TOML_BOOL);
		if (bat)
			cfg->toolbar_battery = bat->u.b;
		read_int(&r, tb, "toolbar.", "tray_min_width", &cfg->toolbar_tray_min_width, 10, 100);
		read_string(&r, tb, "toolbar.", "font", &cfg->toolbar_font);
		read_string(&r, tb, "toolbar.", "clock_format", &cfg->toolbar_clock_format);
		read_color(&r, tb, "toolbar.", "background", &cfg->toolbar_background);
		read_color(&r, tb, "toolbar.", "foreground", &cfg->toolbar_foreground);
		read_color(&r, tb, "toolbar.", "highlight", &cfg->toolbar_highlight);
	}

	rei_theme_defaults(&cfg->theme, cfg->toolbar_background, cfg->toolbar_foreground, cfg->toolbar_highlight);
	{
		const struct toml_value *th = toml_table_get(root, "theme");
		if (th && th->type == TOML_TABLE)
			read_string(&r, th->u.tbl, "theme.", "flavor_icons", &cfg->flavor_icons);
	}
	if (!r.failed) {
		char theme_err[300];
		if (!rei_theme_read(&cfg->theme, root, theme_err, sizeof theme_err))
			fail(&r, "%s", theme_err);
	}

	const struct toml_table *clip = get_table(&r, root, "clipboard");
	if (clip) {
		read_string(&r, clip, "clipboard.", "terminals", &cfg->clipboard_terminals);
		const struct toml_value *t2c = get(&r, clip, "clipboard.", "terminal_selection_to_clipboard", TOML_BOOL);
		if (t2c)
			cfg->clipboard_terminal_to_clipboard = t2c->u.b;
	}

	const struct toml_table *deco = get_table(&r, root, "decoration");
	if (deco) {
		read_int(&r, deco, "decoration.", "notch_height", &cfg->notch_height, 0, 64);
		const struct toml_value *ctb = get(&r, deco, "decoration.", "client_titlebars", TOML_BOOL);
		if (ctb)
			cfg->decoration_client_titlebars = ctb->u.b;
	}

	const struct toml_table *wp = get_table(&r, root, "wallpaper");
	if (wp) {
		static const char *const modes[] = { "fill", "fit", "stretch", "center", "tile", NULL };
		const struct toml_value *path = get(&r, wp, "wallpaper.", "path", TOML_STRING);
		if (path && path->u.s[0])
			cfg->wallpaper_path = expand_home(path->u.s);
		read_choice(&r, wp, "wallpaper.", "mode", &cfg->wallpaper_mode, modes);
		const struct toml_value *color = get(&r, wp, "wallpaper.", "color", TOML_STRING);
		if (color && !parse_color(color->u.s, &cfg->background_rgb))
			fail(&r, "wallpaper.color: '%s' is not a colour like \"#1b2733\"", color->u.s);
	}

	const struct toml_value *groups = get(&r, root, "", "wallpaper_group", TOML_ARRAY);
	if (groups) {
		cfg->wallpaper_groups = rei_xcalloc(groups->u.arr.len, sizeof *cfg->wallpaper_groups);
		cfg->n_wallpaper_groups = groups->u.arr.len;
		int any_count = 0;
		for (size_t i = 0; i < groups->u.arr.len && !r.failed; i++) {
			const struct toml_value *item = groups->u.arr.items[i];
			if (item->type != TOML_TABLE) {
				fail(&r, "wallpaper_group[%zu]: expected table", i);
				break;
			}
			struct rei_wallpaper_group *g = &cfg->wallpaper_groups[i];
			char prefix[48];
			snprintf(prefix, sizeof prefix, "wallpaper_group[%zu].", i);
			g->interval = 600;
			read_string(&r, item->u.tbl, prefix, "name", &g->name);
			read_int(&r, item->u.tbl, prefix, "interval", &g->interval, 5, 86400);
			const struct toml_value *any = get(&r, item->u.tbl, prefix, "any", TOML_BOOL);
			g->any = any && any->u.b;
			any_count += g->any;
			const struct toml_value *paths = toml_table_get(item->u.tbl, "paths");
			const struct toml_value *one = toml_table_get(item->u.tbl, "path");   /* a single one may be given as `path` */
			if (paths && paths->type != TOML_ARRAY)
				fail(&r, "%spaths: expected an array of strings", prefix);
			else if (paths) {
				g->paths = rei_xcalloc(paths->u.arr.len + 1, sizeof *g->paths);
				for (size_t k = 0; k < paths->u.arr.len && !r.failed; k++) {
					if (paths->u.arr.items[k]->type != TOML_STRING)
						fail(&r, "%spaths: expected an array of strings", prefix);
					else if (paths->u.arr.items[k]->u.s[0])
						g->paths[g->n_paths++] = expand_home(paths->u.arr.items[k]->u.s);
				}
			}
			if (one && !r.failed) {
				if (one->type != TOML_STRING)
					fail(&r, "%spath: expected a string", prefix);
				else if (one->u.s[0]) {
					g->paths = rei_xrealloc(g->paths, (g->n_paths + 2) * sizeof *g->paths);
					g->paths[g->n_paths++] = expand_home(one->u.s);
				}
			}
			const struct toml_value *ws = toml_table_get(item->u.tbl, "workspaces");
			if (ws && ws->type != TOML_ARRAY)
				fail(&r, "%sworkspaces: expected an array of workspace numbers", prefix);
			else if (ws) {
				g->workspaces = rei_xcalloc(ws->u.arr.len + 1, sizeof *g->workspaces);
				for (size_t k = 0; k < ws->u.arr.len && !r.failed; k++) {
					const struct toml_value *n = ws->u.arr.items[k];
					if (n->type != TOML_INT || n->u.i < 1 || n->u.i > 99)
						fail(&r, "%sworkspaces: workspace numbers are from 1 to 99", prefix);
					else
						g->workspaces[g->n_workspaces++] = (int32_t)n->u.i;
				}
			}
			if (!g->name)
				g->name = rei_xstrdup("");
		}
		if (!r.failed && any_count > 1)
			fail(&r, "wallpaper_group: only one group can be the \"any\" group (any = true), not %d", any_count);
		/* A workspace shows one group. */
		for (size_t i = 0; i < cfg->n_wallpaper_groups && !r.failed; i++)
			for (size_t k = 0; k < cfg->wallpaper_groups[i].n_workspaces && !r.failed; k++)
				for (size_t j = i + 1; j < cfg->n_wallpaper_groups; j++)
					for (size_t m = 0; m < cfg->wallpaper_groups[j].n_workspaces; m++)
						if (cfg->wallpaper_groups[i].workspaces[k] == cfg->wallpaper_groups[j].workspaces[m])
							fail(&r, "wallpaper_group: workspace %d is given to two groups (\"%s\" and \"%s\")",
							     (int)cfg->wallpaper_groups[i].workspaces[k], cfg->wallpaper_groups[i].name,
							     cfg->wallpaper_groups[j].name);
	}

	const struct toml_value *outs = get(&r, root, "", "output", TOML_ARRAY);
	if (outs) {
		cfg->outputs = rei_xcalloc(outs->u.arr.len, sizeof *cfg->outputs);
		cfg->n_outputs = outs->u.arr.len;
		for (size_t i = 0; i < outs->u.arr.len && !r.failed; i++) {
			const struct toml_value *item = outs->u.arr.items[i];
			if (item->type != TOML_TABLE) {
				fail(&r, "output[%zu]: expected table", i);
				break;
			}
			read_output(&r, item->u.tbl, i, &cfg->outputs[i]);
		}
	}

	if (r.failed) {
		rei_config_free(cfg);
		return NULL;
	}
	return cfg;
}

char *rei_config_default_path(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *path = NULL;

	if (xdg && xdg[0] == '/') {
		if (asprintf(&path, "%s/rei-wm/config.toml", xdg) < 0)
			return NULL;
	} else if (home && home[0]) {
		if (asprintf(&path, "%s/.config/rei-wm/config.toml", home) < 0)
			return NULL;
	}
	return path;
}

static bool mkdir_p(const char *path)
{
	char *copy = rei_xstrdup(path);
	bool ok = true;
	for (char *p = copy + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(copy, 0755) < 0 && errno != EEXIST)
			ok = false;
		*p = '/';
	}
	if (ok && mkdir(copy, 0755) < 0 && errno != EEXIST)
		ok = false;
	free(copy);
	return ok;
}

static bool write_default(const char *path)
{
	char *dir = rei_xstrdup(path);
	char *slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		if (!mkdir_p(dir)) {
			free(dir);
			return false;
		}
	}
	free(dir);

	/* O_EXCL semantics via "wx": never clobber a file that appeared meanwhile. */
	FILE *f = fopen(path, "wx");
	if (!f)
		return false;
	const char *text = rei_config_default_text();
	bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
	ok = (fclose(f) == 0) && ok;
	return ok;
}

static char *read_file(const char *path, int *err_no)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		*err_no = errno;
		return NULL;
	}
	char *buf = NULL;
	size_t len = 0, cap = 0;
	for (;;) {
		if (len + 4096 + 1 > cap) {
			cap = cap ? cap * 2 : 8192;
			buf = rei_xrealloc(buf, cap);
		}
		size_t n = fread(buf + len, 1, cap - len - 1, f);
		len += n;
		if (n == 0)
			break;
	}
	bool bad = ferror(f);
	fclose(f);
	if (bad) {
		free(buf);
		*err_no = EIO;
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

struct rei_config *rei_config_load(const char *path, bool create_if_missing,
				   char *err, size_t errlen)
{
	int e = 0;
	char *text = read_file(path, &e);

	if (!text && e == ENOENT && create_if_missing) {
		if (write_default(path))
			rei_info("wrote default config to %s", path);
		else
			rei_warn("could not create %s (%s); using built-in defaults",
				 path, strerror(errno));
		struct rei_config *cfg = rei_config_defaults();
		cfg->path = rei_xstrdup(path);
		return cfg;
	}
	if (!text) {
		snprintf(err, errlen, "cannot read %s: %s", path, strerror(e));
		return NULL;
	}

	struct rei_config *cfg = rei_config_parse(text, err, errlen);
	free(text);
	if (!cfg) {
		/* Prefix with the path so the message is actionable. */
		char detail[512];
		snprintf(detail, sizeof detail, "%s", err);
		snprintf(err, errlen, "%s: %s", path, detail);
		return NULL;
	}
	cfg->path = rei_xstrdup(path);
	return cfg;
}

void rei_config_free(struct rei_config *cfg)
{
	if (!cfg)
		return;
	free(cfg->path);
	free(cfg->backend);
	free(cfg->composition_key);
	free(cfg->toolbar_font);
	free(cfg->flavor_icons);
	free(cfg->start_label);
	free(cfg->start_shutdown_command);
	free(cfg->start_reboot_command);
	free(cfg->start_logout_command);
	free(cfg->xresources);
	free(cfg->cursor_theme);
	free(cfg->toolbar_wifi_command);
	free(cfg->brightness_device);
	free(cfg->brightness_command);
	free(cfg->mpd_host);
	free(cfg->clipboard_terminals);
	free(cfg->mpd_start_command);
	free(cfg->start_run_font);
	free(cfg->messages_font);
	for (size_t i = 0; i < cfg->n_shortcuts; i++) {
		free(cfg->shortcuts[i].name);
		free(cfg->shortcuts[i].command);
	}
	free(cfg->shortcuts);
	free(cfg->toolbar_clock_format);
	free(cfg->kb_rules);
	free(cfg->kb_model);
	free(cfg->kb_layout);
	free(cfg->kb_variant);
	free(cfg->kb_options);
	for (size_t i = 0; i < cfg->n_bindings; i++) {
		free(cfg->bindings[i].combo);
		free(cfg->bindings[i].action);
	}
	free(cfg->bindings);
	for (size_t i = 0; i < cfg->n_wallpaper_groups; i++) {
		struct rei_wallpaper_group *g = &cfg->wallpaper_groups[i];
		free(g->name);
		for (size_t k = 0; k < g->n_paths; k++)
			free(g->paths[k]);
		free(g->paths);
		free(g->workspaces);
	}
	free(cfg->wallpaper_groups);
	free(cfg->wallpaper_path);
	free(cfg->wallpaper_mode);
	for (size_t i = 0; i < cfg->n_outputs; i++)
		free(cfg->outputs[i].name);
	free(cfg->outputs);
	toml_free(cfg->root);
	free(cfg);
}
