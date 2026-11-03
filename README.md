# rei-wm

A Wayland compositor, tiling/floating window manager and small desktop
environment, written in C11. MIT licensed.

**Status: early development.** Milestones 0–3 are done: the build, config,
event loop and threading model, the core Wayland protocols (surfaces,
subsurfaces, xdg-shell windows and popups, outputs, seat, clipboard objects),
and an OpenGL ES renderer with alpha blending, wallpaper and a cursor. Real
applications can connect and be displayed. There is **no input** yet (no
keyboard, no pointer events), no tiling and no decorations: see the roadmap in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

![Three unmodified Weston demo clients running inside rei-wm](docs/images/milestone-3-demo.png)

*Three unmodified Weston demo clients (one of them OpenGL) inside rei-wm, over a
wallpaper. Windows are alpha-blended; the arrow is rei-wm's own cursor.*

## Building

Dependencies (Debian/Ubuntu package names):

```
meson ninja-build pkg-config libwayland-dev wayland-protocols libevent-dev
libegl-dev libgles-dev
```

Rendering uses OpenGL ES 3 through EGL's surfaceless platform, so it needs a
Mesa (or other) GLES driver but no display server; software rendering
(llvmpipe) works.

```bash
meson setup build
ninja -C build
meson test -C build
```

## Running

```bash
# Inside another Wayland session: each output becomes a window.
./build/src/rei-wm

# Without a display (CI, containers): simulated monitors, no output.
./build/src/rei-wm --backend headless -v
```

Then start any Wayland client against the socket rei-wm prints:

```bash
WAYLAND_DISPLAY=wayland-1 weston-simple-shm
```

In the headless backend nothing is visible, so ask rei-wm for a screenshot:
`kill -USR2 <pid>` writes a PPM per output to `$REI_SCREENSHOT_DIR` (default
`$XDG_RUNTIME_DIR`). `kill -USR1` forces a redraw.

Configuration lives in `~/.config/rei-wm/config.toml` (or
`$XDG_CONFIG_HOME/rei-wm/`). A commented default is written on first run. Use
`rei-wm --check-config` to validate a file, and `--help` for all options.
