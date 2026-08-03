#include "../settings_common.h"
#include "../settings_pages.h"
#include <json-glib/json-glib.h>

static void
on_apply_displays_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    GtkDropDown *combo_res = g_object_get_data(G_OBJECT(btn), "combo_res");
    GtkDropDown *combo_orient = g_object_get_data(G_OBJECT(btn), "combo_orient");
    GtkDropDown *combo_scale = g_object_get_data(G_OBJECT(btn), "combo_scale");
    GtkWidget *tile_vrr = g_object_get_data(G_OBJECT(btn), "tile_vrr");

    const char *res = "2880x1800@120.001";
    if (combo_res) {
        guint sel = gtk_drop_down_get_selected(combo_res);
        if (sel == 1) res = "2880x1800@48.00";
        else if (sel == 2) res = "1920x1200@120.00";
        else if (sel == 3) res = "1920x1080@60.00";
    }

    const char *orient = "normal";
    if (combo_orient) {
        guint sel = gtk_drop_down_get_selected(combo_orient);
        if (sel == 1) orient = "90";
        else if (sel == 2) orient = "180";
        else if (sel == 3) orient = "270";
    }

    double sc = 1.75;
    if (combo_scale) {
        guint sel = gtk_drop_down_get_selected(combo_scale);
        if (sel == 1) sc = 1.50;
        else if (sel == 2) sc = 1.25;
        else if (sel == 3) sc = 1.00;
        else if (sel == 4) sc = 2.00;
    }

    gboolean vrr = tile_vrr ? gtk_widget_has_css_class(tile_vrr, "active") : TRUE;

    const gchar *xdg_config = g_get_user_config_dir();
    gchar *dir = g_build_filename(xdg_config, "niri", "config.d", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "10-outputs.kdl", NULL);

    gchar *kdl_content = g_strdup_printf(
        "// Managed by kajo-settings — DO NOT EDIT DIRECTLY\n\n"
        "output \"eDP-1\" {\n"
        "    mode \"%s\"\n"
        "    scale %.2f\n"
        "    transform \"%s\"\n"
        "    position x=0 y=0\n"
        "    vrr %s\n"
        "}\n",
        res, sc, orient, vrr ? "true" : "false");

    g_file_set_contents(path, kdl_content, -1, NULL);
    g_message("Saved modular Niri outputs configuration to %s", path);
    g_free(kdl_content);
    g_free(dir);
    g_free(path);

    niri_send_action("{\"Action\":\"ReloadConfig\"}");
}/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageDisplays {
    GtkBox parent_instance;

    GtkWidget *lbl_mon_hdr;
    GtkWidget *combo_res;
    GtkWidget *combo_orient;
    GtkWidget *combo_scale;
    GtkWidget *grid_features;
    GtkWidget *btn_apply;
} SettingsPageDisplays;

typedef struct _SettingsPageDisplaysClass {
    GtkBoxClass parent_class;
} SettingsPageDisplaysClass;

G_DEFINE_TYPE(SettingsPageDisplays, settings_page_displays, GTK_TYPE_BOX)

