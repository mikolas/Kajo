# Developer Guide & Contribution Rules

This document provides technical guidelines, architecture maps, testing workflows, and contribution standards for developers working on `kajo`, `kajo-ide`, `kajo-settings`, `kajo-files`, `kajo-player`, and `kajo-greeter`.

---

## 1. Architecture & Source File Map

The codebase is written in standard C17 (`-std=c17`) using GTK4, `gtksourceview-5`, `gtk4-layer-shell-0` (including `gtk4-session-lock`), `wayland-client`, `xkbcommon`, `json-glib-1.0`, `gio-2.0`, `libmpv`, `libpulse`, `libpulse-mainloop-glib`, `libpam`, `poppler-glib`, and `libarchive`.

```
src/
├── main.c                          kajo daemon entry point & IPC client mode
├── shell.h/c                       ShellApp — core subsystem wiring & live widget reconciliation
├── panel.h/c                       Panel layer-shell surface & 2px Wayland autohide trigger strip
├── autohide.h/c                    Autohide reveal/hide state machine & live config reload
├── config.h/c                      JSON configuration parser & serializer (3-tier path resolution)
├── theme.h/c                       GTK CSS theme engine, override loader & GFileMonitor hot-reload
├── icons.h/c                       Icon resolution (Fluent UI SVGs + desktop/theme fallback)
├── dbus_helpers.h/c                Shared GDBus helpers (NetworkManager, BlueZ, UPower)
├── compositor/
│   ├── compositor.h                Compositor abstraction interface
│   └── niri.c                      Niri UNIX socket ($NIRI_SOCKET) event stream & RPC actions
├── files/
│   ├── main.c                      kajo-files standalone entry point (org.kajo.Files)
│   ├── files_window.h/c            Main window controller, history stack, breadcrumbs & shortcuts
│   ├── files_model.h/c             KajoFileItem GObject, async GVFS enumerator, sort & filter
│   ├── files_ops.h/c               Async Copy/Move/Trash/Restore/Delete/Rename/Mkdir & clipboard
│   ├── files_sidebar.h/c           Places, Trash, GVolumeMonitor drives & inline GVFS/LUKS auth
│   └── files_quicklook.h/c         Inspector drawer & Space Quick Look modal (img/media/PDF/archive/code)
├── hardware/
│   ├── hw_caps.h/c                 Runtime hardware capability probing (ISH hinge/accel, EC, LEDs)
│   ├── kbd_backlight.h/c           Sysfs & HP EC (0x97 HKBL / 0x96 HKBT) keyboard backlight control
│   └── touch_gestures.h/c          Edge layer-shell bezel swipe gesture recognizers
├── ide/
│   ├── main.c                      kajo-ide standalone entry point (org.kajo.IDE)
│   ├── ide_window.h/c              Main IDE window, 3-zone header, tab ribbon, status bar & chord HUD
│   ├── ide_fsm.h/c                 10-state selection-first modal FSM & Nordic text objects (m i/a)
│   ├── ide_buffer.h/c              IdeBuffer, 11-token semantic highlighter, Git diff & Agent snapshots
│   ├── ide_gutter.h/c              68px 6-column Cairo gutter (DAP, LSP, Git, Agent, line nums, folds)
│   ├── ide_tree.h/c                Scroll-stable Project File Explorer, Git porcelain parser & CRUD
│   ├── ide_picker.h/c              Fuzzy File/Buffer/Ripgrep modal & 85-command Help Palette
│   ├── ide_search.h/c              Regex Search & Replace + async compiler build output drawer
│   ├── ide_lsp.h/c                 JSON-RPC framing helpers + ripgrep-backed hover/completion/definition/rename (real LSP client planned)
│   ├── ide_dap.h/c                 Debugger UI scaffold: transport buttons & 3-pane inspector (no adapter connected yet)
│   ├── ide_markdown.h/c            Graphical Markdown preview, live task checkboxes & GitHub alerts
│   ├── ide_mermaid.h/c             Pure C11 Cairo/Pango vector Mermaid diagram engine & SVG exporter
│   └── ide_agent_ipc.h/c           AI Agent JSON-RPC 2.0 Unix socket & atomic multi-file diff review
├── ipc/
│   └── socket.h/c                  UNIX domain socket server ($XDG_RUNTIME_DIR/kajo.sock)
├── launcher/
│   ├── launcher_surface.h/c        App launcher overlay surface & prefix plugin router
│   └── emoji_dataset.h             Embedded dataset with 1,914 emoji entries
├── lock/
│   ├── session_lock.h/c            Wayland ext-session-lock-v1 lock screen & logind integration
│   ├── pam_auth.h/c                Asynchronous GTask PAM authentication worker (libpam)
│   ├── screensaver.h/c             Fullscreen layer-shell Metro screen saver & Niri DPMS control
│   └── idle_manager.h/c            Wayland ext-idle-notify-v1 seat timers & logind idle inhibitor FD
├── greeter/
│   ├── main.c                      kajo-greeter GTK4 Layer-Shell / XDG fullscreen / --test UI
│   ├── greetd_ipc.h/c              greetd UNIX socket ($GREETD_SOCK) binary framing & GTask client
│   └── greeter_users.h/c           POSIX getpwent() user discovery & .desktop session parser
├── notifications/
│   └── daemon.h/c                  Freedesktop notification server (org.freedesktop.Notifications)
├── osd/
│   └── osd.h/c                     Volume, brightness & status OSD overlay surface
├── osk/
│   ├── osk.h/c                     ShellOSK 28-column proportional virtual keyboard & IMv2 monitor
│   └── osk_xkb.h/c                 XKB keymap builder & keycode/keysym lookup
├── posture/
│   └── posture_manager.h/c         360° hinge angle posture state machine & auto-rotation
├── player/
│   ├── main.c                      kajo-player standalone entry point (org.kajo.Player)
│   ├── player_window.h/c           Main window, fullscreen overlay HUD, subtitle/audio/speed popovers
│   ├── player_mpv.h/c              libmpv + GtkGLArea OpenGL/EGL engine, track/chapter/telemetry parser
│   ├── player_queue.h/c            Sibling folder media scanner, KajoMediaTrack & queue drawer
│   └── player_mpris.h/c            MPRIS2 D-Bus server (org.mpris.MediaPlayer2.kajo_player)
├── settings/
│   ├── main.c                      kajo-settings standalone entry point (org.kajo.Settings)
│   ├── settings_window.h/c         Settings container shell, category sidebar & GtkStack
│   ├── settings_common.h/c         Shared Metro card, tile, dropdown & IPC helpers
│   └── pages/
│       └── page_*.c                11 modular preference pages (displays, network, bluetooth,
│                                   audio, theme, panel, widgets, notifications, ide, niri, about)
├── vendor/
│   └── tinyexpr/                   TinyExpr recursive-descent math expression evaluator
└── widgets/
    ├── widget.h                    ShellWidgetClass vtable interface & mode visibility helper
    ├── registry.h/c                Panel widget registry
    ├── media.c                     MPRIS2 controller & 16-band Cooley-Tukey FFT visualizer
    ├── tray.c                      StatusNotifierItem (SNI) watcher & ARGB32 pixmap decoder
    └── *.c                         22 built-in panel widget implementations

data/
├── org.kajo.Files.desktop          Desktop entry for kajo-files (MimeType=inode/directory;)
├── org.kajo.Player.desktop         Desktop entry for kajo-player (MimeType=video/*;audio/*;)
├── org.kajo.IDE.desktop            Desktop entry for kajo-ide (MimeType=text/plain;text/x-csrc;...)
├── ide/
│   └── *.xml                       5 bundled GtkSourceView-5 syntax schemes (kajo-metro-dark, etc.)
├── defaults/
│   ├── config.json                 Default panel, widget, notification, IDE & idle configuration
│   ├── style.css                   Master GTK4 CSS stylesheet (panel, popovers, settings, files, ide, osk, lock, greeter)
│   └── icon-map.json               Logical-to-symbolic icon mappings
├── greetd/
│   ├── niri-greeter.kdl            Locked-down Niri configuration for the greetd login session
│   └── kajo-greeter-session        Wrapper launcher script for /etc/greetd/config.toml
├── shaders/
│   └── *.glsl                      Bundled Niri GLSL window open/close/resize shader presets
└── ui/
    └── *.blp                       Blueprint Markup (.blp) declarative UI templates

tests/
├── test_config.c                   Config JSON load/save roundtrip & defaults resolution tests
├── test_pam_auth.c                 Async PAM worker error/null/cancellation handling tests
├── test_idle_manager.c             Idle manager timeout & inhibitor state machine tests
├── test_ipc.c                      UNIX domain socket IPC server/client dispatch tests
├── test_autohide.c                 Autohide state transitions, hold-open & margin calculation tests
├── test_registry.c                 Widget registry completeness & lifecycle tests
├── test_greetd_ipc.c               greetd binary framing, auth roundtrip & session parser tests
├── test_osk_xkb.c                  XKB keymap compilation & keysym/keycode resolution tests
├── test_touch_gestures.c           Bezel touch gesture directional threshold & trigger tests
├── test_files_model.c              KajoFileItem formatting, sorting, filtering & async file op tests
├── test_player_queue.c             KajoPlayer time/size formatting, media detection & mpv track label tests
├── test_ide_fsm.c                  Kajo IDE 10-state modal FSM, Nordic text objects & chord tests
├── test_ide_buffer.c               Kajo IDE buffer undo/redo, semantic tokens, Git diff & Agent snapshot tests
├── test_ide_ipc.c                  Kajo IDE JSON-RPC 2.0 framing & Agent IPC protocol tests
└── test_shaders.c                  Headless EGL/GLES2 compiler verification for Niri GLSL shaders
```

