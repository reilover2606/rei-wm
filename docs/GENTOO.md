# rei-wm on Gentoo

Package notes collected while getting rei-wm to build and run, plus a draft ebuild.
Atom names are from memory of the Gentoo tree, not checked against it: `eix`/`equery`
will tell you if one has moved (Gentoo renamed `dev-util/meson` to `dev-build/meson`
in 2023, for instance).

## Build dependencies

| what | atom | needed for |
|---|---|---|
| Meson, Ninja, pkg-config | `dev-build/meson` `dev-build/ninja` `virtual/pkgconfig` | building |
| Wayland | `dev-libs/wayland` (>= 1.20) `dev-libs/wayland-protocols` (>= 1.34, for the explicit-sync protocol) | always |
| libevent | `dev-libs/libevent` | always |
| Mesa (EGL, GLES 3) | `media-libs/mesa` with `USE="egl gles2"` | always (the renderer) |
| xkbcommon | `x11-libs/libxkbcommon` and `x11-misc/xkeyboard-config` | always (keyboard layouts) |
| Cairo, Pango, fontconfig | `x11-libs/cairo` `x11-libs/pango` `media-libs/fontconfig` | toolbar text |
| A font | `media-fonts/dejavu` (or any) | toolbar text |
| **DRM/KMS backend** | | `-Ddrm=enabled` |
| libdrm | `x11-libs/libdrm` | |
| GBM | part of `media-libs/mesa`, needs `USE="gbm"` | |
| libseat | `sys-auth/seatd` (provides libseat; also talks to elogind) | |
| libinput | `dev-libs/libinput` | |
| libudev | `virtual/libudev` (`sys-fs/udev` or `sys-fs/eudev`) | |
| **Xwayland** | | `-Dxwayland=enabled` |
| libxcb | `x11-libs/libxcb` | build (its XFixes part, `xcb-xfixes`, is used for copy and paste with X11 programs) |
| Xwayland binary | `x11-base/xwayland` | run time |
| **PipeWire volume** | | `-Daudio=enabled` |
| PipeWire | `media-video/pipewire` (provides libpipewire) | build and run |
| WirePlumber | `media-video/wireplumber` | run time (it publishes the default sink) |
| **Settings app** | | `-Dsettings=enabled` |
| Qt 6 | `dev-qt/qtbase:6` with `USE="widgets gui"` | |

### Programs that must be installed to run (not just to build)

rei-wm starts and uses these, and carries on without them (with a log line) when they are missing:

| Program | Package | What breaks without it |
|---|---|---|
| **Xwayland** | `x11-base/xwayland` | Every X11 program (urxvt, leafpad, xterm, GTK2 programs) fails with "cannot open display". rei-wm logs `xwayland: the Xwayland process exited ...`. |
| cursor themes | `x11-themes/adwaita-cursors` (or `x11-themes/xcursor-themes`, `x11-themes/vanilla-dmz-aa-xcursors`, ...) | Only the built-in arrow; install one, then pick it in the settings app's Cursor tab. Gentoo puts some under `/usr/share/cursors/xorg-x11`, which rei-wm searches |
| xrdb | `x11-apps/xrdb` | `~/.Xresources` is not loaded into Xwayland (urxvt/xterm ignore their colours, fonts and transparency) |
| seatd or elogind | `sys-auth/seatd` or `sys-auth/elogind` | no access to the display and input devices |
| PipeWire | `media-video/pipewire` | no volume widget |
| WirePlumber | `media-video/wireplumber` | PipeWire without sound devices: no volume widget |
| pipewire-pulse | `media-video/pipewire` with `USE="sound-server"` | programs that speak PulseAudio (Firefox) have no sound |
| a terminal | `gui-apps/foot` (the default `Mod+Return`) | nothing to type in |

With `[audio] autostart = true` (the default) rei-wm starts `pipewire`, `wireplumber` and `pipewire-pulse`
itself if they are installed and not already running.

If a feature's libraries are missing, Meson silently builds without it (`auto`); pass
`-D<feature>=enabled` to make that an error instead.

## Installing, and starting it from a display manager (LXDM)

```bash
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
```

This installs `rei-wm`, `rei-settings` (if Qt 6 was found), the launcher `rei-wm-session`, a session entry
`/usr/share/wayland-sessions/rei-wm.desktop` and a menu entry for the settings app. `rei-wm` is then findable by
the display manager as a session called **rei-wm**. (`--prefix=/usr/local`, Meson's default, puts the entry in
`/usr/local/share/wayland-sessions`, which display managers do not read: use `/usr` for the session entry.)

