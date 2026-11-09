#include "../settings_common.h"
#include "../settings_pages.h"
#include <string.h>

static const char *const SWIPE_CENTER_IDS[] = { "launcher", "overview", "osk", "none" };
static const char *const SWIPE_CENTER_LABELS[] = {
    "Open App Launcher (Default)",
    "Toggle Niri Overview",
    "Toggle Virtual Keyboard",
    "Disabled",
    NULL
};

static const char *const SWIPE_RIGHT_IDS[] = { "control-center", "notifications", "osk", "none" };
static const char *const SWIPE_RIGHT_LABELS[] = {
    "Open Control Center (Default)",
    "Open Notifications Center",
    "Toggle Virtual Keyboard",
    "Disabled",
    NULL
};

static const char *const SWIPE_HORIZ_IDS[] = { "column", "workspace", "none" };
static const char *const SWIPE_HORIZ_LABELS[] = {
    "Switch Window Column Left / Right",
    "Switch Workspace Up / Down",
    "Disabled",
    NULL
};

static const char *const OSK_MODE_IDS[] = { "tablet", "always", "manual" };
static const char *const OSK_MODE_SHORT[] = { "TABLET", "ALWAYS", "MANUAL" };

typedef struct _SettingsPagePanel {
    GtkBox parent_instance;

    GtkWidget *card_geom;
    GtkWidget *seg_pos;
    GtkWidget *tile_hide;
    GtkWidget *row_rev;
    GtkWidget *scale_rev;
    GtkWidget *scale_height;

    GtkWidget *card_touch;
    GtkWidget *sw_touch_enabled;
    GtkWidget *row_swipe_center;
    GtkWidget *combo_swipe_center;
    GtkWidget *row_swipe_right;
    GtkWidget *combo_swipe_right;
    GtkWidget *row_swipe_horiz;
    GtkWidget *combo_swipe_horiz;
    GtkWidget *row_touch_dwell;
    GtkWidget *scale_touch_dwell;

    GtkWidget *card_osk;
    GtkWidget *seg_osk_mode;
    GtkWidget *seg_osk_push;
    GtkWidget *scale_osk_height;

    gboolean   syncing_ui;
} SettingsPagePanel;

typedef struct _SettingsPagePanelClass {
    GtkBoxClass parent_class;
} SettingsPagePanelClass;

G_DEFINE_TYPE(SettingsPagePanel, settings_page_panel, GTK_TYPE_BOX)

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
sync_panel_Dependent_sensitivity(SettingsPagePanel *self)
{
    if (!self)
        return;

    gboolean hide_on = self->tile_hide &&
                       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tile_hide));
    if (self->tile_hide) {
        toggle_card_set_subtitle(
            self->tile_hide,
            hide_on ? "Enabled — 2px screen-edge reveal strip" : "Disabled — panel stays docked");
    }
    if (self->row_rev)
        gtk_widget_set_sensitive(self->row_rev, hide_on);

    gboolean touch_on = self->sw_touch_enabled &&
                        gtk_switch_get_active(GTK_SWITCH(self->sw_touch_enabled));
    if (self->row_swipe_center) gtk_widget_set_sensitive(self->row_swipe_center, touch_on);
    if (self->row_swipe_right)  gtk_widget_set_sensitive(self->row_swipe_right, touch_on);
    if (self->row_swipe_horiz)  gtk_widget_set_sensitive(self->row_swipe_horiz, touch_on);
    if (self->row_touch_dwell)  gtk_widget_set_sensitive(self->row_touch_dwell, touch_on);
}

static void
update_panel_badges_and_telemetry(SettingsPagePanel *self)
{
    if (!self)
        return;

    gboolean is_btm = (segmented_group_get_selected(self->seg_pos) == 1);
    const gchar *edge_str = is_btm ? "BOTTOM" : "TOP";
    int h_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_height));
    gboolean hide_on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tile_hide));
    gboolean touch_on = gtk_switch_get_active(GTK_SWITCH(self->sw_touch_enabled));
    guint osk_idx = segmented_group_get_selected(self->seg_osk_mode);
    if (osk_idx >= G_N_ELEMENTS(OSK_MODE_SHORT))
        osk_idx = 0;
    int osk_h = (int)gtk_range_get_value(GTK_RANGE(self->scale_osk_height));

    g_autofree gchar *geom_badge = g_strdup_printf("%s • %dPX", edge_str, h_px);
    section_card_set_badge(self->card_geom, geom_badge, "accent");
    section_card_set_badge(self->card_touch, touch_on ? "ENABLED" : "DISABLED", touch_on ? "accent" : NULL);

    g_autofree gchar *osk_badge = g_strdup_printf("%dPX", osk_h);
    section_card_set_badge(self->card_osk, osk_badge, NULL);

    g_autofree gchar *c0 = g_strdup_printf("%s (%dPX)", edge_str, h_px);
    g_autofree gchar *c3 = g_strdup_printf("%s (%dPX)", OSK_MODE_SHORT[osk_idx], osk_h);

    settings_page_set_counter(GTK_WIDGET(self), 0, "EDGE", c0, "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "AUTOHIDE", hide_on ? "ENABLED" : "DISABLED", NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "SWIPES", touch_on ? "ENABLED" : "DISABLED", NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "OSK", c3, NULL);
    settings_page_set_nav_badge(GTK_WIDGET(self), "panel", is_btm ? "BTM" : "TOP", NULL);
}

