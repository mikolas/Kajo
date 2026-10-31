# Technical Specification: Native Screen Saver, Session Lock & PAM Unlock

**Branch**: `feat/screen-lock-and-saver` (based on `master`)  
**Target Executables**: `kajo` (Wayland Shell) & `kajo-settings` (Settings Application)  
**Target Compositor**: Niri Wayland Compositor  

---

## 1. Executive Summary & Objectives

Currently, Kajo's **Lock** actions (`control-center.c`, `power.c`) only invoke `org.freedesktop.login1.Manager.LockSessions` over D-Bus without a session lock surface implementation, and the **Keep Awake / Idle Inhibitor** controls (`idle-inhibitor.c`, `control-center.c`) only toggle local UI state without inhibiting system idle timers or managing screen blanking/saving.

This specification defines a complete, zero-external-daemon implementation built directly into `kajo` and `kajo-settings`:

1. **Native Wayland Session Lock (`ext-session-lock-v1`)**: Multi-monitor lock screen managed via `GtkSessionLockInstance` (`<gtk4-session-lock.h>` from `gtk4-layer-shell-0`).
2. **Non-Blocking PAM Password Unlock (`libpam`)**: Asynchronous password authentication against PAM (`system-login` / `login`) running on a `GTask` worker thread so the GTK main loop and animations never stall.
3. **Native Idle Management & Screen Saver (`ext-idle-notify-v1`)**: Wayland seat idle detection driving a configurable **Metro Screen Saver** overlay and/or **DPMS Monitor Power-Off** (`PowerOffMonitors` / `PowerOnMonitors` via Niri IPC), plus automatic session locking after inactivity or before system sleep.
4. **Functional Idle Inhibition ("Keep Awake")**: Unified idle inhibitor state shared between the panel `idle-inhibitor` widget and the Control Center tile, holding a real `systemd-logind` inhibitor file descriptor (`Inhibit("idle", ...)`) and pausing Kajo's idle timers.
5. **Settings & IPC Integration**: CLI flags (`kajo --lock`, `kajo --screensaver`), `config.json` persistence, and user-facing controls in `kajo-settings`.

---

## 2. System Dependencies & Build Configuration (`meson.build`)

All required libraries and protocol XML definitions are already present on the target system:

| Component | Header / Protocol Source | Meson Dependency / Tool |
| :--- | :--- | :--- |
| **GTK4 Session Lock** | `<gtk4-session-lock.h>` | Provided by existing `gtk4-layer-shell-0` |
| **PAM Authentication** | `<security/pam_appl.h>` | `cc.find_library('pam', required: true)` (`-lpam`) |
| **Wayland Client** | `<wayland-client.h>`, `<gdk/wayland/gdkwayland.h>` | `dependency('wayland-client')` |
| **Wayland Idle Notify** | `/usr/share/wayland-protocols/staging/ext-idle-notify/ext-idle-notify-v1.xml` | `wayland-scanner` (`private-code` + `client-header`) |

### `meson.build` Additions
- Locate `pam_dep = cc.find_library('pam', required: true)` and `wayland_client_dep = dependency('wayland-client')`.
- Locate `wayland_protocols_dep = dependency('wayland-protocols')` and `wayland_scanner = find_program('wayland-scanner')`.
- Generate `ext-idle-notify-v1-client-protocol.h` and `ext-idle-notify-v1-protocol.c` at build time using `custom_target()`.
- Add `src/lock/session_lock.c`, `src/lock/pam_auth.c`, `src/lock/screensaver.c`, and `src/lock/idle_manager.c` to `kajo`.

---

## 3. Architecture & Module Design

```
src/lock/
├── session_lock.h / session_lock.c   # GtkSessionLockInstance lifecycle, per-monitor lock UI, logind Lock/Unlock/PrepareForSleep
├── pam_auth.h / pam_auth.c           # Asynchronous GTask PAM conversation & credential verification
├── screensaver.h / screensaver.c     # Layer-shell fullscreen screen saver overlay & Niri DPMS control
└── idle_manager.h / idle_manager.c   # ext-idle-notify-v1 seat timers, logind idle inhibitor FD, state sync
```

