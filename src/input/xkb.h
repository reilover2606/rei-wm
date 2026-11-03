#ifndef REI_XKB_H
#define REI_XKB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct rei_config;

/* Modifier bits used by keybindings (locks like Caps/Num are deliberately not
 * part of this: bindings should work whether or not they are on). */
#define REI_MOD_SHIFT (1u << 0)
#define REI_MOD_CTRL  (1u << 1)
#define REI_MOD_ALT   (1u << 2)
#define REI_MOD_LOGO  (1u << 3)

/* The compositor's keyboard model: one keymap and one modifier state shared by
 * every client, as in a normal desktop. */
struct rei_xkb {
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *state;

	int keymap_fd;       /* memfd holding the keymap text, sent to clients */
	uint32_t keymap_size;  /* Including the terminating NUL. */

	xkb_mod_index_t idx_shift, idx_ctrl, idx_alt, idx_logo;
};

/* Compiles the keymap named by the [input] config (falling back to the
 * XKB_DEFAULT_* environment and system defaults for empty fields). NULL on
 * failure, with a reason in err. */
struct rei_xkb *rei_xkb_create(const struct rei_config *config, char *err, size_t errlen);
void rei_xkb_destroy(struct rei_xkb *xkb);

/* Feeds a key event (evdev keycode) to the state. Returns true if the
 * modifier or layout state changed and clients must be told. */
bool rei_xkb_update_key(struct rei_xkb *xkb, uint32_t evdev_code, bool pressed);

/* Effective REI_MOD_* bits. */
uint32_t rei_xkb_mods(const struct rei_xkb *xkb);

/* The symbol the key produces with no modifiers applied (shift level), so
 * that "Mod+Shift+1" matches the 1 key rather than "!". */
xkb_keysym_t rei_xkb_base_sym(const struct rei_xkb *xkb, uint32_t evdev_code);
/* The symbol the key produces with the current modifiers. */
xkb_keysym_t rei_xkb_sym(const struct rei_xkb *xkb, uint32_t evdev_code);

struct rei_xkb_mods {
	uint32_t depressed, latched, locked, group;
};
struct rei_xkb_mods rei_xkb_serialize(const struct rei_xkb *xkb);

#endif
