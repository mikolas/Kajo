# `kajo-comp` & `kajo-session` — CLI and IPC Reference

Authoritative reference for **`kajo-comp`** startup options, **`kajo-comp msg`** IPC subcommands, actions, events, and **`kajo-session`** options.

---

## 1. Startup CLI (`kajo-comp`)

```text
kajo-comp [OPTIONS] [-- COMMAND [ARGS...]]
kajo-comp msg <SUBCOMMAND> [ARGS...]
```

| Option | Argument | Description |
| :--- | :--- | :--- |
| `--session` | — | Run as the primary login session compositor (enables session log rotation in `$XDG_STATE_HOME/kajo/kajo-comp.log`, defaults `--mod` to `super`). |
| `--greeter-mode` | — | Run as a locked-down pre-login greeter host (disables config reload, IPC socket, and `spawn`; exits when child exits; defaults child to `kajo-greeter`). |
| `-c`, `--config` | `PATH` | Override primary config file path (or config directory if `PATH` is a directory). |
| `--mod` | `alt \| super` | Override `mod` modifier in `binds.toml`. Default is `super` in session/bare-metal mode, `alt` when nested under `WAYLAND_DISPLAY` or `WLR_BACKENDS=wayland`. |
| `--exit-with-command` | — | Exit the compositor when `COMMAND` exits, returning its exit status (requires `-- COMMAND`). |
| `-v`, `--verbose` | — | Enable `WLR_DEBUG` logging. |
| `-q`, `--quiet` | — | Log errors only (`WLR_ERROR`). |
| `-h`, `--help` | — | Print CLI help and exit. |

### Environment Variables

| Variable | Purpose |
| :--- | :--- |
| `KAJO_COMP_SOCKET` | Override Unix socket path for `kajo-comp msg` and clients (default `$XDG_RUNTIME_DIR/kajo-comp.sock`, fallback `/tmp/kajo-comp-<uid>.sock`; `msg` also scans nested `$XDG_RUNTIME_DIR/kajo-comp-*.sock` when unset). |
| `KAJO_COMP_LOG_FILE` | Explicit log file path (rotates previous file to `<path>.prev`). |
| `KAJO_CONFIG_DIR` | Override config directory (default `$XDG_CONFIG_HOME/kajo` / `~/.config/kajo`). |
| `KAJO_DEFAULTS_DIR` | Override defaults directory (fallback `$XDG_DATA_HOME/kajo/defaults`, then `$XDG_DATA_DIRS/kajo/defaults`). |
| `WLR_BACKENDS` | wlroots backend selection (e.g. `headless`, `wayland`, `drm`). |
| `WLR_RENDERER` | wlroots renderer override (e.g. `vulkan`, `gles2`, `pixman`). HDR10 and custom KFX shaders require `vulkan`. |

---

## 2. IPC Client (`kajo-comp msg`)

Replies are single-line JSON (`{"ok":...}` or `{"error":"..."}`). Exit codes: `0` on `{"ok":...}`, `1` on IPC error or connection failure, `2` on invalid CLI arguments.

| Subcommand | Description |
| :--- | :--- |
| `kajo-comp msg get-outputs` | Return JSON list of outputs (name, make/model, modes, current mode, scale, transform, VRR, HDR policy/metadata). |
| `kajo-comp msg get-workspaces` | Return JSON list of workspaces across the vertical strip (`id`, `idx`, `is_active`, `active_window_id`). |
| `kajo-comp msg get-windows` | Return JSON list of mapped windows (`id`, `app_id`, `title`, `workspace_id`, `is_focused`, `is_floating`, `is_fullscreen`, `tabbed`). |
| `kajo-comp msg get-keyboard-layouts` | Return configured XKB layout names and `current_idx`. |
| `kajo-comp msg debug-mem` | Dump compositor heap/talloc and surface memory diagnostics. |
| `kajo-comp msg subscribe [TOPIC ...]` | Stream newline-delimited JSON events continuously. If no topics are given, subscribes to all topics. |
| `kajo-comp msg action <ACTION> [KEY=VALUE ...]` | Dispatch a layout, bind, or IPC-only action (optional parameters depend on the action). |

