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
| **`kajo-settings` (`org.kajo.Settings`)** | ✅ Done | 12 modular settings pages (Displays, Network, Bluetooth, Audio, Theme & Custom `#RRGGBB` Hex Picker, Panel & Touch, Widgets, Notifications, Kajo IDE & Code, Kajo Mod & DSP, Niri Shaders, About) with responsive 3-tier Niri column reflow |
| **`kajo-greeter`** | ✅ Done | Native `greetd` Wayland login greeter with PAM authentication & session selector |
| **Foldable & 2-in-1 Posture Manager** | ✅ Done | Hinge angle, tablet mode, IIO accelerometer auto-rotation (`src/posture/posture_manager.c`) |
| **Niri GLSL Window Shaders** | ✅ Done | Live shader preset switcher for window open/close/resize animations (`src/settings/pages/page_niri.c`) |
| **Touchscreen Bezel Swipe Gestures** | ✅ Done | Edge layer-shell recognizers for Launcher, Overview, OSK, and Notifications (`src/hardware/touch_gestures.c`) |
| **kajo-comp: 3-Finger Drag & Flick** | ✅ Done | `kajo-comp` (`src/comp/comp_drag.c`, `src/comp/core/comp_gesture.c`): touchscreen and touchpad drags follow the fingers along the strip or between workspaces and snap on release by position or velocity (spec §3.15 GS1–GS6). Touch input verified by unit tests only, not yet on hardware. |
| **kajo-comp: Workspace Strip & Animated Resize** | ✅ Done | `kajo-comp` (`src/comp/core/comp_motion.c`): continuous vertical workspace strip with per-workspace cameras (WS7) and animated displayed size on resize (M5). |
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
| **Kajo IDE: user key remapping** | 📋 Planned | `kajo-ide` (FSM remap table exists in `ide_fsm.c`; config loading is not wired) |
| **`kajo-files` Phase 2** | ✅ Done | [`docs/spec_files_phase2.md`](docs/spec_files_phase2.md) — Virtual read-only archive folders (`archive://`), smart extraction, archive compression, UDisks2 ISO/disk image loop-mounting, global view/sort persistence, breadcrumb auto-scroll & sibling jump popover, and multi-selection |
| **Kajo Document & Image Viewer** | 📋 Planned | `kajo-viewer` (`org.kajo.Viewer`) |
| **Kajo System Monitor** | 📋 Planned | `kajo-monitor` (`org.kajo.Monitor`) |
| **Kajo Capture** | 📋 Planned | `kajo-capture` (`org.kajo.Capture`) |
| **kajo-comp: wlroots fork for visual effects** | 📋 Planned | `kajo-comp` — see §3.6 below (fork branch, built as a static submodule) |

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

---

### 3.6 [PLANNED] `kajo-comp` wlroots fork for visual effects

**Status:** 📋 Planned, not started. Nothing below is implemented; `kajo-comp` builds against the system `wlroots-0.20` (0.20.2).

**Why:** Effects such as backdrop blur, window open/close/resize transitions, dimming, shadows and, later, HDR tone mapping (BT.2390) and an adjustable SDR white level need render passes and scene features that stock `wlroots` does not offer (`wlr_render_pass` has only `add_texture` / `add_rect`, see spec §9). HDR requires the Vulkan renderer: in 0.20.2 only Vulkan sets `features.input_color_transform` / `output_color_transform`, so GLES-based solutions (SceneFX, the GLES2 step in spec §9.3) are excluded. Replacing `wlroots` entirely (~75k lines of backends, protocols and XWayland) is out of scope.

**Approach:** Maintain a fork of `wlroots` on GitHub and build `kajo-comp` from a branch of it.

- **Branch per upstream base:** `kajo/<upstream-tag>` (first: `kajo/0.20.2`), with the effect work as a short series of additive commits on top. Rebase onto new upstream releases (not master); cherry-pick only security / crash fixes in between.
- **Pinned commits are never rewritten:** never force-push a branch that Kajo pins; tag every pinned commit (e.g. `kajo-0.20.2-fx1`) so old Kajo revisions stay buildable.
- **Additive patches:** effects in new files (`render/vulkan/fx_*`, `types/scene/fx_*`, Vulkan shaders); upstream files only get small hooks. A `FORK.md` in the fork lists the base tag and the patch series.
- **Vulkan only**, effects applied in linear light before the output encode so they compose with the colour pipeline and later PQ output. No rounded corners (excluded by the 0 px design system).
- **Build:** git submodule at `subprojects/wlroots`, used via `subproject('wlroots')` (not `dependency('wlroots-0.20')`, so the system wlroots is never picked), linked statically (`default_library=static`), trimmed with the wlroots meson options. `PKGBUILD` gets the fork as a source plus wlroots' build dependencies (it currently does not declare wlroots at all); `DEVELOPMENT.md` documents `--recursive` clones.
- **Tests:** headless Vulkan pixel tests per effect hook, so a rebase that breaks a hook fails the gate. Every effect must be damage-gated (no work while nothing changes; idle CPU budget in `AGENTS.md`).

**Order:**

1. Fork, branch `kajo/0.20.2` with no changes, tag, add the submodule, switch `meson.build` to `subproject()`, full gate. (No behaviour change; proves the build plumbing.)
2. Effect hook + a trivial effect (tint) + headless Vulkan pixel test.
3. Backdrop blur (rewrite spec §9.3 around Vulkan; drop its GLES2 step).
4. Window transitions; HDR tone mapping and SDR white level when HDR work starts (spec §9.2).

**Open, not decided:** whether to contribute parts back upstream once they are stable.
