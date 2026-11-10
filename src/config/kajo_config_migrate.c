#include "kajo_config_migrate.h"
#include "kajo_config_schema.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

static void
read_json_str(JsonObject *obj, const char *key, gchar **dst)
{
    if (!obj || !key || !dst || !json_object_has_member(obj, key))
        return;
    JsonNode *n = json_object_get_member(obj, key);
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) {
        const gchar *s = json_node_get_string(n);
        if (s) {
            gchar *dup = g_strdup(s);
            g_free(*dst);
            *dst = dup;
        }
    }
}

static void
read_json_hex(JsonObject *obj, const char *key, gchar **dst)
{
    if (!obj || !key || !dst || !json_object_has_member(obj, key))
        return;
    JsonNode *n = json_object_get_member(obj, key);
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) {
        const gchar *s = json_node_get_string(n);
        if (s && kajo_config_is_valid_hex_color(s)) {
            gchar *lower = g_ascii_strdown(s, -1);
            g_free(*dst);
            *dst = lower;
        }
    }
}

static void
read_json_int_clamped(JsonObject *obj, const char *key, gint *dst, gint min_v, gint max_v)
{
    if (!obj || !key || !dst || !json_object_has_member(obj, key))
        return;
    JsonNode *n = json_object_get_member(obj, key);
    if (JSON_NODE_HOLDS_VALUE(n) &&
        (json_node_get_value_type(n) == G_TYPE_INT64 ||
         json_node_get_value_type(n) == G_TYPE_DOUBLE)) {
        gint v = (gint)json_object_get_int_member(obj, key);
        if (min_v < max_v)
            v = CLAMP(v, min_v, max_v);
        *dst = v;
    }
}

static void
read_json_bool(JsonObject *obj, const char *key, gboolean *dst)
{
    if (!obj || !key || !dst || !json_object_has_member(obj, key))
        return;
    JsonNode *n = json_object_get_member(obj, key);
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN) {
        *dst = json_object_get_boolean_member(obj, key) ? TRUE : FALSE;
    }
}

static GPtrArray *
parse_json_widget_array(JsonArray *jarr, ShellWidgetMode default_mode)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)shell_widget_config_free);
    guint len = json_array_get_length(jarr);

    for (guint i = 0; i < len; i++) {
        JsonNode *elem = json_array_get_element(jarr, i);
        if (JSON_NODE_HOLDS_VALUE(elem) && json_node_get_value_type(elem) == G_TYPE_STRING) {
            const gchar *id = json_node_get_string(elem);
            if (id && *id)
                g_ptr_array_add(arr, shell_widget_config_new(id, default_mode));
        } else if (JSON_NODE_HOLDS_OBJECT(elem)) {
            JsonObject *wobj = json_node_get_object(elem);
            if (!json_object_has_member(wobj, "id"))
                continue;
            const gchar *id = json_object_get_string_member(wobj, "id");
            if (!id || !*id)
                continue;

            ShellWidgetMode mode = default_mode;
            if (json_object_has_member(wobj, "mode")) {
                mode = shell_widget_mode_from_string(json_object_get_string_member(wobj, "mode"));
            }

            ShellWidgetConfig *wc = shell_widget_config_new(id, mode);
            if (json_object_has_member(wobj, "interval")) {
                gint iv = (gint)json_object_get_int_member(wobj, "interval");
                if (iv > 0)
                    wc->interval_sec = CLAMP(iv, 1, 86400);
            }
            if (json_object_has_member(wobj, "interval_sec")) {
                gint iv = (gint)json_object_get_int_member(wobj, "interval_sec");
                if (iv > 0)
                    wc->interval_sec = CLAMP(iv, 1, 86400);
            }
            read_json_str(wobj, "exec",            &wc->exec_cmd);
            read_json_str(wobj, "exec_cmd",        &wc->exec_cmd);
            read_json_str(wobj, "click_left",      &wc->click_left_cmd);
            read_json_str(wobj, "click_left_cmd",  &wc->click_left_cmd);
            read_json_str(wobj, "click_right",     &wc->click_right_cmd);
            read_json_str(wobj, "click_right_cmd", &wc->click_right_cmd);
            g_ptr_array_add(arr, wc);
        }
    }
    return arr;
}

