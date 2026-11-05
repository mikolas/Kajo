# Kajo Desktop Environment — Roadmap & Future Ideas (`ROADMAP.md`)

This document tracks completed milestones, active specifications, and future first-party applications and enhancements planned for the **Kajo** desktop suite on the **Niri Wayland Compositor**.

---

## 1. Visual & Architectural Identity ("Aero + Metro")

All Kajo components and standalone applications share a unified design language:
- **Frameless Aero + Metro Chrome:** Custom `GtkWindowHandle` header bar with technical uppercase titles (`⚙ SYSTEM SETTINGS`, `📁 KAJO FILES`), `🗗 FLOAT / TILE` Niri IPC button, and `#e81123` close button.
- **Crisp Dark Surfaces:** `#111111` / `#141414` / `#181818` surfaces, `0px` corner radius on cards and windows, `1px solid #2b2b2b` structural borders, and live `@metro-accent` highlights synced from `~/.config/kajo/style.d/00-accent.css`.
- **Technical Monospace Typography:** `Fira Code` headers, status badges (`[ MKV | 14.8 MB ]`), and telemetry readouts.
- **Popout- & Map-Gated Resource Discipline:** Zero background CPU/audio polling when surfaces or previews are unmapped.

---

## 2. Completed Milestones

| Component / Binary | Status | Specification / Notes |
| :--- | :---: | :--- |
| **`kajo` (Core Shell & Panel)** | ✅ Done | Panel, 20+ widgets, 16-band FFT Spectrum Analyzer, MPRIS2, SNI Tray, Notification Daemon, App Launcher, Session Lock & Screensaver |
| **`kajo-settings` (`org.kajo.Settings`)** | ✅ Done | 10 modular settings pages (Displays, Network, Bluetooth, Audio, Theme, Panel & Touch, Widgets, Notifications, Niri Shaders, About) |
| **`kajo-greeter`** | ✅ Done | Native `greetd` Wayland login greeter with PAM authentication & session selector |
| **Foldable & 2-in-1 Posture Manager** | ✅ Done | [`SPEC_FOLDABLE_SENSORS.md`](file:///home/mikolas/src/Kajo/SPEC_FOLDABLE_SENSORS.md) — Hinge angle, tablet mode, IIO accelerometer auto-rotation |
| **Niri GLSL Window Shaders** | ✅ Done | [`SPEC_NIRI_SHADERS.md`](file:///home/mikolas/src/Kajo/SPEC_NIRI_SHADERS.md) — Live shader preset switcher for window open/close/resize animations |
| **Touchscreen Bezel Swipe Gestures** | ✅ Done | [`SPEC_TOUCH_GESTURES.md`](file:///home/mikolas/src/Kajo/SPEC_TOUCH_GESTURES.md) — Edge layer-shell recognizers for Launcher, Overview, OSK, and Notifications |
| **Virtual Keyboard (`ShellOSK`)** | ✅ Done | [`SPEC_VIRTUAL_KEYBOARD.md`](file:///home/mikolas/src/Kajo/SPEC_VIRTUAL_KEYBOARD.md) — Windows Touch Keyboard flat 28-column proportional grid with XKB keymap & layout switching |
| **Kajo Files (`kajo-files`, `org.kajo.Files`)** | 🧪 Alpha | [`SPEC_FILE_BROWSER.md`](file:///home/mikolas/src/Kajo/SPEC_FILE_BROWSER.md) — GVFS/UDisks2 file manager with natural sorting, hit-tested context menu (`Open With / Set Default`, `libarchive` `Extract Here`), and `Space` Quick Look (`GtkPicture`, `libmpv` + `GtkGLArea`, Poppler PDFs, `libarchive` archives, code/text) |
| **Kajo Player (`kajo-player`, `org.kajo.Player`)** | 🚀 Beta | [`SPEC_VIDEO_PLAYER.md`](file:///home/mikolas/src/Kajo/SPEC_VIDEO_PLAYER.md) — `libmpv` + `GtkGLArea` media player with subtitle/audio stream selectors, external `.srt`/`.ass` loading, sync delay, speed/frame-stepping, chapters, screenshots, fullscreen overlay HUD, and MPRIS2 D-Bus integration |

---

## 3. Future Roadmap (Not Yet Implemented)

| Planned Component / Feature | Status | Target Binary / Scope |
| :--- | :---: | :--- |
| **`kajo-files` Phase 2** | 📋 Planned | `kajo-files` (Archive extraction, context menu, DnD) |
| **Kajo Document & Image Viewer** | 📋 Planned | `kajo-viewer` (`org.kajo.Viewer`) |
| **Kajo System Monitor** | 📋 Planned | `kajo-monitor` (`org.kajo.Monitor`) |
| **Kajo Capture** | 📋 Planned | `kajo-capture` (`org.kajo.Capture`) |

### 3.2 [PLANNED] `kajo-files` Future Enhancements
- **Archive Compression (`libarchive`):**
  - Contextual `[ 📦 COMPRESS SELECTED ]` action in `kajo-files` for creating `.zip` and `.tar.zst` archives.
- **Drag-and-Drop (`GtkDragSource` / `GtkDropTarget`):**
  - Drag files between `kajo-files` windows or drop onto external Wayland applications.
- **Additional Quick Look Renderers:**
  - Office/EPUB document text/thumbnail preview, font specimen preview (`.ttf` / `.otf` waterfall rendering), and audio waveform/cover-art view for `.mp3` / `.flac`.

---

### 3.3 [PLANNED] Kajo Document & Image Viewer (`kajo-viewer`, `org.kajo.Viewer`)
- Dedicated full-window PDF, EPUB, and high-resolution image viewer sharing the Poppler + Cairo rendering pipeline from `kajo-files`.
- Features continuous/two-page PDF scrolling, document outline/bookmarks sidebar, full-text search, zoom presets, and image rotation/cropping.

---

### 3.4 [PLANNED] Kajo System Monitor (`kajo-monitor`, `org.kajo.Monitor`)
- Full-window companion to the panel `cpu-mem` telemetry widget:
  - Live per-core CPU frequency & utilization grid.
  - Memory, ZRAM/Swap, GPU, and NVMe I/O graphs.
  - Searchable process table with tree view, CPU/RSS sorting, and `[ SIGTERM ]` / `[ SIGKILL ]` / `nice` priority controls.

---

### 3.5 [PLANNED] Kajo Capture (`kajo-capture`, `org.kajo.Capture`)
- Native Wayland screenshot & screen-recording utility integrating with Niri and PipeWire:
  - Region, active window, or full output capture.
  - Quick Metro markup bar (crop, arrow, rectangle, redact/pixelate, copy to Wayland clipboard, save to `~/Pictures/Screenshots`).
