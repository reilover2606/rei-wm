# Sanitizer suppressions for third-party libraries (see tsan.supp and lsan.supp).
# Harmless when the tests aren't built with TSan.
test_env = environment()
test_env.set('TSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'tsan.supp')
test_env.set('LSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'lsan.supp')

test_deps = [rei_core_dep]

foreach t : ['toml', 'config', 'output_layout', 'msgq', 'rects', 'wallpaper', 'keybindings', 'layout', 'decoration', 'toolbar', 'icons', 'theme', 'battery', 'sysinfo', 'flavor']
  exe = executable('test_' + t, 'test_' + t + '.c',
    include_directories: inc,
    dependencies: test_deps,
  )
  test(t, exe, env: test_env)
endforeach

if have_drm
  exe = executable('test_drm_logic', 'test_drm_logic.c',
    include_directories: inc,
    dependencies: test_deps,
  )
  test('drm_logic', exe, env: test_env)
endif

integration = executable('test_integration', 'test_integration.c',
  include_directories: inc,
  dependencies: [wayland_client],
)
test('integration', integration,
  env: test_env,
  args: [rei_wm.full_path()],
  depends: rei_wm,
  timeout: 60,
)

render_test = executable('test_render', 'test_render.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client],
)
test('render', render_test,
  env: test_env,
  args: [rei_wm.full_path()],
  depends: rei_wm,
  timeout: 60,
)

input_test = executable('test_input', 'test_input.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('input', input_test,
  env: test_env,
  args: [rei_wm.full_path()],
  depends: rei_wm,
  timeout: 90,
)

wm_test = executable('test_wm', 'test_wm.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('wm', wm_test,
  args: [rei_wm.full_path()],
  depends: rei_wm,
  env: test_env,
  timeout: 120,
)

notch_test = executable('test_notch', 'test_notch.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('notch', notch_test,
  args: [rei_wm.full_path()],
  depends: rei_wm,
  env: test_env,
  timeout: 120,
)

if have_xwayland
  xwayland_test = executable('test_xwayland', 'test_xwayland.c', xdg_shell_client_h, xdg_shell_code,
    include_directories: inc,
    dependencies: [wayland_client, m_dep, xcb],
  )
  test('xwayland', xwayland_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env,
    timeout: 180)
endif

reload_test = executable('test_reload', 'test_reload.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('reload', reload_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

if have_audio
  audio_test = executable('test_audio', 'test_audio.c', xdg_shell_client_h, xdg_shell_code,
    include_directories: inc,
    dependencies: [wayland_client, m_dep],
  )
  test('audio', audio_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)
endif

tray_test = executable('test_tray', 'test_tray.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('tray', tray_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 120)

tray_ui_test = executable('test_tray_ui', 'test_tray_ui.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('tray-ui', tray_ui_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 240)

close_test = executable('test_close_sequence', 'test_close_sequence.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('close-sequence', close_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

battbar_test = executable('test_battery_bar', 'test_battery_bar.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('battery-bar', battbar_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

bar_test = executable('test_bar', 'test_bar.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
foreach mode : ['top', 'bottom', 'left', 'right', 'multi']
  test('bar-' + mode, bar_test,
    args: [rei_wm.full_path(), mode],
    depends: rei_wm,
    env: test_env,
    timeout: 120,
  )
endforeach

# The same end-to-end tests again, with the outputs driven through the vsync
# path that real displays use (draw, queue a flip, wait for it, then report).
vsync_env = environment()
vsync_env.set('TSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'tsan.supp')
vsync_env.set('LSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'lsan.supp')
vsync_env.set('REI_HEADLESS_VSYNC', '1')
test('render-vsync', render_test, args: [rei_wm.full_path()], depends: rei_wm,
  env: vsync_env, timeout: 90)
test('input-vsync', input_test, args: [rei_wm.full_path()], depends: rei_wm,
  env: vsync_env, timeout: 90)
test('wm-vsync', wm_test, args: [rei_wm.full_path()], depends: rei_wm,
  env: vsync_env, timeout: 120)
test('notch-vsync', notch_test, args: [rei_wm.full_path()], depends: rei_wm,
  env: vsync_env, timeout: 120)
foreach mode : ['top', 'multi']
  test('bar-vsync-' + mode, bar_test, args: [rei_wm.full_path(), mode], depends: rei_wm,
    env: vsync_env, timeout: 120)
endforeach

start_test = executable('test_start', 'test_start.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('start', start_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 120)
