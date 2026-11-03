# Technical Specification: Touchscreen & Swipe Gestures (`feat/touch-gestures`)

**Target Branch:** `feat/touch-gestures`  
**Target Binaries:** `kajo` ([`src/panel.c`](src/panel.c), [`src/autohide.c`](src/autohide.c), [`src/config.c`](src/config.c), [`src/launcher/launcher_surface.c`](src/launcher/launcher_surface.c), [`src/notifications/daemon.c`](src/notifications/daemon.c), [`src/lock/session_lock.c`](src/lock/session_lock.c), [`src/lock/screensaver.c`](src/lock/screensaver.c)) & `kajo-settings` ([`data/ui/page_panel.blp`](data/ui/page_panel.blp), [`src/settings/pages/page_panel.c`](src/settings/pages/page_panel.c))

---

## 1. Overview & Motivation

On 2-in-1 convertible and foldable touchscreen devices (such as the HP OmniBook X Flip 14 `ELAN2514:00 04F3:4428`), standard desktop pointer interactions have three limitations:
1. **Autohide Edge Target & Hover Loss**:
   - [`src/autohide.c`](src/autohide.c) currently uses a `2px` edge strip (`autohide_edge_strip_px`) and relies on `GtkEventControllerMotion` (`pointer_inside = TRUE`) to keep the panel revealed.
   - On a touchscreen, a `2px` strip is too narrow for reliable finger swipes, and lifting a finger immediately ends hover state.
2. **Lack of Panel & Edge Swipe Gestures**:
   - Swiping inward from the panel (or hidden panel edge strip) should reveal the panel or open key shell surfaces (**App Launcher**, **Niri Overview**, **Control Center**, **Notifications**) based on which zone of the panel was swiped.
   - Swiping horizontally across the panel should switch **Niri Workspaces** (`FocusWorkspaceDown` / `FocusWorkspaceUp`) or **Window Columns** (`FocusColumnRight` / `FocusColumnLeft`).
3. **Lack of Touch Dismissal on Overlays & Toasts**:
   - In tablet mode (where the physical keyboard is inhibited and no `Escape` key is available), users need to dismiss the **App Launcher** by tapping or swiping the backdrop outside the card, and dismiss **Notification Toasts** by swiping them away.

---

## 2. Touch Gesture Map & Behavior

| Surface / Zone | Touch Gesture (`GtkGestureSwipe` / `GtkGestureClick`) | Behavior |
| :--- | :--- | :--- |
| **Hidden Autohide Panel Strip** | **Swipe Inward** (Down from top edge / Up from bottom edge) or **Touch Tap** | Calls `shell_autohide_touch_reveal(autohide, config->touch_dwell_ms)` to reveal the panel and hold it visible for `touch_dwell_ms` (default `4000 ms`) so it does not immediately re-hide when the finger lifts |
| **Visible Panel — Left / Center Zone** ($x < \frac{2}{3} W$) | **Swipe Inward** ($\|v_y\| > 1.25 \|v_x\|$, $\|v_y\| \ge 220\text{ px/s}$, downward on top panel / upward on bottom panel) | Executes `config->touch_swipe_center`:<br>• **`"launcher"`** *(default)*: Toggles Application Launcher (`shell_launcher_surface_toggle`)<br>• **`"overview"`**: Toggles Niri Overview (`{"Action":{"ToggleOverview":{}}}`)<br>• **`"none"`**: Disabled |
| **Visible Panel — Right Zone** ($x \ge \frac{2}{3} W$) | **Swipe Inward** ($\|v_y\| > 1.25 \|v_x\|$, $\|v_y\| \ge 220\text{ px/s}$, downward on top panel / upward on bottom panel) | Executes `config->touch_swipe_right`:<br>• **`"control-center"`** *(default)*: Opens the Control Center popover on the panel (falling back to Notifications popover if Control Center is not placed on the panel)<br>• **`"notifications"`**: Opens the Notifications popover on the panel<br>• **`"none"`**: Disabled |
| **Visible Panel — Any Zone** | **Swipe Outward** ($\|v_y\| > 1.25 \|v_x\|$, $\|v_y\| \ge 220\text{ px/s}$, upward on top panel / downward on bottom panel) | Closes any open panel popover and, if `autohide_enabled`, immediately hides the panel (`shell_autohide_force_hide()`) |
| **Visible Panel — Horizontal** | **Swipe Left / Right** ($\|v_x\| > 1.5 \|v_y\|$, $\|v_x\| \ge 250\text{ px/s}$) | Executes `config->touch_swipe_horizontal`:<br>• **`"workspace"`** *(default)*: Swipe Up/Left $\rightarrow$ `FocusWorkspaceDown`, Swipe Down/Right $\rightarrow$ `FocusWorkspaceUp`<br>• **`"column"`**: Swipe Left $\rightarrow$ `FocusColumnRight` (natural scroll), Swipe Right $\rightarrow$ `FocusColumnLeft`<br>• **`"none"`**: Disabled |
| **Launcher Surface ([`launcher_surface.c`](src/launcher/launcher_surface.c))** | **Tap Outside Card** (on fullscreen backdrop) or **Vertical Swipe** on backdrop | Dismisses the Launcher (`shell_launcher_surface_hide()`) |
| **Notification Toast ([`daemon.c`](src/notifications/daemon.c))** | **Swipe Left / Right / Up** on toast window | Dismisses the active notification toast immediately |
| **Screensaver & Lock Screen ([`screensaver.c`](src/lock/screensaver.c), [`session_lock.c`](src/lock/session_lock.c))** | **Swipe / Tap** on screensaver or lock surface | Dismisses screensaver / focuses password entry on lock screen |

