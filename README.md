# rei-wm

A Wayland compositor, tiling/floating window manager and small desktop
environment, written in C11. MIT licensed.

**Status: early development.** Milestones 0–11 are done (the DRM/KMS backend
is written but still untested on real hardware: see [docs/DRM.md](docs/DRM.md)): the build, config,
event loop and threading model, the core Wayland protocols, an OpenGL ES
renderer, keyboard/pointer input with a configurable keybinding engine, the
window manager (an i3-style tiling tree with configurable gaps, floating windows,
fullscreen, per-output workspaces), the window notches (a drag handle and
minimize/maximize/close tabs on the focused window), and a built-in toolbar with
workspace buttons and a clock. Real applications can connect, be tiled, and be
driven from the keyboard or mouse. On real hardware it drives displays through
DRM/KMS with libinput, dma-buf clients, direct scanout and a hardware cursor. X11 programs run through Xwayland, tiled and floated like native windows. The config
reloads live, and a Qt settings app edits it. The toolbar shows the PipeWire volume (click to mute, wheel to change) and the media keys
work. Screen capture through PipeWire is still to come: see the roadmap in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

![Two unmodified Weston demo clients tiled by rei-wm under the toolbar](docs/images/milestone-7-toolbar.png)

*Unmodified Weston demo clients tiled by rei-wm over a wallpaper, under the toolbar:
workspace buttons at the start (workspace 1 is where you are; the pointer is over 2,
which has a window of its own) and the clock at the end. Windows have no border; the
focused one shows its drag handle and minimize/maximize/close tabs above it.*

## Building

Dependencies (Debian/Ubuntu package names):

```
meson ninja-build pkg-config libwayland-dev wayland-protocols libevent-dev
libegl-dev libgles-dev libxkbcommon-dev xkb-data
libcairo2-dev libpango1.0-dev fontconfig libxcb1-dev
```

For the volume widget and media keys (optional): `libpipewire-0.3-dev`.

For the settings app (optional): Qt 6 (`qt6-base-dev`).

For X11 programs, install `xwayland` (the binary is found at run time).

For the DRM backend (optional): `libdrm-dev libgbm-dev libseat-dev libinput-dev
libudev-dev`.

and at least one font (e.g. `fonts-dejavu-core`) for the toolbar's text.

Rendering uses OpenGL ES 3 through EGL's surfaceless platform, so it needs a
Mesa (or other) GLES driver but no display server; software rendering
(llvmpipe) works.

```bash
meson setup build
ninja -C build
meson test -C build
```

## The toolbar's application tray

The middle three fifths of the toolbar list the open windows, one tile per application (icon and name,
or just the icon: `[toolbar] tray = "icon_and_name" | "icon" | "off"`). Several windows of one
application show as a stack of tiles; clicking it opens a list to pick from. A single window's tile
focuses the window, and hides it again if it already has the focus. Hide windows with the notch's
minimize button or `Mod+m`; a restored window goes back to the same place in the tiling grid (next to
the same neighbour) or, if floating, to the same position.

**Start menu.** A *Start* button sits at the start of the toolbar. It opens a list: your shortcuts
at the top (defined as `[[shortcut]]` entries with a `name` and a `command`), then **Run**, then
**Logout**, **Shutdown** and **Reboot**. The power rows ask first in a confirmation dialog (Enter presses the highlighted button, which starts on Cancel; Tab/arrows move it; Escape cancels; `confirm_power = false`
turns that off), and their commands are configurable under `[start_menu]`. **Run** (or `Mod+r`)
opens a large, see-through command prompt in the middle of the screen: type a command, press Enter
to run it, Escape (or a click) to dismiss. Long commands wrap onto new lines and the box grows,
staying centred; once it reaches 60% of the screen height, no more can be typed. Backspace,
Ctrl+Backspace/Ctrl+W (word) and Ctrl+U (line) edit. Style it with `[theme.run_prompt]` and
`[theme.start_button]`, and the font with `run_font`.

- **Filling up:** as more windows open, tiles shrink in width, down to half their size
  (`tray_min_width`). After that the newest windows go under a "+N" overflow tile, which opens a list
  too. When 32 windows are under it, the next window opens on the next workspace (`tray_scope =
  "workspace"`, the default, lists the current workspace's windows; `"output"` lists every workspace of
  the monitor, with no cap).
