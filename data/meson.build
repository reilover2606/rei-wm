# What a login needs: the session launcher, and the entry the display manager lists.
conf = configuration_data()
conf.set('bindir', get_option('prefix') / get_option('bindir'))

session_script = configure_file(
  input: 'rei-wm-session.in',
  output: 'rei-wm-session',
  configuration: conf,
  install: true,
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)

# Wayland sessions are listed from <datadir>/wayland-sessions (the freedesktop convention that display managers
# and greeters follow); -Dxsession_entry=true also installs one in xsessions, for a display manager that only
# reads that directory.
session_entry = configure_file(
  input: 'rei-wm.desktop.in',
  output: 'rei-wm.desktop',
  configuration: conf,
  install: true,
  install_dir: get_option('datadir') / 'wayland-sessions',
)
if get_option('xsession_entry')
  install_data(session_entry, install_dir: get_option('datadir') / 'xsessions')
endif

if qt6_dep.found()
  configure_file(
    input: 'rei-settings.desktop.in',
    output: 'rei-settings.desktop',
    configuration: conf,
    install: true,
    install_dir: get_option('datadir') / 'applications',
  )
endif

test('session-launcher', find_program('../tests/test_session.sh'), args: [session_script, session_entry], timeout: 30)
