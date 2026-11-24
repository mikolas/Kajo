# Specification: `kajo-viewer` — Aero + Metro PDF, Markdown & Image Viewer (`docs/spec_kajo_viewer.md`)

**Status:** Active Specification — tracking branch `feat/kajo-viewer`  
**Target Binary:** `kajo-viewer` (`org.kajo.Viewer`, `src/viewer/`)  
**Shared Cores:** `src/common/pdf_render.{h,c}`, `src/common/kajo_markdown*.{h,c}`, `src/common/kajo_mermaid.{h,c}`  
**Config Integration:** `KajoConfigStore` domain `viewer.toml` (`src/config/schemas/schema_viewer.c`)  
**Core Dependencies:** `gtk4`, `gio-2.0`, `cairo`, `pangocairo`, `poppler-glib`, `libarchive`  
**Optional Dependencies (`required: false`):** `glycin-2`, `glycin-gtk4-2`, `librsvg-2.0`, `libexif`  
**Interactive Mockup:** [`docs/kajo-viewer-mockup.html`](kajo-viewer-mockup.html)

---

## 1. Objectives, Window Model & Resource Discipline

`kajo-viewer` completes Kajo's first-party document & media viewing triangle (`kajo-player` for video/audio, `kajo-mod` for retro modules, **`kajo-viewer`** for PDFs, Markdown documentation, and high-resolution images) inside the unified frameless **Aero + Metro** aesthetic:

1. **Primary Functions First:**
   - **PDF Documents (`poppler-glib`):** Continuous scroll and 2-page spread, lazy HiDPI rendering, document Outline + Page Thumbnail sidebar, full-text search with match highlights, text selection/copy, internal/external links, rotation, paper tone modes (`PAPER` / `SEPIA` / `DARK`), and encrypted PDF unlocking.
   - **Markdown + Diagrams (Shared Kajo Markdown & Cairo Vector Mermaid):** Full graphical rendering of headings, tables, code blocks, checklists, embedded local images/PDFs/SVGs/carousels, and vector Mermaid diagrams, with auto-generated Heading Outline, `GFileMonitor` live reload, Back/Forward navigation across relative links, reading-width presets, and `Ctrl+F` search.
   - **High-Resolution & Modern Images (`glycin-gtk4-2` / `gdk-pixbuf` / `librsvg`):** Fast zoom-to-cursor and pan, vector `.svg` crisp zoom scaling, AVIF/HEIF/JXL/WebP/RAW/GIF decoding, EXIF orientation + metadata sidebar, sibling folder prev/next browsing (`←`/`→`) + collapsible filmstrip, checkerboard transparency backdrop, rotation/flip, and clipboard copy.
2. **Window Model (Tiling-First Single-Document Window):**
   - Designed for `kajo-comp`'s scrollable-tiling workspace strip and `kajo-files` launch workflow (`Open With` / default opener): one document or folder-image view per `kajo-viewer` window (matching `kajo-player`), with **in-window Back/Forward history** (`Alt+←` / `Alt+→`) when following relative links inside Markdown or switching sibling images in a directory.
   - `GtkApplication` (`org.kajo.Viewer`, `G_APPLICATION_NON_UNIQUE` or explicit window-per-open dispatch) allows comparing two PDFs/Markdown docs side-by-side in separate tiled columns cleanly.
3. **Map-Gated & Viewport-Gated Resource Discipline (`AGENTS.md §4.D`, `§7`):**
   - **Zero idle polling:** No background timers when unmapped or idle (`< 0.05%` idle CPU). Animated GIF/WebP frame timers pause immediately on `unmap` and resume on `map`.
   - **Bounded memory for massive PDFs:** Only visible pages (plus a small +/- 2 prefetch window) are rendered asynchronously on worker threads (`GTask` + `GCancellable`), cached in an LRU texture ring (default max 12 full-scale pages) and evicted cleanly on scroll/zoom.
   - **Strict async lifetime safety:** Every async decode, Poppler render, `GFileMonitor`, or clipboard read uses `GWeakRef` (`GtkWidget` subclasses) or `GCancellable` checked before touching UI state.

---

## 2. Shared Code Extraction & `src/viewer/` Architecture

Per `AGENTS.md §7` (*one source of truth; no duplicated subsystems*), `kajo-viewer` reuses and promotes existing Kajo code rather than maintaining parallel PDF or Markdown renderers:

