# rei-wm on Debian and Ubuntu

Install and run notes for Debian 12/13 and Ubuntu 24.04 or newer. **The package names were checked against an Ubuntu
24.04 package index, and rei-wm builds and passes its tests there** (headless, Xwayland, PipeWire, the settings app); it
has not been run on a real screen on Debian or Ubuntu, and Debian 12 / Ubuntu 22.04 were not tried at all, so their
notes below are from the version numbers alone.

## Which release

rei-wm needs **wayland-protocols 1.34 or newer** (the explicit-sync protocol) and libdrm 2.4.109, Wayland 1.20, Qt 6 and Meson 1.0 or newer.

| release | wayland-protocols | |
|---|---|---|
| Ubuntu 24.04 | 1.34 (1.45 in updates) | works as is |
| Debian 13 (trixie) and newer | 1.4x | works as is |
| Debian 12 (bookworm) | 1.31 | too old: take it from `bookworm-backports` or build it, see below |
| Ubuntu 22.04 and older | 1.25 | too old, and so are Meson (0.61) and Qt 6 (missing): not covered here |

## Packages

### To build

```bash
sudo apt install build-essential meson ninja-build pkg-config git \
    libwayland-dev wayland-protocols libevent-dev libegl-dev libgles-dev libgbm-dev libxkbcommon-dev \
    libcairo2-dev libpango1.0-dev libfontconfig-dev \
    libdrm-dev libinput-dev libseat-dev libudev-dev \
    libxcb1-dev libxcb-xfixes0-dev libxcb-cursor-dev libpipewire-0.3-dev qt6-base-dev
```

| what | packages | for |
|---|---|---|
| a C/C++ toolchain, Meson, Ninja | `build-essential meson ninja-build pkg-config` | building (C11; the settings app is C++17) |
| Wayland | `libwayland-dev wayland-protocols` (>= 1.34) | always |
| libevent | `libevent-dev` | always |
| Mesa headers (EGL, GLES 3, GBM) | `libegl-dev libgles-dev libgbm-dev` | always (the renderer) and the DRM backend |
| xkbcommon, Cairo, Pango, fontconfig | `libxkbcommon-dev libcairo2-dev libpango1.0-dev libfontconfig-dev` | keys; the toolbar's text and pictures |
| **DRM backend** (a real screen) | `libdrm-dev libinput-dev libseat-dev libudev-dev` | `-Ddrm=enabled` |
| **Xwayland support** | `libxcb1-dev libxcb-xfixes0-dev`, and optionally `libxcb-cursor-dev` (X windows then show your cursor theme; without it, a plain arrow) | `-Dxwayland=enabled` |
| **PipeWire volume** | `libpipewire-0.3-dev` | `-Daudio=enabled` |
| **Settings app** | `qt6-base-dev` (includes the Test module the tests use) | `-Dsettings=enabled` |

If a feature's libraries are missing Meson quietly builds without it (`auto`); pass `-Ddrm=enabled -Dxwayland=enabled
-Daudio=enabled -Dsettings=enabled` to make that an error.

**Debian 12: a newer wayland-protocols.** Try `sudo apt install -t bookworm-backports wayland-protocols` (check with
`apt-cache policy wayland-protocols` that it is 1.34 or more; this was not checked). Otherwise build it, it is only XML files:

```bash
git clone --branch 1.45 https://gitlab.freedesktop.org/wayland/wayland-protocols.git
cd wayland-protocols
meson setup build --prefix=/usr/local && sudo ninja -C build install
export PKG_CONFIG_PATH=/usr/local/share/pkgconfig   # for the rei-wm `meson setup` below
```

### To run

```bash
sudo apt install mesa-vulkan-drivers libgl1-mesa-dri xwayland x11-xserver-utils xkb-data fonts-dejavu-core \
    libpam-systemd dbus polkitd pipewire wireplumber pipewire-pulse foot
