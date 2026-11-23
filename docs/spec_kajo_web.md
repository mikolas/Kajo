# Specification: `kajo-web` — Minimalist Aero + Metro Wayland Web Browser (`docs/spec_kajo_web.md`)

**Status:** Draft Design Specification — branch `feat/kajo-web`  
**Target Binary:** `kajo-web` (`org.kajo.Web`, `src/web/`)  
**Settings Integration:** `kajo-settings` Page 13 (`src/settings/pages/page_web.c`), `KajoConfigStore` domain `web.toml` (`src/config/schemas/schema_web.c`)  
**Companion Visual Prototypes:** [`docs/kajo-web-mockup.html`](kajo-web-mockup.html) • [`docs/kajo-web-storyboard.html`](kajo-web-storyboard.html)  
**Core Dependencies:** `webkitgtk-6.0` (`>= 2.50`, verified `2.54.3` on Host A), `gtk4`, `libsoup-3.0`, `sqlite3`, `json-glib-1.0`, `libsecret-1`, `libgcrypt`, `KajoConfigStore` (`src/config/`)

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
6. **Chrome/Thorium-Style Multiple Profiles & Free Google Drive AppData Sync:**
   - First-class isolated profiles (`WORK`, `PERSONAL`, `DEV`, …) powered by per-profile `WebKitNetworkSession` containers, an Aero + Metro **Startup Profile Picker** (`760×480` card grid), a top-bar profile chip & switcher (`[ ● WORK ]`), and optional per-profile `1px` window border tinting.
   - Optional **per-profile Google Drive AppData sync** (`https://www.googleapis.com/auth/drive.appdata`, 100% free via personal Google Cloud OAuth 2.0 PKCE, refresh tokens stored in `libsecret`, optional client-side `AES-256-GCM` E2EE) synchronizing bookmarks, history deltas, pinned tabs/groups, and settings across `kajo-web` machines.
7. **First-Class Native Suite & Portal Integration:**
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
| **Native Ad & Tracker Blocking** | Two-layer (`WebKitUserContentFilterStore` + User Scripts) | Layer 1 compiles Safari-format JSON rulepacks (EasyList/EasyPrivacy) into fast native DFA bytecode (URL block + CSS hide; no native per-page block count API, so Omnibar shows real `ON`/`OFF` + compiled rule count). Layer 2 supports opt-in URL-scoped `~/.config/kajo/web/scripts/*.{js,css}` via `WebKitUserContentManager` for cosmetic/cookie-banner/YouTube mitigation. |
| **Normal Web Video (YouTube & Social)** | Hardware-accelerated (`gstreamer` `va` + DMA-BUF) | Verified on Host A (`gstreamer 1.28.9` + `gst-plugins-bad` unified `va` plugin: `vah264dec`, `vavp9dec`, `vaav1dec` zero-copy into WebKit DMA-BUF). Plays YouTube (up to 4K60 H.264/VP9/AV1), Reddit, X, Instagram, TikTok, Twitch, Vimeo; plus **`▶ OPEN IN KAJO PLAYER`** (`libmpv`). |
| **Built-In Video / Audio Downloader** | Three-tier (`WebKitDownload` + VDH-style `.m3u8`/`.mpd` sniffer $\rightarrow$ `ffmpeg` + optional `yt-dlp`) | Tier 1 downloads direct HTML5 `<video>`/`<audio>` natively. Tier 2 passively sniffs HLS (`.m3u8`), DASH (`.mpd`), and progressive video streams via `WebKitWebView::resource-load-started` (like Video DownloadHelper, with zero Companion App), parses resolution variants, and remuxes via system `ffmpeg` (`-c copy`) with profile cookies/Referer. Tier 3 spawns optional async `yt-dlp` + `ffmpeg` for JS-obfuscated YouTube/social sites (clearly disabled with `"Requires yt-dlp"` if absent). |
| **Mandatory Process Sandboxing** | Always ON | WebKitGTK 6.0 enforces Bubblewrap + `xdg-dbus-proxy` + `seccomp` (cannot be disabled in API). |
| **DevTools / Web Inspector** | Built-in (`WebKitWebInspector`) | Dockable bottom/right or detached WebKit Inspector (`F12` / `Ctrl+Shift+I`). |
| **Multiple Profiles & Google Web Login** | Full (`WebKitNetworkSession`) | Isolated cookies, IndexedDB, cache, history, bookmarks, and Google Workspace/Gmail web sessions per profile window. |
| **Cross-Device Sync (`kajo-web` $\leftrightarrow$ `kajo-web`)** | Built-in (Google Drive `drive.appdata`, free) | Uses official public Google Drive API v3 `appDataFolder` + PKCE OAuth 2.0 (`libsecret` token storage, optional `AES-256-GCM` E2EE). **Does not** sync into Google Chrome/Thorium's private Chrome Sync database (restricted by Google to official Chrome keys since 2021). |
| **Widevine Commercial DRM** | **Not supported** | WebKitGTK ships W3C ClearKey EME only; Netflix/Disney+/Spotify full-DRM streams require Chromium/Firefox. |
| **Chrome / Firefox Browser Extensions** | **Not supported** | `WebKitWebExtension` in 2.54 only parses manifests/metadata—not a full MV2/MV3 extension runtime. Native content blocking + user scripts cover ad-blocking and tweaks instead. |

