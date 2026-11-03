#include <string.h>

#include "check.h"
#include "config.h"
#include "input/keybindings.h"
#include "input/xkb.h"

static void combo_ok(const char *text, const char *comp, uint32_t mods, xkb_keysym_t sym)
{
	char err[200];
	uint32_t m;
	xkb_keysym_t s;
	if (!rei_combo_parse(text, comp, &m, &s, err, sizeof err)) {
		fprintf(stderr, "'%s' should parse: %s\n", text, err);
		exit(1);
	}
	CHECK(m == mods);
	CHECK(s == sym);
}

static void combo_bad(const char *text, const char *expect)
{
	char err[200];
	uint32_t m;
	xkb_keysym_t s;
	if (rei_combo_parse(text, "super", &m, &s, err, sizeof err)) {
		fprintf(stderr, "'%s' should not parse\n", text);
		exit(1);
	}
	if (!strstr(err, expect)) {
		fprintf(stderr, "error '%s' lacks '%s'\n", err, expect);
		exit(1);
	}
}

static void test_combos(void)
{
	/* "Mod" means whatever the composition key is. */
	combo_ok("Mod+Return", "super", REI_MOD_LOGO, XKB_KEY_Return);
	combo_ok("Mod+Return", "alt", REI_MOD_ALT, XKB_KEY_Return);
	combo_ok("mod+return", "ctrl", REI_MOD_CTRL, XKB_KEY_Return);
	combo_ok("Super+Shift+Tab", "alt", REI_MOD_LOGO | REI_MOD_SHIFT, XKB_KEY_Tab);
	combo_ok("Ctrl+Alt+Delete", "super", REI_MOD_CTRL | REI_MOD_ALT, XKB_KEY_Delete);
	combo_ok("F5", "super", 0, XKB_KEY_F5);
	combo_ok("Mod+plus", "super", REI_MOD_LOGO, XKB_KEY_plus);
	combo_ok("Mod+1", "super", REI_MOD_LOGO, XKB_KEY_1);
	/* Case of the key name doesn't matter: "Q" is the q key. */
	combo_ok("Mod+Shift+Q", "super", REI_MOD_LOGO | REI_MOD_SHIFT, XKB_KEY_q);
	combo_ok(" Mod + q ", "super", REI_MOD_LOGO, XKB_KEY_q);

	combo_bad("", "expected");
	combo_bad("+q", "expected");
	combo_bad("Mod+", "expected");
	combo_bad("Mod++q", "expected");
	combo_bad("Hyper+q", "unknown modifier 'Hyper'");
	combo_bad("Mod+NoSuchKey", "unknown key 'NoSuchKey'");
}