---

### 3.1. PAM Password Authentication (`src/lock/pam_auth.c`)

#### Requirements
- Never block the GTK main thread during `pam_authenticate()` (which intentionally delays ~2 seconds on failed attempts via `pam_unix` / `pam_faillock`).
- Never store plaintext passwords beyond the lifetime of the single `GTask` authentication call; zero memory with `explicit_bzero()` before freeing.

#### Implementation Details
1. **Service Fallback Chain**:
   - Attempt `pam_start("system-login", username, &conv, &pamh)` first; fall back to `"login"`, then `"system-auth"` if a service file is missing.
   - Resolve `username` via `getpwuid(getuid())->pw_name` (fallback `g_get_user_name()`).
2. **Conversation Callback (`struct pam_conv`)**:
   - Handle `PAM_PROMPT_ECHO_OFF` and `PAM_PROMPT_ECHO_ON` by duplicating the submitted password into `resp[i].resp`.
   - Handle `PAM_ERROR_MSG` and `PAM_TEXT_INFO` by capturing optional status messages for UI feedback.
3. **Worker Thread Execution**:
   - `shell_pam_authenticate_async(const char *password, GAsyncReadyCallback callback, gpointer user_data)` spawns a `GTask` via `g_task_run_in_thread()`.
   - Inside the worker thread:
     1. `pam_start(...)`
     2. `pam_authenticate(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK)`
     3. If `PAM_SUCCESS`, call `pam_acct_mgmt(pamh, PAM_SILENT)` (ignoring `PAM_NEW_AUTHTOK_REQD` or treating it as valid for unlock so the user is never locked out).
     4. `pam_end(pamh, retval)`
   - Deliver boolean result + descriptive error string (e.g. `"Authentication failed"`, `"Account temporarily locked (faillock)"`) back to the main thread callback.

---

### 3.2. Wayland Session Lock (`src/lock/session_lock.c`)

#### Protocol & Lifecycle
1. **Initialization (`shell_session_lock_new(ShellApp *app)`)**:
   - Verify support with `gtk_session_lock_is_supported()`.
   - Instantiate `GtkSessionLockInstance *lock_instance = gtk_session_lock_instance_new()`.
   - Connect signals:
     - `::monitor` $\rightarrow$ `on_lock_monitor(GtkSessionLockInstance *instance, GdkMonitor *monitor, gpointer user_data)`
     - `::locked` $\rightarrow$ `on_session_locked(GtkSessionLockInstance *instance, gpointer user_data)`
     - `::failed` $\rightarrow$ `on_session_lock_failed(GtkSessionLockInstance *instance, gpointer user_data)`
     - `::unlocked` $\rightarrow$ `on_session_unlocked(GtkSessionLockInstance *instance, gpointer user_data)`
2. **Lock Activation (`shell_session_lock_activate(ShellSessionLock *self)`)**:
   - If already locked, return immediately.
   - If the Screen Saver overlay is currently visible, dismiss it before acquiring the session lock.
   - Close any open panel popovers or launcher windows.
   - Call `gtk_session_lock_instance_lock(self->lock_instance)`.
   - Notify `systemd-logind` session object via `SetLockedHint(TRUE)`.