---

## 3. Window Chrome & Vertical Autohide Tab Rail

### 3.1 Spatial Layout

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│ [🌐] [◀][▶][↻] [◧] │ [🔒 docs.kajo.local/spec  🛡ON] │ [⬇][↓1][●WORK][⚙][🗗][✕]│  34px Top Chrome
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
  - **Resting (unfocused):** Single flat `26px` monospace bar showing TLS status (`🔒` green HTTPS / `⚠` orange HTTP / `❖` cyan `kajo://`), bold host (`github.com`) + muted path (`/mikolas/Kajo`), inline ad-block shield pill (`[ 🛡 ON ]` / `[ 🛡 OFF ]` clickable for per-site allowlist toggle and compiled rulepack status — never a fake request count), and a `2px` bottom loading progress hairline (`@metro-accent`).
  - **Focused (`Ctrl+L` or click):** Expands to full editable URL/search input with instant dropdown suggestions (open tabs `[ TAB ]`, bookmarks `[ ★ ]`, history `[ ↺ ]`, search engine / bang expansion `[ 🔍 DDG ]`).
- **Right Cluster (Minimalist — Conditional Badges Only):**
  - `[ ⚠ SW RENDER ]` (amber warning — **only** when hardware acceleration is disabled or fell back).
  - `[ 🕶 PRIVATE ]` (magenta — **only** in ephemeral private windows).
  - `[ 125% ]` (zoom reset button — **only** when zoom `!= 100%`).
  - `[ ⬇ MEDIA ]` (video/audio download & `▶ OPEN IN KAJO PLAYER` popover — shown on video/media pages or via context menu).
  - `[ ↓ 1 ]` (downloads progress badge — **only** when downloads or `yt-dlp` jobs are active/completed this session).
  - `[ ● WORK ]` (active profile chip & switcher).
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
user_scripts_enabled      = false           # opt-in ~/.config/kajo/web/scripts/*.{js,css}
itp_enabled               = true            # Intelligent Tracking Prevention
cookie_policy             = "no-third-party" # "always" | "no-third-party" | "never"
https_only                = true
autoplay_policy           = "allow-without-sound" # "allow" | "allow-without-sound" | "deny"
enable_webrtc             = true
dns_prefetch              = true
media_download_format     = "best-mp4"      # "best-mp4" | "1080p" | "720p" | "audio-opus" | "audio-mp3"

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
# === Profiles & Google Drive AppData Sync ===
show_picker_on_startup    = true            # shown automatically when > 1 profile exists
profile_border_accent     = true            # tint active 1px window border by profile accent
sync_oauth_client_id      = ""              # personal Google Cloud Desktop OAuth Client ID
sync_oauth_client_secret  = ""              # optional Desktop OAuth Client Secret
sync_bookmarks            = true
sync_history              = true
sync_tabs_and_groups      = true
sync_settings             = true
sync_e2ee_enabled         = false           # AES-256-GCM passphrase encryption before Drive upload
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
   - **User Scripts & CSS** Toggle Card (opt-in `~/.config/kajo/web/scripts/*.{js,css}` via `WebKitUserContentManager`, status shows loaded script count)
   - Switch rows: **Intelligent Tracking Prevention (ITP)**, **HTTPS-Only Mode**, **WebRTC**
   - Cookie Policy segmented group (`BLOCK 3P` | `BLOCK ALL` | `ALLOW`)
   - Autoplay Policy segmented group (`MUTE ONLY` | `DENY ALL` | `ALLOW`)
   - Media Downloader default preset (`BEST MP4` | `1080P` | `720P` | `AUDIO`) + live `yt-dlp` / `ffmpeg` availability badge
   - Danger action row: `[ 🗑 CLEAR CACHE & COOKIES ]` (with inline confirmation, no modal dialog)
