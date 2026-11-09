# Kajo "Aero + Metro" Unified UI Design System & Style Guide (`docs/design_system.md`)

**Status:** Active Specification  
**Applies To:** `kajo` (`org.kajo.shell`), `kajo-settings` (`org.kajo.Settings`), `kajo-files` (`org.kajo.Files`), `kajo-player` (`org.kajo.Player`), `kajo-mod` (`org.kajo.Mod`), `kajo-ide` (`org.kajo.IDE`), `kajo-greeter` (`org.kajo.greeter`), and future Kajo applications (`kajo-viewer`, `kajo-monitor`, `kajo-capture`)  
**Interactive Reference:** [`docs/ui-gallery.html`](file:///home/mikolas/src/Kajo/docs/ui-gallery.html)  
**Master Stylesheet:** [`data/defaults/style.css`](file:///home/mikolas/src/Kajo/data/defaults/style.css)

---

## 1. Design Philosophy

The Kajo Desktop Suite blends **Windows 8/10 Metro** (sharp `0px` geometry, high-contrast typographic hierarchy, live accent borders, technical badges) with **Windows Aero Glass / OLED Dark Discipline** (deep `#101010`–`#1a1a1a` layered surfaces, translucent panel/OSD surfaces, sub-0.05% idle resource usage):

1. **Zero Corner Radius (`0px`):** All windows, cards, popovers, buttons, inputs, dropdowns, sliders, and levelbars use `border-radius: 0px` (sole exception: `3px` keycap radius on `window.osk-window button.osk-key` for touch ergonomics).
2. **1px Structural Borders + Active Accent Framing:**
   - Top-level windows and popovers are framed by `1px solid @metro-accent`.
   - Internal cards, rows, and inputs use `1px solid #2b2b2b` (`@metro-border`), lighting up to `1px solid @metro-accent` on `:hover`, `:focus-within`, or `.active` / `:selected`.
3. **Monospace Technical Typography (`Fira Code`):**
   - Uppercase headers with `1px` letter-spacing, bracketed telemetry badges (`[ HEVC•VAAPI | 1920×1080 ]`), and right-aligned keyboard shortcut hints (`Ctrl+Shift+C`).
4. **3-Zone Container Architecture:**
   - Every popover, modal overlay, and standalone window divides cleanly into **Zone 1: Header Bar** (`#141414`, bottom border), **Zone 2: Body / Content Canvas** (`#141414` / `#181818`), and **Zone 3: Footer / Status Bar** (`#141414`, top border).

---

## 2. Color & Surface Elevation Tokens

| GTK4 `@define-color` Token | CSS Variable (`ui-gallery.html`) | Default Value | Role |
| :--- | :--- | :--- | :--- |
| `@metro-surface-0` | `--surface-0-canvas` | `#101010` | Recessed media/thumbnail well, code viewer canvas |
| `@metro-surface-bg` | `--surface-1-base` | `#141414` | Primary window, popover, header, and sidebar background |
| `@metro-surface-2` | `--surface-2-ribbon` | `#161616` | Secondary action ribbon and sub-toolbar background |
| `@metro-surface-3` | `--surface-3-card` | `#181818` | Popover body, list row, and table column header surface |
| `@metro-surface-4` | `--surface-4-control` | `#1a1a1a` | Interactive button, tile card, and section card surface |
| `@metro-surface-hover` | `--surface-5-hover` | `#222222` | Hovered button, row, or card surface |
| `@metro-border` | `--metro-border` | `#2b2b2b` | Structural `1px` divider and inactive control border |
| `@metro-accent` | `--metro-accent` | `#0078d4` | Primary active accent (overridden live from `00-accent.css`) |
| `@metro-accent-hover` | `--metro-accent-hover` | `#1084d8` | Hovered primary accent fill |
| `@metro-fg-primary` | `--metro-fg-primary` | `#ffffff` | Primary text and active icon foreground |
| `@metro-fg-secondary` | `--metro-fg-secondary` | `#a0a0a0` | Secondary subtitles, metadata, and inactive icons |
| `@metro-urgent` | `--metro-urgent` | `#e81123` | Destructive fills/borders, critical battery, window close hover |
| `@metro-urgent-text` | `--metro-urgent-text` | `#ff6b6b` | Error / danger **text** on dark surfaces (readable red) |
| `@metro-warning` | `--metro-warning` | `#ff8c00` | Warning banners, VU meter high zone, center accent stripe |
| `@metro-success` | `--metro-success` | `#10893e` | Connected/mounted indicators, right accent stripe |
| `@metro-telemetry` | `--metro-telemetry` | `#00d8f0` | FFT spectrum bars, PAM verifying state, live OSD metrics |

Use the tokens in `style.css`; do not add new raw hex literals for these roles. Remaining one-off grays (diff tints, syntax colors, the `#202020`/`#1c1c1c` family) are intentionally literal for now and should be folded into this ladder when touched.

---

## 3. Typography & Configurable Font Category Sizes

Kajo exposes **4 independent Font Category Size sliders** in `kajo-settings → Theme` (`config.json` & `~/.config/kajo/style.d/00-accent.css` via `shell_theme_write_override_css()`):

| Category Key (`config.json`) | Default (`px`) | Range (`px`) | Target Classes & Selectors |
| :--- | :--- | :--- | :--- |
| `ui_font_size_px` (**Primary Body & Row Titles**) | `14px` | `11–20px` | `.row-title`, `.sidebar-label`, `.navigation-sidebar row`, `.files-list-name`, `.files-tile-title`, `.tile-title`, `entry`, `passwordentry`, `searchentry`, `.shell-launcher-input`, `dropdown > button`, `dropdown listview row`, `button.files-menu-row`, `.shell-launcher-row label`, `.shell-widget`, `.shell-osd-label` |
| `font_heading_size_px` (**Section & Window Headers**) | `13px` | `10–18px` | `.settings-header-title`, `.shell-popover-title`, `.heading`, `.shell-popover-section-title`, `.settings-section-title`, `.sidebar-category-header` |
| `font_subtitle_size_px` (**Subtitles, Buttons & Metadata**) | `12px` | `10–18px` | `.row-subtitle`, `.dim-label`, `.shell-popover-subtitle`, `.empty-state-label`, `.shell-device-placeholder`, `popover button`, `window.shell-settings-window button`, `button.files-nav-btn`, `button.player-transport-btn`, `.files-breadcrumb-btn`, `.files-list-meta`, `.files-meta-key`, `.files-meta-val`, `.files-status-text`, `.files-quicklook-textview`, `checkbutton` |
| `font_badge_size_px` (**Technical Badges & Micro Labels**) | `11px` | `9–16px` | `.shell-popover-badge`, `.files-ctx-shortcut`, `button.files-col-header-btn`, `.tile-subtitle`, `.files-tile-sub`, `.player-timecode`, `.player-stats-text`, `.code-preview-box`, `.process-row`, `.greeter-field-label` |

---

## 4. Canonical Component Inventory

### 4.1 Window Chrome & Navigation Bars
- **Window Root:** `window.shell-settings-window` (`1px solid @metro-accent`, `#141414` background, `0 8px 32px rgba(0,0,0,0.7)` shadow).
- **Header Bar (`.settings-header-bar`):** Wrapped in `GtkWindowHandle`, `padding: 6px 12px`, `border-bottom: 1px solid #2b2b2b`. Always contains:
  - Left: App icon + `.settings-header-title` (plus optional compact nav buttons `.files-nav-btn`).
  - Center: Location breadcrumb bar (`.files-breadcrumb-btn`) or status badge (`.shell-popover-badge`).
  - Right: View/drawer toggles, `"🗗 FLOAT / TILE"` button, and `.settings-close-btn` (`✕`, `#e81123` on `:hover`).
- **Secondary Action Ribbon (`.files-action-ribbon`):** `#161616` background, `border-bottom: 1px solid #2b2b2b`, `padding: 5px 12px`, housing context actions (`+ NEW FOLDER`, `+ NEW FILE`, `📋 PASTE`, `👁 HIDDEN`, `⇅ SORT`, `>_ TERMINAL`).
- **Navigation Sidebar (`.navigation-sidebar`):** `250px` width, `#141414` background, `border-right: 1px solid #2b2b2b`. Selected row uses `background-color: alpha(@metro-accent, 0.80); border-left: 3px solid #ffffff; color: #ffffff;`.

### 4.2 Buttons
1. **Standard Button (`button` inside Kajo windows/popovers):**
   - `#1a1a1a` background, `1px solid #2b2b2b`, `#ffffff` text, `padding: 6px 12px`, `font-size: 11px`, `font-weight: 600`.
   - `:hover`: `#222222` background, `1px solid @metro-accent`.
   - `:active` / `.active`: `@metro-accent` background, `#ffffff` text.
2. **Primary Accent Button (`.action-btn-primary`, `.files-accent-btn`, `.player-transport-btn-primary`):**
   - `@metro-accent` background, `1px solid @metro-accent`, `#ffffff` bold text (`font-weight: 700`).
   - `:hover`: `@metro-accent-hover` background.
3. **Compact Toolbar / Icon Button (`.files-nav-btn`, `.player-transport-btn`):**
   - `min-width: 28px`, `min-height: 26px`, `padding: 2px 8px`, `#1a1a1a` background, `1px solid #2b2b2b`.
   - `.active` / `:hover`: `@metro-accent` background and border.
4. **Destructive / Danger Button (`.action-btn-danger`, `.destructive-action`):**
   - Default: `#1a1a1a` background (or transparent), `#e81123` text/border.
   - `:hover`: `#e81123` background, `#e81123` border, `#ffffff` text.

### 4.3 Form Controls, Sliders & Meters
- **Text / Search / Password Entries (`.shell-launcher-input`, `.lock-password-entry`, `entry`):**
  - `#141414` background, `1px solid #2b2b2b`, `0px` radius, `padding: 6px 10px`, `13px` `Fira Code`.
  - `:focus` / `:focus-within`: `#1a1a1a` background, `1px solid @metro-accent`.
- **DropDown (`dropdown > button`):**
  - `#141414` background, `1px solid #2b2b2b`, `0px` radius; popover menu framed by `1px solid @metro-accent`.
- **Sliders (`scale`, `.player-seek-scale`):**
  - `trough`: `#262626`, `0px` radius, `min-height: 6px`.
  - `highlight`: `@metro-accent`, `0px` radius.
  - `slider`: `#ffffff` with `2px solid @metro-accent` border, `0px` radius (`10×14px` on seek bars, `14×14px` on settings sliders).
- **Level Bars (`levelbar`, `.files-drive-bar`):**
  - `trough`: `#262626`, `min-height: 6px`, `0px` radius.
  - `block.filled`: `@metro-accent` (or 3-zone VU colors: `.low` `#0078d4`, `.high` `#ff8c00`, `.full` `#e81123`).

#### Switch row vs Toggle card vs Segmented group (choose by what is being set)
| Control | Use for | Notes |
| :--- | :--- | :--- |
| **Switch row** (`kajo-switch` / `GtkSwitch` in a `.widget-row-card`) | A plain on/off preference whose title and subtitle say everything: mute, word wrap, lock on suspend, Wi-Fi power. | `create_action_row(icon, title, subtitle, GtkSwitch)`. |
| **Toggle card** (`.toggle-card`, a `GtkToggleButton`) | A *feature* with a status line (VRR, Autohide), a possible *unsupported* state, or its own sub-settings. | `create_toggle_card()`. State is `:checked`. A 4px left indicator bar lights with `@metro-accent` when on (dim `@metro-border` when off); the body only gets `alpha(@metro-accent, 0.12)` so title/subtitle keep contrast. Disabled: 45% opacity, bar dim, subtitle states the reason. The panel Control Center quick tiles share this component. |
| **Segmented group** (`.segmented-group` of `GtkToggleButton`s) | 2–4 short, mutually exclusive choices (Top/Bottom, ICON-ONLY/TEXT-ONLY/FULL, 2/4/8). | `create_segmented_group()`. Longer lists stay `GtkDropDown`. |

Never fake an on/off setting with a dropdown, and never ship a toggle that is not backed by real state.

### 4.4 Cards, Rows, Tables & Context Menus
- **Section Cards (`.section-card`, `.section-card-left`, `.section-card-center`, `.section-card-right`):**
  - `#1a1a1a` background, `1px solid #2d2d2d`, optional `4px` left accent border (`@metro-accent`, `#ff8c00`, or `#107c41`).
- **Interactive Control / List Rows (`.widget-row-card`, `.files-list-row`, `.files-menu-row`, `.player-queue-row`):**
  - `#181818` / `#1a1a1a` background, `1px solid #262626`, `border-left: 3px solid transparent`, `padding: 6px 10px`.
  - All horizontal children (`icon`, `vbox`, `label`, `button`, `switch`) **MUST** set `valign: GTK_ALIGN_CENTER`.
  - `:hover`: `#242424` background, `border-color: @metro-accent`.
  - `:selected` / `.active`: `alpha(@metro-accent, 0.25)` background, `border-color: @metro-accent`, `border-left: 3px solid #ffffff` (or `@metro-accent` in menus).
- **Sortable Table Column Headers (`.files-list-header`, `button.files-col-header-btn`):**
  - `#181818` header bar with clickable `11px` uppercase column buttons (`NAME ▲`, `SIZE`, `TYPE`, `MODIFIED`). Active sort column has `color: @metro-accent; border-bottom: 2px solid @metro-accent;`.
- **Metro Tile Grid Cards (`.files-tile-card`, `.files-tile-folder`):**
  - `136×114px` tile in `#1a1a1a` with `1px solid #2b2b2b`; folders carry `border-top: 2px solid @metro-accent`. Selected tile uses `alpha(@metro-accent, 0.26)` and `1px solid @metro-accent`.

### 4.5 File Explorer (`kajo-files`) Places Sidebar, Volumes & Inline Prompts
- **Places & Volumes Sidebar (`.files-sidebar`, `230px` width):**
  - Grouped by uppercase `.sidebar-category-header` (`PLACES`, `DEVICES & DRIVES`, `NETWORK`).
  - **Mounted Volume Rows (`.files-drive-row`):** Two-line row showing volume icon, label, free/total GB readout, a `6px` `.files-drive-bar` capacity `levelbar` (`#10893e` normal, `#ff8c00` >75%, `#e81123` >90%), and a compact `⏏` eject/unmount button (`.files-eject-btn`).
- **Hybrid Breadcrumb / Raw URI Bar (`.files-breadcrumb-bar`):**
  - Full root-to-leaf segment buttons (`.files-breadcrumb-btn`, with `⌂ <user>` badge on `$HOME` and `📦 <archive>` on `archive://` roots) separated by `▸` chevrons, auto-scrolled flush-right after layout so the leaf directory and immediate parents are always visible.
  - Mouse wheel scrolls the breadcrumb bar horizontally (`48px` per tick); `Ctrl+Click` or middle-click opens any ancestor segment in a new window; right-clicking any segment opens the **Breadcrumb Subfolder Jump Popover** (`SUBFOLDERS` badge, up to 50 sorted child directories with `… and N more subfolder(s)` overflow note, `🗔 Open in New Window`, and `🔗 Copy Path / URI`).
  - Clicking `✎` (`Ctrl+L`) switches inline to a full-width monospace URI entry (`file:///`, `archive:///`, `smb://`, `trash:///`, `sftp://`).
- **Multi-Selection, Inspector & Operation Cancellation (`kajo-files`):**
  - Rubber-band and `Ctrl`/`Shift` multi-selection (`GtkMultiSelection`) preserves the full selected URI set across live directory refreshes.
  - Right-clicking when `> 1` items are selected opens the **Batch Context Menu** (`N ITEMS SELECTED` title, `BATCH` badge) exposing only true batch actions (`Compress to Archive...`, `Copy`, `Cut`, `Copy Path / URI`, `Restore from Trash`, `Move to Trash`, `Delete Permanently`).
  - The right-hand Inspector pane (`.files-inspector-pane`) includes a **`☑ EXECUTABLE (+X)`** toggle button (`chmod +x/-x`) for local regular files, while the bottom `.files-status-bar` reveals a danger **`[ ✕ CANCEL ]`** button (`Esc`) whenever an async file operation is in flight.
- **Open With Built-in Rows (`.files-menu-row`):**
  - For disk images (`.iso`, `.img`, `.raw`) the Open With sub-view starts with **`💿 Mount Disk Image`** (UDisks2 loop-mount, then navigates into the mount) above the application list. It is not an application, so "Set as Default" does not apply to it. Plain **Open** browses an ISO as `archive://` and mounts `.img` / `.raw`.
- **Contextual Inline Reveal Bars (`GtkRevealer`):**
  - **New Folder / New File / Rename / Compress / Delete Confirm Bar (`.files-prompt-bar`):** `#181818` surface with `border-left: 3px solid @metro-accent`, inline `GtkEntry`, and `[ CREATE ]` / `[ RENAME ]` / `[ COMPRESS ]` / `[ DELETE PERMANENTLY ]` / `[ CANCEL ]` buttons.
  - **Virtual Read-Only Archive Banner (`.files-prompt-bar`):** Shown when browsing inside `archive://<path>!/<subpath>`, displaying `📦 ARCHIVE VIEW (READ-ONLY) — <archive> / <subpath>` alongside `[ 📦 EXTRACT ALL ]` and `[ ↩ EXIT ]` buttons.
  - **LUKS / SMB Authentication Bar (`.files-auth-bar`):** `#181818` surface with `border-left: 3px solid #ff8c00`, passphrase `GtkPasswordEntry` (or username/domain/password fields for `smb://`), and `[ 🔓 UNLOCK ]` action.
  - **Trash Action Banner (`.files-trash-bar`):** `#1f1214` surface with `1px solid #4a1c20` and `border-left: 3px solid #e81123`, offering `[ ↺ RESTORE ALL ]` and destructive `[ 🗑 EMPTY TRASH ]`.

### 4.6 Kajo IDE (`kajo-ide`) Code Canvas, Tree Explorer, Diff & Debugger Components
- **Buffer Tab Ribbon (`.ide-tab-ribbon`, `.ide-tab-pill`):**
  - `#161616` strip with `1px solid #2b2b2b` bottom border. Each `.ide-tab-pill` displays `[1..9]` index, file icon, filename, state badge (`●` dirty in `@metro-accent`, `⚡` reloaded in `#00d8f0`, `⚠` collision in `#e81123`), and `×` close button. Active tab uses `#101010` background and `border-top: 2px solid @metro-accent`.
- **Hierarchical Project File Tree (`.ide-tree-sidebar`, `.ide-tree-row`):**
  - Compact `26px`-tall tree rows with `1px solid #242424` vertical indentation guides, folder chevrons (`▾`/`▸`), file-type icon, filename (colored `#ff8c00` if modified, `#e81123` if containing LSP errors), and right-aligned Git status pill (`.ide-git-badge`: `M` `#ff8c00`, `A` `#10893e`, `U` `#00d8f0`, `D` `#e81123`).
- **Code Canvas, 6-Column Cairo Gutter & Minimap (`.ide-editor-canvas`, `.ide-gutter`, `.ide-minimap`):**
  - `#101010` canvas with `#171717` current-line highlight, `alpha(@metro-accent, 0.32)` selection fill (or `rgba(255, 140, 0, 0.28)` in `SELECT` mode), and end-of-line inline diagnostic pills (`.ide-inline-diag-pill`).
  - `56px` left gutter (`#121212`, `border-right: 1px solid #222222`) rendering 6 columns: Breakpoint (`●` `#e81123`), Diagnostic (`✖`/`⚠`/`ℹ`), DAP PC (`▶` `#ffd700`), Line Number, Git/Agent Diff Bar (`+` `#10893e`, `~` `#0078d4`, `_` `#e81123`, Agent `#b4009e`), and Fold Chevron (`▾`/`▸`).
- **Modal Status Bar & Mode Pills (`.ide-status-bar`, `.ide-mode-pill`):**
  - `26px` bottom bar with bold uppercase `.ide-mode-pill`: `NORMAL` (`@metro-accent`), `SELECT` (`#ff8c00`), `INSERT` (`#10893e`), `MATCH` (`#b4009e`), `LEADER` (`#00d8f0`), `TREE`/`DRAWER`/`PICKER` (`#0078d4`).
  - Three zones like the gallery: left (mode pill, file, hot-reload), center `.ide-status-chord` (pending leader/match chord, `@metro-warning`), right (`.shell-popover-badge.agent` magenta AGENT badge, LSP badge that gains `.success` once the server is ready, diagnostics `.urgent`, encoding/position).
- **Right-Click Code Menu (`popover.menu modelbutton`):**
  - GTK `extra menu` on the editor: `0px` radius, Fira Code, `@metro-accent` hover fill, `#2b2b2b` separators. Holds only code actions (definition, implementation, references, hover, rename, code actions, format); server-only rows are disabled until the language server is ready. Specimen: `.ide-code-menu` in `docs/ui-gallery.html`.
  - The path breadcrumb shortens to `repo ▸ … ▸ parent ▸ dir ▸ file` beyond 5 segments and ellipsizes at the start; the full path is the tooltip.
- **Pixel-Anchored LSP Completion & Hover Popovers (`.ide-lsp-popover`):**
  - Framed by `1px solid @metro-accent` at cursor `(x, y)` coordinates. Completion rows include a `18×18px` `.ide-symbol-badge` (`ƒ` Function `#dcdcaa`, `S` Struct `#4ec9b0`, `v` Variable `#569cd6`, `k` Keyword `#c586c0`, `m` Macro `#00d8f0`), symbol label, and right-aligned type signature.
- **Unified & Word-Level Diff Hunk Cards (`.ide-diff-card`):**
  - Used in Search & Replace (`Space s r` / `s p`) and Agent Transaction Review (`Space d s`). Hunk header strip (`@@ -140,6 +140,8 @@`) in `#181818` with `[ ✓ ACCEPT HUNK ]` / `[ ✕ REVERT HUNK ]` buttons; deleted lines use `rgba(232, 17, 35, 0.16)` with `#ff8080` text (`rgba(232, 17, 35, 0.42)` on changed tokens) and added lines use `rgba(16, 137, 62, 0.18)` with `#85e89d` text (`rgba(16, 137, 62, 0.45)` on changed tokens).
- **DAP Debugger 3-Pane Inspector (`.ide-dap-inspector`):**
  - Horizontal 3-column layout in the bottom drawer: Call Stack list (active frame highlighted with `▶` `#ffd700`), expandable Scopes/Locals variable tree, and interactive Debug Console / REPL entry.

### 4.7 Full Graphical Markdown & Cairo Vector Mermaid Components (`.ide-md-document`)
- **Document Typography & Inline Code:**
  - Rendered as native GTK4 widgets (never terminal ASCII art). Headings `.ide-md-h1` (`22px` bold, `1px solid #2b2b2b` bottom border), `.ide-md-h2` (`18px`), `.ide-md-h3` (`15px`), proportional body text (`14px`, `#dddddd`), and `.ide-md-code-inline` (`Fira Code` `12px`, `#00d8f0` on `#1e1e1e`, `1px solid #333333`, `padding: 1px 5px`).
- **GitHub-Flavored Callout Alert Cards (`.ide-md-alert`):**
  - `#161616` card with `1px solid #2b2b2b` and `4px` semantic left border: `.note` (`#0078d4`, `ℹ NOTE`), `.tip` (`#10893e`, `💡 TIP`), `.important` (`#b4009e`, `⚑ IMPORTANT`), `.warning` (`#ff8c00`, `⚠ WARNING`), `.caution` (`#e81123`, `🛑 CAUTION`).
- **Fenced Code Blocks with Copy Header (`.ide-md-codeblock`):**
  - `#101010` syntax-highlighted `GtkSourceView` framed by `1px solid #2b2b2b`, topped by a `#181818` `.ide-md-codeblock-header` displaying the language badge (`[ C99 ]`) and `[ 📋 COPY ]` button.
- **Graphical Tables (`.ide-md-table`) & Interactive Task Checklists (`.ide-md-task-row`):**
  - `GtkGrid` table with `#181818` uppercase header cells and `1px solid #2b2b2b` borders; task list items use live `GtkCheckButton` widgets synced bi-directionally with `- [ ]` / `- [x]` in the source buffer.
- **Cairo Vector Mermaid Diagram Card (`.ide-mermaid-card`, `.ide-mermaid-canvas`):**
  - Anti-aliased vector graph canvas (`#101010` background, `1px solid #2b2b2b`) with a top `.ide-mermaid-toolbar` (`[ 🔍− ] [ 100% ] [ 🔍+ ] [ ⛶ FIT ] [ 💾 EXPORT SVG ] [ 📷 EXPORT PNG ]`). Nodes render as crisp Metro boxes (`#181818` fill, `1.5px` `@metro-accent` or semantic stroke) connected by Cubic Bézier vector paths; clicking any node jumps the editor cursor to its source line.

### 4.8 Retro Amiga, C64 SID & Tracker Music Player (`kajo-mod`) Components
- **Niri Column Geometry & Controlled Reflow Rules (`0.33333` / `0.5` / `0.66667`):**
  - In Niri (`default-column-width { proportion 0.5; }`, `preset-column-widths { 0.33333; 0.5; 0.66667; }`, `gaps 6`, `clip-to-geometry true`), column widths on `2880×1800 @ 1.75x` (`1646×1028` logical) are **`~540px`** (`1/3`), **`~814px`** (`1/2` default), and **`~1088px`** (`2/3`).
  - Every top fixed-height zone (`Zones 1–5`) is engineered with a **minimum width $\le 480\text{px}$** and **zero `flex-wrap` height changes** so cycling `Mod+R` across `1/3`, `1/2`, and `2/3` columns never clips the right edge and never shifts vertical widget positions.
- **LCD Telemetry Readout Deck (`.mod-telemetry-deck`, `.mod-counter-strip`):**
  - `#141414` card framed by `1px solid #2b2b2b` and `border-left: 3px solid @metro-accent`.
  - Top row displays `.mod-track-title` and `.mod-track-sub` with `PANGO_ELLIPSIZE_END` (`min-width: 0`) alongside fixed right-aligned engine/hardware badges (`.shell-popover-badge.accent` `[ UADE 3.05 ]` / `[ RESIDFP 3.1 ]` / `[ LIBGME ]` / `[ OPENMPT ]`, format badge `[ TFMX-PRO 7V ]`, and filter state `[ LED: ON ]`).
  - Bottom `.mod-counter-strip` (`#101010` recessed well, `1px solid #242424`) uses a **6-column homogeneous grid (`column-homogeneous: TRUE`)** with tabular numerals (`SUB │ VOICES │ TIMING │ CLOCK │ OUT │ TIME`) so counters divide column width evenly from `480px` to `1920px` without wrapping.
- **2-Row Deterministic Transport & Subsong Deck (`.mod-transport-deck`, `.mod-subsong-box`):**
  - Split into two fixed sub-rows (`min-width ~440px`) so controls never overflow in a `540px` `1/3` Niri column:
    - **Row 1:** Full-width seek slider (`hexpand: TRUE`) + first-class subsong stepper (`[ ◀ SUB ]`, `.mod-subsong-readout` `SUB 01 / 09`, `[ SUB ▶ ]`).
    - **Row 2:** Transport buttons (`⏮`, `▶ PLAY`, `⏸`, `⏹`, `⏭`), loop-mode `.segmented-group` (`ADVANCE` / `LOOP` / `FADE`), and volume slider.
- **2-Row Deterministic Hardware & DSP Matrix (`.mod-dsp-strip`):**
  - `#181818` control deck with `1px solid #2b2b2b` organized into two fixed rows (never `flex-wrap`):
    - **Row 1 (`~480px` min-width):** `FILTER` (`A500` / `A1200` / `NEAR` / `LIN` / `SINC`), `LED` (`AUTO` / `ON` / `OFF`), and `SID` (`AUTO` / `6581` / `8580`).
    - **Row 2 (`1fr 1fr` equal grid):** `STEREO SEP` (`0%..100%`) and `TEMPO` (`50%..200%` + `[1:1]`). Controls unsupported by the active backend are set insensitive (`opacity: 0.38`).
- **Full-Height Niri Tiled Layout & `< 760px` Inspector Reflow (`.mod-scope-deck`, `.mod-paned`, `.mod-info-pane`, `.mod-playlist-pane`):**
  - **Zone 5 (`vexpand: FALSE`, fixed `118px` height):** Per-Channel / Voice `[M]/[S]` Header Strip (`.mod-channel-header`, `4-col` homogeneous grid) + `1fr 1fr` Stereo Oscilloscope & 16-Band FFT Spectrum cards (`88px`).
  - **Zone 6 (`vexpand: TRUE`, vertical `GtkPaned` absorbing 100% of remaining column height):**
    - **Upper Expanding Pane (`.mod-info-pane` — Module, Samples/Voices & STIL/.NFO Inspector):** Uses controlled responsive reflow at the **`760px` width breakpoint** (`GdkSurface` `notify::width`, matching `SettingsApp`):
      - **Wide Column ($\ge 760\text{px}$ — Niri `1/2` `0.5` and `2/3` `0.667` columns):** Displays the scrollable **Samples / Instruments / Hardware Voices Table** (`Fira Code`, preserving 22–32 char Amiga ASCII liner notes) and the scrollable **HVSC STIL / Archive `.NFO` / Song Message** well **side-by-side in a `1.2fr 1fr` split**.
      - **Narrow Column ($< 760\text{px}$ — Niri `1/3` `0.333` column at `~540px`):** Automatically collapses to a **1-column full-width view** with a compact `[ SAMPLES / VOICES │ STIL / .NFO / MSG ]` `.segmented-group` in the pane header so 32-char ASCII sample tables are never clipped.
    - **Lower Expanding Pane (`.mod-playlist-pane` — Always-Docked Playlist & Archive Queue):** Toolbar (`[+ ADD]`, `[+ DIR]`, `[− REM]`, `[CLR]`, filter entry) + scrollable track queue where the track title column (`hexpand: TRUE`, `PANGO_ELLIPSIZE_END`) absorbs horizontal resize smoothly.
  - **Zone 7 (`vexpand: FALSE`, `24px`):** Bottom Status Bar (`.mod-status-bar`).

### 4.9 System & Suite Settings (`kajo-settings`) Scalable Niri Column Architecture
- **Interactive Reference:** [`docs/kajo-settings-mockup.html`](file:///home/mikolas/src/Kajo/docs/kajo-settings-mockup.html)
- **3-Tier Niri Column Reflow (`540px` / `814px` / `1088px` / `1634px`, strict `min-width <= 480px`):**
  - **Narrow Column ($< 720\text{px}$ — Niri `1/3` `0.33333` column at `~540px`):** Left navigation compacts to a **`46px` Icon Rail** (`.navigation-sidebar`, toggleable via `[☰]` in `.settings-header-bar`) so all **12 pages** remain 1-click accessible without a mobile "Back" drill-down, leaving `~494px` for the 1-column card canvas.
  - **Default Column ($720\text{..}959\text{px}$ — Niri `1/2` `0.50` column at `~814px`):** Displays the **`196px` Category Sidebar** (`HARDWARE & SYSTEM`, `SHELL & COMPOSITOR`, `KAJO SUITE APPS`, `SYSTEM INFO` with right-aligned live telemetry micro-badges) + 1-column card stack.
  - **Wide Column ($\ge 960\text{px}$ — Niri `2/3` `0.66667` at `~1088px` & `1.0` at `~1634px`):** Displays the `196px` Category Sidebar + **2-column balanced Bento Card Grid** (`.settings-card-grid`, with `.span-full` for full-width matrices/lists).
- **4-Zone Full-Height Vertical Architecture (`100vh`):**
  1. **Zone 1 (`vexpand: FALSE`, `36px`):** Window Chrome Header Bar (`.settings-header-bar`) with `[☰]` rail toggle, `⚙ SYSTEM SETTINGS`, page index badge (`[ 11 / 12 • KAJO MOD ]`), live search filter (`Ctrl+F`), `[🗗 FLOAT / TILE]`, and `[✕]`.
  2. **Zone 2 (`vexpand: FALSE`, `58px`):** Pinned **Page Telemetry & Hardware Status Deck** (`.settings-page-deck`, `.settings-counter-strip`) displaying the page title, config/D-Bus subtitle, backend badges, and a **4-column homogeneous telemetry strip** that never wraps across `480px..1920px`.
  3. **Zone 3 (`vexpand: TRUE`):** Scrollable **Page Content Canvas** (`.settings-scroll-canvas`) where every setting row uses `.widget-row-card` (`hexpand: TRUE`, `PANGO_ELLIPSIZE_END` on `.row-title` / `.row-subtitle`, zero hardcoded `width-chars` traps) and dynamic list/matrix cards set `vexpand: TRUE` to absorb Niri's full-height column.
  4. **Zone 4 (`vexpand: FALSE`, `36px`):** Docked **Action & Persistence Footer Bar** (`.settings-footer-bar`) pinned at the bottom of the viewport with live dirty/saved status indicator, page secondary action, `[ ↺ REVERT ]`, and `[ ✓ SAVE & APPLY ]`.
- **12 Modular Pages:** `displays`, `network`, `bluetooth`, `audio`, `theme`, `panel`, `widgets`, `notifications`, `niri`, `ide`, **`mod` (`Kajo Mod`)**, and `about`.

### 4.10 Wayland Panel Bar, Widget Chips, 3-Zone Popovers & OSD (`kajo`)
- **Panel Container (`window.shell-panel`, `#panel-container`, `.shell-panel-left`, `.shell-panel-center`, `.shell-panel-right`):**
  - `@metro-panel-bg` (`rgba(24, 24, 24, 0.82)`) surface with `1px` subtle edge hairlines and `0px` corner radius.
  - When hidden in OLED smart autohide mode (`#panel-container.autohide-hidden`), background uses `rgba(0, 0, 0, 0.01)` (never `transparent` or `opacity: 0`) so Wayland pointer/touch enter events fire reliably on the `2px` trigger strip.
- **Single-Item Widget Chips (`.shell-widget`, `menubutton.shell-widget > button`):**
  - `0px` corner radius, `padding: 0 6px`, `min-height: 0px` (scales freely with `cfg->panel_height` `24..64px`), `Fira Code` `ui_font_size_px` (`14px` default).
  - Inner icon + label rows are built with `shell_widget_pack_bar_box()` (`6px` spacing, `GTK_ALIGN_CENTER` on box, `GtkImage`, and `GtkLabel`), and icons size dynamically from `shell_widget_get_icon_size(widget)` without hardcoded CSS `min-width` overrides.
  - `:hover`: `@metro-surface-hover`. `:active`, `:checked`, and **`menubutton.shell-widget > button:checked`** (while popover is open): `@metro-accent` fill with `@metro-fg-primary` text.
  - **Widget Modes (`ShellWidgetMode`):** Every chip supports `ICON`, `LABEL`, and `FULL` modes (`shell_widget_apply_mode_visibility()` never hides both icon and label). Empty contextual widgets (`window` when unfocused, `tray` when `0` SNI items exist, `privacy` when mic/cam are idle, `backlight` on unsupported desktops) set `visible = FALSE` rather than leaving blank hoverable slots.
  - **Semantic Chip States:** `.shell-widget-volume.muted` (`@metro-fg-secondary`), `.shell-widget-battery.critical` (`@metro-urgent` bottom bar + `@metro-urgent-text` foreground at $\le 15\%$ discharging), `.shell-widget-dnd.active` & `.shell-widget-privacy.active` (`@metro-warning` bottom bar + foreground), and `.shell-widget-idle-inhibitor.active` (`@metro-telemetry` bottom bar + foreground).
- **Multi-Item Strip Widgets (`.shell-widget-workspaces`, `.shell-widget-tray`):**
  - Outer strip containers stay `transparent` on `:hover`; hover and active states apply strictly to individual child items:
    - `.workspace`: `2px 8px` flat tile, `:hover` `@metro-surface-hover`, `.active` / `.focused` `alpha(@metro-accent, 0.14)` + `2px solid @metro-accent` bottom indicator, `.urgent` `@metro-urgent` fill, and `<= 18px` `.workspace-app-icon` buttons.
    - `button.tray-item-button`: per-item `:hover` `@metro-surface-hover` and `:checked` `@metro-accent`.
- **Mandatory 3-Zone Popover Architecture (`popover.shell-popover`):**
  - Framed by `1px solid @metro-accent`, `@metro-surface-bg` (`#141414`), `0px` radius, and a canonical width-tier class on the root box: `.popover-compact` (`280px`), `.popover-medium` (`380px`), or `.popover-expanded` (`480px`).
  - **Zone 1 (`.shell-popover-header`):** `@metro-surface-2` header with uppercase `.shell-popover-title` + live right-aligned `.shell-popover-badge` (`.active`, `.success`, `.peak-warn`, `.urgent`).
  - **Zone 2 (`.shell-popover-body`):** Interactive controls following §4.3 (`GtkSwitch` rows, `GtkToggleButton.toggle-card` quick tiles, `.segmented-group` for 2–4 exclusive choices such as Power Profiles and Keyboard Backlight/Timeout, `selection-mode: none` on `GtkListBox` device lists, and `ScrolledWindow` height caps on unbounded telemetry/device lists).
  - **Zone 3 (`.shell-popover-footer`):** `@metro-surface-2` footer housing contextual actions and `kajo-settings --page <id>` shortcuts; destructive actions (`Power Off`, `Reboot`, `Close Window`) use `.destructive-action` and require explicit confirmation before invoking `logind` shutdown/reboot.
- **OSD & Notification Overlays (`window.shell-osd`, `.shell-notification-card`):**
  - `.shell-osd-card` (`240×48px`) centers icon, `0px`-radius `@metro-accent` `levelbar`, and tabular `.shell-osd-label` (`GTK_ALIGN_CENTER`).
  - `.shell-notification-card` uses `1px solid @metro-accent` + `4px` left bar (`@metro-urgent` when `urgency == 2` via `.critical` in both live overlay toasts and notification history) and compact `.popover-dismiss-btn` (`✕`, `@metro-urgent` hover).
