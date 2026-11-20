# rei-wm on Void Linux

Install and run notes for Void. **Written without a Void machine to try them on**: the package names below are from memory
of the Void repositories, not checked against them. If `xbps-install` says it cannot find one, look for it with
`xbps-query -Rs <word>`, or find the package that owns a file with `xlocate <file>` (from `xtools`), for instance
`xlocate pkgconfig/xcb-xfixes.pc`. What rei-wm itself needs is certain; only the Void spelling of it is a guess.

## Packages

### To build

```bash
sudo xbps-install -S meson ninja pkg-config gcc git \
    wayland-devel wayland-protocols libevent-devel MesaLib-devel libxkbcommon-devel \
    cairo-devel pango-devel fontconfig-devel libxcb-devel \
    libdrm-devel libinput-devel seatd-devel eudev-libudev-devel \
    pipewire-devel qt6-base-devel
```

| what | package | for |
|---|---|---|
| Meson, Ninja, a C compiler, pkg-config | `meson ninja gcc pkg-config` | building (rei-wm is C11; the settings app is C++17) |
| Wayland | `wayland-devel wayland-protocols` (>= 1.34: `linux-drm-syncobj`) | always |
| libevent | `libevent-devel` | always |
| Mesa (EGL, GLES 3, GBM headers) | `MesaLib-devel` | always (the renderer) and the DRM backend |
| xkbcommon | `libxkbcommon-devel` | always |
| Cairo, Pango, fontconfig | `cairo-devel pango-devel fontconfig-devel` | the toolbar's text and pictures |
| **DRM backend** (needed for a real screen) | `libdrm-devel libinput-devel seatd-devel eudev-libudev-devel` | `-Ddrm=enabled` |
| **Xwayland support** | `libxcb-devel` (it includes `xcb-xfixes`, used for copy and paste with X11 programs) | `-Dxwayland=enabled` |
| **PipeWire volume** | `pipewire-devel` | `-Daudio=enabled` |
| **Settings app** | `qt6-base-devel` | `-Dsettings=enabled` |

If a feature's libraries are missing Meson builds without it, quietly (`auto`). Pass `-Ddrm=enabled -Dxwayland=enabled
-Daudio=enabled -Dsettings=enabled` to make a missing one an error. Check the end of `meson setup`'s output.

### To run

```bash
sudo xbps-install -S mesa-dri xorg-server-xwayland xrdb xkeyboard-config dejavu-fonts-ttf \
    elogind dbus polkit pipewire wireplumber foot
