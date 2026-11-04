# rei-wm architecture

## Threads

```
                 ┌──────────────── event thread ────────────────┐
 Wayland clients │ libevent loop                                │
 ───────────────►│  ├─ wl_display (libwayland-server) via its fd│
 signals, timers │  ├─ backend fds (parent compositor, later    │
 ───────────────►│  │   libinput / DRM / udev / Xwayland)       │
                 │  ├─ config, window manager, focus, bindings  │
                 │  └─ server.inbox  ◄────────────────┐         │
                 └───────────┬─────────────────────────┼─────────┘
                  snapshots  │ (immutable,             │ frame events
                  resize,stop│  ref-counted)           │ (presented, failed)
                 ┌───────────▼─────────┐  ┌────────────┴────────┐
                 │ render thread: A    │  │ render thread: B    │ … one per output
                 │ own libevent loop   │  │                     │
                 │ EGL context, vblank │  │                     │
                 └─────────────────────┘  └─────────────────────┘
```

**The event thread owns all compositor state**: every `wl_resource`, surface,
window, workspace and input device. libwayland objects are not thread-safe, so
nothing else may touch them.

**Render threads never read live state.** When the scene changes, the event
thread builds a `rei_scene_snapshot` (immutable, reference-counted) and posts it
to each output's render thread. Rendering therefore needs no locks. The render
thread holds its own copy of the output mode and refers to its output by `id`,
never by pointer, so an output can disappear while a frame is in flight.

**All cross-thread traffic goes through `rei_msgq`** (`src/util/msgq.h`): a
mutex-protected list that wakes the receiving libevent loop with
`event_active()`. The vocabulary is in `src/messages.h`. This depends on
`evthread_use_pthreads()` being called before any event base is created.

Frame pacing: a render thread draws only when it has a new snapshot, and then no
more often than the output's refresh rate (the headless/nested backends simulate
vblank with a timer; the DRM backend will use page-flip events). After each
frame it posts `REI_EMSG_FRAME`, which is where frame callbacks, presentation
feedback and `wl_buffer.release` will be driven from. Idle compositors wake no
threads.

## From client commit to pixels

1. A client commits a `wl_surface`. On the event thread, the buffer is copied
   into an immutable, reference-counted `rei_pixbuf` and `wl_buffer.release` is
   sent straight away. Render threads therefore never touch libwayland objects,
   and clients can reuse their buffers immediately. (It costs one memcpy per
   commit. Damage-aware partial copies and dmabuf import, which avoids the copy
   entirely, are future work.)
2. Surface state is double-buffered. Synchronized subsurfaces cache their state
   and apply it when their parent commits, recursively.
3. The commit calls `rei_server_schedule_damage()`. All changes made in one pass
   of the event loop are coalesced into a **single snapshot**, built by
   `rei_scene_build()` as a flat back-to-front list of drawables: wallpaper per
   output, each window followed by its popups, then the cursor.
4. Each render thread draws the newest snapshot with the OpenGL ES renderer
   (`src/render/gles.c`): one textured quad per drawable, premultiplied-alpha
   blending, blending skipped for opaque drawables, textures cached per pixbuf
   and evicted when they leave the scene.
5. When a frame is shown, the render thread posts `REI_EMSG_FRAME` carrying the
   serial of the snapshot it showed. The event thread then fires `wl_surface`
   frame callbacks. A callback records the serial the next snapshot will have at
   commit time and fires only once a frame with that serial or later has been
   presented, so a client is never told about a frame that predates its commit.

The renderer draws into an offscreen framebuffer on EGL's surfaceless platform.
The headless backend discards the result (or writes a screenshot on request), the
nested backend reads it back and attaches it to its parent window, and the DRM
backend will scan it out.

## libwayland and libevent

libevent drives the process. `wl_event_loop_get_fd()` is registered as an
ordinary read event, and each wake-up calls `wl_event_loop_dispatch(loop, 0)`
followed by `wl_display_flush_clients()`.

libwayland also has *idle sources*, which are not backed by an fd, so they only
run when the loop is dispatched for some other reason. Any libevent callback
that changes client-visible state must therefore call
`rei_server_schedule_flush()`. It is coalesced, so calling it liberally is fine.

