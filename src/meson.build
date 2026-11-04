core_sources = files(
  'battery.c',
  'config.c',
  'config_default.c',
  'cursor.c',
  'dmabuf.c',
  'decoration.c',
  'layout.c',
  'desktop.c',
  'output.c',
  'pixbuf.c',
  'reload.c',
  'render_thread.c',
  'scene.c',
  'theme.c',
  'toolbar.c',
  'tray.c',
  'icons.c',
  'toolbar_render.c',
  'server.c',
  'snapshot.c',
  'wallpaper.c',
  'backend/backend.c',
  'backend/headless.c',
  'backend/nested.c',
  'input/actions.c',
  'input/input.c',
  'input/keybindings.c',
  'input/xkb.c',
  'proto/data_device.c',
  'proto/linux_dmabuf.c',
  'proto/seat.c',
  'proto/surface.c',
  'proto/wl_output.c',
  'proto/xdg_shell.c',
  'render/gles.c',
  'util/log.c',
  'util/msgq.c',
  'util/toml.c',
  'util/util.c',
)

if have_drm
  core_sources += files(
    'backend/drm/drm_logic.c',
    'backend/drm/drm.c',
    'backend/drm/drm_info.c',
    'backend/drm/session.c',
    'input/libinput_input.c',
  )
endif

if have_xwayland
  core_sources += files('xwayland.c', 'proto/xwayland_shell.c')
else
  core_sources += files('xwayland_stub.c')
endif

if have_audio
  core_sources += files('audio.c')
else
  core_sources += files('audio_stub.c')
endif

# Third-party single-header code, compiled without our warning flags.
stb = static_library('stb',
  '../third_party/stb_impl.c',
  c_args: ['-w'],
)

generated = [xdg_shell_client_h, xdg_shell_server_h, xdg_shell_code,
  linux_dmabuf_server_h, linux_dmabuf_code, xwayland_shell_server_h, xwayland_shell_code]

drm_deps = have_drm ? [libdrm, gbm, libseat, libinput, libudev] : []
xcb_deps = have_xwayland ? [xcb] : []
audio_deps = have_audio ? [libpipewire] : []
core_deps = drm_deps + xcb_deps + audio_deps + [wayland_server, wayland_client, egl, glesv2, xkbcommon, cairo, pangocairo, fontconfig, libevent, libevent_pthreads, threads, m_dep]

rei_core = static_library('rei-core',
  core_sources, generated,
  include_directories: inc,
  dependencies: core_deps,
  link_with: stb,
)

rei_core_dep = declare_dependency(
  link_with: rei_core,
  include_directories: inc,
  dependencies: core_deps,
  sources: [xdg_shell_client_h, xdg_shell_server_h, linux_dmabuf_server_h, xwayland_shell_server_h],
)

rei_wm = executable('rei-wm', 'main.c',
  dependencies: rei_core_dep,
  link_args: ['-rdynamic'],  # so a crash backtrace shows function names
  install: true,
)
