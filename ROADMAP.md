# Kajo Desktop Environment — Roadmap & Future Ideas (`ROADMAP.md`)

This document tracks completed milestones, active specifications, and future first-party applications and enhancements planned for the **Kajo** desktop suite on **`kajo-comp`** and the **Niri Wayland Compositor**.

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
| **`kajo` (Core Shell & Panel)** | ✅ Done | Panel, 22 widgets, 16-band FFT Spectrum Analyzer, MPRIS2, SNI Tray, Notification Daemon, `org.freedesktop.impl.portal.Settings` backend, App Launcher, Session Lock & Screensaver |
| **`kajo-settings` (`org.kajo.Settings`)** | ✅ Done | 13 modular settings pages (Displays, Network, Bluetooth, Audio, Theme & Surface/Glass Presets + opt-in revertible GTK/Qt external theming, Panel & Touch, Widgets, Notifications, Kajo IDE & Code, Kajo Mod & DSP, Kajo Web, Compositor & Effects, About) with responsive 3-tier column reflow |
| **`kajo-greeter`** | ✅ Done | Native `greetd` Wayland login greeter with PAM authentication & session selector |
| **Foldable & 2-in-1 Posture Manager** | ✅ Done | Hinge angle, tablet mode, IIO accelerometer auto-rotation (`src/posture/posture_manager.c`) |
| **Compositor Window Transitions & Shaders** | ✅ Done | Live preset switcher for window open/close/resize animations on both `kajo-comp` (Vulkan snapshot transitions) and Niri (GLSL shaders) via `src/settings/pages/page_niri.c` |
| **Touchscreen Bezel Swipe Gestures** | ✅ Done | Edge layer-shell recognizers for Launcher, Overview, OSK, and Notifications (`src/hardware/touch_gestures.c`) |
| **kajo-comp: 3-Finger Drag & Flick** | 🧪 Beta | `kajo-comp` (`src/comp/comp_drag.c`, `src/comp/core/comp_gesture.c`): touchscreen and touchpad drags follow the fingers along the strip or between workspaces and snap on release by position or velocity (spec §3.15 GS1–GS6). Touchpad verified on Host A hardware; the touchscreen drag works on hardware but has known bugs (§3.7, "Fix touchscreen gestures"). |
| **kajo-comp: Workspace Strip, Overview & `Mod + Drag`** | ✅ Done | `kajo-comp` (`src/comp/core/comp_motion.c`, `comp_overview_core.c`, `comp_overview.c`, `comp_window_chrome.c`): continuous vertical workspace strip with per-workspace cameras (WS7), animated displayed size on resize (M5), zoomed workspace overview (`Mod+O` / `toggle-overview`), and `Mod + Left/Right Button` tiled column reorder/resize (verified via unit/headless tests & headless renders; pending live desktop session verification). |
| **Virtual Keyboard (`ShellOSK`)** | ✅ Done | Windows Touch Keyboard flat 28-column proportional grid with XKB keymap & layout switching (`src/osk/osk.c`) |
| **Kajo Files (`kajo-files`, `org.kajo.Files`)** | 🚀 Beta | GVFS/UDisks2 file manager with virtual `archive://` browsing, ISO loop-mounting, live network discovery, Wayland Drag-and-Drop (`src/files/files_dnd.c`), native `org.freedesktop.impl.portal.FileChooser` backend (`--portal`), natural sorting, hit-tested context menu (`Open With / Set Default`, `libarchive` `Extract Here`), and `Space` Quick Look (`GtkPicture`, `libmpv` + `GtkGLArea`, Poppler PDFs, `libarchive` archives, code/text) |
| **Kajo Player (`kajo-player`, `org.kajo.Player`)** | 🚀 Beta | `libmpv` + `GtkGLArea` media player with subtitle/audio stream selectors, external `.srt`/`.ass` loading, sync delay, speed/frame-stepping, chapters, screenshots, fullscreen overlay HUD, and MPRIS2 D-Bus integration |
| **Kajo Mod (`kajo-mod`, `org.kajo.Mod`)** | 🚀 Beta | [`docs/spec_kajo_mod.md`](docs/spec_kajo_mod.md) — Retro Amiga (`libuade`), C64 SID (`libsidplayfp`/`reSIDfp` + HVSC), Chiptune (`libgme`), and Tracker (`libopenmpt` + `libxmp`) music player with subsongs, Paula A500/A1200 & LED filter, MOS 6581/8580 SID selector, live `$D400` register & sample/STIL inspector, `.lha`/`.zip` multi-file bundle loading, and 60 FPS oscilloscope + 16-band log FFT |
| **Kajo Web (`kajo-web`, `org.kajo.Web`)** | 🧪 Beta | [`docs/spec_kajo_web.md`](docs/spec_kajo_web.md) — Chromium/CEF Alloy Views Aero + Metro browser (`C++20` + native Wayland + VA-API) with `autohide`/`compact`/`pinned` vertical tab rail, isolated multi-profiles + Startup Profile Picker & ephemeral Private windows, IO-thread ABP + cosmetic CSS ad blocker (EasyList/EasyPrivacy auto-sync), idle tab suspension, always-visible Downloads popout + live `kajo://downloads` manager, `kajo://permissions`, SQLite WAL/FTS5 persistence, and `web.toml` + `kajo-settings` Page 12 (`--page web`) |
| **Kajo IDE (`kajo-ide`, `org.kajo.IDE`)** | 🧪 Beta | [`docs/kajo_ide_manual.md`](docs/kajo_ide_manual.md) — Selection-first modal GTK4 + `GtkSourceView-5` IDE with Nordic text objects (`m i/a`), 6-column Cairo gutter (absolute & hybrid relative line numbers), opt-in `60% | 25% | 15%` Call-Depth Code Strip & Dependency Graph Lenses (`Space t s`, [`docs/spec_ide_code_strip.md`](docs/spec_ide_code_strip.md)), tree-sitter highlighting and text objects (16 languages), real async LSP (`gopls`, `jdtls`, `clangd`, `rust-analyzer`), DAP debugger (expandable variables, watches, conditional breakpoints), project tasks, user key remapping (`ide.toml` `[keys.*]`), GUI Markdown + Cairo vector Mermaid, 120-command Help Palette, and AI Agent JSON-RPC 2.0 IPC |
| **Kajo "Aero + Metro" UI Design System** | ✅ Done | [`docs/design_system.md`](docs/design_system.md) & [`docs/ui-gallery.html`](docs/ui-gallery.html) — Canonical GTK4 CSS style guide and interactive web component gallery |

