#include "../settings_common.h"
#include "../settings_pages.h"
#include <string.h>

#define N_NIRI_NAMESPACES 3

static const char *NIRI_NS_IDS[N_NIRI_NAMESPACES] = {
    "kajo",
    "kajo-launcher",
    "kajo-notification"
};

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

    int ns_opacity[N_NIRI_NAMESPACES];
    gboolean ns_xray[N_NIRI_NAMESPACES];
    gboolean syncing_ui;
} SettingsPageNiri;

typedef struct _SettingsPageNiriClass {
    GtkBoxClass parent_class;
} SettingsPageNiriClass;

G_DEFINE_TYPE(SettingsPageNiri, settings_page_niri, GTK_TYPE_BOX)

static void
settings_page_niri_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), settings_page_niri_get_type());
    G_OBJECT_CLASS(settings_page_niri_parent_class)->dispose(object);
}

static void
settings_page_niri_init(SettingsPageNiri *self)
{
    for (int i = 0; i < N_NIRI_NAMESPACES; i++) {
        self->ns_opacity[i] = 85;
        self->ns_xray[i] = FALSE;
    }
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_niri_class_init(SettingsPageNiriClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_page_niri_dispose;

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
load_existing_layer_rules(SettingsPageNiri *self)
{
    g_autofree gchar *path = g_build_filename(
        g_get_user_config_dir(), "niri", "config.d", "40-layer-rules.kdl", NULL);
    g_autofree gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL) || !contents) {
        return;
    }

    for (int i = 0; i < N_NIRI_NAMESPACES; i++) {
        g_autofree gchar *needle = g_strdup_printf("namespace=\"%s\"", NIRI_NS_IDS[i]);
        const char *pos = strstr(contents, needle);
        if (!pos) continue;

        const char *block_end = strstr(pos, "\n}");
        const char *op_pos = strstr(pos, "opacity ");
        if (op_pos && (!block_end || op_pos < block_end)) {
            double val = g_ascii_strtod(op_pos + 8, NULL);
            if (val > 0.0 && val <= 1.0) {
                self->ns_opacity[i] = (int)(val * 100.0 + 0.5);
            }
        }
        const char *xray_pos = strstr(pos, "xray ");
        if (xray_pos && (!block_end || xray_pos < block_end)) {
            self->ns_xray[i] = g_str_has_prefix(xray_pos + 5, "true");
        }
    }
}

static void
on_combo_ns_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx >= N_NIRI_NAMESPACES) idx = 0;

    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_op), self->ns_opacity[idx]);
    g_autofree gchar *str = g_strdup_printf("%d%%", self->ns_opacity[idx]);
    gtk_label_set_text(GTK_LABEL(self->lbl_op_val), str);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_xray), self->ns_xray[idx] ? 1 : 0);
    self->syncing_ui = FALSE;
}

static void
on_scale_op_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    int val = (int)gtk_range_get_value(range);
    g_autofree gchar *str = g_strdup_printf("%d%%", val);
    gtk_label_set_text(GTK_LABEL(self->lbl_op_val), str);

    if (!self->syncing_ui) {
        guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
        if (idx < N_NIRI_NAMESPACES) {
            self->ns_opacity[idx] = val;
        }
    }
}

static void
on_combo_xray_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui) return;

    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
    if (idx < N_NIRI_NAMESPACES) {
        self->ns_xray[idx] = (gtk_drop_down_get_selected(dropdown) == 1);
    }
}

static void
on_scale_gap_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    g_autofree gchar *str = g_strdup_printf("%dpx", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
}