---

## 3. Autohide Touch Adaptation ([`src/autohide.h`](src/autohide.h), [`src/autohide.c`](src/autohide.c))

1. **Touch-Friendly Edge Strip**:
   - When `config->touch_gestures_enabled` is `TRUE` (default), `autohide_get_hidden_margin()` uses `MAX(self->edge_strip_px, 6)` pixels so the edge trigger remains easy to swipe with a finger while staying visually unobtrusive.
2. **Touch Dwell API**:
   - `void shell_autohide_touch_reveal(ShellAutohide *self, gint dwell_ms);`
     - Reveals the panel if `AUTOHIDE_HIDDEN` or `AUTOHIDE_HIDING`.
     - Resets `self->hide_timeout_id` to `MAX(dwell_ms, self->hide_delay)` (default `4000 ms`) and sets `self->pointer_inside = FALSE` so the timer cleanly hides the panel once the dwell period expires (unless a popover is opened).
   - `void shell_autohide_force_hide(ShellAutohide *self);`
     - Immediately starts the hide animation when the user swipes outward on the panel.

---

## 4. Configuration Schema ([`src/config.h`](src/config.h), [`src/config.c`](src/config.c))

Inside the `"panel"` object in `~/.config/kajo/config.json`:
```json
{
  "panel": {
    "position": "top",
    "height": 32,
    "layer": "top",
    "autohide": {
      "enabled": false,
      "hide_delay": 500,
      "reveal_delay": 200,
      "reveal_duration": 150,
      "edge_strip_px": 2
    },
    "touch_gestures": {
      "enabled": true,
      "dwell_ms": 4000,
      "swipe_center": "launcher",
      "swipe_right": "control-center",
      "swipe_horizontal": "workspace"
    }
  }
}
```
New fields in `ShellConfig` ([`src/config.h`](src/config.h)):
- `gboolean touch_gestures_enabled;` (default: `TRUE`)
- `gint     touch_dwell_ms;` (default: `4000`, clamped `500..15000`)
- `gchar   *touch_swipe_center;` (default: `"launcher"`)
- `gchar   *touch_swipe_right;` (default: `"control-center"`)
- `gchar   *touch_swipe_horizontal;` (default: `"workspace"`)

---

## 5. Settings UI ([`data/ui/page_panel.blp`](data/ui/page_panel.blp), [`src/settings/pages/page_panel.c`](src/settings/pages/page_panel.c))

Add a **`TOUCHSCREEN & SWIPE GESTURES`** Metro section card to the **Panel & Autohide** settings page:
- **Touch Swipe Gestures (`combo_touch_enabled`)**: `Enabled (Default)` / `Disabled`
- **Center / Left Edge Swipe (`combo_swipe_center`)**: `Open App Launcher (Default)`, `Toggle Niri Overview`, `Disabled`
- **Right Edge Swipe (`combo_swipe_right`)**: `Open Control Center (Default)`, `Open Notifications`, `Disabled`
- **Horizontal Panel Swipe (`combo_swipe_horiz`)**: `Switch Workspace (Default)`, `Switch Window Column`, `Disabled`
- **Autohide Touch Dwell Time (`scale_touch_dwell`)**: `1000 ms` – `8000 ms` (step `500 ms`, default `4000 ms`)

Also update sidebar search keywords for `"panel"` in [`src/settings/settings_window.c`](src/settings/settings_window.c) to include `"touch touchscreen gesture gestures swipe"`.

---

## 6. Verification & Testing Plan

1. **Unit Tests ([`tests/test_config.c`](tests/test_config.c), [`tests/test_autohide.c`](tests/test_autohide.c))**:
   - Extend `test-config` to verify default values and JSON round-trip persistence of `touch_gestures_enabled`, `touch_dwell_ms`, `touch_swipe_center`, `touch_swipe_right`, and `touch_swipe_horizontal`.
   - Extend `test-autohide` to verify `shell_autohide_touch_reveal()` and `shell_autohide_force_hide()`.
2. **Build & Runtime Verification**:
   - Verify `meson compile -C builddir` (0 errors, 0 warnings) and `meson test -C builddir` (`8/8` passing).
   - Verify `kajo-settings --page panel` renders the new **Touchscreen & Swipe Gestures** card cleanly and saves/reloads `config.json`.