---

## 3. Future Roadmap (Not Yet Implemented)

| Planned Component / Feature | Status | Target Binary / Scope |
| :--- | :---: | :--- |
| **Kajo IDE: LSP references / signature help / call hierarchy** | ✅ Done | `kajo-ide` (`g r`/`Space g r`, `Ctrl+Shift+Space`/`Space g s`, `Space g c`/`Space g o` with `Tab` toggle, pixel-anchored popovers + mock LSP tests) |
| **Kajo IDE: DAP variable expansion / watches / conditional breakpoints** | ✅ Done | `kajo-ide` (expandable locals/scopes tree, live watch list with compound expansion, conditional breakpoints via `Shift+F9`/`Space d B`/`Shift+Click` gutter + mock DAP tests) |
| **Kajo IDE: relative line numbers** | ✅ Done | `kajo-ide` + `kajo-settings` (hybrid relative line numbers in 6-column Cairo gutter, `Space v r` toggle, persisted in `ide.toml`) |
| **Kajo IDE: user key remapping** | ✅ Done | `kajo-ide` + `kajo-settings` (`ide.toml` `[keys.<state>]` schema, hot-reloaded into `IdeFsm`, editable in `kajo-settings` → Kajo IDE & Code) |
| **Kajo IDE: Scrollable-Tiling Code Strip & Dependency Graph Lenses** | ✅ Done | `kajo-ide` — [`docs/spec_ide_code_strip.md`](docs/spec_ide_code_strip.md), [`docs/kajo-ide-strip-mockup.html`](docs/kajo-ide-strip-mockup.html), [`docs/kajo-ide-strip-storyboard.html`](docs/kajo-ide-strip-storyboard.html): opt-in `60% | 25% | 15%` Call-Depth Strip (`Space t s`) with `28px` Ancestor Spine, peekish magnetic follow (`comp_spring`), 4 workspace-only lenses (`CALLS`/`TYPES`/`IMPACT`/`DIFF`), vertical pin split, Zen `96/4`, Bird's-Eye Overview, and `kajo-settings` controls |
| **`kajo-files` Phase 2** | ✅ Done | [`docs/spec_files_phase2.md`](docs/spec_files_phase2.md) — Virtual read-only archive folders (`archive://`), smart extraction, archive compression, UDisks2 ISO/disk image loop-mounting, global view/sort persistence, breadcrumb auto-scroll & sibling jump popover, and multi-selection |
| **Kajo Document & Image Viewer** | 📋 Planned | `kajo-viewer` (`org.kajo.Viewer`) |
| **Kajo System Monitor** | 📋 Planned | `kajo-monitor` (`org.kajo.Monitor`) |
| **Kajo Capture** | 📋 Planned | `kajo-capture` (`org.kajo.Capture`) |
| **kajo-comp: wlroots fork for visual effects** | ✅ Done | `kajo-comp` — see §3.6 below (fork branch `kajo/0.20.2`, built as a static submodule) |
| **kajo-comp: HDR10 output & mixed SDR/HDR pipeline** | 📋 Planned | `kajo-comp` — §3.7.1 (blocked on `libdisplay-info >= 0.4.0` on Host A) |
| **kajo-comp: soft shadows, glassy backdrop blur, output stage (SDR white, tone mapping), transitions** | ✅ Done | `kajo-comp` + wlroots fork — [`docs/spec_comp_effects.md`](docs/spec_comp_effects.md) (M1–M5 complete) |
| **kajo-comp: overview zoom (`toggle-overview`), `Mod + drag` tiled columns** | ✅ Done | `kajo-comp` (`src/comp/comp_overview.c`, `src/comp/core/comp_overview_core.c`, `src/comp/comp_window_chrome.c`, `Mod+O` / IPC `toggle-overview`, `Mod + Left/Right Button` tiled column move/resize) |
| **kajo-comp: layout extras, multi-monitor, touchscreen gesture fixes** | 📋 Planned | `kajo-comp` — §3.7 |
| **Portal backends: Settings (theme/accent) and FileChooser** | ✅ Done | `kajo` shell (`src/portal/portal_settings.c`) + `kajo-files --portal` (`src/files/files_{portal,chooser}.c`, `kajo-files-portal.service`) — §3.8 |
| **Apply the Kajo theme to other apps (GTK, Qt)** | ✅ Done | `kajo-settings` (`src/theme_export.c` + `page_theme.c`, opt-in with exact snapshot backup & revert) — §3.9 |

