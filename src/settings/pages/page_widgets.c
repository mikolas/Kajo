#include "../settings_common.h"
#include "../settings_pages.h"
#include <string.h>

typedef struct {
    const char *id;
    const char *dropdown_label;
    const char *icon_name;
} WidgetCatalogEntry;

static const WidgetCatalogEntry WIDGET_CATALOG[] = {
    { "launcher",        "launcher — App Launcher",            "system-search-symbolic" },
    { "workspaces",      "workspaces — Niri Workspaces",       "view-grid-symbolic" },
    { "window",          "window — Focused Window Title",      "window-new-symbolic" },
    { "clock",           "clock — Clock & Calendar",           "preferences-system-time-symbolic" },
    { "media",           "media — MPRIS2 + 16-Band FFT",       "media-playback-start-symbolic" },
    { "tray",            "tray — StatusNotifierItem Tray",     "folder-symbolic" },
    { "notifications",   "notifications — Notification Bell",  "preferences-system-notifications-symbolic" },
    { "dnd",             "dnd — Do Not Disturb Toggle",        "notifications-disabled-symbolic" },
    { "network",         "network — NetworkManager Wi-Fi",     "network-wireless-symbolic" },
    { "bluetooth",       "bluetooth — BlueZ Peripherals",      "bluetooth-active-symbolic" },
    { "volume",          "volume — PipeWire Audio Volume",     "audio-volume-high-symbolic" },
    { "backlight",       "backlight — Display Brightness",     "display-brightness-symbolic" },
    { "battery",         "battery — UPower Battery Status",    "battery-good-symbolic" },
    { "cpu-mem",         "cpu-mem — CPU & RAM Telemetry",      "utilities-system-monitor-symbolic" },
    { "keyboard-layout", "keyboard-layout — XKB Layout",       "input-keyboard-symbolic" },
    { "osk",             "osk — Virtual Keyboard Toggle",      "input-keyboard-symbolic" },
    { "privacy",         "privacy — Mic / Cam Privacy",        "security-high-symbolic" },
    { "idle-inhibitor",  "idle-inhibitor — Keep Awake",        "weather-clear-symbolic" },
    { "theme",           "theme — Accent Quick Picker",        "preferences-desktop-appearance-symbolic" },
    { "custom-script",   "custom-script — JSON Script Poll",   "utilities-terminal-symbolic" },
    { "control-center",  "control-center — Quick Tiles",       "emblem-system-symbolic" },
    { "power",           "power — Session Power Menu",         "system-shutdown-symbolic" },
};
#define N_WIDGET_CATALOG G_N_ELEMENTS(WIDGET_CATALOG)

static const char *const WIDGET_MODE_LABELS[] = { "ICON", "TEXT", "FULL", NULL };

typedef struct {
    gint   interval_sec;
    gchar *exec_cmd;
    gchar *click_left_cmd;
    gchar *click_right_cmd;
} WidgetRowExtras;

static void
widget_row_extras_free(gpointer data)
{
    WidgetRowExtras *ex = data;
    if (!ex)
        return;
    g_free(ex->exec_cmd);
    g_free(ex->click_left_cmd);
    g_free(ex->click_right_cmd);
    g_free(ex);
}

typedef struct _SettingsPageWidgets {
    GtkBox parent_instance;

    GtkWidget *combo_add_widget;
    GtkWidget *seg_add_section;
    GtkWidget *seg_add_mode;

    GtkWidget *card_left;
    GtkWidget *list_left;
    GtkWidget *card_center;
    GtkWidget *list_center;
    GtkWidget *card_right;
    GtkWidget *list_right;

    gboolean   syncing_ui;
} SettingsPageWidgets;

typedef struct _SettingsPageWidgetsClass {
    GtkBoxClass parent_class;
} SettingsPageWidgetsClass;

G_DEFINE_TYPE(SettingsPageWidgets, settings_page_widgets, GTK_TYPE_BOX)

static const char *
widget_id_to_icon(const char *id)
{
    for (size_t i = 0; i < N_WIDGET_CATALOG; i++) {
        if (g_strcmp0(WIDGET_CATALOG[i].id, id) == 0)
            return WIDGET_CATALOG[i].icon_name;
    }
    return "view-grid-symbolic";
}

static guint
widget_mode_to_segment(ShellWidgetMode mode)
{
    if (mode == WIDGET_MODE_LABEL) return 1;
    if (mode == WIDGET_MODE_FULL)  return 2;
    return 0;
}