static void
import_theme_from_json(JsonObject *root, KajoThemeCfg *theme)
{
    read_json_hex(root, "accent_color", &theme->accent_color);
    read_json_int_clamped(root, "ui_font_size_px",       &theme->ui_font_size_px,       11, 20);
    read_json_int_clamped(root, "font_heading_size_px",  &theme->font_heading_size_px,  10, 18);
    read_json_int_clamped(root, "font_subtitle_size_px", &theme->font_subtitle_size_px, 10, 18);
    read_json_int_clamped(root, "font_badge_size_px",    &theme->font_badge_size_px,    9,  16);
    read_json_str(root, "css_path", &theme->css_path);
    read_json_int_clamped(root, "settings_width",        &theme->settings_width,        480, 3840);
    read_json_int_clamped(root, "settings_height",       &theme->settings_height,       360, 2160);

    if (json_object_has_member(root, "theme") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "theme"))) {
        JsonObject *tobj = json_object_get_object_member(root, "theme");
        read_json_hex(tobj, "accent_color", &theme->accent_color);
        read_json_int_clamped(tobj, "ui_font_size_px",       &theme->ui_font_size_px,       11, 20);
        read_json_int_clamped(tobj, "font_heading_size_px",  &theme->font_heading_size_px,  10, 18);
        read_json_int_clamped(tobj, "font_subtitle_size_px", &theme->font_subtitle_size_px, 10, 18);
        read_json_int_clamped(tobj, "font_badge_size_px",    &theme->font_badge_size_px,    9,  16);
        read_json_str(tobj, "css_path", &theme->css_path);
    }
}