### 2.1 Shared Cores in `src/common/`

| Shared Module | Provenance | Additions Needed for `kajo-viewer` |
| :--- | :--- | :--- |
| **`src/common/pdf_render.{h,c}`** | Existing helper used by `kajo-files` Quick Look & Markdown PDF cards | Add `kajo_pdf_render_page_scaled(PopplerDocument *doc, int page_idx, double scale, int rotation_deg, KajoPdfPaperTone tone, const GArray *highlight_rects, double active_match_idx)` preserving the existing `kajo_pdf_render_page_texture()` wrapper untouched for callers. |
| **`src/common/kajo_markdown.{h,c}`**<br>**`src/common/kajo_markdown_embeds.{h,c}`**<br>**`src/common/kajo_mermaid.{h,c}`** | Promoted from `src/ide/ide_markdown*.{h,c}` & `src/ide/ide_mermaid.{h,c}` | Remove the unused `#include "ide_buffer.h"` coupling, add heading outline callback / query (`kajo_markdown_view_get_headings()`), read-only checklist policy toggle, reading-width & font-scale CSS/layout hooks, and text-search highlight/jump API. `kajo-ide` retains thin compatibility wrappers or updates includes so **zero** Markdown code is duplicated and all existing `ide-*` tests remain green. |

### 2.2 `src/viewer/` Module Layout

All files stay modular and strictly scoped (`< 1500` LOC each):

| File | Responsibility |
| :--- | :--- |
| `src/viewer/main.c` | `GtkApplication` (`org.kajo.Viewer`, `G_APPLICATION_HANDLES_OPEN`), CLI flags (`--preview-mode pdf\|md\|image` for `scripts/ui-shot.sh`, optional `--page N`, `--no-sidebar`), `kajo_app_init_theme()`, `ShellIcons` init. |
| `src/viewer/viewer_model.{h,c}` | **Pure GTK-free testable core:** file classification (`VIEWER_KIND_PDF`, `VIEWER_KIND_MARKDOWN`, `VIEWER_KIND_IMAGE`), sibling directory scan & natural collation sort (`g_utf8_collate_key_for_filename`), navigation history stack (Back/Forward), zoom clamping/fit calculation (`FIT_WIDTH`, `FIT_PAGE`, `FIT`, `FILL`, `CUSTOM`), search match bookkeeping. |
| `src/viewer/viewer_window.{h,c}` | Main frameless Aero + Metro `GtkApplicationWindow` (`.shell-settings-window.shell-viewer-window`): 34px `GtkWindowHandle` top bar, 36px contextual mode ribbon, opt-in inline `Ctrl+F` Find bar, `GtkPaned`/overlay 240px sidebar, mode `GtkStack` (`empty`, `pdf`, `markdown`, `image`), 26px telemetry status strip, Wayland file DnD target, and file-open chooser. |
| `src/viewer/viewer_sidebar.{h,c}` | Collapsible 240px left rail hosting PDF `OUTLINE`/`PAGES` switcher, Markdown Heading TOC, and Image EXIF/Info inspector. |
| `src/viewer/viewer_pdf.{h,c}` | Poppler document controller: continuous 1-page / 2-page layout metrics, viewport-gated async page render queue + LRU cache, text selection & clipboard copy, search across all pages via `poppler_page_find_text_with_options()`, internal/external link hit-testing, paper tone filter, rotation, encrypted PDF unlock prompt. |
| `src/viewer/viewer_markdown.{h,c}` | Markdown host around shared `KajoMarkdownView`: `GFileMonitor` debounced live reload with scroll preservation, relative-link interception (opening `.md`/`.pdf`/images inside `kajo-viewer`), reading width presets (`NARROW`/`COMFORT`/`FULL`), font scale, `RAW` monospace source view (`GtkTextView`), `[ ✎ OPEN IN IDE ]` (`kajo-ide` handoff). |
| `src/viewer/viewer_image.{h,c}` | High-resolution pan/zoom image canvas: `glycin-gtk4-2` loader (with standard GTK4 `gdk_texture_new_from_file` fallback), `librsvg-2.0` live SVG vector re-rasterization on zoom, `libexif` tag extraction, cursor-centered wheel/pinch zoom, drag pan, rotation/flip transforms, checkerboard backdrop toggle, map-gated animation playback, and collapsible bottom thumbnail strip. |
| `tests/test_viewer_model.c` | GTK-free unit tests (model classification, natural sibling ordering, Back/Forward history, zoom/fit math, PDF/MD/Image state transitions) runnable cleanly under ASan/UBSan. |