static ShellWidgetMode
segment_to_widget_mode(guint seg)
{
    if (seg == 1) return WIDGET_MODE_LABEL;
    if (seg == 2) return WIDGET_MODE_FULL;
    return WIDGET_MODE_ICON;
}

static guint
count_box_widgets(GtkWidget *list_box)
{
    guint count = 0;
    if (!list_box)
        return 0;
    for (GtkWidget *c = gtk_widget_get_first_child(list_box);
         c != NULL;
         c = gtk_widget_get_next_sibling(c)) {
        if (g_object_get_data(G_OBJECT(c), "widget-id") != NULL)
            count++;
    }
    return count;
}

static void
clear_box_children(GtkWidget *box)
{
    if (!box)
        return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL) {
        gtk_box_remove(GTK_BOX(box), child);
    }
}

static void
update_widgets_counts_and_telemetry(SettingsPageWidgets *self)
{
    if (!self)
        return;

    guint nl = count_box_widgets(self->list_left);
    guint nc = count_box_widgets(self->list_center);
    guint nr = count_box_widgets(self->list_right);
    guint total = nl + nc + nr;

    g_autofree gchar *bl = g_strdup_printf("%u ACTIVE", nl);
    g_autofree gchar *bc = g_strdup_printf("%u ACTIVE", nc);
    g_autofree gchar *br = g_strdup_printf("%u ACTIVE", nr);
    section_card_set_badge(self->card_left, bl, "accent");
    section_card_set_badge(self->card_center, bc, NULL);
    section_card_set_badge(self->card_right, br, NULL);

    g_autofree gchar *c0 = g_strdup_printf("%u WIDGETS", nl);
    g_autofree gchar *c1 = g_strdup_printf("%u WIDGETS", nc);
    g_autofree gchar *c2 = g_strdup_printf("%u WIDGETS", nr);
    g_autofree gchar *c3 = g_strdup_printf("%u ACTIVE", total);
    g_autofree gchar *nav = g_strdup_printf("%u", total);

    settings_page_set_counter(GTK_WIDGET(self), 0, "LEFT", c0, "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "CENTER", c1, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "RIGHT", c2, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "TOTAL", c3, NULL);
    settings_page_set_nav_badge(GTK_WIDGET(self), "widgets", nav, NULL);
}

static void
on_row_mode_changed(GtkWidget *group G_GNUC_UNUSED, guint selected G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;
    if (!self || self->syncing_ui)
        return;
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_widget_up_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent))
        return;

    SettingsPageWidgets *self = g_object_get_data(G_OBJECT(row), "page-widgets");
    GtkWidget *prev = gtk_widget_get_prev_sibling(row);
    if (prev && g_object_get_data(G_OBJECT(prev), "widget-id") != NULL) {
        GtkWidget *prev_prev = gtk_widget_get_prev_sibling(prev);
        gtk_box_reorder_child_after(GTK_BOX(parent), row, prev_prev);
        if (self)
            settings_page_set_dirty(GTK_WIDGET(self), TRUE);
    }
}

static void
on_widget_down_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent))
        return;

    SettingsPageWidgets *self = g_object_get_data(G_OBJECT(row), "page-widgets");
    GtkWidget *next = gtk_widget_get_next_sibling(row);
    if (next && g_object_get_data(G_OBJECT(next), "widget-id") != NULL) {
        gtk_box_reorder_child_after(GTK_BOX(parent), row, next);
        if (self)
            settings_page_set_dirty(GTK_WIDGET(self), TRUE);
    }
}

static void
on_widget_move_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent))
        return;

    SettingsPageWidgets *self = g_object_get_data(G_OBJECT(row), "page-widgets");
    GtkWidget *next_list = g_object_get_data(G_OBJECT(parent), "next-list");
    if (next_list && GTK_IS_BOX(next_list)) {
        g_object_ref(row);
        gtk_box_remove(GTK_BOX(parent), row);
        gtk_box_append(GTK_BOX(next_list), row);
        g_object_unref(row);
        if (self) {
            update_widgets_counts_and_telemetry(self);
            settings_page_set_dirty(GTK_WIDGET(self), TRUE);
        }
    }
}

static void
on_widget_remove_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent))
        return;

    SettingsPageWidgets *self = g_object_get_data(G_OBJECT(row), "page-widgets");
    gtk_box_remove(GTK_BOX(parent), row);
    if (self) {
        update_widgets_counts_and_telemetry(self);
        settings_page_set_dirty(GTK_WIDGET(self), TRUE);
    }
}

