# Specification: `kajo-web` — Aero + Metro Chromium/CEF Wayland Browser (`docs/spec_kajo_web.md`)

**Status:** Beta — Implemented on Chromium Embedded Framework (`CEF`, Alloy Views) *(the earlier WebKitGTK 6.0 prototype was removed on branch `feat/kajo-web-cef`)*  
**Target Binary:** `kajo-web` (`org.kajo.Web`, `src/web_cef/` + shared GTK-free C17 core `src/web/`)  
**Settings Integration:** `kajo-settings` Page 12 (`src/settings/pages/page_web.c`, `--page web`), `KajoConfigStore` domain `web.toml` (`src/config/schemas/schema_web.c`)  
**Core Dependencies:** Chromium Embedded Framework (`libcef`, Linux x86\_64 Alloy Views), `gio-2.0`, `glib-2.0`, `sqlite3`, `KajoConfigStore` (`src/config/`)

---

## 1. Objectives & Architecture

`kajo-web` brings Chrome-class modern web compatibility and hardware-accelerated Wayland playback into Kajo's frameless **Aero + Metro** aesthetic without Chromium's stock titlebar or rounded Material chrome:

1. **Alloy Views Host + HTML/CSS Chrome (`src/web_cef/`):**
   - Runs **CEF in Alloy Runtime Style** (`CEF_RUNTIME_STYLE_ALLOY`) directly on native Wayland (`--ozone-platform=wayland`, VA-API enabled by default; Vulkan disabled on Wayland where Chromium warns of Wayland surface incompatibility).
   - Window chrome is rendered inside dedicated chrome `CefBrowserView` layers served from internal **`kajo-chrome://ui/*`** (`topbar.html`, `rail.html`, `overlay.html`) and synchronized in real time with `theme.toml` (`@metro-accent`, surface palette, `ui_font_family`, all 4 Kajo font sizes `ui_font_size_px`, `font_heading_size_px`, `font_subtitle_size_px`, `font_badge_size_px`, dynamic `ComputeChromeUiMetrics` bar/control/icon sizing, `0px` radius, `1px` borders).
2. **Vertical Tab Rail (Left or Right, Zero Page Reflow in Overlay Modes):**
   - Supports three modes (`rail_mode` in `web.toml`):
     - **`autohide`** *(default)*: Collapses to a `4px` edge trigger strip (`rgba(0,0,0,0.01)`) with `2px` tab ticks; reveals as a floating `240px` (`rail_width_px`) overlay over the viewport without reflowing page DOM.
     - **`compact`**: Keeps a docked favicon rail (`48px` at default `14px` UI font, scaling with `ui_font_size_px`) and expands as an overlay on hover without vertical/horizontal icon jump (`0px` shift).
     - **`pinned`**: Permanently docked sidebar sharing width cleanly with the active page viewport.
   - Configurable to **`left`** or **`right`** (`rail_side`), with configurable reveal/hide delays, optional domain subtitles (`show_tab_domains`), pinned favicon grid, collapsible colored tab groups, drag-to-reorder, mute toggle, and `Ctrl+Shift+T` closed-tab undo stack.
3. **Minimalist Theme-Scaled Top Chrome (`kajo-chrome://ui/topbar.html`) & Zen Autohide:**
   - Topbar height, button size, and omnibar scale dynamically from `ui_font_size_px` (`42px` bar / `30px` controls / `16×16` crisp inline SVG icons at default `14px` font, clamping `40..54px` across `11..20px`).
   - **Left:** Inline SVG Back, Forward, Reload/Stop, and vertical rail toggle buttons.
   - **Center:** Clickable & editable omnibar (`Ctrl+L` / `Alt+D`) with semantic TLS/scheme badge (`kajo://`, `HTTPS`, HTTP warning), autocomplete dropdown (open tabs, bookmarks, history, search engine / `!g`, `!ddg`, `!w`, `!gh` bangs), clickable Zoom pill, Bookmark star (`Ctrl+D`), inline `[ 🛡 <N> ]` Ad-Block Shield button, and a `2px` bottom loading progress hairline.
   - **Right:** Conditional `[ ⚠ SW RENDER ]` warning badge (when `--no-hw-accel` or `hardware_acceleration = "never"`), `[ 🕶 PRIVATE ]` chip (in Private mode), **always-visible `↓` Downloads chip** (with active counter + live aggregate progress fill bar), active Profile chip (`[ ● <PROFILE> ]`), Settings (`kajo-settings --page web`), Float/Tile (`niri_toggle_window_floating`), and Close buttons.
   - Supports `top_bar_mode = "autohide"` (`Ctrl+Shift+U` toggle).