---

## 3. PDF Viewer Specification (`viewer_pdf.c`)

### 3.1 Rendering, Layout & Cache Pipeline
- **Layout Modes:** Segmented toggle `[ 1P ] [ 2P ]` (single-page continuous vertical scroll vs. two-page facing spread; page 1 may optionally stand alone on cover spreads).
- **Zoom Modes:** `FIT W` (fit viewport width), `FIT P` (fit full page height), `100%`, and manual zoom (`25%..400%` in multiplicative steps or `Ctrl+Wheel`).
- **Async Viewport-Gated Rendering:**
  - Page geometry (`poppler_page_get_size`) is queried upfront at load to build accurate layout heights with lightweight placeholders (almost zero memory).
  - On scroll/resize/zoom, a debounced pass identifies visible pages `± 2` buffer and queues `GTask` workers calling `kajo_pdf_render_page_scaled()` at `scale * scale_factor` (native HiDPI sharpness).
  - Stale renders (superseded by a new zoom/rotation or closed document) check `g_cancellable_is_cancelled()` before committing to the LRU cache.
  - LRU cache holds at most `12` rendered page textures cleanly; off-screen textures past capacity are unreffed immediately.
- **Paper Tone Filter (`PAPER` / `SEPIA` / `DARK`):**
  - **`PAPER`:** Crisp white `#ffffff` sheet (faithful print proofing).
  - **`SEPIA`:** Warm `#f4ecd8` tinted paper background for long daytime reading.
  - **`DARK`:** Luminance-inverted dark mode blended onto `#141414` (`1 - L` with hue preservation) so bright white PDFs match Aero + Metro night workspaces.

### 3.2 Navigation, Outline, Search, Selection & Encryption
- **Sidebar (`OUTLINE` | `PAGES`):**
  - `OUTLINE`: Populates `GtkTreeListModel` / `GtkListBox` from `PopplerIndexIter` (`poppler_index_iter_new()`). Clicking an entry resolves `PopplerActionGotoDest` and scrolls directly to target page + vertical offset.
  - `PAGES`: Lazy low-res thumbnail strip (`~180px` wide) rendered sequentially only for visible sidebar rows, highlighting the active page with a `3px` `@metro-accent` bar.
- **Inline `Ctrl+F` Find Bar (Opt-in):**
  - Never shown by default. Toggling `Ctrl+F` reveals a `34px` bar above the canvas; typing searches pages asynchronously with `POPPLER_FIND_CASE_SENSITIVE` toggle, displays `[ MATCH 3 / 14 ]`, draws translucent `@metro-accent` / `@metro-warning` highlight rectangles over matches on visible pages, and jumps on `Enter` / `F3` / `Shift+F3`.
- **Text Selection & Links:**
  - Dragging with pointer selects text via `poppler_page_get_selected_region()` / `poppler_page_get_selected_text()`, copies on `Ctrl+C`.
  - Hovering over a `PopplerLinkMapping` changes cursor to pointer; clicking internal `GOTO_DEST` jumps inside the document (pushing onto Back/Forward history), clicking `URI` launches via `g_app_info_launch_default_for_uri()` (`AGENTS.md §4.B`).
- **Encrypted PDF Unlock (`AGENTS.md §4.E`):**
  - If `poppler_document_new_from_gfile()` fails with `POPPLER_ERROR_ENCRYPTED`, displays an inline Aero + Metro unlock bar (`GtkPasswordEntry`, never a modal popup), clears the widget buffer immediately after copying to a temporary string, passes to `poppler_document_new_from_gfile(file, pass, ...)`, and calls `explicit_bzero()` on the temporary copy.

---

## 4. Markdown Viewer Specification (`viewer_markdown.c`)

