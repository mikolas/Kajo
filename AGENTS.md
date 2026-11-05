# AGENTS.md — Kajo Desktop Suite Engineering & AI Agent Rules

This document is the authoritative engineering manual for AI coding assistants working on the **Kajo Desktop Environment** (`kajo`, `kajo-settings`, `kajo-files`, `kajo-greeter`). Follow every rule strictly to maximize development velocity and maintain zero-warning, zero-regression quality.

---

## 1. Agent Execution & Speed Rules (Read First)

1. **NO SUBAGENTS (`invoke_subagent` FORBIDDEN)**:
   - **NEVER** spawn subagents (`research`, `self`, etc.). Perform all `ripgrep` (`rg`) searches, file reads, and edits directly in the primary context for maximum speed and zero context loss.
2. **RESPECT "DO NOT EDIT" / "WAIT" DIRECTIVES**:
   - When the user says *"do not edit"*, *"what do you think?"*, or *"wait"*, perform **zero** file writes/edits. Answer or analyze concisely and wait for explicit approval.
3. **SPECIFICATIONS (`SPEC_*.md`) & `ROADMAP.md` LIVE IN PROJECT ROOT**:
   - Architectural specifications (`SPEC_<FEATURE>.md`), `ROADMAP.md`, `HARDWARE_FINDINGS.md`, `DEVELOPMENT.md`, and `README.md` **MUST** be created and maintained in the **project root directory** (`/home/mikolas/src/Kajo/`).
   - Temporary one-off debug scripts or throwaway notes go in `scratch/` and must never be committed.
4. **DO NOT TOUCH PROTECTED FUNCTIONS**:
   - Do **NOT** modify `populate_app_index` in `src/launcher/launcher_surface.c` unless explicitly instructed by the user.
5. **BUILD & TEST VERIFICATION GATE**:
   - Never claim a task is complete or commit code without running:
     ```bash
     meson compile -C builddir && meson test -C builddir
     ```
   - Both commands **MUST** pass with **0 compiler errors, 0 compiler warnings**, and **100% passing GTest suites**.

---

## 2. Tech Stack & Target Binaries

| Binary | App ID | Role & Key Libraries |
| :--- | :--- | :--- |
| **`kajo`** | `org.kajo.Shell` | Wayland panel, 22 widgets, 16-band FFT spectrum visualizer, notification daemon, app launcher, `ShellOSK` virtual keyboard (`zwp_virtual_keyboard_v1` + `zwp_input_method_v2`), bezel touch gestures, foldable posture manager, `ext-idle-notify-v1` screensaver, and `ext-session-lock-v1` PAM lock screen. |
| **`kajo-settings`** | `org.kajo.Settings` | 10-page modular system settings (`src/settings/pages/page_*.c`) with live D-Bus, PipeWire, Niri IPC, and GLSL shader switching. |
| **`kajo-files`** | `org.kajo.Files` | *(Alpha)* URI-first GIO/GVFS/UDisks2 file manager (`src/files/`) with natural filename collation, interactive column/popover sorting, hit-tested context menu (`Open With / Set Default`, `libarchive` `Extract Here`), inline LUKS/SMB auth, and `Space` Quick Look (`poppler-glib`, `libarchive`, `KajoPlayerMpv` / `libmpv`, `GtkPicture`, `GtkTextView`). |
| **`kajo-player`** | `org.kajo.Player` | *(Beta)* Hardware-accelerated `libmpv` + `GtkGLArea` video/audio player (`src/player/`) with subtitle/audio stream selectors, external `.srt`/`.ass` loading, sync delay, speed/frame-step, chapters, fullscreen bottom-edge overlay HUD, and MPRIS2 D-Bus provider. |
| **`kajo-greeter`** | `org.kajo.Greeter` | Native `greetd` Wayland login greeter (`src/greeter/`) with PAM `--test` mode and multi-monitor Niri / Cage support. |

---

## 3. Visual Architecture ("Aero + Metro" Design System)

Every standalone Kajo application (`kajo-settings`, `kajo-files`, future `kajo-player`, etc.) **MUST** follow the unified **Aero + Metro** aesthetic rather than default Adwaita:

1. **Window Chrome & CSS Classes**:
   - Undecorated window: `gtk_window_set_decorated(win, FALSE)` with CSS class `shell-settings-window` plus an app-specific class (e.g. `shell-files-window`, `shell-player-window`).
   - Top drag bar: `GtkWindowHandle` wrapping a horizontal `GtkBox` with `.settings-header-bar`, uppercase `.settings-header-title` (`⚙ SYSTEM SETTINGS`, `📁 KAJO FILES`), `"🗗 FLOAT / TILE"` button (`niri_send_action("{\"Action\":\"ToggleWindowFloating\"}")`), and `.settings-close-btn` (`✕`, `#e81123` on hover).
2. **Surface & Border Geometry**:
   - **Zero Corner Radius (`0px`)** on windows, cards, popovers, inputs, and levelbars (exception: `3px` keycap radius on `window.osk-window button.osk-key`).
   - **Dark Surfaces**: `#111111` / `#141414` / `#181818` / `#1a1a1a` backgrounds with `1px solid #2b2b2b` structural borders and `1px solid @metro-accent` window/active borders.
   - **Typography**: `Fira Code` monospace for headers, sidebar items, technical badges (`.shell-popover-badge`), and code previews.
3. **Standalone Theme Initialization & Live CSS Sync**:
   - Every standalone `main.c` **MUST** initialize `ShellTheme` (`shell_theme_new()` + `shell_theme_apply()`) and `ShellIcons` (`shell_icons_init()`) on startup.
   - Whenever `data/defaults/style.css` is edited, sync it to `~/.local/share/kajo/defaults/style.css` and `~/.config/kajo/style.css` so running processes and user overrides immediately reflect the new rules:
     ```bash
     cp data/defaults/style.css ~/.local/share/kajo/defaults/style.css
     cp data/defaults/style.css ~/.config/kajo/style.css
     ```

---

## 4. Critical GTK4, GIO, PipeWire & Wayland Pitfalls (DO NOT REPEAT)

### A. GTK4 Widget & Layout Traps
- **Vertical Alignment in Horizontal Rows (`GtkBox`)**:
  When packing an icon (`GtkImage`), text column (`GtkBox`), and chevron/button into a horizontal row, **ALWAYS** set `gtk_widget_set_valign(..., GTK_ALIGN_CENTER)` on the parent `hbox` AND on `icon`, `vbox`, `label`, and `button`. Omitting `valign` causes single-line labels to float to the top edge of the cell next to taller icons.
- **`GtkFlowBox` & `GtkListBox` Double-Click Activation**:
  When `activate-on-single-click: false` is set on `GtkFlowBox` or `GtkListBox`, GTK4 natively selects on 1 click and emits `child-activated` / `row-activated` on double-click and `Enter`. **NEVER** attach a separate `GtkGestureClick` to inner tile/row boxes—it steals the click sequence and breaks activation.
- **GTK4 CSS Background Override**:
  GTK4's built-in theme applies `background-image` gradients to `button`, `entry`, and `row:selected`. Setting `background-color` alone in `style.css` will fail unless `background: none; background-image: none;` is also set.
- **No Un-Scoped Dynamic `GtkCssProvider`**:
  Never pass generic selectors (`box`, `button`, `label`) to `gtk_css_provider_load_from_string()` registered on `gdk_display_get_default()`. Always scope dynamic CSS to unique class names (`.swatch-0`, `.accent-tile-0`).

### B. GIO & Async File Launching Traps
- **Never Use `g_autoptr(GtkUriLauncher)` for Fire-and-Forget Launches**:
  `g_autoptr(GtkUriLauncher)` unrefs and cancels the async D-Bus portal call the instant the function returns.
  To open files/URIs in their default desktop application, use:
  1. `g_app_info_launch_default_for_uri(uri, NULL, &err)`
  2. Fallback: `g_app_info_get_default_for_type(content_type, FALSE)` + `g_app_info_launch()`
  3. Fallback: `g_spawn_async()` with `xdg-open`.
- **URI-First File Operations**:
  Never assume `g_file_get_path(file)` is non-NULL. Virtual GVFS locations (`trash:///`, `network:///`, `recent:///`, unmounted `smb://`) have no local POSIX path; always operate on `GFile *` and check `G_FILE_ATTRIBUTE_STANDARD_TARGET_URI` when navigating virtual folders.

