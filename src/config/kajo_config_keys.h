#ifndef KAJO_CONFIG_KEYS_H
#define KAJO_CONFIG_KEYS_H

#include "../comp/core/comp_schema_keys.h"

/* Domain File Names */
#define KAJO_CFG_FILE_THEME               COMP_CFG_FILE_THEME
#define KAJO_CFG_FILE_SHELL               COMP_CFG_FILE_SHELL
#define KAJO_CFG_FILE_IDE                 "ide.toml"
#define KAJO_CFG_FILE_FILES               "files.toml"
#define KAJO_CFG_FILE_MOD                 "mod.toml"
#define KAJO_CFG_FILE_WEB                 "web.toml"
#define KAJO_CFG_FILE_AMIGA               "amiga.toml"
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
#define KAJO_CFG_DOMAIN_WEB               "web"
#define KAJO_CFG_DOMAIN_AMIGA             "amiga"
#define KAJO_CFG_DOMAIN_COMP              COMP_SEC_COMP

/* theme.toml */
#define KAJO_CFG_KEY_ACCENT_COLOR          "accent_color"
#define KAJO_CFG_KEY_UI_FONT_FAMILY        "ui_font_family"
#define KAJO_CFG_KEY_UI_FONT_SIZE_PX       "ui_font_size_px"
#define KAJO_CFG_KEY_HEADING_FONT_SIZE_PX  "font_heading_size_px"
#define KAJO_CFG_KEY_SUB_FONT_SIZE_PX      "font_subtitle_size_px"
#define KAJO_CFG_KEY_BADGE_FONT_SIZE_PX    "font_badge_size_px"
#define KAJO_CFG_KEY_GLASS_TINT            "glass_tint"
#define KAJO_CFG_KEY_PANEL_OPACITY         "panel_opacity"
#define KAJO_CFG_KEY_GLASS_OPACITY         "glass_opacity"
#define KAJO_CFG_KEY_GLASS_CTRL_CONTRAST   "glass_control_contrast"
#define KAJO_CFG_KEY_WIDGET_CHIP_OPACITY   "widget_chip_opacity"
#define KAJO_CFG_KEY_ACCENT_GLASS_TINT     "accent_glass_tint"
#define KAJO_CFG_KEY_SURFACE_BASE          "surface_base"
#define KAJO_CFG_KEY_SURFACE_CONTRAST      "surface_contrast"
#define KAJO_CFG_KEY_BORDER_OPACITY        "border_opacity"
#define KAJO_CFG_KEY_SECONDARY_FG_CONTRAST "secondary_fg_contrast"
#define KAJO_CFG_KEY_CSS_PATH              "css_path"
#define KAJO_CFG_KEY_SETTINGS_WIDTH        "settings_width"
#define KAJO_CFG_KEY_SETTINGS_HEIGHT       "settings_height"
#define KAJO_CFG_KEY_THEME_EXTERNAL_GTK    "theme_external_gtk"
#define KAJO_CFG_KEY_THEME_EXTERNAL_QT     "theme_external_qt"

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
#define KAJO_CFG_SEC_IDE_KEYS             "keys"
#define KAJO_CFG_KEY_IDE_COLOR_SCHEME     "color_scheme"
#define KAJO_CFG_KEY_IDE_FONT_FAMILY      "font_family"
#define KAJO_CFG_KEY_IDE_FONT_SIZE_PX     "font_size_px"
#define KAJO_CFG_KEY_IDE_TAB_WIDTH        "tab_width"
#define KAJO_CFG_KEY_IDE_INSERT_SPACES    "insert_spaces"
#define KAJO_CFG_KEY_IDE_WORD_WRAP        "word_wrap"
#define KAJO_CFG_KEY_IDE_MARGIN_GUIDE     "show_margin_guide"
#define KAJO_CFG_KEY_IDE_MINIMAP          "show_minimap"
#define KAJO_CFG_KEY_IDE_LINE_NUMBERS     "show_line_numbers"
#define KAJO_CFG_KEY_IDE_RELATIVE_NUMBERS "relative_line_numbers"
#define KAJO_CFG_KEY_IDE_CUR_LINE         "highlight_current_line"
#define KAJO_CFG_KEY_IDE_START_IN_STRIP   "start_in_strip_mode"
#define KAJO_CFG_KEY_IDE_STRIP_MAGNETIC   "strip_magnetic_follow"

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