### 3.1 [PLANNED] `kajo-files` Future Enhancements
- **Additional Quick Look Renderers:**
  - Office/EPUB document text/thumbnail preview, font specimen preview (`.ttf` / `.otf` waterfall rendering), and audio waveform/cover-art view for `.mp3` / `.flac`.

*(Completed in `kajo-files`: Wayland Drag-and-Drop (`src/files/files_dnd.{h,c}`) supporting outbound `GDK_TYPE_FILE_LIST` drags and inbound drops onto directory views, subfolders, breadcrumb segments, Places/Drives, and Trash.)*

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

### 3.6 [DONE] `kajo-comp` wlroots fork for visual effects

**Status:** ✅ Implemented (Milestones 1–5); specifications in [`docs/spec_wlroots_fork.md`](docs/spec_wlroots_fork.md) and [`docs/spec_comp_effects.md`](docs/spec_comp_effects.md). `kajo-comp` builds against `github.com/mikolas/wlroots` at `kajo-0.20.2-fx9` (`3752351e`) as a static subproject with custom Vulkan fragment shaders (`X1`), below-node backdrop render (`X2`), pooled offscreen targets and single-CB target chains (`X3`), scene leaf hooks (`X4`), damage helpers (`X5`), output stage (`X6`: lazy 16×16 blue-noise dither, adjustable SDR reference white, ITU-R BT.2390 PQ tone mapping, HDR headroom query, HDR matrix folding), analytic soft drop shadows, unfocused window dimming, dual-filter Kawase frosted backdrop glass (`xray` background cache + popup propagation), per-pixel linear colour filters, window open/close/resize snapshot transitions (`FX_IN_TEXTURE`, H5), live CSS surface/glass token configurability (`theme.toml` → `00-accent.css`), and compositor-agnostic settings in `kajo-settings`.

