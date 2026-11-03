#include "../settings_common.h"
#include "../settings_pages.h"
#include <string.h>

static const char *SWIPE_CENTER_IDS[] = { "launcher", "overview", "osk", "none" };
static const char *SWIPE_CENTER_LABELS[] = {
    "Open App Launcher (Default)",
    "Toggle Niri Overview",
    "Toggle Virtual Keyboard",
    "Disabled",
    NULL
};

static const char *SWIPE_RIGHT_IDS[] = { "control-center", "notifications", "osk", "none" };
static const char *SWIPE_RIGHT_LABELS[] = {
    "Open Control Center (Default)",
    "Open Notifications Center",
    "Toggle Virtual Keyboard",
    "Disabled",
    NULL
};

static const char *SWIPE_HORIZ_IDS[] = { "column", "workspace", "none" };
static const char *SWIPE_HORIZ_LABELS[] = {
    "Switch Window Column Left / Right (Default)",
    "Switch Workspace Up / Down",
    "Disabled",
    NULL
};

static const char *OSK_MODE_IDS[] = { "tablet", "always", "manual" };
static const char *OSK_MODE_LABELS[] = {
    "In Tablet Mode Only (Default)",
    "Always When Text Input Focused",
    "Manual Toggle Only",
    NULL
};

static const char *OSK_PUSH_LABELS[] = {
    "Push Tiled Windows Up (Exclusive Zone)",
    "Float Over Windows",
    NULL
};

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPagePanel {
    GtkBox parent_instance;

    GtkWidget *combo_pos;
    GtkWidget *toggle_grid;
    GtkWidget *scale_height;
    GtkWidget *lbl_height_val;
    GtkWidget *scale_rev;
    GtkWidget *lbl_rev_val;

    GtkWidget *combo_touch_enabled;
    GtkWidget *combo_swipe_center;
    GtkWidget *combo_swipe_right;
    GtkWidget *combo_swipe_horiz;
    GtkWidget *scale_touch_dwell;
    GtkWidget *lbl_touch_dwell_val;

    GtkWidget *combo_osk_mode;
    GtkWidget *combo_osk_push;
    GtkWidget *scale_osk_height;
    GtkWidget *lbl_osk_height_val;

    GtkWidget *btn_apply;
} SettingsPagePanel;

typedef struct _SettingsPagePanelClass {
    GtkBoxClass parent_class;
} SettingsPagePanelClass;

G_DEFINE_TYPE(SettingsPagePanel, settings_page_panel, GTK_TYPE_BOX)

static void on_combo_pos_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data);

static void
settings_page_panel_init(SettingsPagePanel *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_panel_dispose(GObject *object)
{
    SettingsPagePanel *self = (SettingsPagePanel *)object;
    if (self->combo_pos) {
        g_signal_handlers_disconnect_matched(
            self->combo_pos, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
            G_CALLBACK(on_combo_pos_changed), NULL);
    }
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

    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, combo_touch_enabled);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, combo_swipe_center);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, combo_swipe_right);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, combo_swipe_horiz);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, scale_touch_dwell);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, lbl_touch_dwell_val);

    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, combo_osk_mode);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, combo_osk_push);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, scale_osk_height);
    gtk_widget_class_bind_template_child(widget_class, SettingsPagePanel, lbl_osk_height_val);

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
on_scale_touch_dwell_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%dms", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_combo_pos_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *tile_edge = GTK_WIDGET(user_data);
    guint selected = gtk_drop_down_get_selected(dropdown);
    gboolean is_bottom = (selected == 1);

    if (is_bottom) {
        gtk_widget_add_css_class(tile_edge, "active");
    } else {
        gtk_widget_remove_css_class(tile_edge, "active");
    }

    GtkWidget *lbl_sub = g_object_get_data(G_OBJECT(tile_edge), "tile-subtitle-label");
    if (lbl_sub && GTK_IS_LABEL(lbl_sub)) {
        gtk_label_set_text(GTK_LABEL(lbl_sub), is_bottom ? "Bottom Position" : "Top Position");
    }
}

