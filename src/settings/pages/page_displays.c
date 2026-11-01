#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../hardware/hw_caps.h"
#include "../../hardware/kbd_backlight.h"
#include <fcntl.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

static void
run_niri_output_cmd(const char *out_name, const char *action, const char *arg)
{
    if (!out_name || !action || !arg) return;
    const gchar *argv[] = { "niri", "msg", "output", out_name, action, arg, NULL };
    g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, NULL, NULL);
}

static void
on_vrr_tile_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *lbl_sub = GTK_WIDGET(user_data);
    if (!lbl_sub) return;
    gboolean active = gtk_widget_has_css_class(GTK_WIDGET(btn), "active");
    gtk_label_set_text(GTK_LABEL(lbl_sub), active ? "Adaptive Sync Active" : "Adaptive Sync Disabled");
}

static void
on_apply_displays_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    GtkDropDown *combo_res = g_object_get_data(G_OBJECT(btn), "combo_res");
    GtkDropDown *combo_orient = g_object_get_data(G_OBJECT(btn), "combo_orient");
    GtkDropDown *combo_scale = g_object_get_data(G_OBJECT(btn), "combo_scale");
    GtkWidget *tile_vrr = g_object_get_data(G_OBJECT(btn), "tile_vrr");
    GtkWidget *lbl_mon_hdr = g_object_get_data(G_OBJECT(btn), "lbl_mon_hdr");
    const char *out_name = g_object_get_data(G_OBJECT(btn), "output_name");
    const char *make_model = g_object_get_data(G_OBJECT(btn), "make_model");
    GPtrArray *raw_modes = g_object_get_data(G_OBJECT(btn), "raw_modes");

    if (!out_name || out_name[0] == '\0') out_name = "eDP-1";

    const char *res = "auto";
    if (combo_res && raw_modes) {
        guint sel = gtk_drop_down_get_selected(combo_res);
        if (sel < raw_modes->len) {
            res = g_ptr_array_index(raw_modes, sel);
        }
    }

    const char *orient = "normal";
    if (combo_orient) {
        guint sel = gtk_drop_down_get_selected(combo_orient);
        if (sel == 1) orient = "90";
        else if (sel == 2) orient = "180";
        else if (sel == 3) orient = "270";
    }

    double sc = 1.00;
    if (combo_scale) {
        guint sel = gtk_drop_down_get_selected(combo_scale);
        if (sel == 0) sc = 1.00;
        else if (sel == 1) sc = 1.25;
        else if (sel == 2) sc = 1.50;
        else if (sel == 3) sc = 1.75;
        else if (sel == 4) sc = 2.00;
    }

    gboolean vrr = tile_vrr ? gtk_widget_has_css_class(tile_vrr, "active") : FALSE;

    gchar scale_buf[32];
    g_ascii_formatd(scale_buf, sizeof(scale_buf), "%.2f", sc);

    const gchar *xdg_config = g_get_user_config_dir();
    gchar *dir = g_build_filename(xdg_config, "niri", "config.d", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "10-outputs.kdl", NULL);

    gchar *kdl_content = g_strdup_printf(
        "// Managed by kajo-settings — DO NOT EDIT DIRECTLY\n\n"
        "output \"%s\" {\n"
        "    mode \"%s\"\n"
        "    scale %s\n"
        "    transform \"%s\"\n"
        "    position x=0 y=0\n"
        "%s"
        "}\n",
        out_name, res, scale_buf, orient,
        vrr ? "    variable-refresh-rate\n" : "");

    g_file_set_contents(path, kdl_content, -1, NULL);
    g_message("Saved modular Niri outputs configuration for '%s' to %s", out_name, path);
    g_free(kdl_content);
    g_free(dir);
    g_free(path);

    /* Apply live to Niri compositor via IPC */
    run_niri_output_cmd(out_name, "mode", res);
    run_niri_output_cmd(out_name, "scale", scale_buf);
    run_niri_output_cmd(out_name, "transform", orient);
    run_niri_output_cmd(out_name, "vrr", vrr ? "on" : "off");
    niri_send_action("{\"Action\":\"ReloadConfig\"}");

    if (lbl_mon_hdr) {
        gchar *hdr_text = g_strdup_printf("Monitor %s — %s (%s, Scale %.0f%%)",
                                          out_name,
                                          make_model ? make_model : "Display",
                                          res,
                                          sc * 100.0);
        gtk_label_set_text(GTK_LABEL(lbl_mon_hdr), hdr_text);
        g_free(hdr_text);
    }
}

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageDisplays {
    GtkBox parent_instance;

    GtkWidget *lbl_mon_hdr;
    GtkWidget *combo_res;
    GtkWidget *combo_orient;
    GtkWidget *combo_scale;
    GtkWidget *grid_features;
    GtkWidget *btn_apply;

    GPtrArray *raw_modes;

    GtkWidget *combo_ss_mode;
    GtkWidget *combo_ss_timeout;
    GtkWidget *combo_dpms_timeout;
    GtkWidget *combo_lock_timeout;
    GtkWidget *sw_lock_suspend;
    GtkWidget *btn_preview_ss;
    GtkWidget *btn_lock_now;
    GtkWidget *btn_apply_idle;

    /* Keyboard Backlight & Timeout Card */
    GtkWidget *card_kbd_backlight;
    GtkWidget *combo_kbd_level;
    GtkWidget *row_kbd_timeout;
    GtkWidget *combo_kbd_timeout;
    GtkWidget *btn_apply_kbd;

    /* 360° Foldable Posture Card */
    GtkWidget *card_posture;
    GtkWidget *lbl_sensor_status;
    GtkWidget *row_posture_autorotate;
    GtkWidget *sw_posture_autorotate;
    GtkWidget *row_posture_disable_kbd;
    GtkWidget *sw_posture_disable_kbd;
    GtkWidget *row_posture_disable_touch;
    GtkWidget *sw_posture_disable_touch;
    GtkWidget *btn_apply_posture;
} SettingsPageDisplays;