---

## 2. Build, Test & Debug Workflow

### Prerequisites
- `gtk4` (≥ 4.12), `gtk4-layer-shell` (≥ 1.0)
- `wayland-client`, `wayland-protocols`, `wayland-scanner`, `xkbcommon`
- `json-glib-1.0`, `gio-2.0`, `gio-unix-2.0`
- `libpulse`, `libpulse-mainloop-glib`
- `pam` (`libpam`)
- `poppler-glib`, `libarchive`
- `blueprint-compiler`, `meson`, `ninja`

### Compiling & Running the Test Suite
Every change must compile with **0 errors and 0 warnings** and pass all 16 automated GTest suites:

```bash
# Configure (defaults to prefix=~/.local)
meson setup builddir

# Compile all targets (kajo, kajo-settings, kajo-files, kajo-greeter)
meson compile -C builddir

# Run all 16 automated GTest suites
meson test -C builddir

# Install to ~/.local/bin and ~/.local/share/kajo
meson install -C builddir
```

### Automated Test Suites (`meson test -C builddir`)
| Suite Name | Binary | Coverage |
| :--- | :--- | :--- |
| `config-roundtrip` | `test-config` | `ShellConfig` JSON serialization, `idle` / `panel` / `layout` / `touch_gestures` fields, defaults fallback. |
| `pam-auth` | `test-pam-auth` | `shell_pam_authenticate_async` worker thread lifecycle, empty password rejection, cancellation. |
| `greetd-ipc` | `test-greetd-ipc` | Mock `greetd` UNIX socket server (`uint32_t LE` + JSON), auth success/failure/cancel, JSON quote/backslash escaping, `getpwent()` user discovery, and `.desktop` `%U` field code stripping. |
| `idle-manager` | `test-idle-manager` | Idle timeout configuration, Keep Awake inhibitor toggling, callback notification. |
| `autohide-state` | `test-autohide` | Panel autohide states (`VISIBLE`, `HIDING`, `HIDDEN`, `REVEALING`), 2px trigger strip margins, popover/launcher hold-open, and live config reload. |
| `ipc-socket` | `test-ipc` | `$XDG_RUNTIME_DIR/kajo.sock` command dispatch (`lock`, `screensaver`, `reload-config`, etc.). |
| `widget-registry` | `test-registry` | Verifies all 22 registered `ShellWidgetClass` definitions have valid `.id` and `.name` fields. |
| `osk-xkb` | `test-osk-xkb` | XKB keymap generation (`us`, `fi`, etc.) and keysym/keycode resolution for `ShellOSK`. |
| `touch-gestures` | `test-touch-gestures` | Bezel swipe gesture directional thresholds, off-axis rejection, and action dispatch. |
| `files-model` | `test-files-model` | `KajoFileItem` size/permission formatting, directory-first sorting, filtering, and async `mkdir`/`rename`/`delete` operations. |
| `niri-shaders` | `test-shaders` | Headless EGL/GLES2 shader compilation validation for all bundled `data/shaders/*.glsl` presets. |
| `player-queue` | `test-player-queue` | `kajo-player` time/size formatting, sibling-folder media detection and mpv track-label helpers. |
| `ide-fsm` | `test-ide-fsm` | 10-state modal FSM transitions, Nordic text objects and `Space` leader chords. |
| `ide-buffer` | `test-ide-buffer` | `IdeBuffer` undo/redo, semantic tokens, Git diff, agent snapshots; also the regression test for the style-scheme use-after-free (run it under `scripts/asan-test.sh`). |
| `ide-ipc` | `test-ide-ipc` | JSON-RPC framing, agent IPC dispatch, a real unix-socket round-trip, and a stalled-client case proving the main loop is never blocked. |
| `niri-ipc` | `test-niri` | Mock-Niri socket: exact JSON of `ToggleWindowFloating` / `LoadConfigFile` actions (regression for the dead FLOAT / TILE button), rejection logging, and EventStream parsing (workspaces, windows, focus, keyboard layouts). |