static void
on_tile_edge_clicked(GtkButton *btn, gpointer user_data)
{
    GtkDropDown *combo_pos = GTK_DROP_DOWN(user_data);
    gboolean is_bottom = gtk_widget_has_css_class(GTK_WIDGET(btn), "active");
    GtkWidget *lbl_sub = g_object_get_data(G_OBJECT(btn), "tile-subtitle-label");
    if (lbl_sub && GTK_IS_LABEL(lbl_sub)) {
        gtk_label_set_text(GTK_LABEL(lbl_sub), is_bottom ? "Bottom Position" : "Top Position");
    }
    guint target = is_bottom ? 1 : 0;
    if (gtk_drop_down_get_selected(combo_pos) != target) {
        gtk_drop_down_set_selected(combo_pos, target);
    }
}

static void
on_tile_hide_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    gboolean active = gtk_widget_has_css_class(GTK_WIDGET(btn), "active");
    GtkWidget *lbl_sub = g_object_get_data(G_OBJECT(btn), "tile-subtitle-label");
    if (lbl_sub && GTK_IS_LABEL(lbl_sub)) {
        gtk_label_set_text(GTK_LABEL(lbl_sub), active ? "Enabled" : "Disabled");
    }
}

static void
on_tile_shadow_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    gboolean active = gtk_widget_has_css_class(GTK_WIDGET(btn), "active");
    GtkWidget *lbl_sub = g_object_get_data(G_OBJECT(btn), "tile-subtitle-label");
    if (lbl_sub && GTK_IS_LABEL(lbl_sub)) {
        gtk_label_set_text(GTK_LABEL(lbl_sub), active ? "Active (rgba 82%)" : "Solid Surface");
    }
}

static void
on_tile_margin_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    gboolean active = gtk_widget_has_css_class(GTK_WIDGET(btn), "active");
    GtkWidget *lbl_sub = g_object_get_data(G_OBJECT(btn), "tile-subtitle-label");
    if (lbl_sub && GTK_IS_LABEL(lbl_sub)) {
        gtk_label_set_text(GTK_LABEL(lbl_sub), active ? "8px Floating" : "0px Full Width");
    }
}

static guint
find_id_index(const char *const *ids, guint n_ids, const char *val)
{
    if (!val)
        return 0;
    for (guint i = 0; i < n_ids; i++) {
        if (g_strcmp0(ids[i], val) == 0)
            return i;
    }
    return 0;
}