typedef struct _SettingsPageDisplaysClass {
    GtkBoxClass parent_class;
} SettingsPageDisplaysClass;

G_DEFINE_TYPE(SettingsPageDisplays, settings_page_displays, GTK_TYPE_BOX)

static const gint ss_timeout_values[]   = {  60, 300, 600,  900, 1800, 0 };
static const gint dpms_timeout_values[] = { 300, 600, 900, 1800,    0 };
static const gint lock_timeout_values[] = { 300, 600, 900, 1800,    0 };

static guint
timeout_sec_to_index(gint sec, const gint *values, guint count, guint fallback_idx)
{
    for (guint i = 0; i < count; i++) {
        if (values[i] == sec)
            return i;
    }
    return fallback_idx;
}

static void
on_preview_screensaver_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    settings_send_ipc_command("screensaver");
}

static void
on_lock_now_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    settings_send_ipc_command("lock");
}

static void
on_apply_idle_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageDisplays *page = user_data;
    if (!page)
        return;

    ShellConfig *cfg = shell_config_load(NULL);
    if (!cfg)
        cfg = shell_config_get_default();

    /* Mode */
    guint mode_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_ss_mode));
    g_free(cfg->screensaver_mode);
    if (mode_sel == 1)
        cfg->screensaver_mode = g_strdup("dpms");
    else if (mode_sel == 2)
        cfg->screensaver_mode = g_strdup("disabled");
    else
        cfg->screensaver_mode = g_strdup("overlay");

    /* Timeouts */
    guint ss_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_ss_timeout));
    if (ss_sel < G_N_ELEMENTS(ss_timeout_values))
        cfg->screensaver_timeout_sec = ss_timeout_values[ss_sel];

    guint dpms_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_dpms_timeout));
    if (dpms_sel < G_N_ELEMENTS(dpms_timeout_values))
        cfg->dpms_timeout_sec = dpms_timeout_values[dpms_sel];

    guint lock_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_lock_timeout));
    if (lock_sel < G_N_ELEMENTS(lock_timeout_values))
        cfg->lock_timeout_sec = lock_timeout_values[lock_sel];

    cfg->lock_on_suspend = gtk_switch_get_active(GTK_SWITCH(page->sw_lock_suspend));

    const gchar *xdg_config = g_get_user_config_dir();
    g_autofree gchar *cfg_path = g_build_filename(xdg_config, "kajo", "config.json", NULL);
    shell_config_save(cfg, cfg_path);
    shell_config_destroy(cfg);

    settings_send_ipc_command("reload-config");

    if (btn) {
        gtk_button_set_label(btn, "[ ✓ SAVED & APPLIED TO SHELL ]");
    }
}