### Sanitizer Run (ASan + UBSan)
Run the whole suite under AddressSanitizer and UndefinedBehaviorSanitizer in a throwaway build directory (does not touch `builddir/`):

```bash
scripts/asan-test.sh            # builds in /tmp/kajo-asan and runs meson test
ASAN_OPTIONS=detect_leaks=1 scripts/asan-test.sh   # also report leaks (expect fontconfig/pango cache noise)
```

Run this before merging any change that touches object lifetimes, async callbacks, or string ownership. Code style for new code is described in `.clang-format` (existing files are not reformatted in bulk).

### Running & Debugging Individual Executables
```bash
# 1. Run main desktop shell
./builddir/kajo

# 2. Run settings app directly on a specific page
./builddir/kajo-settings --page audio

# 3. Run file manager on a path or GVFS URI
./builddir/kajo-files ~/Downloads

# 4. Run kajo-greeter in safe desktop preview mode (uses PAM verification, no logout)
./builddir/kajo-greeter --test --cmd niri-session

# 5. Run kajo-greeter inside a nested Niri compositor window on your desktop
KAJO_GREETER_TEST=1 KAJO_GREETER_QUIT_NIRI=1 niri -c data/greetd/niri-greeter.kdl
```

---

## 3. IPC Protocol Reference

