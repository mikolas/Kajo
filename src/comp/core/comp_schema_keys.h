/* comp_schema_keys.h — shared TOML schema & IPC wire protocol constants for kajo-comp (pure C). */
#ifndef KAJO_COMP_SCHEMA_KEYS_H
#define KAJO_COMP_SCHEMA_KEYS_H

/* ───────────────────────── Configuration Filenames ───────────────────────── */
#define COMP_CFG_FILE_THEME               "theme.toml"
#define COMP_CFG_FILE_SHELL               "shell.toml"
#define COMP_CFG_FILE_COMP                "comp.toml"
#define COMP_CFG_FILE_OUTPUTS             "outputs.toml"
#define COMP_CFG_FILE_BINDS               "binds.toml"
#define COMP_CFG_FILE_RULES               "rules.toml"
#define COMP_CFG_DIR_CONFD                "conf.d"
#define COMP_CFG_EXT_TOML                 ".toml"

/* ───────────────────────── Top-Level TOML Sections & Keys ───────────────────────── */
#define COMP_KEY_INCLUDE                  "include"
#define COMP_KEY_ACCENT_COLOR             "accent_color"
#define COMP_KEY_RESET_RULES              "reset_rules"

#define COMP_SEC_LAYOUT                   "layout"
#define COMP_SEC_ANIMATIONS               "animations"
#define COMP_SEC_SWITCHER                 "switcher"
#define COMP_SEC_SCREENSHOTS              "screenshots"
#define COMP_SEC_XWAYLAND                 "xwayland"
#define COMP_SEC_ENV                      "env"
#define COMP_SEC_AUTOSTART                "autostart"
#define COMP_SEC_INPUT                    "input"
#define COMP_SEC_TOUCH                    "touch"
#define COMP_SEC_OUTPUT                   "output"
#define COMP_SEC_BIND                     "bind"
#define COMP_SEC_RULE                     "rule"
#define COMP_SEC_LAYER_RULE               "layer_rule"

/* ───────────────────────── [layout] Keys & Tokens ───────────────────────── */
#define COMP_KEY_LAYOUT_LEFT              "left"
#define COMP_KEY_LAYOUT_CENTER            "center"
#define COMP_KEY_LAYOUT_RIGHT             "right"
#define COMP_KEY_GAP_PX                   "gap_px"
#define COMP_KEY_BORDER_PX                "border_px"
#define COMP_KEY_ACTIVE_BORDER_COLOR      "active_border_color"
#define COMP_KEY_INACTIVE_BORDER_COLOR    "inactive_border_color"
#define COMP_KEY_URGENT_BORDER_COLOR      "urgent_border_color"
#define COMP_KEY_ALWAYS_CENTER_SINGLE     "always_center_single_column"
#define COMP_KEY_CENTER_FOCUSED_COLUMN    "center_focused_column"
#define COMP_KEY_DEFAULT_COLUMN_WIDTH     "default_column_width"
#define COMP_KEY_PRESET_WIDTHS            "preset_widths"
#define COMP_KEY_PRESET_HEIGHTS           "preset_heights"
#define COMP_VAL_ACCENT                   "accent"

/* ───────────────────────── [animations] Keys & Subtables ───────────────────────── */
#define COMP_KEY_ENABLED                  "enabled"
#define COMP_KEY_SLOWDOWN                 "slowdown"
#define COMP_SEC_ANIM_WORKSPACE_SWITCH    "workspace_switch"
#define COMP_SEC_ANIM_HORIZONTAL_SCROLL   "horizontal_scroll"
#define COMP_SEC_ANIM_WINDOW_MOVEMENT     "window_movement"
#define COMP_KEY_DAMPING_RATIO            "damping_ratio"
#define COMP_KEY_STIFFNESS                "stiffness"
#define COMP_KEY_EPSILON                  "epsilon"

/* ───────────────────────── [switcher] Keys ───────────────────────── */
#define COMP_KEY_ACTIVE_COLOR             "active_color"
#define COMP_KEY_URGENT_COLOR             "urgent_color"

/* ───────────────────────── [screenshots] Keys ───────────────────────── */
#define COMP_KEY_PATH                     "path"
#define COMP_KEY_COPY_TO_CLIPBOARD        "copy_to_clipboard"
#define COMP_KEY_SHOW_NOTIFICATION        "show_notification"
#define COMP_KEY_INCLUDE_WINDOW_BORDER    "include_window_border"

