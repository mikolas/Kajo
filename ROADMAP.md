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
| **kajo-comp: 3-Finger Drag & Flick** | 🧪 Beta | `kajo-comp` (`src/comp/comp_drag.c`, `src/comp/core/comp_gesture.c`): touchscreen and touchpad drags follow the fingers along the strip or between workspaces and snap on release by position or velocity (spec §3.15 GS1–GS6). Touchpad verified on Host A hardware; the touchscreen drag works on hardware but has known bugs (§3.7, "Fix touchscreen gestures"). |
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
| **kajo-comp: HDR10 output, SDR white level, tone mapping** | 📋 Planned | `kajo-comp` — §3.7.1 |
| **kajo-comp: backdrop blur, window transitions, shadows** | 📋 Planned | `kajo-comp` — needs the wlroots fork (§3.6), design in §3.7.2 |
| **kajo-comp: overview zoom, `Mod + drag` tiled columns, layout extras, multi-monitor, touchscreen gesture fixes** | 📋 Planned | `kajo-comp` — §3.7 |
| **Portal backends: Settings (theme/accent) and FileChooser** | 📋 Planned | `kajo` shell + `kajo-files --portal` — §3.8 |
| **Apply the Kajo theme to other apps (GTK, Qt)** | 📋 Planned | `kajo-settings` — §3.9 |

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

**Why:** Effects such as backdrop blur, window open/close/resize transitions, dimming, shadows and, later, HDR tone mapping (BT.2390) and an adjustable SDR white level need render passes and scene features that stock `wlroots` does not offer (`wlr_render_pass` has only `add_texture` / `add_rect`). HDR requires the Vulkan renderer: in 0.20.2 only Vulkan sets `features.input_color_transform` / `output_color_transform`, so GLES-based solutions (SceneFX, a GLES2 renderer swap) are excluded. Replacing `wlroots` entirely (~75k lines of backends, protocols and XWayland) is out of scope.

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
3. Backdrop blur (design in §3.7.2).
4. Window transitions; HDR tone mapping and SDR white level when HDR work starts (§3.7.1).

**Open, not decided:** whether to contribute parts back upstream once they are stable.

---

### 3.7 [PLANNED] `kajo-comp` future work (moved from `docs/spec_kajo_comp.md` §9)

The compositor spec describes what is built; everything below is not implemented.

| Item | Why not built yet / blocker | Starting approach |
| :--- | :--- | :--- |
| **HDR10 output & mixed SDR/HDR pipeline** | v1 ships a solid 8-bit SDR compositor first. On Host A, `libdisplay-info` 0.3.0 misses the DisplayID 2.0 CTA HDR blocks (fixed in 0.4.0). | §3.7.1. Vulkan only (GLES2 and pixman have no input/output colour transforms in wlroots 0.20.2). |
| **Adjustable SDR white level (`sdr_brightness_nits`)** | Stock `wlr_scene` fixes SDR reference white at `203 cd/m²` (ITU-R BT.2408) on a PQ output and rejects `color_transform` when an output image description is set. | Patch `wlr_scene`'s luminance multiplier in the wlroots fork (§3.6), or propose upstream. |
| **Compositor-side HDR tone mapping (BT.2390)** | wlroots 0.20.2 has no tone-mapping curve; content above panel peak relies on the display's own tone mapping. | Tone-mapping pass in the wlroots fork (§3.6). |
| **Backdrop blur (dual-Kawase frosted glass)** | `wlr_render_pass` has only `add_texture()` / `add_rect()`, no multi-pass API. `[[layer_rule]]` keys `blur`, `xray`, `blur_popups` are currently rejected with a non-fatal diagnostic. | §3.7.2, on the wlroots fork (§3.6). |
| **Window open/close/resize shaders** | Same render-pass limitation; close animations need frozen textures of unmapped surfaces, resize crossfades need dual-texture blending. (Resize already animates the box size, M5.) | Effect passes in the wlroots fork (§3.6). |
| **Overview zoom (`toggle-overview`)** | `wlr_scene` has no tree scale transform; zooming means recomputing positions and `wlr_scene_buffer_set_dest_size()` for every surface, subsurface and popup. | Manual scene-buffer scaling pass, or a scale-aware scene node in the fork. |
| **Drop shadows** | Not in `wlr_scene`. Rounded corners stay excluded (0 px design system). | Shadow rects / nine-patch (cheap with 0 px radius). |
| **OSK: keep the focused text field visible** | Push-up is built: with `[osk] push_windows = true` the shell sets a layer-shell exclusive zone (`src/osk/osk.c`) and `kajo-comp` shrinks the working area and relays out (`comp_layer_arrange`). Checked by eye on Host A with kitty and Thorium (native Wayland). Missing: scrolling the focused column / window so the text cursor stays above the keyboard when the field is hidden even after the shrink. | Use the `text-input-v3` cursor rectangle to offset the camera. |
| **`Mod + drag` interactive move/resize of tiled columns** | Keyboard-first workflow and presets cover v1; client-initiated move/resize exists for floating windows (`L8`). | `Mod + Left/Right Button` tiled grab. |
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