4. **Idle Tab Suspension, Lazy Restore & Crash Recovery:**
   - **Lazy session restore:** On startup, only the active tab instantiates a `CefBrowserView` while restored inactive tabs start suspended with their URL, title, group, pin state, and persisted favicon.
   - **Idle suspension & renderer teardown:** Background tabs idle beyond `suspend_idle_min` (default `30` min; `0` = off; never pinned, audible, capturing, downloading, or `kajo://` tabs), plus the active tab of an unfocused window once both the window and tab have been idle past `suspend_idle_min`, detach and terminate their backing `CefBrowserView` + renderer cleanly via `TabReaperWindowDelegate` (`CloseBrowser(true)` → `DoClose(false)` → `OnBeforeClose()`). Idle countdown resets on leave-time (`web_model_activate()` / window deactivation), and refocusing an inactive window automatically wakes its active tab if suspended.
   - **Sleeping tab visuals:** Suspended tabs retain their persisted `favicon_url` (with `Origin/favicon.ico` fallback) styled grayed-out (`grayscale(100%)`) with a dashed favicon border and `· zzz` marker. Clicking a suspended tab re-creates its browser view and reloads it cleanly.
   - Renderer crashes navigate to `kajo://crashed?reason=...` with one-click `[ ↻ RELOAD TAB ]` and `[ CLOSE TAB ]`.
5. **Isolated Multi-Profiles, Single-Instance IPC & Ephemeral Private Mode:**
   - Profile registry in `profiles.toml` (`WORK`, `PERSONAL`, etc.) isolates cookies, cache, storage, bookmarks, history, downloads, and site permissions via per-profile `CefRequestContext` instances (`~/.local/share/kajo/web-cef/<id>/` with `profile/`, `session.json`, `history.db`, `permissions.json` + `~/.cache/kajo/web-cef/<id>/`).
   - Per-profile UNIX socket IPC (`ProfileIpcServer` in `src/web_cef/cef_ipc.{h,cc}`) forwards external URL opens to an already-running instance of the same profile instead of racing `profile/` locks.
   - Startup Profile Picker (`show_picker_on_startup`) allows selecting, creating, renaming, accent-coloring, or launching into an **In-Memory Ephemeral Private Context** (`--private` / `Ctrl+Shift+N`).
   - One-shot Chromium/Thorium tab & favicon migration into `session.json` is provided by `scripts/import-chromium-session.py` (SNSS session logs + `Bookmarks` folder fallback + read-only `Favicons` DB lookup).

---

## 2. Capabilities & Honest Boundaries (`AGENTS.md` §8)

