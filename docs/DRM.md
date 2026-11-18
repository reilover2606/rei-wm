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

## NVIDIA, Intel, AMD and more than one GPU

The DRM backend picks nothing by vendor: it asks each GPU what it can do. Three things matter on a machine with an NVIDIA GPU
(or two GPUs), and all of them are automatic once the system is set up.

**System setup (NVIDIA proprietary driver).**

- The kernel module needs mode setting: `nvidia-drm.modeset=1` on the kernel command line (and `nvidia-drm.fbdev=1` with driver
  545 and newer, for a working text console). Without it `rei-wm --drm-info` shows `atomic modesetting: NO` for the card and
  rei-wm skips it.
- The driver's GBM and EGL pieces must be installed (`libnvidia-egl-gbm`, `libnvidia-egl-wayland`; most distributions
  install them with the driver).
- A recent driver is recommended: as far as I know NVIDIA's support for explicit synchronization (which its Wayland and EGL code
  needs, since the driver has no implicit synchronization) arrived with the 555 series, so 555 or newer, and 560 or newer is
  safer. This is from memory, not checked. On the kernel side explicit synchronization in rei-wm needs Linux 6.6 or newer
  (below).
- `rei-wm --drm-info` shows, per GPU: the driver, atomic mode setting, framebuffer modifiers, timeline sync objects and the
  sync object eventfd. Send its output with any bug report.

**Explicit synchronization (`wp_linux_drm_syncobj_v1`).** When the kernel has timeline sync objects and their eventfd (Linux 6.6+),
rei-wm offers the protocol, and clients that use it (Mesa's Wayland support, NVIDIA's, Firefox, games) hand over, with each buffer, a
sync object point that is signaled when their GPU work on it is done and one rei-wm signals when it has finished reading it.
rei-wm holds a commit until its acquire point has been signaled (so a buffer is never sampled half drawn) and signals the release
point when no frame still shows the buffer. Clients that do not use the protocol keep today's implicit synchronization; Intel and AMD
drivers have it and are not affected, NVIDIA's did not before 555. It is on by default and can be turned off with
`[drm] explicit_sync = false` to compare. The log says `explicit synchronization (wp_linux_drm_syncobj_v1) is offered`, or why not.

**dmabuf feedback (`zwp_linux_dmabuf_v1` version 4).** Clients are told which GPU is the main one (its render node) and which
buffer layouts (format and modifier) rei-wm can use, instead of guessing, so a program that renders on one GPU allocates buffers
the compositor can import. With more than one GPU in use only the layouts all of them can sample are offered, so a window shows
on every monitor.

**More than one GPU.** `[drm] devices = "auto"` (the default) uses every GPU that has a monitor plugged in, each drawing for
its own monitors and scanning out from its own memory, so a laptop's panel on the integrated GPU and a monitor on the discrete GPU
both work, with no frame copied between GPUs. A GPU with no monitor is opened only to look and then closed again (so a sleeping
discrete GPU is not kept awake); if a monitor is plugged into one later the udev hotplug event makes rei-wm look again.
`devices = "first"` uses one GPU only. What is not done: rendering on one GPU and showing on another's monitor (a copy between
GPUs), and per-surface feedback that points a program at the GPU that drives its monitor.

**Measuring.** With `-v`, every five seconds each output logs `frames on <output>: N in 5.0 s, average spacing X ms, worst Y ms,
Z late`. Compare that line, with the same program doing the same thing, on each GPU or with `explicit_sync` on and off.

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

## When the screen freezes

The DRM backend runs a watchdog: if the event loop stops for 10 seconds (`REI_WATCHDOG_SECONDS`, 0 = off) it
prints every thread's state, the kernel function it is waiting in, and a backtrace of each thread to stderr,
then exits so the console comes back. Run with `2> rei.log` and send the end of the log. If the screen freezes
and *no* watchdog report appears, the whole machine (usually the GPU or kernel) stalled; then
`journalctl -k -b -1 | grep -iE "i915|drm|gpu hang"` after the next boot is the useful evidence.
Bisecting knobs: `REI_DRM_NO_SCANOUT=1` (no direct scanout of client buffers), `REI_DRM_NO_HW_CURSOR=1`,
`REI_DRM_LINEAR=1` (plain linear scanout buffers instead of tiled/compressed ones).

A VT switch (Ctrl+Alt+Fn) is a synchronous call to logind that returns only once the other session has taken over,
which can take more than ten seconds when that session is slow. The watchdog does not count that wait (the
switch suspends it), so rei-wm is no longer exited in the middle of a switch.