## Backends

A backend creates outputs (and later, input devices) and registers its own fds
with the server's event loop. Interface: `src/backend/backend.h`.

| backend    | purpose                                              | state |
|------------|------------------------------------------------------|-------|
| `headless` | no display; for tests and GPU-less machines          | done  |
| `nested`   | each output is a window in a parent compositor       | done (frames are read back and attached as shm buffers) |
| `drm`      | real hardware: KMS, libinput, libseat, direct scanout| written, untested on hardware ([DRM.md](DRM.md)) |

`render_ops` on an output are the seam where a backend gives its render thread
somewhere to draw; they all run on the render thread. There are two pacing styles.
*Timer-driven* (nested): the thread simulates vblank, draws into its own
framebuffer and calls `present`. *Vsync-driven* (`render` set; DRM, and headless with
`REI_HEADLESS_VSYNC=1` for tests): the backend draws into its own buffers and queues
a flip; the thread waits for `rei_render_notify_vblank()` (sent when the kernel's
page-flip event is read on the event thread), and only then reports the frame as
presented, so frame callbacks follow the real refresh. `REI_RMSG_PAUSE` stops
drawing across a VT switch.

### The DRM backend (`src/backend/drm/`)

- `drm_logic.c`: pure decisions, unit-tested (`tests/test_drm_logic.c`): connector
  names, refresh rates, mode picking, CRTC assignment by backtracking, plane choice,
  `IN_FORMATS` parsing, direct-scanout eligibility, cursor placement.
- `session.c`: libseat. `drm.c`: device selection (udev, boot GPU first), atomic
  modesetting and flips, GBM swapchains imported into EGL, scanout of client buffers
  with a `TEST_ONLY` check, hardware cursor, hotplug (udev monitor), VT pause/resume.
- `src/input/libinput_input.c`: keyboards and pointers, opened through the session.
- `src/proto/linux_dmabuf.c`: clients' GPU buffers. A surface holds a *lease* on the
  `wl_buffer` it shows; `wl_buffer.release` is sent only after every output has
  presented a frame built after the buffer was replaced.

## Window management (`src/layout.c`, `src/desktop.c`)

**The tiling tree** (`layout.c`) is pure data and geometry with no Wayland in it,
so it is tested exhaustively: unit tests plus 12,000 randomised operations that
check its invariants and that no two windows ever overlap. Inner nodes are
splits (horizontal or vertical), leaves are windows, and each child has a weight
(its share of the parent). Layout gives every window an exact rectangle with the
configured gap between neighbours (shrunk, never overflowing, if there are more
windows than pixels); the outer gap is applied by insetting the area first.
Operations follow i3: a new window opens after the focused one; `split` makes the
next window open inside a new container; `move` swaps past a sibling, dives into a
neighbouring container, or steps out of its own, and wraps the root when no
container runs that way.

**Workspaces and views** (`desktop.c`). A workspace has a global number, belongs
to one output, and holds a tiling tree, a stack of floating windows (bottom to
top), a most-recently-focused list, and at most one fullscreen window. Each output
shows one workspace. `workspace N` shows N on the output that owns it (creating it
on the focused output if new); an empty workspace is destroyed when you leave it.
Floating positions are stored relative to their output, so windows follow it when
the layout changes. When an output goes away its workspaces move to another one.

**A window gets its slot at its first commit**, before it has content, so the first
`configure` already carries the right size and the client never draws at a wrong
size. Tiled windows are told their size and the four *tiled* states (so toolkits
can drop rounded corners); floating ones are told 0x0 (their own size) until the
user resizes them; fullscreen ones the output size. The renderer clips a tiled
window to its slot, so a client that has not yet obeyed a resize can't draw over
its neighbours. Dialogs (a parent, or min size == max size) float on their own.

