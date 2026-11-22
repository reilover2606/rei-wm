# Sanitizer suppressions for third-party libraries (see tsan.supp and lsan.supp).
# Harmless when the tests aren't built with TSan.
test_env = environment()
test_env.set('TSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'tsan.supp')
test_env.set('LSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'lsan.supp')

test_deps = [rei_core_dep]

foreach t : ['toml', 'config', 'output_layout', 'msgq', 'rects', 'wallpaper', 'keybindings', 'layout', 'decoration', 'toolbar', 'icons', 'theme', 'battery', 'sysinfo', 'flavor', 'wlsignal']
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
  xwscale_test = executable('test_xwscale', 'test_xwscale.c', xdg_shell_client_h, xdg_shell_code,
    include_directories: inc,
    dependencies: [wayland_client, m_dep, xcb],
  )
  xwdeath_test = executable('test_xwdeath', 'test_xwdeath.c', xdg_shell_client_h, xdg_shell_code,
    include_directories: inc,
    dependencies: [wayland_client, m_dep, xcb],
  )
  test('xwdeath', xwdeath_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)
  xwalpha_test = executable('test_xwalpha', 'test_xwalpha.c', xdg_shell_client_h, xdg_shell_code,
    include_directories: inc,
    dependencies: [wayland_client, m_dep, xcb],
  )
  test('xwalpha', xwalpha_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)
  test('xwscale', xwscale_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env,
    timeout: 120)
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

autostart_test = executable('test_autostart', 'test_autostart.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('autostart', autostart_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

fractional_test = executable('test_fractional', 'test_fractional.c', xdg_shell_client_h, xdg_shell_code,
  viewporter_client_h, viewporter_code, fractional_scale_client_h, fractional_scale_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('fractional', fractional_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

msgbuf_test = executable('test_msgbuf', 'test_msgbuf.c',
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('msgbuf', msgbuf_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

watchdog_test = executable('test_watchdog', 'test_watchdog.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('watchdog', watchdog_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)

cursor_test = executable('test_cursor', 'test_cursor.c', cursor_shape_client_h, tablet_client_h,
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('cursor', cursor_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

textfx_test = executable('test_textfx', 'test_textfx.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('textfx', textfx_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 120)

mpd_test = executable('test_mpd', 'test_mpd.c',
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('mpd', mpd_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 120)

hoverfocus_test = executable('test_hoverfocus', 'test_hoverfocus.c',
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('hoverfocus', hoverfocus_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

brightness_test = executable('test_brightness', 'test_brightness.c',
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('brightness', brightness_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

wifi_test = executable('test_wifi', 'test_wifi.c',
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('wifi', wifi_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

wallpapers_test = executable('test_wallpapers', 'test_wallpapers.c',
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('wallpapers', wallpapers_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 120)

displaysleep_test = executable('test_display_sleep', 'test_display_sleep.c', idle_inhibit_client_h, idle_inhibit_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('display-sleep', displaysleep_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

xdgdeco_test = executable('test_xdg_decoration', 'test_xdg_decoration.c', xdg_decoration_client_h, xdg_decoration_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep, rei_core_dep],
)
test('xdg-decoration', xdgdeco_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)

selection_test = executable('test_selection', 'test_selection.c', xdg_shell_client_h, xdg_shell_code,
  primary_selection_client_h, primary_selection_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('selection', selection_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)
if have_xwayland
  xselection_test = executable('test_xselection', 'test_xselection.c', xdg_shell_client_h, xdg_shell_code,
    primary_selection_client_h, primary_selection_code,
    include_directories: inc,
    dependencies: [wayland_client, m_dep, xcb],
  )
  test('xselection', xselection_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 90)
endif

resize_test = executable('test_resize', 'test_resize.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('resize', resize_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)

thin_test = executable('test_thin', 'test_thin.c', xdg_shell_client_h, xdg_shell_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('thin', thin_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)

pointer_lock_test = executable('test_pointer_lock', 'test_pointer_lock.c', xdg_shell_client_h, xdg_shell_code, pointer_constraints_client_h, pointer_constraints_code,
  relative_pointer_client_h, relative_pointer_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('pointer-lock', pointer_lock_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)

dmabuf_fb_test = executable('test_dmabuf_feedback', 'test_dmabuf_feedback.c', xdg_shell_client_h, xdg_shell_code,
  linux_dmabuf_client_h, linux_dmabuf_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('dmabuf-feedback', dmabuf_fb_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)

syncobj_test = executable('test_syncobj', 'test_syncobj.c', xdg_shell_client_h, xdg_shell_code,
  linux_drm_syncobj_client_h, linux_drm_syncobj_code,
  include_directories: inc,
  dependencies: [wayland_client, m_dep],
)
test('syncobj', syncobj_test, args: [rei_wm.full_path()], depends: rei_wm, env: test_env, timeout: 60)
