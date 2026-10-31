project(
  'kajo',
  'c',
  version: '0.1.0',
  meson_version: '>= 0.60.0',
  default_options: [
    'c_std=c17',
    'warning_level=0',
    'prefix=~/.local',
  ],
)

add_project_arguments(
  '-Wall',
  '-Wextra',
  '-Wno-unused-parameter',
  language: 'c',
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: true)
pam_dep = cc.find_library('pam', required: true)

gtk4_dep = dependency('gtk4', version: '>= 4.12')
gtk4_wayland_dep = dependency('gtk4-wayland', required: true)
layer_shell_dep = dependency('gtk4-layer-shell-0', 'gtk4-layer-shell', fallback: ['gtk4-layer-shell', 'gtk_layer_shell'])
wayland_client_dep = dependency('wayland-client', required: true)
wayland_protocols_dep = dependency('wayland-protocols', required: true)
json_glib_dep = dependency('json-glib-1.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
pulse_dep = dependency('libpulse')
pulse_glib_dep = dependency('libpulse-mainloop-glib')

wayland_scanner = find_program('wayland-scanner', required: true)
wl_protocols_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
ext_idle_notify_xml = wl_protocols_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml'

ext_idle_notify_header = custom_target(
  'ext-idle-notify-v1-client-protocol.h',
  input: ext_idle_notify_xml,
  output: 'ext-idle-notify-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

ext_idle_notify_code = custom_target(
  'ext-idle-notify-v1-protocol.c',
  input: ext_idle_notify_xml,
  output: 'ext-idle-notify-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

sources = files(
  'src/main.c',
  'src/shell.c',
  'src/panel.c',
  'src/config.c',
  'src/theme.c',
  'src/icons.c',
  'src/autohide.c',
  'src/dbus_helpers.c',
  'src/widgets/registry.c',
  'src/widgets/clock.c',
  'src/widgets/volume.c',
  'src/widgets/workspaces.c',
  'src/widgets/window.c',
  'src/widgets/keyboard-layout.c',
  'src/widgets/battery.c',
  'src/widgets/cpu-mem.c',
  'src/widgets/custom-script.c',
  'src/widgets/power.c',
  'src/widgets/network.c',
  'src/widgets/bluetooth.c',
  'src/widgets/media.c',
  'src/widgets/idle-inhibitor.c',
  'src/widgets/dnd.c',
  'src/widgets/privacy.c',
  'src/widgets/tray.c',
  'src/widgets/launcher.c',
  'src/widgets/notifications.c',
  'src/widgets/control-center.c',
  'src/widgets/theme.c',
  'src/osd/osd.c',
  'src/notifications/daemon.c',
  'src/launcher/launcher_surface.c',
  'src/lock/pam_auth.c',
  'src/lock/screensaver.c',
  'src/lock/session_lock.c',
  'src/lock/idle_manager.c',
  'src/vendor/tinyexpr/tinyexpr.c',
  'src/ipc/socket.c',
  'src/compositor/niri.c',
)

fs = import('fs')
blueprint_compiler = find_program('blueprint-compiler', required: true)

blueprints = files(
  'data/ui/network_popover.blp',
  'data/ui/volume_popover.blp',
  'data/ui/battery_popover.blp',
  'data/ui/privacy_popover.blp',
  'data/ui/cpu_mem_popover.blp',
  'data/ui/bluetooth_popover.blp',
  'data/ui/notifications_popover.blp',
  'data/ui/media_popover.blp',
  'data/ui/control_center_popover.blp',
  'data/ui/theme_popover.blp',
  'data/ui/osd_window.blp',
  'data/ui/launcher_window.blp',
  'data/ui/page_about.blp',
  'data/ui/page_panel.blp',
  'data/ui/page_widgets.blp',
  'data/ui/page_niri.blp',
  'data/ui/page_theme.blp',
  'data/ui/page_notifications.blp',
  'data/ui/page_displays.blp',
  'data/ui/page_network.blp',
  'data/ui/page_bluetooth.blp',
  'data/ui/page_audio.blp',
)

compiled_blueprints = []
foreach bp : blueprints
  compiled_ui = custom_target(
    'bp_' + fs.name(bp),
    input: bp,
    output: '@BASENAME@.ui',
    command: [blueprint_compiler, 'compile', '@INPUT@', '--output', '@OUTPUT@'],
    build_by_default: true,
  )
  compiled_blueprints += compiled_ui
endforeach

gnome = import('gnome')
shell_resources = gnome.compile_resources(
  'kajo-resources',
  'data/resources.gresource.xml',
  source_dir: ['data', meson.current_build_dir()],
  dependencies: compiled_blueprints,
)

executable(
  'kajo',
  sources,
  ext_idle_notify_header,
  ext_idle_notify_code,
  shell_resources,
  dependencies: [
    layer_shell_dep,
    gtk4_dep,
    gtk4_wayland_dep,
    wayland_client_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    pulse_dep,
    pulse_glib_dep,
    pam_dep,
    m_dep,
  ],
  install: true,
)

settings_sources = files(
  'src/settings/main.c',
  'src/settings/settings_window.c',
  'src/settings/settings_common.c',
  'src/settings/pages/page_panel.c',
  'src/settings/pages/page_widgets.c',
  'src/settings/pages/page_niri.c',
  'src/settings/pages/page_displays.c',
  'src/settings/pages/page_network.c',
  'src/settings/pages/page_bluetooth.c',
  'src/settings/pages/page_audio.c',
  'src/settings/pages/page_notifications.c',
  'src/settings/pages/page_theme.c',
  'src/settings/pages/page_about.c',
  'src/config.c',
  'src/theme.c',
  'src/icons.c',
  'src/dbus_helpers.c',
  'src/compositor/niri.c',
)

executable(
  'kajo-settings',
  settings_sources,
  shell_resources,
  dependencies: [
    gtk4_dep,
    layer_shell_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    pulse_dep,
    pulse_glib_dep,
  ],
  install: true,
)

greeter_sources = files(
  'src/greeter/main.c',
  'src/greeter/greetd_ipc.c',
  'src/greeter/greeter_users.c',
  'src/lock/pam_auth.c',
  'src/config.c',
  'src/theme.c',
  'src/icons.c',
)

executable(
  'kajo-greeter',
  greeter_sources,
  shell_resources,
  dependencies: [
    gtk4_dep,
    layer_shell_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    pam_dep,
  ],
  install: true,
)

install_data(
  'data/greetd/kajo-greeter-session',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)

install_data(
  'data/greetd/niri-greeter.kdl',
  install_dir: get_option('datadir') / 'kajo' / 'greetd',
)

install_subdir(
  'data/defaults',
  install_dir: get_option('datadir') / 'kajo' / 'defaults',
  strip_directory: true,
)

install_subdir(
  'data/icons',
  install_dir: get_option('datadir') / 'kajo' / 'icons',
  strip_directory: true,
)

install_data(
  'data/icons/kajo.svg',
  'data/icons/kajo-symbolic.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

# --- Automated Test Suite (meson test -C builddir) ---

test_config_exe = executable(
  'test-config',
  files('tests/test_config.c', 'src/config.c'),
  dependencies: [gio_dep, json_glib_dep],
  build_by_default: false,
)
test('config-roundtrip', test_config_exe)

test_pam_exe = executable(
  'test-pam-auth',
  files('tests/test_pam_auth.c', 'src/lock/pam_auth.c'),
  dependencies: [gio_dep, pam_dep],
  build_by_default: false,
)
test('pam-auth', test_pam_exe)

test_idle_exe = executable(
  'test-idle-manager',
  files('tests/test_idle_manager.c', 'src/lock/idle_manager.c'),
  ext_idle_notify_header,
  ext_idle_notify_code,
  dependencies: [gtk4_dep, gtk4_wayland_dep, wayland_client_dep, gio_dep, gio_unix_dep],
  build_by_default: false,
)
test('idle-manager', test_idle_exe)

test_ipc_exe = executable(
  'test-ipc',
  files('tests/test_ipc.c', 'src/ipc/socket.c'),
  dependencies: [gtk4_dep, gio_dep, gio_unix_dep],
  build_by_default: false,
)
test('ipc-socket', test_ipc_exe)

test_autohide_exe = executable(
  'test-autohide',
  files('tests/test_autohide.c', 'src/autohide.c', 'src/config.c'),
  dependencies: [layer_shell_dep, gtk4_dep, json_glib_dep, gio_dep],
  build_by_default: false,
)
test('autohide-state', test_autohide_exe)

test_greetd_exe = executable(
  'test-greetd-ipc',
  files('tests/test_greetd_ipc.c', 'src/greeter/greetd_ipc.c', 'src/greeter/greeter_users.c'),
  dependencies: [gio_dep, gio_unix_dep, json_glib_dep],
  build_by_default: false,
)
test('greetd-ipc', test_greetd_exe)

widget_sources = files(
  'src/config.c',
  'src/theme.c',
  'src/icons.c',
  'src/dbus_helpers.c',
  'src/compositor/niri.c',
  'src/widgets/registry.c',
  'src/widgets/clock.c',
  'src/widgets/volume.c',
  'src/widgets/workspaces.c',
  'src/widgets/window.c',
  'src/widgets/keyboard-layout.c',
  'src/widgets/battery.c',
  'src/widgets/cpu-mem.c',
  'src/widgets/custom-script.c',
  'src/widgets/power.c',
  'src/widgets/network.c',
  'src/widgets/bluetooth.c',
  'src/widgets/media.c',
  'src/widgets/idle-inhibitor.c',
  'src/widgets/dnd.c',
  'src/widgets/privacy.c',
  'src/widgets/tray.c',
  'src/widgets/launcher.c',
  'src/widgets/notifications.c',
  'src/widgets/control-center.c',
  'src/widgets/theme.c',
)

test_registry_exe = executable(
  'test-registry',
  files('tests/test_registry.c'),
  widget_sources,
  shell_resources,
  dependencies: [
    layer_shell_dep,
    gtk4_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    pulse_dep,
    pulse_glib_dep,
    m_dep,
  ],
  build_by_default: false,
)
test('widget-registry', test_registry_exe)

