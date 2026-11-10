#ifndef KAJO_CONFIG_TYPES_H
#define KAJO_CONFIG_TYPES_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KAJO_CONFIG_DOMAIN_THEME = 1 << 0,
    KAJO_CONFIG_DOMAIN_SHELL = 1 << 1,
    KAJO_CONFIG_DOMAIN_IDE   = 1 << 2,
    KAJO_CONFIG_DOMAIN_FILES = 1 << 3,
    KAJO_CONFIG_DOMAIN_MOD   = 1 << 4,
    KAJO_CONFIG_DOMAIN_ALL   = (1 << 5) - 1,
} KajoConfigDomain;

/* ─── Shell Enums ─── */

typedef enum {
    PANEL_POSITION_TOP = 0,
    PANEL_POSITION_BOTTOM,
} ShellPanelPosition;

typedef enum {
    PANEL_LAYER_TOP = 0,
    PANEL_LAYER_OVERLAY,
    PANEL_LAYER_BOTTOM,
    PANEL_LAYER_BACKGROUND,
} ShellPanelLayer;

typedef enum {
    WIDGET_MODE_ICON = 0,
    WIDGET_MODE_LABEL,
    WIDGET_MODE_FULL,
} ShellWidgetMode;

typedef enum {
    TOAST_CLICK_FOCUS = 0,
    TOAST_CLICK_DISMISS,
} ShellToastClickAction;

typedef enum {
    SWIPE_CENTER_LAUNCHER = 0,
    SWIPE_CENTER_OVERVIEW,
    SWIPE_CENTER_OSK,
    SWIPE_CENTER_NONE,
} ShellSwipeCenterAction;

typedef enum {
    SWIPE_RIGHT_CONTROL_CENTER = 0,
    SWIPE_RIGHT_NOTIFICATIONS,
    SWIPE_RIGHT_OSK,
    SWIPE_RIGHT_NONE,
} ShellSwipeRightAction;

typedef enum {
    SWIPE_HORIZ_COLUMN = 0,
    SWIPE_HORIZ_WORKSPACE,
    SWIPE_HORIZ_NONE,
} ShellSwipeHorizAction;

typedef enum {
    OSK_AUTO_SHOW_TABLET = 0,
    OSK_AUTO_SHOW_ALWAYS,
    OSK_AUTO_SHOW_MANUAL,
} ShellOskAutoShowMode;

typedef enum {
    SCREENSAVER_MODE_OVERLAY = 0,
    SCREENSAVER_MODE_DPMS,
    SCREENSAVER_MODE_DISABLED,
} ShellScreensaverMode;

/* ─── Files Enums ─── */

typedef enum {
    KAJO_FILES_VIEW_GRID = 0,
    KAJO_FILES_VIEW_LIST,
} KajoFilesViewMode;

typedef enum {
    KAJO_FILES_SORT_NAME = 0,
    KAJO_FILES_SORT_SIZE,
    KAJO_FILES_SORT_MODIFIED,
    KAJO_FILES_SORT_TYPE,
} KajoFilesSortField;

/* ─── Mod Enums (Shared with src/mod/mod_engine.h) ─── */

#ifndef KAJO_MOD_DSP_ENUMS_DEFINED
#define KAJO_MOD_DSP_ENUMS_DEFINED

typedef enum {
    KAJO_MOD_FILTER_A500   = 0,
    KAJO_MOD_FILTER_A1200  = 1,
    KAJO_MOD_FILTER_NEAR   = 2,
    KAJO_MOD_FILTER_LINEAR = 3,
    KAJO_MOD_FILTER_SINC   = 4,
} KajoModFilterMode;

typedef enum {
    KAJO_MOD_LED_AUTO = 0,
    KAJO_MOD_LED_ON   = 1,
    KAJO_MOD_LED_OFF  = 2,
} KajoModLedMode;

typedef enum {
    KAJO_MOD_SID_AUTO = 0,
    KAJO_MOD_SID_6581 = 1,
    KAJO_MOD_SID_8580 = 2,
} KajoModSidModel;

typedef enum {
    KAJO_MOD_END_NEXT_SUB = 0,
    KAJO_MOD_END_LOOP     = 1,
    KAJO_MOD_END_NEXT_MOD = 2,
} KajoModEndAction;

#endif /* KAJO_MOD_DSP_ENUMS_DEFINED */

/* ─── Widget Item Struct ─── */

typedef struct {
    gchar          *id;
    ShellWidgetMode mode;
    gint            interval_sec;
    gchar          *exec_cmd;
    gchar          *click_left_cmd;
    gchar          *click_right_cmd;
} ShellWidgetConfig;