static void
import_shell_from_json(JsonObject *root, KajoShellCfg *shell)
{
    g_autofree gchar *pos_str = NULL;
    g_autofree gchar *layer_str = NULL;
    g_autofree gchar *wm_str = NULL;
    g_autofree gchar *click_str = NULL;
    g_autofree gchar *sc_str = NULL;
    g_autofree gchar *sr_str = NULL;
    g_autofree gchar *sh_str = NULL;
    g_autofree gchar *osk_str = NULL;
    g_autofree gchar *ss_str = NULL;
    ShellWidgetMode default_widget_mode = WIDGET_MODE_ICON;

    read_json_str(root, "position", &pos_str);
    read_json_int_clamped(root, "height", &shell->panel_height, 24, 96);

    /* Nested "panel" or flat top-level */
    if (json_object_has_member(root, "panel") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "panel"))) {
        JsonObject *p = json_object_get_object_member(root, "panel");
        read_json_str(p, "position", &pos_str);
        read_json_int_clamped(p, "height", &shell->panel_height, 24, 96);
        read_json_str(p, "layer", &layer_str);
        read_json_str(p, "widget_mode", &wm_str);
        read_json_int_clamped(p, "icon_size", &shell->icon_size, 12, 64);

        if (json_object_has_member(p, "touch_gestures") &&
            JSON_NODE_HOLDS_OBJECT(json_object_get_member(p, "touch_gestures"))) {
            JsonObject *tg = json_object_get_object_member(p, "touch_gestures");
            read_json_bool(tg, "enabled", &shell->touch_gestures_enabled);
            read_json_int_clamped(tg, "dwell_ms", &shell->touch_dwell_ms, 500, 15000);
            read_json_str(tg, "swipe_center",     &sc_str);
            read_json_str(tg, "swipe_right",      &sr_str);
            read_json_str(tg, "swipe_horizontal", &sh_str);
            read_json_str(tg, "osk_auto_show_mode", &osk_str);
            read_json_bool(tg, "osk_push_windows", &shell->osk_push_windows);
            read_json_int_clamped(tg, "osk_height_px", &shell->osk_height_px, 180, 400);
        }
    }
    read_json_str(root, "panel_position", &pos_str);
    read_json_int_clamped(root, "panel_height", &shell->panel_height, 24, 96);
    read_json_str(root, "panel_layer", &layer_str);
    read_json_int_clamped(root, "icon_size", &shell->icon_size, 12, 64);

    if (pos_str)
        shell->panel_position = shell_panel_position_from_string(pos_str);
    if (layer_str)
        shell->panel_layer = shell_panel_layer_from_string(layer_str);
    if (wm_str)
        default_widget_mode = shell_widget_mode_from_string(wm_str);

    /* [autohide] */
    if (json_object_has_member(root, "autohide")) {
        JsonNode *ah_node = json_object_get_member(root, "autohide");
        if (JSON_NODE_HOLDS_VALUE(ah_node) && json_node_get_value_type(ah_node) == G_TYPE_BOOLEAN) {
            shell->autohide_enabled = json_node_get_boolean(ah_node) ? TRUE : FALSE;
        } else if (JSON_NODE_HOLDS_OBJECT(ah_node)) {
            JsonObject *ah = json_node_get_object(ah_node);
            read_json_bool(ah, "enabled", &shell->autohide_enabled);
            read_json_int_clamped(ah, "reveal_delay",    &shell->autohide_reveal_delay,    0, 5000);
            read_json_int_clamped(ah, "hide_delay",      &shell->autohide_hide_delay,      0, 10000);
            read_json_int_clamped(ah, "reveal_duration", &shell->autohide_reveal_duration, 0, 2000);
            read_json_int_clamped(ah, "hide_duration",   &shell->autohide_hide_duration,   0, 2000);
        }
    }
    read_json_bool(root, "autohide_enabled", &shell->autohide_enabled);
    read_json_int_clamped(root, "autohide_reveal_delay",    &shell->autohide_reveal_delay,    0, 5000);
    read_json_int_clamped(root, "autohide_hide_delay",      &shell->autohide_hide_delay,      0, 10000);
    read_json_int_clamped(root, "autohide_reveal_duration", &shell->autohide_reveal_duration, 0, 2000);
    read_json_int_clamped(root, "autohide_hide_duration",   &shell->autohide_hide_duration,   0, 2000);

    /* [notifications] */
    read_json_int_clamped(root, "toast_timeout_ms",         &shell->toast_timeout_ms, 1000, 30000);
    read_json_int_clamped(root, "max_notification_history", &shell->max_notification_history, 10, 200);
    read_json_str(root, "toast_click_action", &click_str);
    read_json_bool(root, "dnd_enabled", &shell->dnd_enabled);
    if (json_object_has_member(root, "notifications") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "notifications"))) {
        JsonObject *nf = json_object_get_object_member(root, "notifications");
        read_json_int_clamped(nf, "toast_timeout_ms",         &shell->toast_timeout_ms, 1000, 30000);
        read_json_int_clamped(nf, "max_history",              &shell->max_notification_history, 10, 200);
        read_json_int_clamped(nf, "max_notification_history", &shell->max_notification_history, 10, 200);
        read_json_str(nf, "click_action",       &click_str);
        read_json_str(nf, "toast_click_action", &click_str);
        read_json_bool(nf, "dnd_enabled", &shell->dnd_enabled);
    }
    if (click_str)
        shell->toast_click_action = shell_toast_click_action_from_string(click_str);

    /* [touch] / "touch_gestures" */
    if (json_object_has_member(root, "touch_gestures") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "touch_gestures"))) {
        JsonObject *tc = json_object_get_object_member(root, "touch_gestures");
        read_json_bool(tc, "enabled", &shell->touch_gestures_enabled);
        read_json_str(tc, "swipe_center",     &sc_str);
        read_json_str(tc, "swipe_right",      &sr_str);
        read_json_str(tc, "swipe_horizontal", &sh_str);
        read_json_int_clamped(tc, "dwell_ms", &shell->touch_dwell_ms, 500, 15000);
    }
    if (json_object_has_member(root, "touch") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "touch"))) {
        JsonObject *tc = json_object_get_object_member(root, "touch");
        read_json_bool(tc, "enabled", &shell->touch_gestures_enabled);
        read_json_str(tc, "swipe_center",     &sc_str);
        read_json_str(tc, "swipe_right",      &sr_str);
        read_json_str(tc, "swipe_horizontal", &sh_str);
        read_json_int_clamped(tc, "dwell_ms", &shell->touch_dwell_ms, 500, 15000);
    }
    read_json_bool(root, "touch_gestures_enabled", &shell->touch_gestures_enabled);
    read_json_str(root, "touch_swipe_center",     &sc_str);
    read_json_str(root, "touch_swipe_right",      &sr_str);
    read_json_str(root, "touch_swipe_horizontal", &sh_str);
    read_json_int_clamped(root, "touch_dwell_ms", &shell->touch_dwell_ms, 500, 15000);

    if (sc_str) {
        if (g_ascii_strcasecmp(sc_str, "overview") == 0)  shell->touch_swipe_center = SWIPE_CENTER_OVERVIEW;
        else if (g_ascii_strcasecmp(sc_str, "osk") == 0)  shell->touch_swipe_center = SWIPE_CENTER_OSK;
        else if (g_ascii_strcasecmp(sc_str, "none") == 0) shell->touch_swipe_center = SWIPE_CENTER_NONE;
        else                                              shell->touch_swipe_center = SWIPE_CENTER_LAUNCHER;
    }
    if (sr_str) {
        if (g_ascii_strcasecmp(sr_str, "notifications") == 0) shell->touch_swipe_right = SWIPE_RIGHT_NOTIFICATIONS;
        else if (g_ascii_strcasecmp(sr_str, "osk") == 0)      shell->touch_swipe_right = SWIPE_RIGHT_OSK;
        else if (g_ascii_strcasecmp(sr_str, "none") == 0)     shell->touch_swipe_right = SWIPE_RIGHT_NONE;
        else                                                  shell->touch_swipe_right = SWIPE_RIGHT_CONTROL_CENTER;
    }
    if (sh_str) {
        if (g_ascii_strcasecmp(sh_str, "workspace") == 0) shell->touch_swipe_horizontal = SWIPE_HORIZ_WORKSPACE;
        else if (g_ascii_strcasecmp(sh_str, "none") == 0) shell->touch_swipe_horizontal = SWIPE_HORIZ_NONE;
        else                                              shell->touch_swipe_horizontal = SWIPE_HORIZ_COLUMN;
    }

    /* [osk] */
    if (json_object_has_member(root, "osk") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "osk"))) {
        JsonObject *ok = json_object_get_object_member(root, "osk");
        read_json_str(ok, "auto_show_mode", &osk_str);
        read_json_bool(ok, "push_windows", &shell->osk_push_windows);
        read_json_int_clamped(ok, "height_px", &shell->osk_height_px, 180, 400);
    }
    read_json_str(root, "osk_auto_show_mode", &osk_str);
    read_json_bool(root, "osk_push_windows", &shell->osk_push_windows);
    read_json_int_clamped(root, "osk_height_px", &shell->osk_height_px, 180, 400);
    if (osk_str) {
        if (g_ascii_strcasecmp(osk_str, "always") == 0)      shell->osk_auto_show_mode = OSK_AUTO_SHOW_ALWAYS;
        else if (g_ascii_strcasecmp(osk_str, "manual") == 0) shell->osk_auto_show_mode = OSK_AUTO_SHOW_MANUAL;
        else                                                 shell->osk_auto_show_mode = OSK_AUTO_SHOW_TABLET;
    }

    /* [lock] / "idle" */
    if (json_object_has_member(root, "idle") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "idle"))) {
        JsonObject *lk = json_object_get_object_member(root, "idle");
        read_json_str(lk, "screensaver_mode", &ss_str);
        read_json_int_clamped(lk, "screensaver_timeout_sec", &shell->screensaver_timeout_sec, 0, 86400);
        read_json_int_clamped(lk, "dpms_timeout_sec",        &shell->dpms_timeout_sec,        0, 86400);
        read_json_int_clamped(lk, "lock_timeout_sec",        &shell->lock_timeout_sec,        0, 86400);
        read_json_bool(lk, "lock_on_suspend", &shell->lock_on_suspend);
    }
    if (json_object_has_member(root, "lock") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "lock"))) {
        JsonObject *lk = json_object_get_object_member(root, "lock");
        read_json_str(lk, "screensaver_mode", &ss_str);
        read_json_int_clamped(lk, "screensaver_timeout_sec", &shell->screensaver_timeout_sec, 0, 86400);
        read_json_int_clamped(lk, "dpms_timeout_sec",        &shell->dpms_timeout_sec,        0, 86400);
        read_json_int_clamped(lk, "lock_timeout_sec",        &shell->lock_timeout_sec,        0, 86400);
        read_json_bool(lk, "lock_on_suspend", &shell->lock_on_suspend);
    }
    read_json_str(root, "screensaver_mode", &ss_str);
    read_json_int_clamped(root, "screensaver_timeout_sec", &shell->screensaver_timeout_sec, 0, 86400);
    read_json_int_clamped(root, "dpms_timeout_sec",        &shell->dpms_timeout_sec,        0, 86400);
    read_json_int_clamped(root, "lock_timeout_sec",        &shell->lock_timeout_sec,        0, 86400);
    read_json_bool(root, "lock_on_suspend", &shell->lock_on_suspend);
    if (ss_str)
        shell->screensaver_mode = shell_screensaver_mode_from_string(ss_str);

    /* [hardware] / "posture" + "keyboard_backlight" */
    if (json_object_has_member(root, "posture") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "posture"))) {
        JsonObject *po = json_object_get_object_member(root, "posture");
        read_json_bool(po, "auto_rotate",             &shell->posture_auto_rotate);
        read_json_bool(po, "disable_kbd_in_tablet",   &shell->posture_disable_kbd_in_tablet);
        read_json_bool(po, "disable_touch_in_laptop", &shell->posture_disable_touch_in_laptop);
    }
    if (json_object_has_member(root, "keyboard_backlight") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "keyboard_backlight"))) {
        JsonObject *kb = json_object_get_object_member(root, "keyboard_backlight");
        read_json_int_clamped(kb, "level",       &shell->kbd_backlight_level,       0, 2);
        read_json_int_clamped(kb, "timeout_sec", &shell->kbd_backlight_timeout_sec, 0, 86400);
    }
    if (json_object_has_member(root, "hardware") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "hardware"))) {
        JsonObject *hw = json_object_get_object_member(root, "hardware");
        read_json_int_clamped(hw, "kbd_backlight_level",       &shell->kbd_backlight_level,       0, 2);
        read_json_int_clamped(hw, "kbd_backlight_timeout_sec", &shell->kbd_backlight_timeout_sec, 0, 86400);
        read_json_bool(hw, "posture_auto_rotate",             &shell->posture_auto_rotate);
        read_json_bool(hw, "posture_disable_kbd_in_tablet",   &shell->posture_disable_kbd_in_tablet);
        read_json_bool(hw, "posture_disable_touch_in_laptop", &shell->posture_disable_touch_in_laptop);
    }
    read_json_int_clamped(root, "kbd_backlight_level",       &shell->kbd_backlight_level,       0, 2);
    read_json_int_clamped(root, "kbd_backlight_timeout_sec", &shell->kbd_backlight_timeout_sec, 0, 86400);
    read_json_bool(root, "posture_auto_rotate",             &shell->posture_auto_rotate);
    read_json_bool(root, "posture_disable_kbd_in_tablet",   &shell->posture_disable_kbd_in_tablet);
    read_json_bool(root, "posture_disable_touch_in_laptop", &shell->posture_disable_touch_in_laptop);

    /* [layout] ("layout" or "widgets" in JSON) */
    const char *layout_keys[] = { "layout", "widgets" };
    for (size_t k = 0; k < G_N_ELEMENTS(layout_keys); k++) {
        if (json_object_has_member(root, layout_keys[k]) &&
            JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, layout_keys[k]))) {
            JsonObject *w = json_object_get_object_member(root, layout_keys[k]);
            if (json_object_has_member(w, "left") &&
                JSON_NODE_HOLDS_ARRAY(json_object_get_member(w, "left"))) {
                g_ptr_array_free(shell->layout_left, TRUE);
                shell->layout_left = parse_json_widget_array(json_object_get_array_member(w, "left"), default_widget_mode);
            }
            if (json_object_has_member(w, "center") &&
                JSON_NODE_HOLDS_ARRAY(json_object_get_member(w, "center"))) {
                g_ptr_array_free(shell->layout_center, TRUE);
                shell->layout_center = parse_json_widget_array(json_object_get_array_member(w, "center"), default_widget_mode);
            }
            if (json_object_has_member(w, "right") &&
                JSON_NODE_HOLDS_ARRAY(json_object_get_member(w, "right"))) {
                g_ptr_array_free(shell->layout_right, TRUE);
                shell->layout_right = parse_json_widget_array(json_object_get_array_member(w, "right"), default_widget_mode);
            }
        }
    }
}