4. **Card 4 — Profiles & Google Drive AppData Sync:**
   - Profile manager list (`WORK`, `PERSONAL`, `DEV`, …) with `[ + NEW PROFILE ]`, rename/icon/accent editor, and delete (inline confirm)
   - Switch rows: **Show Profile Picker on Startup** (when `> 1` profile) and **Tint Window Border by Profile Accent**
   - **Google Drive AppData Sync** Toggle Card (per-profile status: `CONNECTED: mikolas@… • SYNCED 2M AGO` or `LOCAL ONLY`), `[ SIGN IN WITH GOOGLE ]` / `[ ↻ SYNC NOW ]` / `[ SIGN OUT ]`
   - Personal Google Cloud OAuth Client ID/Secret entries (with concise inline tip noting *"Set OAuth Consent Screen to In Production (Unverified, Free) to avoid 7-day token expiry"*)
   - Sync scope switches (`Bookmarks`, `History`, `Pinned Tabs & Groups`, `Settings`) + **Client-Side E2EE Passphrase** toggle (`AES-256-GCM`)
5. **Card 5 — Search, Zoom & User-Agent:**
   - Default Search Engine dropdown (`DuckDuckGo`, `Kagi`, `Startpage`, `Brave`, `Google`, `Custom`) + Custom Search URL entry + quick bang reference badge (`!g !gh !arch !mdn !w`)
   - Default Page Zoom slider (`50%–200%`)
   - User-Agent preset segmented group (`DEFAULT` | `SAFARI` | `CHROME` | `CUSTOM`) + custom string entry
6. **Card 6 — Appearance, Downloads & Developer Tools:**
   - Color Scheme segmented group (`FOLLOW KAJO` | `DARK` | `LIGHT`)
   - Font size controls (Default, Monospace, Minimum px)
   - Download Directory row (with `[ 📁 CHOOSE ]` via native portal) + Ask Before Saving switch
   - Enable Web Inspector (`F12`) switch row

---

## 6. Multiple Profiles & Free Google Drive AppData Sync

### 6.1 Multi-Profile Isolation (`WebKitNetworkSession`)

Modeled directly on Chrome/Thorium's window-scoped profile UX, backed natively by WebKitGTK 6.0's `WebKitNetworkSession`:

```text
~/.local/share/kajo/web/
├── profiles.toml                   # Global registry (id, name, icon, accent, email, sync_enabled, last_profile)
└── profiles/
    ├── default/                    # Default profile (WebKit data dir: cookies, IndexedDB, localStorage, HSTS, ITP)
    │   ├── history.db              # SQLite WAL + FTS5 history & bookmarks
    │   ├── bookmarks.json          # UUID + updated_at + tombstone syncable bookmark tree
    │   ├── session.json            # Open tabs, pinned tiles, collapsible groups
    │   └── profile-web.toml        # Optional per-profile overrides on top of ~/.config/kajo/web.toml
    ├── work/
    └── personal/

~/.cache/kajo/web/profiles/<id>/    # Isolated HTTP/disk cache per profile
```