Goal: frosted-glass blur behind `kajo` layer surfaces and popups while keeping `wlr_scene_output_commit()` / `build_state()` (KMS test-before-commit cascade, damage tracking, direct scanout when nothing is blurred, screencopy). Requires the wlroots fork (§3.6) for Vulkan shader passes; GLES2 is excluded (§3.7.1).

1. **Config & layer rules** (`src/comp/core/comp_schema_keys.h`, `comp_rules.{h,c}`, `comp_config.{h,c}`):
   - `[blur]` section in `comp.toml` (`enabled`, `passes = 2`, `radius = 5.0`, `noise = 0.02`).
   - Move `blur` out of `k_layer_unsupported` and add `popup_blur` and `xray` to `[[layer_rule]]`; update `test_rules_unsupported_keys_flagged` and add positive tests.
2. **Backdrop nodes** (`src/comp/comp_layer.c`): a `wlr_scene_buffer` as the lowest child of each blurred layer surface / popup tree, sized to the xdg geometry (not the buffer, so GTK popover margins do not show a halo), with `point_accepts_input` returning false.
3. **Shell CSS:** keep layer `opacity = 1.0` so text and 1 px accent borders stay crisp; lower `@metro-panel-bg` / `popover > contents` alpha (`0.72–0.80`) so the backdrop shows. Document in `docs/design_system.md` and the gallery.
4. **Pipeline** (fork: Vulkan shader passes): capture the trees below the surface into a ¼-resolution offscreen buffer with a margin of the blur radius, 2 down + 2 up dual-Kawase passes, noise on the last pass against banding on dark glass. `xray = true` samples only `tree_background` + `tree_bottom` (wallpaper, nearly static; recommended default for the panel); `xray = false` also samples tiled and floating windows.
5. **Damage gate** (`handle_frame` in `src/comp/comp_output.c`): before the output commit, recompute only if `scene_output->damage_ring.current` intersects a blur rect grown by the radius, or the rect moved. The buffer swap's own damage is consumed by the same commit. Known cost: with `xray = false`, the panel's own repaints (spectrum visualizer) are indistinguishable from damage underneath. Recompute in z-order (a popup's backdrop includes the blurred panel). Drop all buffers on GPU recovery, unmap and output size/scale changes; disable on pixman.
6. **Fallback without the fork** (lower quality, not preferred): the same blur built from stock `wlr_render_pass` draws: repeated `add_texture` with sub-pixel `src_box` offsets, bilinear filtering and alpha weights `w_k / Σ_{j≤k} w_j`, which reproduces any tap kernel. Watch 8-bit banding between passes.

---

### 3.8 [PLANNED] Portal backends (`org.freedesktop.impl.portal.*`): Settings and FileChooser

A portal backend is a D-Bus service that answers requests which `xdg-desktop-portal` forwards from apps (sandboxed apps always use it; others opt in). Kajo has no backend of its own yet.

**State on Host A (checked, not assumed):**
- `kajo-comp` exports `XDG_CURRENT_DESKTOP=kajo`; `kajo-portals.conf` is installed under `/usr/local/share/xdg-desktop-portal/`; no `kajo.portal` exists.
- Only `xdg-desktop-portal-gtk` is running. The frontend offers FileChooser v4, ScreenCast v4 and Screenshot v2.
- `org.freedesktop.appearance` `color-scheme` reads `2` (prefer light), `accent-color` is "not found", and GSettings `org.gnome.desktop.interface` has `color-scheme = 'prefer-light'`. Apps that follow the portal therefore get a light Adwaita look next to Kajo's dark one.
- `data/session/kajo-portals.conf` says `default=gnome;gtk;`. Which backend currently serves FileChooser was not checked (calling it opens a real dialog).

**Scope:** Settings and FileChooser. ScreenCast and Screenshot stay on `xdg-desktop-portal-wlr` (the compositor already offers screencopy and ext-image-copy-capture; an actual capture through the portal has not been tested). GlobalShortcuts, RemoteDesktop, InputCapture and a native ScreenCast are not planned.

**Rules for both backends:** write them from the public portal interface descriptions, not from the GTK or GNOME backends' source (LGPL). Keep the GTK backend as the fallback in `[preferred]` (`…=kajo;gtk;`), because a bug in a backend breaks every app that uses it. Test the D-Bus logic with a mock client in the style of `tests/test_niri.c`; the dialogs themselves need a manual check.

#### 3.8.1 Settings backend (`org.freedesktop.impl.portal.Settings`)

- Answer `Read` / `ReadAll` for `org.freedesktop.appearance`: `color-scheme` (`1` = prefer dark), `accent-color` (RGB doubles from `theme.toml` `accent_color`), and `contrast`; emit `SettingChanged` when `kajo-settings` applies a new theme.
- Host: the always-running `kajo` shell (it already owns the notification name) avoids a cold start; to be decided.
- Register with a `kajo.portal` (`UseIn=kajo`, `Interfaces=org.freedesktop.impl.portal.Settings;`) and a `Settings=kajo;gtk;` line in `kajo-portals.conf`.
- Quick path to evaluate first: writing `org.gnome.desktop.interface color-scheme='prefer-dark'` on SAVE & APPLY. The portal value matched GSettings in the one comparison made, but propagation was not tested, the GTK backend here returns no accent, and it changes a setting GNOME tools also use.

#### 3.8.2 FileChooser (`kajo-files --portal`)

Goal: a native chooser for Kajo apps (`GtkFileDialog` in `kajo-mod`, `kajo-player`, `kajo-ide`) and third-party Wayland apps (browsers, Electron, Flatpak) instead of the GTK dialog.

1. **D-Bus backend in `kajo-files`** (`src/files/files_portal.{h,c}`):
   - Own `org.freedesktop.impl.portal.desktop.kajo`, export `org.freedesktop.impl.portal.FileChooser` at `/org/freedesktop/portal/desktop`: `OpenFile`, `SaveFile`, `SaveFiles` (`(o handle, s app_id, s parent_window, s title, a{sv} options) -> (u response, a{sv} results)`).
   - Export `org.freedesktop.impl.portal.Request` (`Close()`) on each in-flight handle for caller cancellation.
   - Options: `multiple` (`b`), `directory` (`b`), `filters` (`a(sa(us))`), `current_filter` (`(sa(us))`), `current_folder` (`ay`), `current_name` (`s`), `accept_label` (`s`), `choices` (`a(ssa(ss)s)`), `modal` (`b`).
2. **Chooser window** (`src/files/files_chooser.{h,c}`): reuse `files_model.c` and `files_sidebar.c` in an undecorated `880×580` dialog (`.shell-settings-window .shell-files-chooser`):
   - header bar with mode badge (`OPEN FILE`, `OPEN MULTIPLE`, `SELECT FOLDER`, `SAVE FILE`), caller title and `.settings-close-btn`;
   - places sidebar, breadcrumb / URI bar, search, virtualized list / grid with `Space` Quick Look;
   - bottom bar: filename entry (save mode), filter dropdown (glob `u=0`, MIME `u=1`), `[ CANCEL ]` (`response = 1`), `[ ✓ OPEN ]` / `[ ✓ SAVE ]` (`response = 0`, `uris` + `current_filter` in results).
3. **Parent & modal:** parse `parent_window` (`"wayland:<handle>"`), `zxdg_importer_v2_import_toplevel()` + `zxdg_imported_v2_set_parent_of()`, plus `xdg_wm_dialog_v1.get_xdg_dialog()` + `set_modal()`, so `kajo-comp` centres and stacks the chooser over the caller (already supported by the compositor, spec §2.5 / §3.3 `O3`). Add an `^org\.kajo\.Files\.Chooser$` floating rule in `data/defaults/rules.toml` if needed.
4. **Registration:** install `/usr/share/xdg-desktop-portal/portals/kajo.portal` (`DBusName=org.freedesktop.impl.portal.desktop.kajo`, `Interfaces=org.freedesktop.impl.portal.FileChooser;`) and `org.freedesktop.impl.portal.desktop.kajo.service` (`Exec=…/kajo-files --portal`); add `org.freedesktop.impl.portal.FileChooser=kajo;gtk;` under `[preferred]` in `kajo-portals.conf`.

---

### 3.9 [PLANNED] Apply the Kajo theme to other apps (GTK, Qt)

Nothing in the repository themes other toolkits today (no GTK/Qt theme export, no GSettings writes). On Host A, GSettings has `color-scheme = 'prefer-light'`, `gtk-theme = 'Adwaita'` and `accent-color = 'blue'`, so GTK4/libadwaita apps such as Nautilus render light and Adwaita-styled next to the dark shell. `~/.config/gtk-3.0` and `gtk-4.0` contain only a `dank-colors.css` left by another tool (there is no `gtk.css` that loads it). `qt6ct` is installed and configured, but `QT_QPA_PLATFORMTHEME` is not set; `qt5ct`, Kvantum and `adw-gtk3` are not installed.

| Layer | Reaches | Mechanism | Cost / caveat |
| :--- | :--- | :--- | :--- |
| 1. Dark/light and accent | libadwaita/GTK4 apps, Chromium/Thorium, Qt 6 apps that follow the portal | Settings portal backend (§3.8.1), or GSettings `color-scheme` on SAVE & APPLY | Small. Which source libadwaita reads for the accent was not verified |
| 2. Full palette and shape | GTK3 and GTK4 apps | Generate `~/.config/gtk-4.0/gtk.css` and `gtk-3.0/gtk.css` from the `@metro-*` tokens in `data/defaults/style.css` (surfaces, accent, 0 px radius) | Moderate. Breaks when libadwaita renames a named color; apps with their own CSS only partly follow |
| 3. Qt palette | Qt apps | Generate a `qt6ct` color scheme and set the platform theme in the session environment | Moderate. Needs `qt6ct`; Qt 5 and Kvantum are not installed |

**Not reachable:** Electron apps (VS Code) and terminals (kitty) draw their own UI and take only the dark/light preference or their own theme files.

**Rules (AGENTS.md §8):**
- An explicit opt-in toggle in `kajo-settings`, off by default, with a working revert. No automatic changes to other apps' configuration.
- Write only a clearly marked file or block. Never overwrite an existing `gtk.css` or `qt6ct.conf`, and warn about leftovers such as `dank-colors.css`.
- Do not ship the toggle until each layer it offers works end to end; verify against a real GTK4, GTK3 and Qt 6 app.
