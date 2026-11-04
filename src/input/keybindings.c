#include "input/keybindings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "input/xkb.h"
#include "util/util.h"

/* Keep in step with the [keybindings] section of config_default.c (a test checks
 * that they match). */
const struct rei_default_binding rei_default_bindings[] = {
	{ "Mod+Return", "exec ${TERMINAL:-foot}" },
	{ "Mod+Shift+q", "close" },
	{ "Mod+Shift+e", "quit" },
	{ "Mod+Tab", "focus_next" },
	{ "Mod+Shift+Tab", "focus_prev" },
	{ "Mod+h", "focus left" },
	{ "Mod+j", "focus down" },
	{ "Mod+k", "focus up" },
	{ "Mod+l", "focus right" },
	{ "Mod+Shift+h", "move left" },
	{ "Mod+Shift+j", "move down" },
	{ "Mod+Shift+k", "move up" },
	{ "Mod+Shift+l", "move right" },
	{ "Mod+b", "split h" },
	{ "Mod+v", "split v" },
	{ "Mod+Shift+space", "toggle_floating" },
	{ "Mod+f", "fullscreen" },
	{ "Mod+m", "minimize" },
	{ "Mod+r", "run" },
	{ "Mod+1", "workspace 1" },
	{ "Mod+2", "workspace 2" },
	{ "Mod+3", "workspace 3" },
	{ "Mod+4", "workspace 4" },
	{ "Mod+5", "workspace 5" },
	{ "Mod+6", "workspace 6" },
	{ "Mod+7", "workspace 7" },
	{ "Mod+8", "workspace 8" },
	{ "Mod+9", "workspace 9" },
	{ "Mod+0", "workspace 10" },
	{ "Mod+Shift+1", "move_to_workspace 1" },
	{ "Mod+Shift+2", "move_to_workspace 2" },
	{ "Mod+Shift+3", "move_to_workspace 3" },
	{ "Mod+Shift+4", "move_to_workspace 4" },
	{ "Mod+Shift+5", "move_to_workspace 5" },
	{ "Mod+Shift+6", "move_to_workspace 6" },
	{ "Mod+Shift+7", "move_to_workspace 7" },
	{ "Mod+Shift+8", "move_to_workspace 8" },
	{ "Mod+Shift+9", "move_to_workspace 9" },
	{ "Mod+Shift+0", "move_to_workspace 10" },
	{ "XF86AudioRaiseVolume", "volume_up" },
	{ "XF86AudioLowerVolume", "volume_down" },
	{ "XF86AudioMute", "volume_mute" },
	{ NULL, NULL },
};

uint32_t rei_composition_mod(const char *name)
{
	if (!strcmp(name, "alt")) return REI_MOD_ALT;
	if (!strcmp(name, "ctrl")) return REI_MOD_CTRL;
	if (!strcmp(name, "shift")) return REI_MOD_SHIFT;
	return REI_MOD_LOGO;  /* "super" */
}

bool rei_combo_parse(const char *text, const char *composition_key,
		     uint32_t *mods_out, xkb_keysym_t *sym_out, char *err, size_t errlen)
{
	char *copy = rei_xstrdup(text);
	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	bool ok = false;

	/* Split on '+'. A trailing '+' would be the plus key itself, which can
	 * be bound by name ("plus"), so empty tokens are an error. */
	char *save = NULL;
	char *tokens[16];
	size_t n = 0;
	for (char *t = strtok_r(copy, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
		if (n == 16) {
			snprintf(err, errlen, "'%s': too many parts", text);
			goto out;
		}
		while (*t == ' ')
			t++;
		size_t len = strlen(t);
		while (len && t[len - 1] == ' ')
			t[--len] = '\0';
		tokens[n++] = t;
	}
	if (n == 0 || text[0] == '+' || text[strlen(text) - 1] == '+' || strstr(text, "++")) {
		snprintf(err, errlen, "'%s': expected modifiers and a key joined by '+'", text);
		goto out;
	}

	for (size_t i = 0; i + 1 < n; i++) {
		const char *m = tokens[i];
		if (!strcasecmp(m, "mod"))
			mods |= rei_composition_mod(composition_key);
		else if (!strcasecmp(m, "super") || !strcasecmp(m, "logo"))
			mods |= REI_MOD_LOGO;
		else if (!strcasecmp(m, "alt"))
			mods |= REI_MOD_ALT;
		else if (!strcasecmp(m, "ctrl") || !strcasecmp(m, "control"))
			mods |= REI_MOD_CTRL;
		else if (!strcasecmp(m, "shift"))
			mods |= REI_MOD_SHIFT;
		else {
			snprintf(err, errlen, "'%s': unknown modifier '%s' "
				 "(use Mod, Super, Alt, Ctrl or Shift)", text, m);
			goto out;
		}
	}

	const char *key = tokens[n - 1];
	sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) {
		snprintf(err, errlen, "'%s': unknown key '%s'", text, key);
		goto out;
	}
	/* Compare case-insensitively: "Mod+Shift+Q" and "...+q" are the same key. */
	*sym_out = xkb_keysym_to_lower(sym);
	*mods_out = mods;
	ok = true;
out:
	free(copy);
	return ok;
}

