# Specification: `kajo-web` — Minimalist Aero + Metro Wayland Web Browser (`docs/spec_kajo_web.md`)

**Status:** Draft Design Specification — branch `feat/kajo-web`  
**Target Binary:** `kajo-web` (`org.kajo.Web`, `src/web/`)  
**Settings Integration:** `kajo-settings` Page 13 (`src/settings/pages/page_web.c`), `KajoConfigStore` domain `web.toml` (`src/config/schemas/schema_web.c`)  
**Companion Visual Prototypes:** [`docs/kajo-web-mockup.html`](kajo-web-mockup.html) • [`docs/kajo-web-storyboard.html`](kajo-web-storyboard.html)  
**Core Dependencies:** `webkitgtk-6.0` (`>= 2.50`, verified `2.54.3` on Host A), `gtk4`, `libsoup-3.0`, `sqlite3`, `json-glib-1.0`, `KajoConfigStore` (`src/config/`)

---

## 1. Objectives & Design Philosophy

`kajo-web` is a lightweight, native **C17 + GTK4 + WebKitGTK 6.0** web browser engineered to fit the Kajo desktop suite (`kajo-comp`, `kajo`, `kajo-ide`, `kajo-files`, `kajo-settings`) with zero Electron/Chromium bloat and uncompromising visual consistency:

1. **Pure Aero + Metro Visual Identity (`docs/design_system.md`):**
   - Frameless GTK4 window (`gtk_window_set_decorated(win, FALSE)`, `.shell-settings-window.shell-web-window`), strict `0px` corner radius, OLED-native charcoal surfaces (`#101010`–`#1a1a1a`), `1px solid #2b2b2b` structural hairlines, `1px solid @metro-accent` active frame, and `Fira Code` monospace controls and telemetry.
   - Instant live hot-reload of `@metro-accent`, surface tokens, and typography from `theme.toml` / `~/.config/kajo/style.d/00-accent.css`.
2. **Vertical, Autohide-First Tab Rail (Zero Page Reflow):**
   - Tabs live in a **vertical rail** (left edge by default, configurable to right) that **autohides by default** into a `4px` edge trigger strip (`rgba(0, 0, 0, 0.01)` — never `opacity: 0`), sliding over the page as a `240px` Aero + Metro overlay on pointer hover or keyboard focus (`Ctrl+B` / `Ctrl+Tab`) without reflowing the web viewport.
   - Supports three first-class modes configured in `kajo-settings` and toggled live: **`autohide`** (overlay), **`compact`** (`44px` favicon rail that expands on hover), and **`pinned`** (`240px` docked split).
3. **Minimalist Chrome, Maximum Viewport:**
   - One compact `34px` top `GtkWindowHandle` strip combining back/forward, reload/stop, rail-pin button, a unified minimalist Omnibar (domain pill + TLS shield + ad-block counter when unfocused; full URL/search entry on `Ctrl+L`), contextual status badges (only visible when active: Downloads, Mute, Zoom != 100%, Private, Software-Render fallback), `⚙` (`kajo-settings --page web`), `🗗 FLOAT / TILE`, and `✕`.
   - Optional **Chrome Autohide / Zen Mode** (`Ctrl+Shift+U` / `F11`) collapses the top bar too, dedicating 100% of the tile to page content.
4. **Hardware Acceleration Enabled by Default:**
   - Built on **WebKitGTK 6.0 (`2.54+`)** with **Skia 2D + Skia web-process compositing** and **zero-copy Wayland DMA-BUF** presentation (`WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS` by default), WebGL GPU process isolation, smooth scrolling, and GStreamer VA-API/NVDEC media decoding when plugins are installed.
   - Explicit fallback path (`hardware_acceleration = "never"` / `WEBKIT_DISABLE_DMABUF_RENDERER=1`) always surfaces a visible `[ ⚠ SW RENDER ]` indicator in the top chrome and `kajo://gpu` (never silent).
5. **Strong Configurability via `kajo-settings` (`web.toml`):**
   - Keeps the browser window clean by moving deep configuration into a dedicated **`13 / 13 • KAJO WEB`** page in **`kajo-settings`** backed by `~/.config/kajo/web.toml` (plus `conf.d/*.toml` drop-ins), hot-reloaded across all open `kajo-web` windows via `KajoConfigStore` (`GFileMonitor`).
