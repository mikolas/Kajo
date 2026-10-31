#include "../settings_common.h"
#include "../settings_pages.h"
#include <json-glib/json-glib.h>
#include <string.h>

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
} SettingsPageDisplays;

typedef struct _SettingsPageDisplaysClass {
    GtkBoxClass parent_class;
} SettingsPageDisplaysClass;

G_DEFINE_TYPE(SettingsPageDisplays, settings_page_displays, GTK_TYPE_BOX)

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
        GtkWidget *hbox = gtk_button_get_child(GTK_BUTTON(tile_vrr));
        GtkWidget *vbox = hbox ? gtk_widget_get_last_child(hbox) : NULL;
        GtkWidget *lbl_sub = vbox ? gtk_widget_get_last_child(vbox) : NULL;
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

    return create_settings_page_card("DISPLAY OUTPUTS & RESOLUTION", "[ NIRI IPC ACTIVE ]", GTK_WIDGET(page), NULL);
}