- **Strict Isolation:** Every profile instantiates `webkit_network_session_new(data_dir, cache_dir)` shared by all `WebKitWebView`s in that profile's window(s). Signing into Google Workspace in `WORK` and personal Gmail in `PERSONAL` keeps cookies, cache, storage, and history 100% partitioned.
- **Single-Process Multi-Window:** One `GtkApplication` (`org.kajo.Web`) hosts multiple concurrent profile windows (lower UI memory, instant switching), while each profile's `WebKitNetworkSession` isolates its network/web processes and disk state.
- **Guest / Private Windows:** Instantiate `webkit_network_session_new_ephemeral()` — zero disk writes, zero history, zero sync.

### 6.2 Startup Profile Picker & In-Browser Profile Switcher

- **Startup Profile Picker (`760×480`, `.shell-web-picker`):**
  - Shown automatically on bare `kajo-web` launch when `> 1` profile exists and `show_picker_on_startup = true` (or explicitly via `kajo-web --profile-picker`). Skipped when `--profile <id>` or a URL with a last-active window is passed.
  - Displays a sharp `0px` Aero + Metro card grid: each card has a `3px` top accent bar, icon/glyph, uppercase monospace name, sync status (`☁ mikolas@… • SYNCED` or `LOCAL ONLY`), hover edit (`✎`) / delete (`✕`), and `1`–`9` keyboard shortcuts.
  - Footer controls: `[ + ADD PROFILE ]`, `[ 🕶 GUEST / PRIVATE ]`, and `☑ Show on startup`.
- **In-Browser Profile Chip & Switcher Popover:**
  - Minimalist top-right chip (`[ ● WORK ]` with profile accent dot; hidden when only 1 unconfigured profile exists unless hovered/enabled).
  - When `profile_border_accent = true`, the window's `1px` active border tints to that profile's Metro accent so side-by-side `WORK` / `PERSONAL` windows in `kajo-comp` are visually unmistakable.
  - Clicking `[ ● WORK ]` opens a compact **Profile Switcher Popover**: active profile sync status + `[ ↻ SYNC NOW ]`, one-click switch/focus to other profiles, `[ + NEW PROFILE ]`, `[ 🕶 PRIVATE WINDOW ]`, `[ ⊞ PROFILE PICKER ]`, and `[ ⚙ MANAGE ]`.

### 6.3 Free Per-Profile Google Drive AppData Sync (`drive.appdata`)

- **Official Public API, Zero Cost:**
  - Uses Google Drive API v3 with the narrowest scope `https://www.googleapis.com/auth/drive.appdata` (+ `openid email`). Only accesses a hidden app-private `appDataFolder`—cannot read or list any regular Google Drive files.
  - **Avoiding the 7-Day Token Limit for Free:** In Google Cloud Console, setting the personal OAuth Consent Screen to **In Production** (unverified, free for personal use `< 100` users, no billing card or paid audit) grants non-expiring refresh tokens.
- **OAuth 2.0 PKCE Loopback Flow (`src/web/web_sync.c`):**
  - Generates a cryptographic `code_verifier` + SHA-256 `code_challenge`, starts a temporary `GSocketService` listener on `127.0.0.1:<ephemeral_port>`, opens Google's OAuth consent URL inside a tab of the target profile, receives the authorization code on loopback, exchanges it via `libsoup3`, and closes the listener immediately.
  - Refresh tokens are stored per profile in **`libsecret`** (`org.kajo.Web.Sync`, attribute `profile_id`), never in plaintext config files, and temporary token buffers are scrubbed with `explicit_bzero()`.