static void
on_apply_panel_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPagePanel *page = (SettingsPagePanel *)user_data;
    ShellConfig *config = shell_config_load(NULL);
    if (!config) config = shell_config_get_default();

    GtkWidget *tile_hide = g_object_get_data(G_OBJECT(btn), "tile_hide");

    if (page->combo_pos)
        config->panel_position = (gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_pos)) == 1)
                                 ? PANEL_POSITION_BOTTOM
                                 : PANEL_POSITION_TOP;
    if (page->scale_height)
        config->panel_height = (int)gtk_range_get_value(GTK_RANGE(page->scale_height));
    if (page->scale_rev) {
        config->autohide_reveal_delay = (int)gtk_range_get_value(GTK_RANGE(page->scale_rev));
        config->autohide_reveal_duration = (int)gtk_range_get_value(GTK_RANGE(page->scale_rev));
    }
    if (tile_hide)
        config->autohide_enabled = gtk_widget_has_css_class(tile_hide, "active");

    if (page->combo_touch_enabled)
        config->touch_gestures_enabled = (gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_touch_enabled)) == 0);

    if (page->combo_swipe_center) {
        guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_swipe_center));
        if (idx >= G_N_ELEMENTS(SWIPE_CENTER_IDS)) idx = 0;
        g_free(config->touch_swipe_center);
        config->touch_swipe_center = g_strdup(SWIPE_CENTER_IDS[idx]);
    }

    if (page->combo_swipe_right) {
        guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_swipe_right));
        if (idx >= G_N_ELEMENTS(SWIPE_RIGHT_IDS)) idx = 0;
        g_free(config->touch_swipe_right);
        config->touch_swipe_right = g_strdup(SWIPE_RIGHT_IDS[idx]);
    }

    if (page->combo_swipe_horiz) {
        guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_swipe_horiz));
        if (idx >= G_N_ELEMENTS(SWIPE_HORIZ_IDS)) idx = 0;
        g_free(config->touch_swipe_horizontal);
        config->touch_swipe_horizontal = g_strdup(SWIPE_HORIZ_IDS[idx]);
    }

    if (page->scale_touch_dwell)
        config->touch_dwell_ms = (int)gtk_range_get_value(GTK_RANGE(page->scale_touch_dwell));

    if (page->combo_osk_mode) {
        guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_osk_mode));
        if (idx >= G_N_ELEMENTS(OSK_MODE_IDS)) idx = 0;
        g_free(config->osk_auto_show_mode);
        config->osk_auto_show_mode = g_strdup(OSK_MODE_IDS[idx]);
    }

    if (page->combo_osk_push)
        config->osk_push_windows = (gtk_drop_down_get_selected(GTK_DROP_DOWN(page->combo_osk_push)) == 0);

    if (page->scale_osk_height)
        config->osk_height_px = (int)gtk_range_get_value(GTK_RANGE(page->scale_osk_height));

    const gchar *xdg_config = g_get_user_config_dir();
    gchar *dir = g_build_filename(xdg_config, "kajo", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "config.json", NULL);

    if (shell_config_save(config, path)) {
        g_message("Saved panel and touch gesture configuration to %s", path);
        settings_send_ipc_command("reload-config");
    }
    shell_config_destroy(config);
    g_free(dir);
    g_free(path);

    niri_send_action("{\"Action\":{\"LoadConfigFile\":{\"path\":null}}}");
}