- **Hover:** a tile highlights the moment the pointer touches it; half a second later a tooltip with the
  full title (or the list of windows) fades in.
- **Reorder:** drag a tile along the bar to move it; the others make room.
- Only what is running is shown: nothing is pinned. Icons come from your desktop entries and icon theme
  (PNG; SVG-only themes are not read), with a coloured letter tile when none is found. The clock format
  is `[toolbar] clock_format` (any `strftime` string; default `%H:%M:%S`).
- **Look:** colours, corner shapes, transparency and shadows of these elements are a theme; see
  [docs/THEME.md](docs/THEME.md).

## The battery

If the machine has a battery, the toolbar shows a horizontal battery icon that fills in proportion
to the charge, with the percentage, just before the volume and clock (green with a bolt while
charging, red at 15% or less). It is found in `/sys/class/power_supply`: batteries of type
`Battery` that belong to the machine are combined into one (total charge over total capacity, so
two batteries of different sizes weigh correctly); a wireless mouse's or phone's battery is
ignored. A desktop with no battery shows nothing. `[toolbar] battery = false` turns it off. It is
re-read every 15 seconds. Colours and shape are the `battery` theme element.

## Settings

`rei-settings` is a Qt app for everything in `~/.config/rei-wm/config.toml`: input, windows and
gaps, the toolbar, wallpaper, keybindings and monitors. It edits the file in place (your
comments and layout survive; only the values you change move), checks the result with
`rei-wm --check-config`, and saves atomically. A running rei-wm watches the file and applies
most changes at once (gaps, notch, toolbar, wallpaper, keybindings, keyboard layout, repeat
rate, touchpad settings); `[[output]]` and the backend need a restart. The compositor also
reloads on `SIGHUP`, and keeps the old settings (logging why) if the new file is invalid.

```bash
./build/settings/rei-settings                 # edits ~/.config/rei-wm/config.toml
./build/settings/rei-settings --config my.toml
```

## Running

```bash
# Inside another Wayland session: each output becomes a window.
./build/src/rei-wm

# On real hardware, from a text console (read docs/DRM.md first):
./build/src/rei-wm --backend drm

# Without a display (CI, containers): simulated monitors, no output.
./build/src/rei-wm --backend headless -v
```

Then start any Wayland client against the socket rei-wm prints:

```bash
WAYLAND_DISPLAY=wayland-1 weston-simple-shm
```

**Input.** In the nested backend your parent compositor's keyboard and mouse are
forwarded to rei-wm. The parent sees key combinations first, so `Super+...`
shortcuts are usually swallowed by it: when developing nested, set
`composition_key = "alt"` in the config. In the headless backend, point
`REI_HEADLESS_INPUT` at a FIFO and write events to it (this is how the tests
drive it):

```bash
mkfifo /tmp/rei-in
REI_HEADLESS_INPUT=/tmp/rei-in ./build/src/rei-wm --backend headless &
echo "motion_abs 400 300" > /tmp/rei-in     # also: motion, button, key, axis
echo "key 125 1"          > /tmp/rei-in     # evdev codes: Super down
```

In the headless backend nothing is visible, so ask rei-wm for a screenshot:
`kill -USR2 <pid>` writes a PPM per output to `$REI_SCREENSHOT_DIR` (default
`$XDG_RUNTIME_DIR`). `kill -USR1` forces a redraw.

### Using it

`Mod` is the composition key (Super by default). The defaults follow i3:

| keys | action |
|---|---|
| `Mod+Return` | open a terminal (`$TERMINAL`, else `foot`) |
| `Mod+Shift+q` | close the focused window |
| `Mod+h/j/k/l` | focus left / down / up / right |
| `Mod+Shift+h/j/k/l` | move the window that way |
| `Mod+v` / `Mod+b` | the next window opens stacked / side by side |
| `Mod+Shift+Space` | toggle floating |
| `Mod+f` | toggle fullscreen |
| `Mod+1`…`Mod+0` | go to workspace 1–10 (created if new) |
| `Mod+Shift+1`…`0` | send the window to that workspace |
| `Mod+Tab` / `Mod+Shift+Tab` | cycle focus |
| `Mod+Shift+e` | quit |
| `Mod`+left-drag / right-drag | move / resize a floating window |