### A. Kajo Shell Socket (`$XDG_RUNTIME_DIR/kajo.sock`)
Line-delimited UTF-8 commands (`<command>\n`) sent by `kajo <flag>` or `kajo-settings`:

| Command | Aliases | Action |
| :--- | :--- | :--- |
| `launcher-toggle` | `toggle-launcher` | Toggles the application launcher overlay. |
| `launcher-open` | — | Opens the application launcher overlay. |
| `launcher-close` | — | Closes the application launcher overlay. |
| `lock` | `lock-session` | Activates the `ext-session-lock-v1` PAM lock screen. |
| `screensaver` | `start-screensaver` | Activates the fullscreen Metro screen saver overlay. |
| `reload-config` | — | Reloads `~/.config/kajo/config.json` and reconciles panel geometry, idle timers, and active widgets in-place. |
| `reload-theme` | — | Reloads `style.css` and `~/.config/kajo/theme-override.css` across all surfaces. |
| `volume-up` | `volume+` | Increases default audio sink volume by 5% + shows OSD. |
| `volume-down` | `volume-` | Decreases default audio sink volume by 5% + shows OSD. |
| `volume-mute` | `volume-toggle` | Toggles default audio sink mute + shows OSD. |
| `brightness-up` | `brightness+` | Increases backlight brightness by 5% + shows OSD. |
| `brightness-down` | `brightness-` | Decreases backlight brightness by 5% + shows OSD. |
| `toggle-osk` | `osk-toggle` | Toggles the on-screen virtual keyboard (`ShellOSK`). |
| `show-osk` | `osk-open` | Shows the on-screen virtual keyboard. |
| `hide-osk` | `osk-close` | Hides the on-screen virtual keyboard. |

