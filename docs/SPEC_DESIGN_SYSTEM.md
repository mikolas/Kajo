# SPEC_DESIGN_SYSTEM.md — Kajo "Aero + Metro" Unified UI Design System & Style Guide

**Status:** Active Specification  
**Applies To:** `kajo` (`org.kajo.Shell`), `kajo-settings` (`org.kajo.Settings`), `kajo-files` (`org.kajo.Files`), `kajo-player` (`org.kajo.Player`), `kajo-greeter` (`org.kajo.Greeter`), and future Kajo applications (`kajo-viewer`, `kajo-monitor`, `kajo-capture`)  
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