| Capability | Status in `kajo-web` | Implementation Notes |
| :--- | :--- | :--- |
| **Modern Web / JS / CSS / WASM** | Implemented (Chromium/CEF) | Full Blink/V8 compatibility via CEF Alloy Views (`--ozone-platform=wayland`). |
| **Wayland GPU & VA-API Video Decode** | Implemented (Default ON) | `--use-gl=angle --use-angle=gl --ozone-platform=wayland` + VA-API flags (`--enable-features=VaapiVideoDecoder,...`). Passing `--no-hw-accel` or `hardware_acceleration = "never"` switches to SwiftShader/software rendering and surfaces an amber `[ ⚠ SW RENDER ]` badge in the top bar and `kajo://gpu`. |
| **IO-Thread Ad & Tracker Blocking** | Implemented (`CefAdblockEngine`) | Network + third-party + resource-type + hosts-file filter matching (`CefResourceRequestHandler::OnBeforeResourceLoad` on `TID_IO`), cosmetic CSS selector injection (`OnLoadEnd` in main/subframes), `[ 🛡 ON ]`/`[ 🛡 OFF ]` per-site allowlist toggle, optional EasyList/EasyPrivacy background auto-sync (`adblock_auto_sync`), custom filter file (`adblock_custom_rules_path`), opt-in `~/.config/kajo/web/scripts/*.{js,css}`, and `kajo://adblock` dashboard. |
| **Downloads Popout & History Manager** | Implemented | Always-visible topbar `↓` chip + 3-zone Aero Downloads popout (`Pause`/`Resume`/`Cancel`/`Open File`/`Open Folder` + link to `kajo://downloads`), SQLite `downloads` table persistence, configurable `download_dir` (including `~` expansion), `ask_download_location`, and `downloads_auto_open` (via `xdg-open` for completed non-dangerous files). |
| **Per-Profile Site Permissions** | Implemented | Interactive infobar for Mic/Cam/Geolocation/Notifications/Clipboard persisted per `(origin, key)` in `permissions.json` (`PermissionStore`), viewable and revocable on `kajo://permissions` (shared across browser & Web App windows). |
| **Web Apps (`--app-id` / `--app`)** | Implemented | Shared-profile multi-window chromeless app mode (`KajoWebAppWindow` with per-app `wayland_app_id`, scope/auth routing, XDG `.desktop` + hicolor icon installer in `~/.local/share/applications/` & `~/.local/share/icons/hicolor/`, `webapps.json` registry, and `kajo://apps` manager). |
| **Desktop Web Notifications** | Implemented | Renderer V8 `window.Notification` & `ServiceWorkerRegistration.prototype.showNotification` bridge wired to async `org.freedesktop.Notifications` D-Bus (`Notify`, `CloseNotification`, click-to-focus `ActionInvoked`, `NotificationClosed`) honored across browser tabs and Web App windows. |
| **Isolated Multi-Profiles & Private Windows** | Implemented | Per-profile `CefRequestContext` + Startup Profile Picker + topbar Profile Switcher + Private in-memory context + `profile_border_accent`. |
| **Google Drive AppData Sync (`sync_*`)** | **Not implemented** in `kajo-web` | CRDT merge & E2EE helpers exist in `src/web/web_sync_merge.c` (tested by `web-model`) and keys remain in `web.toml` for schema parity, but live OAuth/Drive sync is not wired into `kajo-web` or `page_web.c`. |
| **HLS/DASH Media Sniffer & `yt-dlp` Downloader** | **Not implemented** in `kajo-web` | Pure parser helpers exist in `src/web/web_media_sniff_core.c`, but passive stream sniffing/`ffmpeg`/`yt-dlp` UI are not active in `kajo-web` or `page_web.c`. |
| **Widevine Commercial DRM / Chrome Extensions** | **Not supported** | Alloy runtime does not ship Chrome's extension service runtime or proprietary Widevine CDM bundle. |

---

## 3. Configuration (`~/.config/kajo/web.toml` & `kajo-settings --page web`)

`kajo-web` reads `~/.config/kajo/web.toml` (plus `conf.d/*.toml`) via `KajoConfigStore` (`src/config/schemas/schema_web.c`) and subscribes to live changes:

- **Applied Live on Save in `kajo-settings` (`page_web.c`):**
  - **Card 1 (Rail & Chrome):** `rail_mode`, `rail_side`, `rail_width_px`, `rail_reveal_delay_ms`, `rail_hide_delay_ms`, `top_bar_mode`, `show_tab_domains`, `new_tab_page`, `new_tab_custom_url`, `restore_session`.
  - **Card 2 (Suspension & Scrolling):** `smooth_scrolling`, `suspend_idle_min`.
  - **Card 3 (Privacy & Ad Blocker):** `adblock_enabled`, `adblock_auto_sync`, `adblock_custom_rules_path`, `user_scripts_enabled`, `cookie_policy`, `https_only`, `autoplay_policy`.
  - **Card 4 (Profiles & Startup):** `show_picker_on_startup`, `profile_border_accent`, plus `[ 🌐 MANAGE PERMISSIONS ]` launcher opening `kajo://permissions`.
  - **Card 5 (Search, Zoom & User-Agent):** `search_engine`, `search_custom_url`, `default_zoom_pct`.
  - **Card 6 (Appearance, Downloads & DevTools):** `color_scheme`, `default_font_size_px`, `mono_font_size_px`, `min_font_size_px`, `download_dir`, `ask_download_location`, `downloads_auto_open`, `developer_extras`.
