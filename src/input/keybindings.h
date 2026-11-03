#ifndef REI_KEYBINDINGS_H
#define REI_KEYBINDINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "layout.h"

struct rei_config;

/*
 * Keybindings map a key combination to an action. Both are plain strings in
 * the config so they can be edited by hand or by the settings application:
 *
 *   [keybindings]
 *   "Mod+Return"  = "exec foot"
 *   "Mod+Shift+q" = "close"
 *   "Mod+Tab"     = "none"        # removes a built-in binding
 *
 * A combination is modifiers joined by '+', then a key name as xkbcommon spells
 * it ("Return", "q", "Tab", "F5", "1"). Modifiers: Mod (the configured
 * composition key), Super, Alt, Ctrl, Shift.
 */

enum rei_action_type {
	REI_ACTION_NONE,  /* Unbinds. */
	REI_ACTION_EXEC,
	REI_ACTION_CLOSE,
	REI_ACTION_QUIT,
	REI_ACTION_FOCUS_NEXT,
	REI_ACTION_FOCUS_PREV,
	REI_ACTION_FOCUS_DIR,        /* focus left|right|up|down */
	REI_ACTION_MOVE_DIR,         /* move left|right|up|down */
	REI_ACTION_SPLIT,            /* split h|v */
	REI_ACTION_TOGGLE_FLOATING,
	REI_ACTION_FULLSCREEN,
	REI_ACTION_WORKSPACE,        /* workspace N */
	REI_ACTION_MOVE_TO_WORKSPACE,  /* move_to_workspace N */
	REI_ACTION_MINIMIZE,         /* minimize the focused window into the tray */
	REI_ACTION_VOLUME_UP,        /* volume_up [percent] */
	REI_ACTION_VOLUME_DOWN,      /* volume_down [percent] */
	REI_ACTION_VOLUME_MUTE,      /* volume_mute: toggles */
};

struct rei_action {
	enum rei_action_type type;
	char *arg;  /* Command line for exec; NULL otherwise. */
	int value;  /* A rei_direction, a rei_layout, a workspace number, or a volume step (0: the configured one). */
};

/* The modifier bit the config's composition key ("super", "alt", ...) stands for. */
uint32_t rei_composition_mod(const char *composition_key);

/* `composition_key` is the config's "super"/"alt"/"ctrl"/"shift". */
bool rei_combo_parse(const char *text, const char *composition_key,
		     uint32_t *mods, xkb_keysym_t *sym, char *err, size_t errlen);

/* On success the caller owns out->arg. */
bool rei_action_parse(const char *text, struct rei_action *out, char *err, size_t errlen);
void rei_action_free(struct rei_action *action);

struct rei_binding {
	uint32_t mods;
	xkb_keysym_t sym;
	struct rei_action action;
};

struct rei_bindings {
	struct rei_binding *items;
	size_t n;
};

/* The built-in bindings, as (combination, action) text pairs. NULL-terminated.
 * The default config file lists exactly these, and a test keeps them in step. */
struct rei_default_binding {
	const char *combo;
	const char *action;
};
extern const struct rei_default_binding rei_default_bindings[];

/* Built-ins first, then the config's [keybindings] on top: a later entry for
 * the same combination replaces an earlier one, and "none" removes it. */
struct rei_bindings *rei_bindings_from_config(const struct rei_config *config,
					      char *err, size_t errlen);
void rei_bindings_free(struct rei_bindings *b);

/* NULL if nothing is bound to this exact modifier set and key. */
const struct rei_action *rei_bindings_match(const struct rei_bindings *b,
					    uint32_t mods, xkb_keysym_t sym);

#endif