- **Deterministic Merge & Optional E2EE (`src/web/web_sync_merge.c`):**
  - Stores compact versioned JSON snapshots (`kajo-web-bookmarks.json`, `kajo-web-history.json`, `kajo-web-tabs.json`, `kajo-web-settings.json`, or `.enc` when `sync_e2ee_enabled = true`) inside `appDataFolder`.
  - **Conflict-free merge:** Every bookmark/folder/pinned-tab entry carries a stable UUID, `updated_at` UTC timestamp, and `deleted` tombstone; newer timestamp wins cleanly on multi-machine merge without duplicating items.
  - **Optional Client-Side E2EE:** Derives a 256-bit key from the user's sync passphrase (`PBKDF2-HMAC-SHA256`, 600,000 iterations, random salt) and encrypts payloads with `AES-256-GCM` (`libgcrypt`) before upload so Google Drive stores only opaque ciphertext.
- **Event-Driven Resource Discipline (`AGENTS.md` §4D):**
  - Zero tight background polling. Sync executes asynchronously on: (1) profile startup, (2) `30s` debounce after bookmark/setting mutations, (3) clean profile window close, and (4) explicit `[ ↻ SYNC NOW ]`. All network calls run via `libsoup3` async `GTask` + `GCancellable` (`GWeakRef` safe).

### 6.4 Native Stream Sniffer & Three-Tier Video Downloader (VDH-Style, No Companion App)

Because `kajo-web` is both the browser and a native C17 process, it implements Video DownloadHelper's passive stream-detection architecture natively without a separate Companion App:

```text
WebKitWebView::resource-load-started ──► WebKitURIResponse (URI + MIME + Content-Length)
        │
        ├──► Tier 1: Direct <video>/<audio> / progressive .mp4/.webm (≥ 512 KB) ──► WebKitDownload
        ├──► Tier 2: Sniffed HLS (.m3u8) / DASH (.mpd) manifests ──► variant parser ──► system ffmpeg (-c copy)
        └──► Tier 3: JS-obfuscated sites (YouTube, Reddit, X, TikTok, IG) ──► optional async yt-dlp + ffmpeg
```

- **Passive Network Sniffing (`src/web/web_media_sniff.{h,c}`):**
  - Attaches to `WebKitWebView::resource-load-started` and `WebKitWebResource::notify::response` on each tab to detect:
    - **HLS manifests:** `.m3u8` / `application/vnd.apple.mpegurl` / `application/x-mpegurl`.
    - **DASH manifests:** `.mpd` / `application/dash+xml`.
    - **Progressive streams:** `.mp4` / `.webm` / `video/*` with `content_length >= 512 KB` (filtering out tiny tracking/ad loops).
  - Deduplicates per-tab detected streams and clears the tab's stream list on every `WEBKIT_LOAD_COMMITTED` navigation.
  - When streams are detected, the conditional top-bar chip lights up with the count (`[ ⬇ 3 ]`); clicking it opens the **Media Popover** listing parsed resolution/bitrate variants (`#EXT-X-STREAM-INF` for HLS, `<Representation>` for DASH).
- **Authenticated Lossless Remux via System `ffmpeg`:**
  - Captures the page `Referer`, `User-Agent`, and active profile's cookies (`webkit_cookie_manager_get_cookies()` on the profile's `WebKitNetworkSession`), passing headers to `ffmpeg` via a `0600` private temporary file (unlinked on completion/cancellation — never exposed in process `argv`) so signed/authenticated CDN streams download cleanly without `403`.
  - Runs asynchronously via `GSubprocess` + `GCancellable` using lossless remux (`-c copy -movflags +faststart`) by default, streaming live `%` and speed into the Downloads Popover.
- **Optional `yt-dlp` Backend & `kajo-player` Handoff:**
  - For sites whose media URLs/signatures are JS-computed (YouTube, Reddit, X, TikTok, Instagram), runs optional `yt-dlp` + `ffmpeg` asynchronously (`Best MP4`, `1080p`, `720p`, `Audio Only`). If `yt-dlp` is not found on `PATH`, Tier 3 options are visibly disabled with `"Requires yt-dlp"` while Tiers 1 & 2 continue working via `WebKitDownload` + `ffmpeg`.
  - Every detected stream or completed download offers **`▶ OPEN IN KAJO PLAYER`** (`libmpv`) and **`📁 SHOW IN KAJO FILES`**.

