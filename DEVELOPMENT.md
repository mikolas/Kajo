# Developer Guide & Contribution Rules

This document provides technical guidelines, architecture maps, and contribution standards for developers working on `kajo` and `kajo-settings`.

---

## 1. Architecture & Source File Map

The codebase is built in C17 using GTK4, `gtk4-layer-shell`, `json-glib`, `gio`, `libpulse`, and `libpulse-mainloop-glib`.

```
src/main.c                          Daemon entry point & IPC client mode
src/shell.h/c                       ShellApp — core subsystem wiring & GTK application
src/panel.h/c                       Panel layer-shell surface & OLED trigger strip
src/autohide.h/c                    Autohide reveal/hide state machine
src/config.h/c                      JSON configuration parser & serializer (shell_config_save)
src/theme.h/c                       3-Tier GTK CSS theme engine & GFileMonitor hot-reload
src/icons.h/c                       Icon resolution (Fluent UI SVGs + system theme fallback)
src/compositor/compositor.h         Compositor abstraction interface
src/compositor/niri.c               Niri UNIX socket ($NIRI_SOCKET) event stream & RPC actions
src/launcher/launcher_surface.h/c   App launcher overlay surface & plugin router
src/osd/osd.h/c                     Volume/brightness/toast OSD overlay surface
src/notifications/daemon.h/c        D-Bus notification daemon (org.freedesktop.Notifications)
src/settings/main.c                 kajo-settings standalone entry point (org.kajo.Settings)
src/settings/settings_window.h/c    Control panel window layout, sidebar, & 7 settings pages
src/ipc/socket.h/c                  UNIX domain socket server ($XDG_RUNTIME_DIR/kajo.sock)
src/widgets/widget.h                Widget VTable interface (create, update, destroy)
src/widgets/registry.h/c            19 panel widget registrations
src/widgets/*.c                     Individual widget implementations
```

---

## 2. Debug Build & Development Workflow

### Prerequisites
* `gtk4` (≥ 4.12)
* `gtk4-layer-shell`
* `json-glib-1.0`
* `gio-2.0`
* `libpulse`
* `libpulse-mainloop-glib`

### Debug Compilation
```sh
# Clone and setup debug build with symbol table
git clone git@github.com:mikolas/desktop.git
cd desktop
meson setup builddir -Dbuildtype=debug
ninja -C builddir
```

### Running Executables in Debug Mode
```sh
# Run main shell in verbose debug mode (requires Niri compositor)
NIRI_SOCKET=/run/user/1000/niri.sock ./builddir/kajo

# Run control panel app
./builddir/kajo-settings
```

---

## 3. UNIX Domain Socket IPC Protocol Reference (`$XDG_RUNTIME_DIR/kajo.sock`)

Client commands are transmitted over the UNIX domain socket in instant IPC client mode:

```c
/* Protocol Payload Format: <command_string>\n */
```

* `launcher-toggle`: Toggles application launcher surface overlay.
* `volume-up`: Increases PulseAudio sink volume by 5% and triggers OSD.
* `volume-down`: Decreases PulseAudio sink volume by 5% and triggers OSD.
* `volume-mute`: Toggles PulseAudio sink mute state and triggers OSD.
* `brightness-up`: Increases display brightness via Logind Seat API.
* `brightness-down`: Decreases display brightness via Logind Seat API.

---

## 4. How to Add a New Panel Widget

To add a new widget to the desktop panel:

1. **Create Source File** (`src/widgets/my_widget.c`):
   Implement the `ShellWidgetClass` vtable:
   ```c
   #include "widget.h"

   static GtkWidget *my_widget_create(ShellWidget *widget, ShellApp *app) {
       /* Construct GTK4 widget */
   }

   static void my_widget_update(ShellWidget *widget) {
       /* Update widget UI */
   }

   static void my_widget_destroy(ShellWidget *widget) {
       /* Free private resources */
   }

   const ShellWidgetClass my_widget_class = {
       .name = "my-widget",
       .create = my_widget_create,
       .update = my_widget_update,
       .destroy = my_widget_destroy,
   };
   ```

2. **Register in Registry** (`src/widgets/registry.c`):
   ```c
   extern const ShellWidgetClass my_widget_class;
   shell_widget_registry_register("my-widget", &my_widget_class);
   ```

3. **Update Build System** (`meson.build`):
   Add `'src/widgets/my_widget.c'` to `desktop_shell_sources`.

---

## 5. Strict Contribution Rules & Coding Standards

All pull requests and code modifications must adhere to the following rules:

1. **Zero External Shell Commands Policy**:
   * All system controls (audio, network, session power, compositor actions) **MUST** use native C APIs, GDBus (`gio`), or UNIX domain sockets (`$NIRI_SOCKET`).
   * Spawning external CLI binaries (`pactl`, `wpctl`, `nmcli`, `loginctl`, `pavucontrol`) via `g_spawn_command_line_sync` is strictly prohibited.

2. **User Master Configuration File Sanctity**:
   * **NEVER** touch, mutate, or edit user master configuration files (such as `~/.config/niri/config.kdl`).
   * All shell-managed compositor rules belong strictly in modular include files (`~/.config/niri/config.d/00-kajo.kdl`, `~/.config/niri/config.d/10-outputs.kdl`).

3. **Strict XDG Configuration Directory Scoping**:
   * All settings persistence must write strictly to user XDG paths (`~/.config/kajo/` via `g_get_user_config_dir()`).
   * Never write files to `/tmp`, project root, or desktop directories during runtime.

4. **Locale-Independent String & Float Parsing**:
   * Always use `g_ascii_strtod()` for string-to-float conversions to prevent locale bugs on non-US systems.

5. **Non-Empty String Guards in Window Matching**:
   * All string searching in compositor window focus logic (`niri.c`) must be explicitly guarded against empty strings (`*str != '\0'`) to prevent matching window #0.

6. **Unified Metro Flat Aesthetic Standard**:
   * All UI surfaces must conform to the 100% flat Metro design system: `#141414` dark surface, 13px Fira Code monospace typography, crisp 1px `#2b2b2b` pane dividers, and flat 2-state Metro toggle tiles.