```

(`libgl1-mesa-dri` is the Mesa driver for Intel and AMD; `mesa-vulkan-drivers` is optional. On Debian 12 the polkit package
is `policykit-1`.)

| what | package | what is missing without it |
|---|---|---|
| GPU driver | `libgl1-mesa-dri` (plus `firmware-amd-graphics` / `intel-media-va-driver` from non-free where needed); NVIDIA: see [docs/DRM.md](DRM.md) | no EGL/GLES: the compositor cannot draw |
| **Xwayland** | `xwayland` | every X11 program (urxvt, xterm, GTK2) fails with "cannot open display" |
| `xrdb` | `x11-xserver-utils` | `~/.Xresources` is not loaded into Xwayland (urxvt/xterm colours, fonts) |
| keyboard layouts, a font | `xkb-data`, `fonts-dejavu-core` (or any) | no layouts; no text in the toolbar |
| a **seat provider** | `libpam-systemd` (systemd-logind), or `seatd` | no access to the screen and input devices |
| a terminal | `foot` (the default of `Mod+Return`; or set `TERMINAL=`) | nothing to type in |
| a cursor theme | `adwaita-icon-theme` or `dmz-cursor-theme` | only the built-in arrow |
| PipeWire | `pipewire wireplumber pipewire-pulse` | no volume widget; `pipewire-pulse` serves programs that speak PulseAudio (Firefox) |
| music | `mpd` (and `mpc`) | no music controls |
| the wifi item's program | `wpagui` (`wpa_gui`), or set `[toolbar] wifi_command` | the click has nothing to open |

With `[audio] autostart = true` (the default) rei-wm starts `pipewire`, `wireplumber` and `pipewire-pulse` itself when they
are installed and not already running. Brightness keys write to sysfs, else ask logind over D-Bus, else run `[brightness] command`.

## Build and install

```bash
git clone https://github.com/reilover2606/rei-wm.git
cd rei-wm
meson setup build --prefix=/usr -Ddrm=enabled -Dxwayland=enabled -Daudio=enabled -Dsettings=enabled
ninja -C build
meson test -C build          # optional; the Xwayland and audio tests skip themselves when those are missing
sudo ninja -C build install
```

This installs `/usr/bin/rei-wm`, `rei-settings`, the launcher `rei-wm-session`, the session entry
`/usr/share/wayland-sessions/rei-wm.desktop` and a menu entry for the settings app. The files go straight into `/usr`, which
`dpkg` does not know about: remove them with `sudo ninja -C build uninstall`. To keep them apart from the packages' files use
`--prefix=/usr/local` and link the session entry for login managers:
`sudo ln -s /usr/local/share/wayland-sessions/rei-wm.desktop /usr/share/wayland-sessions/`. Update by pulling and running
`ninja -C build && sudo ninja -C build install` again.

## Seat access

rei-wm opens the screen and input devices through **libseat**: `seatd` if it runs, otherwise systemd-logind.

- **systemd-logind** (the default on Debian and Ubuntu desktops): needs `libpam-systemd` (installed with a desktop;
  `dpkg -s libpam-systemd`) so the login creates `XDG_RUNTIME_DIR` (`/run/user/<uid>`). Check with `echo $XDG_RUNTIME_DIR` and
  `loginctl session-status` after logging in on a text console or from a login manager.
- **seatd** (no logind): `sudo apt install seatd`, `sudo systemctl enable --now seatd`, `sudo usermod -aG _seatd $USER`
  (the group is `_seatd` on Debian and `seat` on some others: `getent group | grep seat`), log in again, and set `XDG_RUNTIME_DIR`
  yourself to a directory you own with mode 700.

Be in the `video` and `input` groups only if something complains (`rei-wm --drm-info` lists what the DRM backend sees).

## Starting it

**From a text console** (try this first): log in on a VT as a normal user and run `rei-wm-session`, or start it on tty1 only
from the end of `~/.bash_profile`:

```bash
[ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = /dev/tty1 ] && exec rei-wm-session
```

**From a login manager** that reads `/usr/share/wayland-sessions` (GDM, SDDM, greetd with a greeter, ly...): pick **rei-wm** in its
session list. Ubuntu's GDM lists it next to "Ubuntu" and "Ubuntu on Xorg". LXDM lists only `xsessions` and cannot start a Wayland
compositor.

`rei-wm-session` reads your login profile, sets the environment of a Wayland session (`XDG_SESSION_TYPE=wayland`,
`XDG_CURRENT_DESKTOP=rei-wm`, `MOZ_ENABLE_WAYLAND=1`, `QT_QPA_PLATFORM=wayland;xcb`...), refuses to start without a runtime
directory (and says why), and keeps rei-wm's output in `~/.cache/rei-wm/session.log` (the previous run's in `session.log.1`).
Extra options such as `-v` go in `REI_WM_ARGS`, set in `~/.profile` or `~/.config/rei-wm/session-env`. The first run writes
`~/.config/rei-wm/config.toml`; open `rei-settings` for the rest.

## Firefox on Ubuntu

Ubuntu's `firefox` is a **snap**, which runs confined: it talks to Wayland fine (the session sets `MOZ_ENABLE_WAYLAND=1`) but
may not see `~/.config` files outside its sandbox, and `xdg-open` handoffs differ. If that bothers you, Mozilla's apt
repository or `firefox-esr` (Debian) are plain packages.

## If it does not come up

1. `~/.cache/rei-wm/session.log` (or the terminal, from a console) says what rei-wm saw: the DRM devices, `libseat` errors
   ("no seat", "permission denied"), Xwayland starting, the display's mode.
2. **"XDG_RUNTIME_DIR is not set"**: no logind session (`libpam-systemd` missing, or `su`/`ssh` login instead of a real one).
3. **A black screen and an error about EGL, GLES or GBM**: the GPU driver (`libgl1-mesa-dri`, firmware) or the `-dev` packages at
   build time. `rei-wm --drm-info` lists the displays, modes and planes the DRM backend finds.
4. **No keyboard or mouse**: the seat provider; `loginctl list-seats` and `loginctl seat-status seat0`, or the `_seatd` group.
5. **X11 programs say "cannot open display"**: `xwayland` is not installed (`command -v Xwayland`).
6. **No sound widget**: `command -v pipewire wireplumber`; the log has `audio:` lines.
7. **`meson setup` says `Dependency "wayland-protocols" not found, tried pkgconfig` / version too old**: see the table at the top.