static void
import_ide_from_json(JsonObject *root, KajoIdeCfg *ide)
{
    if (json_object_has_member(root, "ide") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "ide"))) {
        JsonObject *io = json_object_get_object_member(root, "ide");
        read_json_str(io, "color_scheme", &ide->color_scheme);
        read_json_str(io, "font_family",  &ide->font_family);
        read_json_int_clamped(io, "font_size_px", &ide->font_size_px, 9, 28);
        gint tw = ide->tab_width;
        read_json_int_clamped(io, "tab_width", &tw, 2, 8);
        if (tw == 2 || tw == 4 || tw == 8) ide->tab_width = tw;
        read_json_bool(io, "insert_spaces",          &ide->insert_spaces);
        read_json_bool(io, "word_wrap",              &ide->word_wrap);
        read_json_bool(io, "show_margin_guide",      &ide->show_margin_guide);
        read_json_bool(io, "show_minimap",           &ide->show_minimap);
        read_json_bool(io, "show_line_numbers",      &ide->show_line_numbers);
        read_json_bool(io, "highlight_current_line", &ide->highlight_current_line);
    }

    read_json_str(root, "ide_color_scheme", &ide->color_scheme);
    read_json_str(root, "ide_font_family",  &ide->font_family);
    read_json_int_clamped(root, "ide_font_size_px", &ide->font_size_px, 9, 28);
    gint tw = ide->tab_width;
    read_json_int_clamped(root, "ide_tab_width", &tw, 2, 8);
    if (tw == 2 || tw == 4 || tw == 8) ide->tab_width = tw;
    read_json_bool(root, "ide_insert_spaces",          &ide->insert_spaces);
    read_json_bool(root, "ide_word_wrap",              &ide->word_wrap);
    read_json_bool(root, "ide_show_margin_guide",      &ide->show_margin_guide);
    read_json_bool(root, "ide_show_minimap",           &ide->show_minimap);
    read_json_bool(root, "ide_show_line_numbers",      &ide->show_line_numbers);
    read_json_bool(root, "ide_highlight_current_line", &ide->highlight_current_line);
}