/* ───────────────────────── [xwayland] Keys ───────────────────────── */
#define COMP_KEY_FORCE_ZERO_SCALING       "force_zero_scaling"

/* ───────────────────────── [autostart] Keys ───────────────────────── */
#define COMP_KEY_RESET                    "reset"
#define COMP_KEY_COMMANDS                 "commands"

/* ───────────────────────── [input] Subtables & Keys ───────────────────────── */
#define COMP_SEC_INPUT_KEYBOARD           "keyboard"
#define COMP_SEC_INPUT_TOUCHPAD           "touchpad"
#define COMP_SEC_INPUT_MOUSE              "mouse"
#define COMP_KEY_XKB_LAYOUT               "xkb_layout"
#define COMP_KEY_XKB_OPTIONS              "xkb_options"
#define COMP_KEY_NUMLOCK_ON_STARTUP       "numlock_on_startup"
#define COMP_KEY_REPEAT_RATE              "repeat_rate"
#define COMP_KEY_REPEAT_DELAY             "repeat_delay"
#define COMP_KEY_TAP                      "tap"
#define COMP_KEY_NATURAL_SCROLL           "natural_scroll"
#define COMP_KEY_DWT                      "dwt"
#define COMP_KEY_ACCEL_PROFILE            "accel_profile"
#define COMP_KEY_ACCEL_SPEED              "accel_speed"

/* ───────────────────────── [touch] Keys ───────────────────────── */
#define COMP_KEY_SWIPE_CENTER             "swipe_center"
#define COMP_KEY_SWIPE_RIGHT              "swipe_right"
#define COMP_KEY_SWIPE_HORIZONTAL         "swipe_horizontal"
#define COMP_KEY_DWELL_MS                 "dwell_ms"

/* ───────────────────────── [[output]] Keys & Tokens ───────────────────────── */
#define COMP_KEY_MATCH                    "match"
#define COMP_KEY_MODE                     "mode"
#define COMP_KEY_SCALE                    "scale"
#define COMP_KEY_TRANSFORM                "transform"
#define COMP_KEY_VRR                      "vrr"
#define COMP_VAL_PREFERRED                "preferred"

/* ───────────────────────── [[bind]] Keys & Tokens ───────────────────────── */
#define COMP_KEY_KEY                      "key"
#define COMP_KEY_KEYS                     "keys"
#define COMP_KEY_ACTION                   "action"
#define COMP_KEY_SPAWN                    "spawn"
#define COMP_KEY_REPEAT                   "repeat"
#define COMP_KEY_COOLDOWN_MS              "cooldown_ms"
#define COMP_KEY_ALLOW_LOCKED             "allow_locked"
#define COMP_KEY_ALLOW_INHIBITING         "allow_inhibiting"
#define COMP_KEY_DELTA                    "delta"
#define COMP_KEY_SCOPE                    "scope"
#define COMP_KEY_FILTER                   "filter"
#define COMP_VAL_NONE                     "none"
#define COMP_VAL_APP_ID                   "app-id"

/* ───────────────────────── [[rule]] & [[layer_rule]] Keys ───────────────────────── */
#define COMP_KEY_APP_ID                   "app_id"
#define COMP_KEY_TITLE                    "title"
#define COMP_KEY_CASE_SENSITIVE           "case_sensitive"
#define COMP_KEY_FLOATING                 "floating"
#define COMP_KEY_WIDTH                    "width"
#define COMP_KEY_FOCUS                    "focus"
#define COMP_KEY_ANCHOR_CORNER            "anchor_corner"
#define COMP_KEY_ANCHOR_X                 "anchor_x"
#define COMP_KEY_ANCHOR_Y                 "anchor_y"
#define COMP_KEY_SCROLL_FACTOR            "scroll_factor"
#define COMP_KEY_NAMESPACE                "namespace"
#define COMP_KEY_OPACITY                  "opacity"
#define COMP_KEY_POPUP_OPACITY            "popup_opacity"

/* ───────────────────────── IPC Wire Protocol (`kajo-comp.sock`) ───────────────────────── */
#define COMP_IPC_ENV_SOCKET               "KAJO_COMP_SOCKET"
#define COMP_IPC_SOCK_NAME                "kajo-comp.sock"