**Why:** Effects such as backdrop blur, window open/close/resize transitions, dimming, shadows and, later, HDR tone mapping (BT.2390) and an adjustable SDR white level need render passes and scene features that stock `wlroots` does not offer (`wlr_render_pass` has only `add_texture` / `add_rect`). HDR requires the Vulkan renderer: in 0.20.2 only Vulkan sets `features.input_color_transform` / `output_color_transform`, so GLES-based solutions (SceneFX, a GLES2 renderer swap) are excluded. Replacing `wlroots` entirely (~75k lines of backends, protocols and XWayland) is out of scope.

**Approach:** Maintain a fork of `wlroots` on GitHub (`github.com/mikolas/wlroots`, filled from upstream at `gitlab.freedesktop.org/wlroots/wlroots`; upstream moved off GitHub, so this is a plain repository, not a GitHub fork) and build `kajo-comp` from a branch of it.

- **Branch per upstream base:** `kajo/<upstream-tag>` (first: `kajo/0.20.2`), with the effect work as a short series of additive commits on top. Rebase onto new upstream releases (not master); cherry-pick only security / crash fixes in between.
- **Pinned commits are never rewritten:** never force-push a branch that Kajo pins; tag every pinned commit (e.g. `kajo-0.20.2-fx1`) so old Kajo revisions stay buildable.
- **Additive patches:** effects in new files (`render/vulkan/kfx_*`, `types/scene/kfx_*`, `include/wlr/kfx/`, Vulkan shaders); upstream files only get small hooks. A `FORK.md` in the fork lists the base tag and the patch series.
- **Vulkan only**, effects applied in linear light before the output encode so they compose with the colour pipeline and later PQ output. No rounded corners (excluded by the 0 px design system).
- **Drop-in compatible:** the fork is upstream plus additive extensions (`wlr_kfx_*`, detectable via `WLR_HAS_KFX`); no upstream functionality is removed and API/ABI stay compatible except for documented exceptions (spec §3).
- **Build:** git submodule at `subprojects/wlroots` (HTTPS URL), used via `subproject('wlroots')` (not `dependency('wlroots-0.20')`, so the system wlroots is never picked), linked statically (`default_library=static`), with upstream's default options. `PKGBUILD` has the fork as a source plus wlroots' build and runtime dependencies; `DEVELOPMENT.md` and `README.md` document `--recursive` clones and `--skip-subprojects` installs.
- **Tests:** headless Vulkan pixel tests per effect hook, so a rebase that breaks a hook fails the gate. Every effect must be damage-gated (no work while nothing changes; idle CPU budget in `AGENTS.md`).

**Order:**

