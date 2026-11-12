#ifndef KAJO_CONFIG_KEYS_H
#define KAJO_CONFIG_KEYS_H

#include "../comp/core/comp_schema_keys.h"

/* Domain File Names */
#define KAJO_CFG_FILE_THEME               COMP_CFG_FILE_THEME
#define KAJO_CFG_FILE_SHELL               COMP_CFG_FILE_SHELL
#define KAJO_CFG_FILE_IDE                 "ide.toml"
#define KAJO_CFG_FILE_FILES               "files.toml"
#define KAJO_CFG_FILE_MOD                 "mod.toml"
#define KAJO_CFG_FILE_COMP                COMP_CFG_FILE_COMP
#define KAJO_CFG_FILE_OUTPUTS             COMP_CFG_FILE_OUTPUTS
#define KAJO_CFG_FILE_BINDS               COMP_CFG_FILE_BINDS
#define KAJO_CFG_FILE_RULES               COMP_CFG_FILE_RULES
#define KAJO_CFG_DIR_CONFD                COMP_CFG_DIR_CONFD
#define KAJO_CFG_FILE_LEGACY_JSON         "config.json"
#define KAJO_CFG_FILE_LEGACY_JSON_BAK     "config.json.bak"

/* Top-Level Domain Prefix Names (Used inside conf.d/ drop-ins) */
#define KAJO_CFG_DOMAIN_THEME             COMP_SEC_THEME
#define KAJO_CFG_DOMAIN_SHELL             COMP_SEC_SHELL
#define KAJO_CFG_DOMAIN_IDE               "ide"
#define KAJO_CFG_DOMAIN_FILES             "files"
#define KAJO_CFG_DOMAIN_MOD               "mod"
#define KAJO_CFG_DOMAIN_COMP              COMP_SEC_COMP

/* theme.toml */
#define KAJO_CFG_KEY_ACCENT_COLOR         "accent_color"
#define KAJO_CFG_KEY_UI_FONT_SIZE_PX      "ui_font_size_px"
#define KAJO_CFG_KEY_HEADING_FONT_SIZE_PX "font_heading_size_px"
#define KAJO_CFG_KEY_SUB_FONT_SIZE_PX     "font_subtitle_size_px"
#define KAJO_CFG_KEY_BADGE_FONT_SIZE_PX   "font_badge_size_px"
#define KAJO_CFG_KEY_CSS_PATH             "css_path"
#define KAJO_CFG_KEY_SETTINGS_WIDTH       "settings_width"
#define KAJO_CFG_KEY_SETTINGS_HEIGHT      "settings_height"

/* shell.toml sections */
#define KAJO_CFG_SEC_PANEL                "panel"
#define KAJO_CFG_SEC_AUTOHIDE             "autohide"
#define KAJO_CFG_SEC_NOTIFICATIONS        "notifications"
#define KAJO_CFG_SEC_TOUCH                "touch"
#define KAJO_CFG_SEC_OSK                  "osk"
#define KAJO_CFG_SEC_LOCK                 "lock"
#define KAJO_CFG_SEC_HARDWARE             "hardware"
#define KAJO_CFG_SEC_LAYOUT               "layout"

/* shell.toml [panel] */
#define KAJO_CFG_KEY_PANEL_POSITION       "position"
#define KAJO_CFG_KEY_PANEL_HEIGHT         "height"
#define KAJO_CFG_KEY_PANEL_LAYER          "layer"
#define KAJO_CFG_KEY_PANEL_ICON_SIZE      "icon_size"

/* shell.toml [autohide] */
#define KAJO_CFG_KEY_AUTOHIDE_ENABLED     "enabled"
#define KAJO_CFG_KEY_AUTOHIDE_REV_DELAY   "reveal_delay"
#define KAJO_CFG_KEY_AUTOHIDE_HIDE_DELAY  "hide_delay"
#define KAJO_CFG_KEY_AUTOHIDE_REV_DUR     "reveal_duration"
#define KAJO_CFG_KEY_AUTOHIDE_HIDE_DUR    "hide_duration"
#define KAJO_CFG_KEY_AUTOHIDE_EDGE_STRIP  "edge_strip_px"

/* shell.toml [notifications] */
#define KAJO_CFG_KEY_NOTIF_TIMEOUT_MS     "toast_timeout_ms"
#define KAJO_CFG_KEY_NOTIF_MAX_HISTORY    "max_history"
#define KAJO_CFG_KEY_NOTIF_CLICK_ACTION   "click_action"
#define KAJO_CFG_KEY_NOTIF_DND_ENABLED    "dnd_enabled"

/* shell.toml [touch] */
#define KAJO_CFG_KEY_TOUCH_ENABLED        "enabled"
#define KAJO_CFG_KEY_TOUCH_SWIPE_CENTER   "swipe_center"
#define KAJO_CFG_KEY_TOUCH_SWIPE_RIGHT    "swipe_right"
#define KAJO_CFG_KEY_TOUCH_SWIPE_HORIZ    "swipe_horizontal"
#define KAJO_CFG_KEY_TOUCH_DWELL_MS       "dwell_ms"