`rei-wm-session` is what the entry runs. It reads your login profile (`~/.profile`, `.bash_profile`...), sets
`XDG_SESSION_TYPE=wayland`, `XDG_CURRENT_DESKTOP=rei-wm`, `MOZ_ENABLE_WAYLAND=1`, `QT_QPA_PLATFORM=wayland;xcb`
and a few more, checks that the login has a runtime directory, and starts `rei-wm` with its output going to
`~/.cache/rei-wm/session.log` (the previous run's is `session.log.1`). Extra options for rei-wm, such as `-v`
for a debug log, go in `REI_WM_ARGS`, set in `~/.profile` or in `~/.config/rei-wm/session-env`. Everything else
(backend, the keyboard, the wallpaper, autostart, `[general] session_bus = "private"` if programs need a D-Bus
session bus) is the usual config file, created on first run.

What has to be in place:

- **A login session with seat access.** rei-wm opens the screen and the input devices through libseat, which
  asks elogind (or systemd-logind) for them. LXDM must log you in through PAM with `pam_elogind` (Gentoo's
  `/etc/pam.d/lxdm` includes `system-login`, which has it when elogind is the session provider), and elogind
  must be running (`rc-update add elogind boot`). `loginctl session-status` in the session shows it.
- **A display manager that lists `wayland-sessions`.** The entry follows the freedesktop convention. If yours
  only reads `xsessions`, `meson configure build -Dxsession_entry=true` also installs one there; but the
  display manager then starts an X server first and rei-wm cannot take the screen from it, so this is only worth
  trying with a display manager that hands the VT over itself.
- If the session returns to the login screen at once, `~/.cache/rei-wm/session.log` says why (and
  `journalctl`/`/var/log/messages` for the launcher's own complaint about a missing `XDG_RUNTIME_DIR`).

I could not run a display manager where this was written: the entry and launcher are tested (the launcher runs,
sets the environment, starts the compositor and writes its log; the entry is well-formed), but picking it in
LXDM itself is untested.

## USE flags worth knowing about

Learned the hard way, for the programs you want to run *inside* rei-wm:

- `media-libs/mesa`: `wayland` (clients need `libwayland-egl`: Firefox reported
  `glxtest: missing wayland libraries` without it), plus `gbm`, `egl`, `gles2`.
- `x11-libs/gtk+:3` needs `USE="wayland"`. Without it every GTK3 program (pavucontrol,
  Firefox's toolkit, ...) has no Wayland backend, and falls back to X11 on whatever
  `DISPLAY` points at (your desktop's X server, if you leave it set).
- `www-client/firefox` (or `firefox-bin`): `USE="wayland"`; set `MOZ_ENABLE_WAYLAND=1` if it
  still starts on X11.
- GTK2 programs (leafpad) and wxGTK programs built on GTK2 are X11-only and need Xwayland.
- `media-video/pipewire`: `USE="sound-server"` plus a session manager (WirePlumber), and
  the daemon running in your session.

```
# /etc/portage/package.use/rei-wm
media-libs/mesa       wayland gbm egl gles2
x11-libs/gtk+         wayland
www-client/firefox    wayland
```

## Running on a text console

- Log in as a normal user on a VT; do not run as root (GTK/Firefox misbehave, and
  `XDG_RUNTIME_DIR` will be `/run/user/0`).
- A *seat* provider must be running: `elogind` (`rc-service elogind start`, or via systemd) or
  `seatd` (`rc-service seatd start`; add yourself to the `seat` group). rei-wm tries seatd
  first and logind second.
- `XDG_RUNTIME_DIR` must be set for your user (elogind/PAM does that). If `echo
  $XDG_RUNTIME_DIR` is empty, programs cannot find rei-wm's socket.
- The first run writes `~/.config/rei-wm/config.toml`; set a terminal with `TERMINAL=foot` or
  rebind `Mod+Return`.

## Build by hand

```bash
emerge --ask --onlydeps gui-wm/rei-wm   # once the ebuild below exists; else install the atoms above
meson setup build -Ddrm=enabled -Dxwayland=enabled -Daudio=enabled -Dsettings=enabled
ninja -C build
meson test -C build
```

## Draft ebuild

`dist/gentoo/rei-wm-9999.ebuild` is a *live* ebuild (builds the current git head). **It has
never been run**; treat it as a starting point. To try it from a local overlay:

```bash
mkdir -p /var/db/repos/local/{metadata,profiles,gui-wm/rei-wm}
echo 'local' > /var/db/repos/local/profiles/repo_name
printf 'masters = gentoo\nauto-sync = false\n' > /var/db/repos/local/metadata/layout.conf
cp dist/gentoo/rei-wm-9999.ebuild /var/db/repos/local/gui-wm/rei-wm/
# /etc/portage/repos.conf/local.conf:
#   [local]
#   location = /var/db/repos/local
echo 'gui-wm/rei-wm **' >> /etc/portage/package.accept_keywords/rei-wm   # live ebuilds are unkeyworded
emerge --ask gui-wm/rei-wm
```

Things the ebuild does not do yet, for a finished package: a tagged release tarball and
`SRC_URI` instead of the live git fetch, running `meson test` as `src_test` (several tests need
a writable `/tmp`, and the Xwayland/audio tests skip themselves when those programs are missing),
and a keyworded `KEYWORDS`.
