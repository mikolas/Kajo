project(
  'kajo',
  ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=c17',
    'cpp_std=c++17',
    'warning_level=0',
    'prefix=~/.local',
  ],
)

add_project_arguments(
  '-Wall',
  '-Wextra',
  '-Wno-unused-parameter',
  '-DKAJO_SOURCE_DIR="' + meson.current_source_dir() + '"',
  '-DKAJO_DATA_DIR="' + (get_option('prefix') / get_option('datadir') / 'kajo') + '"',
  language: ['c', 'cpp'],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: true)
pam_dep = cc.find_library('pam', required: true)

gtk4_dep = dependency('gtk4', version: '>= 4.12')
gtk4_wayland_dep = dependency('gtk4-wayland', required: true)
layer_shell_dep = dependency('gtk4-layer-shell-0', 'gtk4-layer-shell', fallback: ['gtk4-layer-shell', 'gtk_layer_shell'])
wayland_client_dep = dependency('wayland-client', required: true)
wayland_protocols_dep = dependency('wayland-protocols', required: true)
xkbcommon_dep = dependency('xkbcommon', required: true)
json_glib_dep = dependency('json-glib-1.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
pulse_dep = dependency('libpulse')
pulse_glib_dep = dependency('libpulse-mainloop-glib')
poppler_dep = dependency('poppler-glib')
archive_dep = dependency('libarchive')
mpv_dep = dependency('mpv')
egl_dep = dependency('egl')
glesv2_dep = dependency('glesv2')
gtksourceview_dep = dependency('gtksourceview-5')
uade_dep = dependency('libuade')
bencode_dep = cc.find_library('bencodetools', required: true)
sidplayfp_dep = dependency('libsidplayfp')
stilview_dep = dependency('libstilview')
gme_dep = dependency('libgme')
openmpt_dep = dependency('libopenmpt')
xmp_dep = dependency('libxmp')

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

vkbd_xml = files('protocol/virtual-keyboard-unstable-v1.xml')

vkbd_header = custom_target(
  'virtual-keyboard-unstable-v1-client-protocol.h',
  input: vkbd_xml,
  output: 'virtual-keyboard-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

vkbd_code = custom_target(
  'virtual-keyboard-unstable-v1-protocol.c',
  input: vkbd_xml,
  output: 'virtual-keyboard-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

im_xml = files('protocol/input-method-unstable-v2.xml')

im_header = custom_target(
  'input-method-unstable-v2-client-protocol.h',
  input: im_xml,
  output: 'input-method-unstable-v2-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

im_code = custom_target(
  'input-method-unstable-v2-protocol.c',
  input: im_xml,
  output: 'input-method-unstable-v2-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

sources = files(
  'src/main.c',
  'src/shell.c',
  'src/panel.c',
  'src/autohide.c',
  'src/dbus_helpers.c',
  'src/audio/pa_conn.c',
  'src/widgets/registry.c',
  'src/widgets/clock.c',
  'src/widgets/volume.c',
  'src/widgets/workspaces.c',
  'src/widgets/window.c',
  'src/widgets/keyboard-layout.c',
  'src/widgets/battery.c',
  'src/widgets/backlight.c',
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
  'src/widgets/osk.c',
  'src/osd/osd.c',
  'src/osk/osk_xkb.c',
  'src/osk/osk.c',
  'src/notifications/daemon.c',
  'src/launcher/launcher_surface.c',
  'src/lock/pam_auth.c',
  'src/lock/screensaver.c',
  'src/lock/session_lock.c',
  'src/lock/idle_manager.c',
  'src/hardware/hw_caps.c',
  'src/hardware/kbd_backlight.c',
  'src/hardware/touch_gestures.c',
  'src/posture/posture_manager.c',
  'src/vendor/tinyexpr/tinyexpr.c',
  'src/ipc/socket.c',
  'src/ipc/ipc_client.c',
  'src/compositor/niri.c',
)

fs = import('fs')
blueprint_compiler = find_program('blueprint-compiler', required: true)

blueprints = files(
  'data/ui/network_popover.blp',
  'data/ui/volume_popover.blp',
  'data/ui/battery_popover.blp',
  'data/ui/backlight_popover.blp',
  'data/ui/privacy_popover.blp',
  'data/ui/cpu_mem_popover.blp',
  'data/ui/bluetooth_popover.blp',
  'data/ui/notifications_popover.blp',
  'data/ui/media_popover.blp',
  'data/ui/control_center_popover.blp',
  'data/ui/theme_popover.blp',
  'data/ui/osd_window.blp',
  'data/ui/launcher_window.blp',
  'data/ui/files_window.blp',
  'data/ui/player_window.blp',
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

kajo_config_sources = files(
  'src/vendor/tomlc17/tomlc17.c',
  'src/config/kajo_config_schema.c',
  'src/config/kajo_config_comp.c',
  'src/config/kajo_config_migrate.c',
  'src/config/kajo_config_store.c',
  'src/config/schemas/schema_theme.c',
  'src/config/schemas/schema_shell.c',
  'src/config/schemas/schema_ide.c',
  'src/config/schemas/schema_files.c',
  'src/config/schemas/schema_mod.c',
)

# Sources shared by every executable are compiled once instead of once per binary.
kajo_core = static_library(
  'kajo-core',
  kajo_config_sources + files(
    'src/theme.c',
    'src/icons.c',
    'src/compositor/niri.c',
    'src/compositor/kajo_comp_ipc.c',
    'src/app_init.c',
  ),
  dependencies: [gtk4_dep, json_glib_dep, gio_dep, gio_unix_dep, m_dep],
)

executable(
  'kajo',
  sources,
  ext_idle_notify_header,
  ext_idle_notify_code,
  vkbd_header,
  vkbd_code,
  im_header,
  im_code,
  shell_resources,
  dependencies: [
    layer_shell_dep,
    gtk4_dep,
    gtk4_wayland_dep,
    wayland_client_dep,
    xkbcommon_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    pulse_dep,
    pulse_glib_dep,
    pam_dep,
    m_dep,
  ],
  link_with: kajo_core,
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
  'src/audio/pa_conn.c',
  'src/settings/pages/page_notifications.c',
  'src/settings/pages/page_theme.c',
  'src/settings/pages/page_ide.c',
  'src/settings/pages/page_mod.c',
  'src/settings/pages/page_about.c',
  'src/dbus_helpers.c',
  'src/hardware/hw_caps.c',
  'src/hardware/kbd_backlight.c',
  'src/ipc/ipc_client.c',
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
    m_dep,
  ],
  link_with: kajo_core,
  install: true,
)

greeter_sources = files(
  'src/greeter/main.c',
  'src/greeter/greetd_ipc.c',
  'src/greeter/greeter_users.c',
  'src/lock/pam_auth.c',
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
  link_with: kajo_core,
  install: true,
)

files_sources = files(
  'src/files/main.c',
  'src/files/files_window.c',
  'src/files/files_window_ctxmenu.c',
  'src/files/files_window_keys.c',
  'src/files/files_model.c',
  'src/files/files_ops.c',
  'src/files/files_diskimage.c',
  'src/files/files_sidebar.c',
  'src/files/files_quicklook.c',
  'src/common/pdf_render.c',
  'src/player/player_mpv.c',
  'src/compositor/niri.c',
)

executable(
  'kajo-files',
  files_sources,
  shell_resources,
  dependencies: [
    gtk4_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    poppler_dep,
    archive_dep,
    mpv_dep,
    egl_dep,
    glesv2_dep,
    m_dep,
  ],
  link_with: kajo_core,
  install: true,
)

install_data(
  'data/org.kajo.Files.desktop',
  'data/org.kajo.Settings.desktop',
  install_dir: get_option('datadir') / 'applications',
)

player_sources = files(
  'src/player/main.c',
  'src/player/player_window.c',
  'src/player/player_mpv.c',
  'src/player/player_queue.c',
  'src/player/player_mpris.c',
  'src/compositor/niri.c',
)

executable(
  'kajo-player',
  player_sources,
  shell_resources,
  dependencies: [
    gtk4_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    mpv_dep,
    egl_dep,
    glesv2_dep,
    m_dep,
  ],
  link_with: kajo_core,
  install: true,
)

install_data(
  'data/org.kajo.Player.desktop',
  'data/org.kajo.Mod.desktop',
  install_dir: get_option('datadir') / 'applications',
)

mod_engine_sources = files(
  'src/mod/mod_archive.c',
  'src/mod/mod_engine.c',
  'src/mod/mod_backend_uade.c',
  'src/mod/mod_backend_sid.cpp',
  'src/mod/mod_backend_gme.c',
  'src/mod/mod_backend_openmpt.c',
  'src/mod/mod_audio.c',
  'src/mod/mod_playlist.c',
)

mod_engine_deps = [
  gio_dep,
  gio_unix_dep,
  archive_dep,
  pulse_dep,
  uade_dep,
  bencode_dep,
  sidplayfp_dep,
  stilview_dep,
  gme_dep,
  openmpt_dep,
  xmp_dep,
  m_dep,
]

mod_sources = files(
  'src/mod/main.c',
  'src/mod/mod_window.c',
  'src/mod/mod_vis.c',
) + mod_engine_sources

executable(
  'kajo-mod',
  mod_sources,
  shell_resources,
  dependencies: [
    gtk4_dep,
    json_glib_dep,
  ] + mod_engine_deps,
  link_with: kajo_core,
  install: true,
)

# Tree-sitter syntax engine (optional: each grammar degrades to the heuristic
# lexer when its library is missing). Grammars without headers/.pc files
# (go) are declared by hand in ide_ts.c and located with find_library().
ts_core_dep = dependency('tree-sitter', required: false)
ts_defs = []
ts_link_deps = [ts_core_dep]
if ts_core_dep.found()
  ts_defs += '-DHAVE_TREE_SITTER=1'
  foreach lang : ['c', 'java', 'go', 'cpp', 'rust', 'bash', 'javascript', 'typescript', 'tsx', 'json', 'toml', 'yaml', 'css', 'html', 'lua', 'm68k']
    lib = cc.find_library('tree-sitter-' + lang, required: false)
    if lib.found()
      ts_defs += '-DHAVE_TS_' + lang.to_upper() + '=1'
      ts_link_deps += lib
    endif
  endforeach
endif
ide_ts_lib = static_library(
  'kajo-ide-ts',
  files('src/ide/ide_ts.c', 'src/ide/ide_ts_queries.c'),
  dependencies: [gio_dep] + ts_link_deps,
  c_args: ts_defs,
)
ide_ts_dep = declare_dependency(
  link_with: ide_ts_lib,
  dependencies: ts_link_deps,
)

ide_sources = files(
  'src/ide/main.c',
  'src/ide/ide_window.c',
  'src/ide/ide_actions.c',
  'src/ide/ide_fsm.c',
  'src/ide/ide_buffer.c',
  'src/ide/ide_syntax.c',
  'src/ide/ide_gutter.c',
  'src/ide/ide_tree.c',
  'src/ide/ide_picker.c',
  'src/ide/ide_search.c',
  'src/ide/ide_tasks.c',
  'src/ide/ide_native_exe.c',
  'src/ide/ide_lsp.c',
  'src/ide/ide_lsp_client.c',
  'src/ide/ide_jsonrpc.c',
  'src/ide/ide_lsp_servers.c',
  'src/ide/ide_dap.c',
  'src/ide/ide_dap_client.c',
  'src/ide/ide_dap_launch.c',
  'src/ide/ide_dap_java.c',
  'src/ide/ide_agent_ipc.c',
  'src/ide/ide_markdown.c',
  'src/ide/ide_mermaid.c',
  'src/common/pdf_render.c',
  'src/compositor/niri.c',
)

executable(
  'kajo-ide',
  ide_sources,
  shell_resources,
  dependencies: [
    gtk4_dep,
    gtksourceview_dep,
    json_glib_dep,
    gio_dep,
    gio_unix_dep,
    poppler_dep,
    archive_dep,
    m_dep,
    ide_ts_dep,
  ],
  link_with: kajo_core,
  install: true,
)

install_data(
  'data/org.kajo.IDE.desktop',
  install_dir: get_option('datadir') / 'applications',
)

install_data(
  'data/session/kajo-session',
  'data/greetd/kajo-greeter-session',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)

install_data(
  'data/greetd/comp-greeter.toml',
  'data/greetd/niri-greeter.kdl',
  install_dir: get_option('datadir') / 'kajo' / 'greetd',
)

install_data(
  'data/session/kajo.desktop',
  'data/session/kajo-debug.desktop',
  'data/session/kajo-niri.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
)

install_data(
  'data/session/kajo-portals.conf',
  install_dir: get_option('datadir') / 'xdg-desktop-portal',
)

install_data(
  'data/systemd/kajo-comp.service',
  'data/systemd/kajo.service',
  'data/systemd/kajo-shutdown.target',
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
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

install_subdir(
  'data/shaders',
  install_dir: get_option('datadir') / 'kajo' / 'shaders',
  strip_directory: true,
)

install_subdir(
  'data/ide',
  install_dir: get_option('datadir') / 'kajo' / 'ide',
  strip_directory: true,
)

install_data(
  'data/icons/kajo.svg',
  'data/icons/kajo-symbolic.svg',
  'data/icons/kajo-comp.svg',
  'data/icons/kajo-ide.svg',
  'data/icons/kajo-files.svg',
  'data/icons/kajo-player.svg',
  'data/icons/kajo-mod.svg',
  'data/icons/kajo-settings.svg',
  'data/icons/kajo-greeter.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

# --- Automated Test Suite (meson test -C builddir) ---

test_config_exe = executable(
  'test-config',
  files('tests/test_config.c') + kajo_config_sources,
  dependencies: [gio_dep, json_glib_dep, m_dep],
  build_by_default: false,
)
test('config-roundtrip', test_config_exe,
  env: {'KAJO_SOURCE_ROOT': meson.project_source_root()})

test_fuzz_toml_exe = executable(
  'test-fuzz-toml-config',
  files('tests/fuzz/fuzz_toml_config.c') + kajo_config_sources,
  dependencies: [gio_dep, json_glib_dep, m_dep],
  build_by_default: false,
)
test('fuzz-toml-config', test_fuzz_toml_exe)

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
  files('tests/test_ipc.c', 'src/ipc/socket.c', 'src/ipc/ipc_client.c'),
  dependencies: [gtk4_dep, gio_dep, gio_unix_dep],
  build_by_default: false,
)
test('ipc-socket', test_ipc_exe)

test_autohide_exe = executable(
  'test-autohide',
  files('tests/test_autohide.c', 'src/autohide.c') + kajo_config_sources,
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

widget_sources = kajo_config_sources + files(
  'src/audio/pa_conn.c',
  'src/theme.c',
  'src/icons.c',
  'src/dbus_helpers.c',
  'src/hardware/hw_caps.c',
  'src/hardware/kbd_backlight.c',
  'src/compositor/niri.c',
  'src/compositor/kajo_comp_ipc.c',
  'src/widgets/registry.c',
  'src/widgets/clock.c',
  'src/widgets/volume.c',
  'src/widgets/workspaces.c',
  'src/widgets/window.c',
  'src/widgets/keyboard-layout.c',
  'src/widgets/battery.c',
  'src/widgets/backlight.c',
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
  'src/widgets/osk.c',
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

test_osk_xkb_exe = executable(
  'test-osk-xkb',
  files('tests/test_osk_xkb.c', 'src/osk/osk_xkb.c'),
  dependencies: [gio_dep, xkbcommon_dep],
  build_by_default: false,
)
test('osk-xkb', test_osk_xkb_exe)

test_touch_gestures_exe = executable(
  'test-touch-gestures',
  files('tests/test_touch_gestures.c', 'src/hardware/touch_gestures.c'),
  dependencies: [layer_shell_dep, gtk4_dep, gtk4_wayland_dep, wayland_client_dep, gio_dep, gio_unix_dep, m_dep],
  build_by_default: false,
)
test('touch-gestures', test_touch_gestures_exe)

test_files_model_exe = executable(
  'test-files-model',
  files('tests/test_files_model.c', 'src/files/files_model.c', 'src/files/files_ops.c', 'src/files/files_diskimage.c'),
  dependencies: [gtk4_dep, gio_dep, gio_unix_dep, archive_dep],
  build_by_default: false,
)
test('files-model', test_files_model_exe)

test_player_queue_exe = executable(
  'test-player-queue',
  files('tests/test_player_queue.c', 'src/player/player_queue.c', 'src/player/player_mpv.c'),
  dependencies: [gtk4_dep, gio_dep, gio_unix_dep, mpv_dep, egl_dep, glesv2_dep, m_dep],
  build_by_default: false,
)
test('player-queue', test_player_queue_exe)

test_niri_exe = executable(
  'test-niri',
  files('tests/test_niri.c', 'src/compositor/niri.c', 'src/compositor/kajo_comp_ipc.c'),
  dependencies: [gio_dep, gio_unix_dep, json_glib_dep],
  build_by_default: false,
)
test('niri-ipc', test_niri_exe)

test_ide_fsm_exe = executable(
  'test-ide-fsm',
  files('tests/test_ide_fsm.c', 'src/ide/ide_fsm.c'),
  dependencies: [gio_dep],
  build_by_default: false,
)
test('ide-fsm', test_ide_fsm_exe)

test_ide_buffer_exe = executable(
  'test-ide-buffer',
  files(
    'tests/test_ide_buffer.c',
    'src/ide/ide_buffer.c',
    'src/ide/ide_syntax.c',
    'src/ide/ide_gutter.c',
    'src/ide/ide_tree.c',
    'src/ide/ide_picker.c',
    'src/ide/ide_search.c',
    'src/ide/ide_tasks.c',
    'src/ide/ide_native_exe.c',
    'src/ide/ide_fsm.c',
  ),
  dependencies: [gtk4_dep, gtksourceview_dep, gio_dep, json_glib_dep, m_dep, ide_ts_dep],
  build_by_default: false,
)
test('ide-buffer', test_ide_buffer_exe,
  env: {'KAJO_SOURCE_ROOT': meson.project_source_root()})

test_dap_client_exe = executable(
  'test-dap-client',
  files('tests/test_dap_client.c', 'src/ide/ide_dap_client.c', 'src/ide/ide_dap_launch.c', 'src/ide/ide_native_exe.c', 'src/ide/ide_dap_java.c', 'src/ide/ide_lsp_servers.c', 'src/ide/ide_jsonrpc.c'),
  dependencies: [gio_dep, json_glib_dep],
  build_by_default: false,
)
test('dap-client', test_dap_client_exe, timeout: 60)

test_ide_tasks_exe = executable(
  'test-ide-tasks',
  files('tests/test_ide_tasks.c', 'src/ide/ide_tasks.c', 'src/ide/ide_native_exe.c'),
  dependencies: [gio_dep, json_glib_dep],
  build_by_default: false,
)
test('ide-tasks', test_ide_tasks_exe)

test_ide_ts_exe = executable(
  'test-ide-ts',
  files('tests/test_ide_ts.c'),
  dependencies: [gio_dep, ide_ts_dep],
  build_by_default: false,
)
test('ide-ts', test_ide_ts_exe)

test_lsp_client_exe = executable(
  'test-lsp-client',
  files('tests/test_lsp_client.c', 'src/ide/ide_lsp_client.c', 'src/ide/ide_lsp_servers.c', 'src/ide/ide_jsonrpc.c'),
  dependencies: [gio_dep, json_glib_dep],
  build_by_default: false,
)
test('lsp-client', test_lsp_client_exe, timeout: 60)

test_ide_ipc_exe = executable(
  'test-ide-ipc',
  files(
    'tests/test_ide_ipc.c',
    'src/ide/ide_lsp.c',
    'src/ide/ide_lsp_client.c',
    'src/ide/ide_jsonrpc.c',
    'src/ide/ide_lsp_servers.c',
    'src/ide/ide_dap.c',
    'src/ide/ide_dap_client.c',
    'src/ide/ide_dap_launch.c',
    'src/ide/ide_dap_java.c',
    'src/ide/ide_jsonrpc.c',
    'src/ide/ide_agent_ipc.c',
    'src/ide/ide_markdown.c',
    'src/ide/ide_mermaid.c',
    'src/common/pdf_render.c',
    'src/ide/ide_buffer.c',
    'src/ide/ide_syntax.c',
    'src/ide/ide_picker.c',
    'src/ide/ide_search.c',
    'src/ide/ide_tasks.c',
    'src/ide/ide_native_exe.c',
  ),
  dependencies: [gtk4_dep, gtksourceview_dep, json_glib_dep, gio_dep, gio_unix_dep, poppler_dep, archive_dep, m_dep, ide_ts_dep],
  build_by_default: false,
)
test('ide-ipc', test_ide_ipc_exe)

test_mod_router_exe = executable(
  'test-mod-router',
  files('tests/test_mod_router.c') + mod_engine_sources,
  dependencies: mod_engine_deps,
  build_by_default: false,
)
test('mod-router', test_mod_router_exe)

# ─── kajo-comp (compositor) ───────────────────────────────────────────────
# Pure core: no wlroots, Vulkan or GLib headers (spec §7.2.4). Built with the
# stricter warning set from spec §7.2.2; -Werror is applied here only.
comp_strict_args = [
  '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wnull-dereference', '-Wformat=2',
  '-Wstrict-prototypes', '-Wmissing-prototypes', '-Wconversion', '-Wsign-conversion',
  '-Wvla', '-Wundef', '-fno-common', '-D_POSIX_C_SOURCE=200809L',
] + cc.get_supported_arguments(['-ftrivial-auto-var-init=zero', '-fstack-protector-strong'])
comp_xkb_dep   = dependency('xkbcommon')
comp_pcre2_dep = dependency('libpcre2-8')
# The vendored TOML parser is third-party code: built without the strict set.
comp_toml_lib = static_library('kajo-comp-toml', files('src/vendor/tomlc17/tomlc17.c'))
comp_core_lib = static_library(
  'kajo-comp-core',
  files(
    'src/comp/core/comp_diag.c',
    'src/comp/core/comp_spring.c',
    'src/comp/core/comp_layout.c',
    'src/comp/core/comp_motion.c',
    'src/comp/core/comp_binds.c',
    'src/comp/core/comp_rules.c',
    'src/comp/core/comp_config.c',
    'src/comp/core/comp_fx_core.c',
    'src/comp/core/comp_touch.c',
    'src/comp/core/comp_gesture.c',
  ),
  c_args: comp_strict_args,
  link_with: comp_toml_lib,
  dependencies: [m_dep, comp_xkb_dep, comp_pcre2_dep],
)
comp_core_dep = declare_dependency(link_with: [comp_core_lib, comp_toml_lib],
                                   dependencies: [m_dep, comp_xkb_dep, comp_pcre2_dep])

test_comp_spring_exe = executable(
  'test-comp-spring',
  files('tests/test_comp_spring.c'),
  dependencies: [comp_core_dep, gio_dep],
  build_by_default: false,
)
test('comp-spring', test_comp_spring_exe)

test_comp_layout_exe = executable(
  'test-comp-layout',
  files('tests/test_comp_layout.c'),
  dependencies: [comp_core_dep, gio_dep],
  build_by_default: false,
)
test('comp-layout', test_comp_layout_exe)

test_comp_motion_exe = executable(
  'test-comp-motion',
  files('tests/test_comp_motion.c'),
  dependencies: [comp_core_dep, gio_dep],
  build_by_default: false,
)
test('comp-motion', test_comp_motion_exe)

test_comp_config_exe = executable(
  'test-comp-config',
  files('tests/test_comp_config.c'),
  dependencies: [comp_core_dep, gio_dep],
  build_by_default: false,
)
test('comp-config', test_comp_config_exe,
  env: {'KAJO_SOURCE_ROOT': meson.project_source_root()})

test_comp_fx_core_exe = executable(
  'test-comp-fx-core',
  files('tests/test_comp_fx_core.c'),
  dependencies: [comp_core_dep, gio_dep],
  build_by_default: false,
)
test('comp-fx-core', test_comp_fx_core_exe)

# The compositor binary. System headers are included as -isystem so the strict
# warning set applies to our code only (spec §7.2.2).
#
# wlroots comes from Kajo's fork (subprojects/wlroots, a git submodule pinned at
# a kajo-* tag; docs/spec_wlroots_fork.md). It is built statically and never
# installed (install with --skip-subprojects). Upstream's defaults are kept
# (spec C1); features kajo-comp relies on are required so a missing dependency
# fails the build instead of silently dropping the feature (spec K4).
if get_option('wlroots') == 'fork'
  comp_wlroots_proj = subproject('wlroots', default_options: [
    'default_library=static',
    'examples=false',
    'werror=false',
    'xwayland=enabled',
    'session=enabled',
    'color-management=enabled',
  ])
  comp_wlroots_dep = comp_wlroots_proj.get_variable('wlroots').as_system()
else
  comp_wlroots_dep = dependency('wlroots-0.20', include_type: 'system', required: true)
endif
comp_have_kfx = comp_wlroots_dep.get_variable('have_kfx', default_value: 'false')

comp_fx_shader_headers = []
if comp_have_kfx == 'true'
  glslang = find_program('glslang', 'glslangValidator', required: true)
  fx_shader_inc = meson.current_source_dir() / 'src/comp/fx/shaders'
  comp_fx_shader_specs = [
    ['shadow.frag', 'shadow_frag.h', 'shadow_frag_spv'],
    ['dim.frag', 'dim_frag.h', 'dim_frag_spv'],
  ]
  foreach spec : comp_fx_shader_specs
    comp_fx_shader_headers += custom_target(
      spec[1],
      input: files('src/comp/fx/shaders' / spec[0]),
      output: spec[1],
      depend_files: files('src/comp/fx/shaders/fx_common.glsl'),
      command: [
        glslang, '-V', '-I' + fx_shader_inc, '@INPUT@',
        '-o', '@OUTPUT@', '--vn', spec[2], '--quiet',
      ],
    )
  endforeach
  comp_fx_sources = files(
    'src/comp/fx/fx_registry.c',
    'src/comp/fx/fx_runtime.c',
    'src/comp/fx/fx_shadow.c',
    'src/comp/fx/fx_dim.c',
  )
else
  comp_fx_sources = files('src/comp/fx/fx_stub.c')
endif
comp_wl_server_dep = dependency('wayland-server', include_type: 'system')
comp_talloc_dep = dependency('talloc', include_type: 'system')

wlr_layer_shell_server_h = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input: files('protocol/wlr-layer-shell-unstable-v1.xml'),
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
wlr_output_power_server_h = custom_target(
  'wlr-output-power-management-unstable-v1-protocol.h',
  input: files('protocol/wlr-output-power-management-unstable-v1.xml'),
  output: 'wlr-output-power-management-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

kajo_comp_exe = executable(
  'kajo-comp',
  files(
    'src/comp/main.c',
    'src/comp/comp_listen.c',
    'src/comp/comp_server.c',
    'src/comp/comp_output.c',
    'src/comp/comp_window.c',
    'src/comp/comp_layer.c',
    'src/comp/comp_lock_idle.c',
    'src/comp/comp_protocols.c',
    'src/comp/comp_xwayland.c',
    'src/comp/comp_input.c',
    'src/comp/comp_drag.c',
    'src/comp/comp_ipc.c',
    'src/comp/comp_spawn.c',
    'src/comp/comp_text.c',
    'src/comp/comp_switcher.c',
    'src/comp/comp_screenshot.c',
  ),
  comp_fx_sources,
  comp_fx_shader_headers,
  wlr_layer_shell_server_h,
  wlr_output_power_server_h,
  c_args: comp_strict_args + ['-DWLR_USE_UNSTABLE'],
  dependencies: [comp_core_dep, comp_wlroots_dep, comp_wl_server_dep, comp_talloc_dep,
                 dependency('libinput', include_type: 'system'),
                 dependency('xkbcommon', include_type: 'system'),
                 dependency('cairo', include_type: 'system'),
                 dependency('pangocairo', include_type: 'system'),
                 dependency('libpng', include_type: 'system')],
  install: true,
)

xdg_shell_client_h = custom_target(
  'xdg-shell-client-protocol.h',
  input: wl_protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_client_c = custom_target(
  'xdg-shell-protocol.c',
  input: wl_protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
xdg_dec_client_h = custom_target(
  'xdg-decoration-unstable-v1-client-protocol.h',
  input: wl_protocols_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  output: 'xdg-decoration-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_dec_client_c = custom_target(
  'xdg-decoration-unstable-v1-protocol.c',
  input: wl_protocols_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  output: 'xdg-decoration-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
spb_client_h = custom_target(
  'single-pixel-buffer-v1-client-protocol.h',
  input: wl_protocols_dir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
  output: 'single-pixel-buffer-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
spb_client_c = custom_target(
  'single-pixel-buffer-v1-protocol.c',
  input: wl_protocols_dir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
  output: 'single-pixel-buffer-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
wlr_layer_shell_client_h = custom_target(
  'wlr-layer-shell-unstable-v1-client-protocol.h',
  input: files('protocol/wlr-layer-shell-unstable-v1.xml'),
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
wlr_layer_shell_client_c = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.c',
  input: files('protocol/wlr-layer-shell-unstable-v1.xml'),
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
ext_session_lock_client_h = custom_target(
  'ext-session-lock-v1-client-protocol.h',
  input: wl_protocols_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  output: 'ext-session-lock-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
ext_session_lock_client_c = custom_target(
  'ext-session-lock-v1-protocol.c',
  input: wl_protocols_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  output: 'ext-session-lock-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
viewporter_client_h = custom_target(
  'viewporter-client-protocol.h',
  input: wl_protocols_dir / 'stable/viewporter/viewporter.xml',
  output: 'viewporter-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
viewporter_client_c = custom_target(
  'viewporter-protocol.c',
  input: wl_protocols_dir / 'stable/viewporter/viewporter.xml',
  output: 'viewporter-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

comp_test_client_exe = executable(
  'comp-test-client',
  files('tests/comp/comp_test_client.c'),
  xdg_shell_client_h, xdg_shell_client_c,
  xdg_dec_client_h, xdg_dec_client_c,
  spb_client_h, spb_client_c,
  wlr_layer_shell_client_h, wlr_layer_shell_client_c,
  ext_session_lock_client_h, ext_session_lock_client_c,
  viewporter_client_h, viewporter_client_c,
  vkbd_header, vkbd_code,
  im_header, im_code,
  c_args: comp_strict_args,
  dependencies: [dependency('wayland-client', include_type: 'system'), comp_xkb_dep],
  build_by_default: false,
)
test_comp_fx_scene_exe = executable(
  'test-comp-fx-scene',
  files('tests/test_comp_fx_scene.c'),
  comp_fx_sources,
  comp_fx_shader_headers,
  c_args: comp_strict_args + ['-DWLR_USE_UNSTABLE'],
  dependencies: [comp_core_dep, comp_wlroots_dep, comp_wl_server_dep, comp_talloc_dep, gio_dep],
  build_by_default: false,
)
test('comp-fx-scene', test_comp_fx_scene_exe)

test('comp-headless', find_program('tests/comp/comp_headless_smoke.sh'),
  args: [kajo_comp_exe, comp_test_client_exe],
  depends: [kajo_comp_exe, comp_test_client_exe],
  timeout: 180,
)
if comp_have_kfx == 'true'
  test('comp-fx-shadow', find_program('tests/comp/comp_fx_shadow_test.sh'),
    args: [kajo_comp_exe, comp_test_client_exe],
    depends: [kajo_comp_exe, comp_test_client_exe],
    timeout: 180,
  )
endif

test('comp-audit', find_program('scripts/audit.sh'), args: ['comp'],
  workdir: meson.project_source_root())

# Fuzz targets, built here as corpus drivers (smoke tests). Real fuzzing: DEVELOPMENT.md.
test_fuzz_comp_layout_exe = executable(
  'test-fuzz-comp-layout',
  files('tests/fuzz/fuzz_comp_layout.c'),
  c_args: comp_strict_args,
  dependencies: [comp_core_dep],
  build_by_default: false,
)
test('fuzz-comp-layout', test_fuzz_comp_layout_exe)

test_fuzz_comp_config_exe = executable(
  'test-fuzz-comp-config',
  files('tests/fuzz/fuzz_comp_config.c'),
  c_args: comp_strict_args,
  dependencies: [comp_core_dep],
  build_by_default: false,
)
test('fuzz-comp-config', test_fuzz_comp_config_exe)

test_fuzz_ipc_socket_exe = executable(
  'test-fuzz-ipc-socket',
  files(
    'tests/fuzz/fuzz_ipc_socket.c',
    'src/comp/comp_listen.c',
    'src/comp/comp_server.c',
    'src/comp/comp_output.c',
    'src/comp/comp_window.c',
    'src/comp/comp_layer.c',
    'src/comp/comp_lock_idle.c',
    'src/comp/comp_protocols.c',
    'src/comp/comp_xwayland.c',
    'src/comp/comp_input.c',
    'src/comp/comp_drag.c',
    'src/comp/comp_ipc.c',
    'src/comp/comp_spawn.c',
    'src/comp/comp_text.c',
    'src/comp/comp_switcher.c',
    'src/comp/comp_screenshot.c',
  ),
  comp_fx_sources,
  comp_fx_shader_headers,
  wlr_layer_shell_server_h,
  wlr_output_power_server_h,
  c_args: comp_strict_args + ['-DWLR_USE_UNSTABLE'],
  dependencies: [comp_core_dep, comp_wlroots_dep, comp_wl_server_dep, comp_talloc_dep,
                 dependency('libinput', include_type: 'system'),
                 dependency('xkbcommon', include_type: 'system'),
                 dependency('cairo', include_type: 'system'),
                 dependency('pangocairo', include_type: 'system'),
                 dependency('libpng', include_type: 'system')],
  build_by_default: false,
)
test('fuzz-ipc-socket', test_fuzz_ipc_socket_exe)

gnome.post_install(
  update_desktop_database: true,
)

