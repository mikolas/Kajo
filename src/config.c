#include "config.h"

#include <json-glib/json-glib.h>
#include <string.h>

static ShellWidgetMode
parse_widget_mode_str(const gchar *str, ShellWidgetMode fallback)
{
    if (g_strcmp0(str, "icon") == 0) return WIDGET_MODE_ICON;
    if (g_strcmp0(str, "label") == 0) return WIDGET_MODE_LABEL;
    if (g_strcmp0(str, "full") == 0) return WIDGET_MODE_FULL;
    return fallback;
}

void
shell_widget_config_free(ShellWidgetConfig *wc)
{
    if (wc) {
        g_free(wc->id);
        g_free(wc->exec_cmd);
        g_free(wc->click_left_cmd);
        g_free(wc->click_right_cmd);
        g_free(wc);
    }
}

static void
free_widget_config(gpointer data)
{
    shell_widget_config_free((ShellWidgetConfig *)data);
}

static GPtrArray *
json_array_to_widget_configs(JsonArray *array, ShellWidgetMode default_mode)
{
    GPtrArray *arr = g_ptr_array_new_full(0, free_widget_config);
    if (!array)
        return arr;

    guint len = json_array_get_length(array);
    for (guint i = 0; i < len; i++) {
        JsonNode *node = json_array_get_element(array, i);
        ShellWidgetConfig *wc = g_new0(ShellWidgetConfig, 1);
        wc->mode = default_mode;

        if (JSON_NODE_HOLDS_VALUE(node)) {
            wc->id = g_strdup(json_node_get_string(node));
        } else if (JSON_NODE_HOLDS_OBJECT(node)) {
            JsonObject *obj = json_node_get_object(node);
            if (json_object_has_member(obj, "id")) {
                wc->id = g_strdup(json_object_get_string_member(obj, "id"));
            }
            if (json_object_has_member(obj, "mode")) {
                const gchar *m = json_object_get_string_member(obj, "mode");
                wc->mode = parse_widget_mode_str(m, default_mode);
            }
            if (json_object_has_member(obj, "interval_sec")) {
                wc->interval_sec = (gint)json_object_get_int_member(obj, "interval_sec");
            }
            if (json_object_has_member(obj, "exec_cmd")) {
                wc->exec_cmd = g_strdup(json_object_get_string_member(obj, "exec_cmd"));
            }
            if (json_object_has_member(obj, "click_left_cmd")) {
                wc->click_left_cmd = g_strdup(json_object_get_string_member(obj, "click_left_cmd"));
            }
            if (json_object_has_member(obj, "click_right_cmd")) {
                wc->click_right_cmd = g_strdup(json_object_get_string_member(obj, "click_right_cmd"));
            }
        }

        if (wc->id != NULL) {
            g_ptr_array_add(arr, wc);
        } else {
            shell_widget_config_free(wc);
        }
    }

    return arr;
}

ShellConfig *
shell_config_get_default(void)
{
    ShellConfig *config = g_new0(ShellConfig, 1);

    config->panel_position        = PANEL_POSITION_TOP;
    config->panel_height          = 32;
    config->panel_layer           = g_strdup("top");
    config->default_widget_mode   = WIDGET_MODE_ICON;
    config->icon_size             = 24;

    config->autohide_enabled      = FALSE;
    config->autohide_hide_delay   = 300;
    config->autohide_reveal_delay = 150;
    config->autohide_reveal_duration = 250;
    config->autohide_edge_strip_px = 2;

    config->osd_timeout_ms           = 1500;
    config->osd_width                = 200;
    config->osd_height               = 120;

    config->toast_timeout_ms         = 8000;
    config->max_notification_history = 50;

    config->launcher_width           = 640;
    config->launcher_max_height      = 320;

    config->settings_width           = 960;
    config->settings_height          = 640;
    config->settings_transition_ms   = 150;

    config->accent_color             = g_strdup("#0078d4");
    config->ui_font_size_px          = 13;

    config->layout_left           = g_ptr_array_new_full(0, free_widget_config);
    config->layout_center         = g_ptr_array_new_full(0, free_widget_config);
    config->layout_right          = g_ptr_array_new_full(0, free_widget_config);

    config->css_path              = NULL;
    config->icon_map_path         = NULL;

    return config;
}