static const gint kbd_timeout_values[] = { 0, 30, 180 };

static void
write_inhibit_paths_direct(GPtrArray *paths, gboolean inhibit)
{
    if (!paths)
        return;
    const gchar *val = inhibit ? "1\n" : "0\n";
    size_t len = strlen(val);
    for (guint i = 0; i < paths->len; i++) {
        const gchar *p = g_ptr_array_index(paths, i);
        if (!p)
            continue;
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            ssize_t unused G_GNUC_UNUSED = write(fd, val, len);
            close(fd);
        }
    }
}

static void
on_apply_kbd_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageDisplays *page = user_data;
    if (!page)
        return;

    ShellConfig *cfg = shell_config_load(NULL);
    if (!cfg)
        cfg = shell_config_get_default();

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    gint sel = (gint)gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_kbd_level));

    if (caps && caps->kbd_bl_backend == KBD_BL_HP_EC) {
        /* On HP EC: sel == 0 -> Wake/Enable Keylights, sel == 1 -> Leave Off on Boot */
        cfg->kbd_backlight_level = (sel == 0) ? 2 : 0;
        if (sel == 0 && shell_kbd_backlight_get_level() <= 0) {
            shell_kbd_backlight_set_level(2);
        }
    } else {
        gint level = CLAMP(sel, 0, 2);
        cfg->kbd_backlight_level = level;
        shell_kbd_backlight_set_level(level);
    }

    if (shell_kbd_backlight_has_timeout()) {
        guint t_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_kbd_timeout));
        if (t_sel < G_N_ELEMENTS(kbd_timeout_values)) {
            cfg->kbd_backlight_timeout_sec = kbd_timeout_values[t_sel];
            shell_kbd_backlight_set_timeout_sec(cfg->kbd_backlight_timeout_sec);
        }
    }

    const gchar *xdg_config = g_get_user_config_dir();
    g_autofree gchar *cfg_path = g_build_filename(xdg_config, "kajo", "config.json", NULL);
    shell_config_save(cfg, cfg_path);
    shell_config_destroy(cfg);

    settings_send_ipc_command("reload-config");

    if (btn) {
        gtk_button_set_label(btn, "[ ✓ KEYBOARD BACKLIGHT APPLIED ]");
    }
}

static void
on_apply_posture_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageDisplays *page = user_data;
    if (!page)
        return;

    ShellConfig *cfg = shell_config_load(NULL);
    if (!cfg)
        cfg = shell_config_get_default();

    cfg->posture_auto_rotate = gtk_switch_get_active(GTK_SWITCH(page->sw_posture_autorotate));
    cfg->posture_disable_kbd_in_tablet = gtk_switch_get_active(GTK_SWITCH(page->sw_posture_disable_kbd));
    cfg->posture_disable_touch_in_laptop = gtk_switch_get_active(GTK_SWITCH(page->sw_posture_disable_touch));

    /* Apply input inhibition immediately for current posture */
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (caps) {
        gdouble deg = 0.0;
        gboolean is_folded = (shell_hw_caps_read_hinge_angle_deg(caps, &deg) && deg >= 180.0);
        write_inhibit_paths_direct(caps->kbd_inhibit_paths, is_folded && cfg->posture_disable_kbd_in_tablet);
        write_inhibit_paths_direct(caps->touchpad_inhibit_paths, is_folded && cfg->posture_disable_kbd_in_tablet);
        write_inhibit_paths_direct(caps->touchscreen_inhibit_paths, (!is_folded) && cfg->posture_disable_touch_in_laptop);
    }

    const gchar *xdg_config = g_get_user_config_dir();
    g_autofree gchar *cfg_path = g_build_filename(xdg_config, "kajo", "config.json", NULL);
    shell_config_save(cfg, cfg_path);
    shell_config_destroy(cfg);

    settings_send_ipc_command("reload-config");

    if (btn) {
        gtk_button_set_label(btn, "[ ✓ 360° POSTURE SETTINGS APPLIED ]");
    }
}