1. ✅ Milestone 1: fork, branch `kajo/0.20.2`, submodule, `subproject()`, full gate (no behaviour change).
2. ✅ Milestone 2 (`kajo-0.20.2-fx3`): custom fragment shaders (`X1`), scene leaf hooks (`X4`), two-pass pathway (`X6`), analytic soft shadows and unfocused dimming (`shadow.frag`, `dim.frag`).
3. ✅ Milestone 3 (`kajo-0.20.2-fx8`): below-node scene render (`X2`), pooled offscreen targets and single-CB target chains (`X3`), damage helpers (`X5`), lazy 16×16 blue-noise output dither (`X6`), dual-filter Kawase frosted backdrop glass (`blur_down.frag`, `blur_up.frag`, `glass.frag`).
4. ✅ Milestone 4 (`kajo-0.20.2-fx9`): output stage (`X6` adjustable SDR reference white, ITU-R BT.2390 PQ tone mapping, HDR headroom query, HDR matrix folding) and per-pixel linear colour filters (`color_filter`, `color_filter_strength`, `night_light_temperature`) with `kajo-settings` controls.
5. ✅ Milestone 5 per [`docs/spec_comp_effects.md`](docs/spec_comp_effects.md) §12.1: window open / close / resize snapshot transitions (`FX_IN_TEXTURE`, H5, `transition_single.frag` & `transition_resize.frag` porting all `data/shaders/` presets); live CSS surface/glass token configurability (`theme.toml` → `00-accent.css`: `glass_tint`, `panel_opacity`, `glass_opacity`, `glass_control_contrast`, `widget_chip_opacity`, `accent_glass_tint`, `surface_base`, `surface_contrast`, `border_opacity`, `ui_font_family`, `secondary_fg_contrast`, 4 surface presets); remaining `comp.toml` glass/shadow/dim/border controls in `kajo-settings`; and compositor-agnostic naming in `kajo-settings` (serving both `kajo-comp` and `niri` where applicable).

**Open, not decided:** whether to contribute parts back upstream once they are stable.

---

### 3.7 [PLANNED] `kajo-comp` future work (moved from `docs/spec_kajo_comp.md` §9)

The compositor spec describes what is built; everything below is not implemented.

| Item | Why not built yet / blocker | Starting approach |
| :--- | :--- | :--- |
| **HDR10 output & mixed SDR/HDR pipeline** | v1 ships a solid 8-bit SDR compositor first. On Host A, `libdisplay-info` 0.3.0 misses the DisplayID 2.0 CTA HDR blocks (fixed in 0.4.0). Fork output-stage SDR white & BT.2390 tone mapping are already in place (M4). | §3.7.1. Vulkan only (GLES2 and pixman have no input/output colour transforms in wlroots 0.20.2). |
| **OSK: keep the focused text field visible** | Push-up is built: with `[osk] push_windows = true` the shell sets a layer-shell exclusive zone (`src/osk/osk.c`) and `kajo-comp` shrinks the working area and relays out (`comp_layer_arrange`). Checked by eye on Host A with kitty and Thorium (native Wayland). Missing: scrolling the focused column / window so the text cursor stays above the keyboard when the field is hidden even after the shrink. | Use the `text-input-v3` cursor rectangle to offset the camera. |
| **Layout extras** | Not needed for v1. | Evaluate individually: `focus-follows-mouse`, `switch-preset-column-width-back`, a `maximize-window-to-edges` keybind, named workspaces, `empty-workspace-above-first`, `default-column-display`, custom struts. |
| **Multi-monitor sessions** | v1 policy drives one active output (spec §3.13 `OUT1–OUT4`; greeter mode is the exception). | Workspaces per output, layer surfaces and IPC per output. Expect a few thousand lines. |
| **Fix touchscreen gestures** | The 3-finger touchscreen drag (spec §3.15 GS1–GS6, `touch_drag_*` in `src/comp/comp_input.c`) works on Host A hardware but is buggy. Low priority; the touchpad path is verified. | Reproduce on hardware, write down each bug with steps, then cover it with a `test_comp_motion` / `test_comp_config` case before fixing. |

#### 3.7.1 HDR10 design notes & Phase 0 hardware findings

