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
| `nested`   | each output is a window in a parent compositor       | done (solid colour only) |
| `drm`      | real hardware: KMS, libinput, libseat, direct scanout| milestone 8 |

`render_ops` on an output are the seam where a backend gives its render thread
somewhere to draw: `init`, `present` and `fini` all run on the render thread.

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
| 2 | protocol baseline (compositor, shm, xdg-shell, seat, output, data-device) | |
| 3 | EGL/GLES renderer: shm upload, alpha blending, wallpaper, software cursor | |
| 4 | libinput + libxkbcommon, keybinding engine | |
| 5 | tiling tree (H/V splits), floating, gaps, workspaces | |
| 6 | window notches: close/min/max (right), drag handle (left) | |
| 7 | toolbar (any edge): workspaces left, clock right | |
| 8 | DRM/KMS backend, dmabuf, direct scanout, hardware cursor | |
| 9 | Xwayland window manager, cursor override | |
| 10 | Qt settings application, config hot reload | |
| 11 | PipeWire volume widget and media keys | |

## Testing

`meson test -C build` runs unit tests (TOML, config, output layout, message
queue under concurrent producers) and an end-to-end test that launches the real
binary, connects to it as a Wayland client, and checks redraws across two
outputs plus clean shutdown. CI-style sanitizer builds:

```bash
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson setup build-tsan -Db_sanitize=thread -Db_lundef=false
```