3. **Per-Monitor Lock Surface (`on_lock_monitor`)**:
   - Create a new, unrealized `GtkWindow` for the given `GdkMonitor` and bind it via `gtk_session_lock_instance_assign_window_to_monitor(instance, GTK_WINDOW(win), monitor)`.
   - Apply CSS class `.shell-lock-window` so `style.css` rules and current Metro accent variables apply.
   - **Primary / Focused Monitor Interactive Card**:
     - Centered vertical layout (`.lock-card`) containing:
       - **Live Clock & Date Header**: Large Metro typography (`HH:MM` in `64px` light weight, full date subtitle `EEEE, MMMM d`). Updated once per second via a `g_timeout_add_seconds(1, ...)` timer that is **strictly gated** to only run while `is_locked == TRUE` (Rule #10).
       - **User Identity Badge**: User icon (`tb-user-symbolic` / avatar) and `g_get_real_name()` / `g_get_user_name()`.
       - **Password Input Row**:
         - `GtkPasswordEntry` (`.lock-password-entry`) with `show-peek-icon = TRUE`, placeholder `"Enter password..."`.
         - Submit button (`→` / `Unlock`) alongside `activate` signal (`Enter` key) on the password entry.
         - `GtkEventControllerKey` handling `GDK_KEY_Escape` to clear the entry and wake/focus the input.
       - **Status / Error Label**: Shows `"Verifying..."` with spinner during PAM check, or `.lock-error-label` (`"Incorrect password — try again"`) with a subtle shake/highlight on failure.
       - **Quick Footer Actions**: `[ 🌙 Turn Off Screen ]` button (triggers Niri `PowerOffMonitors`) and `[ ⏻ Suspend ]` button (calls `logind` `Suspend`).
   - **Secondary Monitors (Multi-Head)**:
     - Receive a synchronized lock window with the dark backdrop and time/date display (or mirrored password prompt on focus) so all outputs are securely covered as required by `ext-session-lock-v1`.
4. **Unlock Flow**:
   - When `shell_pam_authenticate_finish()` returns `TRUE`:
     - Call `gtk_session_lock_instance_unlock(self->lock_instance)`.
     - In `::unlocked` callback: stop the 1 Hz lock clock timer, clear sensitive buffers, and call `SetLockedHint(FALSE)` on `logind`.
5. **`systemd-logind` Signal Integration**:
   - Subscribe to `org.freedesktop.login1.Manager` signal `PrepareForSleep(gboolean start)`:
     - When `start == TRUE` and `config->lock_on_suspend` is enabled, immediately call `shell_session_lock_activate()` so the lock surface is committed before suspend.
   - Subscribe to the current session's `org.freedesktop.login1.Session` (`GetSessionByPID(getpid())`) signals:
     - `Lock` $\rightarrow$ calls `shell_session_lock_activate(self)` (making `loginctl lock-session` and `LockSessions` D-Bus calls work seamlessly).
     - `Unlock` $\rightarrow$ calls `gtk_session_lock_instance_unlock(self->lock_instance)` (allowing administrator `loginctl unlock-session` recovery if ever needed).

---

### 3.3. Screen Saver (`src/lock/screensaver.c`)

#### Visual Modes (Configurable)
1. **`"overlay"` (Default — Metro Floating Clock & Backdrop)**:
   - Creates a `gtk4-layer-shell` fullscreen window on each `GdkMonitor` on layer `GTK_LAYER_SHELL_LAYER_OVERLAY` with keyboard interactivity `GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE`.
   - Solid pitch-black / ultra-dark glass background (`#050508`) with a minimalist Metro clock, date, and subtle accent status pill that gently shifts position every 60 seconds (OLED burn-in protection).
   - Any key press or pointer movement (`GtkEventControllerKey`, `GtkEventControllerMotion`, `GtkGestureClick`) immediately dismisses the screen saver overlay (or transitions to the Lock Screen if the lock timeout has also elapsed).
   - After an additional configurable delay (or `dpms_timeout_sec`), sends `{"Action":{"PowerOffMonitors":{}}}` to Niri to power down the display backlight completely.
2. **`"dpms"` (Direct Monitor Power-Off)**:
   - Directly invokes Niri IPC `{"Action":{"PowerOffMonitors":{}}}` when the screen saver timeout fires, and `{"Action":{"PowerOnMonitors":{}}}` when user input resumes.
3. **`"disabled"`**:
   - Disables automatic screen saver / blanking.

#### Resource Gating (Rule #10)
- The screen saver clock/drift timer (1 Hz / 60s) is **only** active while the screen saver window is mapped. When dismissed, all windows and timers are destroyed (`0.00%` idle CPU).

---

### 3.4. Idle Manager & "Keep Awake" Inhibitor (`src/lock/idle_manager.c`)

#### Wayland `ext-idle-notify-v1` Binding
1. Obtain `struct wl_display *` and `struct wl_seat *` from GDK Wayland (`gdk_wayland_display_get_wl_display()`, `gdk_wayland_seat_get_wl_seat()`).
2. Bind `ext_idle_notifier_v1` from the Wayland registry.
3. Create three `ext_idle_notification_v1` objects (when their respective timeout $> 0$ and not inhibited):
   - **Screen Saver Notification** (`screensaver_timeout_sec * 1000` ms):
     - `idled` $\rightarrow$ starts Screen Saver (`shell_screensaver_start()`).
     - `resumed` $\rightarrow$ stops Screen Saver (`shell_screensaver_stop()`) and powers on monitors.
   - **Auto-Lock Notification** (`lock_timeout_sec * 1000` ms):
     - `idled` $\rightarrow$ activates Session Lock (`shell_session_lock_activate()`).
   - **Lock-Screen DPMS Notification** (`30000` ms, active only while session is locked):
     - `idled` $\rightarrow$ powers off monitors via Niri IPC `PowerOffMonitors` if the user sits at the lock screen without typing for 30 seconds.
     - `resumed` $\rightarrow$ powers on monitors via Niri IPC `PowerOnMonitors`.

#### Real Idle Inhibition ("Keep Awake")
1. `shell_idle_manager_set_inhibited(ShellIdleManager *self, gboolean inhibited)`:
   - When `inhibited == TRUE`:
     - Calls `org.freedesktop.login1.Manager.Inhibit("idle", "kajo", "Keep Awake enabled by user", "block")` with `g_dbus_connection_call_with_unix_fd_list()` and stores the returned Unix file descriptor.
     - Destroys/pauses the active `ext_idle_notification_v1` timers so neither Screen Saver nor Auto-Lock fires.
   - When `inhibited == FALSE`:
     - Closes the stored inhibitor file descriptor (`close(self->inhibit_fd)`), releasing the `logind` lock.
     - Recreates the `ext_idle_notification_v1` timers with the configured timeouts.
   - Emits a notification callback/signal so both [src/widgets/idle-inhibitor.c](file:///home/mikolas/src/Kajo/src/widgets/idle-inhibitor.c) and [src/widgets/control-center.c](file:///home/mikolas/src/Kajo/src/widgets/control-center.c) stay 100% synchronized when toggled from either place.

---

## 4. Configuration Schema (`data/defaults/config.json` & `src/config.h`)

Extend `ShellConfig` in [src/config.h](file:///home/mikolas/src/Kajo/src/config.h) and [src/config.c](file:///home/mikolas/src/Kajo/src/config.c) with a `"security"` / `"idle"` block (backward-compatible with existing user `config.json` files):

```json
{
  "idle": {
    "screensaver_mode": "overlay",
    "screensaver_timeout_sec": 300,
    "dpms_timeout_sec": 600,
    "lock_timeout_sec": 900,
    "lock_on_suspend": true
  }
}
```

- `screensaver_mode`: `"overlay"` (Metro Clock Screen Saver), `"dpms"` (Turn Off Monitors directly), or `"disabled"`.
- `screensaver_timeout_sec`: Inactivity seconds before Screen Saver starts (`0` = disabled, default `300` = 5 min).
- `dpms_timeout_sec`: Inactivity seconds before monitors power off via DPMS (`0` = disabled, default `600` = 10 min).
- `lock_timeout_sec`: Inactivity seconds before automatic Session Lock (`0` = disabled, default `900` = 15 min).
- `lock_on_suspend`: Automatically lock session before system suspend/sleep (`true` by default).

---

## 5. CLI, IPC & Widget Wiring

1. **CLI & IPC Commands (`src/main.c`, `src/ipc/socket.c`)**:
   - `kajo --lock` $\rightarrow$ sends `"lock"` over `kajo.sock` $\rightarrow$ invokes `shell_session_lock_activate()` on the main loop.
   - `kajo --screensaver` $\rightarrow$ sends `"screensaver"` over `kajo.sock` $\rightarrow$ invokes `shell_screensaver_start()` on the main loop.
   - `kajo --reload-config` $\rightarrow$ reloads `idle` timeouts in `ShellIdleManager` live without restarting `kajo`.
2. **Widget Wiring**:
   - [src/widgets/control-center.c](file:///home/mikolas/src/Kajo/src/widgets/control-center.c):
     - `btn_lock` calls `shell_app_lock_session(app)` directly (and closes the popover).
     - `idle_btn` ("Keep Awake") reads and toggles `shell_idle_manager_get/set_inhibited()`.
   - [src/widgets/power.c](file:///home/mikolas/src/Kajo/src/widgets/power.c):
     - `Lock Screen` button calls `shell_app_lock_session(app)` directly.
   - [src/widgets/idle-inhibitor.c](file:///home/mikolas/src/Kajo/src/widgets/idle-inhibitor.c):
     - Connects switch and badge state to `ShellIdleManager` so toggling it inhibits system idle and syncs with Control Center.
3. **Settings Page (`src/settings/pages/page_displays.c` or dedicated section)**:
   - Add a **SCREEN SAVER & SESSION LOCK** card in `page_displays.c` (where `POWER SETTINGS` from the Idle Inhibitor popover already navigates):
     - **Screen Saver Mode** dropdown (`Metro Clock Overlay`, `Turn Off Monitors (DPMS)`, `Disabled`).
     - **Screen Saver Timeout** dropdown (`1 min`, `5 min`, `10 min`, `15 min`, `30 min`, `Never`).
     - **Display Power-Off (DPMS) Timeout** dropdown (`5 min`, `10 min`, `15 min`, `30 min`, `Never`).
     - **Auto-Lock Session Timeout** dropdown (`5 min`, `10 min`, `15 min`, `30 min`, `Never`).
     - **Lock Session on Suspend** toggle switch.
     - Action buttons: `[ 🖥 PREVIEW SCREEN SAVER ]` (`kajo --screensaver`) and `[ 🔒 LOCK SCREEN NOW ]` (`kajo --lock`).

---

## 6. UI Mockups & Visual Specifications (Metro Dark Design Language)

All new surfaces adhere strictly to Kajo's Metro dark glassmorphism aesthetic (`#0a0a0f` / `#12121a` surfaces, `1px solid rgba(255, 255, 255, 0.08)` borders, `@accent_color` highlights, and crisp geometric typography).

### 6.1. Session Lock Screen (`ext-session-lock-v1` — `.shell-lock-window`)

#### State A: Default / Ready for Password Input
```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│  .shell-lock-window (Fullscreen per GdkMonitor, #08080c radial gradient backdrop)        │
│                                                                                          │
│                                        13 : 05                                           │
│                                 .lock-clock-label (72px, weight 200)                     │
│                                                                                          │
│                                SATURDAY, OCTOBER 31                                      │
│                              .lock-date-label (13px, letter-spacing 3px, @accent_color)  │
│                                                                                          │
│                    ╔══════════════════════════════════════════════╗                      │
│                    ║  .lock-card (width: 380px, border-top: 3px)  ║                      │
│                    ║                                              ║                      │
│                    ║               ╭──────────────╮               ║                      │
│                    ║               │      👤      │               ║                      │
│                    ║               ╰──────────────╯               ║                      │
│                    ║                   mikolas                    ║                      │
│                    ║            [ SESSION LOCKED ]                ║                      │
│                    ║                                              ║                      │
│                    ║   ┌─────────────────────────────────┐ ┌────┐ ║                      │
│                    ║   │ ••••••••••••                 👁 │ │ →  │ ║                      │
│                    ║   └─────────────────────────────────┘ └────┘ ║                      │
│                    ║   .lock-password-entry                .lock-submit-btn              │
│                    ║                                              ║                      │
│                    ║        Press Enter to unlock • Esc to clear  ║                      │
│                    ║        .lock-status-label (muted)            ║                      │
│                    ╚══════════════════════════════════════════════╝                      │
│                                                                                          │
│                                                                                          │
│                             [ 🌙 SCREEN OFF ]   [ ⏻ SUSPEND ]                            │
│                             .lock-footer-bar (bottom-center pill buttons)                │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

#### State B: Authenticating (`GTask` PAM Worker Active) & Error State
```text
   Authenticating State (Non-blocking):              Failed Authentication State:
╔══════════════════════════════════════════════╗  ╔══════════════════════════════════════════════╗
║               ╭──────────────╮               ║  ║               ╭──────────────╮               ║
║               │      👤      │               ║  ║               │      👤      │               ║
║               ╰──────────────╯               ║  ║               ╰──────────────╯               ║
║                   mikolas                    ║  ║                   mikolas                    ║
║            [ SESSION LOCKED ]                ║  ║            [ SESSION LOCKED ]                ║
║                                              ║  ║                                              ║
║   ┌─────────────────────────────────┐ ┌────┐ ║  ║   ┌─────────────────────────────────┐ ┌────┐ ║
║   │ •••••••••••• (insensitive)      │ │ ⟳  │ ║  ║   │                              👁 │ │ →  │ ║
║   └─────────────────────────────────┘ └────┘ ║  ║   └─────────────────────────────────┘ └────┘ ║
║                                              ║  ║                                              ║
║          ⟳ Verifying credentials...          ║  ║     ⚠ Authentication failed — try again      ║
║          .lock-status-label.verifying        ║  ║     .lock-status-label.error (#ff5555)       ║
╚══════════════════════════════════════════════╝  ╚══════════════════════════════════════════════╝
```

#### State C: Secondary Monitor Surface (Multi-Monitor Setup)
```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│  .shell-lock-window (Secondary Output — Fullscreen #08080c backdrop)                     │
│                                                                                          │
│                                                                                          │
│                                        13 : 05                                           │
│                                 SATURDAY, OCTOBER 31                                     │
│                                                                                          │
│                                  🔒 SESSION LOCKED                                       │
│                          Enter password on primary display                               │
│                                                                                          │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

---

### 6.2. Metro Screen Saver Overlay (`.shell-screensaver-window`)

When `screensaver_mode` is `"overlay"`, a pitch-black (`#030305`) fullscreen layer-shell surface covers each monitor. Every 60 seconds, the floating clock tile shifts alignment (`halign`/`valign` margins within a $\pm 120\text{px}$ bounding box) to prevent OLED burn-in:

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│  .shell-screensaver-window (Fullscreen Layer-Shell OVERLAY, #030305 pitch-black)         │
│                                                                                          │
│                                                                                          │
│              ┌────────────────────────────────────────────────────────┐                  │
│              │ ┃ 13:05                                                │  ◄── Shifts      │
│              │ ┃ SATURDAY, OCTOBER 31  •  [ KAJO STANDBY ]            │      position    │
│              └────────────────────────────────────────────────────────┘      every 60s   │
│              .screensaver-floating-tile (3px left @accent_color bar)                     │
│                                                                                          │
│                                                                                          │
│                                                                                          │
│                         Move mouse or press any key to resume                            │
│                         .screensaver-hint-label (opacity: 0.35)                          │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

---

### 6.3. Synchronized Idle Inhibitor ("Keep Awake") Surfaces

Toggling **Keep Awake** in either the panel popover or the Control Center updates both widgets and holds/releases the `systemd-logind` idle inhibitor FD:

```text
  Panel Idle Inhibitor Popover (`idle-inhibitor.c`)       Control Center Tile (`control-center.c`)
┌───────────────────────────────────────────────────┐   ┌──────────────────────────────────────────┐
│ IDLE INHIBITOR                        [ AWAKE ]   │   │  ☕  Keep Awake                          │
├───────────────────────────────────────────────────┤   │      Screen Awake (Inhibiting Idle)      │
│ Prevent Screen Blank & Auto-Lock       [  ●━━ ]   │   └──────────────────────────────────────────┘
├───────────────────────────────────────────────────┤     .cc-toggle-tile.active (accent border)
│                              [ POWER SETTINGS ]   │
└───────────────────────────────────────────────────┘
```

---

### 6.4. `kajo-settings` — Screen Saver & Session Lock Card (`page_displays.c`)

Added below the Monitor Configuration cards on the **Displays & Monitors** page (which is also opened by clicking `POWER SETTINGS` in the Idle Inhibitor popover):

```text
╔══════════════════════════════════════════════════════════════════════════════════════════╗
║ ┃ SCREEN SAVER & SESSION LOCK                                                            ║
║ ┃ Configure idle screen blanking, Metro clock screen saver, and automatic session lock.  ║
║ ──────────────────────────────────────────────────────────────────────────────────────── ║
║                                                                                          ║
║  ┌────────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ 🖥  Screen Saver Mode                            [ Metro Clock Overlay        ▼ ]  │  ║
║  │     Visual behavior when the session becomes idle                                  │  ║
║  └────────────────────────────────────────────────────────────────────────────────────┘  ║
║  ┌────────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ ⏱   Screen Saver Timeout                         [ 5 Minutes                  ▼ ]  │  ║
║  │     Idle duration before starting the screen saver                                 │  ║
║  └────────────────────────────────────────────────────────────────────────────────────┘  ║
║  ┌────────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ 🌙  Monitor Power-Off (DPMS) Timeout             [ 10 Minutes                 ▼ ]  │  ║
║  │     Idle duration before powering down monitors via Niri DPMS                      │  ║
║  └────────────────────────────────────────────────────────────────────────────────────┘  ║
║  ┌────────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ 🔒  Automatic Session Lock Timeout               [ 15 Minutes                 ▼ ]  │  ║
║  │     Idle duration before locking the Wayland session with PAM password protection  │  ║
║  └────────────────────────────────────────────────────────────────────────────────────┘  ║
║                                                                                          ║
║  ┌────────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ LOCK ON SYSTEM SUSPEND                                                  [  ●━━ ]   │  ║
║  │ Automatically lock the session before entering sleep or closing the lid            │  ║
║  └────────────────────────────────────────────────────────────────────────────────────┘  ║
║                                                                                          ║
║  ┌─────────────────────────────────────────┐  ┌───────────────────────────────────────┐  ║
║  │       [ 🖥 PREVIEW SCREEN SAVER ]       │  │         [ 🔒 LOCK SCREEN NOW ]        │  ║
║  └─────────────────────────────────────────┘  └───────────────────────────────────────┘  ║
╚══════════════════════════════════════════════════════════════════════════════════════════╝
```

---

## 7. Verification Plan

1. **Build & Compiler Check**:
   - `meson compile -C builddir` $\rightarrow$ 0 errors, 0 warnings.
2. **PAM Authentication Unit & Live Test**:
   - Test wrong password $\rightarrow$ non-blocking spinner, clear error message after PAM delay, session remains locked.
   - Test empty password / `Escape` key $\rightarrow$ clears input cleanly.
   - Test valid password $\rightarrow$ immediate unlock and restoration of desktop session.
3. **Screen Saver & Idle Timeouts**:
   - Trigger `kajo --screensaver` $\rightarrow$ verify fullscreen Metro overlay appears on all monitors and dismisses immediately on mouse/key input.
   - Verify idle timeout triggers screen saver when Keep Awake is off, and does **not** trigger when Keep Awake is enabled.
4. **Control Center, Power Menu, and `loginctl`**:
   - Click `🔒 Lock` in Control Center and Power Menu $\rightarrow$ locks immediately.
   - Run `loginctl lock-session` $\rightarrow$ locks immediately via D-Bus signal handler.
