# Copyright 2026
# Distributed under the terms of the MIT License
#
# DRAFT, NEVER RUN: a live ebuild for rei-wm. See docs/GENTOO.md.

EAPI=8

inherit git-r3 meson

DESCRIPTION="Wayland compositor, tiling/floating window manager and small desktop environment"
HOMEPAGE="https://github.com/reilover2606/rei-wm"
EGIT_REPO_URI="https://github.com/reilover2606/rei-wm.git"

LICENSE="MIT"
SLOT="0"
IUSE="+drm +pipewire +settings test +xwayland"
RESTRICT="!test? ( test )"

DEPEND="
	>=dev-libs/wayland-1.20
	>=dev-libs/wayland-protocols-1.30
	dev-libs/libevent:=
	media-libs/mesa[egl(+),gles2(+),gbm(+)]
	x11-libs/libxkbcommon
	x11-libs/cairo
	x11-libs/pango
	media-libs/fontconfig
	drm? (
		x11-libs/libdrm
		sys-auth/seatd
		dev-libs/libinput:=
		virtual/libudev
	)
	pipewire? ( media-video/pipewire:= )
	settings? ( dev-qt/qtbase:6[gui,widgets] )
	xwayland? ( x11-libs/libxcb )
"
RDEPEND="
	${DEPEND}
	x11-misc/xkeyboard-config
	media-fonts/dejavu
	pipewire? ( media-video/wireplumber )
	xwayland? ( x11-base/xwayland )
"
BDEPEND="
	virtual/pkgconfig
	dev-util/wayland-scanner
"

src_configure() {
	local emesonargs=(
		$(meson_feature drm)
		$(meson_feature pipewire audio)
		$(meson_feature settings)
		$(meson_feature xwayland)
	)
	meson_src_configure
}