static void
import_files_from_json(JsonObject *root, KajoFilesCfg *files)
{
    g_autofree gchar *view_str = NULL;
    g_autofree gchar *sort_str = NULL;

    if (json_object_has_member(root, "files") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "files"))) {
        JsonObject *fo = json_object_get_object_member(root, "files");
        read_json_str(fo, "view_mode",  &view_str);
        read_json_str(fo, "sort_field", &sort_str);
        read_json_bool(fo, "sort_ascending",     &files->sort_ascending);
        read_json_bool(fo, "sort_folders_first", &files->sort_folders_first);
        read_json_bool(fo, "show_hidden",        &files->show_hidden);
        read_json_bool(fo, "show_inspector",     &files->show_inspector);
    }

    read_json_str(root, "files_view_mode",  &view_str);
    read_json_str(root, "files_sort_field", &sort_str);
    read_json_bool(root, "files_sort_ascending",     &files->sort_ascending);
    read_json_bool(root, "files_sort_folders_first", &files->sort_folders_first);
    read_json_bool(root, "files_show_hidden",        &files->show_hidden);
    read_json_bool(root, "files_show_inspector",     &files->show_inspector);

    if (view_str) {
        files->view_mode = (g_ascii_strcasecmp(view_str, "list") == 0)
            ? KAJO_FILES_VIEW_LIST
            : KAJO_FILES_VIEW_GRID;
    }
    if (sort_str) {
        if (g_ascii_strcasecmp(sort_str, "size") == 0)          files->sort_field = KAJO_FILES_SORT_SIZE;
        else if (g_ascii_strcasecmp(sort_str, "modified") == 0) files->sort_field = KAJO_FILES_SORT_MODIFIED;
        else if (g_ascii_strcasecmp(sort_str, "type") == 0)     files->sort_field = KAJO_FILES_SORT_TYPE;
        else                                                    files->sort_field = KAJO_FILES_SORT_NAME;
    }
}