/* web.toml */
#define KAJO_CFG_KEY_WEB_RAIL_MODE        "rail_mode"
#define KAJO_CFG_KEY_WEB_RAIL_SIDE        "rail_side"
#define KAJO_CFG_KEY_WEB_RAIL_WIDTH_PX    "rail_width_px"
#define KAJO_CFG_KEY_WEB_RAIL_REV_DELAY   "rail_reveal_delay_ms"
#define KAJO_CFG_KEY_WEB_RAIL_HIDE_DELAY  "rail_hide_delay_ms"
#define KAJO_CFG_KEY_WEB_TOP_BAR_MODE     "top_bar_mode"
#define KAJO_CFG_KEY_WEB_SHOW_DOMAINS     "show_tab_domains"
#define KAJO_CFG_KEY_WEB_NEW_TAB_PAGE     "new_tab_page"
#define KAJO_CFG_KEY_WEB_NEW_TAB_URL      "new_tab_custom_url"
#define KAJO_CFG_KEY_WEB_RESTORE_SESSION  "restore_session"
#define KAJO_CFG_KEY_WEB_HW_ACCEL         "hardware_acceleration"
#define KAJO_CFG_KEY_WEB_ENABLE_WEBGL     "enable_webgl"
#define KAJO_CFG_KEY_WEB_GPU_PROC_WEBGL   "gpu_process_webgl"
#define KAJO_CFG_KEY_WEB_SMOOTH_SCROLL    "smooth_scrolling"
#define KAJO_CFG_KEY_WEB_SUSPEND_IDLE_MIN "suspend_idle_min"
#define KAJO_CFG_KEY_WEB_MEMORY_LIMIT_MB  "memory_limit_mb"
#define KAJO_CFG_KEY_WEB_ADBLOCK_ENABLED  "adblock_enabled"
#define KAJO_CFG_KEY_WEB_ADBLOCK_SYNC     "adblock_auto_sync"
#define KAJO_CFG_KEY_WEB_ADBLOCK_RULES    "adblock_custom_rules_path"
#define KAJO_CFG_KEY_WEB_USER_SCRIPTS     "user_scripts_enabled"
#define KAJO_CFG_KEY_WEB_ITP_ENABLED      "itp_enabled"
#define KAJO_CFG_KEY_WEB_COOKIE_POLICY    "cookie_policy"
#define KAJO_CFG_KEY_WEB_HTTPS_ONLY       "https_only"
#define KAJO_CFG_KEY_WEB_AUTOPLAY_POLICY  "autoplay_policy"
#define KAJO_CFG_KEY_WEB_ENABLE_WEBRTC    "enable_webrtc"
#define KAJO_CFG_KEY_WEB_DNS_PREFETCH     "dns_prefetch"
#define KAJO_CFG_KEY_WEB_MEDIA_DL_FMT     "media_download_format"
#define KAJO_CFG_KEY_WEB_SEARCH_ENGINE    "search_engine"
#define KAJO_CFG_KEY_WEB_SEARCH_CUSTOM    "search_custom_url"
#define KAJO_CFG_KEY_WEB_DEFAULT_ZOOM     "default_zoom_pct"
#define KAJO_CFG_KEY_WEB_UA_PRESET        "user_agent_preset"
#define KAJO_CFG_KEY_WEB_UA_CUSTOM        "user_agent_custom"
#define KAJO_CFG_KEY_WEB_COLOR_SCHEME     "color_scheme"
#define KAJO_CFG_KEY_WEB_FONT_SIZE        "default_font_size_px"
#define KAJO_CFG_KEY_WEB_MONO_FONT_SIZE   "mono_font_size_px"
#define KAJO_CFG_KEY_WEB_MIN_FONT_SIZE    "min_font_size_px"
#define KAJO_CFG_KEY_WEB_DOWNLOAD_DIR     "download_dir"
#define KAJO_CFG_KEY_WEB_ASK_DOWNLOAD     "ask_download_location"
#define KAJO_CFG_KEY_WEB_DL_AUTO_OPEN     "downloads_auto_open"
#define KAJO_CFG_KEY_WEB_DEV_EXTRAS       "developer_extras"
#define KAJO_CFG_KEY_WEB_SHOW_PICKER      "show_picker_on_startup"
#define KAJO_CFG_KEY_WEB_PROFILE_BORDER   "profile_border_accent"
#define KAJO_CFG_KEY_WEB_SYNC_CLIENT_ID   "sync_oauth_client_id"
#define KAJO_CFG_KEY_WEB_SYNC_CLIENT_SEC  "sync_oauth_client_secret"
#define KAJO_CFG_KEY_WEB_SYNC_BOOKMARKS   "sync_bookmarks"
#define KAJO_CFG_KEY_WEB_SYNC_HISTORY     "sync_history"
#define KAJO_CFG_KEY_WEB_SYNC_TABS        "sync_tabs_and_groups"
#define KAJO_CFG_KEY_WEB_SYNC_SETTINGS    "sync_settings"
#define KAJO_CFG_KEY_WEB_SYNC_E2EE        "sync_e2ee_enabled"

