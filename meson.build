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
wayland_protocols = dependency('wayland-protocols', version: '>=1.34')
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

# Backtraces in crash reports: glibc has them; musl needs libexecinfo; without either they are left out.
backtrace_deps = []
if cc.has_function('backtrace', prefix: '#include <execinfo.h>')
  add_project_arguments('-DREI_HAVE_BACKTRACE=1', language: 'c')
else
  execinfo = cc.find_library('execinfo', required: false)
  if execinfo.found() and cc.has_function('backtrace', prefix: '#include <execinfo.h>', dependencies: execinfo)
    add_project_arguments('-DREI_HAVE_BACKTRACE=1', language: 'c')
    backtrace_deps = [execinfo]
  endif
endif

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
  if cc.has_function('drmSyncobjEventfd', dependencies: libdrm, prefix: '#include <xf86drm.h>')
    add_project_arguments('-DREI_HAVE_SYNCOBJ_EVENTFD=1', language: 'c')
  endif
endif

# Xwayland support: rei-wm is Xwayland's window manager, speaking X11 through libxcb.
xcb = dependency('xcb', required: get_option('xwayland'))
xcb_xfixes = dependency('xcb-xfixes', required: get_option('xwayland'))
have_xwayland = xcb.found() and xcb_xfixes.found()
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
linux_dmabuf_client_h = scan_client_header.process(linux_dmabuf_xml)
linux_dmabuf_code = scan_code.process(linux_dmabuf_xml)

xwayland_shell_xml = files(protocols_dir / 'staging' / 'xwayland-shell' / 'xwayland-shell-v1.xml')
xwayland_shell_server_h = scan_server_header.process(xwayland_shell_xml)
xwayland_shell_code = scan_code.process(xwayland_shell_xml)

idle_inhibit_xml = files(protocols_dir / 'unstable' / 'idle-inhibit' / 'idle-inhibit-unstable-v1.xml')
idle_inhibit_server_h = scan_server_header.process(idle_inhibit_xml)
idle_inhibit_client_h = scan_client_header.process(idle_inhibit_xml)
idle_inhibit_code = scan_code.process(idle_inhibit_xml)

pointer_constraints_xml = files(protocols_dir / 'unstable' / 'pointer-constraints' / 'pointer-constraints-unstable-v1.xml')
pointer_constraints_server_h = scan_server_header.process(pointer_constraints_xml)
pointer_constraints_client_h = scan_client_header.process(pointer_constraints_xml)
pointer_constraints_code = scan_code.process(pointer_constraints_xml)

relative_pointer_xml = files(protocols_dir / 'unstable' / 'relative-pointer' / 'relative-pointer-unstable-v1.xml')
relative_pointer_server_h = scan_server_header.process(relative_pointer_xml)
relative_pointer_client_h = scan_client_header.process(relative_pointer_xml)
relative_pointer_code = scan_code.process(relative_pointer_xml)

xdg_decoration_xml = files(protocols_dir / 'unstable' / 'xdg-decoration' / 'xdg-decoration-unstable-v1.xml')
xdg_decoration_server_h = scan_server_header.process(xdg_decoration_xml)
xdg_decoration_client_h = scan_client_header.process(xdg_decoration_xml)
xdg_decoration_code = scan_code.process(xdg_decoration_xml)

primary_selection_xml = files(protocols_dir / 'unstable' / 'primary-selection' / 'primary-selection-unstable-v1.xml')
primary_selection_server_h = scan_server_header.process(primary_selection_xml)
primary_selection_client_h = scan_client_header.process(primary_selection_xml)
primary_selection_code = scan_code.process(primary_selection_xml)

linux_drm_syncobj_xml = files(protocols_dir / 'staging' / 'linux-drm-syncobj' / 'linux-drm-syncobj-v1.xml')
linux_drm_syncobj_server_h = scan_server_header.process(linux_drm_syncobj_xml)
linux_drm_syncobj_client_h = scan_client_header.process(linux_drm_syncobj_xml)
linux_drm_syncobj_code = scan_code.process(linux_drm_syncobj_xml)

viewporter_xml = files(protocols_dir / 'stable' / 'viewporter' / 'viewporter.xml')
viewporter_server_h = scan_server_header.process(viewporter_xml)
viewporter_client_h = scan_client_header.process(viewporter_xml)
viewporter_code = scan_code.process(viewporter_xml)

fractional_scale_xml = files(protocols_dir / 'staging' / 'fractional-scale' / 'fractional-scale-v1.xml')
fractional_scale_server_h = scan_server_header.process(fractional_scale_xml)
fractional_scale_client_h = scan_client_header.process(fractional_scale_xml)
fractional_scale_code = scan_code.process(fractional_scale_xml)

cursor_shape_xml = files(protocols_dir / 'staging' / 'cursor-shape' / 'cursor-shape-v1.xml')
cursor_shape_server_h = scan_server_header.process(cursor_shape_xml)
cursor_shape_client_h = scan_client_header.process(cursor_shape_xml)
cursor_shape_code = scan_code.process(cursor_shape_xml)
# cursor-shape-v1 names the tablet tool interface, so its symbols must exist (no tablets are offered).
tablet_xml = files(protocols_dir / 'unstable' / 'tablet' / 'tablet-unstable-v2.xml')
tablet_server_h = scan_server_header.process(tablet_xml)
tablet_client_h = scan_client_header.process(tablet_xml)
tablet_code = scan_code.process(tablet_xml)

inc = include_directories('src')

subdir('src')
subdir('tests')
subdir('settings')
subdir('data')