- **Shared Engine:** Uses `KajoMarkdownView` (promoted from `ide_markdown*` + `ide_mermaid*`) for consistent visual rendering of headings, inline bold/italic/code/strikethrough, links, syntax-toned fenced code blocks, Markdown tables, horizontal rules, embedded images/PDFs/SVGs/carousels, and **native Cairo vector Mermaid diagrams** (`flowchart`, `sequenceDiagram`, `stateDiagram`, `classDiagram`, `erDiagram`, `xychart-beta`).
- **Auto-Generated Heading Outline Sidebar:** Parses `#`..`####` headings (skipping fenced code blocks) into a clickable indentation-leveled sidebar outline that tracks scroll position cleanly.
- **Live File Reload:** Watches the open `.md` file via `g_file_monitor_file()`; coalesced `150ms` debounce re-renders the view while preserving scroll fraction (`scroll_frac`) so edits in `kajo-ide` or a terminal update live.
- **In-Viewer Relative Navigation:** Clicking a relative link (`[spec](spec_kajo_comp.md#3.1)` or `./diagram.png` or `./report.pdf`) resolves against `doc_dir` and pushes the current file + scroll position onto the `ViewerNavHistory` stack before opening the target directly inside `kajo-viewer` (`Alt+←` / `Alt+→` to navigate back/forward). External `https://` links open via `g_app_info_launch_default_for_uri()`.
- **Reading Controls (Strictly Read-Only):**
  - **Width Segmented Group:** `[ NARROW (680) ]` `[ COMFORT (860) ]` `[ FULL ]`
  - **Typography Scale:** `[ A- ]` `100%` `[ A+ ]` (`80%..180%`, `Ctrl+=` / `Ctrl+-` / `Ctrl+0`)
  - **Mode Toggle:** `[ VIEW ]` / `[ RAW ]` (read-only monospace source preview for inspecting raw Markdown/frontmatter)
  - **IDE Handoff:** `[ ✎ OPEN IN IDE ]` launches `kajo-ide <file>` via shared `kajo_spawn_detached()` when editing is desired.

---

## 5. Image Viewer Specification (`viewer_image.c`)

### 5.1 Decoding Backends (Optional Dependencies with Graceful Fallbacks)

| Capability | Primary (when installed) | Compile-Time Fallback |
| :--- | :--- | :--- |
| **Wide Raster / Modern Codecs (`AVIF`, `HEIC`, `JXL`, `WebP`, `RAW`, `PNG`, `JPEG`, `GIF`)** | `glycin-2` + `glycin-gtk4-2` (`GlyLoader`, sandboxed out-of-process decode, EXIF auto-orientation, ICC profiles, animated frame stream) | `gdk_texture_new_from_file()` + `GdkPixbufAnimation` fallback |
| **Vector SVG Sharp Zoom (`SVG`, `SVGZ`)** | `librsvg-2.0` (`RsvgHandle` re-rasterized via Cairo directly at `zoom * scale_factor`, keeping edges razor-sharp even at `1600%`) | Texture upload via Glycin / GTK at base size |
| **Camera / EXIF Metadata Inspector** | `libexif` (`ExifData` parsing Make, Model, Lens, Focal Length, Aperture, Shutter, ISO, DateTime, GPS redacted/summary) | Basic file size, dimensions, bit depth, MIME info only |

### 5.2 Canvas & Browsing Features
- **Pan & Zoom-to-Cursor:** Pointer drag pans smoothly; `Ctrl+Wheel` / pinch gesture zooms toward the exact cursor focal point (`10%..1600%`). Quick presets `[ FIT ]`, `[ 1:1 ]`, `[ FILL ]`.
- **Transforms & Backdrop:** Non-destructive display rotate (`r`/`R`, `90°` CW/CCW), Flip H / Flip V, and Checkerboard alpha grid toggle (essential for transparent PNGs/SVGs/WebP stickers).
- **Map-Gated Animation Controls:** Animated GIF/WebP/APNG automatically shows `[ ⏸ PAUSE ]` / `[ ▶ PLAY ]` + `[ +1F ]` frame step controls and stops its frame tick timer immediately if the window is minimized/unmapped (`AGENTS.md §4.D`).
- **Sibling Folder Browsing & Filmstrip:** Scans the parent directory asynchronously on load using `viewer_model` natural collation order; `←` / `→` (or Space/Backspace) step across images in the folder; collapsible bottom `64px` thumbnail filmstrip (`F` key) lazy-decodes visible thumbnails only.
- **Clipboard Copy:** `Ctrl+C` copies the decoded `GdkTexture` cleanly to the Wayland clipboard via `gdk_clipboard_set_texture()`.