static void test_actions(void)
{
	char err[200];
	struct rei_action a;

	CHECK(rei_action_parse("exec foot --title x", &a, err, sizeof err));
	CHECK(a.type == REI_ACTION_EXEC);
	CHECK_STR(a.arg, "foot --title x");
	rei_action_free(&a);

	CHECK(rei_action_parse("exec   ${TERMINAL:-foot}", &a, err, sizeof err));
	CHECK_STR(a.arg, "${TERMINAL:-foot}");
	rei_action_free(&a);

	CHECK(rei_action_parse("close", &a, err, sizeof err) && a.type == REI_ACTION_CLOSE && !a.arg);
	CHECK(rei_action_parse("none", &a, err, sizeof err) && a.type == REI_ACTION_NONE);
	CHECK(rei_action_parse("focus_next", &a, err, sizeof err) && a.type == REI_ACTION_FOCUS_NEXT);

	/* Window-management actions carry a direction, a split layout or a number. */
	CHECK(rei_action_parse("focus left", &a, err, sizeof err));
	CHECK(a.type == REI_ACTION_FOCUS_DIR && a.value == REI_DIR_LEFT);
	CHECK(rei_action_parse("focus down", &a, err, sizeof err) && a.value == REI_DIR_DOWN);
	CHECK(rei_action_parse("move up", &a, err, sizeof err));
	CHECK(a.type == REI_ACTION_MOVE_DIR && a.value == REI_DIR_UP);
	CHECK(rei_action_parse("move right", &a, err, sizeof err) && a.value == REI_DIR_RIGHT);
	CHECK(rei_action_parse("split h", &a, err, sizeof err));
	CHECK(a.type == REI_ACTION_SPLIT && a.value == REI_LAYOUT_SPLIT_H);
	CHECK(rei_action_parse("split vertical", &a, err, sizeof err) && a.value == REI_LAYOUT_SPLIT_V);
	CHECK(rei_action_parse("toggle_floating", &a, err, sizeof err) && a.type == REI_ACTION_TOGGLE_FLOATING);
	CHECK(rei_action_parse("fullscreen", &a, err, sizeof err) && a.type == REI_ACTION_FULLSCREEN);
	CHECK(rei_action_parse("workspace 7", &a, err, sizeof err));
	CHECK(a.type == REI_ACTION_WORKSPACE && a.value == 7);
	CHECK(rei_action_parse("move_to_workspace 12", &a, err, sizeof err));
	CHECK(a.type == REI_ACTION_MOVE_TO_WORKSPACE && a.value == 12);

	CHECK(!rei_action_parse("focus", &a, err, sizeof err) && strstr(err, "direction"));
	CHECK(!rei_action_parse("focus sideways", &a, err, sizeof err) && strstr(err, "direction"));
	CHECK(!rei_action_parse("split", &a, err, sizeof err) && strstr(err, "h (side by side) or v"));
	CHECK(!rei_action_parse("workspace", &a, err, sizeof err) && strstr(err, "workspace number"));
	CHECK(!rei_action_parse("workspace 0", &a, err, sizeof err) && strstr(err, "1 to 99"));
	CHECK(!rei_action_parse("workspace 100", &a, err, sizeof err));
	CHECK(!rei_action_parse("workspace two", &a, err, sizeof err));
	CHECK(!rei_action_parse("fullscreen now", &a, err, sizeof err) && strstr(err, "takes no argument"));

	CHECK(!rei_action_parse("exec", &a, err, sizeof err) && strstr(err, "needs a command"));
	CHECK(!rei_action_parse("close now", &a, err, sizeof err) && strstr(err, "takes no argument"));
	CHECK(!rei_action_parse("explode", &a, err, sizeof err) && strstr(err, "unknown action 'explode'"));
	CHECK(!rei_action_parse("", &a, err, sizeof err));
}

static struct rei_config *config_from(const char *text)
{
	char err[300];
	struct rei_config *c = rei_config_parse(text, err, sizeof err);
	if (!c)
		fprintf(stderr, "unexpected config error: %s\n", err);
	CHECK(c);
	return c;
}

static void test_bindings_merge(void)
{
	char err[200];
	uint32_t mods = REI_MOD_LOGO;

	/* Defaults alone. */
	struct rei_config *c = config_from("");
	struct rei_bindings *b = rei_bindings_from_config(c, err, sizeof err);
	CHECK(b);
	const struct rei_action *a = rei_bindings_match(b, mods, XKB_KEY_Return);
	CHECK(a && a->type == REI_ACTION_EXEC);
	CHECK_STR(a->arg, "${TERMINAL:-foot}");
	a = rei_bindings_match(b, mods | REI_MOD_SHIFT, XKB_KEY_q);
	CHECK(a && a->type == REI_ACTION_CLOSE);
	/* Bindings need the *exact* modifiers: Mod+q is not Mod+Shift+q. */
	CHECK(!rei_bindings_match(b, mods, XKB_KEY_q));
	CHECK(!rei_bindings_match(b, mods | REI_MOD_CTRL, XKB_KEY_Return));
	CHECK(!rei_bindings_match(b, 0, XKB_KEY_Return));
	/* The i3-style window-management defaults are all there. */
	a = rei_bindings_match(b, mods, XKB_KEY_h);
	CHECK(a && a->type == REI_ACTION_FOCUS_DIR && a->value == REI_DIR_LEFT);
	a = rei_bindings_match(b, mods | REI_MOD_SHIFT, XKB_KEY_l);
	CHECK(a && a->type == REI_ACTION_MOVE_DIR && a->value == REI_DIR_RIGHT);
	a = rei_bindings_match(b, mods, XKB_KEY_v);
	CHECK(a && a->type == REI_ACTION_SPLIT && a->value == REI_LAYOUT_SPLIT_V);
	a = rei_bindings_match(b, mods | REI_MOD_SHIFT, XKB_KEY_space);
	CHECK(a && a->type == REI_ACTION_TOGGLE_FLOATING);
	CHECK(rei_bindings_match(b, mods, XKB_KEY_f)->type == REI_ACTION_FULLSCREEN);
	for (int n = 1; n <= 10; n++) {
		xkb_keysym_t key = XKB_KEY_1 + (xkb_keysym_t)(n % 10 == 0 ? 9 : n - 1);
		if (n == 10)
			key = XKB_KEY_0;
		a = rei_bindings_match(b, mods, key);
		CHECK(a && a->type == REI_ACTION_WORKSPACE && a->value == n);
		a = rei_bindings_match(b, mods | REI_MOD_SHIFT, key);
		CHECK(a && a->type == REI_ACTION_MOVE_TO_WORKSPACE && a->value == n);
	}
	size_t n_defaults = b->n;
	rei_bindings_free(b);
	rei_config_free(c);

	/* The user's entries override, add and remove. */
	c = config_from(
		"[keybindings]\n"
		"\"Mod+Return\" = \"exec xterm\"\n"
		"\"Mod+Tab\" = \"none\"\n"
		"\"Mod+F1\" = \"exec firefox\"\n");
	b = rei_bindings_from_config(c, err, sizeof err);
	CHECK(b);
	a = rei_bindings_match(b, mods, XKB_KEY_Return);
	CHECK(a && !strcmp(a->arg, "xterm"));
	CHECK(!rei_bindings_match(b, mods, XKB_KEY_Tab));
	a = rei_bindings_match(b, mods, XKB_KEY_F1);
	CHECK(a && !strcmp(a->arg, "firefox"));
	CHECK(b->n == n_defaults - 1 + 1);  /* one removed, one added */
	rei_bindings_free(b);
	rei_config_free(c);

	/* The composition key decides what "Mod" is, for defaults and user entries. */
	c = config_from("[general]\ncomposition_key = \"alt\"\n[keybindings]\n\"Mod+F2\" = \"close\"\n");
	b = rei_bindings_from_config(c, err, sizeof err);
	CHECK(rei_bindings_match(b, REI_MOD_ALT, XKB_KEY_Return));
	CHECK(!rei_bindings_match(b, REI_MOD_LOGO, XKB_KEY_Return));
	CHECK(rei_bindings_match(b, REI_MOD_ALT, XKB_KEY_F2));
	rei_bindings_free(b);
	rei_config_free(c);
}

