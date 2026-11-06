# Kajo "Aero + Metro" Unified UI Design System & Style Guide (`docs/design_system.md`)

**Status:** Active Specification  
**Applies To:** `kajo` (`org.kajo.shell`), `kajo-settings` (`org.kajo.Settings`), `kajo-files` (`org.kajo.Files`), `kajo-player` (`org.kajo.Player`), `kajo-ide` (`org.kajo.IDE`), `kajo-greeter` (`org.kajo.greeter`), and future Kajo applications (`kajo-viewer`, `kajo-monitor`, `kajo-capture`)  
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
| *(literal)* `#101010` | `--surface-0-canvas` | `#101010` | Recessed media/thumbnail well, code viewer canvas |
| `@metro-surface-bg` | `--surface-1-base` | `#141414` | Primary window, popover, header, and sidebar background |
| *(literal)* `#161616` | `--surface-2-ribbon` | `#161616` | Secondary action ribbon and sub-toolbar background |
| *(literal)* `#181818` | `--surface-3-card` | `#181818` | Popover body, list row, and table column header surface |
| *(literal)* `#1a1a1a` | `--surface-4-control` | `#1a1a1a` | Interactive button, tile card, and section card surface |
| `@metro-surface-hover` | `--surface-5-hover` | `#222222` | Hovered button, row, or card surface |
| `@metro-border` | `--metro-border` | `#2b2b2b` | Structural `1px` divider and inactive control border |
| `@metro-accent` | `--metro-accent` | `#0078d4` | Primary active accent (synced from `00-accent.css`) |
| `@metro-accent-hover` | `--metro-accent-hover` | `#1084d8` | Hovered primary accent fill |
| `@metro-fg-primary` | `--metro-fg-primary` | `#ffffff` | Primary text and active icon foreground |
| `@metro-fg-secondary` | `--metro-fg-secondary` | `#a0a0a0` | Secondary subtitles, metadata, and inactive icons |
| `@metro-urgent` | `--metro-urgent` | `#e81123` | Destructive actions, critical battery, window close hover |
| *(literal)* `#ff8c00` | `--metro-warning` | `#ff8c00` | Warning banners, VU meter high zone, center accent stripe |
| *(literal)* `#10893e` | `--metro-success` | `#10893e` | Connected/mounted indicators, right accent stripe |
| *(literal)* `#00d8f0` | `--metro-telemetry` | `#00d8f0` | FFT spectrum bars, PAM verifying state, live OSD metrics |

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
- **Secondary Action Ribbon (`.files-action-ribbon`):** `#161616` background, `border-bottom: 1px solid #2b2b2b`, `padding: 5px 12px`, housing context actions (`+ NEW FOLDER`, `📋 PASTE`, `⇅ SORT`).
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
  - Segment buttons (`.files-breadcrumb-btn`) separated by `▸` chevrons; clicking `✎` (`Ctrl+L`) switches inline to a full-width monospace URI entry (`file:///`, `smb://`, `trash:///`, `sftp://`).
- **Contextual Inline Reveal Bars (`GtkRevealer`):**
  - **New Folder / Rename Bar (`.files-prompt-bar`):** `#181818` surface with `border-left: 3px solid @metro-accent`, inline `GtkEntry`, and `[ CREATE ]` / `[ CANCEL ]` buttons.
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