static void
settings_page_displays_dispose(GObject *object)
{
    SettingsPageDisplays *self = (SettingsPageDisplays *)object;
    if (self->raw_modes) {
        g_ptr_array_free(self->raw_modes, TRUE);
        self->raw_modes = NULL;
    }
    gtk_widget_dispose_template(GTK_WIDGET(object), settings_page_displays_get_type());
    G_OBJECT_CLASS(settings_page_displays_parent_class)->dispose(object);
}

static void
settings_page_displays_init(SettingsPageDisplays *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
    self->raw_modes = g_ptr_array_new_with_free_func(g_free);
}

static void
settings_page_displays_class_init(SettingsPageDisplaysClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_page_displays_dispose;

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_displays.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, lbl_mon_hdr);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_res);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_orient);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_scale);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, grid_features);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_apply);

    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_ss_mode);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_ss_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_dpms_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_lock_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, sw_lock_suspend);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_preview_ss);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_lock_now);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_apply_idle);

    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, card_kbd_backlight);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_kbd_level);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, row_kbd_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_kbd_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_apply_kbd);

    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, card_posture);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, lbl_sensor_status);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, row_posture_autorotate);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, sw_posture_autorotate);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, row_posture_disable_kbd);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, sw_posture_disable_kbd);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, row_posture_disable_touch);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, sw_posture_disable_touch);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_apply_posture);
}