6. **First-Class Native Suite & Portal Integration:**
   - Delegates `<input type="file">` (`WebKitFileChooserRequest`) to Kajo's native `kajo-files` FileChooser portal (`org.freedesktop.impl.portal.FileChooser`).
   - Synchronizes `prefers-color-scheme`, `prefers-contrast`, and `prefers-reduced-motion` with `kajo`'s `org.freedesktop.impl.portal.Settings` backend.
   - Uses `shell_compositor_toggle_floating()` for `🗗 FLOAT / TILE` across both `kajo-comp` and Niri.

---

## 2. Scope, Non-Goals & Honest Engine Constraints (`WebKitGTK 6.0`)

Per Kajo's engineering honesty rules (`AGENTS.md` §8), `kajo-web` documents WebKitGTK 6.0's real capabilities and trade-offs up front:

| Capability | Status in `kajo-web` | Technical Basis / Trade-Off |
| :--- | :--- | :--- |
| **Modern Web / JS / CSS / WASM** | Full (WebKit / JavaScriptCore) | Matches Safari/WebKitGTK `2.54+` (Skia renderer, linear text blending, IndexedDB, Service Workers, ES2025+). |
| **Wayland Zero-Copy GPU Acceleration** | Default ON (`ALWAYS`) | Skia + GBM DMA-BUF + damage propagation (`WebKitGTK 2.50+`), WebGL GPU process (`UseGPUProcessForWebGL`). |
| **Native Ad & Tracker Blocking** | Built-in (`WebKitUserContentFilterStore`) | Compiles Safari-format JSON rulepacks to native bytecode filters inside WebKit—fast, zero JS extension overhead. |
| **User Scripts & User CSS** | Built-in (`WebKitUserContentManager`) | Per-origin or global CSS/JS injection in isolated `WebKitScriptWorld`s. |
| **Mandatory Process Sandboxing** | Always ON | WebKitGTK 6.0 enforces Bubblewrap + `xdg-dbus-proxy` + `seccomp` (cannot be disabled in API). |
| **DevTools / Web Inspector** | Built-in (`WebKitWebInspector`) | Dockable bottom/right or detached WebKit Inspector (`F12` / `Ctrl+Shift+I`). |
| **Widevine Commercial DRM** | **Not supported** | WebKitGTK ships W3C ClearKey EME only; Netflix/Disney+/Spotify full-DRM streams require Chromium/Firefox. |
| **Chrome / Firefox Browser Extensions** | **Not supported** | `WebKitWebExtension` in 2.54 only parses manifests/metadata—not a full MV2/MV3 extension runtime. Native content blocking + user scripts cover ad-blocking and tweaks instead. |
| **Video/Audio Decoding** | GStreamer (`gst-plugins-*` + VA-API) | Hardware decode quality depends on installed GStreamer plugins (`gst-libav`, `gst-plugins-good/bad`, `gstreamer-vaapi`). |

---

## 3. Window Chrome & Vertical Autohide Tab Rail

### 3.1 Spatial Layout

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│ [🌐] [◀][▶][↻] [◧] │ [🔒 docs.kajo.local/spec  🛡14] │ [↓2] [⚙] [🗗] [✕]   │  34px Top Chrome
├────┬─────────────────────────────────────────────────────────────────────────┤
│ ▌  │                                                                         │
│ ▌  │                                                                         │
│ ▌  │                  WebKitWebView (100% viewport width)                    │
│ ▌  │                  (Autohide rail slides OVER content —                   │
│ ▌  │                   zero DOM layout reflow on hover)                      │
│ ▌  │                                                                         │
│ ▌  │                                       ┌──────────────────────────────┐  │
│ ▌  │                                       │ FIND: webkit   3/17 [▲][▼][✕]│  │  Optional Find Bar
└────┴───────────────────────────────────────┴──────────────────────────────┘
  ▲
  └─ 4px Trigger Strip (rgba(0,0,0,0.01)) + 2px tab-position ticks in @metro-accent