static void
settings_page_displays_init(SettingsPageDisplays *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_displays_class_init(SettingsPageDisplaysClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

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

    /* Query real connected monitors from Niri IPC socket */
    gchar *name_str = g_strdup("eDP-1");
    gchar *make_model_str = g_strdup("Samsung Display Corp. (0x41AA)");
    gchar *mode_str = g_strdup("2880x1800 @ 120.00Hz");
    gchar *scale_str = g_strdup("Scale 175% (1.75x)");
    gboolean vrr_avail = TRUE;

    gchar *ipc_resp = niri_send_ipc_request("\"Outputs\"");
    if (ipc_resp) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, ipc_resp, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *obj = json_node_get_object(root);
                if (obj && json_object_has_member(obj, "Ok")) {
                    JsonNode *ok_node = json_object_get_member(obj, "Ok");
                    if (ok_node && JSON_NODE_HOLDS_OBJECT(ok_node)) {
                        JsonObject *ok_obj = json_node_get_object(ok_node);
                        if (ok_obj && json_object_has_member(ok_obj, "Outputs")) {
                            JsonNode *outputs_node = json_object_get_member(ok_obj, "Outputs");
                            if (outputs_node && JSON_NODE_HOLDS_OBJECT(outputs_node)) {
                                JsonObject *outputs_obj = json_node_get_object(outputs_node);
                                GList *members = outputs_obj ? json_object_get_members(outputs_obj) : NULL;
                                if (members) {
                                    if (members->data) {
                                        const gchar *out_name = (const gchar *)members->data;
                                        JsonNode *out_node = json_object_get_member(outputs_obj, out_name);
                                        if (out_node && JSON_NODE_HOLDS_OBJECT(out_node)) {
                                            JsonObject *out_info = json_node_get_object(out_node);

                                            g_free(name_str);
                                            name_str = g_strdup(out_name);

                                            const gchar *m_make = (out_info && json_object_has_member(out_info, "make")) ? json_object_get_string_member(out_info, "make") : "";
                                            const gchar *m_model = (out_info && json_object_has_member(out_info, "model")) ? json_object_get_string_member(out_info, "model") : "";
                                            g_free(make_model_str);
                                            make_model_str = g_strdup_printf("%s %s", m_make, m_model);

                                            if (out_info && json_object_has_member(out_info, "current_mode")) {
                                                JsonNode *cmode_node = json_object_get_member(out_info, "current_mode");
                                                if (cmode_node && JSON_NODE_HOLDS_OBJECT(cmode_node)) {
                                                    JsonObject *cmode = json_node_get_object(cmode_node);
                                                    gint64 w = (cmode && json_object_has_member(cmode, "width")) ? json_object_get_int_member(cmode, "width") : 0;
                                                    gint64 h = (cmode && json_object_has_member(cmode, "height")) ? json_object_get_int_member(cmode, "height") : 0;
                                                    double rr = (cmode && json_object_has_member(cmode, "refresh_rate")) ? (json_object_get_int_member(cmode, "refresh_rate") / 1000.0) : 0.0;
                                                    g_free(mode_str);
                                                    mode_str = g_strdup_printf("%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT " @ %.2fHz", w, h, rr);
                                                }
                                            }

                                            if (out_info && json_object_has_member(out_info, "scale")) {
                                                double sc = json_object_get_double_member(out_info, "scale");
                                                g_free(scale_str);
                                                scale_str = g_strdup_printf("Scale %.0f%% (%.2fx)", sc * 100.0, sc);
                                            }
                                        }
                                    }
                                    g_list_free(members);
                                }
                            }
                        }
                    }
                }
            }
        }
        g_free(ipc_resp);
    }

    gchar *hdr_text = g_strdup_printf("Monitor %s — %s (%s, %s)", name_str, make_model_str, mode_str, scale_str);
    gtk_label_set_text(GTK_LABEL(page->lbl_mon_hdr), hdr_text);
    g_free(hdr_text);
    g_free(name_str);
    g_free(make_model_str);
    g_free(mode_str);
    g_free(scale_str);

    const char *res_items[] = {
        "2880x1800 @ 120.00Hz (Native Preferred)",
        "2880x1800 @ 48.00Hz (Battery Saver)",
        "1920x1200 @ 120.00Hz",
        "1920x1080 @ 60.00Hz",
        NULL
    };
    GtkStringList *slist_res = gtk_string_list_new(res_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_res), G_LIST_MODEL(slist_res));

    const char *orient_items[] = {
        "Normal (0° Horizontal)",
        "Portrait Left (90° Counter-Clockwise)",
        "Inverted (180° Upside Down)",
        "Portrait Right (270° Clockwise)",
        NULL
    };
    GtkStringList *slist_orient = gtk_string_list_new(orient_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_orient), G_LIST_MODEL(slist_orient));

    const char *scale_items[] = {
        "175% (1.75x Recommended)",
        "150% (1.50x)",
        "125% (1.25x)",
        "100% (1.00x Native Unscaled)",
        "200% (2.00x HiDPI)",
        NULL
    };
    GtkStringList *slist_scale = gtk_string_list_new(scale_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_scale), G_LIST_MODEL(slist_scale));

    GtkWidget *tile_vrr = create_metro_toggle_tile("display-symbolic", "Variable Refresh Rate (VRR)", vrr_avail ? "Adaptive Sync Active" : "Unsupported", vrr_avail);
    GtkWidget *tile_hdr = create_metro_toggle_tile("weather-clear-symbolic", "High Dynamic Range (HDR)", "SDR Display Panel", FALSE);

    gtk_grid_attach(GTK_GRID(page->grid_features), tile_vrr, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(page->grid_features), tile_hdr, 1, 0, 1, 1);

    g_object_set_data(G_OBJECT(page->btn_apply), "combo_res", page->combo_res);
    g_object_set_data(G_OBJECT(page->btn_apply), "combo_orient", page->combo_orient);
    g_object_set_data(G_OBJECT(page->btn_apply), "combo_scale", page->combo_scale);
    g_object_set_data(G_OBJECT(page->btn_apply), "tile_vrr", tile_vrr);
    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_apply_displays_clicked), NULL);

    return create_settings_page_card("DISPLAY OUTPUTS & RESOLUTION", "[ NIRI IPC ACTIVE ]", GTK_WIDGET(page), NULL);
}