---

## 6. Aero + Metro UI Layout & Design System Alignment

Every widget adheres strictly to [`docs/design_system.md`](design_system.md): **`0px` corner radius everywhere**, crisp `1px solid @metro-border` hairlines, `@metro-surface-*` elevation ladder, `@metro-accent` active highlight, `Fira Code` monospace headers/badges/telemetry, and no dead UI (`AGENTS.md §8.1`).

```
┌──────────────────────────────────────────────────────────────────────────────────┐ 34px  Window Handle (GtkWindowHandle)
│ ▤ KAJO VIEWER │ architecture_spec.pdf │ [ PDF · 24P · 4.2 MB ] │ [📂] [🗗] [✕]   │
├──────────────────────────────────────────────────────────────────────────────────┤ 36px  Contextual Mode Ribbon
│ [◀][▶] │ [☰ OUTLINE] │ [-] 110% [+] [FIT W][FIT P][1:1] │ [1P][2P] [↻] │ [PAPER|SEPIA|DARK] │ [🔍] │
├──────────────────┬───────────────────────────────────────────────────────────────┤
│ 240px Sidebar    │ (Optional Ctrl+F Find Bar — hidden until toggled)             │
│ (Collapsible)    │ ┌───────────────────────────────────────────────────────────┐ │
│                  │ │ [🔍 Search document...]  [Aa]   3 / 14   [▲][▼]   [✕]     │ │
│ [OUTLINE|PAGES]  │ └───────────────────────────────────────────────────────────┘ │
│ ▸ 1. Overview  1 │                                                               │
│ ▾ 2. Effects   4 │                 #101010 Recessed Document Canvas               │
│   2.1 Glass    5 │          (Centered Page / Rendered Markdown / Image)          │
│ ┃ 2.2 Blur     8 │                                                               │
│ ▸ 3. HDR      14 │                                                               │
├──────────────────┴───────────────────────────────────────────────────────────────┤ 26px  Telemetry Strip
│ PAGE 8 / 24 │ 110% │ 1440×2036 px │ ZOOM FIT WIDTH │ ~/docs/architecture_spec.pdf│
└──────────────────────────────────────────────────────────────────────────────────┘
```

- **CSS Integration:** New partial `data/defaults/styles/55-viewer.css` imported from `data/defaults/style.css`, defining `.shell-viewer-window` rules solely through `@metro-*` tokens.
- **Gallery Integration (`AGENTS.md §3.5`, `§7`):** New `kajo-viewer` toolbar/sidebar/paper-tone/canvas specimen added to [`docs/ui-gallery.html`](ui-gallery.html) and documented in [`docs/design_system.md`](design_system.md) alongside implementation.

---

## 7. Configuration (`viewer.toml`), Shortcuts & Desktop Integration

### 7.1 Configuration Domain (`~/.config/kajo/viewer.toml`)

Managed through `KajoConfigStore` (`KAJO_CONFIG_DOMAIN_VIEWER`):

| Key | Type / Default | Purpose |
| :--- | :--- | :--- |
| `sidebar_visible` | `bool = true` | Initial state of the 240px Outline / Info sidebar. |
| `pdf_layout_mode` | `enum ("single" \| "spread") = "single"` | Default 1-page continuous vs. 2-page spread. |
| `pdf_paper_tone` | `enum ("paper" \| "sepia" \| "dark") = "paper"` | Default PDF sheet rendering tone. |
| `pdf_zoom_mode` | `enum ("fit_width" \| "fit_page" \| "actual") = "fit_width"` | Default PDF zoom fit preset. |
| `markdown_width` | `enum ("narrow" \| "comfort" \| "full") = "comfort"` | Markdown column width (`680px` / `860px` / `100%`). |
| `markdown_font_pct` | `int (80..180) = 100` | Markdown reading font scale percentage. |
| `markdown_live_reload` | `bool = true` | Auto-reload `.md` when modified on disk. |
| `image_checkerboard` | `bool = false` | Show alpha transparency checkerboard behind images. |
| `image_filmstrip` | `bool = false` | Show bottom thumbnail filmstrip when browsing an image folder. |

### 7.2 Keyboard Shortcuts