/* shell.toml [osk] */
#define KAJO_CFG_KEY_OSK_AUTO_SHOW        "auto_show_mode"
#define KAJO_CFG_KEY_OSK_PUSH_WINDOWS     "push_windows"
#define KAJO_CFG_KEY_OSK_HEIGHT_PX        "height_px"

/* shell.toml [lock] */
#define KAJO_CFG_KEY_LOCK_SS_MODE         "screensaver_mode"
#define KAJO_CFG_KEY_LOCK_SS_TIMEOUT      "screensaver_timeout_sec"
#define KAJO_CFG_KEY_LOCK_DPMS_TIMEOUT    "dpms_timeout_sec"
#define KAJO_CFG_KEY_LOCK_TIMEOUT         "lock_timeout_sec"
#define KAJO_CFG_KEY_LOCK_ON_SUSPEND      "lock_on_suspend"

/* shell.toml [hardware] */
#define KAJO_CFG_KEY_HW_KBD_BL_LEVEL      "kbd_backlight_level"
#define KAJO_CFG_KEY_HW_KBD_BL_TIMEOUT    "kbd_backlight_timeout_sec"
#define KAJO_CFG_KEY_HW_POSTURE_ROTATE    "posture_auto_rotate"
#define KAJO_CFG_KEY_HW_DISABLE_KBD_TAB   "posture_disable_kbd_in_tablet"
#define KAJO_CFG_KEY_HW_DISABLE_TOUCH_LAP "posture_disable_touch_in_laptop"

/* shell.toml [layout] & widget items */
#define KAJO_CFG_KEY_LAYOUT_LEFT          "left"
#define KAJO_CFG_KEY_LAYOUT_CENTER        "center"
#define KAJO_CFG_KEY_LAYOUT_RIGHT         "right"
#define KAJO_CFG_KEY_WIDGET_ID            "id"
#define KAJO_CFG_KEY_WIDGET_MODE          "mode"
#define KAJO_CFG_KEY_WIDGET_INTERVAL      "interval"
#define KAJO_CFG_KEY_WIDGET_EXEC          "exec"
#define KAJO_CFG_KEY_WIDGET_CLICK_LEFT    "click_left"
#define KAJO_CFG_KEY_WIDGET_CLICK_RIGHT   "click_right"

/* ide.toml */
#define KAJO_CFG_KEY_IDE_COLOR_SCHEME     "color_scheme"
#define KAJO_CFG_KEY_IDE_FONT_FAMILY      "font_family"
#define KAJO_CFG_KEY_IDE_FONT_SIZE_PX     "font_size_px"
#define KAJO_CFG_KEY_IDE_TAB_WIDTH        "tab_width"
#define KAJO_CFG_KEY_IDE_INSERT_SPACES    "insert_spaces"
#define KAJO_CFG_KEY_IDE_WORD_WRAP        "word_wrap"
#define KAJO_CFG_KEY_IDE_MARGIN_GUIDE     "show_margin_guide"
#define KAJO_CFG_KEY_IDE_MINIMAP          "show_minimap"
#define KAJO_CFG_KEY_IDE_LINE_NUMBERS     "show_line_numbers"
#define KAJO_CFG_KEY_IDE_CUR_LINE         "highlight_current_line"

/* files.toml */
#define KAJO_CFG_KEY_FILES_VIEW_MODE      "view_mode"
#define KAJO_CFG_KEY_FILES_SORT_FIELD     "sort_field"
#define KAJO_CFG_KEY_FILES_SORT_ASC       "sort_ascending"
#define KAJO_CFG_KEY_FILES_FOLDERS_FIRST  "sort_folders_first"
#define KAJO_CFG_KEY_FILES_SHOW_HIDDEN    "show_hidden"
#define KAJO_CFG_KEY_FILES_SHOW_INSPECTOR "show_inspector"

/* mod.toml */
#define KAJO_CFG_KEY_MOD_FILTER_MODE      "filter_mode"
#define KAJO_CFG_KEY_MOD_LED_MODE         "led_mode"
#define KAJO_CFG_KEY_MOD_STEREO_SEP_PCT   "stereo_sep_pct"
#define KAJO_CFG_KEY_MOD_END_ACTION       "end_action"
#define KAJO_CFG_KEY_MOD_SID_MODEL        "sid_model"
#define KAJO_CFG_KEY_MOD_UADE_TIMEOUT_SEC "uade_timeout_sec"
#define KAJO_CFG_KEY_MOD_AUTO_EXTRACT     "auto_extract_archives"
#define KAJO_CFG_KEY_MOD_HVSC_PATH        "hvsc_path"

#endif /* KAJO_CONFIG_KEYS_H */