### B. `greetd` Socket (`$GREETD_SOCK`)
Binary length-prefixed framing (`uint32_t` native-endian length + UTF-8 JSON payload) implemented in `src/greeter/greetd_ipc.c`:
1. `{"type": "create_session", "username": "<user>"}`
2. `{"type": "post_auth_message_response", "response": "<password>"}`
3. `{"type": "start_session", "cmd": ["..."], "env": ["..."]}` (or `{"type": "cancel_session"}` on failure).

---

## 4. How to Add a New Panel Widget

1. **Create `src/widgets/my_widget.c`**:
   Every `ShellWidgetClass` **must** initialize both `.id` and `.name` (Rule #7):
   ```c
   #include "widget.h"

   const ShellWidgetClass my_widget_class = {
       .id      = "my-widget",
       .name    = "My Widget",
       .create  = my_widget_create,
       .destroy = my_widget_destroy,
       .get_widget = my_widget_get_widget,
       .enable  = my_widget_enable,
       .disable = my_widget_disable,
   };
   ```
2. **Register in `src/widgets/registry.c`** and add `'src/widgets/my_widget.c'` to `sources` and `widget_sources` in `meson.build`.

---

## 5. Strict Coding, Resource & Security Standards

1. **No Mock Data in UI**: All Wi-Fi APs, Bluetooth devices, PipeWire sinks/sources/streams, UPower batteries, Niri outputs, and system users/sessions must query live system interfaces.
2. **Popout-Gated Resource Architecture**: PulseAudio monitor streams (`@DEFAULT_MONITOR@`) must start corked (`PA_STREAM_START_CORKED`), uncork on popover `map`, and re-cork on popover `closed`/`unmap`. High-frequency timers (30 FPS FFT) must be removed when unmapped so idle background CPU stays $<0.05\%$.
3. **Credential & Lock/Greeter Hardening**:
   - Never leave plaintext passwords in `GtkPasswordEntry` while asynchronous PAM or `greetd` verification is in flight—copy, clear the widget buffer immediately, reset `show-peek-icon` (`FALSE` $\rightarrow$ `TRUE`) so visibility returns to masked mode, and scrub temporary buffers with `explicit_bzero()`.
   - Never pass passwords through `JsonBuilder` / `JsonGenerator` (which allocate unscrubbed heap copies).
   - Always disable GTK interactive debugging (`gtk_window_set_interactive_debugging(FALSE)`) on lock and greeter surfaces, and set `prctl(PR_SET_DUMPABLE, 0)` in `kajo-greeter`.
4. **Autohide Wayland Surface Visibility**: Never set `opacity: 0` on `#panel-container.autohide-hidden` in `style.css` (GTK4 skips rendering nodes with `opacity: 0`, which prevents Wayland surface buffer commits and breaks pointer enter events on the 2px trigger strip).
