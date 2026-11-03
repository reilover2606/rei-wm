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
| `drm`      | real hardware: KMS, libinput, libseat, direct scanout| milestone 8 |

`render_ops` on an output are the seam where a backend gives its render thread
somewhere to draw: `init`, `present` and `fini` all run on the render thread.

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

## Known gaps and placeholders

These are deliberate and tracked by later milestones:

- **Window management is i3-style but partial.** Splits are horizontal or vertical
  only (no tabbed or stacked containers yet); the weights that size tiles are
  always equal (no resizing tiles from the keyboard); tiled windows can't be
  dragged to a new slot with the mouse (milestone 6); `maximize` and `minimize`
  are placeholders until the decorations exist; there are no window rules or
  urgency hints. Floating windows are not clamped to stay on screen.
- **No real hardware input.** libinput arrives with the DRM backend (milestone 8).
  Until then input comes from the nested parent or the headless FIFO.
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
- **No server-side decorations** (`xdg-decoration`), `linux-dmabuf`, `viewporter`
  or fractional scale yet.

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

## Roadmap

| # | milestone | state |
|---|-----------|-------|
| 0 | scaffold, logging, TOML config, tests | done |
| 1 | event loop, thread boundary, outputs, headless + nested backends | done |
| 2 | protocol baseline (compositor, shm, xdg-shell, seat, output, data-device) | done |
| 3 | EGL/GLES renderer: shm upload, alpha blending, wallpaper, software cursor | done |
| 4 | libxkbcommon, keybinding engine, focus, clipboard, move (libinput moved to 8) | done |
| 5 | tiling tree (H/V splits), floating, fullscreen, gaps, per-output workspaces | done |
| 6 | window notches: close/min/max (right), drag handle (left) | |
| 7 | toolbar (any edge): workspaces left, clock right | |
| 8 | DRM/KMS backend, libseat + libinput, dmabuf, direct scanout, hardware cursor | |
| 9 | Xwayland window manager, cursor override | |
| 10 | Qt settings application, config hot reload | |
| 11 | PipeWire volume widget and media keys | |

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
`weston-screenshooter`. The suite also runs clean under sanitizers (`tests/tsan.supp` suppresses one
shutdown race inside Mesa's llvmpipe driver; races in rei-wm itself are still
reported). The nested backend's *input forwarding* is not covered by automated
tests (Weston's headless mode has no input to forward); it is written and
compiles but has only been exercised for presentation. CI-style sanitizer builds:

```bash
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson setup build-tsan -Db_sanitize=thread -Db_lundef=false
```
