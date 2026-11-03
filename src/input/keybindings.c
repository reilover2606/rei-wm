#include "input/keybindings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "input/xkb.h"
#include "util/util.h"

/* Keep in step with the [keybindings] section of config_default.c. Bindings
 * for tiling, workspaces and floating arrive with the window manager. */
const struct rei_default_binding rei_default_bindings[] = {
	{ "Mod+Return", "exec ${TERMINAL:-foot}" },
	{ "Mod+Shift+q", "close" },
	{ "Mod+Shift+e", "quit" },
	{ "Mod+Tab", "focus_next" },
	{ "Mod+Shift+Tab", "focus_prev" },
	{ NULL, NULL },
};

static uint32_t composition_bit(const char *name)
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
			mods |= composition_bit(composition_key);
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

bool rei_action_parse(const char *text, struct rei_action *out, char *err, size_t errlen)
{
	while (*text == ' ')
		text++;
	const char *space = strchr(text, ' ');
	size_t name_len = space ? (size_t)(space - text) : strlen(text);
	const char *arg = space ? space + 1 : "";
	while (*arg == ' ')
		arg++;

	struct {
		const char *name;
		enum rei_action_type type;
		bool takes_arg;
	} actions[] = {
		{ "none", REI_ACTION_NONE, false },
		{ "exec", REI_ACTION_EXEC, true },
		{ "close", REI_ACTION_CLOSE, false },
		{ "quit", REI_ACTION_QUIT, false },
		{ "focus_next", REI_ACTION_FOCUS_NEXT, false },
		{ "focus_prev", REI_ACTION_FOCUS_PREV, false },
	};

	for (size_t i = 0; i < REI_ARRAY_LEN(actions); i++) {
		if (strlen(actions[i].name) != name_len || strncmp(actions[i].name, text, name_len))
			continue;
		if (actions[i].takes_arg && !arg[0]) {
			snprintf(err, errlen, "'%s' needs a command, e.g. \"exec foot\"", actions[i].name);
			return false;
		}
		if (!actions[i].takes_arg && arg[0]) {
			snprintf(err, errlen, "'%s' takes no argument", actions[i].name);
			return false;
		}
		out->type = actions[i].type;
		out->arg = actions[i].takes_arg ? rei_xstrdup(arg) : NULL;
		return true;
	}

	snprintf(err, errlen, "unknown action '%.*s' (known: none, exec, close, quit, "
		 "focus_next, focus_prev)", (int)name_len, text);
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