ShellWidgetConfig *shell_widget_config_new(const gchar *id, ShellWidgetMode mode);
ShellWidgetConfig *shell_widget_config_copy(const ShellWidgetConfig *src);
void               shell_widget_config_free(ShellWidgetConfig *wc);
gboolean           shell_widget_config_equal(const ShellWidgetConfig *a,
                                             const ShellWidgetConfig *b);

/* ─── 1. Theme & Typography Domain (theme.toml) ─── */

typedef struct {
    gchar *accent_color;          /* "#0078d4" */
    gint   ui_font_size_px;       /* [11..20], default 14 */
    gint   font_heading_size_px;  /* [10..18], default 13 */
    gint   font_subtitle_size_px; /* [10..18], default 12 */
    gint   font_badge_size_px;    /* [9..16],  default 11 */
    gchar *css_path;              /* Optional custom CSS override path or "" */
    gint   settings_width;        /* [480..3840], default 900 */
    gint   settings_height;       /* [360..2160], default 640 */
} KajoThemeCfg;

/* ─── 2. Wayland Shell Domain (shell.toml) ─── */

typedef struct {
    /* [panel] */
    ShellPanelPosition     panel_position;
    gint                   panel_height;               /* [24..96], default 39 */
    ShellPanelLayer        panel_layer;
    gint                   icon_size;                  /* [12..64], default 24 */

    /* [autohide] */
    gboolean               autohide_enabled;           /* default TRUE */
    gint                   autohide_reveal_delay;      /* [0..5000], default 200 */
    gint                   autohide_hide_delay;        /* [0..10000], default 1000 */
    gint                   autohide_reveal_duration;   /* [0..2000], default 150 */
    gint                   autohide_hide_duration;     /* [0..2000], default 200 */

    /* [notifications] */
    gint                   toast_timeout_ms;           /* [1000..30000], default 8000 */
    gint                   max_notification_history;   /* [10..200], default 50 */
    ShellToastClickAction  toast_click_action;
    gboolean               dnd_enabled;

    /* [touch] */
    gboolean               touch_gestures_enabled;
    ShellSwipeCenterAction touch_swipe_center;
    ShellSwipeRightAction  touch_swipe_right;
    ShellSwipeHorizAction  touch_swipe_horizontal;
    gint                   touch_dwell_ms;             /* [500..15000], default 4000 */

    /* [osk] */
    ShellOskAutoShowMode   osk_auto_show_mode;
    gboolean               osk_push_windows;
    gint                   osk_height_px;              /* [180..400], default 260 */

    /* [lock] */
    ShellScreensaverMode   screensaver_mode;
    gint                   screensaver_timeout_sec;    /* [0..86400], default 300 */
    gint                   dpms_timeout_sec;           /* [0..86400], default 600 */
    gint                   lock_timeout_sec;           /* [0..86400], default 900 */
    gboolean               lock_on_suspend;

    /* [hardware] */
    gint                   kbd_backlight_level;        /* [0..2], default 2 */
    gint                   kbd_backlight_timeout_sec;  /* [0..86400], default 0 */
    gboolean               posture_auto_rotate;
    gboolean               posture_disable_kbd_in_tablet;
    gboolean               posture_disable_touch_in_laptop;

    /* [layout] */
    GPtrArray             *layout_left;   /* ShellWidgetConfig* */
    GPtrArray             *layout_center; /* ShellWidgetConfig* */
    GPtrArray             *layout_right;  /* ShellWidgetConfig* */
} KajoShellCfg;

/* ─── 3. IDE Domain (ide.toml) ─── */

typedef struct {
    gchar   *color_scheme;           /* "kajo-metro-dark" */
    gchar   *font_family;            /* "Fira Code" */
    gint     font_size_px;           /* [9..28], default 13 */
    gint     tab_width;              /* {2, 4, 8}, default 4 */
    gboolean insert_spaces;
    gboolean word_wrap;
    gboolean show_margin_guide;
    gboolean show_minimap;
    gboolean show_line_numbers;
    gboolean highlight_current_line;
} KajoIdeCfg;

/* ─── 4. File Manager Domain (files.toml) ─── */

typedef struct {
    KajoFilesViewMode  view_mode;
    KajoFilesSortField sort_field;
    gboolean           sort_ascending;
    gboolean           sort_folders_first;
    gboolean           show_hidden;
    gboolean           show_inspector;
} KajoFilesCfg;

/* ─── 5. Tracker & Synth Workstation Domain (mod.toml) ─── */

typedef struct {
    KajoModFilterMode filter_mode;
    KajoModLedMode    led_mode;
    gint              stereo_sep_pct;        /* [0..100], default 70 */
    KajoModEndAction  end_action;
    KajoModSidModel   sid_model;
    gint              uade_timeout_sec;      /* 0 or [60..600], default 180 */
    gboolean          auto_extract_archives;
    gchar            *hvsc_path;             /* "" */
} KajoModCfg;