static void test_config_validation(void)
{
	char err[300];
	struct { const char *text, *expect; } bad[] = {
		{ "[keybindings]\n\"Mod+Nope\" = \"close\"\n", "unknown key 'Nope'" },
		{ "[keybindings]\n\"Hyper+q\" = \"close\"\n", "unknown modifier" },
		{ "[keybindings]\n\"Mod+q\" = \"explode\"\n", "keybindings.\"Mod+q\": unknown action" },
		{ "[keybindings]\n\"Mod+q\" = \"exec\"\n", "needs a command" },
		{ "[keybindings]\n\"Mod+q\" = 5\n", "expected a string" },
		{ "[input]\nrepeat_rate = -1\n", "input.repeat_rate" },
		{ "[input]\nrepeat_delay = \"fast\"\n", "input.repeat_delay" },
		{ "[input]\nkeyboard_layout = 3\n", "input.keyboard_layout" },
		{ "[keybindings]\n\"Mod+q\" = \"workspace 0\"\n", "1 to 99" },
		{ "[keybindings]\n\"Mod+q\" = \"focus nowhere\"\n", "direction" },
		{ "[layout]\nnew_windows = \"sideways\"\n", "layout.new_windows" },
		{ "[general]\nfocus_follows_mouse = \"yes\"\n", "general.focus_follows_mouse" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		struct rei_config *c = rei_config_parse(bad[i].text, err, sizeof err);
		if (c || !strstr(err, bad[i].expect)) {
			fprintf(stderr, "config:\n%s\nexpected error containing '%s', got '%s'\n",
				bad[i].text, bad[i].expect, c ? "(success)" : err);
			exit(1);
		}
	}

	struct rei_config *c = config_from(
		"[input]\nkeyboard_layout = \"de\"\nkeyboard_variant = \"nodeadkeys\"\n"
		"keyboard_options = \"ctrl:nocaps\"\nrepeat_rate = 40\nrepeat_delay = 250\n");
	CHECK_STR(c->kb_layout, "de");
	CHECK_STR(c->kb_variant, "nodeadkeys");
	CHECK_STR(c->kb_options, "ctrl:nocaps");
	CHECK(c->repeat_rate == 40 && c->repeat_delay == 250);
	rei_config_free(c);
}

/* The file written on first run must say exactly what the built-ins do, so
 * editing it never silently changes behaviour. */
static void test_wm_config(void)
{
	struct rei_config *c = config_from("");
	CHECK(!c->float_new_windows && !c->focus_follows_mouse);  /* tile, click to focus */
	rei_config_free(c);
	c = config_from("[general]\nfocus_follows_mouse = true\n[layout]\nnew_windows = \"float\"\n");
	CHECK(c->float_new_windows && c->focus_follows_mouse);
	rei_config_free(c);
}

static void test_default_text_matches_builtins(void)
{
	struct rei_config *c = config_from(rei_config_default_text());
	size_t n = 0;
	for (const struct rei_default_binding *d = rei_default_bindings; d->combo; d++, n++) {
		CHECK(n < c->n_bindings);
		CHECK_STR(c->bindings[n].combo, d->combo);
		CHECK_STR(c->bindings[n].action, d->action);
	}
	CHECK(n == c->n_bindings);
	CHECK(c->repeat_rate == 25 && c->repeat_delay == 600);
	rei_config_free(c);
}

/* ---- real keymaps ---------------------------------------------------- */

#define KEY_Q 16
#define KEY_Z 44
#define KEY_LEFTSHIFT 42
#define KEY_LEFTMETA 125
#define KEY_CAPSLOCK 58
#define KEY_LEFTALT 56

static void test_xkb(void)
{
	char err[300];
	struct rei_config *c = config_from("[input]\nkeyboard_layout = \"us\"\n");
	struct rei_xkb *x = rei_xkb_create(c, err, sizeof err);
	if (!x) {
		fprintf(stderr, "test_keybindings: no xkb data here (%s), skipping keymap checks\n", err);
		rei_config_free(c);
		return;
	}

	CHECK(x->keymap_fd >= 0 && x->keymap_size > 100);
	CHECK(rei_xkb_mods(x) == 0);
	CHECK(rei_xkb_base_sym(x, KEY_Q) == XKB_KEY_q);
	CHECK(rei_xkb_sym(x, KEY_Q) == XKB_KEY_q);

	/* Shift changes the produced symbol but not the base one. */
	CHECK(rei_xkb_update_key(x, KEY_LEFTSHIFT, true));
	CHECK(rei_xkb_mods(x) == REI_MOD_SHIFT);
	CHECK(rei_xkb_sym(x, KEY_Q) == XKB_KEY_Q);
	CHECK(rei_xkb_base_sym(x, KEY_Q) == XKB_KEY_q);
	CHECK(rei_xkb_serialize(x).depressed != 0);
	CHECK(rei_xkb_update_key(x, KEY_LEFTSHIFT, false));
	CHECK(rei_xkb_mods(x) == 0);

	CHECK(rei_xkb_update_key(x, KEY_LEFTMETA, true));
	CHECK(rei_xkb_mods(x) == REI_MOD_LOGO);
	CHECK(rei_xkb_update_key(x, KEY_LEFTALT, true));
	CHECK(rei_xkb_mods(x) == (REI_MOD_LOGO | REI_MOD_ALT));
	rei_xkb_update_key(x, KEY_LEFTMETA, false);
	rei_xkb_update_key(x, KEY_LEFTALT, false);

	/* A plain letter key doesn't change modifier state. */
	CHECK(!rei_xkb_update_key(x, KEY_Q, true));
	rei_xkb_update_key(x, KEY_Q, false);

	/* Caps Lock is a lock: it must not count as a binding modifier. */
	rei_xkb_update_key(x, KEY_CAPSLOCK, true);
	rei_xkb_update_key(x, KEY_CAPSLOCK, false);
	CHECK(rei_xkb_mods(x) == 0);
	CHECK(rei_xkb_serialize(x).locked != 0);
	CHECK(rei_xkb_sym(x, KEY_Q) == XKB_KEY_Q);  /* but it does capitalise */
	rei_xkb_destroy(x);
	rei_config_free(c);

	/* The layout setting really selects a different keymap: on a German
	 * keyboard the key in the Z position produces y. */
	c = config_from("[input]\nkeyboard_layout = \"de\"\n");
	x = rei_xkb_create(c, err, sizeof err);
	CHECK(x);
	CHECK(rei_xkb_base_sym(x, KEY_Z) == XKB_KEY_y);
	rei_xkb_destroy(x);
	rei_config_free(c);

	/* A layout that doesn't exist is an error naming it, not a crash. */
	c = config_from("[input]\nkeyboard_layout = \"zz-nonexistent\"\n");
	CHECK(!rei_xkb_create(c, err, sizeof err));
	CHECK(strstr(err, "zz-nonexistent"));
	rei_config_free(c);
}

int main(void)
{
	test_combos();
	test_actions();
	test_bindings_merge();
	test_config_validation();
	test_wm_config();
	test_default_text_matches_builtins();
	test_xkb();
	puts("test_keybindings: ok");
	return 0;
}