Verified against `wlroots 0.20.2` (`backend/drm`, `types/scene/wlr_scene.c`, `render/color.c`, `render/vulkan`) and measured on Host A (HP OmniBook X Flip 14 OLED, `Samsung Display Corp. 0x41AA`, Intel Lunar Lake Xe2, Mesa 26.2.6) during the Phase 0 feasibility spike:

1. **DRM capability detection & `libdisplay-info` 0.4.0:**
   - The DRM backend sets `BT2020` in `wlr_output.supported_primaries` when the connector exposes `Colorspace` and the EDID signals BT.2020, and `ST2084_PQ` in `supported_transfer_functions` when `HDR_OUTPUT_METADATA` and EDID PQ static metadata are present.
   - Host A's `eDP-1` stores its HDR static metadata (`616.9 cd/m²` peak, `400 cd/m²` sustained, `0.0004 cd/m²` black) in CTA blocks nested in a **DisplayID 2.0 extension block**. `libdisplay-info` 0.3.0 does not scan those; `>= 0.4.0` (upstream commit `73ec53d`) does.
2. **Commit through `wlr_scene_output_build_state()`:**
   - When enabling HDR10 (`DRM_FORMAT_XRGB2101010` + `WLR_COLOR_NAMED_PRIMARIES_BT2020` + `WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ`), the output image description **must** go through `wlr_scene_output_build_state()` so `wlr_scene` sets up its Vulkan output colour transform (SDR sRGB surfaces encoded at `203 cd/m²` PQ reference white instead of raw gamma 2.2). `kajo-comp` already commits through `wlr_scene_output_build_state()` when tearing is allowed (`src/comp/comp_output.c`).
   - Never pass primaries other than `NONE` or `BT2020` in an output image description (`convert_primaries_to_colorspace()` in the DRM backend calls `abort()` otherwise).
3. **Headless Vulkan:** `WLR_BACKENDS=headless WLR_RENDERER=vulkan` works on Host A with `input_color_transform`, `output_color_transform` and timeline syncobjs, and the 6-patch SDR self-test (`#000000`, `#111111`, `#1a1a1a`, `#0078d4`, `#808080`, `#ffffff`) reads back bit-exact 8-bit values headless and on bare-metal `2880×1800`.
4. **Renderer constraint:** only the Vulkan renderer sets `features.input_color_transform` / `output_color_transform` in 0.20.2; with GLES2 or pixman the colour manager advertises no PQ / BT.2020. HDR therefore rules out any GLES-based effect path.

#### 3.7.2 Backdrop blur design notes

Superseded by [`docs/spec_comp_effects.md`](docs/spec_comp_effects.md) §10 (glassy backdrop blur) and the fork extension points X1–X6 there. The GLES exclusion is covered there (§1, §5); the earlier fallback of emulating blur with repeated stock texture draws is dropped.

---

### 3.8 [DONE] Portal backends (`org.freedesktop.impl.portal.*`): Settings and FileChooser

Implemented from the public D-Bus portal interface specifications with GTK kept as the fallback backend in `data/session/kajo-portals.conf` (`org.freedesktop.impl.portal.Settings=kajo;gtk;`, `org.freedesktop.impl.portal.FileChooser=kajo-files;gtk;`). ScreenCast and Screenshot remain on `xdg-desktop-portal-wlr`; Secret remains on `gnome-keyring`.

#### 3.8.1 Settings backend (`org.freedesktop.impl.portal.Settings`)
- **Implemented** in `src/portal/portal_settings.{h,c}`, hosted directly by the always-running `kajo` shell (`org.freedesktop.impl.portal.desktop.kajo`, `data/session/kajo.portal`).
- Serves `org.freedesktop.appearance` (`color-scheme = 1`, `accent-color = (ddd)` from `theme.toml`, `contrast`, `reduced-motion`), supports `ReadAll` namespace globbing, returns `org.freedesktop.portal.Error.NotFound` on missing keys, and emits `SettingChanged` whenever `kajo-settings` updates `theme.toml`.
- **Verified:** mock D-Bus unit tests in `tests/test_portals.c` (including ASan/UBSan) and live session verification on Host A.