- **Applied at Process Startup / App Restart:**
  - `hardware_acceleration` (`always` vs. `never` GL/Angle/VA-API flags in `KajoCefApp`), `enable_webgl`, `enable_webrtc` (WebRTC IP handling policy), and `user_agent_preset` / `user_agent_custom` (`CefSettings.user_agent`: `default`, `safari`, `chrome-linux`, `custom`).

---

## 4. Keyboard Shortcuts & `kajo://` Pages

### 4.1 Keyboard Shortcuts (`OnTabShortcut` / `OnChromeShortcut`)

| Shortcut | Action |
| :--- | :--- |
| `Ctrl+T` / `Ctrl+W` / `Ctrl+Shift+T` | New tab / Close tab / Reopen last closed tab |
| `Ctrl+B` | Toggle / reveal vertical tab rail |
| `Ctrl+L` / `Alt+D` | Focus & select unified Omnibar |
| `Ctrl+K` | Open Command, Navigation & Open-Tab Fuzzy Palette |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Visual next / previous tab (peeks rail briefly) |
| `Alt+1` .. `Alt+9` | Jump to tab `1..8` (`Alt+9` selects last tab) |
| `Ctrl+R` / `Ctrl+Shift+R` / `Ctrl+F5` | Reload / Hard reload bypassing cache |
| `Ctrl+F` | Toggle inline Find Bar |
| `Ctrl+Shift+U` | Toggle Top Bar Zen Autohide |
| `Ctrl+Shift+N` | Open Ephemeral Private Window |
| `Ctrl+J` | Toggle Downloads Popout |
| `Ctrl+D` | Bookmark current page (inline popover) |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | Zoom In / Out / Reset `100%` |
| `Ctrl+M` | Mute / Unmute active tab |
| `Ctrl+Shift+M` | Toggle Profile Switcher Popout |
| `Ctrl+,` | Launch `kajo-settings --page web` |

*(DevTools can be opened via right-click context menu `Inspect Element` when `developer_extras = true`.)*

### 4.2 Built-In `kajo://` Pages (`src/web_cef/cef_chrome_ui.cc`)

- **`kajo://newtab`**: Speed-dial pinned bookmarks, recent history links, search box, active profile, GPU status, and ad-block rule count.
- **`kajo://bookmarks`** & **`kajo://history`**: Searchable/filterable SQLite FTS5 tables (`~/.local/share/kajo/web-cef/<id>/history.db`) with inline delete / clear.
- **`kajo://downloads`**: Live-updating active downloads + SQLite download history with search, status filters (`ALL` / `ACTIVE` / `COMPLETED`), `Pause` / `Resume` / `Cancel` / `Retry`, `OPEN`, `📁` (`Open Folder`), and `Remove` / `CLEAR FINISHED`.
- **`kajo://adblock`**: Global on/off toggle, per-site paused hosts list + unpause, manual list update (`EasyList` / `EasyPrivacy`), rule statistics, and user-script count.
- **`kajo://permissions`**: List of granted/denied origin permissions with single-entry revoke and `CLEAR ALL PERMISSIONS`.
- **`kajo://apps`**: Installed profile Web Apps manager with quick URL install, launch (`KajoWebAppWindow`), and uninstall (removing desktop/icon files).
- **`kajo://gpu`**, **`kajo://crashed`**, **`kajo://about`**: Hardware acceleration / VA-API diagnostics (+ link to `chrome://gpu`), tab crash recovery, and suite runtime info.

---

## 5. Source Layout & Automated Tests