static void
load_panel_ui_from_config(SettingsPagePanel *self)
{
    if (!self)
        return;

    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();

    self->syncing_ui = TRUE;

    segmented_group_set_selected(self->seg_pos, config->panel_position == PANEL_POSITION_BOTTOM ? 1 : 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tile_hide), config->autohide_enabled);
    gtk_range_set_value(GTK_RANGE(self->scale_height), config->panel_height > 0 ? config->panel_height : 32);
    gtk_range_set_value(GTK_RANGE(self->scale_rev),
                        config->autohide_reveal_delay > 0 ? config->autohide_reveal_delay : 150);

    gtk_switch_set_active(GTK_SWITCH(self->sw_touch_enabled), config->touch_gestures_enabled);
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(self->combo_swipe_center),
        find_id_index(SWIPE_CENTER_IDS, G_N_ELEMENTS(SWIPE_CENTER_IDS), config->touch_swipe_center));
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(self->combo_swipe_right),
        find_id_index(SWIPE_RIGHT_IDS, G_N_ELEMENTS(SWIPE_RIGHT_IDS), config->touch_swipe_right));
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(self->combo_swipe_horiz),
        find_id_index(SWIPE_HORIZ_IDS, G_N_ELEMENTS(SWIPE_HORIZ_IDS), config->touch_swipe_horizontal));
    gtk_range_set_value(GTK_RANGE(self->scale_touch_dwell),
                        config->touch_dwell_ms > 0 ? config->touch_dwell_ms : 4000);

    segmented_group_set_selected(
        self->seg_osk_mode,
        find_id_index(OSK_MODE_IDS, G_N_ELEMENTS(OSK_MODE_IDS), config->osk_auto_show_mode));
    segmented_group_set_selected(self->seg_osk_push, config->osk_push_windows ? 0 : 1);
    gtk_range_set_value(GTK_RANGE(self->scale_osk_height),
                        config->osk_height_px >= 180 ? config->osk_height_px : 260);

    self->syncing_ui = FALSE;
    shell_config_destroy(config);

    sync_panel_Dependent_sensitivity(self);
    update_panel_badges_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
mark_panel_changed(SettingsPagePanel *self)
{
    if (!self || self->syncing_ui)
        return;
    sync_panel_Dependent_sensitivity(self);
    update_panel_badges_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_panel_seg_changed(GtkWidget *group G_GNUC_UNUSED, guint selected G_GNUC_UNUSED, gpointer user_data)
{
    mark_panel_changed((SettingsPagePanel *)user_data);
}

static void
on_panel_toggle_changed(GtkToggleButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    mark_panel_changed((SettingsPagePanel *)user_data);
}

static void
on_panel_switch_or_combo_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    mark_panel_changed((SettingsPagePanel *)user_data);
}

static void
on_panel_range_changed(GtkRange *range G_GNUC_UNUSED, gpointer user_data)
{
    mark_panel_changed((SettingsPagePanel *)user_data);
}

static void
action_toggle_osk(gpointer user_data G_GNUC_UNUSED)
{
    settings_send_ipc_command("toggle-osk");
}

static void
action_revert_panel(gpointer user_data)
{
    load_panel_ui_from_config((SettingsPagePanel *)user_data);
}

