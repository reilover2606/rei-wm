#include "winfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "server.h"
#include "shader.h"
#include "theme.h"
#include "tray.h"
#include "util/log.h"
#include "util/util.h"

struct rule {
	char app[128];   /* "" or "*": every window */
	struct rei_fx fx;
};

struct rei_winfx {
	struct rule rules[REI_MAX_WINDOW_SHADERS];   /* from the command line */
	int n_rules;
	bool theme_on;
	struct rei_fx theme_fx;
};

void rei_winfx_configure(struct rei_server *s)
{
	if (!s->winfx)
		s->winfx = rei_xcalloc(1, sizeof *s->winfx);
	struct rei_winfx *w = s->winfx;
	const struct rei_theme *theme = &s->config->theme;
	struct rei_text_fx glow = theme->e[REI_ELEM_WINDOW_TEXT].text_fx;
	char err[400];

	/* The theme's rule. */
	w->theme_on = false;
	const struct rei_shader *sh = rei_shader_named(theme->window_shader, err, sizeof err);
	if (!sh)
		rei_warn("theme.window_text.shader: %s", err);
	else {
		rei_fx_for_window(&glow, theme->window_threshold, theme->window_area_limit, sh, &w->theme_fx);
		w->theme_on = true;
	}

	/* The command line's. A built-in glow asked for there glows even if the theme has it at size 0. */
	if (glow.glow_size <= 0)
		glow.glow_size = 5;
	if (glow.glow_strength <= 0)
		glow.glow_strength = 1;
	w->n_rules = 0;
	for (int i = 0; i < s->n_window_shaders && w->n_rules < REI_MAX_WINDOW_SHADERS; i++) {
		const char *spec = s->window_shaders[i];
		/* "SHADER:APP": the application comes after the last colon (a path has none after its name). */
		const char *colon = s->window_shader_is_rule[i] ? strrchr(spec, ':') : NULL;
		char name[512];
		snprintf(name, sizeof name, "%.*s", colon ? (int)(colon - spec) : (int)strlen(spec), spec);
		sh = rei_shader_named(name, err, sizeof err);
		if (!sh) {
			rei_warn("--window-shader-fg: %s", err);
			continue;
		}
		struct rule *r = &w->rules[w->n_rules++];
		snprintf(r->app, sizeof r->app, "%s", colon ? colon + 1 : "");
		rei_fx_for_window(&glow, theme->window_threshold, theme->window_area_limit, sh, &r->fx);
		rei_info("window shader '%s' for %s", rei_shader_name(sh), r->app[0] ? r->app : "every window");
	}
}

void rei_winfx_finish(struct rei_server *s)
{
	free(s->winfx);
	s->winfx = NULL;
}

const struct rei_fx *rei_winfx_for(struct rei_server *s, const struct rei_view *view)
{
	struct rei_winfx *w = s->winfx;
	if (!w)
		return NULL;
	const char *app = rei_view_app_id(view);
	for (int i = 0; i < w->n_rules; i++) {
		const struct rule *r = &w->rules[i];
		if (!r->app[0] || !strcmp(r->app, "*") || strcasecmp(r->app, app) == 0)
			return &r->fx;
	}
	if (w->theme_on && rei_theme_window_matches(&s->config->theme, app))
		return &w->theme_fx;
	return NULL;
}
