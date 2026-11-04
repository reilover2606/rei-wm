# Running rei-wm on real hardware (DRM/KMS)

**This backend has not been run on a real GPU by its author.** It was written
against the libdrm, GBM, libseat and libinput APIs and unit-tested for its
decision logic (mode choice, CRTC assignment, format negotiation, direct-scanout
eligibility, cursor placement) and, through a simulated-vsync headless mode, for
its frame pacing. The code that actually talks to the kernel is untested, so
expect first-run problems and please report what the log says.

## Before you try it

- **Keep a way back in.** Have SSH from another machine (or a phone) ready, or
  know the Ctrl+Alt+F-key for your usual desktop's VT. Input is read directly
  from your keyboard, so if rei-wm hangs you may not be able to type into it.
- Run it **from a text console** (Ctrl+Alt+F3, log in), not from inside a desktop.
  Inside a desktop session use `--backend nested` instead.
- Needs: atomic KMS (any GPU from roughly 2016 on with an open driver: Intel,
  AMD, nouveau, virtio-gpu), GBM + EGL with GLES 3, and either `seatd` or
  `systemd-logind` (you get logind on any systemd distribution, and being logged in
  on a local VT is enough). Proprietary NVIDIA drivers are likely to fail (GBM/EGL
  dma-buf support varies); try `nvidia-drm.modeset=1` and expect rough edges.
- Single GPU only: the one marked boot VGA, or `REI_DRM_DEVICE=/dev/dri/card1`.

Build dependencies for this backend: `libdrm-dev libgbm-dev libseat-dev
libinput-dev libudev-dev`. If any is missing Meson silently builds without the
backend; `meson setup build -Ddrm=enabled` makes that an error instead.

## First steps

```bash
# Safe, read-only: works from a desktop session. Lists GPUs, connectors, modes.
./build/src/rei-wm --drm-info

# On a text console:
REI_LOG=debug ./build/src/rei-wm --backend drm 2> rei.log
```

Config: use the connector names from `--drm-info` in `[[output]]`:

```toml
[[output]]
name = "DP-1"
mode = "2560x1440@144"
x = 0
y = 0

[[output]]
name = "HDMI-A-1"
enabled = false
```

Quit with your quit binding (Mod+Shift+e by default) or `kill -TERM`. Ctrl+Alt+F1..F12
switch VT; rei-wm pauses drawing and gives up the devices, then redraws when you return.

## Troubleshooting switches

| symptom | try |
|---|---|
| "cannot open a seat" | not on a local VT / no logind or seatd: start `seatd` and join its group, or run as root |
| "no keyboard found" | add your user to the `input` group, or use logind/seatd (they grant access) |
| black screen, log shows commits failing | `REI_DRM_NO_SCANOUT=1`; also try a different mode in the config |
| pointer invisible or doubled | `REI_DRM_NO_HW_CURSOR=1` forces a software cursor |
| wrong GPU | `REI_DRM_DEVICE=/dev/dri/cardN` |
| tearing/flicker only with one fullscreen app | `REI_DRM_NO_SCANOUT=1` (direct scanout off) |

Please include `rei.log` (run with `REI_LOG=debug`) and the output of `--drm-info`
in any report.

## How it works

See "Backends" in [ARCHITECTURE.md](ARCHITECTURE.md). In short: each monitor has two
GBM buffers imported into EGL as render targets; the output's render thread draws,
queues an atomic page flip, and the event thread reads the flip-complete event to
pace the next frame. An opaque client dma-buf that exactly covers a monitor is
scanned out directly. The pointer is a hardware cursor when the monitor's scale is 1.

## Known limits

- One GPU; no multi-GPU or PRIME offload.
- No gamma/colour management, VRR, HDR, overlay planes, or runtime mode changes
  (change the config and restart).
- Hotplug adds/removes monitors; a monitor whose resolution you change by
  hand is not renegotiated.
- Touchpad gestures, tablets, touch and key-repeat for the compositor itself are not handled.
- Kinetic touchpad scrolling ends without an `axis_stop`, so some apps may glide.

## If rei-wm crashes

rei-wm now prints a backtrace to stderr when it crashes, so run it with the log redirected
(`./build/src/rei-wm --backend drm 2> rei.log`) and send the end of `rei.log`.

For a crash that looks like memory corruption (for example `corrupted double-linked list`,
`free(): invalid pointer`), the most useful thing is a run of an AddressSanitizer build, which stops at
the exact bad access rather than later:

```bash
meson setup build-asan -Db_sanitize=address -Db_lundef=false
ninja -C build-asan
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./build-asan/src/rei-wm --backend drm -v 2> rei-asan.log
```

then do whatever made it crash and send `rei-asan.log`. (It runs a little slower; that is expected.)