/* amiga.toml */
#define KAJO_CFG_KEY_AMIGA_AMIBERRY_BIN      "amiberry_bin"
#define KAJO_CFG_KEY_AMIGA_DEFAULT_PROFILE   "default_profile"
#define KAJO_CFG_KEY_AMIGA_DEFAULT_VIDEO     "default_video"
#define KAJO_CFG_KEY_AMIGA_KICKSTART_A500    "kickstart_a500"
#define KAJO_CFG_KEY_AMIGA_KICKSTART_A1200   "kickstart_a1200"
#define KAJO_CFG_KEY_AMIGA_DEFAULT_BOOT_MODE "default_boot_mode"
#define KAJO_CFG_KEY_AMIGA_GDB_HOST          "gdb_host"
#define KAJO_CFG_KEY_AMIGA_GDB_PORT          "gdb_port"
#define KAJO_CFG_KEY_AMIGA_ADF_UAEBOARD      "adf_uaeboard_guest_output"
#define KAJO_CFG_KEY_AMIGA_DEFAULT_TOOLCHAIN "default_toolchain"
#define KAJO_CFG_KEY_AMIGA_EXTRA_BIN_PATHS   "extra_bin_paths"
#define KAJO_CFG_KEY_AMIGA_C_INCLUDE_DIRS    "c_include_dirs"
#define KAJO_CFG_KEY_AMIGA_ASM_INCLUDE_DIRS  "asm_include_dirs"
#define KAJO_CFG_KEY_AMIGA_HRM_DIR           "hrm_dir"
#define KAJO_CFG_KEY_AMIGA_INHERIT_IDE_PREFS "inherit_ide_editor_prefs"
#define KAJO_CFG_KEY_AMIGA_SHOW_CYCLE_GUTTER "show_cycle_gutter"
#define KAJO_CFG_KEY_AMIGA_SHOW_SEL_CYCLES   "show_selection_cycle_sum"
#define KAJO_CFG_KEY_AMIGA_HOVER_DECODERS    "enable_hover_decoders"
#define KAJO_CFG_KEY_AMIGA_ENABLE_M68K_LSP   "enable_m68k_lsp"
#define KAJO_CFG_KEY_AMIGA_ENABLE_CLANGD     "enable_clangd"
#define KAJO_CFG_KEY_AMIGA_ENTRY_BREAK_MODE  "entry_break_mode"
#define KAJO_CFG_KEY_AMIGA_CAPTURE_GUEST_OUT "capture_guest_output"
#define KAJO_CFG_KEY_AMIGA_TRACE_HISTORY     "enable_trace_history"
#define KAJO_CFG_KEY_AMIGA_EXC_MASK          "default_exception_mask"
#define KAJO_CFG_KEY_AMIGA_TRACE_BLITS       "trace_blitter_writes"
#define KAJO_CFG_KEY_AMIGA_HEX_GROUPING      "default_hex_grouping"
#define KAJO_CFG_KEY_AMIGA_BPL_GRID_16PX     "bitplane_grid_16px"
#define KAJO_CFG_KEY_AMIGA_INSPECTOR_VISIBLE "inspector_visible"

#endif /* KAJO_CONFIG_KEYS_H */