static void
import_mod_from_json(JsonObject *root, KajoModCfg *mod)
{
    g_autofree gchar *flt_str = NULL;
    g_autofree gchar *led_str = NULL;
    g_autofree gchar *end_str = NULL;
    g_autofree gchar *sid_str = NULL;

    if (json_object_has_member(root, "mod") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "mod"))) {
        JsonObject *mo = json_object_get_object_member(root, "mod");
        read_json_str(mo, "filter_mode", &flt_str);
        read_json_str(mo, "led_mode",    &led_str);
        read_json_int_clamped(mo, "stereo_sep_pct", &mod->stereo_sep_pct, 0, 100);
        read_json_str(mo, "end_action",  &end_str);
        read_json_str(mo, "sid_model",   &sid_str);
        gint ut = mod->uade_timeout_sec;
        read_json_int_clamped(mo, "uade_timeout_sec", &ut, 0, 600);
        mod->uade_timeout_sec = (ut == 0) ? 0 : CLAMP(ut, 60, 600);
        read_json_bool(mo, "auto_extract_archives", &mod->auto_extract_archives);
        read_json_str(mo, "hvsc_path", &mod->hvsc_path);
    }

    read_json_str(root, "mod_filter_mode", &flt_str);
    read_json_str(root, "mod_led_mode",    &led_str);
    read_json_int_clamped(root, "mod_stereo_sep_pct", &mod->stereo_sep_pct, 0, 100);
    read_json_str(root, "mod_end_action",  &end_str);
    read_json_str(root, "mod_sid_model",   &sid_str);
    if (json_object_has_member(root, "mod_uade_timeout_sec")) {
        gint ut = (gint)json_object_get_int_member(root, "mod_uade_timeout_sec");
        mod->uade_timeout_sec = (ut == 0) ? 0 : CLAMP(ut, 60, 600);
    }
    read_json_bool(root, "mod_auto_extract_archives", &mod->auto_extract_archives);
    read_json_str(root, "mod_hvsc_path", &mod->hvsc_path);

    if (flt_str) mod->filter_mode = kajo_mod_filter_mode_from_string(flt_str);
    if (led_str) mod->led_mode    = kajo_mod_led_mode_from_string(led_str);
    if (end_str) mod->end_action  = kajo_mod_end_action_from_string(end_str);
    if (sid_str) mod->sid_model   = kajo_mod_sid_model_from_string(sid_str);
}

