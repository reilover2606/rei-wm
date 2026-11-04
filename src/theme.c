#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "util/toml.h"
#include "util/util.h"

static const char *const element_names[REI_ELEM_COUNT] = {
	"toolbar", "workspace_button", "tray_tile", "tray_overflow", "tooltip", "menu", "menu_row", "start_button", "run_prompt", "dialog", "battery",
};
static const char *const shape_names[] = { "square", "rounded", "pill", "bevel" };

const char *rei_element_name(enum rei_element e)
{
	return e < REI_ELEM_COUNT ? element_names[e] : "?";
}

const char *rei_shape_name(enum rei_shape s)
{
	return (unsigned)s < REI_ARRAY_LEN(shape_names) ? shape_names[s] : "?";
}

int rei_shape_parse(const char *name)
{
	for (size_t i = 0; i < REI_ARRAY_LEN(shape_names); i++)
		if (strcmp(name, shape_names[i]) == 0)
			return (int)i;
	return -1;
}

uint32_t rei_color_mix(uint32_t a, uint32_t b, double t)
{
	uint32_t out = 0;
	for (int shift = 16; shift >= 0; shift -= 8) {
		double ca = (double)((a >> shift) & 0xff), cb = (double)((b >> shift) & 0xff);
		long v = lround(ca + (cb - ca) * t);
		out |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << shift;
	}
	return out;
}

void rei_theme_defaults(struct rei_theme *t, uint32_t bg, uint32_t fg, uint32_t hi)
{
	memset(t, 0, sizeof *t);
	for (int i = 0; i < REI_ELEM_COUNT; i++) {
		struct rei_style *s = &t->e[i];
		s->color = bg;
		s->active_color = hi;
		s->inactive_color = rei_color_mix(bg, fg, 0.06);
		s->text_color = s->active_text_color = fg;
		s->inactive_text_color = rei_color_mix(bg, fg, 0.55);
		s->border_color = rei_color_mix(bg, fg, 0.30);
		s->shape = REI_SHAPE_SQUARE;
		s->opacity = 1.0f;
	}

	/* The workspace buttons: no fill unless something is going on; "inactive" is a workspace shown
	 * on a monitor that is not the focused one. */
	struct rei_style *ws = &t->e[REI_ELEM_WORKSPACE_BUTTON];
	ws->inactive_color = rei_color_mix(bg, hi, 0.45);
	ws->shape = REI_SHAPE_ROUNDED;
	ws->radius = 4;

	for (int i = REI_ELEM_TRAY_TILE; i <= REI_ELEM_TRAY_OVERFLOW; i++) {
		struct rei_style *s = &t->e[i];
		s->color = rei_color_mix(bg, fg, 0.13);
		s->inactive_color = rei_color_mix(bg, fg, 0.06);
		s->shape = REI_SHAPE_ROUNDED;
		s->radius = 4;
	}

	struct rei_style *tip = &t->e[REI_ELEM_TOOLTIP];
	tip->color = rei_color_mix(bg, fg, 0.10);
	tip->shape = REI_SHAPE_ROUNDED;
	tip->radius = 4;
	tip->opacity = 0.97f;
	tip->border_width = 1;
	tip->shadow = (struct rei_shadow){ 0x000000, 0.35f, 0, 2, 6 };

	struct rei_style *menu = &t->e[REI_ELEM_MENU];
	menu->border_width = 1;
	menu->shape = REI_SHAPE_SQUARE;
	menu->shadow = (struct rei_shadow){ 0x000000, 0.30f, 0, 2, 6 };
	/* Menu rows have no fill until hovered. */

	struct rei_style *start = &t->e[REI_ELEM_START_BUTTON];
	start->color = rei_color_mix(bg, hi, 0.55);
	start->shape = REI_SHAPE_ROUNDED;
	start->radius = 4;

	/* The Run prompt: a see-through box, so what is behind it stays faintly visible. */
	struct rei_style *run = &t->e[REI_ELEM_RUN_PROMPT];
	run->color = rei_color_mix(bg, 0x000000, 0.25);
	run->opacity = 0.80f;
	run->shape = REI_SHAPE_ROUNDED;
	run->radius = 14;
	run->border_width = 1;
	run->shadow = (struct rei_shadow){ 0x000000, 0.45f, 0, 4, 18 };

	/* Confirmation dialogs: like the menus, but rounder and more opaque. */
	struct rei_style *dlg = &t->e[REI_ELEM_DIALOG];
	*dlg = t->e[REI_ELEM_MENU];
	dlg->opacity = 0.97f;
	dlg->shape = REI_SHAPE_ROUNDED;
	dlg->radius = 12;
	dlg->border_width = 1;
	dlg->shadow = (struct rei_shadow){ 0x000000, 0.5f, 0, 6, 24 };

	/* The battery icon: its level fill, a green one while charging, a red one when low. The outline
	 * is the border. */
	struct rei_style *bat = &t->e[REI_ELEM_BATTERY];
	bat->color = rei_color_mix(bg, fg, 0.9);
	bat->active_color = 0x6bc46d;
	bat->inactive_color = 0xe5534b;
	bat->border_color = rei_color_mix(bg, fg, 0.75);
	bat->border_width = 1.2f;
	bat->shape = REI_SHAPE_ROUNDED;
	bat->radius = 2.5f;
}