/* What follows the action's name. */
enum arg_kind { ARG_NONE, ARG_TEXT, ARG_DIR, ARG_SPLIT, ARG_WORKSPACE, ARG_OPT_PERCENT };

bool rei_action_parse(const char *text, struct rei_action *out, char *err, size_t errlen)
{
	while (*text == ' ')
		text++;
	const char *space = strchr(text, ' ');
	size_t name_len = space ? (size_t)(space - text) : strlen(text);
	const char *arg = space ? space + 1 : "";
	while (*arg == ' ')
		arg++;

	static const struct {
		const char *name;
		enum rei_action_type type;
		enum arg_kind kind;
	} actions[] = {
		{ "none", REI_ACTION_NONE, ARG_NONE },
		{ "exec", REI_ACTION_EXEC, ARG_TEXT },
		{ "close", REI_ACTION_CLOSE, ARG_NONE },
		{ "quit", REI_ACTION_QUIT, ARG_NONE },
		{ "focus_next", REI_ACTION_FOCUS_NEXT, ARG_NONE },
		{ "focus_prev", REI_ACTION_FOCUS_PREV, ARG_NONE },
		{ "focus", REI_ACTION_FOCUS_DIR, ARG_DIR },
		{ "move", REI_ACTION_MOVE_DIR, ARG_DIR },
		{ "split", REI_ACTION_SPLIT, ARG_SPLIT },
		{ "toggle_floating", REI_ACTION_TOGGLE_FLOATING, ARG_NONE },
		{ "fullscreen", REI_ACTION_FULLSCREEN, ARG_NONE },
		{ "minimize", REI_ACTION_MINIMIZE, ARG_NONE },
		{ "run", REI_ACTION_RUN, ARG_NONE },
		{ "workspace", REI_ACTION_WORKSPACE, ARG_WORKSPACE },
		{ "move_to_workspace", REI_ACTION_MOVE_TO_WORKSPACE, ARG_WORKSPACE },
		{ "volume_up", REI_ACTION_VOLUME_UP, ARG_OPT_PERCENT },
		{ "volume_down", REI_ACTION_VOLUME_DOWN, ARG_OPT_PERCENT },
		{ "volume_mute", REI_ACTION_VOLUME_MUTE, ARG_NONE },
	};

	for (size_t i = 0; i < REI_ARRAY_LEN(actions); i++) {
		if (strlen(actions[i].name) != name_len || strncmp(actions[i].name, text, name_len))
			continue;

		out->type = actions[i].type;
		out->arg = NULL;
		out->value = 0;
		const char *name = actions[i].name;

		switch (actions[i].kind) {
		case ARG_NONE:
			if (arg[0]) {
				snprintf(err, errlen, "'%s' takes no argument", name);
				return false;
			}
			return true;
		case ARG_TEXT:
			if (!arg[0]) {
				snprintf(err, errlen, "'%s' needs a command, e.g. \"exec foot\"", name);
				return false;
			}
			out->arg = rei_xstrdup(arg);
			return true;
		case ARG_DIR:
			if (!strcmp(arg, "left")) out->value = REI_DIR_LEFT;
			else if (!strcmp(arg, "right")) out->value = REI_DIR_RIGHT;
			else if (!strcmp(arg, "up")) out->value = REI_DIR_UP;
			else if (!strcmp(arg, "down")) out->value = REI_DIR_DOWN;
			else {
				snprintf(err, errlen, "'%s' needs a direction: left, right, up or down", name);
				return false;
			}
			return true;
		case ARG_SPLIT:
			if (!strcmp(arg, "h") || !strcmp(arg, "horizontal")) out->value = REI_LAYOUT_SPLIT_H;
			else if (!strcmp(arg, "v") || !strcmp(arg, "vertical")) out->value = REI_LAYOUT_SPLIT_V;
			else {
				snprintf(err, errlen, "'split' needs h (side by side) or v (stacked)");
				return false;
			}
			return true;
		case ARG_OPT_PERCENT: {
			char *end = NULL;
			long n = arg[0] ? strtol(arg, &end, 10) : 0;
			if (arg[0] && (*end || n < 1 || n > 100)) {
				snprintf(err, errlen, "'%s' takes an optional step from 1 to 100 (percent)", name);
				return false;
			}
			out->value = (int)n;
			return true;
		}
		case ARG_WORKSPACE: {
			char *end = NULL;
			long n = strtol(arg, &end, 10);
			if (!arg[0] || *end || n < 1 || n > 99) {
				snprintf(err, errlen, "'%s' needs a workspace number from 1 to 99", name);
				return false;
			}
			out->value = (int)n;
			return true;
		}
		}
	}

	snprintf(err, errlen, "unknown action '%.*s' (known: none, exec, close, quit, focus_next, "
		 "focus_prev, focus, move, split, toggle_floating, fullscreen, minimize, run, workspace, "
		 "move_to_workspace, volume_up, volume_down, volume_mute)", (int)name_len, text);
	return false;
}