| Scope | Keybinding | Action |
| :--- | :--- | :--- |
| **Global** | `Ctrl+O` / `Ctrl+B` | Open file / Toggle left sidebar |
| **Global** | `Ctrl+F` / `F3` / `Shift+F3` / `Esc` | Toggle Find bar / Next / Previous / Close Find bar |
| **Global** | `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | Zoom In / Zoom Out / Reset (`100%` or `Fit`) |
| **Global** | `Alt+←` / `Alt+→` / `Ctrl+Q` | Navigate History Back / Forward / Quit window |
| **PDF** | `Ctrl+G` | Focus page number input in ribbon / status strip |
| **PDF** | `1` / `2` / `W` / `P` | 1-Page / 2-Page Spread / Fit Width / Fit Page |
| **PDF** | `r` / `Shift+R` / `D` | Rotate CW / CCW / Cycle `PAPER` → `SEPIA` → `DARK` |
| **PDF** | `Ctrl+C` | Copy selected PDF text |
| **Markdown** | `Ctrl+U` / `R` | Toggle Rendered vs. `RAW` Markdown source |
| **Image** | `←` / `→` / `F` | Previous / Next image in folder / Toggle filmstrip |
| **Image** | `r` / `Shift+R` / `H` / `V` / `T` | Rotate CW / CCW / Flip H / Flip V / Toggle checkerboard |
| **Image** | `Space` / `Ctrl+C` | Play-Pause animation (or next image if static) / Copy image texture |

### 7.3 Desktop Entry & `kajo-files` Integration
- Install `data/desktop/org.kajo.Viewer.desktop` with `MimeType=application/pdf;text/markdown;text/x-markdown;image/png;image/jpeg;image/webp;image/avif;image/heif;image/jxl;image/gif;image/svg+xml;image/bmp;image/tiff;` so `kajo-files` (`Open With` / `Set Default`), `kajo` app launcher, and `xdg-open` launch `kajo-viewer %U` cleanly.
- Add window rules entry for `org.kajo.Viewer` in `data/defaults/rules.toml` (clean tiling default, crisp accent border).

---

## 8. Phased Milestones, Verification & Honest Non-Goals for v1

### 8.1 Implementation Milestones
1. **M1 — Shared Extraction & App Skeleton:** Extract `kajo_markdown*` + `kajo_mermaid*` to `src/common/`, extend `pdf_render.{h,c}`, add `viewer_model` + `schema_viewer.c` + `test_viewer_model.c`, build `kajo-viewer` frameless Aero + Metro shell + `55-viewer.css` + `ui-gallery.html` specimen.
2. **M2 — PDF Viewer (`viewer_pdf.c`):** Async lazy page rendering + LRU cache, `1P`/`2P`, `PAPER`/`SEPIA`/`DARK`, Outline & Thumbnails sidebar, `Ctrl+F` search + highlights, text selection/copy, links, rotate, encrypted unlock.
3. **M3 — Markdown Viewer (`viewer_markdown.c`):** Shared Markdown + Cairo Mermaid view, Heading Outline, live `GFileMonitor` reload, Back/Forward relative-link navigation, width/scale/`RAW`/checklist lock + `[ ✎ OPEN IN IDE ]`.
4. **M4 — Image Viewer (`viewer_image.c`):** `glycin-gtk4-2` + `librsvg` + `libexif` (optional deps with fallback), pan & zoom-to-cursor, sibling prev/next + filmstrip, rotate/flip, checkerboard, animation controls, EXIF sidebar, clipboard copy.
5. **M5 — Desktop Integration & Full Verification:** `.desktop` file + `docs/` sync + `meson compile -C builddir && meson test -C builddir` + ASan/UBSan + `scripts/ui-shot.sh` headless screenshots of PDF, Markdown, and Image modes compared against `docs/ui-gallery.html` and [`docs/kajo-viewer-mockup.html`](kajo-viewer-mockup.html).

### 8.2 Explicit v1 Non-Goals (`AGENTS.md §8`)
- No destructive PDF editing or annotation authoring (highlights/signatures/redactions) in v1.
- No destructive image editing (crop/resize/save-back) — rotations and flips are display transforms only.
- No EPUB / DjVu / PostScript (`ddjvuapi` / `libspectre`) backends in v1; if added later they belong to a subsequent phase and will not be advertised until implemented and tested.