static GtkWidget *
create_widget_item_row(SettingsPageWidgets     *self,
                       const char              *id,
                       ShellWidgetMode          mode,
                       const ShellWidgetConfig *src_wc)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_add_css_class(row, "widget-row-card");
    gtk_widget_add_css_class(row, "widget-item-row");
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(row, TRUE);
    g_object_set_data_full(G_OBJECT(row), "widget-id", g_strdup(id), g_free);
    g_object_set_data(G_OBJECT(row), "page-widgets", self);

    if (src_wc && (src_wc->interval_sec != 0 || src_wc->exec_cmd ||
                   src_wc->click_left_cmd || src_wc->click_right_cmd)) {
        WidgetRowExtras *ex = g_new0(WidgetRowExtras, 1);
        ex->interval_sec   = src_wc->interval_sec;
        ex->exec_cmd       = g_strdup(src_wc->exec_cmd);
        ex->click_left_cmd = g_strdup(src_wc->click_left_cmd);
        ex->click_right_cmd = g_strdup(src_wc->click_right_cmd);
        g_object_set_data_full(G_OBJECT(row), "widget-extras", ex, widget_row_extras_free);
    }

    GtkWidget *drag_handle = gtk_label_new("☰");
    gtk_widget_add_css_class(drag_handle, "dim-label");
    gtk_widget_set_hexpand(drag_handle, FALSE);
    gtk_widget_set_valign(drag_handle, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), drag_handle);

    GtkWidget *icon = gtk_image_new_from_icon_name(widget_id_to_icon(id));
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 14);
    gtk_widget_set_hexpand(icon, FALSE);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), icon);

    GtkWidget *lbl = gtk_label_new(id);
    gtk_widget_add_css_class(lbl, "row-title");
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_FILL);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(lbl), 3);
    gtk_widget_set_tooltip_text(lbl, id);
    gtk_box_append(GTK_BOX(row), lbl);

    GtkWidget *seg_mode = create_segmented_group(WIDGET_MODE_LABELS, widget_mode_to_segment(mode));
    segmented_group_connect(seg_mode, on_row_mode_changed, self);
    gtk_box_append(GTK_BOX(row), seg_mode);
    g_object_set_data(G_OBJECT(row), "seg-mode", seg_mode);

    GtkWidget *btn_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_hexpand(btn_bar, FALSE);
    gtk_widget_set_valign(btn_bar, GTK_ALIGN_CENTER);

    GtkWidget *btn_up = gtk_button_new_with_label("↑");
    gtk_widget_add_css_class(btn_up, "files-nav-btn");
    gtk_widget_add_css_class(btn_up, "widget-item-btn");
    gtk_widget_set_tooltip_text(btn_up, "Move Widget Up");
    g_signal_connect(btn_up, "clicked", G_CALLBACK(on_widget_up_clicked), row);

    GtkWidget *btn_down = gtk_button_new_with_label("↓");
    gtk_widget_add_css_class(btn_down, "files-nav-btn");
    gtk_widget_add_css_class(btn_down, "widget-item-btn");
    gtk_widget_set_tooltip_text(btn_down, "Move Widget Down");
    g_signal_connect(btn_down, "clicked", G_CALLBACK(on_widget_down_clicked), row);

    GtkWidget *btn_move = gtk_button_new_with_label("⇄");
    gtk_widget_add_css_class(btn_move, "files-nav-btn");
    gtk_widget_add_css_class(btn_move, "widget-item-btn");
    gtk_widget_set_tooltip_text(btn_move, "Transfer to Next Section (Left → Center → Right)");
    g_signal_connect(btn_move, "clicked", G_CALLBACK(on_widget_move_clicked), row);

    GtkWidget *btn_rem = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(btn_rem, "files-nav-btn");
    gtk_widget_add_css_class(btn_rem, "widget-item-btn");
    gtk_widget_add_css_class(btn_rem, "action-btn-danger");
    gtk_widget_set_tooltip_text(btn_rem, "Remove Widget from Panel");
    g_signal_connect(btn_rem, "clicked", G_CALLBACK(on_widget_remove_clicked), row);

    gtk_box_append(GTK_BOX(btn_bar), btn_up);
    gtk_box_append(GTK_BOX(btn_bar), btn_down);
    gtk_box_append(GTK_BOX(btn_bar), btn_move);
    gtk_box_append(GTK_BOX(btn_bar), btn_rem);
    gtk_box_append(GTK_BOX(row), btn_bar);

    return row;
}

