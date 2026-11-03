core_sources = files(
  'config.c',
  'config_default.c',
  'cursor.c',
  'decoration.c',
  'layout.c',
  'desktop.c',
  'output.c',
  'pixbuf.c',
  'render_thread.c',
  'scene.c',
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

# Third-party single-header code, compiled without our warning flags.
stb = static_library('stb',
  '../third_party/stb_impl.c',
  c_args: ['-w'],
)

generated = [xdg_shell_client_h, xdg_shell_server_h, xdg_shell_code]

core_deps = [wayland_server, wayland_client, egl, glesv2, xkbcommon, libevent, libevent_pthreads, threads, m_dep]

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
  sources: [xdg_shell_client_h, xdg_shell_server_h],
)

rei_wm = executable('rei-wm', 'main.c',
  dependencies: rei_core_dep,
  install: true,
)
