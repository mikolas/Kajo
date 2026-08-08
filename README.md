# Kajo

A high-performance, lightweight Wayland desktop environment and control panel for the [Niri](https://github.com/niri-wm/niri) compositor. Provides an integrated status panel, application launcher, on-screen display overlays, and a Freedesktop notification daemon as a single process.

Built in standard C17 with GTK4, gtk4-layer-shell, and Blueprint Markup. Designed for OLED displays, minimal resource usage, and complete keyboard-driven operation.

---

## 🌟 Key Features

### 🖥️ Status Panel & Telemetry
- **20 Built-in Widgets**: Workspaces, clock, volume dropdown, control center, battery, network, bluetooth, media, CPU/memory, disk telemetry, network throughput, theme picker, keyboard layout, system tray, notifications, power menu, privacy, DND, idle-inhibitor, and custom user scripts.
- **Hardware Telemetry**: Real-time normalized CPU load (`btop` parity), 2-column per-core load grid (`C01`...`C12`), RAPL/HWMON power draw (Watts), physical disk space (`statvfs`/`/proc/mounts` deduplication), and live dual network throughput (`⬇ RX` / `⬆ TX`).
- **16-Band Live PulseAudio FFT Spectrum Visualizer**: Dynamic MPRIS player bus discovery, 2-column hero album art popover, and real-time 16-band vertical FFT spectrum analyzer ($86\text{ Hz} \dots 21.5\text{ kHz}$) using a zero-dependency 512-point Cooley-Tukey FFT engine with logarithmic $\log_{10}$ decibel compression and volume-independent dynamic auto-sensitivity (`auto_sens`).
- **Persistent Audio Control**: Integrated PulseAudio/WirePlumber C API sink selector (`pa_context_set_default_sink`).
- **Metro Theme & Accent Switcher**: Live accent color switcher with 8 presets (Cobalt Blue, Windows Purple, Catppuccin, Nord, Emerald, Crimson, Amber Gold, Cyberpunk Cyan) dynamically updating UI controls and telemetry LevelBars in sub-10ms.

### ⚙️ Kajo Settings Control Panel *(Under Construction / Proof of Concept)*
- **Proof of Concept Application (`kajo-settings`)**: The current settings application serves as an initial functional proof-of-concept for display output configuration, Niri window rules, and per-application WirePlumber audio stream mixing.
- **Upcoming Revamp**: `kajo-settings` is undergoing active redesign and will be completely overhauled in an upcoming release.

### 🚀 Launcher, OSD & Notifications
- **Multi-Plugin Launcher**: Keyboard-driven app launcher, emoji picker (1914 entries), calculator, terminal command runner, and window switcher.
- **Sub-Millisecond OSD**: Instant UNIX domain socket IPC overlays for volume, brightness, and media feedback.
- **Notification Daemon**: Integrated `org.freedesktop.Notifications` server with unread history popover, PWA window title focus routing, and custom toast timeouts.

### ⚡ Performance, Zero-CPU Popout Gating & Reliability
- **Popout-Gated Resource Architecture**: PulseAudio monitor streams start corked (`PA_STREAM_START_CORKED`); 30 FPS FFT timers, audio callbacks, sysfs 128-core frequency reads, and popover widget decodes run **only when popouts are mapped open**. Background idle CPU footprint is $<0.05\%$.
- **Robust StatusNotifierItem Tray Engine**: Full `org.kde.StatusNotifierWatcher` implementation with safe GIO name watcher lifecycle, ARGB32 `IconPixmap` bitmap decoding, and fallback theme lookups for Electron, Qt, and GTK applications.
- **Zero Command Spawning**: 100% native GDBus, PulseAudio C API, and Niri socket IPC.
- **Smart Icon Engine**: Case-insensitive `.desktop` file parsing via `GKeyFile`, `Papirus-Dark` theme integration, and GTK symbolic fallback pipeline.
- **OLED Auto-Hide**: 100% transparent edge trigger strip with smooth cubic ease reveal/hide animations.

---

## 🛠️ Building and Installing

### Dependencies
`gtk4` (≥ 4.12), `gtk4-layer-shell`, `json-glib`, `gio-2.0`, `libpulse`, `libpulse-mainloop-glib`, `blueprint-compiler`

```sh
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

By default, `meson.build` sets the installation prefix to `~/.local`, placing binaries in `~/.local/bin` and data assets in `~/.local/share/kajo/`.

### Running & Keybinding Integration in Niri

Add the following to your `~/.config/niri/config.kdl`:

```kdl
// 1. Include modular desktop settings (managed by kajo-settings)
include "config.d/*.kdl"

// 2. Launch kajo at startup
spawn-at-startup "kajo"

// 3. Keybindings (Fast UNIX Domain Socket Client Mode)
binds {
    Mod+D { spawn "kajo" "--toggle-launcher"; }
    Mod+S { spawn "kajo-settings"; }
    XF86AudioRaiseVolume { spawn "kajo" "--volume-up"; }
    XF86AudioLowerVolume { spawn "kajo" "--volume-down"; }
    XF86AudioMute        { spawn "kajo" "--volume-mute"; }
    XF86MonBrightnessUp   { spawn "kajo" "--brightness-up"; }
    XF86MonBrightnessDown { spawn "kajo" "--brightness-down"; }
}
```

---

## 📄 Configuration Files

All user edits made via `kajo-settings` or manual edits are strictly written to your user XDG configuration directory (`~/.config/`). Master user files (such as `~/.config/niri/config.kdl`) are **never modified directly**.

| Configuration File Path | Purpose & Managed Settings |
| :--- | :--- |
| **`~/.config/kajo/config.json`** | Panel geometry, edge position (top/bottom), autohide delays, widget modes (`icon`, `label`, `full`), custom script widgets (`custom-script`), notification toast timeouts (`toast_timeout_ms`), and max history limits. |
| **`~/.config/niri/config.d/00-kajo.kdl`** | Modular Niri include file managed by `kajo-settings` for panel layer rules (opacity, blur, xray), floating window rules, gaps, and focus ring colors. *(Requires `include "config.d/*.kdl"` in `config.kdl`)*. |
| **`~/.config/niri/config.d/10-outputs.kdl`** | Modular Niri include file for hardware display outputs, resolutions, refresh rates, scaling, and VRR. |
| **`~/.config/kajo/style.d/00-accent.css`** | GTK CSS theme snippet managed by `kajo-settings` defining live Metro accent color tokens (`@define-color metro-accent #0078d4;`). |
| **`~/.config/kajo/style.css`** | User custom GTK CSS stylesheet overrides. |

### 🛠️ Custom Script Widgets (`custom-script`)

`kajo` supports custom user script widgets that execute shell commands or scripts at configurable intervals and display real-time output in the panel:

```json
"widgets": {
  "custom_weather": {
    "type": "custom-script",
    "exec_cmd": "data/examples/scripts/weather.sh Helsinki",
    "interval_sec": 60,
    "click_left_cmd": "zen https://wttr.in/Helsinki",
    "click_right_cmd": "kitty -e curl wttr.in/Helsinki",
    "mode": "both",
    "icon": "weather-clear-symbolic"
  }
}
```

Pre-made example scripts are bundled in `data/examples/scripts/` (`weather.sh`, `crypto.py`, `vpn_status.sh`, `disk_usage.sh`). For full documentation, refer to [docs/script_widgets.md](docs/script_widgets.md).

---

## 📄 License & Attribution

- **Icons**: Incorporates [Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons) by Microsoft Corporation, used under the MIT License.
- **License**: This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.

## Author

Mikolas Hämäläinen — [mikolas@mikolas.net](mailto:mikolas@mikolas.net)