static void
action_save_panel(gpointer user_data)
{
    SettingsPagePanel *self = (SettingsPagePanel *)user_data;
    if (!self)
        return;

    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();

    config->panel_position = (segmented_group_get_selected(self->seg_pos) == 1)
        ? PANEL_POSITION_BOTTOM
        : PANEL_POSITION_TOP;
    config->autohide_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tile_hide));
    config->panel_height = (int)gtk_range_get_value(GTK_RANGE(self->scale_height));
    config->autohide_reveal_delay = (int)gtk_range_get_value(GTK_RANGE(self->scale_rev));
    config->autohide_reveal_duration = config->autohide_reveal_delay;

    config->touch_gestures_enabled = gtk_switch_get_active(GTK_SWITCH(self->sw_touch_enabled));

    guint c_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_swipe_center));
    if (c_idx >= G_N_ELEMENTS(SWIPE_CENTER_IDS)) c_idx = 0;
    g_free(config->touch_swipe_center);
    config->touch_swipe_center = g_strdup(SWIPE_CENTER_IDS[c_idx]);

    guint r_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_swipe_right));
    if (r_idx >= G_N_ELEMENTS(SWIPE_RIGHT_IDS)) r_idx = 0;
    g_free(config->touch_swipe_right);
    config->touch_swipe_right = g_strdup(SWIPE_RIGHT_IDS[r_idx]);

    guint h_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_swipe_horiz));
    if (h_idx >= G_N_ELEMENTS(SWIPE_HORIZ_IDS)) h_idx = 0;
    g_free(config->touch_swipe_horizontal);
    config->touch_swipe_horizontal = g_strdup(SWIPE_HORIZ_IDS[h_idx]);

    config->touch_dwell_ms = (int)gtk_range_get_value(GTK_RANGE(self->scale_touch_dwell));

    guint osk_idx = segmented_group_get_selected(self->seg_osk_mode);
    if (osk_idx >= G_N_ELEMENTS(OSK_MODE_IDS)) osk_idx = 0;
    g_free(config->osk_auto_show_mode);
    config->osk_auto_show_mode = g_strdup(OSK_MODE_IDS[osk_idx]);

    config->osk_push_windows = (segmented_group_get_selected(self->seg_osk_push) == 0);
    config->osk_height_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_osk_height));

    g_autofree gchar *dir = g_build_filename(g_get_user_config_dir(), "kajo", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_autofree gchar *path = g_build_filename(dir, "config.json", NULL);

    if (shell_config_save(config, path)) {
        settings_send_ipc_command("reload-config");
    }
    shell_config_destroy(config);

    niri_reload_config();
    update_panel_badges_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
on_panel_page_mapped(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    update_panel_badges_and_telemetry((SettingsPagePanel *)widget);
}

static void
settings_page_panel_init(SettingsPagePanel *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_panel_class_init(SettingsPagePanelClass *klass G_GNUC_UNUSED)
{
}

GtkWidget *
build_page_panel(void)
{
    SettingsPagePanel *self = g_object_new(settings_page_panel_get_type(), NULL);

    /* ─── Card 1: Layer-Shell Panel Geometry & Autohide ─── */
    self->card_geom = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. LAYER-SHELL PANEL GEOMETRY & AUTOHIDE",
        "TOP • 32PX",
        "accent",
        NULL);

    static const char *const pos_items[] = { "TOP EDGE", "BOTTOM EDGE", NULL };
    self->seg_pos = create_segmented_group(pos_items, 0);
    gtk_box_append(GTK_BOX(self->card_geom), create_boxed_row(
        "▬",
        "Panel Screen Edge Position",
        "gtk4-layer-shell anchor edge",
        self->seg_pos));

    self->tile_hide = create_toggle_card(
        "👁",
        "Autohide",
        "Disabled — panel stays docked",
        FALSE);
    gtk_box_append(GTK_BOX(self->card_geom), self->tile_hide);

    GtkWidget *w_rev = create_scale_with_badge(50, 500, 25, 150, "%.0fms", &self->scale_rev, NULL);
    self->row_rev = create_boxed_row(
        "⏱",
        "Autohide Reveal Delay",
        "Edge hover dwell before panel slides in",
        w_rev);
    gtk_box_append(GTK_BOX(self->card_geom), self->row_rev);

    GtkWidget *w_height = create_scale_with_badge(24, 56, 1, 32, "%.0fpx", &self->scale_height, NULL);
    gtk_box_append(GTK_BOX(self->card_geom), create_boxed_row(
        "↕",
        "Panel Bar Thickness",
        "24px compact .. 56px touch-friendly",
        w_height));

    /* ─── Card 2: Touchscreen Bezel & Swipe Gestures ─── */
    self->card_touch = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. TOUCHSCREEN BEZEL & SWIPE GESTURES",
        "ENABLED",
        "accent",
        NULL);

    self->sw_touch_enabled = gtk_switch_new();
    gtk_box_append(GTK_BOX(self->card_touch), create_boxed_row(
        "👆",
        "Enable Panel Touch & Edge Swipes",
        "Bezel swipe recognition on touch displays",
        self->sw_touch_enabled));

    self->combo_swipe_center = create_string_dropdown(SWIPE_CENTER_LABELS);
    self->row_swipe_center = create_boxed_row(
        "↓",
        "Center / Left Inward Swipe",
        "Downward bezel pull action",
        self->combo_swipe_center);
    gtk_box_append(GTK_BOX(self->card_touch), self->row_swipe_center);

    self->combo_swipe_right = create_string_dropdown(SWIPE_RIGHT_LABELS);
    self->row_swipe_right = create_boxed_row(
        "↘",
        "Right Zone Inward Swipe",
        "Right-corner bezel pull action",
        self->combo_swipe_right);
    gtk_box_append(GTK_BOX(self->card_touch), self->row_swipe_right);

    self->combo_swipe_horiz = create_string_dropdown(SWIPE_HORIZ_LABELS);
    self->row_swipe_horiz = create_boxed_row(
        "↔",
        "Horizontal Panel Swipe",
        "Horizontal drag along panel bar",
        self->combo_swipe_horiz);
    gtk_box_append(GTK_BOX(self->card_touch), self->row_swipe_horiz);

    GtkWidget *w_dwell = create_scale_with_badge(1000, 8000, 500, 4000, "%.0fms", &self->scale_touch_dwell, NULL);
    self->row_touch_dwell = create_boxed_row(
        "⏳",
        "Autohide Touch Dwell Time",
        "Keep autohidden panel visible after touch",
        w_dwell);
    gtk_box_append(GTK_BOX(self->card_touch), self->row_touch_dwell);

    settings_bento_add_pair(GTK_WIDGET(self), self->card_geom, self->card_touch);

    /* ─── Card 3: Virtual Keyboard (ShellOSK) ─── */
    self->card_osk = create_section_card(
        SETTINGS_CARD_RIGHT,
        "3. VIRTUAL KEYBOARD (SHELLOSK • zwp_input_method_v2)",
        "260PX",
        NULL,
        NULL);

    static const char *const osk_mode_items[] = { "TABLET ONLY", "ALWAYS", "MANUAL", NULL };
    self->seg_osk_mode = create_segmented_group(osk_mode_items, 0);
    gtk_box_append(GTK_BOX(self->card_osk), create_boxed_row(
        "⌨",
        "Auto-Show Virtual Keyboard",
        "When a Wayland text-input-v3 surface gains focus",
        self->seg_osk_mode));

    static const char *const osk_push_items[] = { "PUSH WINDOWS", "FLOAT OVER", NULL };
    self->seg_osk_push = create_segmented_group(osk_push_items, 0);
    gtk_box_append(GTK_BOX(self->card_osk), create_boxed_row(
        "🗔",
        "Window Layout Behavior",
        "Exclusive layer-shell zone vs floating overlay",
        self->seg_osk_push));

    GtkWidget *w_osk_h = create_scale_with_badge(180, 420, 10, 260, "%.0fpx", &self->scale_osk_height, NULL);
    gtk_box_append(GTK_BOX(self->card_osk), create_boxed_row(
        "↕",
        "Virtual Keyboard Height",
        "180px compact .. 420px tall layout",
        w_osk_h));

    settings_bento_add_full(GTK_WIDGET(self), self->card_osk);

    /* Load saved config and wire signals */
    load_panel_ui_from_config(self);

    segmented_group_connect(self->seg_pos, on_panel_seg_changed, self);
    g_signal_connect(self->tile_hide, "toggled", G_CALLBACK(on_panel_toggle_changed), self);
    g_signal_connect(self->scale_rev, "value-changed", G_CALLBACK(on_panel_range_changed), self);
    g_signal_connect(self->scale_height, "value-changed", G_CALLBACK(on_panel_range_changed), self);

    g_signal_connect(self->sw_touch_enabled, "notify::active", G_CALLBACK(on_panel_switch_or_combo_changed), self);
    g_signal_connect(self->combo_swipe_center, "notify::selected", G_CALLBACK(on_panel_switch_or_combo_changed), self);
    g_signal_connect(self->combo_swipe_right, "notify::selected", G_CALLBACK(on_panel_switch_or_combo_changed), self);
    g_signal_connect(self->combo_swipe_horiz, "notify::selected", G_CALLBACK(on_panel_switch_or_combo_changed), self);
    g_signal_connect(self->scale_touch_dwell, "value-changed", G_CALLBACK(on_panel_range_changed), self);

    segmented_group_connect(self->seg_osk_mode, on_panel_seg_changed, self);
    segmented_group_connect(self->seg_osk_push, on_panel_seg_changed, self);
    g_signal_connect(self->scale_osk_height, "value-changed", G_CALLBACK(on_panel_range_changed), self);
    g_signal_connect(self, "map", G_CALLBACK(on_panel_page_mapped), NULL);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "⌨ TOGGLE OSK",
        action_toggle_osk,
        action_revert_panel,
        action_save_panel,
        self);

    return GTK_WIDGET(self);
}