---

## 7. Keyboard Ergonomics & Command Palette

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
| `Ctrl+Shift+M` | Open Profile Switcher Popover (or Profile Picker) |
| `Ctrl+,` | Launch `kajo-settings --page web` |

---

## 8. Built-In `kajo://` Internal Pages & Persistence

Registered via `webkit_web_context_register_uri_scheme(ctx, "kajo", ...)`, rendered in pure Aero + Metro HTML/CSS synced with the active `@metro-accent`:

- **`kajo://newtab`:** Minimalist speed-dial grid (pinned bookmarks, recently visited tiles, quick search bar, and a compact status footer showing active profile, Drive sync status, GPU mode, and ad-blocker rule count).
- **`kajo://gpu`:** Live hardware acceleration diagnostics (WebKitGTK version, hardware acceleration policy, DMA-BUF status, WebGL / GPU process status, memory pressure thresholds, active process count).
- **`kajo://history` & `kajo://bookmarks`:** Fast SQLite-backed searchable lists (`~/.local/share/kajo/web/profiles/<id>/history.db`, WAL mode, FTS5 full-text search) with instant delete/filter.
- **`kajo://downloads`:** Full session & historical download log with "Show in Kajo Files" (`org.freedesktop.FileManager1` / `kajo-files`).

---

## 9. Modular C17 Architecture (`src/web/`)

Strict adherence to `AGENTS.md` §5 (`< 1500` lines per file, `< 150` lines per function, pure GTK-free testable core):

