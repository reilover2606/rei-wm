project('rei-wm', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

cc = meson.get_compiler('c')

wayland_server = dependency('wayland-server', version: '>=1.20')
wayland_client = dependency('wayland-client', version: '>=1.20')
wayland_protocols = dependency('wayland-protocols', version: '>=1.24')
egl = dependency('egl')
glesv2 = dependency('glesv2')
xkbcommon = dependency('xkbcommon')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
fontconfig = dependency('fontconfig')
libevent = dependency('libevent')
libevent_pthreads = dependency('libevent_pthreads')
threads = dependency('threads')
m_dep = cc.find_library('m', required: false)

# The DRM/KMS backend, for running on real hardware from a TTY. Optional so the
# compositor still builds (headless and nested) on machines without the libraries.
libdrm = dependency('libdrm', version: '>=2.4.109', required: get_option('drm'))
gbm = dependency('gbm', required: get_option('drm'))
libseat = dependency('libseat', required: get_option('drm'))
libinput = dependency('libinput', required: get_option('drm'))
libudev = dependency('libudev', required: get_option('drm'))
have_drm = libdrm.found() and gbm.found() and libseat.found() and libinput.found() and libudev.found()

if have_drm
  add_project_arguments('-DREI_HAVE_DRM=1', language: 'c')
endif

# Xwayland support: rei-wm is Xwayland's window manager, speaking X11 through libxcb.
xcb = dependency('xcb', required: get_option('xwayland'))
have_xwayland = xcb.found()
if have_xwayland
  add_project_arguments('-DREI_HAVE_XWAYLAND=1', language: 'c')
endif

# Volume control through PipeWire: the toolbar's volume widget and the media keys.
libpipewire = dependency('libpipewire-0.3', required: get_option('audio'))
have_audio = libpipewire.found()

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'))
protocols_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')

# Protocol glue. The server header is for the compositor's own xdg-shell
# implementation; the client header is for the nested backend (rei-wm acting as
# a client of a parent compositor) and for the tests.
scan_client_header = generator(wayland_scanner,
  output: '@BASENAME@-client-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@'])
scan_server_header = generator(wayland_scanner,
  output: '@BASENAME@-server-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'])
scan_code = generator(wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'])

xdg_shell_xml = files(protocols_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml')
xdg_shell_client_h = scan_client_header.process(xdg_shell_xml)
xdg_shell_server_h = scan_server_header.process(xdg_shell_xml)
xdg_shell_code = scan_code.process(xdg_shell_xml)

linux_dmabuf_xml = files(protocols_dir / 'unstable' / 'linux-dmabuf' / 'linux-dmabuf-unstable-v1.xml')
linux_dmabuf_server_h = scan_server_header.process(linux_dmabuf_xml)
linux_dmabuf_code = scan_code.process(linux_dmabuf_xml)

xwayland_shell_xml = files(protocols_dir / 'staging' / 'xwayland-shell' / 'xwayland-shell-v1.xml')
xwayland_shell_server_h = scan_server_header.process(xwayland_shell_xml)
xwayland_shell_code = scan_code.process(xwayland_shell_xml)

inc = include_directories('src')

subdir('src')
subdir('tests')
subdir('settings')
