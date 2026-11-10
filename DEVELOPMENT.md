# Developer Guide & Contribution Rules

This document provides technical guidelines, architecture maps, testing workflows, and contribution standards for developers working on `kajo`, `kajo-ide`, `kajo-settings`, `kajo-files`, `kajo-player`, `kajo-mod`, and `kajo-greeter`.

---

## 1. Architecture & Source File Map

The codebase is written in standard C17 (`-std=c17`, plus C++20 `-std=c++20` for `src/mod/mod_backend_sid.cpp`) using GTK4, `gtksourceview-5`, `gtk4-layer-shell-0` (including `gtk4-session-lock`), `wayland-client`, `xkbcommon`, `json-glib-1.0`, `gio-2.0`, `libmpv`, `libuade`, `libsidplayfp`, `libgme`, `libopenmpt`, `libxmp`, `libpulse`, `libpulse-mainloop-glib`, `libpam`, `poppler-glib`, and `libarchive`.

```
src/
├── main.c                          kajo daemon entry point & IPC client mode
├── shell.h/c                       ShellApp — core subsystem wiring & live widget reconciliation
├── panel.h/c                       Panel layer-shell surface & 2px Wayland autohide trigger strip
├── autohide.h/c                    Autohide reveal/hide state machine & live config reload
├── config.h/c                      JSON configuration parser & serializer (3-tier path resolution)
├── theme.h/c                       GTK CSS theme engine, override loader, GFileMonitor hot-reload, accent presets & shell_ide_font_stack()
├── app_init.h/c                    Standalone-app boot: kajo_app_init_theme() / kajo_app_shutdown_theme()
├── icons.h/c                       Icon resolution (Fluent UI SVGs + desktop/theme fallback)
├── dbus_helpers.h/c                Shared GDBus helpers (NetworkManager, BlueZ, UPower)
├── audio/
│   └── pa_conn.h/c                 Shared PulseAudio/PipeWire connection + 2s auto-reconnect (volume, control-center, media, settings audio)
├── common/
│   └── pdf_render.h/c              Poppler page → GdkTexture (files Quick Look & IDE Markdown)
├── compositor/
│   ├── compositor.h                Compositor abstraction interface
│   └── niri.c                      Niri UNIX socket ($NIRI_SOCKET) event stream & RPC actions
├── files/
│   ├── main.c                      kajo-files standalone entry point (org.kajo.Files)
│   ├── files_window.h/c            Main window controller, history stack, breadcrumbs & shortcuts
│   ├── files_window_private.h      Window struct + helpers shared by the files_window_* modules (internal)
│   ├── files_window_ctxmenu.c      Context menu popover + Open With sub-view
│   ├── files_window_keys.c         Keyboard shortcut controller (Space Quick Look, navigation, clipboard)
│   ├── files_model.h/c             KajoFileItem GObject, async GVFS + archive:// enumerator, sort & filter, breadcrumb chain
│   ├── files_ops.h/c               Async Copy/Move/Trash/Restore/Delete/Rename/Mkdir, archive extract/compress & clipboard
│   ├── files_diskimage.h/c         UDisks2 loop-mount of ISO / raw disk images (LoopSetup -> Mount -> autoclear)
│   ├── files_sidebar.h/c           Places, Trash, GVolumeMonitor drives & inline GVFS/LUKS auth
│   └── files_quicklook.h/c         Inspector drawer & Space Quick Look modal (img/media/PDF/archive/code)
├── hardware/
│   ├── hw_caps.h/c                 Runtime hardware capability probing (ISH hinge/accel, EC, LEDs)
│   ├── kbd_backlight.h/c           Sysfs & HP EC (0x97 HKBL / 0x96 HKBT) keyboard backlight control
│   └── touch_gestures.h/c          Edge layer-shell bezel swipe gesture recognizers
├── ide/
│   ├── main.c                      kajo-ide standalone entry point (org.kajo.IDE)
│   ├── ide_window.h/c              Main IDE window, 3-zone header, tab ribbon, status bar & chord HUD
│   ├── ide_window_private.h        Window struct + helpers shared with ide_actions.c (internal)
│   ├── ide_actions.c               Action dispatcher + buffer motion/edit execution
│   ├── ide_fsm.h/c                 10-state selection-first modal FSM & Nordic text objects (m i/a)
│   ├── ide_buffer.h/c              IdeBuffer, text objects, Git diff & Agent snapshots, scheme search paths
│   ├── ide_syntax.c                11-token highlighter (ide_highlight_source_buffer): drives the tree-sitter engine, heuristic lexer fallback
│   ├── ide_ts.c / ide_ts.h         Tree-sitter engine: incremental parse, base token pass + per-language query layer (no GTK)
│   ├── ide_ts_queries.c            Highlight queries for all 15 languages (C, C++, Java, Go, Rust, Bash, JS/TS/TSX, Lua, JSON, TOML, YAML, CSS, HTML)
│   ├── ide_gutter.h/c              68px 6-column Cairo gutter (DAP, LSP, Git, Agent, line nums, folds)
│   ├── ide_tree.h/c                Scroll-stable Project File Explorer, Git porcelain parser & CRUD
│   ├── ide_picker.h/c              Fuzzy File/Buffer/Ripgrep modal & 102-command Help Palette
│   ├── ide_search.h/c              Regex Search & Replace + async compiler build output drawer
│   ├── ide_lsp.h/c                 LSP manager: per-root clients, document sync, diagnostics, hover/completion/definition/rename/code actions/format popovers (ripgrep fallbacks)
│   ├── ide_lsp_client.h/c          GTK-free async JSON-RPC 2.0 / LSP client (GSubprocess, framing, request queue, capabilities)
│   ├── ide_dap.h/c                 Debugger view: session state machine, call stack / locals / console panes
│   ├── ide_dap_client.h/c          GTK-free async DAP client (adapter process or any GIO stream pair)
│   ├── ide_dap_launch.h/c          Adapter + debuggee discovery (.kajo/debug.json, Meson introspection, Cargo, Delve)
│   ├── ide_dap_java.h/c            Java debug preparation: jdtls executeCommand chain (main class, classpath, JDK, debug port) + TCP connect
│   ├── ide_jsonrpc.h/c             Content-Length framing shared by the LSP and DAP clients
│   ├── ide_tasks.h/c               Build-system detection (Maven/Gradle/Meson/CMake/Cargo/Go/Make) -> build/test/run/clean commands (no GTK)
│   ├── ide_lsp_servers.h/c         Server registry: extension -> server, project-root markers, launch argv (jdtls JDK, clangd compile db)
│   ├── ide_native_exe.h/c          Native executable discovery for run/debug (Meson/CMake/Make build trees)
│   ├── ide_markdown.h/c            Graphical Markdown preview, live task checkboxes & GitHub alerts
│   ├── ide_mermaid.h/c             Pure C11 Cairo/Pango vector Mermaid diagram engine & SVG exporter
│   └── ide_agent_ipc.h/c           AI Agent JSON-RPC 2.0 Unix socket & atomic multi-file diff review
├── ipc/
│   ├── socket.h/c                  UNIX domain socket server ($XDG_RUNTIME_DIR/kajo.sock)
│   └── ipc_client.h/c              shell_ipc_client_send(): one-line client used by the `kajo` CLI and kajo-settings
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
├── mod/
│   ├── main.c                      kajo-mod standalone entry point (org.kajo.Mod)
│   ├── mod_window.h/c              Main window, telemetry deck, DSP strip, 4-voice header, inspector & Niri column reflow
│   ├── mod_vis.h/c                 Cairo 512-sample stereo oscilloscope & 16-band logarithmic FFT spectrum with peak hold
│   ├── mod_engine.h/c              KajoModEngine VTable, strict 5-tier format router & ProTracker sample header parser
│   ├── mod_backend_uade.c          Priority 1: libuade Amiga 68000 + Paula BLEP/LED filter & eagleplayer engine
│   ├── mod_backend_sid.cpp         Priority 2: libsidplayfp + reSIDfp 6581/8580 C++20 engine, $D400 telemetry & HVSC parser
│   ├── mod_backend_gme.c           Priority 3: libgme 8/16-bit console chiptune engine (SPC, NSF, VGM, GBS, AY, SAP, HES)
│   ├── mod_backend_openmpt.c       Priority 4 & 5: libopenmpt + libxmp PC/extended tracker engines (XM, S3M, IT, MPTM, MED)
│   ├── mod_archive.h/c             libarchive multi-file (.lha/.zip/.7z) extractor, companion filter & CP437 .nfo decoder
│   ├── mod_playlist.h/c            KajoModPlaylist queue manager & recursive directory/archive scanner
│   └── mod_audio.h/c               libpulse 48 kHz S16LE synthesis worker thread, 512-pt Radix-2 FFT, stereo crossfeed & fade
├── settings/
│   ├── main.c                      kajo-settings standalone entry point (org.kajo.Settings)
│   ├── settings_window.h/c         Responsive 3-tier Niri column shell, icon-rail/sidebar & GtkStack
│   ├── settings_common.h/c         Shared Metro card, bento grid, tile, dropdown & IPC helpers
│   └── pages/
│       └── page_*.c                12 modular preference pages (displays, network, bluetooth,
│                                   audio, theme, panel, widgets, notifications, ide, mod, niri, about)
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
├── org.kajo.Mod.desktop            Desktop entry for kajo-mod (MimeType=audio/x-mod;audio/prs.sid;...)
├── org.kajo.IDE.desktop            Desktop entry for kajo-ide (MimeType=text/plain;text/x-csrc;...)
├── ide/
│   └── *.xml                       5 bundled GtkSourceView-5 syntax schemes (kajo-metro-dark, etc.)
├── defaults/
│   ├── {theme,shell,ide,files,mod}.toml  Shipped per-domain TOML 1.1 default configurations
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
├── test_config.c                   Per-domain TOML store tests: defaults parity, round-trip, `conf.d`, patching, subscriptions, migration
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
├── test_dap_client.c               DAP client against tests/mock_dap_adapter.py + launch resolution
├── mock_dap_adapter.py             Scripted debug adapter used by the DAP tests
├── test_ide_tasks.c                Task resolution per build system, nested projects, Java main-class, shell quoting
├── test_lsp_client.c               LSP client against tests/mock_lsp_server.py (handshake queueing, errors, cancel, server death)
├── mock_lsp_server.py              Scripted language server used by the LSP tests
├── test_ide_ts.c                   Tree-sitter engine: token classification for all 15 languages, query validity against the installed grammars, detection, incremental == fresh parse
├── test_ide_buffer.c               Kajo IDE buffer undo/redo, semantic tokens, Git diff & Agent snapshot tests
├── test_ide_ipc.c                  Kajo IDE JSON-RPC 2.0 framing, Agent IPC dispatch, socket round-trip & stalled-client tests
├── test_mod_router.c               Kajo Mod 5-tier format router, companion/NFO filter, archive extraction & live PCM synthesis tests
├── test_niri.c                     Mock-Niri socket: action JSON shape, rejection logging, EventStream parsing
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
- `poppler-glib`, `libarchive`, `mpv` (`libmpv`)
- `uade` (`libuade`), `libsidplayfp`, `libgme`, `libopenmpt`, `libxmp` (for `kajo-mod`)
- `blueprint-compiler`, `meson`, `ninja`
- Tree-sitter (optional, enables the `kajo-ide` syntax engine). Runtime + grammars on Arch / CachyOS:
  - official / CachyOS repos: `tree-sitter`, `tree-sitter-c`, `tree-sitter-bash`, `tree-sitter-javascript`, `tree-sitter-lua`, `tree-sitter-rust`
  - AUR: `tree-sitter-cpp`, `tree-sitter-css`, `tree-sitter-go`, `tree-sitter-html`, `tree-sitter-java`, `tree-sitter-json`, `tree-sitter-toml`, `tree-sitter-typescript` (also provides TSX), `tree-sitter-yaml`, `tree-sitter-m68k`
  - `meson.build` probes `tree-sitter` via pkg-config and each `libtree-sitter-<lang>` via `find_library()` (several grammars ship no header or `.pc` file, so their entry points are declared in `src/ide/ide_ts.c`). A missing grammar only disables that language's tree-sitter path.
  - Highlight rules for every tree-sitter language live in `src/ide/ide_ts_queries.c`. A shared base pass (`ide_ts.c`) colors comments, strings, numbers, keywords, operators and primitive types; the query adds the context-sensitive roles (methods vs. fields, declaration names, calls, annotations), with a small convention fallback (UPPER_SNAKE → constant; Java PascalCase → type). The `ide-ts` test compiles every query against the installed grammars, so a grammar update that renames a node fails `meson test` instead of silently dropping colors.

### Compiling & Running the Test Suite
Every change must compile with **0 errors and 0 warnings** and pass all 21 automated GTest suites:

```bash
# Configure (defaults to prefix=~/.local)
meson setup builddir