static void
populate_section_list(SettingsPageWidgets *self, GtkWidget *list_box, GPtrArray *arr)
{
    clear_box_children(list_box);
    if (!arr)
        return;
    for (guint i = 0; i < arr->len; i++) {
        ShellWidgetConfig *wc = g_ptr_array_index(arr, i);
        if (!wc || !wc->id)
            continue;
        GtkWidget *row = create_widget_item_row(self, wc->id, wc->mode, wc);
        gtk_box_append(GTK_BOX(list_box), row);
    }
}

static void
load_widgets_from_shell_config(SettingsPageWidgets *self, const KajoShellCfg *config, gboolean mark_dirty)
{
    if (!self || !config)
        return;

    self->syncing_ui = TRUE;
    populate_section_list(self, self->list_left, config->layout_left);
    populate_section_list(self, self->list_center, config->layout_center);
    populate_section_list(self, self->list_right, config->layout_right);
    self->syncing_ui = FALSE;

    update_widgets_counts_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), mark_dirty);
}

static void
on_add_widget_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;
    if (!self)
        return;

    guint w_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_add_widget));
    if (w_idx >= N_WIDGET_CATALOG)
        w_idx = 0;
    const char *widget_id = WIDGET_CATALOG[w_idx].id;

    guint sec_idx = segmented_group_get_selected(self->seg_add_section);
    GtkWidget *target_list = self->list_right;
    if (sec_idx == 0)      target_list = self->list_left;
    else if (sec_idx == 1) target_list = self->list_center;

    ShellWidgetMode mode = segment_to_widget_mode(segmented_group_get_selected(self->seg_add_mode));

    GtkWidget *row = create_widget_item_row(self, widget_id, mode, NULL);
    gtk_box_append(GTK_BOX(target_list), row);
    update_widgets_counts_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static GPtrArray *
collect_list_widget_configs(GtkWidget *list_box)
{
    GPtrArray *arr = g_ptr_array_new_full(0, (GDestroyNotify)shell_widget_config_free);
    if (!list_box)
        return arr;

    for (GtkWidget *child = gtk_widget_get_first_child(list_box);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        const char *id = g_object_get_data(G_OBJECT(child), "widget-id");
        if (!id)
            continue;

        GtkWidget *seg_mode = g_object_get_data(G_OBJECT(child), "seg-mode");
        ShellWidgetMode mode = seg_mode
            ? segment_to_widget_mode(segmented_group_get_selected(seg_mode))
            : WIDGET_MODE_ICON;

        ShellWidgetConfig *wc = g_new0(ShellWidgetConfig, 1);
        wc->id = g_strdup(id);
        wc->mode = mode;

        WidgetRowExtras *ex = g_object_get_data(G_OBJECT(child), "widget-extras");
        if (ex) {
            wc->interval_sec    = ex->interval_sec;
            wc->exec_cmd        = g_strdup(ex->exec_cmd);
            wc->click_left_cmd  = g_strdup(ex->click_left_cmd);
            wc->click_right_cmd = g_strdup(ex->click_right_cmd);
        }

        g_ptr_array_add(arr, wc);
    }
    return arr;
}

static void
action_default_widgets_layout(gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;
    if (!self)
        return;
    g_autoptr(KajoShellCfg) def_cfg = kajo_shell_cfg_new_default();
    load_widgets_from_shell_config(self, def_cfg, TRUE);
}

static void
action_revert_widgets(gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;
    if (!self)
        return;
    g_autoptr(KajoShellCfgRef) cfg = kajo_config_store_acquire_shell(NULL);
    load_widgets_from_shell_config(self, cfg, FALSE);
}

static void
mutate_page_widgets_cfg(KajoShellCfg *config, gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;
    GPtrArray *new_left   = collect_list_widget_configs(self->list_left);
    GPtrArray *new_center = collect_list_widget_configs(self->list_center);
    GPtrArray *new_right  = collect_list_widget_configs(self->list_right);

    if (config->layout_left)   g_ptr_array_free(config->layout_left, TRUE);
    if (config->layout_center) g_ptr_array_free(config->layout_center, TRUE);
    if (config->layout_right)  g_ptr_array_free(config->layout_right, TRUE);

    config->layout_left   = new_left;
    config->layout_center = new_center;
    config->layout_right  = new_right;
}