| Module | Responsibility |
| :--- | :--- |
| `src/web/web_model.{h,c}` | **Pure GTK-free core** (testable without Wayland/WebKit): tab tree, pinned/group ordering, MRU stack, closed-tab undo stack, omnibar URL vs. search/bang classifier (`!gh`, `!arch`, etc.), autohide rail FSM math, idle suspension selector. |
| `src/web/web_sync_merge.{h,c}` | **Pure GTK-free sync core**: UUID/timestamp/tombstone CRDT merge for bookmarks/history/tabs and optional `libgcrypt` `AES-256-GCM` + `PBKDF2` E2EE envelope. |
| `src/web/main.c` | `GtkApplication` (`org.kajo.Web`), `kajo_app_init_theme()`, CLI flags (`[URL...]`, `--profile <id>`, `--profile-picker`, `--private`, `--no-hw-accel`), single-instance URL & profile window dispatch (`G_APPLICATION_HANDLES_OPEN`). |
| `src/web/web_profiles.{h,c}` | Profile registry (`profiles.toml`), per-profile directory bootstrap, and `WebKitNetworkSession` lifecycle cache (including ephemeral Private/Guest sessions). |
| `src/web/web_picker.{h,c}` | Frameless `760×480` Aero + Metro Startup Profile Picker window (`.shell-web-picker`) with card grid, inline profile creator/editor, and keyboard `1`–`9` launch. |
| `src/web/web_sync.{h,c}` | Async `libsoup3` Google Drive API v3 (`drive.appdata`) client, loopback OAuth 2.0 PKCE listener (`GSocketService`), `libsecret` token store (`explicit_bzero` scrubbing), and event-driven debounced sync scheduler. |
| `src/web/web_window.{h,c}` & `web_window_private.h` | Frameless Aero + Metro window (`KajoWebWindow`), `34px` header bar, profile chip & switcher popover, `GtkOverlay` staging for autohide rail, Find Bar, Permission Infobar, Downloads popover, Web Inspector dock, and `KajoConfigStore` live reload subscriber. |
| `src/web/web_tab_rail.{h,c}` | Vertical tab rail widget: `4px` edge trigger strip (`rgba(0,0,0,0.01)`), real reveal/hide timers, `autohide`/`compact`/`pinned` layout modes, pinned grid, collapsible groups, drag-to-reorder (`GtkDragSource`/`GtkDropTarget`), mute & close buttons. |
| `src/web/web_tab.{h,c}` | `KajoWebTab` lifecycle wrapping `WebKitWebView` bound to the window's profile `WebKitNetworkSession`: hardware-acceleration settings, `WebKitWebViewSessionState` suspension/wake, crash recovery UI, per-site policies, context menu, and `WebKitFileChooserRequest` portal bridge. |
| `src/web/web_omnibar.{h,c}` | Minimalist resting domain pill + TLS/adblock badges $\leftrightarrow$ focused editable URL/search bar with async autocomplete dropdown. |
| `src/web/web_palette.{h,c}` | `Ctrl+K` fuzzy command, open-tab, closed-tab, bookmark, history, and profile-switch modal switcher. |
| `src/web/web_adblock.{h,c}` | `WebKitUserContentFilterStore` async JSON rulepack compiler, per-domain allowlist (`unless-domain`), and opt-in `~/.config/kajo/web/scripts/*.{js,css}` loader (`WebKitUserContentManager`). |
| `src/web/web_storage.{h,c}` | Per-profile async SQLite (`history.db`, FTS5) bookmarks/history store + atomic `session.json` save/restore. |
| `src/web/web_media_sniff.{h,c}` | **Pure GTK-free manifest & stream classifier + WebKit resource hook**: detects `.m3u8`, `.mpd`, and progressive `video/*` streams via `resource-load-started`, parses HLS `#EXT-X-STREAM-INF` / DASH `<Representation>` variants, and tracks per-tab detected media. |
| `src/web/web_downloads.{h,c}` | Unified three-tier downloader (`WebKitDownload` + async system `ffmpeg` lossless remux with `0600` cookie/header handoff + optional `yt-dlp`), live progress in Downloads Popover, **`▶ OPEN IN KAJO PLAYER`**, and **`📁 SHOW IN KAJO FILES`**. |
| `src/web/web_schemes.{h,c}` | Custom `kajo://` URI scheme handler (`newtab`, `gpu`, `history`, `bookmarks`, `downloads`) styled with live `@metro-accent`. |
| `src/settings/pages/page_web.c` | `kajo-settings` Page 13 (`13 / 13 • KAJO WEB`) with live GPU telemetry readout, 6 configuration cards (including Profiles & Drive AppData Sync), and `SAVE & APPLY` / `REVERT`. |
| `data/defaults/styles/80-web.css` | Scoped Aero + Metro CSS partial imported by `data/defaults/style.css`. |

---

## 10. Verification & Testing Plan

1. **Pure Core Unit Suite (`tests/test_web_model.c`):**
   - Tab add/close/reorder/pin/group invariants, MRU ordering, LIFO undo-close stack.
   - Omnibar classifier (`localhost:8080`, `kajo://gpu`, `https://...`, bare hostnames, `!gh`/`!arch` bangs, plain search queries).
   - Autohide Rail FSM transitions: trigger enter $\rightarrow$ reveal timer $\rightarrow$ expanded $\rightarrow$ leave $\rightarrow$ hide timer $\rightarrow$ collapsed, plus hold-open cancellation, re-enter cancellation, focus-loss collapse, and Escape dismissal (both immediate and "wait, then input" timing tests).
   - Idle suspension eligibility (verifies pinned, audible, active, and capture-active tabs are never suspended).
   - Profile registry round-trip (`profiles.toml`), UUID/tombstone CRDT sync merge conflict cases, `AES-256-GCM` E2EE encrypt/decrypt round-trip + wrong-passphrase rejection, and VDH-style stream sniffer tests (HLS `#EXT-X-STREAM-INF` master playlist parsing, DASH `<Representation>` parsing, MIME/512 KB size filtering, and deduplication).
2. **Config Store Parity (`tests/test_config.c`):**
   - Add `web.toml` to `config-roundtrip` and `fuzz-toml-config` verifying compiled-vs-shipped default parity, value clamping, `conf.d` overrides, and comment-preserving patching.
