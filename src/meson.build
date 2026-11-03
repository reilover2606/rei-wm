core_sources = files(
  'config.c',
  'config_default.c',
  'output.c',
  'render_thread.c',
  'server.c',
  'snapshot.c',
  'backend/backend.c',
  'backend/headless.c',
  'backend/nested.c',
  'util/log.c',
  'util/msgq.c',
  'util/toml.c',
  'util/util.c',
)

core_deps = [wayland_server, wayland_client, libevent, libevent_pthreads, threads, m_dep]

rei_core = static_library('rei-core',
  core_sources, xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: core_deps,
)

rei_core_dep = declare_dependency(
  link_with: rei_core,
  include_directories: inc,
  dependencies: core_deps,
  sources: xdg_shell_client_h,
)

rei_wm = executable('rei-wm', 'main.c',
  dependencies: rei_core_dep,
  install: true,
)