#define COMP_IPC_KEY_CMD                  "cmd"
#define COMP_IPC_KEY_EVENTS               "events"
#define COMP_IPC_KEY_NAME                 "name"
#define COMP_IPC_KEY_ID                   "id"
#define COMP_IPC_KEY_IDX                  "idx"
#define COMP_IPC_KEY_APP_ID               "app_id"
#define COMP_IPC_KEY_OUTPUT               "output"
#define COMP_IPC_KEY_COMMAND              "command"
#define COMP_IPC_KEY_HOLD                 "hold"

#define COMP_IPC_CMD_GET_OUTPUTS          "get-outputs"
#define COMP_IPC_CMD_GET_WORKSPACES       "get-workspaces"
#define COMP_IPC_CMD_GET_WINDOWS          "get-windows"
#define COMP_IPC_CMD_GET_KEYBOARD_LAYOUTS "get-keyboard-layouts"
#define COMP_IPC_CMD_DEBUG_MEM            "debug-mem"
#define COMP_IPC_CMD_SUBSCRIBE            "subscribe"
#define COMP_IPC_CMD_ACTION               "action"

#define COMP_IPC_SUB_WORKSPACES           "workspaces"
#define COMP_IPC_SUB_WINDOWS              "windows"
#define COMP_IPC_SUB_KEYBOARD_LAYOUTS     "keyboard_layouts"
#define COMP_IPC_SUB_OUTPUTS              "outputs"
#define COMP_IPC_SUB_TOUCH_GESTURES       "touch_gestures"
#define COMP_IPC_SUB_SCREENSHOTS          "screenshots"

#define COMP_IPC_EVENT_WORKSPACES_CHANGED       "WorkspacesChanged"
#define COMP_IPC_EVENT_WINDOWS_CHANGED          "WindowsChanged"
#define COMP_IPC_EVENT_WINDOW_FOCUS_CHANGED     "WindowFocusChanged"
#define COMP_IPC_EVENT_KEYBOARD_LAYOUTS_CHANGED "KeyboardLayoutsChanged"
#define COMP_IPC_EVENT_OUTPUTS_CHANGED          "OutputsChanged"
#define COMP_IPC_EVENT_TOUCH_BEZEL_SWIPE        "TouchBezelSwipe"
#define COMP_IPC_EVENT_SCREENSHOT_SAVED         "ScreenshotSaved"
#define COMP_IPC_EDGE_TOP                       "top"
#define COMP_IPC_EDGE_BOTTOM                    "bottom"

#define COMP_IPC_ACT_FOCUS_WINDOW           "focus-window"
#define COMP_IPC_ACT_FOCUS_APP              "focus-app"
#define COMP_IPC_ACT_SWITCH_WORKSPACE       "switch-workspace"
#define COMP_IPC_ACT_SWITCH_WINDOW_NEXT     "switch-window-next"
#define COMP_IPC_ACT_SWITCH_WINDOW_PREV     "switch-window-prev"
#define COMP_IPC_ACT_SWITCHER_CONFIRM       "switcher-confirm"
#define COMP_IPC_ACT_SWITCHER_CANCEL        "switcher-cancel"
#define COMP_IPC_ACT_SWITCH_KEYBOARD_LAYOUT "switch-keyboard-layout"
#define COMP_IPC_ACT_SET_OUTPUT_TRANSFORM   "set-output-transform"
#define COMP_IPC_ACT_SET_OUTPUT_CONFIG      "set-output-config"
#define COMP_IPC_ACT_SCREENSHOT_CONFIRM     "screenshot-confirm"
#define COMP_IPC_ACT_SCREENSHOT_CANCEL      "screenshot-cancel"
#define COMP_IPC_ACT_TOGGLE_WINDOW_FLOATING "toggle-window-floating"
#define COMP_IPC_ACT_RELOAD_CONFIG          "reload-config"
#define COMP_IPC_ACT_FOCUS_WORKSPACE_DOWN   "focus-workspace-down"
#define COMP_IPC_ACT_FOCUS_WORKSPACE_UP     "focus-workspace-up"
#define COMP_IPC_ACT_FOCUS_COLUMN_RIGHT     "focus-column-right"
#define COMP_IPC_ACT_FOCUS_COLUMN_LEFT      "focus-column-left"
#define COMP_IPC_ACT_POWER_ON_MONITORS      "power-on-monitors"
#define COMP_IPC_ACT_POWER_OFF_MONITORS     "power-off-monitors"

#endif /* KAJO_COMP_SCHEMA_KEYS_H */
