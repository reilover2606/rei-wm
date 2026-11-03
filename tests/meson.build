# Sanitizer suppressions for third-party libraries (see tsan.supp and lsan.supp).
# Harmless when the tests aren't built with TSan.
test_env = environment()
test_env.set('TSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'tsan.supp')
test_env.set('LSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'lsan.supp')

test_deps = [rei_core_dep]

foreach t : ['toml', 'config', 'output_layout', 'msgq', 'rects', 'wallpaper', 'keybindings', 'layout', 'decoration', 'toolbar']
  exe = executable('test_' + t, 'test_' + t + '.c',
    include_directories: inc,
    dependencies: test_deps,
  )
  test(t, exe, env: test_env)
endforeach

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