3. **Sanitizer Gate (`scripts/asan-test.sh`):**
   - Run `test-web-model` and `test-config` under ASan + UBSan with zero leaks or UAFs.
4. **Headless Visual Verification (`scripts/ui-shot.sh`):**
   - Render `kajo-web --profile-picker`, `kajo-web kajo://newtab` (autohide collapsed, autohide expanded, compact rail, pinned rail), `kajo-web kajo://gpu`, and `kajo-settings --page web`, comparing against [`docs/kajo-web-mockup.html`](kajo-web-mockup.html) and [`docs/ui-gallery.html`](ui-gallery.html).
5. **Live Hardware Verification (Host A — Intel Lunar Lake Xe2 + `kajo-comp`):**
   - Verify zero-copy DMA-BUF presentation (`SKIA + DMABUF`), 60/120 FPS smooth scrolling, WebGL rendering, VA-API video playback, two concurrent isolated profiles (`WORK` + `PERSONAL`) logged into separate Google accounts, `drive.appdata` OAuth PKCE sync round-trip, `kajo-files` FileChooser portal trigger on `<input type="file">`, and `kajo-settings --page web` live hot-reload.

---

## 11. Phased Implementation Plan

| Phase | Scope | Exit Gate |
| :--- | :--- | :--- |
| **Phase 1 — Core, Profiles, Config & Skeleton** | `web.toml` schema (`schema_web.c`), `web_model.{h,c}` + `test_web_model.c`, `web_profiles.{h,c}` (`WebKitNetworkSession` per profile) + `web_picker.{h,c}`, `kajo-web` frameless window + `WebKitWebView` with hardware acceleration (`ALWAYS`) + minimalist top bar + `kajo://gpu`. | `meson compile && meson test` pass (0 warnings), `kajo-web` launches isolated profile windows with Skia + DMA-BUF. |
| **Phase 2 — Vertical Autohide Tab Rail** | `web_tab_rail.{h,c}` (`autohide` / `compact` / `pinned`, left/right, `4px` trigger strip, real reveal/hide timers, pinned tiles, groups, mute/close, drag-reorder, `Ctrl+B`, `Ctrl+Shift+T`). | Rail FSM tests + headless visual captures for all 3 rail modes. |
| **Phase 3 — `kajo-settings` Page 13 & Live Reload** | `src/settings/pages/page_web.c` (all 6 cards, `SAVE & APPLY` / `REVERT`) + `GFileMonitor` hot-reload in `kajo-web`. | Changing rail mode, side, profile border accent, search engine, ad-blocker, or zoom in `kajo-settings` updates open `kajo-web` windows immediately. |
| **Phase 4 — Omnibar, Palette, Storage & Ad-Blocker** | `web_omnibar.{h,c}`, `web_palette.{h,c}` (`Ctrl+K`), `web_storage.{h,c}` (per-profile SQLite FTS5 + session restore), `web_adblock.{h,c}` (`WebKitUserContentFilterStore` + bundled rules + opt-in user scripts), `kajo://newtab` / `history` / `bookmarks`. | Unit + ASan tests pass; native content filter compiles & blocks ads with live `ON`/`OFF` shield & rule count. |
| **Phase 5 — Drive Sync, Stream Sniffer & Overlays** | `web_sync_merge.{h,c}`, `web_sync.{h,c}` (`drive.appdata` PKCE OAuth + `libsecret` + optional `AES-256-GCM` E2EE), `web_media_sniff.{h,c}` (`.m3u8`/`.mpd`/`.mp4` sniffer $\rightarrow$ `ffmpeg` + optional `yt-dlp` + `kajo-player` handoff), inline Find Bar (`Ctrl+F`), Permission Infobar, Downloads Popover, idle tab suspension (`WebKitWebViewSessionState`), `WebKitFileChooserRequest` portal bridge, docked Web Inspector (`F12`), gallery & docs sync. | Full build, test, ASan, `ui-shot.sh`, and live Host A verification (including multi-profile, Drive AppData sync, and stream sniffing/remux) complete. |