# Compile all targets (kajo, kajo-ide, kajo-files, kajo-player, kajo-mod, kajo-settings, kajo-greeter)
meson compile -C builddir

# Run all 21 automated GTest suites
meson test -C builddir

# Install to ~/.local/bin and ~/.local/share/kajo
meson install -C builddir
```

### Automated Test Suites (`meson test -C builddir`)
| Suite Name | Binary | Coverage |
| :--- | :--- | :--- |
| `config-roundtrip` | `test-config` | `KajoConfigStore` per-domain TOML: compiled-vs-shipped defaults parity, round trips and clamping, `conf.d` overlays with base-save isolation, concurrent readers, syntax-error preservation, comment/unknown-key preserving patch, patcher never writing an unparsable file, subscriber thread delivery, legacy `config.json` migration. |
| `fuzz-toml-config` | `test-fuzz-toml-config` | Smoke run of the libFuzzer entry point over all five domain schemas. |
| `pam-auth` | `test-pam-auth` | `shell_pam_authenticate_async` worker thread lifecycle, empty password rejection, cancellation. |
| `greetd-ipc` | `test-greetd-ipc` | Mock `greetd` UNIX socket server (`uint32_t LE` + JSON), auth success/failure/cancel, JSON quote/backslash escaping, `getpwent()` user discovery, and `.desktop` `%U` field code stripping. |
| `idle-manager` | `test-idle-manager` | Idle timeout configuration, Keep Awake inhibitor toggling, callback notification. |
| `autohide-state` | `test-autohide` | Panel autohide states (`VISIBLE`, `HIDING`, `HIDDEN`, `REVEALING`), 2px trigger strip margins, popover/launcher hold-open, and live config reload. |
| `ipc-socket` | `test-ipc` | `$XDG_RUNTIME_DIR/kajo.sock` command dispatch (`lock`, `screensaver`, `reload-config`, etc.). |
| `widget-registry` | `test-registry` | Verifies all 22 registered `ShellWidgetClass` definitions have valid `.id` and `.name` fields. |
| `osk-xkb` | `test-osk-xkb` | XKB keymap generation (`us`, `fi`, etc.) and keysym/keycode resolution for `ShellOSK`. |
| `touch-gestures` | `test-touch-gestures` | Bezel swipe gesture directional thresholds, off-axis rejection, and action dispatch. |
| `files-model` | `test-files-model` | `KajoFileItem` formatting, sorting and filtering, async file operations (non-clobbering move, cancellation), breadcrumb chain and selection restore, clipboard parsing and 4 MB cap, executable toggle, `archive://` URI round-trip and path normalizer, archive listing/extraction/compression hardening (symlinked destination, hardlinks, `..`/absolute entries, modes, preview cache), disk-image classification and UDisks2 reply helpers. |
| `niri-shaders` | `test-shaders` | Headless EGL/GLES2 shader compilation validation for all bundled `data/shaders/*.glsl` presets. |
| `player-queue` | `test-player-queue` | `kajo-player` time/size formatting, sibling-folder media detection and mpv track-label helpers. |
| `mod-router` | `test-mod-router` | `kajo-mod` 5-tier format router (`UADE` vs `libsidplayfp` vs `libgme` vs `libopenmpt` vs `libxmp`), Amiga companion (`smpl.*`, `.ins`) & `.nfo` filtering, ProTracker sample header parsing, `libarchive` bundle extraction, and live non-zero PCM synthesis. |
| `ide-fsm` | `test-ide-fsm` | 10-state modal FSM transitions, Nordic text objects and `Space` leader chords. |
| `dap-client` | `test-dap-client` | DAP client session flow against the mock adapter (breakpoints, stop, stack, evaluate, errors, adapter death) and debuggee/adapter discovery. |
| `ide-tasks` | `test-ide-tasks` | Build/test/run/clean command resolution for every supported build system. |
| `lsp-client` | `test-lsp-client` | Async LSP client against the scripted mock server. The `ide-ipc` suite additionally drives `IdeLspManager` end to end (diagnostics, rename, format, definition) and unit-tests text edits, snippets and the server registry. |
| `ide-ts` | `test-ide-ts` | Tree-sitter token classification for Java, Go and C, language detection, incremental re-parse equals a fresh parse, error tolerance. |
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

