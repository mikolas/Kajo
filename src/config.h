#ifndef SHELL_CONFIG_H
#define SHELL_CONFIG_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_POSITION_TOP = 0,
    PANEL_POSITION_BOTTOM
} PanelPosition;

typedef enum {
    WIDGET_MODE_ICON = 0,
    WIDGET_MODE_LABEL,
    WIDGET_MODE_FULL
} ShellWidgetMode;

typedef struct {
    gchar          *id;
    ShellWidgetMode mode;
    gint            interval_sec;
    gchar          *exec_cmd;
    gchar          *click_left_cmd;
    gchar          *click_right_cmd;
} ShellWidgetConfig;

typedef struct _ShellConfig ShellConfig;

struct _ShellConfig {
    PanelPosition   panel_position;
    gint            panel_height;
    gchar          *panel_layer;
    ShellWidgetMode default_widget_mode;
    gint            icon_size;

    gboolean        autohide_enabled;
    gint            autohide_hide_delay;
    gint            autohide_reveal_delay;
    gint            autohide_reveal_duration;
    gint            autohide_edge_strip_px;

    /* Touchscreen & Swipe Gestures */
    gboolean        touch_gestures_enabled;
    gint            touch_dwell_ms;
    gchar          *touch_swipe_center;     /* "launcher", "overview", "none" */
    gchar          *touch_swipe_right;      /* "control-center", "notifications", "none" */
    gchar          *touch_swipe_horizontal; /* "workspace", "column", "none" */

    /* Virtual Keyboard (On-Screen Keyboard) */
    gchar          *osk_auto_show_mode;     /* "tablet", "always", "manual" */
    gboolean        osk_push_windows;
    gint            osk_height_px;

    gint            osd_timeout_ms;
    gint            osd_width;
    gint            osd_height;

    gint            toast_timeout_ms;
    gint            max_notification_history;
    gchar          *toast_click_action;        /* "focus" or "dismiss", default "focus" */
    gboolean        dnd_enabled;               /* suppress toast popups, default FALSE */

    gint            launcher_width;
    gint            launcher_max_height;

    gint            settings_width;
    gint            settings_height;
    gint            settings_transition_ms;

    gchar          *accent_color;
    gint            ui_font_size_px;
    gint            font_heading_size_px;
    gint            font_subtitle_size_px;
    gint            font_badge_size_px;

    gchar          *screensaver_mode;         /* "overlay", "dpms", or "disabled" */
    gint            screensaver_timeout_sec;
    gint            dpms_timeout_sec;
    gint            lock_timeout_sec;
    gboolean        lock_on_suspend;

    /* 360° Foldable Posture & Sensors */
    gboolean        posture_auto_rotate;
    gboolean        posture_disable_kbd_in_tablet;
    gboolean        posture_disable_touch_in_laptop;

    /* Keyboard Backlight & Auto-Off Timeout */
    gint            kbd_backlight_level;       /* 0 = Off, 1 = Low, 2 = High */
    gint            kbd_backlight_timeout_sec; /* 0 = Always On, 30 = 30s, 180 = 3m */

    /* Kajo IDE (org.kajo.IDE) Typography, Color Scheme & Chrome */
    gchar          *ide_color_scheme;          /* "kajo-metro-dark", "kajo-oled-pitch", etc. */
    gchar          *ide_font_family;           /* "Fira Code", "JetBrains Mono", etc. */
    gint            ide_font_size_px;          /* 10..24, default 13 */
    gint            ide_tab_width;             /* 2, 4, or 8, default 2 */
    gboolean        ide_show_minimap;
    gboolean        ide_show_line_numbers;
    gboolean        ide_highlight_current_line;
    gboolean        ide_word_wrap;             /* soft-wrap long lines, default off */
    gboolean        ide_show_margin_guide;     /* vertical guide at column 100, default off */
    gboolean        ide_insert_spaces;         /* Tab inserts spaces (default) instead of \t */

    /* Kajo Files (org.kajo.Files) View, Sort & Chrome Preferences */
    gchar          *files_view_mode;           /* "grid" or "list", default "grid" */
    gchar          *files_sort_field;          /* "name", "size", "modified", "type", default "name" */
    gboolean        files_sort_ascending;      /* default TRUE */
    gboolean        files_sort_folders_first;  /* default TRUE */
    gboolean        files_show_hidden;         /* default FALSE */
    gboolean        files_show_inspector;      /* default FALSE */

    /* Kajo Mod (org.kajo.Mod) Retro Amiga, C64 SID & Tracker Engine Defaults */
    gchar          *mod_filter_mode;           /* "a500", "a1200", "near", "sinc", default "a500" */
    gchar          *mod_led_mode;              /* "auto", "on", "off", default "auto" */
    gchar          *mod_sid_model;             /* "auto", "6581", "8580", default "auto" */
    gint            mod_stereo_sep_pct;        /* 0..100, default 70 */
    gchar          *mod_end_action;            /* "next-sub", "loop", "next-mod", default "next-sub" */
    gchar          *mod_hvsc_path;             /* optional HVSC root override, default "" */
    gint            mod_uade_timeout_sec;      /* 60..600, default 180 */
    gboolean        mod_auto_extract_archives; /* default TRUE */

    GPtrArray      *layout_left;   /* Array of ShellWidgetConfig* */
    GPtrArray      *layout_center; /* Array of ShellWidgetConfig* */
    GPtrArray      *layout_right;  /* Array of ShellWidgetConfig* */

    gchar          *css_path;
    gchar          *icon_map_path;
};

ShellConfig *shell_config_load(const gchar *path);
ShellConfig *shell_config_get_default(void);
gboolean     shell_config_save(ShellConfig *config, const gchar *path);
void         shell_config_destroy(ShellConfig *config);
void         shell_widget_config_free(ShellWidgetConfig *wc);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_CONFIG_H */