ShellConfig *
shell_config_load(const gchar *path)
{
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    JsonNode *root_node = NULL;
    JsonObject *root = NULL;
    ShellConfig *config = NULL;
    gchar *resolved_path = NULL;

    /* Resolve config file path */
    if (path) {
        resolved_path = g_strdup(path);
    } else {
        gchar *cwd = g_get_current_dir();
        gchar *dev_config = g_build_filename(cwd, "data", "defaults", "config.json", NULL);
        g_free(cwd);
        if (g_file_test(dev_config, G_FILE_TEST_EXISTS)) {
            resolved_path = dev_config;
        } else {
            g_free(dev_config);
            const gchar *xdg_config = g_get_user_config_dir();
            resolved_path = g_build_filename(xdg_config, "kajo", "config.json", NULL);

            if (!g_file_test(resolved_path, G_FILE_TEST_EXISTS)) {
                g_free(resolved_path);
                resolved_path = g_strdup("/usr/share/kajo/defaults/config.json");
            }
        }
    }

    if (!g_file_test(resolved_path, G_FILE_TEST_EXISTS)) {
        g_message("Config file not found at '%s', using defaults", resolved_path);
        g_free(resolved_path);
        return shell_config_get_default();
    }

    parser = json_parser_new();
    if (!json_parser_load_from_file(parser, resolved_path, &error)) {
        g_warning("Failed to parse config '%s': %s", resolved_path, error->message);
        g_free(resolved_path);
        return shell_config_get_default();
    }

    g_free(resolved_path);

    root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
        g_warning("Config root is not a JSON object, using defaults");
        return shell_config_get_default();
    }

    root = json_node_get_object(root_node);
    config = shell_config_get_default();

    /* Panel settings */
    if (json_object_has_member(root, "panel")) {
        JsonObject *panel = json_object_get_object_member(root, "panel");

        if (json_object_has_member(panel, "position")) {
            const gchar *pos = json_object_get_string_member(panel, "position");
            if (g_strcmp0(pos, "bottom") == 0)
                config->panel_position = PANEL_POSITION_BOTTOM;
            else
                config->panel_position = PANEL_POSITION_TOP;
        }

        if (json_object_has_member(panel, "height"))
            config->panel_height = (gint)json_object_get_int_member(panel, "height");

        if (json_object_has_member(panel, "layer")) {
            g_free(config->panel_layer);
            config->panel_layer = g_strdup(json_object_get_string_member(panel, "layer"));
        }

        if (json_object_has_member(panel, "widget_mode")) {
            const gchar *wm = json_object_get_string_member(panel, "widget_mode");
            config->default_widget_mode = parse_widget_mode_str(wm, WIDGET_MODE_ICON);
        }

        if (json_object_has_member(panel, "icon_size")) {
            config->icon_size = (gint)json_object_get_int_member(panel, "icon_size");
        }
    }

    /* Autohide settings */
    if (json_object_has_member(root, "autohide")) {
        JsonObject *autohide = json_object_get_object_member(root, "autohide");

        if (json_object_has_member(autohide, "enabled"))
            config->autohide_enabled = json_object_get_boolean_member(autohide, "enabled");

        if (json_object_has_member(autohide, "hide_delay"))
            config->autohide_hide_delay = (gint)json_object_get_int_member(autohide, "hide_delay");

        if (json_object_has_member(autohide, "reveal_delay"))
            config->autohide_reveal_delay = (gint)json_object_get_int_member(autohide, "reveal_delay");

        if (json_object_has_member(autohide, "reveal_duration"))
            config->autohide_reveal_duration = (gint)json_object_get_int_member(autohide, "reveal_duration");
    }

    /* UI Configurability parameters */
    if (json_object_has_member(root, "autohide_edge_strip_px"))
        config->autohide_edge_strip_px = (gint)json_object_get_int_member(root, "autohide_edge_strip_px");

    if (json_object_has_member(root, "osd_timeout_ms"))
        config->osd_timeout_ms = (gint)json_object_get_int_member(root, "osd_timeout_ms");

    if (json_object_has_member(root, "osd_width"))
        config->osd_width = (gint)json_object_get_int_member(root, "osd_width");

    if (json_object_has_member(root, "osd_height"))
        config->osd_height = (gint)json_object_get_int_member(root, "osd_height");

    if (json_object_has_member(root, "toast_timeout_ms"))
        config->toast_timeout_ms = (gint)json_object_get_int_member(root, "toast_timeout_ms");

    if (json_object_has_member(root, "max_notification_history"))
        config->max_notification_history = (gint)json_object_get_int_member(root, "max_notification_history");

    if (json_object_has_member(root, "notifications")) {
        JsonObject *notif_obj = json_object_get_object_member(root, "notifications");
        if (notif_obj) {
            if (json_object_has_member(notif_obj, "toast_timeout_ms"))
                config->toast_timeout_ms = (gint)json_object_get_int_member(notif_obj, "toast_timeout_ms");
            if (json_object_has_member(notif_obj, "max_notification_history"))
                config->max_notification_history = (gint)json_object_get_int_member(notif_obj, "max_notification_history");
        }
    }

    if (json_object_has_member(root, "launcher_width"))
        config->launcher_width = (gint)json_object_get_int_member(root, "launcher_width");

    if (json_object_has_member(root, "launcher_max_height"))
        config->launcher_max_height = (gint)json_object_get_int_member(root, "launcher_max_height");

    if (json_object_has_member(root, "settings_width"))
        config->settings_width = (gint)json_object_get_int_member(root, "settings_width");

    if (json_object_has_member(root, "settings_height"))
        config->settings_height = (gint)json_object_get_int_member(root, "settings_height");

    if (json_object_has_member(root, "settings_transition_ms"))
        config->settings_transition_ms = (gint)json_object_get_int_member(root, "settings_transition_ms");

    if (json_object_has_member(root, "accent_color")) {
        g_free(config->accent_color);
        config->accent_color = g_strdup(json_object_get_string_member(root, "accent_color"));
    }

    if (json_object_has_member(root, "ui_font_size_px"))
        config->ui_font_size_px = (gint)json_object_get_int_member(root, "ui_font_size_px");

    /* Layout settings */
    if (json_object_has_member(root, "layout")) {
        JsonObject *layout = json_object_get_object_member(root, "layout");

        if (json_object_has_member(layout, "left")) {
            if (config->layout_left) g_ptr_array_free(config->layout_left, TRUE);
            config->layout_left = json_array_to_widget_configs(
                json_object_get_array_member(layout, "left"), config->default_widget_mode);
        }

        if (json_object_has_member(layout, "center")) {
            if (config->layout_center) g_ptr_array_free(config->layout_center, TRUE);
            config->layout_center = json_array_to_widget_configs(
                json_object_get_array_member(layout, "center"), config->default_widget_mode);
        }

        if (json_object_has_member(layout, "right")) {
            if (config->layout_right) g_ptr_array_free(config->layout_right, TRUE);
            config->layout_right = json_array_to_widget_configs(
                json_object_get_array_member(layout, "right"), config->default_widget_mode);
        }
    }

    /* CSS path */
    if (json_object_has_member(root, "css_path")) {
        g_free(config->css_path);
        config->css_path = g_strdup(json_object_get_string_member(root, "css_path"));
    }

    /* Icon map path */
    if (json_object_has_member(root, "icon_map_path")) {
        g_free(config->icon_map_path);
        config->icon_map_path = g_strdup(json_object_get_string_member(root, "icon_map_path"));
    }

    return config;
}

