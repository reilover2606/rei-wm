# The Qt settings application: edits ~/.config/rei-wm/config.toml in place.
qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Core', 'Widgets', 'Test'], required: get_option('settings'))

if qt6_dep.found()
  add_languages('cpp', native: false)

  moc_files = qt6.preprocess(moc_headers: ['mainwindow.h'])

  settings_lib = static_library('rei-settings-core',
    ['tomldoc.cpp', 'mainwindow.cpp', moc_files],
    dependencies: qt6_dep,
    cpp_args: ['-Wall', '-Wextra'],
    override_options: ['cpp_std=c++17'],
  )
  settings_dep = declare_dependency(link_with: settings_lib, dependencies: qt6_dep,
    include_directories: include_directories('.'))

  executable('rei-settings', 'main.cpp',
    dependencies: settings_dep,
    install: true,
    override_options: ['cpp_std=c++17'],
  )

  test_env_qt = environment()
  test_env_qt.set('QT_QPA_PLATFORM', 'offscreen')
  # TSan's runtime aborts on its own (an internal CHECK, not a race in our code) when Qt
  # starts a helper process, so the window test is not run under it.
  tests_to_run = get_option('b_sanitize').contains('thread') ? ['tomldoc'] : ['tomldoc', 'window']
  foreach t : tests_to_run
    exe = executable('test_settings_' + t, 'test_' + t + '.cpp',
      dependencies: settings_dep,
      override_options: ['cpp_std=c++17'],
    )
    test('settings-' + t, exe, env: test_env_qt, args: [rei_wm.full_path()], depends: rei_wm, timeout: 60)
  endforeach
endif
