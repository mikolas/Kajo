# Kajo Compositor (`kajo-comp`) Specification

> **Status:** v1 Normative Specification
> **Target Repository:** `/home/mikolas/src/Kajo` (branch `feat/kajo-comp`)
> **Implementation Language:** Pure C11 (Meson, `wlroots-0.20`, Vulkan, `tomlc17`; GLib data helpers only outside the compositor event loop)

---

## 1. Overview, Ground Rules & Target Hardware

`kajo-comp` (`org.kajo.Compositor`) is the first-party Wayland compositor for the Kajo desktop suite (`kajo`, `kajo-settings`, `kajo-greeter`, `kajo-files`, `kajo-player`, `kajo-mod`, `kajo-ide`).

### 1.1 v1 Design Scope

1. **SDR Output on Stock `wlroots 0.20.2` (Vulkan Forced):**
   - Session mode forces the `wlroots` Vulkan renderer (`WLR_RENDERER=vulkan` unless `WLR_RENDERER` is already set in the environment for development or automated tests). If the Vulkan renderer cannot be created in session mode, `kajo-comp` logs the error and exits non-zero (no silent GLES2 fallback).
   - Output color pipeline in v1 is strictly 8-bit SDR. HDR10 is deferred to v2 (§9).
2. **Scrolling-Tiling Layout with Spring-Driven Motion:**
   - Horizontal strip of columns per workspace, vertical stack of dynamic workspaces, plus one per-workspace floating layer.
   - Analytical 2nd-order spring engine driving camera and window **positions** (`workspace_switch`, `horizontal_scroll`, `window_movement`) with instantaneous velocity preservation on mid-flight retargeting.
3. **Single-Monitor Session Policy across Two Form Factors:**
   - A user session drives **one active output at a time** (§3.13 `OUT1–OUT4`). Greeter mode (`--greeter-mode`, §6.5) is the sole exception and enables all connected outputs left-to-right so `kajo-greeter` can present a layer-shell surface on each monitor.
   - **Host Profile A — 14" 16:10 Convertible OLED Laptop:** HP OmniBook X Flip 14 (`eDP-1`, Samsung `0x41AA`, `2880×1800 @ 120Hz`, scale `1.75` $\rightarrow$ `1646×1029` logical px), Intel Lunar Lake Xe2, touchpad + touchscreen + `ELAN2514` stylus + IIO hinge/posture rotation + `/sys/class/backlight/*` EC backlight.
   - **Host Profile B — 34" 21:9 Ultrawide Desktop:** external `DP-*` / `HDMI-A-*` (`3440×1440` or `5120×2160`, scale `1.0–1.25`, discrete GPU, `vrr = "on-demand"`).
4. **Domain-Split TOML Configuration & Live Hot-Reload:**
   - Compositor configuration lives in `~/.config/kajo/{comp,outputs,binds,rules}.toml` plus `conf.d/*.toml` overrides (§5), hot-reloaded without restarting the compositor via `inotify` on `wl_event_loop`.

### 1.2 Engineering Ground Rules

- **R1 — Normative Spec:** Nothing in this specification is simplified or altered silently in code. Any implementation gap is raised before changing behavior.
- **R2 — Quality Gate:** Every commit must pass `meson compile -C builddir` (0 errors, 0 warnings), `meson test -C builddir`, and `scripts/asan-test.sh` (ASan + UBSan + LSan).
- **R3 — Live-Session Safety:** No test or development command may act on the user's running desktop session. Automated tests use `WLR_BACKENDS=headless` or isolated nested `WLR_BACKENDS=wayland` instances with private `WAYLAND_DISPLAY` and `KAJO_COMP_SOCKET` names.

---

## 2. Technical Architecture (`wlroots 0.20` + Vulkan)

### 2.1 Compositor Foundation Selection

| Option | Language | Verdict |
| :--- | :---: | :--- |
| **`wlroots 0.20.2` + Vulkan (`wlr_vk_renderer`)** | **Pure C11** | **Selected.** Native C11/Meson/ASan fit. Provides `wlr_scene` damage tracking, explicit sync (`linux-drm-syncobj-v1`), tearing control, XWayland, layer-shell, session-lock, input-method, fractional scale, capture protocols, and color management. |
| **`scenefx` on `wlroots`** | C | **Rejected.** GLES-only (`mesa`/`libglvnd`); incompatible with the Vulkan renderer path. |
| **Forked/patched `wlroots` Vulkan renderer** | C | **Deferred (§9).** Required only for multi-pass backdrop blur and custom window shaders. |
| **C++ frameworks (`Aquamarine`, `libkwin`, `Louvre`, `Mir`)** | C++ | **Rejected.** Mismatched language/toolkit stack (`wlroots` C99 `static` array declarators require shims in C++; `libkwin` pulls Qt6). |

### 2.2 Core `wlroots 0.20` Subsystem Map

| Subsystem | `wlroots 0.20` API / Protocol | Role in `kajo-comp` |
| :--- | :--- | :--- |
| **Backend & Seat** | `wlr_backend_autocreate()`, `libseat`, DRM, `libinput`, Wayland backend, headless backend | Bare-metal KMS/DRM via `greetd`/`kajo-session`; nested (`WLR_BACKENDS=wayland`) for development; `headless` for automated tests. |
| **Renderer** | `wlr_renderer_autocreate()` | Session mode forces `WLR_RENDERER=vulkan` unless `WLR_RENDERER` is already set in the environment; exits non-zero if creation fails. Greeter mode uses automatic renderer selection (§6.5.4). |
| **Render Pass** | `wlr_render_pass_add_texture()` / `add_rect()` (via `wlr_scene`) | Stock `wlr_scene` primitives: sampled texture with alpha, source/dest box, opacity, and solid rectangles (`0px` corner radius). |
| **GPU Sync & Scanout** | `wlr_linux_drm_syncobj_v1`, `wlr_tearing_control_v1`, `wlr_output_layer` / scene direct scanout | Explicit GPU timeline sync, optional tearing flips, and automatic fullscreen direct scanout via `wlr_scene`. |
| **Scene Graph** | `wlr_scene`, `wlr_scene_tree`, `wlr_scene_rect`, `wlr_scene_buffer`, `wlr_scene_surface` | Automatic damage tracking. `wlr_scene_node_set_position()` uses integer logical coordinates. Window frames use `1px` `0px`-radius `wlr_scene_rect` nodes (`#2b2b2b` inactive, `@metro-accent` focused, `#e81123` urgent). |
| **Color Management** | `wlr_color_manager_v1_create()`, `wlr_scene_set_color_manager_v1()`, `wlr_color_representation_v1` | Advertises renderer-supported transfer functions and primaries to clients and handles YCbCr color representation. |
| **Shell Surfaces** | `wlr_xdg_shell`, `wlr_layer_shell_v1`, `wlr_session_lock_v1` | Hosts tiled/floating application windows, `kajo` layer surfaces (panel, launcher, notifications, OSD, OSK, screensaver), `kajo --lock`, and `kajo-greeter`. |
| **X11 Compatibility** | `wlr_xwayland` | On-demand XWayland support for Steam, Wine/Proton, and legacy X11 clients, including `force_zero_scaling` on HiDPI outputs (§2.5). |
| **OSK & Text Input** | `wlr_input_method_v2`, `wlr_virtual_keyboard_v1`, `wlr_text_input_v3` | Input-method and virtual-keyboard relay for `ShellOSK` (`src/osk/osk.c`). |
| **Scaling & Capture** | `wlr_fractional_scale_v1`, `wlr_viewporter`, `wlr_ext_image_copy_capture_v1`, `wlr_ext_image_capture_source_v1`, `wlr_screencopy_v1` | Fractional scale (`1.75` on 14" OLED, `1.0–1.25` on 34" ultrawide). Single-window capture uses `wlr_ext_image_capture_source_v1_create_with_scene_node()`. |
| **Idle & DPMS** | `wlr_idle_notify_v1`, `wlr_idle_inhibit_v1`, `wlr_output_power_management_v1` | Drives `src/lock/screensaver.c`, DPMS output power states, and media/call idle inhibition. |

### 2.3 Scene Tree Hierarchy

```text
root (wlr_scene)
├── layer_background       (zwlr_layer_shell_v1: BACKGROUND)
├── layer_bottom           (zwlr_layer_shell_v1: BOTTOM)
├── workspace_viewport     (active workspace; camera_x / switch_y animated by spring engine)
│   └── column[0..N-1]     (x_pos, target_width_px, tabbed_mode, side tab indicator)
│       └── window[0..M-1] (1px 0px-radius border wlr_scene_rect + wlr_scene_surface + xdg popups)
├── floating_tree          (floating layer of active workspace: dialogs, PiP, calculators, Steam toasts, 🗗 FLOAT windows)
├── layer_top              (kajo panel, launcher)
├── fullscreen_tree        (focused stationary fullscreen window + black backdrop; direct-scanout eligible)
├── layer_overlay          (kajo OSD, notifications, Alt+Tab switcher, in-compositor screenshot UI)
└── session_lock_tree      (ext-session-lock-v1: obscures and blocks all lower trees while locked)
```

### 2.4 Window Decorations (`prefer-no-csd`) & GPU Buffer Sharing

1. **Zero-Copy DMABUF Sharing:** GTK4 (`ngl`/`vulkan`), Kitty, browsers, and VA-API/NVDEC video decoders pass `dmabuf` file descriptors directly to `wlr_vk_renderer` via `linux-dmabuf-v1` and `linux-drm-syncobj-v1`.
2. **Server-Side Decoration Negotiation (`wlr_xdg_decoration_manager_v1` + `wlr_server_decoration`):**
   - `kajo-comp` advertises `SERVER_SIDE` (`prefer-no-csd`) and sets `tiled` edges on all windows.
   - Kajo suite applications retain their frameless Aero + Metro `GtkWindowHandle` header bar.
   - Terminals, browsers, Qt, Electron, `mpv`, and XWayland clients omit toolkit titlebars; CSD-only GTK4/Libadwaita clients honor `tiled` states by squaring corners and dropping client shadows.
3. **Borders & Tab Indicators:**
   - Every tiled and floating window (except fullscreen and maximized-to-edges windows) is framed by a `border_px` (`1px` default) `0px`-radius `wlr_scene_rect` (`@metro-accent` active, `#2b2b2b` inactive, `#e81123` urgent).
   - A tabbed column (`Mod+W`) draws a non-textual vertical tab bar indicator outside the left edge of the column without altering client surface dimensions (§3.9 `T3`).

### 2.5 Application Compatibility & XDG Desktop Portal Wiring

1. **XDG Desktop Portals & Environment Activation:**
   - Ship `data/kajo-portals.conf` routing `org.freedesktop.impl.portal.ScreenCast=wlr` and `org.freedesktop.impl.portal.Screenshot=wlr` to `xdg-desktop-portal-wlr`, `org.freedesktop.impl.portal.Secret=gnome-keyring`, and `default=gtk`.
   - Expose `wlr_screencopy_v1`, `wlr_ext_image_copy_capture_v1`, `wlr_output_management_v1`, and `wlr_xdg_output_v1` for PipeWire screen sharing (OBS, WebRTC, Discord, Zoom).
   - Export `XDG_CURRENT_DESKTOP=kajo` and update `systemd --user` / D-Bus activation environment immediately once sockets bind (§6.4) so GTK/Electron/Flatpak startups never block on D-Bus portal timeouts.
2. **3D Games, Proton/Steam & DCC Viewports (`pointer-constraints-v1` + `relative-pointer-v1`):**
   - Implement `wlr_pointer_constraints_v1` (pointer lock and confinement regions) and `wlr_relative_pointer_v1` (unaccelerated deltas).
3. **Cross-Process Portal Dialog Parenting (`xdg-foreign-v2` + `xdg-dialog-v1`):**
   - Wire `wlr_xdg_foreign_registry`, `wlr_xdg_foreign_v2`, and `wlr_xdg_dialog_v1` so portal file choosers parent to the requesting window and open centered on the floating layer (§3.3 `O3`, §3.11 `L3`).
4. **Media & Call Idle Inhibition (`idle-inhibit-v1`):**
   - Track mapped and visible `wlr_idle_inhibitor_v1` surfaces and update `wlr_idle_notifier_v1_set_inhibited()` so `kajo` screensaver and DPMS timers stay suppressed during active playback or calls.
