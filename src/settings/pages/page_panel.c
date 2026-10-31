#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

static void
notify_shell_ipc(const gchar *json_cmd)
{
    const gchar *runtime_dir = g_get_user_runtime_dir();
    if (!runtime_dir)
        return;

    g_autofree gchar *sock_path = g_build_filename(runtime_dir, "kajo.sock", NULL);
    if (!g_file_test(sock_path, G_FILE_TEST_EXISTS))
        return;

    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(sock_path);
    g_autoptr(GSocketConnection) conn = g_socket_client_connect(
        client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    if (!conn)
        return;

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_autofree gchar *line = g_strdup_printf("%s\n", json_cmd);
    g_output_stream_write_all(out, line, strlen(line), NULL, NULL, NULL);
}

static void
update_tile_subtitle(GtkWidget *tile, const gchar *subtitle)
{
    if (!tile || !subtitle)
        return;
    GtkWidget *hbox = gtk_button_get_child(GTK_BUTTON(tile));
    if (!hbox)
        return;
    GtkWidget *vbox = gtk_widget_get_last_child(hbox);
    if (!vbox)
        return;
    GtkWidget *sub_lbl = gtk_widget_get_last_child(vbox);
    if (sub_lbl && GTK_IS_LABEL(sub_lbl)) {
        gtk_label_set_text(GTK_LABEL(sub_lbl), subtitle);
    }
}

static void
on_tile_hide_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    gboolean active = gtk_widget_has_css_class(GTK_WIDGET(btn), "active");
    update_tile_subtitle(GTK_WIDGET(btn), active ? "Enabled" : "Disabled");
}

static void
on_tile_edge_clicked(GtkButton *btn, gpointer user_data)
{
    GtkDropDown *combo_pos = GTK_DROP_DOWN(user_data);
    gboolean bottom = gtk_widget_has_css_class(GTK_WIDGET(btn), "active");
    update_tile_subtitle(GTK_WIDGET(btn), bottom ? "Bottom Position" : "Top Position");
    if (combo_pos && gtk_drop_down_get_selected(combo_pos) != (bottom ? 1u : 0u)) {
        gtk_drop_down_set_selected(combo_pos, bottom ? 1u : 0u);
    }
}

static void
on_combo_pos_changed(GtkDropDown *combo, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *tile_edge = GTK_WIDGET(user_data);
    if (!tile_edge)
        return;
    gboolean bottom = (gtk_drop_down_get_selected(combo) == 1);
    if (bottom)
        gtk_widget_add_css_class(tile_edge, "active");
    else
        gtk_widget_remove_css_class(tile_edge, "active");
    update_tile_subtitle(tile_edge, bottom ? "Bottom Position" : "Top Position");
}

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPagePanel {
    GtkBox parent_instance;

    GtkWidget *combo_pos;
    GtkWidget *toggle_grid;
    GtkWidget *scale_height;
    GtkWidget *lbl_height_val;
    GtkWidget *scale_rev;
    GtkWidget *lbl_rev_val;
    GtkWidget *btn_apply;
} SettingsPagePanel;

typedef struct _SettingsPagePanelClass {
    GtkBoxClass parent_class;
} SettingsPagePanelClass;

G_DEFINE_TYPE(SettingsPagePanel, settings_page_panel, GTK_TYPE_BOX)

static void
settings_page_panel_init(SettingsPagePanel *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_panel_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), settings_page_panel_get_type());
    G_OBJECT_CLASS(settings_page_panel_parent_class)->dispose(object);
}

static void
settings_page_panel_class_init(SettingsPagePanelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_page_panel_dispose;

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_panel.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, combo_pos);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, toggle_grid);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, scale_height);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, lbl_height_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, scale_rev);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, lbl_rev_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, btn_apply);
}