GtkWidget *
build_page_panel(void)
{
    SettingsPagePanel *page = g_object_new(settings_page_panel_get_type(), NULL);
    ShellConfig *config = shell_config_load(NULL);
    if (!config) config = shell_config_get_default();

    const char *pos_items[] = { "Top Edge (Default)", "Bottom Edge", NULL };
    GtkStringList *slist = gtk_string_list_new(pos_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_pos), G_LIST_MODEL(slist));
    g_object_unref(slist);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_pos), config->panel_position == PANEL_POSITION_BOTTOM ? 1 : 0);

    /* Quick Toggle Tiles Grid */
    GtkWidget *tile_edge = create_metro_toggle_tile("tb-adjustments-horizontal-symbolic", "Panel Edge", config->panel_position == PANEL_POSITION_BOTTOM ? "Bottom Position" : "Top Position", config->panel_position == PANEL_POSITION_BOTTOM);
    GtkWidget *tile_hide = create_metro_toggle_tile("view-reveal-symbolic", "Autohide", config->autohide_enabled ? "Enabled" : "Disabled", config->autohide_enabled);
    GtkWidget *tile_shadow = create_metro_toggle_tile("layer-visible-symbolic", "Panel Blur", "Active (rgba 82%)", TRUE);
    GtkWidget *tile_margin = create_metro_toggle_tile("zoom-fit-best-symbolic", "Outer Margins", "0px Full Width", FALSE);

    g_signal_connect(page->combo_pos, "notify::selected", G_CALLBACK(on_combo_pos_changed), tile_edge);
    g_signal_connect_after(tile_edge, "clicked", G_CALLBACK(on_tile_edge_clicked), page->combo_pos);
    g_signal_connect_after(tile_hide, "clicked", G_CALLBACK(on_tile_hide_clicked), NULL);
    g_signal_connect_after(tile_shadow, "clicked", G_CALLBACK(on_tile_shadow_clicked), NULL);
    g_signal_connect_after(tile_margin, "clicked", G_CALLBACK(on_tile_margin_clicked), NULL);

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

    /* Touch & Swipe Gesture Controls */
    const char *touch_en_items[] = { "Enabled (Default)", "Disabled", NULL };
    GtkStringList *en_list = gtk_string_list_new(touch_en_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_touch_enabled), G_LIST_MODEL(en_list));
    g_object_unref(en_list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_touch_enabled), config->touch_gestures_enabled ? 0 : 1);

    GtkStringList *center_list = gtk_string_list_new(SWIPE_CENTER_LABELS);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_swipe_center), G_LIST_MODEL(center_list));
    g_object_unref(center_list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_swipe_center),
                               find_id_index(SWIPE_CENTER_IDS, G_N_ELEMENTS(SWIPE_CENTER_IDS), config->touch_swipe_center));

    GtkStringList *right_list = gtk_string_list_new(SWIPE_RIGHT_LABELS);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_swipe_right), G_LIST_MODEL(right_list));
    g_object_unref(right_list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_swipe_right),
                               find_id_index(SWIPE_RIGHT_IDS, G_N_ELEMENTS(SWIPE_RIGHT_IDS), config->touch_swipe_right));

    GtkStringList *horiz_list = gtk_string_list_new(SWIPE_HORIZ_LABELS);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_swipe_horiz), G_LIST_MODEL(horiz_list));
    g_object_unref(horiz_list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_swipe_horiz),
                               find_id_index(SWIPE_HORIZ_IDS, G_N_ELEMENTS(SWIPE_HORIZ_IDS), config->touch_swipe_horizontal));

    gint dwell_ms = config->touch_dwell_ms > 0 ? config->touch_dwell_ms : 4000;
    gtk_range_set_value(GTK_RANGE(page->scale_touch_dwell), dwell_ms);
    gchar *dwell_str = g_strdup_printf("%dms", dwell_ms);
    gtk_label_set_text(GTK_LABEL(page->lbl_touch_dwell_val), dwell_str);
    g_free(dwell_str);

    g_signal_connect(page->scale_touch_dwell, "value-changed",
                     G_CALLBACK(on_scale_touch_dwell_changed), page->lbl_touch_dwell_val);

    GtkStringList *osk_mode_list = gtk_string_list_new(OSK_MODE_LABELS);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_osk_mode), G_LIST_MODEL(osk_mode_list));
    g_object_unref(osk_mode_list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_osk_mode),
                               find_id_index(OSK_MODE_IDS, G_N_ELEMENTS(OSK_MODE_IDS), config->osk_auto_show_mode));

    GtkStringList *osk_push_list = gtk_string_list_new(OSK_PUSH_LABELS);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_osk_push), G_LIST_MODEL(osk_push_list));
    g_object_unref(osk_push_list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_osk_push), config->osk_push_windows ? 0 : 1);

    gint osk_h = config->osk_height_px >= 180 ? config->osk_height_px : 260;
    gtk_range_set_value(GTK_RANGE(page->scale_osk_height), osk_h);
    gchar *osk_h_str = g_strdup_printf("%dpx", osk_h);
    gtk_label_set_text(GTK_LABEL(page->lbl_osk_height_val), osk_h_str);
    g_free(osk_h_str);

    g_signal_connect(page->scale_osk_height, "value-changed",
                     G_CALLBACK(on_scale_height_changed), page->lbl_osk_height_val);

    shell_config_destroy(config);

    g_object_set_data(G_OBJECT(page->btn_apply), "tile_hide", tile_hide);
    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_apply_panel_clicked), page);

    return create_settings_page_card("PANEL GEOMETRY & TOUCH GESTURES", "[ LIVE GEOMETRY & TOUCH ]", GTK_WIDGET(page), NULL);
}
