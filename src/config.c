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
	cfg->toolbar_tray_scope = REI_TRAY_SCOPE_WORKSPACE;
	cfg->toolbar_tray_min_width = 50;
	cfg->toolbar_font = rei_xstrdup("Sans 11");
	cfg->toolbar_clock_format = rei_xstrdup("%H:%M:%S");
	cfg->toolbar_background = 0x161c26;
	cfg->toolbar_foreground = 0xd6dde8;
	cfg->toolbar_highlight = 0x3c6ebe;
	rei_theme_defaults(&cfg->theme, cfg->toolbar_background, cfg->toolbar_foreground, cfg->toolbar_highlight);
	cfg->notch_height = 22;
	cfg->repeat_rate = 25;
	cfg->repeat_delay = 600;
	cfg->tap_to_click = true;
	cfg->xwayland = true;
	cfg->audio_enabled = true;
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
		const struct toml_value *xw = get(&r, general, "general.", "xwayland", TOML_BOOL);
		if (xw)
			cfg->xwayland = xw->u.b;
		const struct toml_value *ffm = get(&r, general, "general.", "focus_follows_mouse", TOML_BOOL);
		if (ffm)
			cfg->focus_follows_mouse = ffm->u.b;
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

	const struct toml_table *audio = get_table(&r, root, "audio");
	if (audio) {
		const struct toml_value *en = get(&r, audio, "audio.", "enabled", TOML_BOOL);
		if (en)
			cfg->audio_enabled = en->u.b;
		read_int(&r, audio, "audio.", "volume_step", &cfg->volume_step, 1, 100);
		read_int(&r, audio, "audio.", "max_volume", &cfg->max_volume, 10, 150);
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
	if (!r.failed) {
		char theme_err[300];
		if (!rei_theme_read(&cfg->theme, root, theme_err, sizeof theme_err))
			fail(&r, "%s", theme_err);
	}

	const struct toml_table *deco = get_table(&r, root, "decoration");
	if (deco)
		read_int(&r, deco, "decoration.", "notch_height", &cfg->notch_height, 0, 64);

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
	free(cfg->wallpaper_path);
	free(cfg->wallpaper_mode);
	for (size_t i = 0; i < cfg->n_outputs; i++)
		free(cfg->outputs[i].name);
	free(cfg->outputs);
	toml_free(cfg->root);
	free(cfg);
}