```

| what | package | what is missing without it |
|---|---|---|
| GPU driver | `mesa-dri` (Intel: also `intel-video-accel`; firmware `linux-firmware-intel` / `linux-firmware-amd`) | no EGL/GLES: the compositor cannot draw |
| **Xwayland** | `xorg-server-xwayland` | every X11 program (urxvt, xterm, GTK2) fails with "cannot open display" |
| `xrdb` | `xrdb` | `~/.Xresources` is not loaded into Xwayland (urxvt/xterm colours, fonts, transparency) |
| keyboard layouts | `xkeyboard-config` | no layouts |
| a font | `dejavu-fonts-ttf` (or any) | no text in the toolbar |
| a **seat provider** | `elogind` (with `dbus` and `polkit`), or `seatd` | no access to the screen and input devices |
| a terminal | `foot` (the default of `Mod+Return`; or set `TERMINAL=`) | nothing to type in |
| a cursor theme | e.g. `xcursor-themes` or an Adwaita/Breeze cursor package | only the built-in arrow |
| PipeWire | `pipewire wireplumber` | no volume widget; `pipewire-pulse` is part of `pipewire` and serves programs that speak PulseAudio (Firefox) |
| music | `mpd` (and `mpc` if you like) | no music controls |
| wifi icon's program | `wpa_supplicant` and its GUI (`wpa_gui`, or set `[toolbar] wifi_command`) | the click on the wifi item has nothing to open |
| Firefox | `firefox` | run it with `MOZ_ENABLE_WAYLAND=1` (the session launcher sets it) |

With `[audio] autostart = true` (the default) rei-wm starts `pipewire`, `wireplumber` and `pipewire-pulse` itself when they
are installed and not already running, so no service for them is needed. Brightness keys write to sysfs, else ask
logind over D-Bus (`dbus-send`, from `dbus`), else run `[brightness] command`.

### glibc or musl

Void has both. rei-wm uses `<execinfo.h>` for the backtrace in crash reports; on musl install `libexecinfo-devel` before
`meson setup` (without it the build still works, and crash reports just have no backtrace). Programs for musl that need
glibc (some proprietary ones) are outside rei-wm's concern.

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
`/usr/share/wayland-sessions/rei-wm.desktop` and a menu entry for the settings app. Files go straight into `/usr`, which the
package manager does not know about: to remove them again use `sudo ninja -C build uninstall`. To keep them apart from
`xbps`'s files, use `--prefix=/usr/local` and, for login managers, link the session entry:
`sudo ln -s /usr/local/share/wayland-sessions/rei-wm.desktop /usr/share/wayland-sessions/` (its `Exec=` has the absolute path).

Update by pulling and running `ninja -C build && sudo ninja -C build install` again.

## Services (runit)

Void starts services from `/var/service`; nothing is on until you link it:

```bash
sudo ln -s /etc/sv/dbus    /var/service/
sudo ln -s /etc/sv/elogind /var/service/      # session and seat management (or: seatd, below)
```

rei-wm opens the screen and input devices through **libseat**, which uses `seatd` if it is running and otherwise asks
`elogind` (logind). Pick one:

- **elogind** (what the notes above assume): `dbus` and `elogind` running, and the login must go through PAM with
  `pam_elogind` so that `XDG_RUNTIME_DIR` (`/run/user/<uid>`) is created: check with `echo $XDG_RUNTIME_DIR` and
  `loginctl session-status` after logging in. Void's `elogind` package is meant to set this up in `/etc/pam.d/system-login`;
  if the variable is empty, that file is the place to look (`session optional pam_elogind.so`).
- **seatd** (no elogind): `sudo ln -s /etc/sv/seatd /var/service/`, add yourself to the seat group
  (`sudo usermod -aG _seatd $USER`, or `seatd`, whichever the package created: `getent group | grep seat`), and set
  `XDG_RUNTIME_DIR` yourself, for instance from your shell profile, to a directory you own with mode 700. Without logind,
  the brightness keys' D-Bus fallback and a few session conveniences do not apply.

Be in the `video` and `input` groups only if something complains (`rei-wm --drm-info` lists what the DRM backend sees).

## Starting it

**From a text console** (the simplest, and what to try first): log in on a VT as a normal user (not root) and run
`rei-wm-session`, or add this to the end of `~/.bash_profile` (or `~/.profile`) to start it on tty1 only:

```bash
[ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = /dev/tty1 ] && exec rei-wm-session
```

**From a login manager** that reads `/usr/share/wayland-sessions` (greetd with a greeter, SDDM, ly...): choose **rei-wm**
in its session list, or point it at the command `rei-wm-session`. A minimal greetd setup, for example, is
`/etc/greetd/config.toml` with

```toml
[default_session]
command = "tuigreet --cmd rei-wm-session"
user = "_greeter"   # the user Void's greetd package created
```

plus `sudo ln -s /etc/sv/greetd /var/service/` (and disabling any other manager's service). Void's package names and
service names for these differ in details I could not check, so treat that as the shape of it. LXDM, which lists only
`xsessions`, cannot start a Wayland compositor.

`rei-wm-session` reads your login profile, sets the environment of a Wayland session (`XDG_SESSION_TYPE=wayland`,
`XDG_CURRENT_DESKTOP=rei-wm`, `MOZ_ENABLE_WAYLAND=1`, `QT_QPA_PLATFORM=wayland;xcb`...), refuses to start without a runtime
directory (and says why), and keeps rei-wm's output in `~/.cache/rei-wm/session.log` (the previous run's in
`session.log.1`). Extra options such as `-v` go in `REI_WM_ARGS`, set in `~/.profile` or `~/.config/rei-wm/session-env`. The first
run writes `~/.config/rei-wm/config.toml`; open `rei-settings` for the rest.

## If it does not come up

1. `~/.cache/rei-wm/session.log` (or the terminal, from a console) says what rei-wm saw: the DRM devices, `libseat`
   errors ("no seat", "permission denied"), Xwayland starting, the display's mode.
2. **"XDG_RUNTIME_DIR is not set"**: no logind session (elogind not running, or the login did not go through PAM).
3. **A black screen and an error about EGL, GLES or GBM**: the GPU driver (`mesa-dri`, firmware) or `MesaLib-devel` at build
   time. `rei-wm --drm-info` lists the displays, modes and planes the DRM backend finds.
4. **No keyboard or mouse**: the seat provider. Under elogind `loginctl list-seats` and `loginctl seat-status seat0`; under
   seatd the group above.
5. **X11 programs say "cannot open display"**: `xorg-server-xwayland` is not installed (`command -v Xwayland`), and the log has
   `xwayland: ...` lines saying so. Programs started from rei-wm get `DISPLAY` and `WAYLAND_DISPLAY` set.
6. **No sound widget**: `pipewire` and `wireplumber` are installed? (`command -v pipewire wireplumber`); the log has `audio:` lines.

## What is not done

There is no `xbps-src` template: xbps-src builds from a release tarball with a checksum, and rei-wm has no tagged release
yet. A template is a short file once there is one; until then the build above is the way.
