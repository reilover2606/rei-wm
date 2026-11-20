# rei-wm on Arch Linux

Install and run notes for Arch (and derivatives: Manjaro, EndeavourOS, CachyOS). **Written without an Arch machine to try
them on**: the package names are from memory of the repositories, not checked, though rei-wm itself builds and passes its
tests on another distribution with the same libraries. If `pacman` cannot find a name, look it up with `pacman -Ss <word>`,
or find what owns a file with `pacman -F <file>` (e.g. `pacman -F xcb-xfixes.pc`). Arch ships current versions of
everything, so none of rei-wm's minimum versions (wayland-protocols 1.34, libdrm 2.4.109, Wayland 1.20, Qt 6, Meson 1.0) is a worry.

## Packages

### To build

```bash
sudo pacman -S --needed base-devel git meson ninja pkgconf \
    wayland wayland-protocols libevent mesa libxkbcommon cairo pango fontconfig \
    libdrm libinput seatd systemd-libs libxcb pipewire qt6-base
```

Arch packages carry their headers, so there are no separate `-dev` packages.

| what | package | for |
|---|---|---|
| toolchain, Meson, Ninja, pkg-config | `base-devel meson ninja pkgconf` | building (C11; the settings app is C++17) |
| Wayland | `wayland wayland-protocols` | always |
| libevent | `libevent` | always |
| Mesa (EGL, GLES 3, GBM) | `mesa` | always (the renderer) and the DRM backend |
| xkbcommon, Cairo, Pango, fontconfig | `libxkbcommon cairo pango fontconfig` | keys; the toolbar's text and pictures |
| **DRM backend** (a real screen) | `libdrm libinput seatd systemd-libs` (libudev comes with `systemd-libs`) | `-Ddrm=enabled` |
| **Xwayland support** | `libxcb` (includes `xcb-xfixes`) | `-Dxwayland=enabled` |
| **PipeWire volume** | `pipewire` (its headers are in the same package) | `-Daudio=enabled` |
| **Settings app** | `qt6-base` | `-Dsettings=enabled` |

If a feature's libraries are missing Meson quietly builds without it (`auto`); pass `-Ddrm=enabled -Dxwayland=enabled
-Daudio=enabled -Dsettings=enabled` to make that an error. Check the end of `meson setup`'s output.

### To run

```bash
sudo pacman -S --needed mesa xorg-xwayland xorg-xrdb xkeyboard-config ttf-dejavu \
    polkit pipewire pipewire-pulse wireplumber foot