static void
on_scale_height_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%dpx", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_scale_rev_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%dms", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_apply_panel_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    ShellConfig *config = shell_config_load(NULL);
    if (!config) return;

    GtkDropDown *combo_pos = g_object_get_data(G_OBJECT(btn), "combo_pos");
    GtkRange *scale_height = g_object_get_data(G_OBJECT(btn), "scale_height");
    GtkRange *scale_rev = g_object_get_data(G_OBJECT(btn), "scale_rev");
    GtkWidget *tile_hide = g_object_get_data(G_OBJECT(btn), "tile_hide");

    if (combo_pos) config->panel_position = (gtk_drop_down_get_selected(combo_pos) == 1) ? PANEL_POSITION_BOTTOM : PANEL_POSITION_TOP;
    if (scale_height) config->panel_height = (int)gtk_range_get_value(scale_height);
    if (scale_rev) {
        config->autohide_reveal_delay = (int)gtk_range_get_value(scale_rev);
        config->autohide_reveal_duration = (int)gtk_range_get_value(scale_rev);
    }
    if (tile_hide) config->autohide_enabled = gtk_widget_has_css_class(tile_hide, "active");

    const gchar *xdg_config = g_get_user_config_dir();
    gchar *dir = g_build_filename(xdg_config, "kajo", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "config.json", NULL);

    if (shell_config_save(config, path)) {
        g_message("Saved panel configuration to %s", path);
    }
    shell_config_destroy(config);
    g_free(dir);
    g_free(path);

    notify_shell_ipc("{\"command\":\"reload-config\"}");
    niri_send_action("{\"Action\":\"ReloadConfig\"}");
}

GtkWidget *
build_page_panel(void)
{
    SettingsPagePanel *page = g_object_new(settings_page_panel_get_type(), NULL);
    ShellConfig *config = shell_config_load(NULL);
    if (!config) return GTK_WIDGET(page);

    const char *pos_items[] = { "Top Edge (Default)", "Bottom Edge", NULL };
    GtkStringList *slist = gtk_string_list_new(pos_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_pos), G_LIST_MODEL(slist));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_pos), config->panel_position == PANEL_POSITION_BOTTOM ? 1 : 0);

    /* Quick Toggle Tiles Grid */
    GtkWidget *tile_edge = create_metro_toggle_tile("tb-adjustments-horizontal-symbolic", "Panel Edge", config->panel_position == PANEL_POSITION_BOTTOM ? "Bottom Position" : "Top Position", config->panel_position == PANEL_POSITION_BOTTOM);
    GtkWidget *tile_hide = create_metro_toggle_tile("view-reveal-symbolic", "Autohide", config->autohide_enabled ? "Enabled" : "Disabled", config->autohide_enabled);
    GtkWidget *tile_shadow = create_metro_toggle_tile("layer-visible-symbolic", "Panel Blur", "Active (rgba 82%)", TRUE);
    GtkWidget *tile_margin = create_metro_toggle_tile("zoom-fit-best-symbolic", "Outer Margins", "0px Full Width", FALSE);

    g_signal_connect_after(tile_edge, "clicked", G_CALLBACK(on_tile_edge_clicked), page->combo_pos);
    g_signal_connect_after(tile_hide, "clicked", G_CALLBACK(on_tile_hide_clicked), NULL);
    g_signal_connect(page->combo_pos, "notify::selected", G_CALLBACK(on_combo_pos_changed), tile_edge);

    gtk_grid_attach(GTK_GRID(page->toggle_grid), tile_edge, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(page->toggle_grid), tile_hide, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(page->toggle_grid), tile_shadow, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(page->toggle_grid), tile_margin, 1, 1, 1, 1);

    gtk_range_set_value(GTK_RANGE(page->scale_height), config->panel_height > 0 ? config->panel_height : 32);
    gchar *h_str = g_strdup_printf("%dpx", config->panel_height > 0 ? config->panel_height : 32);
    gtk_label_set_text(GTK_LABEL(page->lbl_height_val), h_str);
    g_free(h_str);

    g_signal_connect(page->scale_height, "value-changed", G_CALLBACK(on_scale_height_changed), page->lbl_height_val);

    gtk_range_set_value(GTK_RANGE(page->scale_rev), config->autohide_reveal_delay > 0 ? config->autohide_reveal_delay : 150);
    gchar *r_str = g_strdup_printf("%dms", config->autohide_reveal_delay > 0 ? config->autohide_reveal_delay : 150);
    gtk_label_set_text(GTK_LABEL(page->lbl_rev_val), r_str);
    g_free(r_str);

    g_signal_connect(page->scale_rev, "value-changed", G_CALLBACK(on_scale_rev_changed), page->lbl_rev_val);

    shell_config_destroy(config);

    g_object_set_data(G_OBJECT(page->btn_apply), "combo_pos", page->combo_pos);
    g_object_set_data(G_OBJECT(page->btn_apply), "scale_height", page->scale_height);
    g_object_set_data(G_OBJECT(page->btn_apply), "scale_rev", page->scale_rev);
    g_object_set_data(G_OBJECT(page->btn_apply), "tile_hide", tile_hide);
    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_apply_panel_clicked), NULL);

    return create_settings_page_card("PANEL GEOMETRY & AUTOHIDE", "[ LIVE GEOMETRY ]", GTK_WIDGET(page), NULL);
}
