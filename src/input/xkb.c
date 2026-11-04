#include "input/xkb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "config.h"
#include "util/util.h"

/* An empty string means "use the library default" for that field. */
static const char *opt(const char *s)
{
	return (s && s[0]) ? s : NULL;
}

/* Puts the keymap text in a memfd, which is what wl_keyboard.keymap wants. */
static bool make_keymap_fd(struct rei_xkb *x, char *err, size_t errlen)
{
	char *text = xkb_keymap_get_as_string(x->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!text) {
		snprintf(err, errlen, "cannot serialise the keymap");
		return false;
	}
	size_t size = strlen(text) + 1;

	int fd = memfd_create("rei-wm-keymap", MFD_CLOEXEC);
	bool ok = fd >= 0 && ftruncate(fd, (off_t)size) == 0;
	if (ok) {
		void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			ok = false;
		} else {
			memcpy(map, text, size);
			munmap(map, size);
		}
	}
	free(text);
	if (!ok) {
		if (fd >= 0)
			close(fd);
		snprintf(err, errlen, "cannot create the keymap file");
		return false;
	}
	x->keymap_fd = fd;
	x->keymap_size = (uint32_t)size;
	return true;
}

struct rei_xkb *rei_xkb_create(const struct rei_config *cfg, char *err, size_t errlen)
{
	struct rei_xkb *x = rei_xcalloc(1, sizeof *x);
	x->keymap_fd = -1;

	x->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!x->ctx) {
		snprintf(err, errlen, "cannot create an xkb context");
		goto fail;
	}

	struct xkb_rule_names names = {
		.rules = opt(cfg->kb_rules),
		.model = opt(cfg->kb_model),
		.layout = opt(cfg->kb_layout),
		.variant = opt(cfg->kb_variant),
		.options = opt(cfg->kb_options),
	};
	x->keymap = xkb_keymap_new_from_names(x->ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!x->keymap) {
		snprintf(err, errlen,
			 "cannot compile a keymap for layout '%s' variant '%s' (is xkb-data installed?)",
			 names.layout ? names.layout : "(default)",
			 names.variant ? names.variant : "");
		goto fail;
	}
	x->state = xkb_state_new(x->keymap);
	if (!x->state) {
		snprintf(err, errlen, "cannot create xkb state");
		goto fail;
	}

	x->idx_shift = xkb_keymap_mod_get_index(x->keymap, XKB_MOD_NAME_SHIFT);
	x->idx_ctrl = xkb_keymap_mod_get_index(x->keymap, XKB_MOD_NAME_CTRL);
	x->idx_alt = xkb_keymap_mod_get_index(x->keymap, XKB_MOD_NAME_ALT);
	x->idx_logo = xkb_keymap_mod_get_index(x->keymap, XKB_MOD_NAME_LOGO);

	if (!make_keymap_fd(x, err, errlen))
		goto fail;
	return x;

fail:
	rei_xkb_destroy(x);
	return NULL;
}

void rei_xkb_destroy(struct rei_xkb *x)
{
	if (!x)
		return;
	if (x->keymap_fd >= 0)
		close(x->keymap_fd);
	xkb_state_unref(x->state);
	xkb_keymap_unref(x->keymap);
	xkb_context_unref(x->ctx);
	free(x);
}

bool rei_xkb_update_key(struct rei_xkb *x, uint32_t code, bool pressed)
{
	/* evdev keycodes are offset by 8 in xkb. */
	enum xkb_state_component changed = xkb_state_update_key(
		x->state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	return changed != 0;
}

static bool mod_active(const struct rei_xkb *x, xkb_mod_index_t idx)
{
	return idx != XKB_MOD_INVALID &&
	       xkb_state_mod_index_is_active(x->state, idx, XKB_STATE_MODS_EFFECTIVE) > 0;
}

uint32_t rei_xkb_mods(const struct rei_xkb *x)
{
	uint32_t m = 0;
	if (mod_active(x, x->idx_shift)) m |= REI_MOD_SHIFT;
	if (mod_active(x, x->idx_ctrl)) m |= REI_MOD_CTRL;
	if (mod_active(x, x->idx_alt)) m |= REI_MOD_ALT;
	if (mod_active(x, x->idx_logo)) m |= REI_MOD_LOGO;
	return m;
}

xkb_keysym_t rei_xkb_base_sym(const struct rei_xkb *x, uint32_t code)
{
	xkb_keycode_t kc = code + 8;
	xkb_layout_index_t layout = xkb_state_key_get_layout(x->state, kc);
	const xkb_keysym_t *syms;
	if (xkb_keymap_key_get_syms_by_level(x->keymap, kc, layout, 0, &syms) < 1)
		return XKB_KEY_NoSymbol;
	return xkb_keysym_to_lower(syms[0]);
}

xkb_keysym_t rei_xkb_sym(const struct rei_xkb *x, uint32_t code)
{
	return xkb_state_key_get_one_sym(x->state, code + 8);
}

size_t rei_xkb_utf8(const struct rei_xkb *x, uint32_t code, char *buf, size_t n)
{
	int len = xkb_state_key_get_utf8(x->state, code + 8, buf, n);
	return len > 0 ? (size_t)len : 0;
}

struct rei_xkb_mods rei_xkb_serialize(const struct rei_xkb *x)
{
	return (struct rei_xkb_mods){
		.depressed = xkb_state_serialize_mods(x->state, XKB_STATE_MODS_DEPRESSED),
		.latched = xkb_state_serialize_mods(x->state, XKB_STATE_MODS_LATCHED),
		.locked = xkb_state_serialize_mods(x->state, XKB_STATE_MODS_LOCKED),
		.group = xkb_state_serialize_layout(x->state, XKB_STATE_LAYOUT_EFFECTIVE),
	};
}