### C. PipeWire / PulseAudio (`libpulse`) Traps
- **Never Hardcode `2` Channels on Volume Writes**:
  Virtual surround sinks (e.g. 7.1 `effect_input.virtual-surround-7.1-hesuvi`, `Dolby Access`) have `8` channels. Calling `pa_cvolume_set(&cv, 2, vol)` on an 8-channel sink is rejected by PipeWire/PulseAudio. Always store `sink_channels = info->channel_map.channels` from `pa_sink_info` and pass `sink_channels` to `pa_cvolume_set()`.
- **Subscribe to `PA_SUBSCRIPTION_MASK_SERVER`**:
  Always include `PA_SUBSCRIPTION_MASK_SERVER` alongside `SINK` and `SOURCE` so default output/input routing changes trigger `pa_context_get_server_info()` immediately.
- **Auto-Reconnect on Server Restart**:
  When `pa_context_get_state()` enters `PA_CONTEXT_FAILED` or `PA_CONTEXT_TERMINATED` (e.g. `systemctl --user restart pipewire-pulse`), schedule a 2-second reconnect timer (`g_timeout_add_seconds(2, ...)`), cleaning up the timer ID on widget/page destruction.

### D. Wayland Layer-Shell & Resource Discipline
- **Never Use `opacity: 0` on Autohide / Gesture Strips**:
  GTK4 skips rendering surfaces with `opacity: 0`, which stops Wayland `wl_surface.commit` and breaks pointer/touch enter events on 2px edge trigger strips. Use `background-color: rgba(0, 0, 0, 0.01)` instead.
- **Popout- & Map-Gated Resource Architecture**:
  - PulseAudio monitor streams (`@DEFAULT_MONITOR@`) must start corked (`PA_STREAM_START_CORKED`), uncork on `map`, and re-cork on `unmap`.
  - Quick Look media in `kajo-files` MUST use `KajoPlayerMpv` (`libmpv` + `GtkGLArea`) rather than GTK4's `GtkVideo` (`GtkGstMediaFile` / `libgstplay-1.0`), which leaks corked PipeWire `Playback Stream` clients (`pw-top`) and crashes inside the `GstPlay` background thread when rapidly switching files. Call `kajo_player_mpv_stop()` when Quick Look is hidden or switched to a non-media item.
  - Idle background CPU usage across `kajo` must remain `< 0.05%`.

### E. Security & Credential Hardening (`pam_auth`, `session_lock`, `kajo-greeter`)
- Copy and immediately clear `GtkPasswordEntry` text before dispatching async PAM or `greetd` verification, reset `show-peek-icon`, and scrub temporary buffers with `explicit_bzero()`.
- Never serialize plaintext passwords through `JsonBuilder` / `JsonGenerator`.
- Always call `gtk_window_set_interactive_debugging(FALSE)` on lock and greeter windows.

---

## 5. Modular Codebase Standards

1. **No Monolithic Files**:
   - Settings pages live in `src/settings/pages/page_*.c`.
   - File browser subsystems are split across `src/files/{main,files_window,files_model,files_ops,files_sidebar,files_quicklook}.c`.
   - Panel widgets live in `src/widgets/*.c` and **MUST** define both `.id` and `.name` in their `ShellWidgetClass` struct.
2. **No Mock Data or Hardcoded D-Bus Paths**:
   - Every UI list must query live system state (NetworkManager, BlueZ `GDBusObjectManager`, PipeWire, UPower, `GVolumeMonitor`, Niri IPC).
3. **Zero Memory / `GVariant` / Widget Leaks**:
   - Use `g_autoptr` / `g_autofree` consistently, unref `GVariant`s and `GFileInfo`s, cancel in-flight `GCancellable`s in `dispose()`, and remove existing child widgets before repopulating dynamic containers.

---

## 6. Git Branching, Commit & PR Workflow

1. **Feature / Bugfix Branches Only**:
   - Never commit directly to `master` unless explicitly instructed. Create a descriptive branch (`feat/<name>`, `fix/<name>`).
2. **Wait for Explicit PR / Merge Instruction**:
   - Keep commits local until the user asks to open a PR or push.
   - When asked to open a PR: push the branch (`git push -u origin <branch>`) and run `gh pr create`.
   - When asked to merge: run `gh pr merge <num> --merge --delete-branch`, verify `master` is updated locally, and keep `README.md`, `DEVELOPMENT.md`, and `ROADMAP.md` synchronized with the merged state.