GtkWidget *
build_page_displays(void)
{
    SettingsPageDisplays *page = g_object_new(settings_page_displays_get_type(), NULL);

    gchar *name_str = g_strdup("eDP-1");
    gchar *make_model_str = g_strdup("Built-in Display");
    gchar *mode_str = g_strdup("Auto / Native Mode");
    gchar *scale_str = g_strdup("Scale 100% (1.00x)");
    gboolean vrr_avail = FALSE;
    gboolean vrr_active = FALSE;
    guint active_mode_idx = 0;
    double current_scale = 1.0;
    gchar *current_transform = g_strdup("Normal");

    GtkStringList *slist_res = gtk_string_list_new(NULL);

    gchar *ipc_resp = niri_send_ipc_request("\"Outputs\"");
    if (ipc_resp) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, ipc_resp, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *root_obj = json_node_get_object(root);
                JsonObject *outputs_obj = NULL;

                if (json_object_has_member(root_obj, "Ok")) {
                    JsonNode *ok_node = json_object_get_member(root_obj, "Ok");
                    if (ok_node && JSON_NODE_HOLDS_OBJECT(ok_node)) {
                        JsonObject *ok_obj = json_node_get_object(ok_node);
                        if (ok_obj && json_object_has_member(ok_obj, "Outputs")) {
                            JsonNode *out_node = json_object_get_member(ok_obj, "Outputs");
                            if (out_node && JSON_NODE_HOLDS_OBJECT(out_node)) {
                                outputs_obj = json_node_get_object(out_node);
                            }
                        }
                    }
                } else {
                    outputs_obj = root_obj;
                }

                if (outputs_obj) {
                    GList *members = json_object_get_members(outputs_obj);
                    if (members && members->data) {
                        const gchar *out_name = (const gchar *)members->data;
                        JsonNode *out_node = json_object_get_member(outputs_obj, out_name);
                        if (out_node && JSON_NODE_HOLDS_OBJECT(out_node)) {
                            JsonObject *out_info = json_node_get_object(out_node);

                            g_free(name_str);
                            name_str = g_strdup(out_name);

                            const gchar *m_make = json_object_has_member(out_info, "make") ? json_object_get_string_member(out_info, "make") : "";
                            const gchar *m_model = json_object_has_member(out_info, "model") ? json_object_get_string_member(out_info, "model") : "";
                            g_free(make_model_str);
                            make_model_str = g_strdup_printf("%s %s", m_make, m_model);

                            if (json_object_has_member(out_info, "vrr_supported")) {
                                vrr_avail = json_object_get_boolean_member(out_info, "vrr_supported");
                            }
                            if (json_object_has_member(out_info, "vrr_enabled")) {
                                vrr_active = json_object_get_boolean_member(out_info, "vrr_enabled");
                            }

                            if (json_object_has_member(out_info, "current_mode")) {
                                JsonNode *cm_node = json_object_get_member(out_info, "current_mode");
                                if (JSON_NODE_HOLDS_VALUE(cm_node)) {
                                    active_mode_idx = (guint)json_object_get_int_member(out_info, "current_mode");
                                }
                            }

                            if (json_object_has_member(out_info, "logical")) {
                                JsonNode *log_node = json_object_get_member(out_info, "logical");
                                if (log_node && JSON_NODE_HOLDS_OBJECT(log_node)) {
                                    JsonObject *log_obj = json_node_get_object(log_node);
                                    if (json_object_has_member(log_obj, "scale")) {
                                        current_scale = json_object_get_double_member(log_obj, "scale");
                                        gchar sc_buf[16];
                                        g_ascii_formatd(sc_buf, sizeof(sc_buf), "%.2f", current_scale);
                                        g_free(scale_str);
                                        scale_str = g_strdup_printf("Scale %.0f%% (%sx)", current_scale * 100.0, sc_buf);
                                    }
                                    if (json_object_has_member(log_obj, "transform")) {
                                        g_free(current_transform);
                                        current_transform = g_strdup(json_object_get_string_member(log_obj, "transform"));
                                    }
                                }
                            }

                            if (json_object_has_member(out_info, "modes")) {
                                JsonNode *modes_node = json_object_get_member(out_info, "modes");
                                if (modes_node && JSON_NODE_HOLDS_ARRAY(modes_node)) {
                                    JsonArray *modes_arr = json_node_get_array(modes_node);
                                    guint n_modes = json_array_get_length(modes_arr);

                                    for (guint i = 0; i < n_modes; i++) {
                                        JsonObject *m = json_array_get_object_element(modes_arr, i);
                                        gint64 w = json_object_has_member(m, "width") ? json_object_get_int_member(m, "width") : 0;
                                        gint64 h = json_object_has_member(m, "height") ? json_object_get_int_member(m, "height") : 0;
                                        double rr = json_object_has_member(m, "refresh_rate") ? (json_object_get_int_member(m, "refresh_rate") / 1000.0) : 60.0;
                                        gboolean pref = json_object_has_member(m, "is_preferred") ? json_object_get_boolean_member(m, "is_preferred") : FALSE;

                                        gchar rr_raw_buf[32];
                                        g_ascii_formatd(rr_raw_buf, sizeof(rr_raw_buf), "%.3f", rr);
                                        gchar *raw_mode = g_strdup_printf("%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT "@%s", w, h, rr_raw_buf);
                                        g_ptr_array_add(page->raw_modes, raw_mode);

                                        gchar rr_disp_buf[32];
                                        g_ascii_formatd(rr_disp_buf, sizeof(rr_disp_buf), "%.2f", rr);
                                        gchar *disp_mode = g_strdup_printf("%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT " @ %sHz%s",
                                                                           w, h, rr_disp_buf, pref ? " (Preferred)" : "");
                                        gtk_string_list_append(slist_res, disp_mode);

                                        if (i == active_mode_idx) {
                                            g_free(mode_str);
                                            mode_str = g_strdup_printf("%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT " @ %sHz", w, h, rr_disp_buf);
                                        }

                                        g_free(disp_mode);
                                    }
                                }
                            }
                        }
                        g_list_free(members);
                    }
                }
            }
        }
        g_free(ipc_resp);
    }

    if (page->raw_modes->len == 0) {
        gtk_string_list_append(slist_res, "Automatic / Native Preferred Mode");
        g_ptr_array_add(page->raw_modes, g_strdup("auto"));
    }

    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_res), G_LIST_MODEL(slist_res));
    g_object_unref(slist_res);
    if (active_mode_idx < page->raw_modes->len) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_res), active_mode_idx);
    }

    gchar *hdr_text = g_strdup_printf("Monitor %s — %s (%s, %s)", name_str, make_model_str, mode_str, scale_str);
    gtk_label_set_text(GTK_LABEL(page->lbl_mon_hdr), hdr_text);
    g_free(hdr_text);
    g_free(mode_str);
    g_free(scale_str);

    const char *orient_items[] = {
        "Normal (0° Horizontal)",
        "Portrait Left (90° Counter-Clockwise)",
        "Inverted (180° Upside Down)",
        "Portrait Right (270° Clockwise)",
        NULL
    };
    GtkStringList *slist_orient = gtk_string_list_new(orient_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_orient), G_LIST_MODEL(slist_orient));
    g_object_unref(slist_orient);

    guint orient_sel = 0;
    if (g_ascii_strcasecmp(current_transform, "90") == 0) orient_sel = 1;
    else if (g_ascii_strcasecmp(current_transform, "180") == 0) orient_sel = 2;
    else if (g_ascii_strcasecmp(current_transform, "270") == 0) orient_sel = 3;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_orient), orient_sel);
    g_free(current_transform);

    const char *scale_items[] = {
        "100% (1.00x Native Unscaled)",
        "125% (1.25x)",
        "150% (1.50x)",
        "175% (1.75x)",
        "200% (2.00x HiDPI)",
        NULL
    };
    GtkStringList *slist_scale = gtk_string_list_new(scale_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_scale), G_LIST_MODEL(slist_scale));
    g_object_unref(slist_scale);

    guint scale_sel = 0;
    if (current_scale >= 1.9) scale_sel = 4;
    else if (current_scale >= 1.65) scale_sel = 3;
    else if (current_scale >= 1.4) scale_sel = 2;
    else if (current_scale >= 1.15) scale_sel = 1;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_scale), scale_sel);

    GtkWidget *tile_vrr = create_metro_toggle_tile(
        "display-symbolic",
        "Variable Refresh Rate (VRR)",
        vrr_avail ? (vrr_active ? "Adaptive Sync Active" : "Adaptive Sync Disabled") : "Unsupported",
        vrr_active);
    if (!vrr_avail) {
        gtk_widget_set_sensitive(tile_vrr, FALSE);
    } else {
        GtkWidget *lbl_sub = g_object_get_data(G_OBJECT(tile_vrr), "tile-subtitle-label");
        if (lbl_sub) {
            g_signal_connect_after(tile_vrr, "clicked", G_CALLBACK(on_vrr_tile_clicked), lbl_sub);
        }
    }

    GtkWidget *tile_hdr = create_metro_toggle_tile("weather-clear-symbolic", "High Dynamic Range (HDR)", "SDR Display Panel", FALSE);
    gtk_widget_set_sensitive(tile_hdr, FALSE);

    gtk_grid_attach(GTK_GRID(page->grid_features), tile_vrr, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(page->grid_features), tile_hdr, 1, 0, 1, 1);

    g_object_set_data(G_OBJECT(page->btn_apply), "combo_res", page->combo_res);
    g_object_set_data(G_OBJECT(page->btn_apply), "combo_orient", page->combo_orient);
    g_object_set_data(G_OBJECT(page->btn_apply), "combo_scale", page->combo_scale);
    g_object_set_data(G_OBJECT(page->btn_apply), "tile_vrr", tile_vrr);
    g_object_set_data(G_OBJECT(page->btn_apply), "lbl_mon_hdr", page->lbl_mon_hdr);
    g_object_set_data(G_OBJECT(page->btn_apply), "raw_modes", page->raw_modes);
    g_object_set_data_full(G_OBJECT(page->btn_apply), "output_name", name_str, g_free);
    g_object_set_data_full(G_OBJECT(page->btn_apply), "make_model", make_model_str, g_free);

    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_apply_displays_clicked), NULL);

    /* Populate Screen Saver & Session Lock controls from current config */
    ShellConfig *cfg = shell_config_load(NULL);

    const char *ss_mode_items[] = {
        "Metro Clock Overlay",
        "Turn Off Monitors (DPMS)",
        "Disabled",
        NULL
    };
    GtkStringList *slist_ss_mode = gtk_string_list_new(ss_mode_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_ss_mode), G_LIST_MODEL(slist_ss_mode));
    g_object_unref(slist_ss_mode);
    if (cfg && cfg->screensaver_mode) {
        if (g_strcmp0(cfg->screensaver_mode, "dpms") == 0)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_ss_mode), 1);
        else if (g_strcmp0(cfg->screensaver_mode, "disabled") == 0)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_ss_mode), 2);
        else
            gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_ss_mode), 0);
    }

    const char *ss_timeout_items[] = {
        "1 Minute",
        "5 Minutes (Default)",
        "10 Minutes",
        "15 Minutes",
        "30 Minutes",
        "Never (Disabled)",
        NULL
    };
    GtkStringList *slist_ss_timeout = gtk_string_list_new(ss_timeout_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_ss_timeout), G_LIST_MODEL(slist_ss_timeout));
    g_object_unref(slist_ss_timeout);
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(page->combo_ss_timeout),
        timeout_sec_to_index(cfg ? cfg->screensaver_timeout_sec : 300,
                             ss_timeout_values,
                             G_N_ELEMENTS(ss_timeout_values),
                             1));

    const char *dpms_timeout_items[] = {
        "5 Minutes",
        "10 Minutes (Default)",
        "15 Minutes",
        "30 Minutes",
        "Never (Disabled)",
        NULL
    };
    GtkStringList *slist_dpms_timeout = gtk_string_list_new(dpms_timeout_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_dpms_timeout), G_LIST_MODEL(slist_dpms_timeout));
    g_object_unref(slist_dpms_timeout);
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(page->combo_dpms_timeout),
        timeout_sec_to_index(cfg ? cfg->dpms_timeout_sec : 600,
                             dpms_timeout_values,
                             G_N_ELEMENTS(dpms_timeout_values),
                             1));

    const char *lock_timeout_items[] = {
        "5 Minutes",
        "10 Minutes",
        "15 Minutes (Default)",
        "30 Minutes",
        "Never (Disabled)",
        NULL
    };
    GtkStringList *slist_lock_timeout = gtk_string_list_new(lock_timeout_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_lock_timeout), G_LIST_MODEL(slist_lock_timeout));
    g_object_unref(slist_lock_timeout);
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(page->combo_lock_timeout),
        timeout_sec_to_index(cfg ? cfg->lock_timeout_sec : 900,
                             lock_timeout_values,
                             G_N_ELEMENTS(lock_timeout_values),
                             2));

    gtk_switch_set_active(GTK_SWITCH(page->sw_lock_suspend),
                          cfg ? cfg->lock_on_suspend : TRUE);

    /* Hardware-gated Keyboard Backlight & Timeout Card */
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (caps && caps->kbd_bl_backend != KBD_BL_NONE) {
        gtk_widget_set_visible(page->card_kbd_backlight, TRUE);

        if (caps->kbd_bl_backend == KBD_BL_HP_EC) {
            const char *hp_kbd_items[] = {
                "Wake / Enable Keylights (F4 cycles Low / High)",
                "Do Not Auto-Wake Keylights on Startup",
                NULL
            };
            GtkStringList *slist_kbd_lvl = gtk_string_list_new(hp_kbd_items);
            gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_kbd_level), G_LIST_MODEL(slist_kbd_lvl));
            g_object_unref(slist_kbd_lvl);

            gint saved_lvl = cfg ? cfg->kbd_backlight_level : 2;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_kbd_level), (saved_lvl > 0) ? 0 : 1);
        } else {
            const char *kbd_level_items[] = {
                "Off (0%)",
                "Low (50%)",
                "High (100%)",
                NULL
            };
            GtkStringList *slist_kbd_lvl = gtk_string_list_new(kbd_level_items);
            gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_kbd_level), G_LIST_MODEL(slist_kbd_lvl));
            g_object_unref(slist_kbd_lvl);

            gint live_lvl = shell_kbd_backlight_get_level();
            if (live_lvl < 0)
                live_lvl = cfg ? cfg->kbd_backlight_level : 2;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_kbd_level), (guint)CLAMP(live_lvl, 0, 2));
        }

        gtk_widget_set_visible(page->row_kbd_timeout, caps->has_kbd_timeout);
        if (caps->has_kbd_timeout) {
            const char *kbd_timeout_items[] = {
                "Always On (Never Timeout)",
                "30 Seconds",
                "3 Minutes",
                NULL
            };
            GtkStringList *slist_kbd_t = gtk_string_list_new(kbd_timeout_items);
            gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_kbd_timeout), G_LIST_MODEL(slist_kbd_t));
            g_object_unref(slist_kbd_t);

            gint live_t = shell_kbd_backlight_get_timeout_sec();
            if (live_t < 0)
                live_t = cfg ? cfg->kbd_backlight_timeout_sec : 0;
            gtk_drop_down_set_selected(
                GTK_DROP_DOWN(page->combo_kbd_timeout),
                timeout_sec_to_index(live_t, kbd_timeout_values, G_N_ELEMENTS(kbd_timeout_values), 0));
        }

        g_signal_connect(page->btn_apply_kbd, "clicked", G_CALLBACK(on_apply_kbd_clicked), page);
    }

    /* Hardware-gated 360° Foldable Posture Card */
    if (caps && (caps->has_hinge_sensor || caps->has_accel_sensor)) {
        gtk_widget_set_visible(page->card_posture, TRUE);

        gdouble hinge_deg = 0.0;
        if (shell_hw_caps_read_hinge_angle_deg(caps, &hinge_deg)) {
            const gchar *p_mode = (hinge_deg >= 180.0) ? "Tablet / Tent Mode" : "Laptop Mode";
            g_autofree gchar *status_txt = g_strdup_printf(
                "Intel ISH Hinge Sensor Active (Current Angle: %.0f° — %s)",
                hinge_deg, p_mode);
            gtk_label_set_text(GTK_LABEL(page->lbl_sensor_status), status_txt);
        }

        gtk_widget_set_visible(page->row_posture_autorotate, caps->has_accel_sensor);
        gtk_widget_set_visible(page->row_posture_disable_kbd, caps->has_internal_kbd || caps->has_touchpad);
        gtk_widget_set_visible(page->row_posture_disable_touch, caps->has_touchscreen);

        gtk_switch_set_active(GTK_SWITCH(page->sw_posture_autorotate),
                              cfg ? cfg->posture_auto_rotate : TRUE);
        gtk_switch_set_active(GTK_SWITCH(page->sw_posture_disable_kbd),
                              cfg ? cfg->posture_disable_kbd_in_tablet : TRUE);
        gtk_switch_set_active(GTK_SWITCH(page->sw_posture_disable_touch),
                              cfg ? cfg->posture_disable_touch_in_laptop : FALSE);

        g_signal_connect(page->btn_apply_posture, "clicked", G_CALLBACK(on_apply_posture_clicked), page);
    }

    if (cfg)
        shell_config_destroy(cfg);

    g_signal_connect(page->btn_preview_ss, "clicked", G_CALLBACK(on_preview_screensaver_clicked), page);
    g_signal_connect(page->btn_lock_now, "clicked", G_CALLBACK(on_lock_now_clicked), page);
    g_signal_connect(page->btn_apply_idle, "clicked", G_CALLBACK(on_apply_idle_clicked), page);

    return create_settings_page_card("DISPLAY OUTPUTS & RESOLUTION", "[ NIRI IPC ACTIVE ]", GTK_WIDGET(page), NULL);
}
