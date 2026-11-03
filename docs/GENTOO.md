# rei-wm on Gentoo

Package notes collected while getting rei-wm to build and run, plus a draft ebuild.
Atom names are from memory of the Gentoo tree, not checked against it: `eix`/`equery`
will tell you if one has moved (Gentoo renamed `dev-util/meson` to `dev-build/meson`
in 2023, for instance).

## Build dependencies

| what | atom | needed for |
|---|---|---|
| Meson, Ninja, pkg-config | `dev-build/meson` `dev-build/ninja` `virtual/pkgconfig` | building |
| Wayland | `dev-libs/wayland` (>= 1.20) `dev-libs/wayland-protocols` (>= 1.30 for xwayland-shell) | always |
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
| libxcb | `x11-libs/libxcb` | build |
| Xwayland binary | `x11-base/xwayland` | run time |
| **PipeWire volume** | | `-Daudio=enabled` |
| PipeWire | `media-video/pipewire` (provides libpipewire) | build and run |
| WirePlumber | `media-video/wireplumber` | run time (it publishes the default sink) |
| **Settings app** | | `-Dsettings=enabled` |
| Qt 6 | `dev-qt/qtbase:6` with `USE="widgets gui"` | |

If a feature's libraries are missing, Meson silently builds without it (`auto`); pass
`-D<feature>=enabled` to make that an error instead.

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

Things the ebuild does not do yet, for a finished package: a `rei-wm.desktop` file in
`/usr/share/wayland-sessions` (there is none in the tree yet), a tagged release tarball and
`SRC_URI` instead of the live git fetch, running `meson test` as `src_test` (several tests need
a writable `/tmp`, and the Xwayland/audio tests skip themselves when those programs are missing),
and a keyworded `KEYWORDS`.
