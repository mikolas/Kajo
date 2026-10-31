# Developer Guide & Contribution Rules

This document provides technical guidelines, architecture maps, testing workflows, and contribution standards for developers working on `kajo`, `kajo-settings`, and `kajo-greeter`.

---

## 1. Architecture & Source File Map

The codebase is written in standard C17 (`-std=c17`) using GTK4, `gtk4-layer-shell-0` (including `gtk4-session-lock`), `wayland-client`, `json-glib-1.0`, `gio-2.0`, `libpulse`, `libpulse-mainloop-glib`, and `libpam`.

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
├── settings/
│   ├── main.c                      kajo-settings standalone entry point (org.kajo.Settings)
│   ├── settings_window.h/c         Settings container shell, category sidebar & GtkStack
│   ├── settings_common.h/c         Shared Metro card, tile, dropdown & IPC helpers
│   └── pages/
│       └── page_*.c                10 modular preference pages (displays, network, bluetooth,
│                                   audio, theme, panel, widgets, notifications, niri, about)
├── vendor/
│   └── tinyexpr/                   TinyExpr recursive-descent math expression evaluator
└── widgets/
    ├── widget.h                    ShellWidgetClass vtable interface & mode visibility helper
    ├── registry.h/c                Panel widget registry
    ├── media.c                     MPRIS2 controller & 16-band Cooley-Tukey FFT visualizer
    ├── tray.c                      StatusNotifierItem (SNI) watcher & ARGB32 pixmap decoder
    └── *.c                         20 built-in panel widget implementations

data/
├── defaults/
│   ├── config.json                 Default panel, widget, notification & idle configuration
│   ├── style.css                   Master GTK4 CSS stylesheet (panel, popovers, settings, lock, greeter)
│   └── icon-map.json               Logical-to-symbolic icon mappings
├── greetd/
│   ├── niri-greeter.kdl            Locked-down Niri configuration for the greetd login session
│   └── kajo-greeter-session        Wrapper launcher script for /etc/greetd/config.toml
└── ui/
    └── *.blp                       Blueprint Markup (.blp) declarative UI templates

tests/
├── test_config.c                   Config JSON load/save roundtrip & defaults resolution tests
├── test_pam_auth.c                 Async PAM worker error/null/cancellation handling tests
├── test_idle_manager.c             Idle manager timeout & inhibitor state machine tests
├── test_ipc.c                      UNIX domain socket IPC server/client dispatch tests
├── test_autohide.c                 Autohide state transitions, hold-open & margin calculation tests
├── test_registry.c                 Widget registry completeness & lifecycle tests
└── test_greetd_ipc.c               greetd binary framing, auth roundtrip & session parser tests
```

---

## 2. Build, Test & Debug Workflow

### Prerequisites
- `gtk4` (≥ 4.12), `gtk4-layer-shell` (≥ 1.0)
- `wayland-client`, `wayland-protocols`, `wayland-scanner`
- `json-glib-1.0`, `gio-2.0`, `gio-unix-2.0`
- `libpulse`, `libpulse-mainloop-glib`
- `pam` (`libpam`)
- `blueprint-compiler`, `meson`, `ninja`

### Compiling & Running the Test Suite
Every change must compile with **0 errors and 0 warnings** and pass all 7 automated GTest suites:

```bash
# Configure (defaults to prefix=~/.local)
meson setup builddir

# Compile all targets (kajo, kajo-settings, kajo-greeter)
meson compile -C builddir

# Run all 7 automated GTest suites
meson test -C builddir

# Install to ~/.local/bin and ~/.local/share/kajo
meson install -C builddir
```

### Automated Test Suites (`meson test -C builddir`)
| Suite Name | Binary | Coverage |
| :--- | :--- | :--- |
| `config-roundtrip` | `test-config` | `ShellConfig` JSON serialization, `idle` / `panel` / `layout` fields, defaults fallback. |
| `pam-auth` | `test-pam-auth` | `shell_pam_authenticate_async` worker thread lifecycle, empty password rejection, cancellation. |
| `greetd-ipc` | `test-greetd-ipc` | Mock `greetd` UNIX socket server (`uint32_t LE` + JSON), auth success/failure/cancel, JSON quote/backslash escaping, `getpwent()` user discovery, and `.desktop` `%U` field code stripping. |
| `idle-manager` | `test-idle-manager` | Idle timeout configuration, Keep Awake inhibitor toggling, callback notification. |
| `autohide-state` | `test-autohide` | Panel autohide states (`VISIBLE`, `HIDING`, `HIDDEN`, `REVEALING`), 2px trigger strip margins, popover/launcher hold-open, and live config reload. |
| `ipc-socket` | `test-ipc` | `$XDG_RUNTIME_DIR/kajo.sock` command dispatch (`lock`, `screensaver`, `reload-config`, etc.). |
| `widget-registry` | `test-registry` | Verifies all 20 registered `ShellWidgetClass` definitions have valid `.id` and `.name` fields. |

### Running & Debugging Individual Executables
```bash
# 1. Run main desktop shell
./builddir/kajo

# 2. Run settings app directly on a specific page
./builddir/kajo-settings --page audio

# 3. Run kajo-greeter in safe desktop preview mode (uses PAM verification, no logout)
./builddir/kajo-greeter --test --cmd niri-session

# 4. Run kajo-greeter inside a nested Niri compositor window on your desktop
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
