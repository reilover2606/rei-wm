test_deps = [rei_core_dep]

foreach t : ['toml', 'config', 'output_layout', 'msgq', 'rects', 'wallpaper']
  exe = executable('test_' + t, 'test_' + t + '.c',
    include_directories: inc,
    dependencies: test_deps,
  )
  test(t, exe)
endforeach

integration = executable('test_integration', 'test_integration.c',
  include_directories: inc,
  dependencies: [wayland_client],
)
test('integration', integration,
  args: [rei_wm.full_path()],
  depends: rei_wm,
  timeout: 60,
)

render_test = executable('test_render', 'test_render.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client],
)
test('render', render_test,
  args: [rei_wm.full_path()],
  depends: rei_wm,
  timeout: 60,
)