/* ------------------------------------------------------------------------ */

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

static bool number(const struct toml_value *v, double *out)
{
	if (v->type == TOML_FLOAT) {
		*out = v->u.d;
		return true;
	}
	if (v->type == TOML_INT) {
		*out = (double)v->u.i;
		return true;
	}
	return false;
}

bool rei_theme_read(struct rei_theme *t, const struct toml_table *root, char *err, size_t errlen)
{
	const struct toml_value *theme = toml_table_get(root, "theme");
	if (!theme)
		return true;
	if (theme->type != TOML_TABLE) {
		snprintf(err, errlen, "theme: expected a table");
		return false;
	}

	for (size_t i = 0; i < theme->u.tbl->len; i++) {
		const struct toml_entry *el = &theme->u.tbl->entries[i];
		int idx = -1;
		for (int e = 0; e < REI_ELEM_COUNT; e++)
			if (strcmp(el->key, element_names[e]) == 0)
				idx = e;
		if (idx < 0) {
			snprintf(err, errlen, "theme.%s: unknown element (known: toolbar, workspace_button, "
				 "tray_tile, tray_overflow, tooltip, menu, menu_row, start_button, run_prompt, dialog, battery)", el->key);
			return false;
		}
		if (el->val->type != TOML_TABLE) {
			snprintf(err, errlen, "theme.%s: expected a table of properties", el->key);
			return false;
		}
		struct rei_style *st = &t->e[idx];

		for (size_t k = 0; k < el->val->u.tbl->len; k++) {
			const struct toml_entry *kv = &el->val->u.tbl->entries[k];
			const char *key = kv->key;
			const struct toml_value *v = kv->val;
			char where[96];
			snprintf(where, sizeof where, "theme.%s.%s", el->key, key);

			struct { const char *name; uint32_t *dst; } colors[] = {
				{ "color", &st->color }, { "active_color", &st->active_color },
				{ "inactive_color", &st->inactive_color }, { "text_color", &st->text_color },
				{ "active_text_color", &st->active_text_color },
				{ "inactive_text_color", &st->inactive_text_color },
				{ "border_color", &st->border_color }, { "shadow_color", &st->shadow.color },
			};
			bool done = false;
			for (size_t c = 0; c < REI_ARRAY_LEN(colors) && !done; c++) {
				if (strcmp(key, colors[c].name) != 0)
					continue;
				done = true;
				if (v->type != TOML_STRING || !parse_color(v->u.s, colors[c].dst)) {
					snprintf(err, errlen, "%s: expected a colour like \"#1b2733\"", where);
					return false;
				}
			}
			if (done)
				continue;

			struct { const char *name; float *dst; double lo, hi; } nums[] = {
				{ "border_width", &st->border_width, 0, 20 }, { "radius", &st->radius, 0, 100 },
				{ "opacity", &st->opacity, 0, 1 }, { "shadow_opacity", &st->shadow.opacity, 0, 1 },
				{ "shadow_offset_x", &st->shadow.dx, -50, 50 },
				{ "shadow_offset_y", &st->shadow.dy, -50, 50 }, { "shadow_blur", &st->shadow.blur, 0, 50 },
			};
			for (size_t n = 0; n < REI_ARRAY_LEN(nums) && !done; n++) {
				if (strcmp(key, nums[n].name) != 0)
					continue;
				done = true;
				double d;
				if (!number(v, &d)) {
					snprintf(err, errlen, "%s: expected a number", where);
					return false;
				}
				if (d < nums[n].lo || d > nums[n].hi) {
					snprintf(err, errlen, "%s: %g is out of range (%g to %g)", where, d,
						 nums[n].lo, nums[n].hi);
					return false;
				}
				*nums[n].dst = (float)d;
			}
			if (done)
				continue;

			if (strcmp(key, "shape") == 0) {
				int shape = v->type == TOML_STRING ? rei_shape_parse(v->u.s) : -1;
				if (shape < 0) {
					snprintf(err, errlen, "%s: expected one of: square, rounded, pill, bevel", where);
					return false;
				}
				st->shape = (enum rei_shape)shape;
				continue;
			}
			snprintf(err, errlen, "%s: unknown property", where);
			return false;
		}
	}
	return true;
}