### `subscribe` Topics & Events

| Topic | Emitted Events |
| :--- | :--- |
| `workspaces` | `WorkspacesChanged` |
| `windows` | `WindowsChanged`, `WindowFocusChanged` |
| `keyboard_layouts` | `KeyboardLayoutsChanged` |
| `outputs` | `OutputsChanged` |
| `touch_gestures` | `TouchBezelSwipe` (`edge`: `"top"` \| `"bottom"`) |
| `screenshots` | `ScreenshotSaved` |

### `KEY=VALUE` Typing Rules for `kajo-comp msg action`

The CLI converts `KEY=VALUE` arguments into JSON fields automatically:
- Numeric values when key is `id`, `idx`, `scale`, or `hdr_peak_nits` (e.g. `id=42`, `scale=1.5`)
- Booleans when value is literally `true` or `false` (e.g. `hold=true`)
- JSON arrays when value begins with `[` (e.g. `command=["kitty","-1"]`)
- JSON strings for all other values (e.g. `hdr=auto`, `delta=+10%`)

---

## 3. Complete `action` Reference (59 Actions + 2 Aliases)

All layout and bind actions work in both `binds.toml` and `kajo-comp msg action <NAME>`. IPC-only actions are dispatched via `kajo-comp msg action` or `$KAJO_COMP_SOCKET`.

### A. Layout Actions (32)

| Group | Actions |
| :--- | :--- |
| **Column & Window Focus** | `focus-column-left`, `focus-column-right`, `focus-column-first`, `focus-column-last`, `focus-window-up`, `focus-window-down` |
| **Move, Consume & Expel** | `move-column-left`, `move-column-right`, `move-column-to-first`, `move-column-to-last`, `move-window-up`, `move-window-down`, `consume-window-into-column`, `expel-window-from-column`, `consume-or-expel-window-left`, `consume-or-expel-window-right` |
| **Workspaces** | `focus-workspace-up`, `focus-workspace-down`, `move-column-to-workspace-up`, `move-column-to-workspace-down`, `move-workspace-up`, `move-workspace-down` |
| **Sizing & Modes** | `switch-preset-column-width`, `switch-preset-window-height`, `reset-window-height`, `maximize-column`, `expand-column-to-available-width`, `toggle-column-tabbed`, `toggle-window-floating`, `switch-focus-floating-tiling`, `center-column`, `center-visible-columns` |

### B. Compositor Bind & IPC Actions (14 + 2 Aliases)

| Action | Parameters (`msg action KEY=VALUE`) | Description |
| :--- | :--- | :--- |
| `close-window` | `id=<UINT>` *(optional)* | Close the target window, or the currently focused window if `id` is omitted. |
| `fullscreen-window` | — | Toggle fullscreen on the focused window. |
| `next-window` *(alias: `switch-window-next`)* | `scope=all\|output\|workspace`, `filter=app-id`, `hold=true\|false` | Step forward in the MRU `Alt+Tab` switcher. |
| `previous-window` *(alias: `switch-window-prev`)* | `scope=all\|output\|workspace`, `filter=app-id`, `hold=true\|false` | Step backward in the MRU `Alt+Tab` switcher. |
| `set-column-width` | `delta=<+10%\|-10%\|50%\|+20\|800>` | Adjust or set focused column width (relative/absolute `%` or `px`). |
| `set-window-height` | `delta=<+10%\|-10%\|50%\|+20\|800>` | Adjust or set focused stacked window height. |
| `spawn` | `command=["BIN","ARG"...]` | Spawn a child command in the compositor environment (disabled in `--greeter-mode`). |
| `toggle-overview` | — | Open or cancel/close the zoomed-out workspace overview. |
| `screenshot` | — | Open the interactive region/window/screen screenshot selector. |
| `screenshot-window` | — | Capture the currently focused window immediately. |
| `screenshot-screen` | — | Capture the active output immediately. |
| `break-keyboard-shortcuts-inhibit` | — | Deactivate an active client keyboard-shortcuts inhibitor. |
| `reload-config` | — | Reload `{comp,outputs,binds,rules}.toml` and `conf.d/*.toml`. |
| `quit` | — | Gracefully terminate `kajo-comp`. |