```

When the pointer enters the `4px` trigger strip (or `Ctrl+B` / `Ctrl+Tab` reveals the rail), a `240px` Aero + Metro rail slides over the viewport edge (`GtkOverlay` + smooth `120ms` ease-out transform):

```text
┌──────────────────────────────┬───────────────────────────────────────────────┐
│ PINNED                       │                                               │
│ [GH][AW][MD][KJ][+]          │                                               │
├──────────────────────────────┤                                               │
│ ▾ WORK (3)                   │                                               │
│ ┃🌐 Kajo — GitHub        [✕] │                                               │
│ │📘 Wayland — Arch Wiki   🔊 │         WebKitWebView (dimmed 0% by default;  │
│ │🧪 kajo://gpu telemetry     │          no resize, no layout thrash)         │
├──────────────────────────────┤                                               │
│ ▾ RESEARCH (2)               │                                               │
│ │📄 WebGL2 — MDN             │                                               │
│ │💤 Old Spec (Suspended)     │                                               │
├──────────────────────────────┤                                               │
│ [+ NEW TAB]  [🔍 CTRL+K]  [◧]│                                               │
└──────────────────────────────┴───────────────────────────────────────────────┘
```

### 3.2 Vertical Rail Modes (`rail_mode` in `web.toml`)

| Mode | Resting Width | Expanded Width | Page Reflow? | Behavior |
| :--- | :---: | :---: | :---: | :--- |
| **`autohide`** *(Default)* | `4px` edge trigger strip (`rgba(0,0,0,0.01)`) | `240px` (`rail_width_px`) | **No** (overlay) | Subtle `2px` edge ticks mark open tabs & active tab. Slides over viewport after `rail_reveal_delay_ms` (`80ms`); retracts `rail_hide_delay_ms` (`220ms`) after pointer leaves. |
| **`compact`** | `44px` docked favicon rail | `240px` (`rail_width_px`) | **No** (expands as overlay) | Always displays crisp favicons + `3px` active accent bar + audio/spinner badges; expands over viewport on hover to reveal full titles, groups, and close buttons. |
| **`pinned`** | `240px` (`rail_width_px`) | `240px` | **Yes** (docked split) | Permanently docked sidebar pushing the `WebKitWebView` width via `GtkPaned` / `GtkBox`. |

### 3.3 Autohide Rail State Machine (Real Timers, Never Lazy)

Every transition is driven by explicit `GLib` timers cancelled on dispose (matching `src/autohide.c` & `AGENTS.md` §8 rule 3):

```text
                    pointer enters trigger strip
   [ COLLAPSED ] ─────────────────────────────────► [ REVEAL_WAIT ]
         ▲                                                │
         │ hide_timer fires                     reveal_timer fires (80ms)
         │ (220ms, no hold-open)                or keyboard Ctrl+B / Ctrl+Tab
         │                                                ▼
   [ HIDE_WAIT ] ◄────────────────────────────────── [ EXPANDED ]
         │          pointer leaves rail + trigger         │
         │          (and hold_count == 0)                 │ hold_count > 0:
         └────────── pointer re-enters rail ──────────────┘ (context menu, drag,
                     (cancels hide_timer)                    rename, tab filter)
```

**Hold-open invariants:**
- Active tab drag-reorder, open tab context menu, inline group rename entry, or focused tab filter entry increments `hold_count`, preventing premature collapse.
- Window focus loss (`is-active == FALSE`) cancels pending reveal timers and collapses `EXPANDED` $\rightarrow$ `COLLAPSED` once popovers close.
- Pressing `Escape` while the rail has keyboard focus immediately collapses it and returns focus to the active `WebKitWebView`.
- **Wayland trigger surface rule (`AGENTS.md` §4D):** The `4px` collapsed trigger strip uses `background-color: rgba(0, 0, 0, 0.01)` — **never `opacity: 0`** — so GTK4/Wayland always delivers pointer enter/motion events.

### 3.4 Top Minimalist Control Bar (`34px`)

- **Left Cluster:**
  - Compact app icon (`🌐` / `kajo-web.svg`), Back (`◀`), Forward (`▶`), Reload/Stop (`↻`/`✕`), Rail Pin/Mode quick toggle (`◧`).
- **Center Unified Omnibar (`flex: 1`, centered max-width `760px`):**
  - **Resting (unfocused):** Single flat `26px` monospace bar showing TLS status (`🔒` green HTTPS / `⚠` orange HTTP / `❖` cyan `kajo://`), bold host (`github.com`) + muted path (`/mikolas/Kajo`), inline ad-block counter pill (`[ 🛡 14 ]` clickable for per-site whitelist toggle), and a `2px` bottom loading progress hairline (`@metro-accent`).
  - **Focused (`Ctrl+L` or click):** Expands to full editable URL/search input with instant dropdown suggestions (open tabs `[ TAB ]`, bookmarks `[ ★ ]`, history `[ ↺ ]`, search engine / bang expansion `[ 🔍 DDG ]`).
- **Right Cluster (Minimalist — Conditional Badges Only):**
  - `[ ⚠ SW RENDER ]` (amber warning — **only** when hardware acceleration is disabled or fell back).
  - `[ 🕶 PRIVATE ]` (magenta — **only** in ephemeral private windows).
  - `[ 125% ]` (zoom reset button — **only** when zoom `!= 100%`).
  - `[ ↓ 2 ]` (downloads progress badge — **only** when downloads are active or completed this session).
  - `[ ⚙ ]` (launches `kajo-settings --page web`), `[ 🗗 ]` (`shell_compositor_toggle_floating()`), `[ ✕ ]` (`.settings-close-btn`, `#e81123` hover).
- **Zero Chrome Mode (`Ctrl+Shift+U` / `F11`):** Hides the `34px` top bar into a `2px` top hover strip (`rgba(0,0,0,0.01)`), leaving only the web canvas.

### 3.5 Tab Features & Lifecycle

- **Pinned Tabs:** Compact `40×34px` square favicon tiles at top of the rail (5 per row), never auto-suspended, cannot be accidentally closed with middle-click (requires context menu Unpin or explicit `Close`).
- **Tab Groups:** Lightweight named folders (`WORK`, `DOCS`, `MEDIA`, etc.) with a `3px` semantic accent bar (`@metro-accent`, `@metro-telemetry`, `@metro-success`, `@metro-warning`, `@metro-magenta`), collapsible with tab count badge.
- **Audio & Media Indicators:** Tabs playing audio display a live clickable `🔊` / `🔇` button wired to `webkit_web_view_set_is_muted()`.
- **Idle Tab Suspension (`suspend_idle_min`):**
  - Background tabs idle beyond the configured threshold (default `30` min; `0` = disabled) serialize state via `webkit_web_view_get_session_state()`, destroy their backing `WebKitWebView` (freeing the WebKitWebProcess + Skia GPU textures), and show a dimmed `💤` indicator.
  - Never suspends: active tab, pinned tabs, audible tabs, tabs with active downloads, or tabs with active WebRTC/capture permissions.
  - Clicking a suspended tab recreates its `WebKitWebView`, restores `WebKitWebViewSessionState`, and reloads smoothly.
- **Session Restore & Private Windows (`Ctrl+Shift+N`):**
  - Standard windows share a persistent `WebKitNetworkSession` (`~/.local/share/kajo/web/` data, `~/.cache/kajo/web/` cache) and persist open tabs/groups/pinned state atomically to `~/.local/state/kajo/web-session.json` on clean exit and debounced tab changes.
  - Private windows create `webkit_network_session_new_ephemeral()` — zero disk cookies, zero history writes, zero session persistence.

---

## 4. Hardware Acceleration & Performance Architecture

### 4.1 GPU Rendering Pipeline

`kajo-web` configures WebKitGTK 6.0 explicitly on startup **before** creating any `WebKitWebView`:

| `web.toml` `hardware_acceleration` | WebKitGTK 6.0 Policy | Env / Feature Flags | UI Telemetry Badge |
| :--- | :--- | :--- | :--- |
| **`"always"`** *(Default)* | `WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS` | DMA-BUF enabled; `enable-webgl = web_enable_webgl`; `UseGPUProcessForWebGL` enabled when `web_gpu_process_webgl = true`; `enable-smooth-scrolling` enabled | Clean (shows `SKIA + DMABUF` in `kajo://gpu` & `kajo-settings`) |
| **`"on-demand"`** | `WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND` | Hardware compositing engaged on GPU-heavy layers (WebGL/CSS3D/video) | Clean (shows `SKIA ON-DEMAND` in `kajo://gpu`) |
| **`"never"`** | `WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER` | Sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` before first `WebKitWebContext` init (requires window/app restart if toggled live; settings banner notes restart needed) | Visible amber `[ ⚠ SW RENDER ]` badge in top bar |

### 4.2 Process & Memory Discipline (`WebKitMemoryPressureSettings`)

- Configures `WebKitMemoryPressureSettings` on the `WebKitNetworkSession` / `WebKitWebContext` from `web.toml` (`memory_limit_mb`, default `2048` MB):
  - `conservative_threshold = 0.60`
  - `strict_threshold = 0.85`
  - `kill_threshold = 1.0` (only terminates non-visible / runaway background web processes; crashed tabs show a clean Aero + Metro `kajo://crashed` recovery card with `[ ↻ RELOAD TAB ]`, never taking down the browser window).
- **Web Process Crash Isolation:** Listening to `web-process-terminated` displays an inline crash banner with exact reason (`CRASHED` / `EXCEEDED MEMORY LIMIT` / `TERMINATED BY API`) and a one-click reload button.
- **Map-Gated UI Timers:** Zero background polling timers in the UI process. Loading progress, favicon updates, title changes, and `GFileMonitor` config reloads are 100% signal/event-driven so idle UI CPU stays `< 0.05%`.

---

## 5. Configuration Schema (`~/.config/kajo/web.toml`) & `kajo-settings` Integration

### 5.1 Domain Schema (`src/config/schemas/schema_web.c`, `data/defaults/web.toml`)

Follows Kajo's flat TOML 1.1 conventions (`docs/spec_toml_config.md`) with full `conf.d/*.toml` overlay support and comment-preserving patching:

```toml
# web.toml — Kajo Web Browser Defaults (~/.config/kajo/web.toml)

# === Vertical Tab Rail & Minimalist Chrome ===
rail_mode                 = "autohide"      # "autohide" | "compact" | "pinned"
rail_side                 = "left"          # "left" | "right"
rail_width_px             = 240             # 180..360
rail_reveal_delay_ms      = 80              # 0..300
rail_hide_delay_ms        = 220             # 50..1000
top_bar_mode              = "always"        # "always" | "autohide"
show_tab_domains          = true
new_tab_page              = "dashboard"     # "dashboard" | "blank" | "custom"
new_tab_custom_url        = ""
restore_session           = true

# === Hardware Acceleration & Performance ===
hardware_acceleration     = "always"        # "always" | "on-demand" | "never"
enable_webgl              = true
gpu_process_webgl         = true
smooth_scrolling          = true
suspend_idle_min          = 30              # 0 (off) | 15 | 30 | 60 | 120
memory_limit_mb           = 2048            # 512..16384

# === Privacy, Content Blocking & Media ===
adblock_enabled           = true
adblock_custom_rules_path = ""              # optional path to Safari JSON content filter
itp_enabled               = true            # Intelligent Tracking Prevention
cookie_policy             = "no-third-party" # "always" | "no-third-party" | "never"
https_only                = true
autoplay_policy           = "allow-without-sound" # "allow" | "allow-without-sound" | "deny"
enable_webrtc             = true
dns_prefetch              = true

# === Search & Navigation ===
search_engine             = "duckduckgo"    # "duckduckgo" | "kagi" | "startpage" | "brave" | "google" | "custom"
search_custom_url         = ""              # e.g. "https://kagi.com/search?q=%s"
default_zoom_pct          = 100             # 50..200
user_agent_preset         = "default"       # "default" | "safari" | "chrome-linux" | "custom"
user_agent_custom         = ""

# === Appearance, Fonts & Developer Tools ===
color_scheme              = "follow-kajo"   # "follow-kajo" | "dark" | "light"
default_font_size_px      = 16              # 10..32
mono_font_size_px         = 13              # 10..28
min_font_size_px          = 9               # 0..24
download_dir              = ""              # "" = XDG_DOWNLOAD_DIR
ask_download_location     = false
developer_extras          = true            # enables F12 WebKitWebInspector
```

### 5.2 `kajo-settings` Page (`src/settings/pages/page_web.c` — `13 / 13 • KAJO WEB`)

Follows `docs/design_system.md` §4.3 control-selection rules (Toggle Cards for features with status/sub-settings, Segmented Groups for 2–4 exclusive choices, Switch Rows for plain booleans, explicit **SAVE & APPLY** + **REVERT** footer):

1. **Card 1 — Vertical Tab Rail & Chrome:**
   - Rail Mode segmented group (`AUTOHIDE` | `COMPACT` | `PINNED`)
   - Rail Side segmented group (`LEFT` | `RIGHT`)
   - Top Bar Mode segmented group (`ALWAYS` | `AUTOHIDE`)
   - Rail Width slider (`180–360 px`)
   - Reveal Delay / Hide Delay sliders (`0–300 ms` / `50–1000 ms`)
   - New Tab Page segmented group (`DASHBOARD` | `BLANK` | `CUSTOM`) + URL entry
   - Restore Previous Session switch row
2. **Card 2 — Hardware Acceleration & Resource Limits:**
   - Live telemetry banner (`ENGINE: WEBKITGTK 2.54 • RENDERER: SKIA + DMABUF • SANDBOX: BWRAP`)
   - Hardware Acceleration segmented group (`ALWAYS` | `ON-DEMAND` | `NEVER`) with inline note when switching to/from `NEVER` (requires browser restart)
   - Toggle Cards for **WebGL + GPU Process Isolation** and **Smooth Scrolling**
   - Idle Tab Suspension segmented group (`OFF` | `15M` | `30M` | `60M`)
   - Memory Limit slider (`512–16384 MB`)
3. **Card 3 — Privacy, Ad-Blocking & Media:**
   - **Native Content Blocker** Toggle Card (status line: `ACTIVE • 42,180 COMPILED RULES` + custom JSON filter file picker + `[ ↻ RECOMPILE FILTERS ]`)
   - Switch rows: **Intelligent Tracking Prevention (ITP)**, **HTTPS-Only Mode**, **WebRTC**
   - Cookie Policy segmented group (`BLOCK 3P` | `BLOCK ALL` | `ALLOW`)
   - Autoplay Policy segmented group (`MUTE ONLY` | `DENY ALL` | `ALLOW`)
   - Danger action row: `[ 🗑 CLEAR CACHE & COOKIES ]` (with inline confirmation, no modal dialog)
4. **Card 4 — Search, Zoom & User-Agent:**
   - Default Search Engine dropdown (`DuckDuckGo`, `Kagi`, `Startpage`, `Brave`, `Google`, `Custom`) + Custom Search URL entry + quick bang reference badge (`!g !gh !arch !mdn !w`)
   - Default Page Zoom slider (`50%–200%`)
   - User-Agent preset segmented group (`DEFAULT` | `SAFARI` | `CHROME` | `CUSTOM`) + custom string entry
5. **Card 5 — Appearance, Downloads & Developer Tools:**
   - Color Scheme segmented group (`FOLLOW KAJO` | `DARK` | `LIGHT`)
   - Font size controls (Default, Monospace, Minimum px)
   - Download Directory row (with `[ 📁 CHOOSE ]` via native portal) + Ask Before Saving switch
   - Enable Web Inspector (`F12`) switch row

---

## 6. Keyboard Ergonomics & Command Palette

All shortcuts work without touching the mouse and never conflict with `kajo-comp` `Mod+*` bindings:

| Shortcut | Action |
| :--- | :--- |
| `Ctrl+T` / `Ctrl+W` / `Ctrl+Shift+T` | New tab / Close tab / Reopen last closed tab |
| `Ctrl+B` | Toggle vertical tab rail (`autohide` $\leftrightarrow$ `pinned`, or reveal if hidden) |
| `Ctrl+L` / `Alt+D` | Focus & select unified Omnibar |
| `Ctrl+K` | Open **Quick Command & Tab Switcher Palette** (fuzzy search open tabs, closed tabs, bookmarks, history, actions) |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | MRU / vertical next & previous tab (temporarily peeks rail while `Ctrl` held) |
| `Alt+1` .. `Alt+9` | Jump to pinned/indexed tab `1..9` |
| `Alt+Left` / `Alt+Right` | History Back / Forward |
| `Ctrl+R` / `Ctrl+Shift+R` | Reload / Hard reload bypassing cache |
| `Ctrl+F` / `Esc` | Open / Close inline bottom-right Find Bar |
| `Ctrl+Shift+U` / `F11` | Toggle Zero-Chrome Zen Mode / Fullscreen |
| `Ctrl+Shift+N` | Open Ephemeral Private Window |
| `Ctrl+J` | Toggle Downloads Popover |
| `Ctrl+D` | Bookmark current page (inline popover with folder/tag) |
| `Ctrl++` / `Ctrl+-` / `Ctrl+0` | Zoom In / Out / Reset `100%` |
| `Ctrl+M` | Mute / Unmute active tab |
| `F12` / `Ctrl+Shift+I` | Toggle docked WebKit Web Inspector |
| `Ctrl+,` | Launch `kajo-settings --page web` |

---

## 7. Built-In `kajo://` Internal Pages & Persistence

Registered via `webkit_web_context_register_uri_scheme(ctx, "kajo", ...)`, rendered in pure Aero + Metro HTML/CSS synced with the active `@metro-accent`:

- **`kajo://newtab`:** Minimalist speed-dial grid (pinned bookmarks, recently visited tiles, quick search bar, and a compact status footer showing active GPU mode + ad-blocker rule count).
- **`kajo://gpu`:** Live hardware acceleration diagnostics (WebKitGTK version, hardware acceleration policy, DMA-BUF status, WebGL / GPU process status, memory pressure thresholds, active process count).
- **`kajo://history` & `kajo://bookmarks`:** Fast SQLite-backed searchable lists (`~/.local/share/kajo/web/history.db`, WAL mode, FTS5 full-text search) with instant delete/filter.
- **`kajo://downloads`:** Full session & historical download log with "Show in Kajo Files" (`org.freedesktop.FileManager1` / `kajo-files`).

---

## 8. Modular C17 Architecture (`src/web/`)

Strict adherence to `AGENTS.md` §5 (`< 1500` lines per file, `< 150` lines per function, pure GTK-free testable core):

| Module | Responsibility |
| :--- | :--- |
| `src/web/web_model.{h,c}` | **Pure GTK-free core** (testable without Wayland/WebKit): tab tree, pinned/group ordering, MRU stack, closed-tab undo stack, omnibar URL vs. search/bang classifier (`!gh`, `!arch`, etc.), autohide rail FSM math, idle suspension selector. |
| `src/web/main.c` | `GtkApplication` (`org.kajo.Web`), `kajo_app_init_theme()`, CLI flags (`[URL...]`, `--private`, `--no-hw-accel`), single-instance URL forwarding (`G_APPLICATION_HANDLES_OPEN`). |
| `src/web/web_window.{h,c}` & `web_window_private.h` | Frameless Aero + Metro window (`KajoWebWindow`), `34px` header bar, `GtkOverlay` staging for autohide rail, Find Bar, Permission Infobar, Downloads popover, Web Inspector dock, and `KajoConfigStore` live reload subscriber. |
| `src/web/web_tab_rail.{h,c}` | Vertical tab rail widget: `4px` edge trigger strip (`rgba(0,0,0,0.01)`), real reveal/hide timers, `autohide`/`compact`/`pinned` layout modes, pinned grid, collapsible groups, drag-to-reorder (`GtkDragSource`/`GtkDropTarget`), mute & close buttons. |
| `src/web/web_tab.{h,c}` | `KajoWebTab` lifecycle wrapping `WebKitWebView`: hardware-acceleration settings application, `WebKitWebViewSessionState` suspension/wake, crash recovery UI, per-site policies, context menu, and `WebKitFileChooserRequest` portal bridge. |
| `src/web/web_omnibar.{h,c}` | Minimalist resting domain pill + TLS/adblock badges $\leftrightarrow$ focused editable URL/search bar with async autocomplete dropdown. |
| `src/web/web_palette.{h,c}` | `Ctrl+K` fuzzy command, open-tab, closed-tab, bookmark, and history modal switcher. |
| `src/web/web_adblock.{h,c}` | `WebKitUserContentFilterStore` async rulepack compiler, per-domain exception list, blocked-request counter bridge (`WebKitUserMessage`). |
| `src/web/web_storage.{h,c}` | Async SQLite (`history.db`, FTS5) bookmarks/history store + atomic `web-session.json` save/restore. |
| `src/web/web_downloads.{h,c}` | `WebKitDownload` tracker, progress badges, Aero + Metro downloads popover, and `kajo-files` reveal integration. |
| `src/web/web_schemes.{h,c}` | Custom `kajo://` URI scheme handler (`newtab`, `gpu`, `history`, `bookmarks`, `downloads`) styled with live `@metro-accent`. |
| `src/settings/pages/page_web.c` | `kajo-settings` Page 13 (`13 / 13 • KAJO WEB`) with live GPU telemetry readout, 5 configuration cards, and `SAVE & APPLY` / `REVERT`. |
| `data/defaults/styles/80-web.css` | Scoped Aero + Metro CSS partial imported by `data/defaults/style.css`. |

---

## 9. Verification & Testing Plan

1. **Pure Core Unit Suite (`tests/test_web_model.c`):**
   - Tab add/close/reorder/pin/group invariants, MRU ordering, LIFO undo-close stack.
   - Omnibar classifier (`localhost:8080`, `kajo://gpu`, `https://...`, bare hostnames, `!gh`/`!arch` bangs, plain search queries).
   - Autohide Rail FSM transitions: trigger enter $\rightarrow$ reveal timer $\rightarrow$ expanded $\rightarrow$ leave $\rightarrow$ hide timer $\rightarrow$ collapsed, plus hold-open cancellation, re-enter cancellation, focus-loss collapse, and Escape dismissal (both immediate and "wait, then input" timing tests).
   - Idle suspension eligibility (verifies pinned, audible, active, and capture-active tabs are never suspended).
2. **Config Store Parity (`tests/test_config.c`):**
   - Add `web.toml` to `config-roundtrip` and `fuzz-toml-config` verifying compiled-vs-shipped default parity, value clamping, `conf.d` overrides, and comment-preserving patching.
3. **Sanitizer Gate (`scripts/asan-test.sh`):**
   - Run `test-web-model` and `test-config` under ASan + UBSan with zero leaks or UAFs.
4. **Headless Visual Verification (`scripts/ui-shot.sh`):**
   - Render `kajo-web kajo://newtab` (autohide collapsed, autohide expanded, compact rail, pinned rail), `kajo-web kajo://gpu`, and `kajo-settings --page web`, comparing against [`docs/kajo-web-mockup.html`](kajo-web-mockup.html) and [`docs/ui-gallery.html`](ui-gallery.html).
5. **Live Hardware Verification (Host A — Intel Lunar Lake Xe2 + `kajo-comp`):**
   - Verify zero-copy DMA-BUF presentation (`SKIA + DMABUF`), 60/120 FPS smooth scrolling, WebGL rendering, VA-API video playback, `kajo-files` FileChooser portal trigger on `<input type="file">`, and `kajo-settings --page web` live hot-reload.

---

## 10. Phased Implementation Plan

| Phase | Scope | Exit Gate |
| :--- | :--- | :--- |
| **Phase 1 — Core, Config & Skeleton** | `web.toml` schema (`schema_web.c`), `web_model.{h,c}` + `test_web_model.c`, `kajo-web` frameless window + `WebKitWebView` with hardware acceleration (`ALWAYS`) + minimalist top bar + `kajo://gpu`. | `meson compile && meson test` pass (0 warnings), `kajo-web` renders live web pages with Skia + DMA-BUF. |
| **Phase 2 — Vertical Autohide Tab Rail** | `web_tab_rail.{h,c}` (`autohide` / `compact` / `pinned`, left/right, `4px` trigger strip, real reveal/hide timers, pinned tiles, groups, mute/close, drag-reorder, `Ctrl+B`, `Ctrl+Shift+T`). | Rail FSM tests + headless visual captures for all 3 rail modes. |
| **Phase 3 — `kajo-settings` Page 13 & Live Reload** | `src/settings/pages/page_web.c` (all 5 cards, `SAVE & APPLY` / `REVERT`) + `GFileMonitor` hot-reload in `kajo-web`. | Changing rail mode, side, search engine, ad-blocker, or zoom in `kajo-settings` updates open `kajo-web` windows immediately. |
| **Phase 4 — Omnibar, Palette, Storage & Ad-Blocker** | `web_omnibar.{h,c}`, `web_palette.{h,c}` (`Ctrl+K`), `web_storage.{h,c}` (SQLite FTS5 + session restore), `web_adblock.{h,c}` (`WebKitUserContentFilterStore` + bundled rules), `kajo://newtab` / `history` / `bookmarks`. | Unit + ASan tests pass; native content filter compiles & blocks ads with counter badge. |
| **Phase 5 — Overlays, Portals, Suspension & Polish** | Inline Find Bar (`Ctrl+F`), Permission Infobar, Downloads Popover, idle tab suspension (`WebKitWebViewSessionState`), `WebKitFileChooserRequest` portal bridge, docked Web Inspector (`F12`), gallery & docs sync. | Full build, test, ASan, `ui-shot.sh`, and live Host A verification complete. |