**Focus** is per workspace (most recent first) with one active window overall,
on the focused output. New windows take focus; clicking focuses; optionally the
pointer does (`focus_follows_mouse`, only on real motion, so a window appearing
under a still pointer doesn't steal focus). Directional focus and move pick the
nearest window that overlaps on the other axis, across tiles and floats, and
continue onto the neighbouring output at the edge.

**Mouse.** `Mod`+left-drag moves and `Mod`+right-drag resizes a floating window
from the corner nearest the pointer; clients can also ask via
`xdg_toplevel.move/resize`. A window dropped on another output joins its
workspace. Windows on a workspace that isn't showing get no frame callbacks, so
clients throttle.

### Toolbar (`src/toolbar.c`, `src/toolbar_render.c`)

One bar per output, along the configured edge. It is drawn by the compositor as an
ordinary opaque drawable below every window (so a fullscreen window covers it) and the
edge it occupies is removed from the output's *usable area*: tiling, floating
placement, the notch pull-back rule and initial centring all read that one area, so
none of them needed to know about the bar.

- **Geometry is font independent.** Workspace buttons are fixed `thickness` x
  `thickness` squares laid from the start of the bar, so hit-testing never depends on
  how text happens to measure. Only the clock's text depends on the font.
- **Text** is rendered with Pango and Cairo on the event thread into a pixbuf at the
  output's physical resolution (Cairo's ARGB32 is already our premultiplied BGRA). A
  vertical bar is too narrow for "14:32", so the clock is broken into stacked parts
  ("14" over "32").
- **Redrawn only when something changes.** Each output caches its last
  `rei_bar_state` (the workspace list and their states, the hovered button, the clock
  text, size, scale, font and colours, built from a zeroed struct) and re-renders only if
  a byte-for-byte comparison differs, so most scene rebuilds reuse the pixbuf.
- **The clock ticks exactly when it must.** The timer is armed for the next moment
  the formatted text differs, found by formatting successive seconds, so `%H:%M` wakes
  once a minute on the minute and `%S` once a second, with no format parsing.
- **Input.** The bar belongs to the compositor like the notches: over it no client has
  the pointer; a workspace button is clicked by press and release on it (the grab ends
  before the workspace changes, as with the notches); the wheel steps through that output's
  workspaces and stops at the ends; and a window above the bar (a floating one) or a
  fullscreen window takes precedence over it.

### Window notches (`src/decoration.c`)

Windows have no border or title bar. The *focused* window gets two tabs that hover
directly above its top edge, drawn by the compositor: a drag handle at the left
and minimize/maximize/close at the right. `decoration.c` is pure geometry,
hit-testing and artwork, so it is unit-tested alone: geometry is a function of the
window rectangle and the notch height, the tabs end exactly at the window's top edge
and never extend past its sides or overlap (a window too narrow for both drops the
handle, then everything), and the artwork is rendered procedurally with 4x4
supersampling, so it is anti-aliased and sharp at any integer output scale (cached
per size, scale and hover state).

- **They never resize anything.** Notches overlay the space above a window and take
  nothing from it. To make room, layout reserves at least the notch height above
  every tiled window whether or not it is focused: the top gap is
  `max(gap_outer, notch)` and the gap between stacked windows is
  `max(gap_inner, notch)` (the tree takes separate horizontal and vertical gaps for
  this). Moving focus therefore changes no sizes.
- **The pull-back rule.** A floating window's top edge can't go higher than the notch
  height from the top of its output, so its tabs never leave the screen. Dragging
  past that just stops there; the corrected position is what is remembered.
- **Input.** Notches are the compositor's: the pointer over one belongs to no client
  (the client gets a `leave`), the hovered part is highlighted, and a button
  counts as clicked only if the press and the release are both on it, so a press can
  be cancelled by sliding off.
