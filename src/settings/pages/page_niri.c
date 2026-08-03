#include "../settings_common.h"
#include "../settings_pages.h"

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageNiri {
    GtkBox parent_instance;

    GtkWidget *combo_ns;
    GtkWidget *scale_op;
    GtkWidget *lbl_op_val;
    GtkWidget *scale_gap;
    GtkWidget *lbl_gap_val;
    GtkWidget *combo_xray;
    GtkWidget *btn_apply;
} SettingsPageNiri;

typedef struct _SettingsPageNiriClass {
    GtkBoxClass parent_class;
} SettingsPageNiriClass;

G_DEFINE_TYPE(SettingsPageNiri, settings_page_niri, GTK_TYPE_BOX)

static void
settings_page_niri_init(SettingsPageNiri *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_niri_class_init(SettingsPageNiriClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_niri.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageNiri, combo_ns);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNiri, scale_op);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNiri, lbl_op_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNiri, scale_gap);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNiri, lbl_gap_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNiri, combo_xray);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNiri, btn_apply);
}

static void
on_scale_op_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%d%%", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_scale_gap_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%dpx", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_reload_niri_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    GtkRange *scale_op = g_object_get_data(G_OBJECT(btn), "scale_op");
    GtkRange *scale_gap = g_object_get_data(G_OBJECT(btn), "scale_gap");
    GtkDropDown *combo_xray = g_object_get_data(G_OBJECT(btn), "combo_xray");

    double op = scale_op ? (gtk_range_get_value(scale_op) / 100.0) : 0.85;
    int gap = scale_gap ? (int)gtk_range_get_value(scale_gap) : 12;
    gboolean xray = combo_xray ? (gtk_drop_down_get_selected(combo_xray) == 1) : FALSE;

    const gchar *xdg_config = g_get_user_config_dir();
    gchar *dir = g_build_filename(xdg_config, "niri", "config.d", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "00-kajo.kdl", NULL);

    gchar *kdl_content = g_strdup_printf(
        "// Managed by kajo-settings — DO NOT EDIT DIRECTLY\n\n"
        "layer-rule {\n"
        "    match namespace=\"kajo\"\n"
        "    opacity %.2f\n"
        "    background-effect {\n"
        "        blur true\n"
        "        xray %s\n"
        "    }\n"
        "}\n\n"
        "layout {\n"
        "    gaps %d\n"
        "    focus-ring {\n"
        "        width 2\n"
        "        active-color \"#0078d4\"\n"
        "    }\n"
        "}\n\n"
        "window-rule {\n"
        "    match app-id=\"org.kajo.Settings\"\n"
        "    open-floating true\n"
        "}\n",
        op, xray ? "true" : "false", gap);

    g_file_set_contents(path, kdl_content, -1, NULL);
    g_message("Saved modular Niri layer-rules to %s", path);
    g_free(kdl_content);
    g_free(dir);
    g_free(path);

    niri_send_action("{\"Action\":\"ReloadConfig\"}");
}

GtkWidget *
build_page_niri(void)
{
    SettingsPageNiri *page = g_object_new(settings_page_niri_get_type(), NULL);

    const char *ns_items[] = {
        "kajo (Panel & Popovers)",
        "kajo-launcher (App Launcher)",
        "kajo-notification (Toasts)",
        NULL
    };
    GtkStringList *slist_ns = gtk_string_list_new(ns_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_ns), G_LIST_MODEL(slist_ns));

    const char *xray_items[] = {
        "Disabled (Default Solid Blur)",
        "Enabled (Pass-through Wallpaper)",
        NULL
    };
    GtkStringList *slist_xray = gtk_string_list_new(xray_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_xray), G_LIST_MODEL(slist_xray));

    gtk_range_set_value(GTK_RANGE(page->scale_op), 85);
    g_signal_connect(page->scale_op, "value-changed", G_CALLBACK(on_scale_op_changed), page->lbl_op_val);

    gtk_range_set_value(GTK_RANGE(page->scale_gap), 12);
    g_signal_connect(page->scale_gap, "value-changed", G_CALLBACK(on_scale_gap_changed), page->lbl_gap_val);

    g_object_set_data(G_OBJECT(page->btn_apply), "scale_op", page->scale_op);
    g_object_set_data(G_OBJECT(page->btn_apply), "scale_gap", page->scale_gap);
    g_object_set_data(G_OBJECT(page->btn_apply), "combo_xray", page->combo_xray);
    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_reload_niri_clicked), NULL);

    return create_settings_page_card("NIRI COMPOSITOR RULES & GAPS", "[ KDL IPC ]", GTK_WIDGET(page), NULL);
}
