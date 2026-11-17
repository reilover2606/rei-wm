#!/bin/sh
# The session launcher and the display manager's entry: the launcher is valid shell, sets the Wayland session
# environment, runs the installed rei-wm with the user's extra options; the entry names the launcher.
set -e
script="$1"
entry="$2"
sh -n "$script"

out=$(REI_SESSION_DRY_RUN=1 REI_WM_ARGS="-v" HOME=/nonexistent sh "$script")
echo "$out" | grep -q "^args: -v$"
echo "$out" | grep -q "XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=rei-wm"
echo "$out" | grep -q "MOZ_ENABLE_WAYLAND=1"
echo "$out" | grep -q "^rei-wm: /.*/rei-wm$"

# The entry points at the launcher, by absolute path, and has what a display manager needs.
grep -q "^Type=Application$" "$entry"
grep -q "^Name=rei-wm$" "$entry"
grep -q "^Exec=/.*/rei-wm-session$" "$entry"
if grep -Eq "@[a-z_]+@" "$entry" "$script"; then
	echo "unsubstituted @...@ in the generated files" >&2
	exit 1
fi

# Without a login session (no XDG_RUNTIME_DIR) it fails with a message instead of starting rei-wm.
if env -u XDG_RUNTIME_DIR REI_SESSION_LOGIN=1 HOME=/nonexistent sh "$script" 2>/dev/null; then
	echo "started without XDG_RUNTIME_DIR" >&2
	exit 1
fi
echo "session launcher: ok"