**Configuration while developing** (see `docs/spec_toml_config.md`):

```bash
# Use a throw-away config directory instead of ~/.config/kajo (never touches your real files)
KAJO_CONFIG_DIR=/tmp/kajo-cfg ./builddir/kajo-settings

# Read shipped defaults from the source tree (the lookup no longer checks the current
# directory or a compiled-in source path; without this, a missing user file falls back to the
# installed defaults in ~/.local/share/kajo/defaults, /usr/local/share/kajo/defaults, /usr/share/kajo/defaults,
# and finally to the compiled defaults, which the test suite keeps identical to data/defaults/*.toml)
KAJO_DEFAULTS_DIR=$PWD/data/defaults ./builddir/kajo
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
| `reload-config` | — | Reloads `~/.config/kajo/shell.toml` (plus `conf.d/*.toml`) and reconciles panel geometry, idle timers, and active widgets in-place. |
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

---

## 6. Visual Inspection & UI Debugging

Compiling and unit tests do not catch layout, CSS or contrast bugs. Every UI change must be **rendered and compared with `docs/ui-gallery.html`** before it is called done.

### 6.1 Headless screenshots (`scripts/ui-shot.sh`)
Renders a Kajo app in a nested `cage` compositor on wlroots' headless backend (its own Wayland socket, 1280×720 output) and captures it with `grim`. Needs `cage` and `grim`; nothing appears on your desktop.

```bash
meson compile -C builddir
scripts/ui-shot.sh -o /tmp/ide.png builddir/kajo-settings --page ide      # one settings page
scripts/ui-shot.sh -o /tmp/files.png builddir/kajo-files ~/Downloads
scripts/ui-shot.sh -d 6 -o /tmp/p.png builddir/kajo-player some.mkv       # -d: seconds to wait
```

The script is isolated on purpose: private `XDG_RUNTIME_DIR`, `NIRI_SOCKET`/`WAYLAND_DISPLAY`/`DISPLAY` unset (Niri actions and `kajo` IPC can never reach your real session), throwaway **copies** of `~/.config/kajo` and `~/.local/share/kajo`, and the working tree's `style.css` copied over the CSS so you see your changes (`-k` keeps your installed CSS). It refuses to run the `kajo` shell. The D-Bus buses are your real ones, so pages read live state (Wi-Fi list, paired devices) but nothing is clicked. Do not wrap it in `dbus-run-session` (it activates your installed services and leaves FUSE mounts behind).

Limits: the output is fixed at 1280×720 and there is no input, so only the top of long/scrolling pages is visible and dialogs/popovers that need a click cannot be opened. For those, review the Blueprint/C code and test in a real session. Log lines such as `Gtk-CRITICAL` are echoed after the capture; treat any as a bug.

### 6.2 How to analyze a screenshot
Open the PNG (image viewer, or an AI agent reading the file) and crop/zoom for detail: `python3 -c "from PIL import Image; Image.open('/tmp/x.png').crop((420,150,1260,310)).resize((1680,320)).save('/tmp/crop.png')"`. Check, against the gallery:

| Check | What a deviation looks like |
| :--- | :--- |
| **Control type** | A tile/button/dropdown standing in for a switch, toggle card or segmented group (see §6.3) |
| **Geometry** | Rounded corners (only the OSK keycap may have 3px), double borders, a dropdown with an inner darker box |
| **Alignment** | Slider/dropdown columns starting at different x per row; labels floating above neighbours; readouts in different styles |
| **Overflow** | Right-hand content clipped at the viewport edge (also test at the default 960×640 window) |
| **Contrast/state** | Subtitle text unreadable on an accent fill; no visible difference between on/off/disabled |
| **Tokens** | Colours that don't match `@metro-*` roles; `Inter`/proportional text where Fira Code is required |
| **Fonts** | Preview text in a proportional font: the requested family is not installed and the code used a bare family name instead of `shell_ide_font_stack()` |

`fc-match "Fira Code"` shows what a family really resolves to. Validate CSS without a display:
`python3 -c "import gi;gi.require_version('Gtk','4.0');from gi.repository import Gtk;p=Gtk.CssProvider();p.connect('parsing-error',lambda *a:print(a[2].message));p.load_from_path('data/defaults/style.css')"` (no output = 0 errors).

On a real session, `GTK_DEBUG=interactive kajo-settings` opens the GTK inspector to read computed CSS nodes and `G_MESSAGES_DEBUG=all` shows GLib debug output.

### 6.3 UI controls: which one, and how
Specimens: `docs/ui-gallery.html` §04–§06. Rules: `docs/design_system.md` §4.3. Pick by what is being set:

| Control | Use for | Create it with |
| :--- | :--- | :--- |
| **Switch row** | Plain on/off preference whose title/subtitle say everything (mute, word wrap, lock on suspend) | `create_action_row(icon, title, subtitle, gtk_switch_new())` |
| **Toggle card** | A feature with a status line, an *unsupported* state or its own sub-settings (VRR, Autohide) | `create_toggle_card(icon, title, subtitle, active)`; read `gtk_toggle_button_get_active()`, react to `"toggled"`, update text with `toggle_card_set_subtitle()` |
| **Segmented group** | 2–4 short mutually exclusive choices (Top/Bottom, ICON-ONLY/TEXT-ONLY/FULL, 2/4/8) | `create_segmented_group(labels, selected)`; `segmented_group_connect/get_selected/set_selected` |
| **Dropdown** | Longer lists (timeouts, shaders, devices) | `create_string_dropdown(items)` or `DropDown` in Blueprint |

The **toggle card** (`.toggle-card`, shared with the panel's Control Center quick tiles) shows state with a 4px left indicator bar: dim `@metro-border` when off, lit `@metro-accent` when on, with only a faint tint on the body so text keeps its contrast; disabled = 45% opacity with the reason in the subtitle. Example:

```c
GtkWidget *vrr = create_toggle_card("display-symbolic", "Variable Refresh Rate (VRR)",
                                    available ? "Adaptive Sync Disabled" : "Unsupported by this output", active);
gtk_widget_set_sensitive(vrr, available);
g_signal_connect(vrr, "toggled", G_CALLBACK(on_vrr_toggled), NULL);   /* update the subtitle in the handler */
```

Adding a new component = implement the helper, add CSS in `data/defaults/style.css` (using tokens), add a specimen with usage notes to `docs/ui-gallery.html`, add the rule to `docs/design_system.md`, render it with `scripts/ui-shot.sh`, then sync the CSS (AGENTS.md §3).

---

## 7. Code Analysis for Bad Practices

Layered, cheapest first. Treat tool output as leads and confirm each finding by reading the code.

| Tool | Command | Finds | Notes |
| :--- | :--- | :--- | :--- |
| Normal build | `meson compile -C builddir` | Compiler errors/warnings (`-Wall -Wextra`) | Must be 0 warnings |
| ASan + UBSan | `scripts/asan-test.sh` | Use-after-free, overflows, UB (found the `scheme_id` UAF) | Leak reports from fontconfig/pango caches are noise; `ASAN_OPTIONS=detect_leaks=1` to see them |
| Extra warnings | `scripts/audit.sh warnings` | `-Wshadow -Wformat=2 -Wnull-dereference -Wduplicated-cond -Wlogical-op -Wcast-qual -Wstrict-prototypes` | `-Wcast-qual` on sort comparators is benign |
| Static analyzer | `scripts/audit.sh tidy` | clang-analyzer: null derefs, dead stores, stream misuse (found `if (x) {…}` followed by an unconditional `x->…`) | False positives: `g_autofree` (reported as leak), flag-enum casts, `sizeof(*ptr)` in `g_new0` |
| Function/file size | `scripts/audit.sh sizes` | Split candidates (> ~150-line functions, > ~1500-line files) | Heuristic parser |
| Duplication | `scripts/audit.sh dups` | Copy-pasted 8-line windows grouped by file pair | Whole re-implemented subsystems are not exact clones: grep for `pa_context_connect`, `kajo.sock`, `poppler_page_render` too |
| UI-thread blocking | `scripts/audit.sh blocking` | `g_spawn_sync`, `*_call_sync`, `g_bus_get_sync` | Each hit is a potential freeze; prefer `GSubprocess`/async D-Bus |
| Callback lifetimes | `scripts/audit.sh lifetimes` | Async calls with a NULL cancellable and a raw user_data | Hits that pass a `GWeakRef`/ctx struct are fine |
| Colour drift | `scripts/audit.sh colors` | Raw hex literals that should be `@metro-*` tokens | |

Manual techniques that paid off:
- **Probe external protocols with a harmless equivalent.** To learn whether `{"Action":"X"}` is valid, send an action with the same shape but no visible effect (e.g. `ToggleWindowUrgent` on a bogus id) with a short Python socket script and read the `{"Ok"|"Err"}` reply. Never send an action that changes the live session.
- **Mutation-check new tests.** Re-introduce the bug in a scratch copy (e.g. the old `"ToggleWindowFloating"` string) and confirm the test fails; a regression test that cannot fail is worthless.
- **Check what a family/format really resolves to** (`fc-match`, `file`, a Pango font map load) instead of assuming the name works.
- **Compare claims with code.** `rg` the README/manual feature claims against `src/` (e.g. spawn of a language server, a config key) before trusting documentation.
- **Audit ownership by pattern.** `rg "g_free\(.*\);\s*\n.*= g_strdup\("` style aliasing, `->pa_ctx` style raw pointers captured by callbacks, `static` helpers copied into several files.
- `meson test -C builddir --print-errorlogs` for failures; `G_DEBUG=fatal-warnings` makes any GLib/GTK warning abort a test under a debugger.

Recommended loop for a risky change: `audit.sh` leads → fix → `meson compile && meson test` → `scripts/asan-test.sh` → `scripts/ui-shot.sh` (for UI) → docs/gallery updated in the same commit.