### C. IPC-Only Actions (13)

| Action | Parameters (`msg action KEY=VALUE`) | Description |
| :--- | :--- | :--- |
| `focus-window` | `id=<UINT>` *(required)* | Focus window by numeric IPC ID. |
| `focus-app` | `app_id=<STR>` *(required)* | Focus the first window matching `app_id`. |
| `switch-workspace` | `idx=<1..N>` *(required)* | Switch to 1-based workspace index `idx`. |
| `switcher-confirm` | — | Commit the current `Alt+Tab` switcher selection. |
| `switcher-cancel` | — | Cancel the `Alt+Tab` switcher and restore previous focus. |
| `switch-keyboard-layout` | `idx=<0..N>` *(optional)* | Switch XKB layout index (`-1` or omitted cycles next). |
| `set-output-transform` | `transform=normal\|90\|180\|270`, `output=<NAME>` *(optional)* | Apply runtime output rotation (`output` omitted targets first/active output). |
| `set-output-config` | `output=<NAME>`, `mode=<WxH@Hz\|preferred>`, `scale=<FLOAT>`, `transform=normal\|90\|180\|270`, `vrr=off\|on\|on-demand`, `hdr=off\|on\|auto`, `hdr_peak_nits=<0\|100..10000>` | Configure any subset of output properties at runtime. |
| `set-output-hdr` | `hdr=off\|on\|auto` and/or `hdr_peak_nits=<0\|100..10000>`, `output=<NAME>` *(optional)* | Change HDR10 policy and/or peak luminance override (`0` = EDID/CTA). |
| `screenshot-confirm` | — | Confirm pending interactive screenshot selection. |
| `screenshot-cancel` | — | Cancel pending interactive screenshot UI. |
| `power-on-monitors` | — | Re-apply output configuration and wake displays. |
| `power-off-monitors` | — | Disable active outputs via atomic output state commit. |

### Examples

```bash
kajo-comp msg get-outputs
kajo-comp msg subscribe workspaces windows
kajo-comp msg action toggle-overview
kajo-comp msg action set-column-width delta=+10%
kajo-comp msg action switch-workspace idx=2
kajo-comp msg action set-output-hdr output=eDP-1 hdr=auto hdr_peak_nits=0
kajo-comp msg action set-output-config output=eDP-1 scale=1.5 vrr=on-demand
kajo-comp msg action spawn 'command=["kajo-files","/tmp"]'
```

---

## 4. Session Wrapper CLI (`kajo-session`)

`kajo-session` is installed alongside `kajo-comp` and invoked by the display manager `.desktop` entries:

```text
kajo-session [OPTIONS] [-- COMMAND [ARGS...]]
```

| Option | Argument | Description |
| :--- | :--- | :--- |
| `--compositor` | `kajo-comp \| niri` | Select compositor backend (default: `kajo-comp`). |
| `--profile` | `release \| debug \| asan` | Enable verbose compositor logging or ASan/UBSan + rotating `session-*.log` in `$XDG_STATE_HOME/kajo/logs/`. |
| `--config-dir` | `PATH` | Set `KAJO_CONFIG_DIR` and `XDG_CONFIG_HOME` for sandboxed config testing. |
| `--nested` | — | Launch an isolated nested compositor window inside a running desktop session. |
| `--resolution` | `WxH` | Nested window geometry (default: `1920x1080`). |
| `-h`, `--help` | — | Print `kajo-session` help and exit. |