static void
action_save_widgets(gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;
    if (!self)
        return;

    if (kajo_config_store_update_shell(NULL, mutate_page_widgets_cfg, self, NULL)) {
        settings_send_ipc_command("reload-config");
    }

    update_widgets_counts_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
on_widgets_page_mapped(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    update_widgets_counts_and_telemetry((SettingsPageWidgets *)widget);
}

static void
settings_page_widgets_init(SettingsPageWidgets *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_widgets_class_init(SettingsPageWidgetsClass *klass G_GNUC_UNUSED)
{
}

GtkWidget *
build_page_widgets(void)
{
    SettingsPageWidgets *self = g_object_new(settings_page_widgets_get_type(), NULL);

    /* ─── Top Full-Width Card: Add Panel Widget Catalog ─── */
    GtkWidget *card_add = create_section_card(
        SETTINGS_CARD_LEFT,
        "ADD PANEL WIDGET TO SECTION (22 REGISTERED SHELLWIDGET CLASSES)",
        "22 MODULES",
        "accent",
        NULL);

    const char *catalog_labels[N_WIDGET_CATALOG + 1];
    for (size_t i = 0; i < N_WIDGET_CATALOG; i++)
        catalog_labels[i] = WIDGET_CATALOG[i].dropdown_label;
    catalog_labels[N_WIDGET_CATALOG] = NULL;

    self->combo_add_widget = create_string_dropdown(catalog_labels);
    GtkWidget *add_mod_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(add_mod_box, GTK_ALIGN_CENTER);

    GtkWidget *btn_add = gtk_button_new_with_label("+ ADD");
    gtk_widget_add_css_class(btn_add, "files-nav-btn");
    gtk_widget_add_css_class(btn_add, "action-btn-primary");
    gtk_widget_set_valign(btn_add, GTK_ALIGN_CENTER);
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_add_widget_clicked), self);

    gtk_box_append(GTK_BOX(add_mod_box), self->combo_add_widget);
    gtk_box_append(GTK_BOX(add_mod_box), btn_add);

    gtk_box_append(GTK_BOX(card_add), create_boxed_row(
        "🧩",
        "Widget Module",
        "Select from 22 built-in ShellWidget classes",
        add_mod_box));

    static const char *const sec_items[] = { "LEFT", "CENTER", "RIGHT", NULL };
    self->seg_add_section = create_segmented_group(sec_items, 2);
    self->seg_add_mode = create_segmented_group(WIDGET_MODE_LABELS, 0);

    GtkWidget *add_target_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(add_target_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(add_target_box), self->seg_add_section);
    gtk_box_append(GTK_BOX(add_target_box), self->seg_add_mode);

    gtk_box_append(GTK_BOX(card_add), create_boxed_row(
        "📍",
        "Target Section & Mode",
        "Panel zone and initial display mode (Icon / Text / Full)",
        add_target_box));

    settings_bento_add_full(GTK_WIDGET(self), card_add);

    /* ─── Active Section Cards (1. Left + 2. Center Bento Pair, 3. Right Full) ─── */
    self->card_left = create_section_card(
        SETTINGS_CARD_LEFT, "1. LEFT PANEL SECTION", "0 ACTIVE", "accent", NULL);
    self->list_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(self->list_left, TRUE);
    gtk_box_append(GTK_BOX(self->card_left), self->list_left);

    self->card_center = create_section_card(
        SETTINGS_CARD_CENTER, "2. CENTER PANEL SECTION", "0 ACTIVE", NULL, NULL);
    self->list_center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(self->list_center, TRUE);
    gtk_box_append(GTK_BOX(self->card_center), self->list_center);

    self->card_right = create_section_card(
        SETTINGS_CARD_RIGHT, "3. RIGHT PANEL SECTION", "0 ACTIVE", NULL, NULL);
    self->list_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(self->list_right, TRUE);
    gtk_box_append(GTK_BOX(self->card_right), self->list_right);

    g_object_set_data(G_OBJECT(self->list_left), "next-list", self->list_center);
    g_object_set_data(G_OBJECT(self->list_center), "next-list", self->list_right);
    g_object_set_data(G_OBJECT(self->list_right), "next-list", self->list_left);

    settings_bento_add_pair(GTK_WIDGET(self), self->card_left, self->card_center);
    settings_bento_add_full(GTK_WIDGET(self), self->card_right);

    g_autoptr(KajoShellCfgRef) cfg = kajo_config_store_acquire_shell(NULL);
    load_widgets_from_shell_config(self, cfg, FALSE);

    g_signal_connect(self, "map", G_CALLBACK(on_widgets_page_mapped), NULL);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "↺ DEFAULT LAYOUT",
        action_default_widgets_layout,
        action_revert_widgets,
        action_save_widgets,
        self);

    return GTK_WIDGET(self);
}