void rei_action_free(struct rei_action *a)
{
	free(a->arg);
	a->arg = NULL;
}

static void add_binding(struct rei_bindings *b, uint32_t mods, xkb_keysym_t sym,
			struct rei_action action)
{
	/* Replace an existing binding for the same key. */
	for (size_t i = 0; i < b->n; i++) {
		if (b->items[i].mods == mods && b->items[i].sym == sym) {
			rei_action_free(&b->items[i].action);
			b->items[i].action = action;
			return;
		}
	}
	b->items = rei_xrealloc(b->items, (b->n + 1) * sizeof *b->items);
	b->items[b->n++] = (struct rei_binding){ mods, sym, action };
}

static bool add_text(struct rei_bindings *b, const char *composition_key,
		     const char *combo, const char *action_text, char *err, size_t errlen)
{
	uint32_t mods;
	xkb_keysym_t sym;
	struct rei_action action;
	char detail[256];

	if (!rei_combo_parse(combo, composition_key, &mods, &sym, detail, sizeof detail) ||
	    !rei_action_parse(action_text, &action, detail, sizeof detail)) {
		snprintf(err, errlen, "keybindings: %s", detail);
		return false;
	}
	add_binding(b, mods, sym, action);
	return true;
}

struct rei_bindings *rei_bindings_from_config(const struct rei_config *cfg, char *err, size_t errlen)
{
	struct rei_bindings *b = rei_xcalloc(1, sizeof *b);

	for (const struct rei_default_binding *d = rei_default_bindings; d->combo; d++) {
		if (!add_text(b, cfg->composition_key, d->combo, d->action, err, errlen))
			goto fail;
	}
	for (size_t i = 0; i < cfg->n_bindings; i++) {
		if (!add_text(b, cfg->composition_key, cfg->bindings[i].combo,
			      cfg->bindings[i].action, err, errlen))
			goto fail;
	}

	/* "none" entries only existed to replace something; drop them. */
	size_t kept = 0;
	for (size_t i = 0; i < b->n; i++) {
		if (b->items[i].action.type == REI_ACTION_NONE)
			continue;
		b->items[kept++] = b->items[i];
	}
	b->n = kept;
	return b;

fail:
	rei_bindings_free(b);
	return NULL;
}

void rei_bindings_free(struct rei_bindings *b)
{
	if (!b)
		return;
	for (size_t i = 0; i < b->n; i++)
		rei_action_free(&b->items[i].action);
	free(b->items);
	free(b);
}

const struct rei_action *rei_bindings_match(const struct rei_bindings *b,
					    uint32_t mods, xkb_keysym_t sym)
{
	for (size_t i = 0; i < b->n; i++) {
		if (b->items[i].mods == mods && b->items[i].sym == sym)
			return &b->items[i].action;
	}
	return NULL;
}