gboolean
shell_config_save(ShellConfig *config, const gchar *path)
{
    if (!config || !path) return FALSE;

    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "position");
    json_builder_add_string_value(builder, config->panel_position == PANEL_POSITION_BOTTOM ? "bottom" : "top");

    json_builder_set_member_name(builder, "height");
    json_builder_add_int_value(builder, config->panel_height);

    json_builder_set_member_name(builder, "autohide");
    json_builder_add_boolean_value(builder, config->autohide_enabled);

    json_builder_set_member_name(builder, "autohide_edge_strip_px");
    json_builder_add_int_value(builder, config->autohide_edge_strip_px > 0 ? config->autohide_edge_strip_px : 2);

    json_builder_set_member_name(builder, "osd_timeout_ms");
    json_builder_add_int_value(builder, config->osd_timeout_ms > 0 ? config->osd_timeout_ms : 1500);

    json_builder_set_member_name(builder, "osd_width");
    json_builder_add_int_value(builder, config->osd_width > 0 ? config->osd_width : 200);

    json_builder_set_member_name(builder, "osd_height");
    json_builder_add_int_value(builder, config->osd_height > 0 ? config->osd_height : 120);

    json_builder_set_member_name(builder, "toast_timeout_ms");
    json_builder_add_int_value(builder, config->toast_timeout_ms > 0 ? config->toast_timeout_ms : 8000);

    json_builder_set_member_name(builder, "max_notification_history");
    json_builder_add_int_value(builder, config->max_notification_history > 0 ? config->max_notification_history : 50);

    json_builder_set_member_name(builder, "launcher_width");
    json_builder_add_int_value(builder, config->launcher_width > 0 ? config->launcher_width : 640);

    json_builder_set_member_name(builder, "launcher_max_height");
    json_builder_add_int_value(builder, config->launcher_max_height > 0 ? config->launcher_max_height : 320);

    json_builder_set_member_name(builder, "settings_width");
    json_builder_add_int_value(builder, config->settings_width > 0 ? config->settings_width : 960);

    json_builder_set_member_name(builder, "settings_height");
    json_builder_add_int_value(builder, config->settings_height > 0 ? config->settings_height : 640);

    json_builder_set_member_name(builder, "settings_transition_ms");
    json_builder_add_int_value(builder, config->settings_transition_ms > 0 ? config->settings_transition_ms : 150);

    json_builder_set_member_name(builder, "accent_color");
    json_builder_add_string_value(builder, config->accent_color ? config->accent_color : "#0078d4");

    json_builder_set_member_name(builder, "ui_font_size_px");
    json_builder_add_int_value(builder, config->ui_font_size_px > 0 ? config->ui_font_size_px : 13);

    /* Layout settings serialization */
    json_builder_set_member_name(builder, "layout");
    json_builder_begin_object(builder);

    /* left array */
    json_builder_set_member_name(builder, "left");
    json_builder_begin_array(builder);
    if (config->layout_left) {
        for (guint i = 0; i < config->layout_left->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_left, i);
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, wc->id);
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(builder, wc->mode == WIDGET_MODE_FULL ? "full" : (wc->mode == WIDGET_MODE_LABEL ? "label" : "icon"));
            json_builder_end_object(builder);
        }
    }
    json_builder_end_array(builder);

    /* center array */
    json_builder_set_member_name(builder, "center");
    json_builder_begin_array(builder);
    if (config->layout_center) {
        for (guint i = 0; i < config->layout_center->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_center, i);
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, wc->id);
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(builder, wc->mode == WIDGET_MODE_FULL ? "full" : (wc->mode == WIDGET_MODE_LABEL ? "label" : "icon"));
            json_builder_end_object(builder);
        }
    }
    json_builder_end_array(builder);

    /* right array */
    json_builder_set_member_name(builder, "right");
    json_builder_begin_array(builder);
    if (config->layout_right) {
        for (guint i = 0; i < config->layout_right->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_right, i);
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, wc->id);
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(builder, wc->mode == WIDGET_MODE_FULL ? "full" : (wc->mode == WIDGET_MODE_LABEL ? "label" : "icon"));
            json_builder_end_object(builder);
        }
    }
    json_builder_end_array(builder);

    json_builder_end_object(builder);

    json_builder_end_object(builder);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, TRUE);

    return json_generator_to_file(gen, path, NULL);
}

void
shell_config_destroy(ShellConfig *config)
{
    if (!config)
        return;

    g_free(config->panel_layer);
    g_free(config->accent_color);
    if (config->layout_left) g_ptr_array_free(config->layout_left, TRUE);
    if (config->layout_center) g_ptr_array_free(config->layout_center, TRUE);
    if (config->layout_right) g_ptr_array_free(config->layout_right, TRUE);
    g_free(config->css_path);
    g_free(config->icon_map_path);
    g_free(config);
}
