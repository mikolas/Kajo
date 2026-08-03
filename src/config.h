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

    gint            osd_timeout_ms;
    gint            osd_width;
    gint            osd_height;

    gint            toast_timeout_ms;
    gint            max_notification_history;

    gint            launcher_width;
    gint            launcher_max_height;

    gint            settings_width;
    gint            settings_height;
    gint            settings_transition_ms;

    gchar          *accent_color;
    gint            ui_font_size_px;

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