5. **Unscaled Native-DPI XWayland (`xwayland.force_zero_scaling`):**
   - When `force_zero_scaling = true` on a fractional-scale output (e.g. `scale = 1.75` on the 14" `2880×1800` OLED), advertise native physical pixel dimensions to XWayland and set `Xft.dpi` via `xrdb` so DPI-aware X11 clients (Steam, Wine/Proton) render at 1:1 physical pixels instead of bilinearly upscaling.
6. **Client Toolkit Globals:**
   - `wlr_single_pixel_buffer_v1`, `wlr_viewporter`, `wlr_subcompositor`, `wlr_alpha_modifier_v1`.
   - `wlr_presentation_time` (`wp-presentation` for `mpv`/`kajo-player` A/V sync and `VK_KHR_present_wait`).
   - `wlr_pointer_gestures_v1` (2-finger touchpad pinch/rotate forwarding to clients).
   - `wlr_keyboard_shortcuts_inhibit_v1` (allows VM/remote-desktop clients such as QEMU/`virt-manager`/Moonlight to capture compositor shortcuts, with built-in **`Mod+Escape`** (`break-keyboard-shortcuts-inhibit`) unconditionally revoking the active inhibitor grab).
   - `wlr_tablet_v2` (stylus pressure/tilt for the OmniBook `ELAN2514` pen, transformed with the active output rotation).
   - `wlr_drm_lease_v1` (VR headset KMS connector leasing).

### 2.6 Memory Model: Pure Core Arrays + Compositor-Layer `talloc` & Generational IDs

`kajo-comp` separates pure layout state from `wlroots` object lifetimes:

1. **Pure Core (`src/comp/core/`):**
   - Contains **zero** `wlroots`, Vulkan, `talloc`, or GLib main-loop headers.
   - Owns plain dynamic arrays (`workspaces` $\rightarrow$ `columns` $\rightarrow$ `windows`, plus one `floating` array per workspace) freed by explicit `*_free()` functions so unit tests and `libFuzzer` targets verify zero leaks under ASan/LSan.
2. **Compositor Layer (`src/comp/`):**
   - Uses `talloc` (`libtalloc`, hierarchical pool allocator with deterministic destructors) for server-owned Wayland wrapper structs, paired with a generational slot table (`CompWindowSlots`) and a 64 KB per-frame bump scratch arena:
     ```text
     CompServer (root talloc context)
     ├── server_listeners_ctx    # Freed FIRST in server teardown before wlr_backend/seat/cursor destroy
     ├── CompConfig              # Reloaded atomically: talloc_free(old_cfg) frees all strings, rules, binds & regexes
     ├── CompOutput              # Single active session output; destructor releases swapchain state
     ├── CompLayout (pure core)  # Plain arrays: workspaces -> columns -> window IDs + floating layer
     ├── CompWindowSlots         # Generational slot table: uint64_t {slot, gen} -> CompWindow*
     ├── CompWindow              # talloc destructor unlinks all wl_listeners & calls wlr_scene_node_destroy()
     │   ├── title / app_id      # talloc_strdup(win, ...)
     │   ├── focus_timestamp     # Monotonic timestamp for Alt-Tab MRU ordering
     │   ├── CompPopup[0..P-1]
     │   └── CompAnimState       # Displayed rect + springs; destroyed automatically with the window
     ├── CompLayerSurface
     ├── CompIpcClient           # Socket EOF -> talloc_free(client) frees buffers, write queue & subscriptions
     └── frame_scratch_arena     # 64 KB bump buffer reset at the end of each output frame
     ```
3. **Four Lifetime Invariants:**
   - **Auto-Unlinking `comp_listen(owner_ctx, &wlr_obj->events.foo, fn)`:** Allocates a child listener on `owner_ctx` whose `talloc` destructor automatically calls `wl_list_remove(&l->link)`. Raw `struct wl_listener` fields with manual cleanup are forbidden.
   - **Strict Teardown Ordering:** `CompWindow` destructors call `wlr_scene_node_destroy()` before child allocations release; server-level listeners on `wlr_backend`, `wlr_seat`, and `wlr_cursor` live in `server_listeners_ctx` and are freed before destroying the underlying `wlroots` objects (`wlroots 0.20` asserts listener lists are empty on destroy). Signal event sources on `wl_event_loop` are removed explicitly before `wl_display_destroy()`.
   - **`uint64_t` Generational Window IDs (`{ uint32_t slot, uint32_t gen }`):** Never store raw `CompWindow *` pointers in the pure layout core, IPC requests, `Alt+Tab` MRU lists, timers, or async screenshot jobs. Resolve IDs in $O(1)$ via `comp_win_from_id(server, id)`, which returns `NULL` if the window closed.
   - **Live Allocation Introspection (`kajo-comp msg debug-mem`):** Dumps `talloc_report_full(server, ...)` over the IPC socket.

---

## 3. Scrolling-Tiling Layout & Spring Motion Specification

This section defines the complete normative layout and animation behavior implemented by `src/comp/core/comp_layout.c` and `src/comp/core/comp_motion.c`. Every rule is deterministic and testable without a display server.

### 3.1 Geometry (`G1–G7`)

- **G1 (Column Strip):** Each workspace presents a horizontal strip of columns ordered left-to-right. Each column holds one or more windows stacked top-to-bottom, or arranged as tabs (§3.9). The strip scrolls horizontally and never wraps.
- **G2 (Gaps):** `gap_px` (default `6`) separates neighboring columns, neighboring stacked windows within a column, and windows from the working-area edges (top and bottom always; left and right when the view rests against a column edge per `V1`). Gaps round to whole physical pixels on the active output.
- **G3 (Column Widths):**
  - A proportional width `p` produces a tile width (including borders) of:
    $$\text{tile\_width} = \lfloor p \times (\text{working\_width} - \text{gap\_px}) - \text{gap\_px} \rfloor$$
    so $n$ columns of proportion $1/n$ fit the working area exactly with all inner and outer gaps included.
  - A fixed pixel width (e.g. `"800"`) specifies the client surface width; `2 * border_px` is added to form the tile width (`802` px with `border_px = 1`).
- **G4 (Window Heights within a Column):**
  - Windows in a stacked column share the available height ($\text{working\_height} - (M + 1) \times \text{gap\_px}$ for $M$ windows) by weight, equal by default.
  - At most **one** window in a column may hold an explicit height (preset, pixel, or percentage). Setting an explicit height on a window converts any other explicit-height window in that column back to shared weight, preserving the relative weight ratio among shared windows, and clamps the explicit height so remaining windows retain their minimum height.
  - A lone window in a column may also be given an explicit height smaller than the column; it aligns to the top of the column (`y = working_top + gap_px`).
  - When a window closes and one window remains in the column: if the survivor uses shared height, it expands to fill the column; if it holds an explicit height, it retains that explicit height.
- **G5 (Rounding & Bounds):** Calculated window widths round down (`floor`) to whole logical pixels; window heights round to nearest (`round`) whole logical pixels. Client minimum and maximum size hints (`xdg_toplevel` min/max size) are respected.
- **G6 (Borders):** A `border_px` (`1px` default) rectangular border is drawn around every tiled and floating window, and omitted on fullscreen and maximized-to-edges windows.
- **G7 (Oversized Windows):** A window wider or taller than the working area aligns to the top-left of its allocated slot so its top-left region remains visible.

### 3.2 Horizontal View / Camera (`V1–V8`)

Each workspace maintains its own horizontal camera position (`view_x`).

- **V1 (`center_focused_column = "never"`, default):** Focusing a column that is already completely visible inside the working area does not move the view. Otherwise, the view scrolls the minimum distance required to bring the focused column fully on screen with one `gap_px` margin at the edge it entered from (choosing whichever side requires the smaller scroll). When a column is nearly as wide as the working area, the edge margin shrinks proportionally; a column at least as wide as the working area aligns its left edge to the working area left edge with zero margin.
- **V2 (`center_focused_column = "always"`):** The focused column is centered horizontally in the working area, or left-aligned if its width is $\ge \text{working\_width}$.
- **V3 (`center_focused_column = "on-overflow"`):** Behaves like `"never"`, except when the newly focused column and the previously focused column (or a newly opened window and the column it opened from) do not fit together within the working area including outer and inner gaps; in that case the newly focused column is centered as in `V2`. Resizing an already-focused column does not trigger `"on-overflow"` centering (`V6`).
- **V4 (`always_center_single_column = true`, default):** While a workspace contains exactly one column, that column is centered as in `V2`. When two or more columns exist (even if their combined width is narrower than the working area), camera positioning follows `center_focused_column` (`V1–V3`). When `always_center_single_column = false`, a single column under `"never"` aligns left with one `gap_px` margin.
- **V5 (Focused Column Stability):** The focused column does not shift on screen when columns to its left open, close, resize, or reorder; the camera offset adjusts simultaneously so the focused column remains stationary on screen while neighboring columns slide.
- **V6 (Post-Resize Visibility):** When the focused column changes width or state (preset switch, maximize, fullscreen, window added/removed), the camera moves the minimum distance required to keep the focused column visible per `V1–V4`.
- **V7 (Fullscreen Alignment):** A fullscreen column spans the full output resolution (`0, 0, output_width, output_height`); when focused, the camera aligns exactly to its left edge.
- **V8 (Immediate Logical State):** Layout and focus mutations take effect immediately in the logical model; only the rendered screen coordinates animate via springs (§3.14).

### 3.3 Opening Windows (`O1–O5`)

- **O1 (Default Tiled Placement):** A new tiled window opens as a new single-window column immediately to the right of the currently focused column and receives focus. Opening the first window on an empty workspace places the camera directly at its initial `V1–V4` position without a horizontal scroll animation.
- **O2 (Opening over Fullscreen):** A newly opened window takes focus even when the currently focused window is fullscreen; the fullscreen window remains fullscreen in its column to the left while the camera scrolls to the new column.
- **O3 (Automatic Floating Placement):** A window opens on the floating layer when it has a parent (`xdg_toplevel` parent or `xdg_foreign_v2` parent), has fixed min == max size hints, or matches a `[[rule]]` with `floating = true` (`focus = false` in a rule suppresses focus transfer). A tiled window spawned with an activation token or parent reference to a specific tiled window opens immediately to the right of that window's column.
- **O4 (Initial Width):** A new tiled column takes its width from a matching window rule's `width`, or `default_column_width` (`0.5` default) otherwise.
- **O5 (Temporary Window View Restoration):** If a tiled window opens to the right of a focused column and is closed while it still holds focus (with zero intervening focus changes), focus returns to the originating column and the camera restores the exact `view_x` it had prior to opening the temporary window. Any focus change clears this saved view state.

### 3.4 Closing Windows (`C1–C3`)

- **C1 (Closing a Single-Window Focused Column):** When the sole window of the focused column closes, focus transfers to the column that slides into its place (its right neighbor, or the new rightmost column if the closed column was last), unless `O5` restores the originating column.
- **C2 (Closing inside a Multi-Window Column):** When a focused window closes inside a column containing multiple windows, focus transfers to the window immediately below it, or to the window above it if the closed window was at the bottom.
- **C3 (Unfocused Close):** Closing an unfocused window never changes the focused window or moves thefocused column on screen (`V5`).

### 3.5 Focus, Pointer Click-to-Focus & Column/Window Movement (`F1–F6`)

- **F1 (Directional Focus):** `focus-column-left`, `focus-column-right`, `focus-column-first`, and `focus-column-last` move focus across columns in the strip (clamping at the ends). `focus-window-up` and `focus-window-down` move focus between stacked windows or tabs inside the focused column (clamping at top/bottom).
- **F2 (Directional Reordering):** `move-column-left`, `move-column-right`, `move-column-to-first`, and `move-column-to-last` reorder the focused column within the strip while retaining focus. `move-window-up` and `move-window-down` swap the focused window with its upper or lower neighbor inside the column.
- **F3 (Floating Layer Routing):** When focus is on the floating layer, directional focus and movement actions operate on floating windows (§3.11 `L4`).
- **F4 (Click-to-Focus & Click Pass-Through):** Pressing any pointer button (`BTN_LEFT`, `BTN_MIDDLE`, `BTN_RIGHT`) or touching (`WL_TOUCH_DOWN`) an unfocused client surface focuses that window immediately **and** forwards the activating button or touch press event to the client surface on the same input frame (unless consumed by a compositor binding or active lock/overlay grab).
- **F5 (Cross-Layer Pointer Focus & Off-Screen Scroll):**
  - Clicking or touching a **floating window** switches active layer focus to the floating layer, raises that window to the top of `floating_tree` (`L5`), and leaves the tiled strip camera (`view_x`) unchanged.
  - Clicking or touching a **tiled window** switches active layer focus to the tiled strip (keeping floating windows rendered above in `floating_tree`), focuses that column and window, and applies `V1–V4` camera rules—scrolling the strip smoothly if the clicked tiled column was only partially visible at the viewport edge.
- **F6 (Pointer Focus During Animations):** Surface hit-testing (`wlr_scene_node_at()`) is re-evaluated at the current animated scene coordinates on pointer motion and at each output frame tick while springs are in flight, updating `wlr_seat_pointer_notify_enter()` / `notify_motion()` without altering keyboard focus.

### 3.6 Consume & Expel (`K1–K4`)

- **K1 (`consume-or-expel-window-left` / `consume-or-expel-window-right`):**
  - If the focused window is the **only** window in its column, it is consumed into the bottom of the adjacent column in the given direction (no-op if no adjacent column exists in that direction).
  - If the focused window shares its column with other windows, it is expelled out of the column to form a new single-window column immediately to the left or right of its former column.
  - In both cases, the moved window retains focus.
- **K2 (`consume-window-into-column`):** Pulls the top window of the column immediately to the right into the bottom of the focused column. Focus does not change.
- **K3 (`expel-window-from-column`):** Expels the bottom window of the focused column into a new single-window column immediately to the right. Focus does not change.
- **K4 (Expelled Column Width):** Any window expelled into its own column inherits the width of the column it left.
- **Default Bindings:** `Mod+Ctrl+Left` and `Mod+Ctrl+Right` bind to `consume-or-expel-window-left` and `consume-or-expel-window-right` (layout-independent across US and Nordic keyboards; `K2`/`K3` remain available via IPC and user bindings).

### 3.7 Widths & Heights (`W1–W6`)

- **W1 (`switch-preset-column-width`):** Cycles the focused column's width through `preset_widths` (`[0.33333, 0.5, 0.66667]` default) in ascending order, wrapping from the last preset back to the first. If the column's current width does not match a preset, it jumps to the first preset wider than its current width (or wraps to the first preset if already wider than all presets).
- **W2 (`maximize-column`):** Toggles the focused column between full working width (`proportion = 1.0`, keeping outer `gap_px` and `border_px`) and its previous unmaximized width.
- **W3 (`expand-column-to-available-width`):** Expands the focused column to fill the remaining horizontal working area not occupied by other currently fully-visible columns, keeping the leftmost fully-visible column anchored at the left margin. If the focused column is the only fully-visible column or `center_focused_column = "always"` is active, it behaves identically to `maximize-column`. If the column is already full-width or fullscreen, it is a no-op.
- **W4 (`set-column-width` / `set-window-height`):** Accepts a `delta` string argument:
  - Signed percentage (`"+10%"`, `"-10%"`): adds/subtracts percentage points of the working dimension.
  - Unsigned percentage (`"50%"`): sets an exact proportion of the working dimension.
  - Signed pixels (`"+20"`, `"-20"`): adds/subtracts logical pixels.
  - Unsigned pixels (`"800"`): sets exact client surface size in logical pixels.
  - Bare decimals without `%` (e.g. `"0.25"`) are rejected by the parser.
- **W5 (`switch-preset-window-height` & `reset-window-height`):** `switch-preset-window-height` cycles the focused window's explicit height through `preset_heights` (`[0.33333, 0.5, 0.66667, 1.0]` default), jumping to the first preset taller than the current height if not currently on a preset (works on both multi-window and single-window columns per `G4`). `reset-window-height` clears the focused window's explicit height back to shared weight (in a tabbed column, resets the column's height to full column height).
- **W6 (Floating Sizing):** When a floating window is focused, width and height preset/delta commands resize the focused floating window (proportions are relative to the working area) while keeping it clamped inside the working area (§3.11 `L6`).

### 3.8 Centering Commands (`Z1–Z2`)

- **Z1 (`center-column`):** Scrolls the camera to center the focused column horizontally in the working area. When the floating layer is focused, centers the focused floating window within the working area.
- **Z2 (`center-visible-columns`):** Scrolls the camera to center the contiguous bounding box of all currently fully-visible columns as a group. No-op if `center_focused_column = "always"` is active or if the focused column itself is not fully visible.

### 3.9 Tabbed Columns (`T1–T3`)

- **T1 (`toggle-column-tabbed`):** Switches the focused column between vertical stacking and tabbed display. In tabbed mode, only the active tab's surface is visible and receives the full column height; `focus-window-up/down` cycle the active tab; `move-window-up/down` reorder tabs; consume and expel add and remove tabs.
- **T2 (Fullscreen Interaction):** A tabbed column may be made fullscreen while keeping all its tabs grouped; toggling a fullscreen tabbed column back to stacked mode exits fullscreen first.
- **T3 (Tab Indicator Geometry & Colors):**
  - Rendered as a vertical column of $M$ `0px`-radius `wlr_scene_rect` segments immediately outside the column's left border and does not reduce client surface width.
  - **Metrics (logical px):** indicator strip width = `4px`, horizontal position = `col_left - 6px` (leaving a `2px` gap between the indicator and the column's outer border), vertical gap between segments = `2px`, total span = full column height (`col_bottom - col_top`) divided equally across the $M$ tab segments.
  - **Colors:** active tab segment = `active_border_color` (`@metro-accent`), urgent inactive tab segment = `urgent_border_color` (`#e81123`), normal inactive tab segment = `#444444`.

### 3.10 Fullscreen & Maximize-to-Edges (`S1–S6`)

- **S1 (`fullscreen-window`):** Toggles fullscreen state on the focused window. The column remains in its position in the workspace strip (the user can scroll away to neighboring columns and scroll back), covering the full output (`output_width × output_height`) without borders over a solid `#000000` backdrop.
- **S2 (Promoting from a Multi-Window Stacked Column):** If a window inside a multi-window stacked column enters fullscreen, it is first expelled into its own column immediately to the right and remains in that column after leaving fullscreen.
- **S3 (Camera Restoration):** Entering fullscreen records the current `view_x`; exiting fullscreen without an intervening focus change restores that `view_x`.
- **S4 (Fullscreening a Floating Window):** Fullscreening a floating window temporarily places it into the strip as a fullscreen column; exiting fullscreen returns the window to the floating layer at its saved floating position and size.
- **S5 (Maximize-to-Edges):** Triggered via client `xdg_toplevel.set_maximized` request; sizes the window to the full working area without gaps or borders while keeping it below the floating and `TOP` layer-shell layers. Rules `S2–S4` apply identically.
- **S6 (Layer Stacking Order):** While a fullscreen column is focused and stationary at `view_x`, it renders in `fullscreen_tree` above `floating_tree` and `layer_top` (`kajo` panel), allowing direct scanout when a single matching surface covers the output.

### 3.11 Floating Layer (`L1–L8`)

Each workspace has one floating layer rendered above the tiled column strip (`floating_tree`).

- **L1 (Viewport Independence):** Floating windows do not scroll with `view_x` and remain above tiled columns.
- **L2 (`toggle-window-floating`):** Moves the focused window between the tiled strip and the floating layer:
  - **First float from tiled:** Keeps its tiled pixel dimensions and places the window offset by **`+50 px` right and `+50 px` down** from its current on-screen tiled coordinates, clamped by `L6`.
  - **Subsequent floats:** Restores the window's last floating size and working-area-relative position.
  - **Return to tiled:** Inserts the window as a new single-window column immediately to the right of the last focused tiled column, preserving its floating width.
- **L3 (Initial Floating Position):** A window that opens directly on the floating layer centers in the working area (unless a matching `[[rule]]` specifies `anchor_corner`, `anchor_x`, and `anchor_y`). A child dialog opens centered over its parent window and takes focus.
- **L4 (`switch-focus-floating-tiling` & Floating Navigation):** Toggles focus between the floating layer and the tiled strip (no-op if the target layer has no windows). While the floating layer is focused:
  - `focus-column-left/right` and `focus-window-up/down` move focus to the nearest floating window whose center lies in that direction; `focus-column-first/last` focus the leftmost/rightmost floating window.
  - `move-column-left/right` and `move-window-up/down` translate the focused floating window by **`50 px`** in that direction, clamped by `L6`.
  - Column-only actions (`consume-*`, `expel-*`, `toggle-column-tabbed`, `move-column-to-first/last`, `maximize-column`, `expand-column-to-available-width`, `center-visible-columns`, `reset-window-height`) are no-ops.
- **L5 (Stacking Order):** Focusing a floating window raises it to the top of `floating_tree`.
- **L6 (Working-Area Clamping & Proportional Persistence):** At least $\clamp(\lfloor \text{dim} / 4 \rfloor, 10\text{ px}, 75\text{ px})$ of a floating window's width and height must remain inside the working area on all four edges. Floating positions are stored as fractions of the working area so they adapt cleanly across output resolution or panel exclusive-zone changes.
- **L7 (Automatic Layer Fallback):** Closing the last window on the currently focused layer (tiled or floating) automatically switches focus to the other layer if it is non-empty.
- **L8 (Client-Initiated Interactive Move & Resize):** Client `xdg_toplevel.request_move` and `xdg_toplevel.request_resize` grabs (such as dragging the `GtkWindowHandle` header bar on a floated Kajo suite window after clicking `🗗 FLOAT / TILE`) are supported in v1 for **floating windows**, clamping the final geometry by `L6`. (`xdg_toplevel.request_move` / `request_resize` on tiled windows is ignored).

### 3.12 Dynamic Vertical Workspaces (`WS1–WS7`)

- **WS1 (Trailing Empty Workspace Invariant):** The vertical workspace list always maintains an empty workspace at the bottom. Opening or moving a window onto that bottom workspace immediately appends a new empty workspace below it.
- **WS2 (Empty Intermediate Workspace Reaper):** Any workspace above the bottom one that becomes empty is automatically removed once focus switches away from it and the vertical workspace switch animation completes.
- **WS3 (`focus-workspace-up` / `focus-workspace-down`):** Switches the active workspace up or down. Keyboard and pointer focus transfer to the target workspace immediately while the vertical transition animates.
- **WS4 (`move-column-to-workspace-up` / `move-workspace-to-workspace-down`):** Moves the focused column (or, when the floating layer is focused, the focused floating window) to the target workspace and switches focus to follow it. Moving onto the bottom empty workspace appends a new empty workspace per `WS1`.
- **WS5 (`move-workspace-up` / `move-workspace-down`):** Reorders the active workspace with its upper or lower neighbor without playing a vertical switch animation. Moving the bottom empty workspace upward keeps it focused and creates a new trailing empty workspace at the bottom.
- **WS6 (Dynamic Indexing):** Workspaces are indexed `1..K` dynamically by their current vertical order (no fixed workspace numbers or named workspaces in v1).
- **WS7 (Vertical Switch Geometry):** During a workspace switch animation, adjacent workspaces are separated vertically by a gap of $\lfloor 0.10 \times \text{output\_height} \rfloor$.

### 3.13 Single-Output Selection & Hotplug Policy (`OUT1–OUT4`)

- **OUT1 (Single Active Session Output):** In session mode, `kajo-comp` drives at most one output at a time; all workspaces belong to that output.
- **OUT2 (Deterministic Output Priority):** When multiple monitors are connected, `kajo-comp` enables the first connected output matching an `[[output]]` block's `match` field in `comp.toml` / `outputs.toml` / `conf.d/*.toml` (in load order, matched case-insensitively by connector name or EDID `Make Model`); if none matches, it prefers an internal panel (`eDP-*`), and otherwise the first connected connector. All other connected outputs remain disabled.
- **OUT3 (Disconnect & Reconnect Preservation):** If the active output disconnects and another output is connected, `OUT2` selects the replacement and transfers all workspaces, windows, and camera states intact. If zero outputs remain connected, all workspaces and client windows stay alive in memory and re-bind when an output connects.
- **OUT4 (Geometry Reflow):** Changes to output mode, scale, transform, or layer-shell exclusive zones preserve the workspace/column/window tree, recomputing proportional tile sizes and re-applying `V1–V4` camera constraints.

### 3.14 Spring Motion Engine (`M1–M5`)

All animations in v1 are driven by an analytical 2nd-order spring solver (`src/comp/core/comp_spring.c`, parameterized by `damping_ratio`, `stiffness`, and settling threshold `epsilon`):

- **M1 (Zero Discontinuous Jumps):** Every change to a window's or camera's logical target position retargets its active spring from its current displayed position **and preserves its instantaneous velocity**. Two retargets within a single frame must never introduce a velocity discontinuity.
- **M2 (Three Spring Profiles):**
  - `workspace_switch`: `damping_ratio = 0.80`, `stiffness = 523`, `epsilon = 0.0001`
  - `horizontal_scroll`: `damping_ratio = 0.85`, `stiffness = 423`, `epsilon = 0.0001`
  - `window_movement`: `damping_ratio = 0.75`, `stiffness = 323`, `epsilon = 0.0001`
- **M3 (Stationary Invariant):** Windows whose on-screen coordinates do not change as a result of an action (`V5`) remain at zero velocity and do not oscillate.
- **M4 (Cross-Workspace Window Transfer):** A column or floating window moved to another workspace (`WS4`) animates smoothly from its prior on-screen coordinates into its target slot on the destination workspace.
- **M5 (Immediate Resize, Animated Neighbors):** On width or height changes, the resized client receives a single `xdg_toplevel.configure` with its final target size immediately, while neighboring columns and windows slide into their new positions via `window_movement` springs.

### 3.15 In-Process Touch & Touchpad Gestures, Wheel Cooldown & Tablet Posture

1. **Touchscreen Multi-Finger & Bezel Gesture State Machine:**
   - Handled directly inside `kajo-comp`'s `libinput` touch handler (matching the state machine in `src/hardware/touch_gestures.c`, which disables its client-side `/dev/input/event*` reader when `KAJO_COMP_SOCKET` is active).
   - **Multi-Finger Re-Anchoring:** Up to `10` active touch slots are tracked. Whenever the active finger count increases (`1 -> 2 -> 3`), all active slot origins (`start_x`, `start_y`) re-anchor to their current positions so multi-finger displacement is measured cleanly.
   - **3-Finger Global Swipes (`SWIPE_3F_THRESHOLD_MM = 15.0`):** When `active_count == 3` and average physical displacement (rotated by the active output transform `0/90/180/270`) reaches $\ge 15.0\text{ mm}$ with dominant-axis dominance ($|\Delta_{\text{dom}}| > 1.25 \times |\Delta_{\text{ortho}}|$):
     - Swipe Up ($\Delta y < 0$) $\rightarrow$ `focus-workspace-down` (natural scroll)
     - Swipe Down ($\Delta y > 0$) $\rightarrow$ `focus-workspace-up`
     - Swipe Left ($\Delta x < 0$) $\rightarrow$ `focus-column-right` (or `focus-workspace-down` when `shell.toml` `[touch].swipe_horizontal = "workspace"`, no-op when `"none"`)
     - Swipe Right ($\Delta x > 0$) $\rightarrow$ `focus-column-left` (or `focus-workspace-up` when `"workspace"`, no-op when `"none"`)
   - **1-Finger Top/Bottom Bezel Swipe (`EDGE_BEZEL_RATIO = 0.032`, `EDGE_SWIPE_MM = 12.0`):** When a 1-finger touch begins within `3.2%` of the rotated top or bottom screen edge and moves inward by $\ge 12.0\text{ mm}$ ($|\Delta y| > 1.3 \times |\Delta x|$), `kajo-comp` emits `{"event":"TouchBezelSwipe","edge":"top"|"bottom","x_ratio":<0.0..1.0>}` to subscribed `kajo-comp.sock` clients (§5.3) so `kajo` reveals the autohide panel (`shell_autohide_touch_reveal()`) or triggers the configured edge action (`swipe_center` / `swipe_right`).
   - **Client Touch Cancellation:** The instant a 3-finger swipe or 1-finger bezel swipe crosses its activation threshold, `kajo-comp` latches `gesture_triggered = true` until all fingers lift and sends `wlr_seat_touch_notify_cancel()` to the client surface so the underlying application never processes stray taps or scrolls.
2. **Touchpad 3-Finger Swipes (`LIBINPUT_EVENT_GESTURE_SWIPE_*`):**
   - Accumulates `libinput_event_gesture_get_dx()` / `get_dy()` on 3-finger touchpad swipe events. Once cumulative displacement reaches `40.0` logical units along the dominant axis ($|\Delta_{\text{dom}}| > 1.25 \times |\Delta_{\text{ortho}}|$), triggers `focus-column-left/right` (horizontal) or `focus-workspace-up/down` (vertical), inverting direction when `input.touchpad.natural_scroll = true` (default), and latches until `LIBINPUT_EVENT_GESTURE_SWIPE_END`.
3. **Wheel Bind Cooldown:**
   - Discrete and high-resolution wheel bindings support `cooldown_ms` (`150` ms default) so a single flick of a free-spinning wheel steps one workspace at a time.
4. **Convertible Posture Rotation:**
   - `src/posture/posture_manager.c` sends `set-output-transform` (`normal`, `90`, `180`, `270`) over `kajo-comp.sock`, rotating both the output scene and `libinput` touch/tablet (`ELAN2514`) calibration matrices atomically.

---

## 4. In-Compositor UIs & Shell Integration

All in-compositor UI overlays render in `layer_overlay` using `wlr_scene_rect`, mirror `wlr_scene_buffer` nodes, and text labels rasterized via `pangocairo` (`shell_ide_font_stack()`, `Fira Code`) into `wlr_buffer`s.

### 4.1 Alt-Tab Recent-Windows Switcher

1. **Activation & Ordering:**
   - Bound to `Alt+Tab` (`next-window`, `scope = "output"`), `Alt+Shift+Tab` (`previous-window`, `scope = "output"`), and `Alt+grave` / `Alt+Shift+grave` (`filter = "app-id"`).
   - Orders windows by monotonic per-window `focus_timestamp` (most recently focused first).
   - Supported scopes: `"all"` / `"output"` (equivalent under single-output policy `OUT1`) and `"workspace"`. When `filter = "app-id"` is set, only windows matching the currently focused window's `app_id` are included.
2. **Visual Geometry & Aero + Metro Styling (Logical px):**
   - **Placement & Container:** Centered horizontally and vertically on the active output inside `layer_overlay`. Container backdrop is `#111111` at `0.94` opacity with a `1px solid #2b2b2b` `0px`-radius border, `16px` outer padding, and `12px` horizontal gap between cards.
   - **Card Geometry:** Each card is `220×140` logical px (`#181818` fill, `0px` corner radius), containing a centered `204×108` live thumbnail box (aspect-preserving mirror `wlr_scene_buffer` of the client surface) above a `24px` title strip (`Fira Code 10pt` via `pangocairo`, `#ffffff` on the selected card, `#aaaaaa` on unselected cards, single-line ellipsized with `PANGO_ELLIPSIZE_END`).
   - **Card Borders:** Selected card uses a `2px solid` border in `switcher.active_color` (`@metro-accent`, or `switcher.urgent_color` `#e81123` if urgent); unselected cards use `1px solid #2b2b2b` (`#e81123` if urgent).
   - **Horizontal Overflow:** Displays up to $\min(7, \lfloor (\text{output\_width} - 64) / 232 \rfloor)$ cards simultaneously; if more windows exist in the candidate list, the visible card window scrolls horizontally to keep the selected card visible.
   - **Keyboard Interaction:** Holding `Alt` and pressing `Tab` / `Shift+Tab` (or `Left` / `Right` arrow keys) steps the selection; releasing `Alt` commits focus to the selected window (switching workspaces if necessary); pressing `Escape` while `Alt` is held cancels and keeps the original window focused.

### 4.2 In-Compositor Screenshot UI (`screenshot`, `screenshot-window`, `screenshot-screen`)

The screenshot UI is built directly into `kajo-comp` with zero external CLI helper dependencies (`grim`/`slurp` are not used):

1. **`screenshot` (`Mod+Shift+S` / `Print` — Interactive Area Selection):**
   - Freezes the current output frame into a texture and dims the region outside the selection rectangle using four `wlr_scene_rect` quads filled with `rgba(0, 0, 0, 0.55)`.
   - Draws a `1px solid` `@metro-accent` (`0px`-radius) border immediately **outside** the selection rectangle (so border pixels are never included in the captured image) plus eight `6×6` logical px square `@metro-accent` resize handles at the corners and edge midpoints, and sets the cursor to `crosshair`.
   - Renders a `pangocairo` dimension badge (`W × H` in physical pixels, `#111111` background at `0.92` alpha, `1px solid #2b2b2b` border, `6px 8px` padding, `#ffffff` `Fira Code 10pt`) anchored `6px` above the top-left corner of the selection (flipping to `6px` inside the top-left corner when the selection is within `32px` of the top output edge).
   - Supports pointer click-drag to define, move, or resize the selection rectangle, keyboard arrow keys to nudge the selection (`1px` logical per press, `10px` with `Shift`), `Enter` / `Space` to confirm, and `Escape` to cancel.
2. **`screenshot-window` (`Alt+Print` — Isolated Focused Window Capture):**
   - Captures the focused window's own `wlr_scene_tree` (including its `1px` border when `screenshots.include_window_border = true`) via `wlr_ext_image_capture_source_v1_create_with_scene_node()`, excluding any overlapping floating windows or layer-shell panels.
3. **`screenshot-screen` (`Ctrl+Print` — Whole Output Capture):**
   - Immediately captures the entire active output at native physical pixel resolution.
4. **Off-Loop Encoding, Clipboard & Notification Delivery:**
   - Raw pixel readback completes on the render pass; PNG compression (`libpng`), disk write to `screenshots.path` (`~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png`), and `wl_seat` clipboard population (`image/png` when `screenshots.copy_to_clipboard = true`) execute on a background worker thread so `wl_event_loop` never blocks.
   - Upon completion on the main loop, when `screenshots.show_notification = true`, `kajo-comp` emits `{"event":"ScreenshotSaved","path":"<abs-path>","copied":true|false}` to subscribed `kajo-comp.sock` clients (§5.3) so `kajo`'s notification daemon presents the screenshot toast without `kajo-comp` touching D-Bus.

### 4.3 `kajo` Layer-Shell & Panel Parity

1. **GTK4 CSS & Layer Opacity:**
   - `kajo` surfaces (`kajo`, `kajo-launcher`, `kajo-notification`, `kajo-osk`, `kajo-osd`) render premultiplied-alpha DMABUFs styled by `data/defaults/style.css` and `style.d/00-accent.css`.
   - `kajo-comp` applies `[[layer_rule]]` `opacity` and `popup_opacity` (`0.85` default) via `wlr_scene_buffer_set_opacity()`. (Multi-pass background blur is deferred to §9; unsupported keys `blur`, `xray`, and `blur_popups` in `rules.toml` emit a non-fatal diagnostic).
2. **`zwlr_layer_shell_v1` Semantics:**
   - Supports animated margin commits for `src/autohide.c`, exclusive zones for `src/panel.c`, `src/osk/osk.c`, and `src/lock/screensaver.c`, `KEYBOARD_INTERACTIVITY_EXCLUSIVE` for the app launcher, and `KEYBOARD_INTERACTIVITY_NONE` for `ShellOSK`.

---

## 5. Configuration (`*.toml`) & IPC Wire Protocol (`kajo-comp.sock`)

Suite-wide TOML configuration architecture, comment-preserving scalar patching, `flock` write serialization, and legacy `config.json` migration are implemented and specified in [`docs/spec_toml_config.md`](file:///home/mikolas/src/Kajo/docs/spec_toml_config.md). This section specifies `kajo-comp`'s own configuration files and Unix socket protocol.

### 5.1 Suite-Wide TOML Grammar, Drop-In Splitting & Multi-File Merge Semantics

All Kajo configuration files (`theme.toml`, `shell.toml`, `ide.toml`, `files.toml`, `mod.toml`, `comp.toml`, `outputs.toml`, `binds.toml`, `rules.toml`, and `conf.d/*.toml`) follow one coherent, parser-friendly TOML grammar:
- **100% Lowercase Identifiers & Tokens:** All section headers, key names (`snake_case`), enum values (`kebab-case`), action names (`kebab-case`), and key chords (`mod+shift+h`, `mod+wheel-down`, `volume-up`) are lowercase.
- **Zero Inline Curly-Brace Tables (`{ ... }`):** Fixed settings groups use standard `[section]` or `[section.subsection]` tables (`[animations.workspace_switch]`, `[input.keyboard]`); repeatable items use standard `[[array_of_tables]]` blocks (`[[output]]`, `[[bind]]`, `[[rule]]`, `[[layer_rule]]`, and `[[widget]]` in `shell.toml`).
- **Zero Quoted Table Headers & Zero Polymorphic Keys:** Data values are never encoded inside `[section."..."]` headers or left-hand key names, and every key has a single fixed type (always scalar or always array).

No configuration file in `kajo-comp` is required to be a monolithic dump. Configuration is split across small domain files, optional user sub-files (`include`), and lexicographically ordered drop-ins in `conf.d/*.toml`:

```text
~/.config/kajo/
├── comp.toml           # [GUI + Human] Layout, springs, switcher, screenshots, xwayland, env, autostart, input, [[output]]
├── outputs.toml        # [GUI]         GUI-written [[output]] overrides (merged after comp.toml; preserves unattached outputs)
├── binds.toml          # [HUMAN ONLY]  [[bind]] key, mouse wheel & media bindings (never modified by GUI)
├── rules.toml          # [HUMAN ONLY]  [[rule]] & [[layer_rule]] blocks with PCRE2 regex arrays (never modified by GUI)
├── theme.toml          # [GUI + Human] Suite theme (read by kajo-comp for accent_color when border/switcher = "accent")
├── shell.toml          # [GUI + Human] Suite shell prefs & [[widget]] blocks (read by kajo-comp for [touch])
└── conf.d/*.toml       # [HUMAN ONLY]  Modular drop-ins (e.g. 10-laptop.toml, 20-desktop.toml, 30-gaming-rules.toml, 40-binds.toml)
```

1. **Deterministic Load Order & `include` Globs:**
   - Config directory discovery (`$KAJO_CONFIG_DIR`, `$XDG_CONFIG_HOME/kajo`, `~/.config/kajo`) and shipped defaults discovery (`$KAJO_DEFAULTS_DIR`, `$KAJO_COMP_DEFAULTS_DIR`, `$XDG_DATA_HOME/kajo/defaults`, `$XDG_DATA_DIRS/kajo/defaults`, `KAJO_DATA_DIR/defaults`) are identical across `kajo-comp` and `KajoConfigStore`.
   - On startup and hot-reload, `kajo-comp` builds a fresh `CompConfig` from compiled defaults and merges files in strict order:
     1. Shipped defaults in `<defaults_dir>/{theme,shell,comp,outputs,binds,rules}.toml`.
     2. User files in `<config_dir>/{theme,shell,comp,outputs,binds,rules}.toml` (overriding only the keys/rules they specify).
     3. Any relative path or glob listed in top-level `include = ["rules.d/*.toml", "binds.d/*.toml"]` inside a loaded compositor file (expanded in sorted path order, with cycle detection and a max include depth of `4`).
     4. All `<config_dir>/conf.d/*.toml` drop-ins in lexicographic (`strcmp`) filename order. Drop-ins may use either top-level compositor sections (`[layout]`, `[[output]]`, `[[bind]]`, `[[rule]]`, `[[layer_rule]]`) or domain-scoped tables (`[theme]`, `[shell.touch]`, `[comp.layout]`, `[[comp.output]]`, `[[comp.bind]]`, `[[comp.rule]]`, `[[comp.layer_rule]]`) so a single drop-in file works seamlessly across both `KajoConfigStore` and `kajo-comp`.
2. **Multi-File Composition & Distributed Block Merging:**
   - Any compositor section (`[layout]`, `[animations.*]`, `[input.*]`, `[[output]]`, `[[bind]]`, `[[rule]]`, `[[layer_rule]]`, `[env]`, `[autostart]`) may appear in **multiple files** (for example, base rules in `rules.toml`, gaming rules in `conf.d/30-gaming-rules.toml`, and work-app rules in `conf.d/40-work-rules.toml`):
     - **Distributed `[[rule]]` and `[[layer_rule]]` Blocks (Concatenation):** Every `[[rule]]` and `[[layer_rule]]` block across `rules.toml`, `comp.toml`, included files, and `conf.d/*.toml` is **appended in load order** into one combined rule list. When matching a window or layer surface, all matching rules in the combined list apply top-to-bottom per property (`has_*`), so a rule in `conf.d/90-local.toml` overrides only the specific properties (`floating`, `width`, `vrr`, etc.) it sets. Setting `reset_rules = true` at the top of a file clears previously accumulated window rules before appending that file's rules.
     - **Distributed `[[bind]]` Blocks (Chord-Keyed Merge & Unbind):** `[[bind]]` blocks across multiple files merge by normalized lowercase chord (`super+shift+h`, `super+wheel-down`). A `[[bind]]` in a later file replaces an earlier binding for the same chord while leaving all other bindings intact; setting `action = "none"` in a later `[[bind]]` unbinds a previously defined chord.
     - **Distributed `[[output]]` Blocks (Match-Keyed Field Merge):** Multiple `[[output]]` blocks with the same case-insensitive `match` string (connector name `"edp-1"` or EDID `"samsung display corp. 0x41aa"`) across `comp.toml`, `outputs.toml`, and `conf.d/*.toml` merge field-by-field (`mode`, `scale`, `transform`, `vrr`), with later files overriding only the fields they specify.
     - **Scalar Tables (`[layout]`, `[animations.*]`, `[switcher]`, `[screenshots]`, `[xwayland]`, `[input.*]`):** Merge field-by-field across files without resetting unspecified sibling keys.
     - **`[env]` and `[autostart]`:** `[env]` merges by variable name; `[autostart].commands` arrays concatenate in load order (unless `reset = true` is set inside `[autostart]`).
3. **Event-Loop Integration (`inotify`, Zero `GMainLoop`) & Fault Isolation:**
   - `kajo-comp` runs exclusively on `wl_event_loop`. It watches `~/.config/kajo/` and `conf.d/` (plus any included subdirectories) via a non-blocking `inotify` file descriptor registered with `wl_event_loop_add_fd()` (coalesced with a `50 ms` timer) and atomically swaps its `talloc`-allocated `CompConfig` tree.
   - If a base file (`comp.toml`, `binds.toml`, `rules.toml`) has a TOML syntax error on reload, `kajo-comp` logs a structured diagnostic (`file:line:col`) and retains the previous valid `CompConfig` snapshot. If an individual `conf.d/*.toml` drop-in has a syntax error, that drop-in is skipped with a diagnostic while the rest of the configuration loads normally.
4. **GUI Write Isolation & Multi-Host Portability:**
   - GUI saves from `kajo-settings` patch `[layout] gap_px` in `comp.toml` in place, update `[[output]]` rules in `outputs.toml` (preserving unset fields on non-matching rules), and update `[[layer_rule]]` blocks in `rules.toml` (preserving `[[rule]]` window rules and `reset_rules`), and **never** overwrite or flatten `conf.d/*.toml` or `binds.toml`.
   - `[[output]]` blocks for currently unplugged monitors remain dormant in memory and are preserved untouched when `kajo-settings` writes `outputs.toml`. Runtime `wlr-output-management` or IPC output changes survive config reloads unless the matching `[[output]]` rule in `outputs.toml` itself changed.

### 5.2 Reference TOML Schemas

#### 1. `~/.config/kajo/comp.toml`
```toml
# Kajo Compositor Configuration — Outputs, Input & Scrolling Layout

[layout]
gap_px = 6
border_px = 1
active_border_color = "accent"          # Tracks @metro-accent from theme.toml, or explicit "#0078d4"
inactive_border_color = "#2b2b2b"
urgent_border_color = "#e81123"
always_center_single_column = true
center_focused_column = "never"         # "never" | "on-overflow" | "always"
default_column_width = 0.5
preset_widths = [0.33333, 0.5, 0.66667]
preset_heights = [0.33333, 0.5, 0.66667, 1.0]

[animations]
enabled = true

[animations.workspace_switch]
damping_ratio = 0.80
stiffness = 523.0
epsilon = 0.0001

[animations.horizontal_scroll]
damping_ratio = 0.85
stiffness = 423.0
epsilon = 0.0001

[animations.window_movement]
damping_ratio = 0.75
stiffness = 323.0
epsilon = 0.0001

[switcher]
active_color = "accent"
urgent_color = "#e81123"

[screenshots]
path = "~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png"
copy_to_clipboard = true
show_notification = true
include_window_border = true

[xwayland]
enabled = true
force_zero_scaling = true

[env]
XDG_CURRENT_DESKTOP = "kajo"
ELECTRON_OZONE_PLATFORM_HINT = "auto"

[autostart]
commands = [
    ["gnome-keyring-daemon", "--start", "--components=secrets"],
    ["/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1"],
    ["kajo"],
]

[input.keyboard]
xkb_layout = "us"                       # Or comma-separated "us,fi"
xkb_options = ""
numlock_on_startup = true
repeat_rate = 35
repeat_delay = 250

[input.touchpad]
tap = true
natural_scroll = true
dwt = true

[input.mouse]
accel_profile = "flat"
accel_speed = 0.0
natural_scroll = false

# ─── Host Profile A: HP OmniBook X Flip 14 OLED (matched case-insensitively by EDID Make/Model or "edp-1") ───
[[output]]
match = "samsung display corp. 0x41aa"
mode = "2880x1800@120.001"
scale = 1.75
transform = "normal"
vrr = "on"

# ─── Host Profile B: 34" Ultrawide Desktop Monitor (matched case-insensitively by connector or EDID Make/Model) ───
[[output]]
match = "dp-1"
mode = "preferred"
scale = 1.0
transform = "normal"
vrr = "on-demand"                       # Engages adaptive sync only for fullscreen windows or vrr=true rules
```

#### 2. `~/.config/kajo/binds.toml` (and distributed `[[bind]]` blocks)
```toml
# Keyboard, Mouse Wheel & Hardware Key Bindings
# Every binding is a [[bind]] block with `key = "..."` or `keys = ["...", "..."]` (100% lowercase).
# Modifiers: mod (super by default, alt with --mod alt), super, ctrl, alt, shift.
# Built-in lowercase aliases: volume-up, volume-down, volume-mute, mic-mute, brightness-up,
# brightness-down, media-play, media-next, media-prev, page-up, page-down, bracket-left,
# bracket-right, wheel-up, wheel-down, wheel-left, wheel-right.
# Actions scheduled for upcoming Phase 1/2c/3 commits are commented until wired in comp_binds.c.

# === Shell, Launcher, Lock & Session ===
[[bind]]
key = "mod+space"
spawn = ["kajo", "--toggle-launcher"]

[[bind]]
keys = ["mod+shift+s", "print"]
action = "screenshot"                                         # Phase 3 (§4.2)

[[bind]]
key = "alt+print"
action = "screenshot-window"                                  # Phase 3 (§4.2)

[[bind]]
key = "ctrl+print"
action = "screenshot-screen"                                  # Phase 3 (§4.2)

[[bind]]
key = "mod+ctrl+l"
spawn = ["kajo", "--lock"]
allow_locked = true

[[bind]]
key = "mod+ctrl+s"
spawn = ["kajo", "--screensaver"]

[[bind]]
key = "mod+t"
spawn = ["kitty"]

[[bind]]
key = "mod+shift+e"
action = "quit"

# === Audio, Brightness & Media ===
[[bind]]
key = "volume-up"
spawn = ["kajo", "--volume-up"]
allow_locked = true

[[bind]]
key = "volume-down"
spawn = ["kajo", "--volume-down"]
allow_locked = true

[[bind]]
key = "volume-mute"
spawn = ["kajo", "--volume-mute"]
allow_locked = true

[[bind]]
key = "brightness-up"
spawn = ["kajo", "--brightness-up"]
allow_locked = true

[[bind]]
key = "brightness-down"
spawn = ["kajo", "--brightness-down"]
allow_locked = true

[[bind]]
key = "media-play"
spawn = ["playerctl", "play-pause"]
allow_locked = true

[[bind]]
key = "media-next"
spawn = ["playerctl", "next"]
allow_locked = true

[[bind]]
key = "media-prev"
spawn = ["playerctl", "previous"]
allow_locked = true

# === Window Switcher ===
[[bind]]
key = "alt+tab"
action = "next-window"
scope = "output"

[[bind]]
key = "alt+shift+tab"
action = "previous-window"
scope = "output"

[[bind]]
key = "alt+grave"
action = "next-window"
filter = "app-id"

[[bind]]
key = "alt+shift+grave"
action = "previous-window"
filter = "app-id"

# === Window Lifecycle & State ===
[[bind]]
key = "mod+q"
action = "close-window"
repeat = false

[[bind]]
key = "mod+f"
action = "maximize-column"

[[bind]]
key = "mod+shift+f"
action = "fullscreen-window"

[[bind]]
key = "mod+shift+t"
action = "toggle-window-floating"

[[bind]]
key = "mod+shift+v"
action = "switch-focus-floating-tiling"

[[bind]]
key = "mod+w"
action = "toggle-column-tabbed"

[[bind]]
key = "mod+escape"
action = "break-keyboard-shortcuts-inhibit"
allow_inhibiting = false

# === Directional Focus & Movement ===
[[bind]]
keys = ["mod+left", "mod+h", "mod+wheel-left", "mod+shift+wheel-up"]
action = "focus-column-left"

[[bind]]
keys = ["mod+right", "mod+l", "mod+wheel-right", "mod+shift+wheel-down"]
action = "focus-column-right"

[[bind]]
keys = ["mod+down", "mod+j"]
action = "focus-window-down"

[[bind]]
keys = ["mod+up", "mod+k"]
action = "focus-window-up"

[[bind]]
keys = ["mod+shift+left", "mod+shift+h", "mod+ctrl+wheel-left", "mod+ctrl+shift+wheel-up"]
action = "move-column-left"

[[bind]]
keys = ["mod+shift+right", "mod+shift+l", "mod+ctrl+wheel-right", "mod+ctrl+shift+wheel-down"]
action = "move-column-right"

[[bind]]
keys = ["mod+shift+down", "mod+shift+j"]
action = "move-window-down"

[[bind]]
keys = ["mod+shift+up", "mod+shift+k"]
action = "move-window-up"

[[bind]]
key = "mod+home"
action = "focus-column-first"

[[bind]]
key = "mod+end"
action = "focus-column-last"

[[bind]]
key = "mod+ctrl+home"
action = "move-column-to-first"

[[bind]]
key = "mod+ctrl+end"
action = "move-column-to-last"

[[bind]]
keys = ["mod+bracket-left", "mod+comma"]
action = "consume-window-into-column"

[[bind]]
keys = ["mod+bracket-right", "mod+period"]
action = "expel-window-from-column"

[[bind]]
key = "mod+ctrl+left"
action = "consume-or-expel-window-left"

[[bind]]
key = "mod+ctrl+right"
action = "consume-or-expel-window-right"

# === Vertical Workspaces & Wheel Navigation ===
[[bind]]
keys = ["mod+page-down", "mod+u"]
action = "focus-workspace-down"

[[bind]]
keys = ["mod+page-up", "mod+i"]
action = "focus-workspace-up"

[[bind]]
keys = ["mod+ctrl+down", "mod+ctrl+u"]
action = "move-column-to-workspace-down"

[[bind]]
keys = ["mod+ctrl+up", "mod+ctrl+i"]
action = "move-column-to-workspace-up"

[[bind]]
keys = ["mod+shift+page-down", "mod+shift+u"]
action = "move-workspace-down"

[[bind]]
keys = ["mod+shift+page-up", "mod+shift+i"]
action = "move-workspace-up"

[[bind]]
key = "mod+wheel-down"
action = "focus-workspace-down"
cooldown_ms = 150

[[bind]]
key = "mod+wheel-up"
action = "focus-workspace-up"
cooldown_ms = 150

[[bind]]
key = "mod+ctrl+wheel-down"
action = "move-column-to-workspace-down"
cooldown_ms = 150

[[bind]]
key = "mod+ctrl+wheel-up"
action = "move-column-to-workspace-up"
cooldown_ms = 150

# === Column & Window Sizing ===
[[bind]]
key = "mod+r"
action = "switch-preset-column-width"

[[bind]]
key = "mod+shift+r"
action = "switch-preset-window-height"

[[bind]]
key = "mod+ctrl+r"
action = "reset-window-height"

[[bind]]
key = "mod+ctrl+f"
action = "expand-column-to-available-width"

[[bind]]
key = "mod+c"
action = "center-column"

[[bind]]
key = "mod+ctrl+c"
action = "center-visible-columns"

[[bind]]
key = "mod+minus"
action = "set-column-width"
delta = "-10%"

[[bind]]
key = "mod+equal"
action = "set-column-width"
delta = "+10%"

[[bind]]
key = "mod+shift+minus"
action = "set-window-height"
delta = "-10%"

[[bind]]
key = "mod+shift+equal"
action = "set-window-height"
delta = "+10%"
```

#### 3. `~/.config/kajo/rules.toml` (and distributed `[[rule]]` / `[[layer_rule]]` blocks)
```toml
# Window & Layer Rules — single quotes '...' are literal PCRE2 regular expressions
# Match fields (`app_id`, `title`, `namespace`) are always non-empty string arrays and
# match case-insensitively by default (`case_sensitive = true` opts into exact case).
# Unanchored patterns match substrings unless ^ / $ are specified; later rules override per property.

[[rule]]
app_id = ['^gnome-control-center$', '^pavucontrol$', '^nm-connection-editor$']
width = 0.5
floating = false

[[rule]]
app_id = [
    '^org\.gnome\.calculator$',
    '^gnome-calculator$',
    '^galculator$',
    '^blueman-manager$',
    '^xdg-desktop-portal',
    '^org\.freedesktop\.impl\.portal\.',
]
floating = true

[[rule]]
app_id = ['^steam$']
title = ['^notificationtoasts_\d+_desktop$']
floating = true
anchor_corner = "bottom-right"
anchor_x = 10
anchor_y = 10
focus = false

[[rule]]
app_id = ['firefox$']
title = ['^picture-in-picture$']
floating = true

[[rule]]
app_id = ['^zoom$']
floating = true

[[rule]]
title = ['brave', 'whatsapp', 'signal', 'thorium', 'chrome', 'chromium', 'code']
scroll_factor = 0.15

[[rule]]
app_id = ['^org\.kajo\.player$', '^mpv$', '^steam_app_\d+$']
vrr = true

[[layer_rule]]
namespace = ['^kajo$', '^kajo-launcher$', '^kajo-notification$', '^kajo-osk$', '^kajo-osd$']
opacity = 0.85
popup_opacity = 0.85
```

**Rule Evaluation Timing & Property Semantics (`comp_rules.h`):**
- **Matching Rules Across Files:** All `[[rule]]` and `[[layer_rule]]` blocks across `rules.toml`, `comp.toml`, included files, and `conf.d/*.toml` are evaluated in load order (`comp_rules_match_window()`, `comp_rules_match_layer()`). Inside a single rule block, all specified match fields (`app_id`, `title`) must match; each match field is a non-empty array of PCRE2 patterns (`PCRE2_CASELESS` by default unless `case_sensitive = true` is set on the rule) where any matching pattern satisfies that field.
- **Initial Map vs. Dynamic `title` / `app_id` Re-Evaluation:**
  - At initial `xdg_surface.map`, all rule properties (`floating`, `width` in `0.05..1.0`, `focus`, `anchor_corner` / `anchor_x` / `anchor_y`, `scroll_factor` in `0.01..10.0`, `vrr`) are evaluated and applied.
  - Whenever a mapped window emits `xdg_toplevel.set_title` or `set_app_id`, `comp_rules_match_window()` re-runs: `scroll_factor` and `vrr` update immediately; if the window has **not** been manually floated/tiled since map and a newly matching rule sets `floating = true` (e.g. Firefox setting `title = "Picture-in-Picture"` after map), the window is promoted to the floating layer and positioned per `anchor_corner`/`anchor_x`/`anchor_y` (if set) or centered (`L3`).
- **`anchor_corner`, `anchor_x`, `anchor_y` (`CompCorner`):** `anchor_corner` accepts `"top-left"`, `"top-right"`, `"bottom-left"`, or `"bottom-right"`. `anchor_x` and `anchor_y` (defaulting to `0` when omitted) are inward logical pixel offsets measured from the corresponding corner of the **working area** (respecting layer-shell exclusive zones), clamped by `L6`. Setting `anchor_x` or `anchor_y` without `anchor_corner` logs a diagnostic and is ignored.
- **`scroll_factor` Application:** Multiplies both continuous `wl_pointer.axis` scroll deltas and high-resolution `axis_value120` deltas forwarded to that window's surfaces by `scroll_factor`.
- **`vrr` Application:** When the active output is configured with `vrr = "on-demand"`, adaptive sync is engaged on the output whenever the focused visible window on the active workspace is fullscreen **or** has `vrr = true`.

### 5.3 IPC Wire Protocol (`$XDG_RUNTIME_DIR/kajo-comp.sock`)

`kajo-comp` listens on `$XDG_RUNTIME_DIR/kajo-comp.sock` (exported in `KAJO_COMP_SOCKET`, capped at `<= 64 KB` per incoming frame, `O_NONBLOCK`). Messages are newline-delimited UTF-8 JSON objects (`\n`).

#### Requests & Responses

| Request JSON | Response (`{"ok": ...}` or `{"error": "..."}`) | Used By |
| :--- | :--- | :--- |
| `{"cmd":"get-outputs"}` | `{"ok":{"outputs":[{"name":"eDP-1","make":"Samsung Display Corp.","model":"0x41AA","active":true,"width":2880,"height":1800,"refresh_mhz":120001,"scale":1.75,"transform":"normal","vrr":"on","modes":[...]}]}}` | `kajo-settings` (`page_displays.c`), `kajo` |
| `{"cmd":"get-workspaces"}` | `{"ok":{"workspaces":[{"idx":1,"id":1,"is_active":true,"is_urgent":false,"window_count":3}]}}` | `kajo` workspace indicator |
| `{"cmd":"get-windows"}` | `{"ok":{"windows":[{"id":4294967297,"title":"...","app_id":"org.kajo.IDE","workspace_idx":1,"is_focused":true,"is_floating":false,"is_fullscreen":false,"is_urgent":false}]}}` | `kajo` taskbar / window list |
| `{"cmd":"get-keyboard-layouts"}` | `{"ok":{"names":["English (US)","Finnish"],"current_idx":0}}` | `kajo` keyboard widget |
| `{"cmd":"action","name":"<action-name>", ...}` | `{"ok":null}` — executes any layout or compositor action (see Action Parameter Table below) | `ShellCompositor`, `posture_manager.c`, `kajo-comp msg` |
| `{"cmd":"subscribe","events":["workspaces","windows","keyboard_layouts","outputs","touch_gestures","screenshots"]}` | `{"ok":null}` followed by initial state snapshot events (`WorkspacesChanged`, `WindowsChanged`, `WindowFocusChanged`, `KeyboardLayoutsChanged`, `OutputsChanged`) and live delta events on the same socket | `ShellCompositor` (`src/compositor/kajo_comp_ipc.c`) |
| `{"cmd":"debug-mem"}` | `{"ok":{"report":"..."}}` — full `talloc` hierarchy dump | `kajo-comp msg debug-mem` |

#### `{"cmd":"action"}` Parameter Schemas

| Action `name` | Additional JSON Fields | Behavior |
| :--- | :--- | :--- |
| Any zero-arg layout/compositor action (`focus-column-left`, `move-column-right`, `toggle-window-floating`, `switch-focus-floating-tiling`, `toggle-column-tabbed`, `maximize-column`, `expand-column-to-available-width`, `switch-preset-column-width`, `switch-preset-window-height`, `reset-window-height`, `center-column`, `center-visible-columns`, `consume-or-expel-window-left/right`, `consume-window-into-column`, `expel-window-from-column`, `focus-workspace-up/down`, `move-column-to-workspace-up/down`, `move-workspace-up/down`, `fullscreen-window`, `screenshot`, `screenshot-window`, `screenshot-screen`, `reload-config`, `break-keyboard-shortcuts-inhibit`, `quit`) | *(none)* | Executes the named action on the currently focused window/column/workspace. |
| `set-column-width` / `set-window-height` | `"delta": "+10%" \| "-10%" \| "50%" \| "+20" \| "-20" \| "800"` | Resizes focused column width or window height per `W4`. |
| `switch-workspace` | `"idx": <1..K>` | Switches to the `1`-based dynamic workspace index (`WS6`). |
| `focus-window` | `"id": <uint64>` | Focuses the window with generational ID `id` (switching workspaces if needed). |
| `close-window` | `"id": <uint64>` *(optional)* | Sends `xdg_toplevel.close` to `id` (or the focused window when `id` is omitted or `0`). |
| `focus-app` | `"app_id": "<string>"` | Focuses the most-recently-focused window (`focus_timestamp`) matching `app_id`. |
| `next-window` / `previous-window` | `"scope": "output" \| "workspace" \| "all"` *(opt)*, `"filter": "app-id"` *(opt)* | Steps or activates the MRU window switcher (§4.1). |
| `switch-keyboard-layout` | `"idx": <0..N-1>` *(optional)* | Switches active XKB group to `idx` (cycles to next group when `idx` is omitted). |
| `set-output-transform` | `"output": "<name>"` *(opt)*, `"transform": "normal" \| "90" \| "180" \| "270"` | Rotates output and touch/tablet matrices atomically (`output` defaults to active output). |
| `set-output-config` | `"output": "<name>"` *(opt)*, `"mode": "<WxH@Hz\|preferred>"` *(opt)*, `"scale": <double>` *(opt)*, `"transform": "<str>"` *(opt)*, `"vrr": "off" \| "on" \| "on-demand"` *(opt)* | Tests and applies live output configuration via atomic KMS cascade (§7.3.1). |
| `spawn` | `"command": ["<prog>", "<arg1>", ...]` | Spawns detached child command (`setsid()`). |

#### Asynchronous Event Stream (after `subscribe`)

- `{"event":"WorkspacesChanged","workspaces":[...]}`
- `{"event":"WindowsChanged","windows":[...]}`
- `{"event":"WindowFocusChanged","id":4294967297}` (`0` when no window is focused)
- `{"event":"KeyboardLayoutsChanged","names":[...],"current_idx":0}`
- `{"event":"OutputsChanged","outputs":[...]}`
- `{"event":"TouchBezelSwipe","edge":"top"|"bottom","x_ratio":0.5}` (when subscribed to `"touch_gestures"`)
- `{"event":"ScreenshotSaved","path":"/home/.../Screenshot.png","copied":true}` (when subscribed to `"screenshots"`)

---

## 6. CLI, Session Orchestration (`kajo-session`), Greeter Mode & Suite Touchpoints

### 6.1 `kajo-comp` Command-Line Interface

```text
Usage: kajo-comp [OPTIONS] [-- COMMAND [ARGS...]]
       kajo-comp msg <SUBCOMMAND> [ARGS...]

Options:
  --session                 Run as the user's primary login session compositor (pushes WAYLAND_DISPLAY,
                            DISPLAY, and KAJO_COMP_SOCKET to systemd/D-Bus and sends sd_notify READY=1
                            and periodic WATCHDOG=1 heartbeats)
  --greeter-mode            Run as a locked-down pre-login greeter host (enables all outputs, no IPC socket,
                            no binds, no XWayland, exits with the child greeter's exit status)
  --config <PATH>           Override primary config file path (comp.toml or comp-greeter.toml)
  --mod <super|alt>         Override the Mod key (default: super in bare-metal/session, alt when nested
                            unless explicitly specified)
  --exit-with-command       Terminate the compositor when the startup COMMAND exits (implicit in --greeter-mode)

IPC Subcommands (`kajo-comp msg` — connects to $KAJO_COMP_SOCKET, prints JSON to stdout, exits 0 on ok / 1 on error):
  kajo-comp msg get-outputs
  kajo-comp msg get-workspaces
  kajo-comp msg get-windows
  kajo-comp msg get-keyboard-layouts
  kajo-comp msg action <ACTION_NAME> [KEY=VALUE ...]
  kajo-comp msg subscribe [EVENT ...]
  kajo-comp msg debug-mem
```

### 6.2 Installed Session & Greeter Files

```text
/usr/bin/kajo-comp                                  # Kajo Wayland compositor binary
/usr/bin/kajo-session                               # Login-shell & multi-compositor session launcher
/usr/share/wayland-sessions/kajo.desktop            # Production Kajo Desktop (kajo-comp)
/usr/share/wayland-sessions/kajo-debug.desktop      # Kajo Desktop w/ Vulkan validation + trace logs
/usr/share/wayland-sessions/kajo-niri.desktop       # Side-by-side Kajo on Niri session (fallback)
/usr/share/xdg-desktop-portal/kajo-portals.conf     # Portal routing (ScreenCast/Screenshot -> wlr, default -> gtk)
/usr/lib/systemd/user/kajo-comp.service             # Type=notify compositor unit bound to graphical-session.target
/usr/lib/systemd/user/kajo.service                  # Kajo panel/shell unit ordered After=kajo-comp.service
/usr/lib/systemd/user/kajo-shutdown.target          # Clean session teardown target
/usr/local/bin/kajo-greeter                         # Wayland login greeter
/usr/local/bin/kajo-greeter-session                 # Greeter wrapper with kajo-comp -> niri -> cage fallback chain
/etc/greetd/comp-greeter.toml                       # System-wide output/input config for kajo-comp --greeter-mode
```

### 6.3 `kajo-session` Launcher Modes

```text
Usage: kajo-session [OPTIONS]

Options:
  --compositor <kajo-comp|niri>    Select compositor backend (default: kajo-comp)
  --profile <release|debug|asan>   Enable validation layers / sanitizer options / session logs
  --config-dir <PATH>              Set KAJO_CONFIG_DIR for isolated config sandboxing
  --nested                         Launch isolated nested window inside the current live desktop
  --resolution <WxH>               Nested window geometry (default: 1920x1080)
```

1. **Standard Login Mode (`greetd` / `kajo-greeter` / TTY):**
   - Re-execs through `$SHELL -l` if not already a login shell so `~/.profile`, `PATH`, and `SSH_AUTH_SOCK` load consistently.
   - Exports `XDG_SESSION_TYPE=wayland`, `XDG_CURRENT_DESKTOP=kajo`, `XDG_SESSION_DESKTOP=kajo`, `ELECTRON_OZONE_PLATFORM_HINT=auto`, and `QT_WAYLAND_RECONNECT=1`.
   - Runs `systemctl --user reset-failed`, imports the environment, and executes `kajo-comp --session` directly (or delegates to `systemctl --user --wait start kajo-comp.service` if the user has explicitly enabled `kajo-comp.service`).
   - **Logout Environment Scrub:** On exit, starts `kajo-shutdown.target` and runs:
     ```bash
     systemctl --user unset-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP \
         XDG_SESSION_DESKTOP KAJO_COMP_SOCKET NIRI_SOCKET SWAYSOCK
     ```
2. **Debug Profile (`--profile debug`):**
   - Enables `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and `WLR_DEBUG=1`, writing rotating logs to `~/.local/state/kajo/logs/session-%Y%m%d-%H%M%S.log` (retaining the latest 10 sessions).
3. **Isolated Nested Mode (`--nested`):**
   - Launches `kajo-comp` under `WLR_BACKENDS=wayland` with private `WAYLAND_DISPLAY` and `KAJO_COMP_SOCKET` names and **skips** `systemctl --user import-environment` / `dbus-update-activation-environment` so the host session is never polluted.

### 6.4 `systemd --user` Coordination (`Type=notify` & `WatchdogSec`)

```ini
# /usr/lib/systemd/user/kajo-comp.service
[Unit]
Description=Kajo Wayland Compositor
BindsTo=graphical-session.target
Before=graphical-session.target xdg-desktop-autostart.target
Wants=graphical-session-pre.target xdg-desktop-autostart.target
After=graphical-session-pre.target

[Service]
Slice=session.slice
Type=notify
ExecStart=/usr/bin/kajo-comp --session
WatchdogSec=10
TimeoutStopSec=5
```

Once `kajo-comp --session` binds `WAYLAND_DISPLAY`, `DISPLAY` (`wlr_xwayland`), and `KAJO_COMP_SOCKET`, it updates the `systemd --user` / D-Bus activation environment and calls `sd_notify(0, "READY=1")`. While running, a timer on `wl_event_loop` sends `sd_notify(0, "WATCHDOG=1")` every 3 seconds (§7.4).

### 6.5 Greeter Host Mode (`kajo-comp --greeter-mode`)

1. **Process & Handoff Model:**
   ```text
   greetd -> kajo-greeter-session --compositor kajo-comp --cmd kajo-session
               └─ kajo-comp --greeter-mode [--config PATH] -- kajo-greeter
   ```
   - `kajo-comp` spawns `kajo-greeter` directly via `fork`/`exec` (passing `GREETD_SOCK`, `KAJO_GREETER_CMD`, `KAJO_GREETER_USER`, and `WAYLAND_DISPLAY`) and monitors the child via `SIGCHLD` on `wl_event_loop`.
   - When `kajo-greeter` completes `greetd` session start and exits, `kajo-comp` immediately stops its loop, tears down all outputs, **releases DRM master and the seat**, and exits with the child's exit code.
   - If `kajo-comp` fails before the greeter client is running (or if `XDG_RUNTIME_DIR` is missing), it exits with status **`70` (`EX_SOFTWARE`)** so `kajo-greeter-session` immediately falls back down the chain: `kajo-comp --greeter-mode` $\rightarrow$ `niri -c /etc/greetd/niri-greeter.kdl` $\rightarrow$ `cage -s -- kajo-greeter`. Every greeter invocation starts at the top of the chain without caching prior failures.
2. **Greeter Mode Security & Surface Restrictions:**
   - **Disabled:** Keybindings (`binds.toml`), `spawn` actions, `$XDG_RUNTIME_DIR/kajo-comp.sock`, `rules.toml`, `[autostart]`, `[env]`, XWayland, `systemd` `sd_notify`/watchdog, screencopy/image-capture protocols, and debug key logging. Core dumps are disabled via `setrlimit(RLIMIT_CORE, 0)` and `prctl(PR_SET_DUMPABLE, 0)`.
   - **Enabled:** `xdg_shell`, `wlr_layer_shell_v1`, `xdg_output_v1`, `wlr_fractional_scale_v1`, `wlr_viewporter`, `linux-dmabuf-v1`, `wl_seat` (keyboard, pointer, touch), cursor shape, `Ctrl+Alt+Fn` VT switching, and a built-in 5-minute idle DPMS screen blanking timer.
3. **Outputs, Scale & Renderer in Greeter Mode:**
   - **All connected outputs are enabled** at their preferred mode and arranged left-to-right in connector order so `kajo-greeter` can map one `OVERLAY` layer-shell surface per monitor.
   - **Deterministic Integer Scale Rule:** Unless overridden in `comp-greeter.toml`, `kajo-comp --greeter-mode` assigns integer scale **`2`** if the connector is an internal panel (`eDP-*`) with vertical resolution $\ge 1600$ px or physical DPI $\ge 170$ (so the `2880×1800` 14" laptop uses scale `2`), and **`1`** otherwise (so the `3440×1440` 34" ultrawide uses scale `1`).
   - **Renderer:** Uses automatic renderer selection (`wlr_renderer_autocreate()`) rather than forced Vulkan so a broken Vulkan driver never blocks login.
   - **Configuration Lookup Order:** `--config PATH`, `/etc/greetd/comp-greeter.toml`, `/usr/local/share/kajo/greetd/comp-greeter.toml`, `/usr/share/kajo/greetd/comp-greeter.toml`, `$HOME/.local/share/kajo/greetd/comp-greeter.toml`, falling back to built-in defaults. Only `[[output]]` and `[input.*]` sections are accepted; user `~/.config/kajo/` files are never read.

### 6.6 Client, Config Engine & Session Integration Touchpoints

| File / Module | Target Change |
| :--- | :--- |
| `src/config/kajo_config_schema.c`, `src/config/schemas/schema_shell.c`, `data/defaults/shell.toml` | Use `toml_seek(domain_tab, f->section)` in `kajo_cfg_domain_apply_toml_table()` to support `[section.subsection]` headers (`[animations.*]`, `[input.*]`), and replace `[layout]` inline `{ id = "...", mode = "..." }` tables in `shell.toml` with uniform `[[widget]]` blocks (`zone`, `id`, `mode`, `interval`, `exec`, `click_left`, `click_right`) so the entire suite has zero `{ ... }` curly-brace tables. |
| `src/comp/core/comp_binds.{h,c}`, `src/comp/core/comp_rules.{h,c}` | Parse uniform `[[bind]]` (`key` / `keys`, lowercase hardware/wheel aliases, multi-file merge/unbind) and `[[rule]]` / `[[layer_rule]]` (`anchor_corner`, `anchor_x`, `anchor_y`, array-only match lists, `PCRE2_CASELESS` default). |
| `src/compositor/compositor.h`, `src/compositor/niri.c`, `src/compositor/kajo_comp_ipc.c` | Provide compositor-neutral `shell_compositor_toggle_floating()`, `shell_compositor_reload_config()`, and `shell_compositor_send_action()`. Auto-detect `KAJO_COMP_SOCKET` first, falling back to `NIRI_SOCKET` when running under `kajo-niri.desktop`. |
| `src/settings/pages/page_displays.c` | Query outputs dynamically via `ShellCompositor` IPC, add VRR segmented selector (`Off | On | On-Demand`), and write `[[output]]` blocks while preserving unattached host output entries. |
| `src/posture/posture_manager.c` | Send `set-output-transform` over `ShellCompositor` IPC. |
| `src/hardware/touch_gestures.c` | Disable client-side `/dev/input/event*` reader when `KAJO_COMP_SOCKET` is active (handled natively by `kajo-comp`). |
| `src/greeter/main.c`, `src/greeter/greeter_users.c`, `data/greetd/*` | Exit normally after `start_session` so `kajo-comp --greeter-mode` reaps the child and releases the seat. Default fallback session entry becomes `kajo-session`. |

---

## 7. Hardening, Verification Gates & Implementation Plan

### 7.1 Defensive C11 Coding Invariants (Enforced by `scripts/audit.sh`)

1. **Zero `assert()` / `abort()` / `g_error()` in `src/comp/`:**
   - Runtime checks use `COMP_WARN_RETURN(cond, fallback)` which logs a rate-limited diagnostic and returns cleanly.
   - Pre-validate arguments before calling `wlroots` functions that internally `assert()` (e.g. call `wlr_scene_set_color_manager_v1()` once; clamp surface dimensions to `1..32767`; guard `width * stride` against overflow).
   - Ignore `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`) on startup.
2. **Mandatory Compiler Hardening Flags (`comp_strict_args` in `meson.build`):**
   - `-Wall -Wextra -Werror -Wshadow -Wnull-dereference -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wsign-conversion -Wvla -fno-common`
   - `-ftrivial-auto-var-init=zero -fstack-protector-strong -D_FORTIFY_SOURCE=3`
3. **Zero Blocking Calls on `wl_event_loop`:**
   - All sockets are `O_NONBLOCK`; PNG encoding, disk writes, and child processes run asynchronously off the event-loop thread.

### 7.2 Static Analysis, Sanitizers & Fuzzing

1. **Static Analysis (`scripts/audit.sh`):** `clang-tidy` (`clang-analyzer-*`, `bugprone-*`), GCC `-fanalyzer`, `cppcheck`, plus an automated check rejecting `assert(`, `abort(`, `g_error(`, `g_spawn_sync`, or blocking `waitpid(` in `src/comp/`.
2. **Sanitizer Gate (`scripts/asan-test.sh`):** All compositor unit and headless tests run under `ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1` and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` with zero suppressions in Kajo code (`scripts/lsan-comp.supp` is restricted to third-party Mesa driver allocations).
3. **`libFuzzer` Targets (`tests/fuzz/`):**
   - `fuzz_toml_config`: Fuzzes `comp.toml`, `binds.toml`, and `rules.toml` parsers.
   - `fuzz_ipc_socket`: Fuzzes `kajo-comp.sock` JSON framing and command dispatch.
   - `fuzz_layout_monkey`: Stateful property fuzzer exercising random sequences of `open`, `close`, `close_mid_anim`, `float`, `fullscreen`, `tabbed`, `move_col`, `move_window`, `consume/expel`, `switch_ws`, `output_change`, and `step_springs` under ASan/LSan.
4. **Spec-Example Parity Tests (`test_binds_spec_example`, `test_rules_spec_example`):** Unit tests in `tests/test_comp_config.c` extract and parse the exact `binds.toml` and `rules.toml` code blocks from §5.2 with zero diagnostics (`d.n == 0`); actions scheduled for upcoming phases stay commented in the `binds.toml` example block until the commit that implements them in `comp_binds.c`.

### 7.3 Hardware Fault Recovery & Session Isolation

1. **Atomic KMS Test-Before-Commit Cascade (`wlr_output_test_state`):** Every output configuration is tested via `wlr_output_test_state()` before commit; on failure, `kajo-comp` steps down (`vrr = "off"` $\rightarrow$ lower refresh $\rightarrow$ preferred mode) and posts a notification.
2. **Vulkan `VK_ERROR_DEVICE_LOST` Recovery:** Recreates `wlr_vk_renderer`, allocator, and output swapchains on GPU reset without terminating connected clients.
3. **Systemd Stall Watchdog (`WatchdogSec=10`):** In `--session` mode, `kajo-comp` emits `sd_notify(0, "WATCHDOG=1")` from `wl_event_loop`. If the main loop stalls for $>10$ seconds, `systemd` terminates the process, the kernel releases DRM master, and `seatd`/`logind` restores VT switching (`Ctrl+Alt+F2`).
4. **Detached Child Sessions:** Spawned client processes run in their own session (`setsid()`) or transient `systemd --user` scopes (`app-*.scope`) so background/CLI workloads survive compositor termination.

### 7.4 Implementation Phases & Current Status

| Phase | Scope & Gate | Status |
| :--- | :--- | :--- |
| **Phase 1 — Pure Core** | `src/comp/core/`: `comp_spring.c`, `comp_layout.c` (all §3 rules `G`, `V`, `O`, `C`, `F`, `K`, `W`, `Z`, `T`, `S`, `L`, `WS`, `OUT`), `comp_motion.c`, `comp_binds.c`, `comp_rules.c`, unit tests, `fuzz_layout_monkey`, and `comp_strict_args` hardening flags. **Gate:** 100% unit + fuzz smoke tests pass under ASan/UBSan/LSan. | **Done** |
| **Phase 2a — Server Skeleton** | `src/comp/{main,comp_server,comp_output,comp_input,comp_spawn,comp_listen}.c`: display, backend, forced Vulkan renderer, scene, seat, clipboard, VT switch, `comp_listen()`, `comp-headless` + `comp-audit` tests. | **Done** |
| **Phase 2b — Windows & Layout Wiring** | `xdg-shell`, popups, generational slot table, focus timestamps, `1px` borders, `comp_motion` driving scene nodes, server-side decorations, fractional scale, client-initiated floating move/resize (`L8`). | **Done** |
| **Phase 2c — Shell & Client Protocols** | `layer-shell-v1`, `session-lock-v1`, `idle-notify-v1`/`idle-inhibit-v1`, OSK input-method/virtual-keyboard relay, `pointer-constraints-v1`, `xdg-foreign-v2`, `tablet-v2`, `keyboard-shortcuts-inhibit-v1` (`Mod+Escape`), XWayland (`force_zero_scaling`), headless chaos test. **Gate:** `kajo`, `kajo-settings`, `kajo-files`, and `kitty` run nested cleanly. | **Done** |
| **Phase 2d — Config Hot-Reload & IPC** | `comp.toml`/`outputs.toml`/`binds.toml`/`rules.toml`/`theme.toml`/`shell.toml` + `conf.d/*.toml` / `include` multi-file merge (§5.1) + `inotify` reload on `wl_event_loop`; `$XDG_RUNTIME_DIR/kajo-comp.sock` (§5.3) + mock-compositor test; `src/compositor/kajo_comp_ipc.c`. | **Done** |
| **Phase 3 — In-Compositor UIs** | `pangocairo` text buffer helper, live window preview mirror buffers, Alt-Tab switcher (§4.1), in-compositor screenshot UI (§4.2) with off-thread PNG writer, third-party capture globals. | **Done** |
| **Phase 4 — Bare Metal (SDR)** | DRM atomic test-before-commit cascade, `vrr = "on-demand"`, hotplug (`OUT1–OUT4`), `VK_ERROR_DEVICE_LOST` recovery, `WatchdogSec=10` heartbeat, in-compositor touch gestures & posture rotation. **Gate:** verified on both Host A (14" laptop) and Host B (34" desktop). | **Done** |
| **Phase 5 — Session & Greeter** | `kajo-session`, `wayland-sessions/*.desktop`, `kajo-comp.service`, `kajo-comp --greeter-mode`, `kajo-greeter-session` fallback chain (`kajo-comp` $\rightarrow$ `niri` $\rightarrow$ `cage`). | **Done** |
| **Phase 6 — Client IPC Switch-Over** | Compositor-neutral `shell_compositor_*` helpers, `page_displays.c` on `kajo-comp` IPC, and compositor settings in `kajo-settings`. | **Done** |

---

## 8. Build & Runtime Dependencies

### 8.1 Core `kajo-comp` Dependencies

| Package | Version (Verified) | Role in `kajo-comp` |
| :--- | :--- | :--- |
| `wlroots0.20` | 0.20.2 | Core compositor library (`-DWLR_USE_UNSTABLE`). |
| `wayland` / `wayland-protocols` | 1.26.0 / 1.50 | Wayland server runtime, `wayland-scanner`, protocol XMLs. |
| `vulkan-icd-loader` / `vulkan-headers` | 1.4.363.0 | Vulkan loader and headers (`<vulkan/vulkan_core.h>`). |
| `mesa` (`vulkan-intel` / `vulkan-radeon`) | 26.2.6 | Vulkan GPU drivers. |
| `libdrm` | 2.4.134 | KMS/DRM modesetting and syncobjs. |
| `libinput` | 1.32.0 | Touchpad, touchscreen, stylus, keyboard, and mouse input. |
| `libxkbcommon` | 1.13.2 | Keymap compilation and keysym translation. |
| `pixman` | 0.46.4 | Damage region math. |
| `seatd` (`libseat.so.1`) | 0.9.3 | Rootless seat and DRM master management. |
| `libdisplay-info` | 0.3.0 | EDID parsing (linked by `wlroots`). |
| `talloc` (`libtalloc.so.2`) | 2.5.0 | Hierarchical allocator with destructors in `src/comp/`. |
| `glib2` / `json-glib` | 2.90.1 / 1.10.8 | Data structures, PCRE2 regexes, and IPC JSON serialization (no `GMainLoop` in `kajo-comp`). |
| `systemd-libs` (`libsystemd`) | 262.1 | `sd_notify` (`READY=1`, `WATCHDOG=1`). |
| `pango` (`pangocairo`) + `cairo` | 1.58.2 / 1.18.6 | Text rendering for Alt-Tab switcher and screenshot UI. |
| `libpng` | 1.6.59 | Asynchronous PNG encoding for screenshots. |
| `xorg-xwayland` + `libxcb` / `xcb-util-wm` | 24.1.14 / 1.17.0 | XWayland server and XWM helpers. |

### 8.2 Verification & Portal Packages

- **Static & Dynamic Analysis:** `clang` / `llvm` / `clang-tidy` (23.1.4, `libFuzzer`), `gcc` (16.2.1, `-fanalyzer`), `cppcheck` (2.22.0), `valgrind` (3.25.1, `--track-fds=yes`), `vulkan-validation-layers` (1.4.363.0), `vulkan-tools`.
- **Session & Portals:** `xdg-desktop-portal-wlr` (0.8.4), `xdg-desktop-portal-gtk`, `gnome-keyring`, `polkit-gnome`.
- **Drift Check:** `scripts/audit.sh deps` verifies `pkg-config --exists` for all libraries in §8.1 and checks `/usr/include/vulkan/vulkan_core.h`.

---

## 9. Deferred (Post-v1 Ledger)

This section is the single ledger of compositor features deferred past v1, recording the verified technical reason and the starting design for each item.

### 9.1 Deferred Feature Summary

| Deferred Item | Reason / Blocker in v1 | Target Approach (Post-v1) |
| :--- | :--- | :--- |
| **HDR10 Output & Mixed SDR/HDR Pipeline (v2)** | Scoped to v2 so v1 ships a rock-solid SDR compositor first. On Host A, `libdisplay-info` 0.3.0 also misses DisplayID 2.0 CTA HDR blocks (fixed in `libdisplay-info` 0.4.0). | Full design and Phase 0 hardware spike findings recorded in §9.2 below. |
| **Adjustable SDR White Level (`sdr_brightness_nits`)** | Stock `wlr_scene` fixes SDR reference white at `203 cd/m²` (ITU-R BT.2408) when compositing onto a PQ HDR output and rejects `color_transform` when an output image description is set. | Small patch to `wlr_scene`'s luminance multiplier or upstream `wlroots` proposal in v2. |
| **Compositor-Side HDR Tone Mapping (BT.2390)** | Stock `wlroots 0.20.2` has no tone-mapping curve in `render/` or `types/`; content exceeding panel peak luminance relies on display hardware tone mapping. | Custom tone-mapping shader pass in v2+. |
| **Full-Strip Vertical Workspace Scrolling** | v1 hides non-active workspace windows immediately on workspace switch and stores a single `view_x` camera per output, causing outgoing workspace windows to flick off instead of scrolling off-screen simultaneously with incoming workspace windows. | Store per-workspace `view_x` cameras, compute continuous vertical strip offsets (`ws_idx * (vh + ws_gap) - ws_y`), and keep both outgoing and incoming workspace windows enabled during vertical spring transitions (see §9.3). |
| **Backdrop Blur (Dual-Kawase Frosted Glass)** | `wlr_render_pass` exposes only `add_texture()` and `add_rect()` with no multi-pass downsample/upsample API. | Inject backdrop `wlr_scene_buffer` nodes behind blurred layer surfaces and popups, driven by a scissored 2-pass Dual-Kawase downsample/upsample pipeline (`blur = true`, `popup_blur = true`, `xray = false`; see §9.4). |
| **Window Open/Close/Resize Shaders** | Same `wlr_render_pass` limitation; closing animations also require freezing textures of unmapped surfaces, and resize crossfades require dual-texture blending. | Defer custom SPIR-V/GLSL window effects. |
| **Overview Zoom (`toggle-overview`)** | `wlr_scene` has no scene-tree scale transform; zooming requires recomputing per-buffer positions and `wlr_scene_buffer_set_dest_size()` across all surfaces, subsurfaces, and popups. | Implement manual scene-buffer scaling pass post-v1. |
| **Drop Shadows** | Not built into `wlr_scene` (rounded corners are permanently excluded by the Aero + Metro `0px` radius design system). | Optional shadow rects/nine-patch in a later release. |
| **OSK Window Push-Up Reflow** | Requires dynamic working-area reflow when `ShellOSK` maps in tablet posture. | Dynamic layer-shell exclusive zone + column viewport offset. |
| **Interactive Mouse Drag/Resize of Tiled Columns (`Mod + Drag`)** | Keyboard-first workflow + preset sizing cover v1; client-initiated drag/resize is implemented for floating windows (`L8`). | Add `Mod + Left/Right Button` interactive tiled grab in a subsequent point release. |
| **Native Aero + Metro File Chooser Portal (`org.freedesktop.impl.portal.FileChooser`)** | v1 routes `FileChooser` requests to `xdg-desktop-portal-gtk` (`default=gtk` in `kajo-portals.conf`), which renders a GTK3 Adwaita dialog rather than a native Aero + Metro surface. | Implement `org.freedesktop.impl.portal.FileChooser` directly in `kajo-files` (`org.freedesktop.impl.portal.desktop.kajo`) with `zxdg_importer_v2` + `xdg_wm_dialog_v1` parent/modal attachment (see §9.5). |
| **Additional Layout Extras** | `focus-follows-mouse`, `switch-preset-column-width-back`, `maximize-window-to-edges` keybind, named workspaces, `empty-workspace-above-first`, `default-column-display`, custom struts, and multi-monitor workspace spanning. | Evaluate individually post-v1. |

### 9.2 Deferred v2 Design Notes: HDR10 & Phase 0 Hardware Findings

The following facts were verified against `wlroots 0.20.2` (`backend/drm`, `types/scene/wlr_scene.c`, `render/color.c`, `render/vulkan`) and measured on Host A (HP OmniBook X Flip 14 OLED, `Samsung Display Corp. 0x41AA`, Intel Lunar Lake Xe2, Mesa 26.2.6) during the Phase 0 feasibility spike:

1. **DRM Capability Detection & `libdisplay-info` 0.4.0 Requirement:**
   - `wlroots`' DRM backend sets `BT2020` in `wlr_output.supported_primaries` when the connector exposes `Colorspace` and the EDID signals BT.2020, and sets `ST2084_PQ` in `supported_transfer_functions` when `HDR_OUTPUT_METADATA` and EDID PQ static metadata are present.
   - Host A's `eDP-1` OLED panel stores its HDR static metadata (`616.9 cd/m²` peak, `400 cd/m²` sustained, `0.0004 cd/m²` black) inside CTA data blocks nested within a **DisplayID 2.0 extension block**. `libdisplay-info` 0.3.0 does not scan CTA blocks inside DisplayID 2.0; `libdisplay-info >= 0.4.0` (upstream commit `73ec53d`) parses them properly.
2. **Atomic Output State Must Route Through `wlr_scene_output_build_state()`:**
   - When enabling HDR10 on a capable output (`DRM_FORMAT_XRGB2101010` + `WLR_COLOR_NAMED_PRIMARIES_BT2020` + `WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ`), the output image description **must** be committed through `wlr_scene_output_build_state()` so `wlr_scene` configures its Vulkan output color transform (encoding SDR sRGB surfaces at `203 cd/m²` PQ reference white rather than raw gamma 2.2).
   - Never pass primaries other than `NONE` or `BT2020` in an output image description (`convert_primaries_to_colorspace()` in `wlroots`' DRM backend calls `abort()` on any other value).
3. **Headless Vulkan Verification:**
   - `WLR_BACKENDS=headless WLR_RENDERER=vulkan` succeeds on Host A with `input_color_transform`, `output_color_transform`, and timeline syncobjs available, and the 6-patch SDR color accuracy self-test (`#000000`, `#111111`, `#1a1a1a`, `#0078d4`, `#808080`, `#ffffff`) reads back bit-exact 8-bit values on both headless and bare-metal `2880×1800`.

### 9.3 Deferred Design Notes: Full-Strip Vertical Workspace Scrolling

In v1 (`comp_layout.c` + `comp_motion.c` + `comp_xdg.c`), switching workspaces updates `active_ws` immediately, disables scene nodes for windows not on `active_ws`, and resets `ws_y` to `(new_ws - old_ws) * (vh + ws_gap)` to decay back to `0`. Consequently, outgoing workspace windows disappear instantly while only the incoming workspace windows slide into view.

To match Niri's continuous vertical strip transition where both outgoing and incoming workspaces remain visible and scroll together:

1. **Per-Workspace Horizontal Camera (`CompWorkspace.view_x` in `src/comp/core/comp_layout.h` & `comp_layout.c`):**
   - Store `double view_x` per workspace (`CompWorkspace`) rather than a single `view_x` per output so each workspace preserves its own horizontal scroll offset simultaneously during a vertical transition.
   - Add an explicit `double ws_gap` (default `32` logical px) between stacked workspaces on the vertical axis.
2. **All-Workspace Layout Solver (`comp_layout_solve` in `src/comp/core/comp_layout.c`):**
   - Instead of skipping non-active workspaces (`if (c->ws != out->active_ws) continue;`), solve column/window `target_x` and `target_y` across all workspaces on the output using each workspace's own `ws->view_x` and vertical strip offset:
     $$\text{ws\_offset\_y}(w) = (w - \text{active\_ws}) \cdot (\text{vh} + \text{ws\_gap})$$
3. **Continuous Vertical Camera Spring (`src/comp/core/comp_motion.c`):**
   - Replace the relative `ws_y` jump-and-decay with a continuous vertical camera spring targeting `active_ws * (vh + ws_gap)` (or offset each workspace's solved `y` by `ws_y.pos`).
   - Add `bool on_screen` to `CompSolvedWindow`, set to `true` whenever the window belongs to `active_ws` **or** the vertical `ws_y` spring is actively in flight and the window's vertical bounding box intersects `[vy, vy + vh]`.
4. **Scene Node Visibility Gate (`comp_server_sync_layout` in `src/comp/comp_xdg.c`):**
   - Replace `bool visible = (win->workspace_idx == active_ws);` with `bool visible = sw->on_screen;` so outgoing workspace windows smoothly scroll out of the viewport as incoming workspace windows scroll in, and are disabled in `wlr_scene` only after they leave `[vy, vy + vh]` and `ws_y` settles.

### 9.4 Deferred Design Notes: Dual-Kawase Backdrop Blur Pipeline

To restore frosted-glass backdrop blur behind `kajo` layer surfaces and popups (`blur = true`, `popup_blur = true`, `xray = false`) while preserving `wlr_scene_output_commit()` (DRM atomic test-before-commit cascade, damage ring tracking, direct scanout when unblurred, and screencopy):

1. **Config & Layer-Rule Schema (`src/comp/core/comp_schema_keys.h`, `comp_rules.{h,c}`, `comp_config.{h,c}`):**
   - Add a top-level `[blur]` section to `comp.toml` (`enabled = true`, `passes = 2`, `radius = 5.0`, `noise = 0.02`).
   - Move `"blur"` into `k_known_layer_keys` and add `"popup_blur"` and `"xray"` to `[[layer_rule]]` (`CompLayerRuleResult`: `has_blur`/`blur`, `has_popup_blur`/`popup_blur`, `has_xray`/`xray`).
   - Update `tests/test_comp_config.c` (`test_rules_unsupported_keys_flagged`) to test a different unimplemented key and add positive unit tests for `blur`, `popup_blur`, and `xray`.
2. **Backdrop `wlr_scene_buffer` Nodes (`src/comp/comp_layer.c`):**
   - For each `CompLayerSurface` and `CompLayerPopup`, attach a `struct wlr_scene_buffer *blur_backdrop` as the lowest-z child inside `cls->scene_layer->tree` and `popup->scene_tree`, enabled when `blur = true` / `popup_blur = true`.
   - Keep `opacity = 1.0` and `popup_opacity = 1.0` in `rules.toml` so foreground `#ffffff` text and `1px` `@metro-accent` borders stay crisp, while lowering `@metro-panel-bg` and `popover > contents` alpha in `style.css` (`0.72–0.80`) so the blurred backdrop buffer shines through.
3. **Dual-Kawase Offscreen Pipeline (`src/comp/comp_blur.{h,c}` & `src/comp/comp_server.c`):**
   - When `config->blur.enabled` is true (and `WLR_RENDERER` is not overridden), prefer `wlr_gles2_renderer_create_with_drm_fd()` in `create_server_renderer()` since `wlroots-0.20` exposes raw EGL/GL handles in `<wlr/render/gles2.h>`.
   - Maintain per-output half/quarter-resolution ping-pong FBO textures (`tex_ping`, `tex_pong`) and compile two GLSL ES fragment shaders:
     - `kawase_down.frag`: 5-tap diagonal + center downsample filter.
     - `kawase_up.frag`: 8-tap tent upsample filter with procedural dithering (`noise`) on the final pass to prevent 8-bit banding on dark `#141414` Aero glass.
   - Support both `xray = false` (samples `tree_background` + `tree_bottom` + `tree_tiled` + `tree_floating` so scrolling windows refract dynamically behind the bar and popovers) and `xray = true` (samples only `tree_background` + `tree_bottom`).
4. **Damage-Gated Per-Frame Hook (`handle_frame` in `src/comp/comp_output.c`):**
   - Before calling `wlr_scene_output_commit(o->scene_output, NULL)`, collect output-local bounding boxes of mapped layer surfaces and popups with `blur = true`.
   - **Zero-Cost Idle Fast Path:** If no scene node below `tree_top` / `tree_overlay` intersecting those bounding boxes (expanded by the blur kernel radius) was damaged this frame and no blurred popup moved/resized, skip the blur pass (`0.0 ms` GPU overhead) and reuse the existing `blur_backdrop` buffer.
   - **Dirty Blur Update Path:** When underlying damage intersects a blur rect, render the underlying scene trees clipped to the union of dirty blur rects, run 2 downsample + 2 upsample passes restricted via `glScissor`, and bind the resulting `wlr_buffer` to each `blur_backdrop` `wlr_scene_buffer` with `wlr_scene_buffer_set_source_box()` and `wlr_scene_buffer_set_dest_size()`.

### 9.5 Deferred Design Notes: Native Aero + Metro File Chooser Portal (`org.freedesktop.impl.portal.FileChooser`)

In v1, `data/session/kajo-portals.conf` routes `org.freedesktop.impl.portal.FileChooser` to `xdg-desktop-portal-gtk` (`default=gtk`). To replace the stock GTK3 dialog with a native Aero + Metro file selector across both Kajo apps (`GtkFileDialog` in `kajo-mod`, `kajo-player`, `kajo-ide`) and third-party Wayland apps (browsers, Electron, Flatpak):

1. **D-Bus Backend in `kajo-files` (`src/files/files_portal.{h,c}`):**
   - Acquire bus name `org.freedesktop.impl.portal.desktop.kajo` and export `org.freedesktop.impl.portal.FileChooser` at `/org/freedesktop/portal/desktop`:
     - `OpenFile(o handle, s app_id, s parent_window, s title, a{sv} options) -> (u response, a{sv} results)`
     - `SaveFile(o handle, s app_id, s parent_window, s title, a{sv} options) -> (u response, a{sv} results)`
     - `SaveFiles(o handle, s app_id, s parent_window, s title, a{sv} options) -> (u response, a{sv} results)`
   - Export `org.freedesktop.impl.portal.Request` (`Close()`) on each in-flight `handle` object path to honor caller-initiated cancellation.
   - Support standard `a{sv}` options: `multiple` (`b`), `directory` (`b`), `filters` (`a(sa(us))`), `current_filter` (`(sa(us))`), `current_folder` (`ay`), `current_name` (`s`), `accept_label` (`s`), `choices` (`a(ssa(ss)s)`), and `modal` (`b`).
2. **Aero + Metro Chooser Surface (`src/files/files_chooser.{h,c}`):**
   - Reuse `files_model.c` and `files_sidebar.c` inside an undecorated `880×580` dialog window (`.shell-settings-window .shell-files-chooser`, `0px` corner radius):
     - Header bar (`.settings-header-bar`) with mode badge (`OPEN FILE`, `OPEN MULTIPLE`, `SELECT FOLDER`, `SAVE FILE`), caller `title`, and `.settings-close-btn` (`✕`).
     - Places/mounts sidebar (`files_sidebar`) + breadcrumb/URI bar + search + virtualized `GtkListView` / `GtkGridView` (with `Space` Quick Look preview).
     - Bottom action bar with filename `GtkEntry` (in `SaveFile` mode), filter `GtkDropDown` (glob `u=0` and MIME `u=1` patterns), `[ CANCEL ]` (`response = 1`), and `[ ✓ OPEN ]` / `[ ✓ SAVE ]` (`response = 0`, returning `uris` and `current_filter` in `results`).
3. **Wayland Foreign Parent & Modal Integration (`zxdg_importer_v2` + `xdg_wm_dialog_v1`):**
   - Parse `parent_window` (`"wayland:<handle>"`), import the parent surface via `zxdg_importer_v2_import_toplevel(handle)`, and call `zxdg_imported_v2_set_parent_of()` on the chooser's `wl_surface` alongside `xdg_wm_dialog_v1.get_xdg_dialog()` + `xdg_dialog_v1.set_modal()` so `kajo-comp` centers and stacks the chooser over the requesting window.
   - Add `app_id = "^org\\.kajo\\.Files\\.Chooser$"` (or `floating = true` via `xdg_dialog_v1` modal / parent rules) in `data/defaults/rules.toml`.
4. **Portal Registration & Session Routing:**
   - Install `/usr/share/xdg-desktop-portal/portals/kajo.portal` (`DBusName=org.freedesktop.impl.portal.desktop.kajo`, `Interfaces=org.freedesktop.impl.portal.FileChooser;`) and D-Bus service file `org.freedesktop.impl.portal.desktop.kajo.service` (`Exec=/usr/local/bin/kajo-files --portal`).
   - Add `org.freedesktop.impl.portal.FileChooser=kajo` under `[preferred]` in `data/session/kajo-portals.conf`.
