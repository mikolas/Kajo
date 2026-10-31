# Kajo

A high-performance, lightweight Wayland desktop shell, system settings suite, session lock, and `greetd` login greeter built for the [Niri](https://github.com/niri-wm/niri) Wayland compositor.

Built in standard C17 with GTK4, `gtk4-layer-shell`, `libpulse`, `libpam`, and Blueprint Markup. Designed for OLED displays, sub-0.05% idle CPU usage, and keyboard-driven operation.

---

## 🌟 Overview of Executables

The project builds and installs three integrated executables plus a `greetd` session wrapper:

| Executable | Purpose |
| :--- | :--- |
| **`kajo`** | Primary Wayland desktop shell daemon (status panel, 20 live widgets, multi-plugin app launcher, OSD overlays, `org.freedesktop.Notifications` daemon, `ext-idle-notify-v1` idle manager, Metro screen saver, and `ext-session-lock-v1` PAM lock screen) + instant CLI IPC client. |
| **`kajo-settings`** | Standalone 10-page Metro system settings application with live D-Bus, PipeWire/PulseAudio, Niri IPC, and Kajo IPC integration. |
| **`kajo-greeter`** | Graphical Wayland login greeter for `greetd` supporting both `niri` (`gtk4-layer-shell` multi-monitor) and `cage` (XDG fullscreen kiosk) compositors. |
| **`kajo-greeter-session`** | Wrapper launcher script for `/etc/greetd/config.toml`. |

---

## ✨ Key Features

### 🖥️ Status Panel & Hardware Telemetry (`kajo`)
- **20 Built-in Panel Widgets**: Workspaces, clock & calendar, volume mixer, control center, battery, network/Wi-Fi, bluetooth, media player, CPU/memory/disk/network telemetry, theme picker, keyboard layout, StatusNotifierItem system tray, notification history, power menu, privacy indicator, Do-Not-Disturb (DND), idle inhibitor ("Keep Awake"), active window title, launcher button, and custom user scripts.
- **16-Band Live FFT Spectrum Visualizer**: Dynamic MPRIS2 player discovery, album art hero view, and a 30 FPS 16-band Cooley-Tukey FFT spectrum analyzer capturing directly from `@DEFAULT_MONITOR@` (corked automatically when the popover closes for 0.0% background CPU usage).
- **OLED Smart Autohide**: Smooth cubic-bezier slide animations with a 2px transparent Wayland edge trigger strip, popover/launcher hold-open guards, and live config reload.

### 🔒 Native Session Lock, Screen Saver & Idle Manager (`kajo`)
- **Wayland Session Lock (`ext-session-lock-v1`)**: Multi-monitor lock screen via `gtk4-session-lock` with asynchronous non-blocking PAM authentication (`libpam`), `systemd-logind` `Lock`/`Unlock`/`PrepareForSleep` integration, and scrubbed credential memory (`explicit_bzero`).
- **Metro Screen Saver**: Fullscreen layer-shell overlay with 3 visual modes (`clock`, `tiles`, `blank`) and OLED burn-in drift protection, plus automatic Niri DPMS monitor power-off.
- **Wayland Idle & Sleep Manager (`ext-idle-notify-v1`)**: Configurable inactivity timeouts for screen saver, monitor DPMS power-off, and automatic session lock, synchronized with the panel and Control Center **Keep Awake** (`systemd-logind` idle inhibitor FD).

### ⚙️ Kajo Settings Application (`kajo-settings`)
- **Displays & Idle Security (`displays`)**: Queries live monitor modes, refresh rates, fractional scale, orientation, and VRR from `niri msg -j outputs`, applies changes immediately via Niri IPC, persists to `~/.config/niri/config.kdl`, and configures Screen Saver & Session Lock timeouts.
- **Network & Wi-Fi (`network`)**: Live NetworkManager D-Bus scanning, connection switching, and inline WPA/WPA2 password authentication.
- **Bluetooth (`bluetooth`)**: Dynamic BlueZ adapter discovery (`GDBusObjectManager`), live device scanning, pairing, and connection toggling.
- **Audio & PipeWire (`audio`)**: Asynchronous non-blocking PulseAudio/PipeWire context for default output sink selection, microphone input gain/mute, and per-application stream volume sliders.
- **Theme (`theme`), Panel (`panel`), Widgets (`widgets`), Notifications (`notifications`), Niri (`niri`), About (`about`)**: Live accent color switching (`@metro-accent`), panel edge/height/autohide configuration, interactive widget ordering (`▲`/`▼`) and display mode (`icon-only`, `text-only`, `full`) reconciliation without restarting `kajo`.

### 🔐 Kajo Login Greeter (`kajo-greeter`)
- **Native `greetd` IPC Client**: Speaks the `greetd` binary protocol (`$GREETD_SOCK`) with zero-copy scrubbed password serialization, `prctl(PR_SET_DUMPABLE, 0)` core-dump protection, and GTK Inspector lockout.
- **Automatic User & Session Discovery**: Enumerates human users via `getpwent()` and parses `/usr/share/wayland-sessions/*.desktop` & `/usr/share/xsessions/*.desktop`, remembering the last user and session.
- **Dual Compositor & Desktop Preview Support**: Runs natively on multi-monitor `niri` (`gtk4-layer-shell`), single-monitor `cage` (`--fullscreen`), or inside your active desktop session (`kajo-greeter --test`) for instant previewing without logging out.

---

## 🛠️ Building and Installing

### 1. System Dependencies
Ensure the following development packages are installed (Arch Linux / CachyOS package names shown):
- `gtk4` (≥ 4.12)
- `gtk4-layer-shell` (≥ 1.0, includes `gtk4-session-lock`)
- `wayland`, `wayland-protocols`
- `json-glib`, `glib2` (`gio-2.0`, `gio-unix-2.0`)
- `libpulse` (`libpulse`, `libpulse-mainloop-glib`)
- `pam` (`libpam`)
- `blueprint-compiler`, `meson`, `ninja`

```bash
sudo pacman -S --needed gtk4 gtk4-layer-shell wayland wayland-protocols \
    json-glib glib2 libpulse pam blueprint-compiler meson ninja
```

### 2. Build & User Installation (`~/.local`)
By default, `meson.build` configures `prefix=~/.local`, installing binaries to `~/.local/bin` and default data/icons to `~/.local/share/kajo/`:

```bash
meson setup builddir
meson compile -C builddir
meson test -C builddir
meson install -C builddir
```

Ensure `~/.local/bin` is in your `$PATH`.

### 3. Do You Need to Copy Config or Theme Files Manually?
**No.** You do **not** need to copy `data/defaults/config.json` or `data/defaults/style.css` into `~/.config/kajo/`.

Kajo uses an automatic 3-tier asset resolution chain:
1. **User Overrides (`~/.config/kajo/`)**: Created automatically when you click **Apply / Save** in `kajo-settings` (`~/.config/kajo/config.json` and `~/.config/kajo/theme-override.css`).
2. **Installed System/User Share (`~/.local/share/kajo/defaults/` & `/usr/local/share/kajo/defaults/`)**: Installed automatically by `meson install -C builddir`.
3. **Source Tree Fallback (`data/defaults/`)**: Used automatically when running binaries directly from `./builddir/`.

---

## 🖥️ Configuring Kajo for Niri

Add the following to your `~/.config/niri/config.kdl`:

```kdl
// 1. Launch Kajo shell daemon at session startup
spawn-at-startup "kajo"

// 2. Recommended Keybindings (Instant UNIX Socket IPC Client Mode)
binds {
    // Application Launcher & Settings
    Mod+D             { spawn "kajo" "--toggle-launcher"; }
    Mod+S             { spawn "kajo-settings"; }

    // Session Lock & Screen Saver
    Mod+Shift+L       { spawn "kajo" "--lock"; }
    Mod+Shift+S       { spawn "kajo" "--screensaver"; }

    // Audio Volume & Mute (with Kajo OSD)
    XF86AudioRaiseVolume allow-when-locked=true { spawn "kajo" "--volume-up"; }
    XF86AudioLowerVolume allow-when-locked=true { spawn "kajo" "--volume-down"; }
    XF86AudioMute        allow-when-locked=true { spawn "kajo" "--volume-mute"; }

    // Display Backlight Brightness (with Kajo OSD)
    XF86MonBrightnessUp   allow-when-locked=true { spawn "kajo" "--brightness-up"; }
    XF86MonBrightnessDown allow-when-locked=true { spawn "kajo" "--brightness-down"; }
}
```

---

## 🔐 Installing & Testing `kajo-greeter` for `greetd`

### Testing Without Logging Out (Two Safe Desktop Modes)

Before enabling `kajo-greeter` in `/etc/greetd/config.toml`, you can test it directly inside your running desktop session:

1. **Stage 1 — Instant Window Preview (`--test`)**:
   Opens `kajo-greeter` in a regular `1280x800` window on your desktop and verifies your password against PAM (`libpam`) without starting a new session:
   ```bash
   ./builddir/kajo-greeter --test --cmd niri-session
   ```
   *(Press `Ctrl+Esc` at any time to close the preview window.)*

2. **Stage 2 — Nested `niri` Compositor Test**:
   Runs a nested windowed `niri` compositor using the locked-down greeter config:
   ```bash
   KAJO_GREETER_TEST=1 KAJO_GREETER_QUIT_NIRI=1 niri -c data/greetd/niri-greeter.kdl
   ```

### System-Wide Installation for `greetd`

Because the `greetd` daemon runs as the unprivileged system user `greeter` (which cannot access `/home/<user>/.local/`), use the bundled installer script [data/greetd/install-greeter.sh](data/greetd/install-greeter.sh) to copy `kajo-greeter`, `kajo-greeter-session`, `niri-greeter.kdl`, and the default theme/icons to `/usr/local` and `/etc/greetd`:

```bash
# Install files to /usr/local & /etc/greetd
./data/greetd/install-greeter.sh

# Or install AND automatically activate in /etc/greetd/config.toml (backs up config.toml.bak)
./data/greetd/install-greeter.sh --enable

# To revert /etc/greetd/config.toml back to your previous greeter at any time:
./data/greetd/install-greeter.sh --restore
```

If configuring `/etc/greetd/config.toml` manually:

**Option A — Using `niri` (Recommended: Multi-Monitor + Native HiDPI)**:
```toml
[terminal]
vt = 1

[default_session]
command = "kajo-greeter-session --cmd niri-session"
user = "greeter"
```

**Option B — Using `cage` (Single-Monitor Kiosk Compositor)**:
```toml
[terminal]
vt = 1

[default_session]
command = "cage -s -- kajo-greeter --cmd niri-session"
user = "greeter"
```

---

## 💻 Command-Line Interface (CLI) Reference

### 1. `kajo` (Desktop Shell Daemon & IPC Client)
When invoked with no arguments, `kajo` starts as the Wayland desktop shell daemon. When invoked with an argument, it sends an IPC command to `$XDG_RUNTIME_DIR/kajo.sock`:

| Option / Argument | Aliases | Description |
| :--- | :--- | :--- |
| *(no arguments)* | — | Starts the Kajo Wayland desktop shell daemon. |
| `--toggle-launcher` | `-t`, `launcher-toggle` | Toggles the application launcher overlay. |
| `--lock` | `lock` | Immediately activates the `ext-session-lock-v1` PAM lock screen. |
| `--screensaver` | `screensaver` | Immediately starts the fullscreen Metro screen saver overlay. |
| `--volume-up` | `volume-up` | Increases default audio sink volume by 5% and shows OSD. |
| `--volume-down` | `volume-down` | Decreases default audio sink volume by 5% and shows OSD. |
| `--volume-mute` | `volume-mute` | Toggles default audio sink mute state and shows OSD. |
| `--brightness-up` | `brightness-up` | Increases display backlight brightness by 5% and shows OSD. |
| `--brightness-down` | `brightness-down` | Decreases display backlight brightness by 5% and shows OSD. |

### 2. `kajo-settings` (System Settings Application)
| Option / Flag | Description |
| :--- | :--- |
| *(no arguments)* | Opens the settings window on the default `displays` page. |
| `-p <PAGE>`, `--page <PAGE>` | Opens directly to a specific page: `displays`, `network`, `bluetooth`, `audio`, `theme`, `panel`, `widgets`, `notifications`, `niri`, or `about`. |

### 3. `kajo-greeter` (`greetd` Login Greeter)
| Option / Flag | Description |
| :--- | :--- |
| `-c <CMD>`, `--cmd <CMD>` | Default session command (also read from `$KAJO_GREETER_CMD`). |
| `-u <USER>`, `--user <USER>` | Pre-selected username (also read from `$KAJO_GREETER_USER`). |
| `-f`, `--fullscreen` | Force standard XDG fullscreen window mode (used automatically under `cage`). |
| `-t`, `--test` | Desktop preview mode (`1280x800` window using PAM verification; automatically disabled if `$GREETD_SOCK` is set). |

---

## ⌨️ Launcher Search Prefix Modes

When the launcher is open (`Mod+D` or `kajo --toggle-launcher`):

| Prefix | Plugin Mode | Example | Action on `Enter` |
| :--- | :--- | :--- | :--- |
| *(None)* | **App Search** | `firefox`, `term` | Launches matching `.desktop` application. |
| `:` | **Emoji Picker** | `:fire`, `:rocket` | Copies selected emoji (from 1,914 entries) to clipboard. |
| `=` | **Calculator** | `= (24 * 60) + 120` | Evaluates math expression via TinyExpr and copies result. |
| `>` | **Shell Command** | `> htop` | Runs command in terminal or spawns directly. |
| `w:` / `W:` | **Window Switcher** | `w:code` | Focuses matching open Niri window. |

---

## 📄 Configuration File Locations

| Path | Managed By | Purpose |
| :--- | :--- | :--- |
| **`~/.config/kajo/config.json`** | `kajo-settings` / User | Panel edge, height, autohide settings, widget layout (`left`, `center`, `right`), widget modes, notification timeout, and `idle` security settings (`screensaver_mode`, `screensaver_timeout_sec`, `dpms_timeout_sec`, `lock_timeout_sec`, `lock_on_suspend`). |
| **`~/.config/kajo/theme-override.css`** | `kajo-settings` (`theme`) | Live Metro accent color definitions (`@define-color metro-accent ...`) and user CSS overrides, hot-reloaded via IPC. |
| **`~/.config/niri/config.kdl`** | `kajo-settings` (`displays` / `niri`) | Output resolution/scale/transform/VRR blocks and Niri layout/focus-ring settings. |
| **`/var/cache/kajo-greeter/state.json`** | `kajo-greeter` | Persists `last_user` and `last_session` across logins (falls back to `~/.cache/kajo/greeter-state.json` in `--test` mode). |

---

## 📄 License & Attribution

- **Icons**: Incorporates [Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons) by Microsoft Corporation (MIT License).
- **License**: MIT License — see [LICENSE](LICENSE).

## Author

Mikolas Hämäläinen — [mikolas@mikolas.net](mailto:mikolas@mikolas.net)