```

| what | package | what is missing without it |
|---|---|---|
| GPU driver | `mesa` (Intel and AMD; firmware in `linux-firmware`); NVIDIA: `nvidia-open` (or `nvidia`) and `nvidia-utils`, see [docs/DRM.md](DRM.md) | no EGL/GLES: the compositor cannot draw |
| **Xwayland** | `xorg-xwayland` | every X11 program (urxvt, xterm, GTK2) fails with "cannot open display" |
| `xrdb` | `xorg-xrdb` | `~/.Xresources` is not loaded into Xwayland (urxvt/xterm colours, fonts) |
| keyboard layouts, a font | `xkeyboard-config`, `ttf-dejavu` (or any) | no layouts; no text in the toolbar |
| a **seat provider** | systemd-logind (already there), or `seatd` | no access to the screen and input devices |
| a terminal | `foot` (the default of `Mod+Return`; or set `TERMINAL=`) | nothing to type in |
| a cursor theme | `adwaita-cursors` (or `xcursor-vanilla-dmz`) | only the built-in arrow |
| PipeWire | `pipewire pipewire-pulse wireplumber` | no volume widget; `pipewire-pulse` serves programs that speak PulseAudio (Firefox) |
| music | `mpd` (and `mpc`) | no music controls |
| the wifi item's program | `wpa_supplicant`'s GUI is not in the repositories; set `[toolbar] wifi_command` to your own (`iwgtk`, `nm-connection-editor`, a terminal and `iwctl`...) | the click has nothing to open |

With `[audio] autostart = true` (the default) rei-wm starts `pipewire`, `wireplumber` and `pipewire-pulse` itself when they
are installed and not already running; Arch's packages also ship user units for them (`systemctl --user enable --now pipewire
pipewire-pulse wireplumber`), and rei-wm sees those as already running. Brightness keys write to sysfs, else ask logind over
D-Bus, else run `[brightness] command`.

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
`/usr/share/wayland-sessions/rei-wm.desktop` and a menu entry for the settings app. Files installed this way are unknown to
`pacman`: remove them with `sudo ninja -C build uninstall`. To keep them apart from pacman's files use `--prefix=/usr/local`
and link the session entry for login managers:
`sudo ln -s /usr/local/share/wayland-sessions/rei-wm.desktop /usr/share/wayland-sessions/`.
Update by pulling and running `ninja -C build && sudo ninja -C build install` again.

A proper package is a short `PKGBUILD` (`depends` = the run list, `makedepends` = the build list, `arch-meson` and
`meson install --destdir`); there is none yet because rei-wm has no tagged release to point `source=` at.

## Seat access

rei-wm opens the screen and input devices through **libseat**: `seatd` if it runs, otherwise systemd-logind.

- **systemd-logind** (the Arch default): nothing to set up. The login creates `XDG_RUNTIME_DIR` (`/run/user/<uid>`) through
  `pam_systemd`, which Arch's PAM configuration already has. Check with `echo $XDG_RUNTIME_DIR` and `loginctl session-status`
  after logging in on a text console or from a login manager.
- **seatd** (no logind): `sudo systemctl enable --now seatd`, `sudo usermod -aG seat $USER`, log in again, and set
  `XDG_RUNTIME_DIR` yourself to a directory you own with mode 700.

Be in the `video` and `input` groups only if something complains (`rei-wm --drm-info` lists what the DRM backend sees).

## Starting it

**From a text console** (try this first): log in on a VT as a normal user and run `rei-wm-session`, or start it on tty1 only
from the end of `~/.bash_profile` (or `~/.zprofile`):

```bash
[ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = /dev/tty1 ] && exec rei-wm-session
```

**From a login manager** that reads `/usr/share/wayland-sessions` (SDDM, GDM, greetd with a greeter, ly...): pick **rei-wm** in
its session list; for greetd, `command = "tuigreet --cmd rei-wm-session"` in `/etc/greetd/config.toml`. LXDM lists only
`xsessions` and cannot start a Wayland compositor.

`rei-wm-session` reads your login profile, sets the environment of a Wayland session (`XDG_SESSION_TYPE=wayland`,
`XDG_CURRENT_DESKTOP=rei-wm`, `MOZ_ENABLE_WAYLAND=1`, `QT_QPA_PLATFORM=wayland;xcb`...), refuses to start without a runtime
directory (and says why), and keeps rei-wm's output in `~/.cache/rei-wm/session.log` (the previous run's in `session.log.1`).
Extra options such as `-v` go in `REI_WM_ARGS`, set in `~/.profile` or `~/.config/rei-wm/session-env`. The first run writes
`~/.config/rei-wm/config.toml`; open `rei-settings` for the rest.

## If it does not come up

1. `~/.cache/rei-wm/session.log` (or the terminal, from a console) says what rei-wm saw: the DRM devices, `libseat` errors
   ("no seat", "permission denied"), Xwayland starting, the display's mode.
2. **"XDG_RUNTIME_DIR is not set"**: no logind session (a `su` or `ssh` login instead of a real one).
3. **A black screen and an error about EGL, GLES or GBM**: the GPU driver or firmware. `rei-wm --drm-info` lists the displays,
   modes and planes the DRM backend finds. On NVIDIA, see [docs/DRM.md](DRM.md) (the `nvidia-drm.modeset=1` kernel parameter).
4. **No keyboard or mouse**: the seat provider; `loginctl list-seats` and `loginctl seat-status seat0`, or the `seat` group.
5. **X11 programs say "cannot open display"**: `xorg-xwayland` is not installed (`command -v Xwayland`).
6. **No sound widget**: `command -v pipewire wireplumber`; the log has `audio:` lines.
