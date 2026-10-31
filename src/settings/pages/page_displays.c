#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>

static gboolean
send_kajo_ipc_command(const gchar *command)
{
    if (!command)
        return FALSE;

    const gchar *runtime_dir = g_getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir)
        runtime_dir = "/tmp";

    g_autofree gchar *path = g_build_filename(runtime_dir, "kajo.sock", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return FALSE;

    GError *error = NULL;
    GSocket *socket = g_socket_new(G_SOCKET_FAMILY_UNIX,
                                   G_SOCKET_TYPE_STREAM,
                                   G_SOCKET_PROTOCOL_DEFAULT,
                                   &error);
    if (!socket) {
        if (error)
            g_clear_error(&error);
        return FALSE;
    }

    GSocketAddress *address = g_unix_socket_address_new(path);
    gboolean ok = FALSE;
    if (g_socket_connect(socket, address, NULL, NULL)) {
        GSocketConnection *conn = g_socket_connection_factory_create_connection(socket);
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        g_autofree gchar *msg = g_strconcat(command, "\n", NULL);
        ok = g_output_stream_write_all(out, msg, strlen(msg), NULL, NULL, NULL);
        g_object_unref(conn);
    }

    g_object_unref(address);
    g_object_unref(socket);
    return ok;
}

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

    GtkWidget *combo_ss_mode;
    GtkWidget *combo_ss_timeout;
    GtkWidget *combo_dpms_timeout;
    GtkWidget *combo_lock_timeout;
    GtkWidget *sw_lock_suspend;
    GtkWidget *btn_preview_ss;
    GtkWidget *btn_lock_now;
    GtkWidget *btn_apply_idle;
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
    send_kajo_ipc_command("screensaver");
}

static void
on_lock_now_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    send_kajo_ipc_command("lock");
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

    send_kajo_ipc_command("reload-config");

    if (btn) {
        gtk_button_set_label(btn, "[ ✓ SAVED & APPLIED TO SHELL ]");
    }
}

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

    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_ss_mode);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_ss_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_dpms_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, combo_lock_timeout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, sw_lock_suspend);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_preview_ss);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_lock_now);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageDisplays, btn_apply_idle);
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
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(page->combo_lock_timeout),
        timeout_sec_to_index(cfg ? cfg->lock_timeout_sec : 900,
                             lock_timeout_values,
                             G_N_ELEMENTS(lock_timeout_values),
                             2));

    gtk_switch_set_active(GTK_SWITCH(page->sw_lock_suspend),
                          cfg ? cfg->lock_on_suspend : TRUE);

    if (cfg)
        shell_config_destroy(cfg);

    g_signal_connect(page->btn_preview_ss, "clicked", G_CALLBACK(on_preview_screensaver_clicked), page);
    g_signal_connect(page->btn_lock_now, "clicked", G_CALLBACK(on_lock_now_clicked), page);
    g_signal_connect(page->btn_apply_idle, "clicked", G_CALLBACK(on_apply_idle_clicked), page);

    return create_settings_page_card("DISPLAY OUTPUTS & RESOLUTION", "[ NIRI IPC ACTIVE ]", GTK_WIDGET(page), NULL);
}