- **Drag handle.** For a floating window it is an ordinary move. A tiled window waits
  until the pointer has moved 6px (so a click doesn't disturb it), then lifts out of
  the tree and follows the pointer while the others close up. The window under the
  pointer, and which side of it is nearest, pick the drop slot; a translucent hint
  shows it, and releasing inserts the window beside the target (wrapping it in a new
  split if needed). Released over empty space, the window stays floating. A window
  dropped on another output joins that output's workspace.

## Input (`src/input/`)

Backends feed raw events (evdev key and button codes, absolute or relative
motion, scroll) into `rei_input_*`; `input.c` decides what they mean.

```
backend ─ key ──► xkb state ─► keybinding match ─► action (exec/close/focus/move/workspace/...)
                                    │ no match
                                    └──────────────► keyboard-focused client
backend ─ motion ─► cursor (clamped to outputs) ─► hit-test ─► pointer focus
backend ─ button ─► popup dismissal ─► click-to-focus/raise ─► client (with grab)
```

- **Keymap** comes from libxkbcommon using the `[input]` config (layout, variant,
  options). Clients get it as a memfd on `wl_keyboard`; the compositor tracks
  modifiers itself and sends them after the key that changed them.
- **Keybindings** (`keybindings.c`) are `"Mod+Shift+q" = "close"`-style strings.
  The built-in defaults are merged with the config's `[keybindings]`; `"none"`
  removes one. Matching needs the *exact* modifier set (Caps/Num Lock are
  ignored) and uses the key's unshifted symbol, so `Mod+Shift+1` is the 1 key.
  A matched press is swallowed and so is its release; modifier keys themselves
  are still forwarded. `exec` runs `/bin/sh -c` detached (double fork, so there
  are no zombies) with `WAYLAND_DISPLAY` set; the environment is built before
  forking because a forked child of a multithreaded process may only call
  async-signal-safe functions.
- **Focus.** Keyboard focus is the topmost popup holding a grab (a menu), else
  the active window. Pointer focus is whatever surface is topmost under the
  cursor, honouring input regions and subsurface stacking, and is recomputed on
  every scene change so a window appearing under a stationary pointer gets its
  `enter`. While a button is held the pointer stays with the surface it was
  pressed on (implicit grab). Clicking a window raises and activates it.
- **Popups** that asked for a grab take the keyboard and are told to close
  (`popup_done`) by a click outside them; clicking inside one closes only the
  popups opened after it.
- **Clipboard.** `set_selection` is honoured only from the client holding
  keyboard focus (a background program can't silently replace what you copied),
  and the selection is announced to a client whenever focus moves to it.
- **Move.** `xdg_toplevel.move` with the serial of the latest button press starts
  an interactive drag; the client sees no pointer events until release.

Input backends: **headless** reads a FIFO (`REI_HEADLESS_INPUT`, for tests and
scripting); **nested** forwards the parent's seat, hiding the parent's cursor
and releasing held keys when it loses focus. **libinput** is not here yet: it
needs libseat to open devices without root, so it arrives with the DRM backend
(milestone 8).

### A listener-list pitfall

`wl_signal_emit()` is only safe if a listener removes *itself*. When a client
dies, its `wl_surface` is destroyed before its xdg objects, and the surface's
destroy listeners run while the window is still mapped: unmapping changes
keyboard focus, which unlinks the focus listeners from the very list being
walked, and `wl_signal_emit` then spins forever on a self-linked node. Surface
and data-source destruction therefore use `rei_signal_emit()`
(`util/wlsignal.h`), which detaches each listener before calling it.

## Protocol objects (`src/proto/`)

| global | version | notes |
|---|---|---|
| `wl_compositor` | 6 | surfaces, regions, frame callbacks, buffer scale |
| `wl_subcompositor` | 1 | sync/desync, positioning, z-order |
| `wl_shm` | 1 | ARGB8888, XRGB8888 |
| `xdg_wm_base` | 5 | toplevels, popups, positioners, window geometry |
| `wl_output` | 4 | one per output; removal has a 5 s grace period |
| `wl_seat` | 7 | keyboard and pointer, offered by the backend |
| `wl_data_device_manager` | 3 | clipboard; drag-and-drop is refused (cancelled) for now |

Object lifetimes follow one rule: when a client disconnects, libwayland destroys
its objects in id order, so a parent object can die before the role object that
refers to it. Every cross-object pointer is therefore either guarded by a
destroy listener or cleared by the object that dies first.

## Xwayland (`src/xwayland.c`, `src/proto/xwayland_shell.c`)

rei-wm starts Xwayland itself (`-rootless`, with `-listenfd` sockets it created, so it owns
the `:N` display) and acts as its window manager over a libxcb connection that Xwayland hands
it (`-wm`). Notes from getting it to work:

- Xwayland does not accept X11 clients until the window manager has claimed the `WM_S0`
  selection (and redirected the root's subwindows with Composite); until then clients hang
  silently in connect. Composite's one request is sent by hand to avoid libxcb-composite.
- A managed X11 window (`MapRequest`) is shown once three things are true: it is mapped on the
  X side, its `wl_surface` has been matched to it, and that surface has content. The match uses
  `xwayland_shell_v1`: the surface gets a serial, the X window sends the same serial in a
  `WL_SURFACE_SERIAL` client message. Only the Xwayland client may bind that global.
- `rei_view` has either an `xdg` toplevel or an `xwin`; `desktop.c` dispatches through small
  `view_*` helpers, so X11 windows tile, float, go fullscreen and get notches like native ones.
- X11 coordinates are the global layout coordinates, 1:1 (no scaling for X11 windows).
- Override-redirect windows (menus, tooltips) are not managed: drawn where X11 put them, above
  all windows, and hit-tested before windows.
- Programs launched by keybindings get `DISPLAY` for it. `[general] xwayland = false` turns it off.

## Known gaps and placeholders

These are deliberate and tracked by later milestones:

- **Window management is i3-style but partial.** Splits are horizontal or vertical
  only (no tabbed or stacked containers yet); the weights that size tiles are
  always equal (no resizing tiles from the keyboard); there are no window rules or
  urgency hints.
- **The notch buttons for minimize and maximize are placeholders**: they highlight and
  click but do nothing yet (close works). Notches are drawn only for windows that
  use xdg-shell as toplevels, and are not themable yet (the colours are built in).
  The bigger gap between stacked tiles (the notch height) is the price of keeping
  the lower window's tabs clear of the one above.
- **Hardware input and output are untested.** The DRM and libinput code has never run on a
  real machine (see DRM.md); only its decision logic and the vsync-driven frame path are tested.
- **Interactive move and resize** work for floating windows only. Key repeat is
  done by clients from the `repeat_info` we send (as the protocol intends), not by
  the compositor.
- **Drag and drop** is refused (the source is cancelled). No touch, tablet or
  gesture support; no pointer-constraints or relative-pointer protocol (games).
- **Popups** are positioned from anchor/gravity/offset only: constraint adjustment
  (keeping them on screen) is not done.
- **`wl_surface.damage` is ignored** and `set_buffer_transform` is stored but not
  applied: the whole buffer is re-uploaded and drawn untransformed. Every frame
  redraws the whole output; damage tracking is future work.
- **The toolbar is workspaces and a clock.** No other modules (volume, battery, tray) yet and
  no urgency highlighting; its colours and font are configurable but there are no themes.
- **Xwayland**: no X11 clipboard/selection bridging beyond what Xwayland does itself via
  `wl_data_device` (untested), no HiDPI scaling for X11 windows, no restart if Xwayland dies,
  and `_NET_WM_STATE` is only handled for fullscreen.
- **No server-side decorations** (`xdg-decoration`), `viewporter`
  or fractional scale yet. `linux-dmabuf` exists only on the DRM backend.

## Multi-monitor

Outputs are a first-class list from the start. Each has a position in a global
logical coordinate space (physical size divided by scale). Outputs without a
pinned position are placed left to right in config order.

## Configuration

TOML, parsed by a small built-in reader (`src/util/toml.c`). Values that are
present but invalid are errors, not warnings, so typos are not silently
ignored. The parsed document is retained in `rei_config.root` so later
subsystems (keybindings, toolbar, wallpaper) read their own sections without
changing the core loader. All keybindings will be configurable there.

## Config hot reload (`src/reload.c`)

An inotify watch on the config file's *directory* (so a save-by-rename is seen) plus `SIGHUP`
trigger a reload after a 150 ms settle. A reload parses the whole new file, builds everything
that can fail (keybindings, and the keymap if the layout changed) and only then commits, so an
invalid file changes nothing. Most code reads `server->config` on every use, so swapping the
pointer is enough; superseded configs are kept until exit because a few places may briefly hold
a string from one. Things that cache are refreshed explicitly: bindings and keymap (resent to
clients with the repeat rate), the wallpaper, libinput device settings (through the backend's
`config_changed` hook), and the window layout (`rei_wm_arrange_all`). `[[output]]` and the backend
choice need a restart, and the log says so.

## The application tray and minimize (`src/tray.c`, `src/icons.c`, `src/desktop.c`)

`rei_tray_collect` groups the mapped windows of an output's workspaces by app id (an X11 window's
WM_CLASS; a title stands in when there is none), in order of first appearance. The groups become part
of `rei_bar_state`, so the bar redraws only when a title, count, focus or minimized state changes. The
tile layout (`rei_bar_tray_layout`) is pure and shared by the renderer and by hit-testing: tiles are
centred on the bar between the workspace buttons and the volume/clock, names shrink before tiles are
dropped, and a stack is drawn as up to four tiles peeking out 4 px each. A click on a one-window tile
focuses, restores or (if focused) minimizes it; on a stack it opens a compositor-drawn list
(`rei_tray_menu_*`) that takes the next press, closes on Escape, and is rendered like the bar.

Minimizing keeps a window in its workspace's focus list (so the workspace does not vanish) but takes it
out of the tiling tree and every draw, pick and navigation path (`SHOWN()` in `desktop.c`). A tiled
window records its nearest sibling leaf and the side, and `rei_tree_insert_beside` puts it back there;
if that tile is gone it goes next to the focused window as usual. A floating window just keeps its
`float_rect`. `Mod+m`, the notch button and a client's `xdg_toplevel.set_minimized` all minimize.
Icons: desktop entries are indexed lazily by file name, `StartupWMClass` and the program in `Exec=`;
`Icon=` is looked up in themes as PNG (stb_image), else a hashed-colour letter tile.

### The tray's layout, overflow and interaction

`rei_bar_tray_layout` is pure and shared by the renderer, hit-testing and the window-add path. The tray
lives in the middle three fifths of the bar (clamped clear of the workspace buttons and the
volume/clock). Natural widths are 150 px with names and 30 px icon-only; when the groups do not fit, all
tiles shrink evenly, never below `tray_min_width` percent (and never below 18 px); past that, the largest
number of tiles that fit are kept and the rest go under a 34 px overflow tile. The renderer stores the
layout it drew with the bar's cached picture, so pointer hit-testing and hover reuse it instead of
re-measuring text on every motion.

`src/tray.c` holds the interaction state on the event thread: the user's order of applications (a list of
app ids, new ones appended, rewritten when a drag ends), the hover target with its 500 ms timer, the
tooltip (rendered once, faded by a 16 ms tick that only runs during the fade: the opacity is a
per-drawable `alpha` in the snapshot, applied in the fragment shader), and the press/drag state. Dragging
only starts after 6 px of movement, so a click is still a click. `rei_wm_add_view` asks the toolbar how
many windows would sit under a workspace's overflow tile; at 32 it tries the next workspace number
(creating it) and switches there.

### Themes (`src/theme.c`, `docs/THEME.md`)

One `rei_style` per `rei_element`, defaults derived from the three base colours, overrides from
`[theme.<element>]`. The bar state carries the whole theme (so a theme change redraws the bar), and every
themed element is drawn through the same helpers (`shape_path`, `draw_shadow`, `draw_box`), which is what
keeps the schema general: a new element is a style entry plus a call to `draw_box`.

## Battery (`src/battery.c`)

`rei_battery_read(dir, &info)` is pure apart from reading files, so it is tested against a fake
`power_supply` tree. Every supply with `type = Battery`, not `scope = Device`, and not
`present = 0` counts. Each contributes (charge, capacity) in energy units (`energy_*` in µWh;
`charge_*` in µAh converted with the design voltage; a bare `capacity` percentage as a
100-unit battery), and the percentage is the sum of charge over the sum of capacity. Charging is
"any battery reports Charging". The server polls every 15 s (a timer; the sysfs has no change
notification worth depending on) and damages the scene only if the result changed; the toolbar
state carries percent and charging, so the bar redraws exactly then. The icon is drawn by
`draw_battery`, positioned left of the volume (or the clock), and the tray's range stops short of it.

## Audio (`src/audio.c`)

rei-wm is a native PipeWire client (libpipewire, driven from the libevent loop by watching the
PipeWire loop's fd). It follows the default audio sink: the `default` metadata object that
WirePlumber maintains names it (`default.audio.sink`); without one it uses the highest-priority
`Audio/Sink`. It subscribes to the sink node's `Props` for `channelVolumes` and `mute`, and writes
the same two properties to change them. Volume is on the perceptual (cubic) scale that wpctl and
pavucontrol use, so 50% is a linear gain of 0.125; raising the volume un-mutes. The toolbar's
volume text sits just before the clock (its rectangle is computed by the same Pango layout code
that draws it, so click and wheel hit-testing cannot drift): a click toggles mute, the wheel
changes the volume. `volume_up`, `volume_down [percent]` and `volume_mute` are keybinding actions,
bound to the XF86Audio keys by default. If PipeWire is not running the widget is hidden and
rei-wm retries every 5 seconds. Without libpipewire at build time, `audio_stub.c` makes all of it
inert. Screen capture through PipeWire is not implemented yet.

## Settings app (`settings/`)

Qt 6 Widgets. `TomlDoc` keeps the file as lines and edits values in place, so comments survive;
it understands only the TOML subset the config uses. Each simple option is a widget bound to
`[section] key`; only fields the user changed are written, and `[[output]]` blocks are rewritten
only if the monitors table changed. Before saving, the candidate file is checked by running
`rei-wm --check-config`, so the app can never write something the compositor would refuse. A
missing config is started from `rei-wm --print-default-config`.

## Roadmap

| # | milestone | state |
|---|-----------|-------|
| 0 | scaffold, logging, TOML config, tests | done |
| 1 | event loop, thread boundary, outputs, headless + nested backends | done |
| 2 | protocol baseline (compositor, shm, xdg-shell, seat, output, data-device) | done |
| 3 | EGL/GLES renderer: shm upload, alpha blending, wallpaper, software cursor | done |
| 4 | libxkbcommon, keybinding engine, focus, clipboard, move (libinput moved to 8) | done |
| 5 | tiling tree (H/V splits), floating, fullscreen, gaps, per-output workspaces | done |
| 6 | window notches: close/min/max (right), drag handle (left) | done |
| 7 | toolbar (any edge): workspaces at the start, clock at the end | done |
| 8 | DRM/KMS backend, libseat + libinput, dmabuf, direct scanout, hardware cursor | written; awaiting hardware testing |
| 9 | Xwayland window manager | done |
| 10 | Qt settings application, config hot reload | done |
| 11 | PipeWire volume widget and media keys | done (screencast: later) |

## Testing

`meson test -C build` runs:

- unit tests: TOML, config, output layout, the message queue under concurrent
  producers, rectangle sets, wallpaper fit modes;
- `integration`: launches the real binary, connects as a Wayland client, checks
  redraws across two outputs and clean shutdown;
- `keybindings`: combo/action parsing, config merge and validation, exact-modifier
  matching, real xkb layouts, and a guard that the shipped default config and the
  built-in defaults can't drift apart;
- `input`: keyboard, pointer, focus, bindings, popup grabs, move, clipboard between
  two clients and an abrupt client disconnect, all through the headless FIFO and
  observed from real Wayland clients;
- `toolbar`: bar geometry for all four positions (the bar and window area exactly partition an
  output), button hit-testing, colour mixing, the renderer's pixels (fills, hover, clock placement,
  vertical stacking, scale) and config validation;
- `bar-top`, `bar-bottom`, `bar-left`, `bar-right`, `bar-multi`: the toolbar end to end, once per
  position and once on two outputs: the reduced window area, workspace buttons and highlighting,
  clicking, hover, the wheel, a fullscreen window covering the bar, a ticking clock, and one bar per
  output;
- `drm_logic`: the DRM backend's pure decisions (see above); `*-vsync`: the render, input, wm, notch and
  bar tests again with `REI_HEADLESS_VSYNC=1`, exercising the vsync-driven frame path;
- `reload`: config hot reload end to end (gaps, keybindings, toolbar, an invalid file, SIGHUP);
  `settings-tomldoc` and `settings-window`: the settings app's in-place TOML editor and its window,
  driven offscreen against the real `rei-wm --check-config`;
- `audio`: against a real PipeWire daemon the test starts itself, with a null sink: the media keys
  change volume and mute, raising un-mutes, the volume stops at `max_volume`, and the toolbar redraws;
- `tray`: tiles per application and a stack for several windows, click to focus/hide/restore, the window
  list and Escape, minimize from the keyboard, a restored tile returning to its slot in the grid and a floating
  window to its position; `icons`: desktop-entry and icon-theme lookup; the tray layout is covered in `toolbar`;
- `tray-ui`: hover highlight (immediate) and tooltip (delayed, fading), drag to reorder in both directions,
  the overflow tile and its list, and the 32-window cap that opens the next window on the next workspace;
  `theme`: defaults, overrides and validation, and the renderer applying shape, opacity and shadow;
- `battery`: aggregation of several batteries (energy, charge+voltage, percent-only), ignoring peripherals
  and empty bays; `battery-bar`: the widget on the bar with a fake sysfs, following a change to charging;
- `decoration`: notch geometry, hit-testing, hover/scale variants and artwork pixels;
- `notch`: the notches end to end (focus-only, no resizing, hover, click and cancel,
  handle drag, the pull-back rule, drag-and-drop into a slot with the drop hint,
  fullscreen), asserted from screenshots;
- `layout`: the tiling tree (exact rectangles, gaps, weights, split, move, neighbours) plus
  randomised sequences checking its invariants and non-overlap;
- `wm`: the window manager end to end on two outputs with clients that obey `configure`:
  gaps, splits, moving, floating, fullscreen, workspaces, dialogs, mouse move/resize,
  an abruptly dying tiled client, and shutdown with windows still open, all asserted
  from screenshots;
- `render`: a Wayland client against the real binary on a software GL context.
  It checks the advertised globals and `wl_output` state, maps windows, a
  subsurface and a popup, checks frame callbacks and activation, provokes a
  protocol error from a second client and checks the compositor survives, then
  asks for a screenshot and asserts **exact pixel values** (wallpaper corners,
  opaque and alpha-blended windows, cursor). It is skipped (exit 77) where no
  OpenGL ES is available.

The nested backend is not covered by the automated tests; it was verified by
hand against a headless Weston parent, capturing the parent's screen with
`weston-screenshooter`. The suite also runs clean under sanitizers. Three narrow suppressions cover third-party
libraries only, with their reasons written next to them: `tests/tsan.supp` for a shutdown race
inside Mesa's llvmpipe driver and for Pango's background font thread (glib's futex-based
locks are invisible to TSan), and `tests/lsan.supp` for a rare fontconfig allocation still held
at exit. Races and leaks in rei-wm's own code are still reported. The nested backend's *input forwarding* is not covered by automated
tests (Weston's headless mode has no input to forward); it is written and
compiles but has only been exercised for presentation. CI-style sanitizer builds:

```bash
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson setup build-tsan -Db_sanitize=thread -Db_lundef=false
```

## The Start menu and Run prompt (`src/startmenu.c`, `src/runprompt.c`, `src/popup.c`)

`popup.c` is the generic compositor-drawn list (shared with the tray's window list; one open at a
time, the press that closes or picks is consumed). `startmenu.c` builds the Start list from the
config (shortcuts, Run, then the power unit; power rows need a second click) and handles picks.
`runprompt.c` is the floating prompt: while open it intercepts every key in `rei_input_key`
(VT switching still works), edits a UTF-8 buffer, repeats held keys with timers, blinks a cursor
(`REI_PROMPT_BLINK_MS` for tests), re-renders via `rei_bar_prompt_render` (Pango, `PANGO_WRAP_CHAR`)
and is emitted centred on the focused output; text is refused once the box would exceed 60% of the
output height. Tested by `tests/test_start.c`; the popup logs row centres so tests can click rows.
