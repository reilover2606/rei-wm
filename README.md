# rei-wm

A Wayland compositor, tiling/floating window manager and small desktop
environment, written in C11. MIT licensed.

**Status: early development.** Milestones 0–1 are done: the build, config
system, event loop, threading model and output/backend abstraction. There is
no protocol support or rendering yet, so nothing useful can be shown on screen
(see the roadmap in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)).

## Building

Dependencies (Debian/Ubuntu package names):

```
meson ninja-build pkg-config libwayland-dev wayland-protocols libevent-dev
```

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

Configuration lives in `~/.config/rei-wm/config.toml` (or
`$XDG_CONFIG_HOME/rei-wm/`). A commented default is written on first run. Use
`rei-wm --check-config` to validate a file, and `--help` for all options.
