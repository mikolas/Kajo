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
| **`kajo-settings` (`org.kajo.Settings`)** | ✅ Done | 11 modular settings pages (Displays, Network, Bluetooth, Audio, Theme, Panel & Touch, Widgets, Notifications, Kajo IDE & Code, Niri Shaders, About) |
| **`kajo-greeter`** | ✅ Done | Native `greetd` Wayland login greeter with PAM authentication & session selector |
| **Foldable & 2-in-1 Posture Manager** | ✅ Done | Hinge angle, tablet mode, IIO accelerometer auto-rotation (`src/posture/posture_manager.c`) |
| **Niri GLSL Window Shaders** | ✅ Done | Live shader preset switcher for window open/close/resize animations (`src/settings/pages/page_niri.c`) |
| **Touchscreen Bezel Swipe Gestures** | ✅ Done | Edge layer-shell recognizers for Launcher, Overview, OSK, and Notifications (`src/hardware/touch_gestures.c`) |
| **Virtual Keyboard (`ShellOSK`)** | ✅ Done | Windows Touch Keyboard flat 28-column proportional grid with XKB keymap & layout switching (`src/osk/osk.c`) |
| **Kajo Files (`kajo-files`, `org.kajo.Files`)** | 🧪 Alpha | GVFS/UDisks2 file manager with natural sorting, hit-tested context menu (`Open With / Set Default`, `libarchive` `Extract Here`), and `Space` Quick Look (`GtkPicture`, `libmpv` + `GtkGLArea`, Poppler PDFs, `libarchive` archives, code/text) |
| **Kajo Player (`kajo-player`, `org.kajo.Player`)** | 🚀 Beta | `libmpv` + `GtkGLArea` media player with subtitle/audio stream selectors, external `.srt`/`.ass` loading, sync delay, speed/frame-stepping, chapters, screenshots, fullscreen overlay HUD, and MPRIS2 D-Bus integration |
| **Kajo Mod (`kajo-mod`, `org.kajo.Mod`)** | 🚀 Beta | [`docs/spec_kajo_mod.md`](docs/spec_kajo_mod.md) — Retro Amiga (`libuade`), C64 SID (`libsidplayfp`/`reSIDfp` + HVSC), Chiptune (`libgme`), and Tracker (`libopenmpt` + `libxmp`) music player with subsongs, Paula A500/A1200 & LED filter, MOS 6581/8580 SID selector, live `$D400` register & sample/STIL inspector, `.lha`/`.zip` multi-file bundle loading, and 60 FPS oscilloscope + 16-band log FFT |
| **Kajo IDE (`kajo-ide`, `org.kajo.IDE`)** | 🧪 Beta | [`docs/kajo_ide_manual.md`](docs/kajo_ide_manual.md) — Selection-first modal GTK4 + `GtkSourceView-5` IDE with Nordic text objects (`m i/a`), 6-column Cairo gutter, tree-sitter highlighting and text objects (15 languages), a real async LSP client (`gopls`, `jdtls`, `clangd`, `rust-analyzer`), DAP debugger (gdb / lldb-dap / dlv / Java via jdtls), project tasks, GUI Markdown + Cairo vector Mermaid, 101-command Help Palette, and AI Agent JSON-RPC 2.0 IPC |
| **Kajo "Aero + Metro" UI Design System** | ✅ Done | [`docs/design_system.md`](docs/design_system.md) & [`docs/ui-gallery.html`](docs/ui-gallery.html) — Canonical GTK4 CSS style guide and interactive web component gallery |

---

## 3. Future Roadmap (Not Yet Implemented)

| Planned Component / Feature | Status | Target Binary / Scope |
| :--- | :---: | :--- |
| **Kajo IDE: LSP references / signature help / call hierarchy** | 📋 Planned | `kajo-ide` (the client is in place; these need list/popover UIs) |
| **Kajo IDE: DAP variable expansion / watches / conditional breakpoints** | 📋 Planned | `kajo-ide` |
| **Kajo IDE: relative line numbers** | 📋 Planned | `kajo-ide` (gutter option; documented before it was built) |
| **kajo-settings: custom accent hex picker** | 📋 Planned | `kajo-settings` Theme page (the UI gallery shows a hex picker; only the 8 presets exist today) |
| **Kajo IDE: user key remapping** | 📋 Planned | `kajo-ide` (FSM remap table exists in `ide_fsm.c`; config loading is not wired) |
| **`kajo-files` Phase 2** | ✅ Done | [`docs/spec_files_phase2.md`](docs/spec_files_phase2.md) — Virtual read-only archive folders (`archive://`), smart extraction, archive compression, UDisks2 ISO/disk image loop-mounting, global view/sort persistence, breadcrumb auto-scroll & sibling jump popover, and multi-selection |
| **Kajo Document & Image Viewer** | 📋 Planned | `kajo-viewer` (`org.kajo.Viewer`) |
| **Kajo System Monitor** | 📋 Planned | `kajo-monitor` (`org.kajo.Monitor`) |
| **Kajo Capture** | 📋 Planned | `kajo-capture` (`org.kajo.Capture`) |

### 3.1 [PLANNED] `kajo-files` Future Enhancements
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
