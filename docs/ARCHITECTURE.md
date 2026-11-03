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

## Protocol objects (`src/proto/`)

| global | version | notes |
|---|---|---|
| `wl_compositor` | 6 | surfaces, regions, frame callbacks, buffer scale |
| `wl_subcompositor` | 1 | sync/desync, positioning, z-order |
| `wl_shm` | 1 | ARGB8888, XRGB8888 |
| `xdg_wm_base` | 5 | toplevels, popups, positioners, window geometry |
| `wl_output` | 4 | one per output; removal has a 5 s grace period |
| `wl_seat` | 7 | **no capabilities yet**: plumbing only, until milestone 4 |
| `wl_data_device_manager` | 3 | sources/offers/devices; clipboard delivery waits for keyboard focus |

Object lifetimes follow one rule: when a client disconnects, libwayland destroys
its objects in id order, so a parent object can die before the role object that
refers to it. Every cross-object pointer is therefore either guarded by a
destroy listener or cleared by the object that dies first.

## Known gaps and placeholders

These are deliberate and tracked by later milestones:

- **Window management is a placeholder** (`src/desktop.c`): windows are centred
  on the first output, cascaded 30px, newest on top and activated. No tiling,
  workspaces, focus handling or interactive move/resize (milestones 4–5).
- **The cursor is drawn but fixed** at the centre of the first output; there is
  no pointer input to move it (milestone 4). Client cursor requests are already
  ignored, as designed.
- **Seat has no capabilities** and sends no input events (milestone 4). The
  clipboard objects exist, but `rei_data_device_send_selection()` has no caller
  until keyboard focus does.
- **Popups** are positioned from anchor/gravity/offset only: constraint
  adjustment (keeping them on screen) and `popup.grab` dismissal are not done.
- **`wl_surface.damage` is ignored** and `set_buffer_transform` is stored but not
  applied: the whole buffer is re-uploaded and drawn untransformed.
- **No server-side decorations** (`xdg-decoration`), `linux-dmabuf`,
  `viewporter` or fractional scale yet.
- Every frame redraws the whole output; damage tracking is future work.

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
| 4 | libinput + libxkbcommon, keybinding engine | |
| 5 | tiling tree (H/V splits), floating, gaps, workspaces | |
| 6 | window notches: close/min/max (right), drag handle (left) | |
| 7 | toolbar (any edge): workspaces left, clock right | |
| 8 | DRM/KMS backend, dmabuf, direct scanout, hardware cursor | |
| 9 | Xwayland window manager, cursor override | |
| 10 | Qt settings application, config hot reload | |
| 11 | PipeWire volume widget and media keys | |

## Testing

`meson test -C build` runs:

- unit tests: TOML, config, output layout, the message queue under concurrent
  producers, rectangle sets, wallpaper fit modes;
- `integration`: launches the real binary, connects as a Wayland client, checks
  redraws across two outputs and clean shutdown;
- `render`: a Wayland client against the real binary on a software GL context.
  It checks the advertised globals and `wl_output` state, maps windows, a
  subsurface and a popup, checks frame callbacks and activation, provokes a
  protocol error from a second client and checks the compositor survives, then
  asks for a screenshot and asserts **exact pixel values** (wallpaper corners,
  opaque and alpha-blended windows, cursor). It is skipped (exit 77) where no
  OpenGL ES is available.

The nested backend is not covered by the automated tests; it was verified by
hand against a headless Weston parent, capturing the parent's screen with
`weston-screenshooter`. CI-style sanitizer builds:

```bash
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson setup build-tsan -Db_sanitize=thread -Db_lundef=false
```