| Module / Test | Role |
| :--- | :--- |
| `src/web_cef/main.cc` | Entry point, CLI parsing (`--profile`, `--app-id`, `--app`, `--private`, `--no-hw-accel`, `--no-adblock-sync`, `[URL...]`), Wayland/Ozone defaults, single-instance forwarding, legacy cache profile migration. |
| `src/web_cef/cef_app.{h,cc}` | `KajoCefApp` & `BrowserHostCoordinator`: Chromium GPU/VA-API/WebRTC switches, shared `CefRequestContext`/`PermissionStore`/`WebAppStore`, `ProfileIpcServer` & `WebNotificationBridge`, multi-window lifecycle, renderer V8 `Notification` shim & manifest prober. |
| `src/web_cef/cef_window.{h,cc}` | Primary Alloy Views browser host window, tab lifecycle, idle suspension timer, Find Bar, Command Palette, Downloads tracking, Filter updates, `install_app` overlay, `KajoConfigStore` subscriber. |
| `src/web_cef/cef_webapp_window.{h,cc}` | Chromeless `KajoWebAppWindow` sharing profile cookies/permissions/notifications with per-app `wayland_app_id`, scope/SSO navigation routing, and isolated permission infobar. |
| `src/web_cef/cef_webapps.{h,cc}` | Pure `WebAppStore` (`webapps.json`), Web App Manifest parser, scope & OAuth/SSO matcher, fallback monogram SVG, XDG `.desktop` + hicolor icon installer. |
| `src/web_cef/cef_notify.{h,cc}` | Async `org.freedesktop.Notifications` D-Bus bridge on dedicated GLib worker thread (`Notify`, `CloseNotification`, `ActionInvoked`, `NotificationClosed`). |
| `src/web_cef/cef_host_coord.h` | Coordinator interface connecting `KajoCefApp`, `KajoBrowserWindow`, and `KajoWebAppWindow`. |
| `src/web_cef/cef_tab.{h,cc}` | Per-tab `CefClient` callbacks (navigation, title, favicon, HTTPS-Only & cookie policy, downloads, JS dialogs, permission prompts, crash handler, shortcuts). |
| `src/web_cef/cef_chrome_ui.{h,cc}` | HTML/CSS/JS chrome builder (`topbar`, `rail`, `overlay`, theme CSS tokens, inline SVGs) and `kajo://*` scheme responses. |
| `src/web_cef/cef_ipc.{h,cc}` | Per-profile single-instance Unix socket server (`ProfileIpcServer`) and external URL forwarding (`TryForwardToRunningInstance`). |
| `src/web_cef/cef_site_util.h` | Pure host/eTLD+1 matching, private-host check, color/href sanitization, origin favicon fallback, and `ComputeChromeUiMetrics`. |
| `src/web_cef/cef_adblock.{h,cc}` | IO-thread ABP network + host-file + cosmetic CSS blocker & EasyList/EasyPrivacy auto-sync cache. |
| `src/web_cef/cef_prefs_helper.{h,cc}` | `PermissionStore`, user-script directory loader, same-site cookie matcher, log severity parser. |
| `src/web/web_{model,storage,abp_compiler,sync_merge,media_sniff_core}.{h,c}` | Shared GTK-free C17 core (`web_model` + SQLite WAL/FTS5 `web_storage` used directly by `kajo-web` and `page_web.c`). |
| `scripts/import-chromium-session.py` | One-shot Chromium/Thorium session (`SNSS` / `Bookmarks`) and `Favicons` importer targeting `session.json` (+ `tests/test_import_chromium_session.py`). |
| `tests/test_web_model.c` (`web-model`) | Tests rail FSM, tab/MRU/undo invariants, leave-time suspension & favicon persistence, omnibar classifier, profile registry, SQLite history/bookmarks/downloads/session storage, CRDT merge, and ABP compiler safety. |
| `tests/test_web_cef_adblock.cc` (`web-cef-adblock`) | Tests `CefAdblockEngine` ABP rules, `$third-party`, `$script`, `$domain`, multi-part eTLD+1 matching, strict option validation, exceptions `@@`, hosts entries, cosmetic CSS, and per-host allowlist. |
| `tests/test_web_cef_core.cc` (`web-cef-core`) | Tests `PermissionStore` save/load/revoke, same-site cookies, user scripts loader, `IsLocalOrPrivateHost`, `SanitizeSafeHref`, `DeriveOriginFaviconFallback`, `ComputeChromeUiMetrics`, `ProfileIpcServer` round-trip, and log severity parsing. |