gboolean
kajo_config_migrate_legacy_json(const char *config_dir)
{
    if (!config_dir || !*config_dir)
        return FALSE;

    g_autofree gchar *json_path  = g_build_filename(config_dir, "config.json", NULL);
    g_autofree gchar *shell_path = g_build_filename(config_dir, KAJO_CFG_FILE_SHELL, NULL);

    if (!g_file_test(json_path, G_FILE_TEST_EXISTS) ||
        g_file_test(shell_path, G_FILE_TEST_EXISTS)) {
        return FALSE;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) err = NULL;
    if (!json_parser_load_from_file(parser, json_path, &err)) {
        g_warning("Config migration: failed to parse '%s' (%s), skipping migration",
                  json_path, err ? err->message : "unknown");
        return FALSE;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
        g_warning("Config migration: root of '%s' is not a JSON object", json_path);
        return FALSE;
    }

    JsonObject *root = json_node_get_object(root_node);

    g_autoptr(KajoThemeCfg) theme = kajo_theme_cfg_new_default();
    g_autoptr(KajoShellCfg) shell = kajo_shell_cfg_new_default();
    g_autoptr(KajoIdeCfg)   ide   = kajo_ide_cfg_new_default();
    g_autoptr(KajoFilesCfg) files = kajo_files_cfg_new_default();
    g_autoptr(KajoModCfg)   mod   = kajo_mod_cfg_new_default();

    import_theme_from_json(root, theme);
    import_shell_from_json(root, shell);
    import_ide_from_json(root, ide);
    import_files_from_json(root, files);
    import_mod_from_json(root, mod);

    g_autofree gchar *theme_path = g_build_filename(config_dir, KAJO_CFG_FILE_THEME, NULL);
    g_autofree gchar *ide_path   = g_build_filename(config_dir, KAJO_CFG_FILE_IDE,   NULL);
    g_autofree gchar *files_path = g_build_filename(config_dir, KAJO_CFG_FILE_FILES, NULL);
    g_autofree gchar *mod_path   = g_build_filename(config_dir, KAJO_CFG_FILE_MOD,   NULL);

    gboolean ok = TRUE;
    ok = ok && kajo_cfg_domain_save_atomic(kajo_schema_get_theme(), theme, theme_path, NULL);
    ok = ok && kajo_cfg_domain_save_atomic(kajo_schema_get_shell(), shell, shell_path, NULL);
    ok = ok && kajo_cfg_domain_save_atomic(kajo_schema_get_ide(),   ide,   ide_path,   NULL);
    ok = ok && kajo_cfg_domain_save_atomic(kajo_schema_get_files(), files, files_path, NULL);
    ok = ok && kajo_cfg_domain_save_atomic(kajo_schema_get_mod(),   mod,   mod_path,   NULL);

    if (ok) {
        g_autofree gchar *bak_path = g_build_filename(config_dir, "config.json.bak", NULL);
        g_rename(json_path, bak_path);
        g_message("Migrated legacy '%s' -> domain TOML files in '%s' (backed up to config.json.bak)",
                  json_path, config_dir);
    }

    return ok;
}