#### 3.8.2 FileChooser (`kajo-files --portal`)
- **Implemented** in `src/files/files_portal.{h,c}` + `src/files/files_chooser.{h,c}`, D-Bus activated on demand via `data/session/kajo-files.portal` (`org.freedesktop.impl.portal.desktop.kajo.Files`), `data/session/org.freedesktop.impl.portal.desktop.kajo.Files.service.in` (`SystemdService=kajo-files-portal.service`), and `data/systemd/kajo-files-portal.service.in` (`Type=dbus`, `PartOf=graphical-session.target`).
- Handles `OpenFile`, `SaveFile`, `SaveFiles`, and per-request `org.freedesktop.impl.portal.Request.Close()` cancellation, including `multiple`, `directory`, `filters` (glob + MIME), `current_filter`, `current_folder`, `current_name`, `current_file`, `accept_label`, `choices` (checkboxes & dropdowns), `modal`, and `file://` URI normalization.
- Undecorated `880×580` Aero+Metro `KajoFilesChooser` window (`.shell-files-chooser`, floating rule in `data/defaults/rules.toml`) with `KajoFilesSidebar`, Browse Network, inline auth/connect, inline overwrite confirm, `+ 📁` new folder, `Space` Quick Look, and Wayland parent attachment (`gdk_wayland_toplevel_set_transient_for_exported()` + `gtk_window_set_modal()`).
- **Verified:** mock D-Bus protocol/cancellation/filter tests in `tests/test_portals.c` (including ASan/UBSan), portal/service/unit cross-check in `tests/test_comp_config.c`, headless UI render (`kajo-files --chooser-preview`), and live browser file-chooser activation on Host A. When installing into an already-running session, run `systemctl --user daemon-reload && systemctl --user reload dbus-broker && systemctl --user restart xdg-desktop-portal` so `dbus-broker` picks up the new service file without a re-login.

---

### 3.9 [DONE] Apply the Kajo theme to other apps (GTK, Qt)

**Implemented** in `src/theme_export.{h,c}` and `kajo-settings → Theme` (Card 5: `5. EXTERNAL APP THEMING`), backed by opt-in `theme_external_gtk` and `theme_external_qt` keys in `theme.toml` (both `false` by default).

- **Exact pre-Kajo snapshot & revert:** On first opt-in, snapshots existing user files (`gtk-4.0/{gtk.css,settings.ini}`, `gtk-3.0/{gtk.css,settings.ini}`, `qt6ct/qt6ct.conf`) and `org.gnome.desktop.interface` GSettings (`color-scheme`, `accent-color`, `gtk-theme`) into `~/.config/kajo/external-theme-backup.json` (never overwritten while active). Disabling a layer or clicking **`↺ RESTORE ORIGINAL DEFAULTS`** restores every file and GSettings key to its exact original state and removes all Kajo-managed files.
- **GTK3 & GTK4/libadwaita:** Writes `~/.config/gtk-{3.0,4.0}/kajo-theme.css` from Kajo's active accent and dark surface tokens (`0px` geometry, `:not(.osk-key)`), appends a clearly marked `@import url("kajo-theme.css");` block to `gtk.css` without clobbering existing user rules, warns on unused leftovers (`dank-colors.css`, `colors.css`, `matugen.css`), and updates `settings.ini` + `org.gnome.desktop.interface`.
- **Qt 6 (`qt6ct`):** Generates a 21-color `~/.config/qt6ct/colors/kajo-metro.conf`, configures `qt6ct.conf`, writes `~/.config/environment.d/90-kajo-qt.conf` (`QT_QPA_PLATFORMTHEME=qt6ct`), updates `systemctl --user` environment, and hooks `data/session/kajo-session` on login.
- **Verified:** isolated temp-directory backup, apply, accent-update, partial-disable, and bit-exact full-revert tests in `tests/test_portals.c` (including ASan/UBSan), headless `kajo-settings --page theme` render, and live session verification on Host A.