static void
on_reload_niri_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;

    guint active_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
    if (active_idx >= N_NIRI_NAMESPACES) active_idx = 0;

    self->ns_opacity[active_idx] = (int)gtk_range_get_value(GTK_RANGE(self->scale_op));
    self->ns_xray[active_idx] = (gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_xray)) == 1);
    int gap = (int)gtk_range_get_value(GTK_RANGE(self->scale_gap));

    const gchar *xdg_config = g_get_user_config_dir();
    g_autofree gchar *dir = g_build_filename(xdg_config, "niri", "config.d", NULL);
    g_mkdir_with_parents(dir, 0755);

    /* 1. Write 40-layer-rules.kdl (included by master config.kdl) */
    g_autofree gchar *layer_path = g_build_filename(dir, "40-layer-rules.kdl", NULL);
    GString *layer_kdl = g_string_new(
        "// Layer Surface Rules (Managed by kajo-settings)\n"
        "layer-rule {\n"
        "    match namespace=\"^quickshell$\"\n"
        "    place-within-backdrop true\n"
        "}\n");

    for (int i = 0; i < N_NIRI_NAMESPACES; i++) {
        char op_buf[32];
        g_ascii_formatd(op_buf, sizeof(op_buf), "%.2f", self->ns_opacity[i] / 100.0);
        g_string_append_printf(
            layer_kdl,
            "\nlayer-rule {\n"
            "    match namespace=\"%s\"\n"
            "    opacity %s\n\n"
            "    background-effect {\n"
            "        blur true\n"
            "        xray %s\n"
            "    }\n\n"
            "    popups {\n"
            "        opacity %s\n"
            "        background-effect {\n"
            "            blur true\n"
            "        }\n"
            "    }\n"
            "}\n",
            NIRI_NS_IDS[i],
            op_buf,
            self->ns_xray[i] ? "true" : "false",
            op_buf);
    }

    g_file_set_contents(layer_path, layer_kdl->str, layer_kdl->len, NULL);
    g_string_free(layer_kdl, TRUE);

    /* 2. Also write 00-kajo.kdl with valid dot-decimal formatting */
    g_autofree gchar *kajo_path = g_build_filename(dir, "00-kajo.kdl", NULL);
    char active_op_buf[32];
    g_ascii_formatd(active_op_buf, sizeof(active_op_buf), "%.2f", self->ns_opacity[active_idx] / 100.0);

    g_autofree gchar *kdl_content = g_strdup_printf(
        "// Managed by kajo-settings — DO NOT EDIT DIRECTLY\n\n"
        "layer-rule {\n"
        "    match namespace=\"%s\"\n"
        "    opacity %s\n"
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
        NIRI_NS_IDS[active_idx],
        active_op_buf,
        self->ns_xray[active_idx] ? "true" : "false",
        gap);

    g_file_set_contents(kajo_path, kdl_content, -1, NULL);
    g_message("Saved modular Niri layer-rules to %s and %s", layer_path, kajo_path);

    niri_send_action("{\"Action\":\"ReloadConfig\"}");
}

GtkWidget *
build_page_niri(void)
{
    SettingsPageNiri *page = g_object_new(settings_page_niri_get_type(), NULL);
    load_existing_layer_rules(page);

    const char *ns_items[] = {
        "kajo (Panel & Popovers)",
        "kajo-launcher (App Launcher)",
        "kajo-notification (Toasts)",
        NULL
    };
    GtkStringList *slist_ns = gtk_string_list_new(ns_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_ns), G_LIST_MODEL(slist_ns));
    g_object_unref(slist_ns);

    const char *xray_items[] = {
        "Disabled (Default Solid Blur)",
        "Enabled (Pass-through Wallpaper)",
        NULL
    };
    GtkStringList *slist_xray = gtk_string_list_new(xray_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_xray), G_LIST_MODEL(slist_xray));
    g_object_unref(slist_xray);

    page->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(page->scale_op), page->ns_opacity[0]);
    g_autofree gchar *op_str = g_strdup_printf("%d%%", page->ns_opacity[0]);
    gtk_label_set_text(GTK_LABEL(page->lbl_op_val), op_str);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_xray), page->ns_xray[0] ? 1 : 0);
    page->syncing_ui = FALSE;

    g_signal_connect(page->combo_ns, "notify::selected", G_CALLBACK(on_combo_ns_changed), page);
    g_signal_connect(page->scale_op, "value-changed", G_CALLBACK(on_scale_op_changed), page);
    g_signal_connect(page->combo_xray, "notify::selected", G_CALLBACK(on_combo_xray_changed), page);

    gtk_range_set_value(GTK_RANGE(page->scale_gap), 12);
    g_signal_connect(page->scale_gap, "value-changed", G_CALLBACK(on_scale_gap_changed), page->lbl_gap_val);

    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_reload_niri_clicked), page);

    return create_settings_page_card("NIRI COMPOSITOR RULES & GAPS", "[ KDL IPC ]", GTK_WIDGET(page), NULL);
}
