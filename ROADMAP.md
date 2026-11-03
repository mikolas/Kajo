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
| **Kajo Files (`kajo-files`, `org.kajo.Files`)** | ✅ Done | [`SPEC_FILE_BROWSER.md`](file:///home/mikolas/src/Kajo/SPEC_FILE_BROWSER.md) — GVFS/UDisks2 file manager with macOS-style `Space` Quick Look (images, audio/video, Poppler PDFs, `libarchive` archives, code/text) |

---

## 3. Future Roadmap (Not Yet Implemented)

| Planned Component / Feature | Status | Target Binary / Scope |
| :--- | :---: | :--- |
| **Kajo Media Player** | 📋 Planned | `kajo-player` (`org.kajo.Player`) |
| **`kajo-files` Phase 2** | 📋 Planned | `kajo-files` (Archive extraction, context menu, DnD) |
| **Kajo Document & Image Viewer** | 📋 Planned | `kajo-viewer` (`org.kajo.Viewer`) |
| **Kajo System Monitor** | 📋 Planned | `kajo-monitor` (`org.kajo.Monitor`) |
| **Kajo Capture** | 📋 Planned | `kajo-capture` (`org.kajo.Capture`) |

### 3.1 [PLANNED] Kajo Media Player (`kajo-player`, `org.kajo.Player`)
> **Status:** 📋 **Not Yet Implemented** — Future standalone application inspired by the `kajo-files` Quick Look video preview aesthetic.

- **Visual Design:**
  - Frameless `window.shell-settings-window.shell-player-window` surface with `1px solid @metro-accent` border, top `🎬 KAJO PLAYER` header bar, technical codec/resolution badge (`[ HEVC | 3840×2160 @ 60fps | 7.1 FLAC ]`), `🗗 FLOAT / TILE` button, and a 1-click **Picture-in-Picture (PiP)** floating pin button.
- **Playback Engine:**
  - GStreamer (`GstPlay` / `gtk4paintablesink` or `GtkMediaFile`) / `libmpv` hardware-accelerated Wayland video output supporting `.mkv`, `.mp4`, `.webm`, `.avi`, `.mov`, `.mp3`, `.flac`, `.ogg`, `.opus`, `.wav`.
- **Transport & Controls:**
  - Auto-hiding bottom Metro transport bar with seek timeline, elapsed/remaining timecode (`00:14:22 / 01:48:05`), play/pause, frame-step (`[` / `]`), playback speed selector (`0.5×` – `2.0×`), volume slider, and fullscreen toggle (`F` / `F11` / Double-Click).
  - Inline track selectors for **Embedded Subtitles** (`.srt` / `.ass` / PGS + external subtitle loading) and **Multi-Language Audio Streams**.
- **Playlist / Queue Drawer:**
  - Compact toggleable right-hand drawer (`240px`, hidden by default) listing current folder media or queued tracks.
- **Native Shell Integration:**
  - Exports an **MPRIS2 D-Bus interface (`org.mpris.MediaPlayer2.kajo_player`)** so active playback automatically populates Kajo's top panel Media widget, lock-screen media controls, and 16-band FFT spectrum analyzer.

---

### 3.2 [PLANNED] `kajo-files` Phase 2 Enhancements
- **1-Click Archive Extraction & Compression (`libarchive`):**
  - Contextual `[ 📦 EXTRACT HERE ]` and `[ 📦 COMPRESS SELECTED ]` actions in `kajo-files` for `.zip`, `.tar.gz`, `.tar.xz`, `.tar.zst`, and `.7z`.
- **Right-Click Context Popover Menu:**
  - Kajo-styled context menu on tiles/rows (`Open`, `Open With...`, `Quick Look`, `Copy`, `Cut`, `Rename`, `Extract`, `Move to Trash`, `Properties`).
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