**The notches.** The focused window shows two small tabs hovering directly above
its top edge: a drag handle on the left, and minimize, maximize and close on the
right. They are drawn over the space above the window, so showing or hiding them
never resizes anything. Click a button (press and release on it) to use it:
close works; minimize and maximize are placeholders for now. Press and hold the
handle to drag: a floating window just moves, and a tiled one lifts out of the
grid. Drop it on another window to join the grid on the side nearest the pointer
(a blue hint shows where it will land), or on empty space to leave it floating.
Because the tabs hover above the window, tiled windows always keep that much room
above them (the top gap, and the gap between stacked windows, is at least the notch
height), and a floating window can't be dragged so high that its tabs leave the
screen. `notch_height` in `[decoration]` sets the size; 0 turns them off.

**The toolbar** is a strip along one edge of every output (`position` = top, bottom,
left or right): workspace buttons at its start and a clock at its end. Each output's
bar lists that output's workspaces; the one you are on is highlighted (more dimly on
outputs that don't have the focus). Click a button to go to that workspace, scroll
the wheel over the bar to step through them, and hover to highlight. Windows never
use the space the bar takes. See the `[toolbar]` section of the config for the
position, size, font, clock format (`strftime`) and colours; `enabled = false`
removes it.

Workspaces belong to an output and are numbered globally, like i3's. An empty
one disappears when you leave it. Gaps (`gap_outer`, `gap_inner`) and the other
options are in the config; set `new_windows = "float"` to stop tiling new windows,
or `focus_follows_mouse = true` for i3's pointer focus. Dialogs always float.

Keybindings are plain strings in the config, so they can be edited by hand (or
by the settings application later). The defaults are written into the file on
first run:

```toml
[keybindings]
"Mod+Return"    = "exec ${TERMINAL:-foot}"
"Mod+Shift+q"   = "close"
"Mod+Tab"       = "focus_next"
"Mod+Shift+e"   = "none"            # "none" removes a built-in binding
"Mod+F1"        = "exec firefox"    # and you can add your own
"Mod+r"         = "workspace 5"
```

Configuration lives in `~/.config/rei-wm/config.toml` (or
`$XDG_CONFIG_HOME/rei-wm/`). A commented default is written on first run. Use
`rei-wm --check-config` to validate a file, and `--help` for all options.

## Focus, the new-window switch and fullscreen

- **Focus follows the pointer** (`[general] focus_follows_mouse`, on by default): hovering a window focuses
  it; moving onto the desktop, the toolbar or a menu leaves nothing focused (keyboard shortcuts such as
  `Mod+q` and `Mod+f` still act on the last window). Set it to `false` for click-to-focus.
- **Tiled/floating switch:** the small square after the Start button decides how *new* windows open (a grid
  of panes: tiled; two overlapping windows, lit: floating). It starts from `[layout] new_windows`, and a
  config reload resets it to that.
- **Fullscreen** fills everything beside the toolbar, but keeps the outer gap (`gap_outer`), so the toolbar
  stays visible and usable. Style the confirmation dialog with `[theme.dialog]`.

## CPU/RAM readout and glow

- **CPU and RAM:** one small box just left of the clock shows `CPU 12%   RAM 5.2/16.0GB` (the busy share of all cores,
  and memory used / total in GiB, one decimal), refreshed every two seconds. `[toolbar] sysinfo = false` removes it; style it with
  `[theme.sysinfo]`.
- **Glow:** hovered menu rows, confirmation dialogs and the Run prompt have a white halo. Per element:
  `glow_color`, `glow_size` (pixels; 0 turns it off) and `glow_opacity` under `[theme.menu_row]`,
  `[theme.dialog]` or `[theme.run_prompt]`; any other element can be given one the same way.

## Icons

Three elements are **iconable** (they can show a square icon, at most 64x64, at their front): the tray's
window tiles, the Start button, and every row of the Start menu. Tiles show the application's icon, as
before. Set `[theme] flavor_icons = "/path/to/dir"` and *every* iconable element instead gets a random icon
from that directory (stable for a given element during a session, so icons don't flicker). Only `.png` files
that are **square (1:1) and at most 64x64** are used; anything else is skipped with a warning in the log.
Transparency is respected. Per element, `[theme.tray_tile]`, `[theme.start_button]` or `[theme.menu_row]`
accept `icon = false` (no icon) and `icon_size` (pixels, up to 64; 0 fits the element).