/* ─── Refcounted Snapshot Lifecycle ─── */

const KajoThemeCfg *kajo_theme_cfg_ref(const KajoThemeCfg *cfg);
void                kajo_theme_cfg_unref(const KajoThemeCfg *cfg);

const KajoShellCfg *kajo_shell_cfg_ref(const KajoShellCfg *cfg);
void                kajo_shell_cfg_unref(const KajoShellCfg *cfg);

const KajoIdeCfg   *kajo_ide_cfg_ref(const KajoIdeCfg *cfg);
void                kajo_ide_cfg_unref(const KajoIdeCfg *cfg);

const KajoFilesCfg *kajo_files_cfg_ref(const KajoFilesCfg *cfg);
void                kajo_files_cfg_unref(const KajoFilesCfg *cfg);

const KajoModCfg   *kajo_mod_cfg_ref(const KajoModCfg *cfg);
void                kajo_mod_cfg_unref(const KajoModCfg *cfg);

/* Mutable deep-copy / default / free helpers for unit tests and mutators */
KajoThemeCfg *kajo_theme_cfg_new_default(void);
KajoThemeCfg *kajo_theme_cfg_copy(const KajoThemeCfg *src);
void          kajo_theme_cfg_free(KajoThemeCfg *cfg);

KajoShellCfg *kajo_shell_cfg_new_default(void);
KajoShellCfg *kajo_shell_cfg_copy(const KajoShellCfg *src);
void          kajo_shell_cfg_free(KajoShellCfg *cfg);

KajoIdeCfg   *kajo_ide_cfg_new_default(void);
KajoIdeCfg   *kajo_ide_cfg_copy(const KajoIdeCfg *src);
void          kajo_ide_cfg_free(KajoIdeCfg *cfg);

KajoFilesCfg *kajo_files_cfg_new_default(void);
KajoFilesCfg *kajo_files_cfg_copy(const KajoFilesCfg *src);
void          kajo_files_cfg_free(KajoFilesCfg *cfg);

KajoModCfg   *kajo_mod_cfg_new_default(void);
KajoModCfg   *kajo_mod_cfg_copy(const KajoModCfg *src);
void          kajo_mod_cfg_free(KajoModCfg *cfg);

/* Enum <-> String conversion helpers */
const gchar       *shell_panel_position_to_string(ShellPanelPosition pos);
ShellPanelPosition shell_panel_position_from_string(const gchar *str);

const gchar       *shell_panel_layer_to_string(ShellPanelLayer layer);
ShellPanelLayer    shell_panel_layer_from_string(const gchar *str);

const gchar       *shell_widget_mode_to_string(ShellWidgetMode mode);
ShellWidgetMode    shell_widget_mode_from_string(const gchar *str);

const gchar          *shell_toast_click_action_to_string(ShellToastClickAction act);
ShellToastClickAction shell_toast_click_action_from_string(const gchar *str);

const gchar       *shell_screensaver_mode_to_string(ShellScreensaverMode mode);
ShellScreensaverMode shell_screensaver_mode_from_string(const gchar *str);

const gchar       *kajo_mod_filter_mode_to_string(KajoModFilterMode mode);
KajoModFilterMode  kajo_mod_filter_mode_from_string(const gchar *str);

const gchar       *kajo_mod_led_mode_to_string(KajoModLedMode mode);
KajoModLedMode     kajo_mod_led_mode_from_string(const gchar *str);

const gchar       *kajo_mod_sid_model_to_string(KajoModSidModel model);
KajoModSidModel    kajo_mod_sid_model_from_string(const gchar *str);

const gchar       *kajo_mod_end_action_to_string(KajoModEndAction act);
KajoModEndAction   kajo_mod_end_action_from_string(const gchar *str);

typedef const KajoThemeCfg KajoThemeCfgRef;
typedef const KajoShellCfg KajoShellCfgRef;
typedef const KajoIdeCfg   KajoIdeCfgRef;
typedef const KajoFilesCfg KajoFilesCfgRef;
typedef const KajoModCfg   KajoModCfgRef;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoThemeCfgRef, kajo_theme_cfg_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoShellCfgRef, kajo_shell_cfg_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoIdeCfgRef,   kajo_ide_cfg_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoFilesCfgRef, kajo_files_cfg_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoModCfgRef,   kajo_mod_cfg_unref)

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoThemeCfg, kajo_theme_cfg_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoShellCfg, kajo_shell_cfg_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoIdeCfg,   kajo_ide_cfg_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoFilesCfg, kajo_files_cfg_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoModCfg,   kajo_mod_cfg_free)

#ifdef __cplusplus
}
#endif

#endif /* KAJO_CONFIG_TYPES_H */
