#include "../settings_common.h"
#include "../settings_pages.h"
#include <string.h>

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageWidgets {
    GtkBox parent_instance;

    GtkWidget *card_left;
    GtkWidget *lbl_left;
    GtkWidget *card_center;
    GtkWidget *lbl_center;
    GtkWidget *card_right;
    GtkWidget *lbl_right;
    GtkWidget *combo_add_widget;
    GtkWidget *combo_add_section;
    GtkWidget *combo_add_mode;
    GtkWidget *btn_add;
    GtkWidget *btn_apply;
} SettingsPageWidgets;

typedef struct _SettingsPageWidgetsClass {
    GtkBoxClass parent_class;
} SettingsPageWidgetsClass;

G_DEFINE_TYPE(SettingsPageWidgets, settings_page_widgets, GTK_TYPE_BOX)

static const char *ALL_WIDGET_IDS[] = {
    "launcher",
    "workspaces",
    "window",
    "clock",
    "tray",
    "notifications",
    "media",
    "network",
    "bluetooth",
    "cpu-mem",
    "keyboard-layout",
    "volume",
    "battery",
    "backlight",
    "privacy",
    "theme",
    "custom-script",
    "idle-inhibitor",
    "dnd",
    "control-center",
    "power",
    NULL
};

static void
settings_page_widgets_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), settings_page_widgets_get_type());
    G_OBJECT_CLASS(settings_page_widgets_parent_class)->dispose(object);
}

static void
settings_page_widgets_init(SettingsPageWidgets *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_widgets_class_init(SettingsPageWidgetsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_page_widgets_dispose;

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_widgets.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, card_left);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, lbl_left);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, card_center);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, lbl_center);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, card_right);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, lbl_right);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, combo_add_widget);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, combo_add_section);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, combo_add_mode);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, btn_add);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, btn_apply);
}

static guint
count_card_widgets(GtkWidget *card)
{
    guint count = 0;
    for (GtkWidget *child = gtk_widget_get_first_child(card);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        if (g_object_get_data(G_OBJECT(child), "widget-id") != NULL) {
            count++;
        }
    }
    return count;
}

static void
update_section_counts(SettingsPageWidgets *self)
{
    if (!self) return;

    guint left_count = count_card_widgets(self->card_left);
    g_autofree gchar *left_title = g_strdup_printf("🔵 1. LEFT PANEL SECTION (%u Active Widgets)", left_count);
    gtk_label_set_text(GTK_LABEL(self->lbl_left), left_title);

    guint center_count = count_card_widgets(self->card_center);
    g_autofree gchar *center_title = g_strdup_printf("🟠 2. CENTER PANEL SECTION (%u Active Widgets)", center_count);
    gtk_label_set_text(GTK_LABEL(self->lbl_center), center_title);

    guint right_count = count_card_widgets(self->card_right);
    g_autofree gchar *right_title = g_strdup_printf("🟢 3. RIGHT PANEL SECTION (%u Active Widgets)", right_count);
    gtk_label_set_text(GTK_LABEL(self->lbl_right), right_title);
}

static void
on_widget_up_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent)) return;

    GtkWidget *prev = gtk_widget_get_prev_sibling(row);
    if (prev && g_object_get_data(G_OBJECT(prev), "widget-id") != NULL) {
        GtkWidget *prev_prev = gtk_widget_get_prev_sibling(prev);
        gtk_box_reorder_child_after(GTK_BOX(parent), row, prev_prev);
    }
}

static void
on_widget_down_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent)) return;

    GtkWidget *next = gtk_widget_get_next_sibling(row);
    if (next) {
        gtk_box_reorder_child_after(GTK_BOX(parent), row, next);
    }
}

static void
on_widget_move_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent)) return;

    SettingsPageWidgets *self = g_object_get_data(G_OBJECT(row), "page-widgets");
    GtkWidget *next_card = g_object_get_data(G_OBJECT(parent), "next-card");
    if (next_card && GTK_IS_BOX(next_card)) {
        g_object_ref(row);
        gtk_box_remove(GTK_BOX(parent), row);
        gtk_box_append(GTK_BOX(next_card), row);
        g_object_unref(row);
        update_section_counts(self);
    }
}

static void
on_widget_remove_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent)) return;

    SettingsPageWidgets *self = g_object_get_data(G_OBJECT(row), "page-widgets");
    gtk_box_remove(GTK_BOX(parent), row);
    update_section_counts(self);
}

static const char *
widget_id_to_icon(const char *id)
{
    if (g_strcmp0(id, "launcher") == 0) return "system-search-symbolic";
    if (g_strcmp0(id, "window") == 0) return "window-new-symbolic";
    if (g_strcmp0(id, "workspaces") == 0) return "view-grid-symbolic";
    if (g_strcmp0(id, "clock") == 0) return "preferences-system-time-symbolic";
    if (g_strcmp0(id, "volume") == 0) return "audio-volume-high-symbolic";
    if (g_strcmp0(id, "network") == 0) return "network-wireless-symbolic";
    if (g_strcmp0(id, "battery") == 0) return "battery-good-symbolic";
    if (g_strcmp0(id, "backlight") == 0) return "display-brightness-symbolic";
    if (g_strcmp0(id, "notifications") == 0) return "notifications-disabled-symbolic";
    if (g_strcmp0(id, "control-center") == 0) return "emblem-system-symbolic";
    if (g_strcmp0(id, "bluetooth") == 0) return "bluetooth-active-symbolic";
    if (g_strcmp0(id, "media") == 0) return "media-playback-start-symbolic";
    if (g_strcmp0(id, "dnd") == 0) return "notifications-disabled-symbolic";
    if (g_strcmp0(id, "tray") == 0) return "folder-symbolic";
    if (g_strcmp0(id, "cpu-mem") == 0) return "utilities-system-monitor-symbolic";
    if (g_strcmp0(id, "power") == 0) return "system-shutdown-symbolic";
    if (g_strcmp0(id, "privacy") == 0) return "security-high-symbolic";
    if (g_strcmp0(id, "idle-inhibitor") == 0) return "weather-clear-symbolic";
    if (g_strcmp0(id, "keyboard-layout") == 0) return "input-keyboard-symbolic";
    if (g_strcmp0(id, "theme") == 0) return "preferences-desktop-appearance-symbolic";
    if (g_strcmp0(id, "custom-script") == 0) return "utilities-terminal-symbolic";
    return "view-grid-symbolic";
}

static GtkWidget *
create_widget_item_row(SettingsPageWidgets *self, const char *icon_name, const char *title, ShellWidgetMode mode)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(row, "widget-row-card");
    g_object_set_data_full(G_OBJECT(row), "widget-id", g_strdup(title), g_free);
    g_object_set_data(G_OBJECT(row), "page-widgets", self);

    /* Col 1: Drag Handle (24px) */
    GtkWidget *drag_handle = gtk_label_new("☰");
    gtk_widget_set_size_request(drag_handle, 24, -1);
    gtk_widget_add_css_class(drag_handle, "dim-label");
    gtk_box_append(GTK_BOX(row), drag_handle);

    /* Col 2: Icon (24px) */
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
    gtk_widget_set_size_request(icon, 24, -1);
    gtk_box_append(GTK_BOX(row), icon);

    /* Col 3: Title (Flex / hexpand) */
    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(row), lbl);

    /* Col 4: Display Mode Dropdown (Fixed 160px) */
    const char *mode_items[] = { "Full (Icon+Text)", "Icon Only", "Text Only", NULL };
    GtkWidget *combo_mode = create_string_dropdown(mode_items);
    guint selected_idx = (mode == WIDGET_MODE_ICON) ? 1 : ((mode == WIDGET_MODE_LABEL) ? 2 : 0);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(combo_mode), selected_idx);
    gtk_widget_set_size_request(combo_mode, 160, -1);
    gtk_box_append(GTK_BOX(row), combo_mode);
    g_object_set_data(G_OBJECT(row), "combo-mode", combo_mode);

    /* Col 5: Action Button Bar (Fixed 150px) */
    GtkWidget *btn_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request(btn_bar, 150, -1);

    GtkWidget *btn_up = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_add_css_class(btn_up, "flat");
    gtk_widget_set_tooltip_text(btn_up, "Move Widget Up");
    g_signal_connect(btn_up, "clicked", G_CALLBACK(on_widget_up_clicked), row);

    GtkWidget *btn_down = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_add_css_class(btn_down, "flat");
    gtk_widget_set_tooltip_text(btn_down, "Move Widget Down");
    g_signal_connect(btn_down, "clicked", G_CALLBACK(on_widget_down_clicked), row);

    GtkWidget *btn_move = gtk_button_new_from_icon_name("object-flip-horizontal-symbolic");
    gtk_widget_add_css_class(btn_move, "flat");
    gtk_widget_set_tooltip_text(btn_move, "Transfer to Next Section (Left/Center/Right)");
    g_signal_connect(btn_move, "clicked", G_CALLBACK(on_widget_move_clicked), row);

    GtkWidget *btn_rem = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(btn_rem, "flat");
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
on_add_widget_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;

    guint widget_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_add_widget));
    guint n_widgets = G_N_ELEMENTS(ALL_WIDGET_IDS) - 1;
    if (widget_idx >= n_widgets) widget_idx = 0;
    const char *widget_id = ALL_WIDGET_IDS[widget_idx];

    guint section_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_add_section));
    GtkWidget *target_card = self->card_right;
    if (section_idx == 0) target_card = self->card_left;
    else if (section_idx == 1) target_card = self->card_center;

    guint mode_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_add_mode));
    ShellWidgetMode mode = WIDGET_MODE_ICON;
    if (mode_idx == 0) mode = WIDGET_MODE_FULL;
    else if (mode_idx == 2) mode = WIDGET_MODE_LABEL;

    GtkWidget *row = create_widget_item_row(self, widget_id_to_icon(widget_id), widget_id, mode);
    gtk_box_append(GTK_BOX(target_card), row);
    update_section_counts(self);
}

static GPtrArray *
collect_card_widget_configs(GtkWidget *card, GPtrArray *old_arr)
{
    GPtrArray *arr = g_ptr_array_new_full(0, (GDestroyNotify)shell_widget_config_free);
    for (GtkWidget *child = gtk_widget_get_first_child(card);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        const char *id = g_object_get_data(G_OBJECT(child), "widget-id");
        if (!id) continue;

        GtkWidget *combo_mode = g_object_get_data(G_OBJECT(child), "combo-mode");
        ShellWidgetMode mode = WIDGET_MODE_ICON;
        if (combo_mode && GTK_IS_DROP_DOWN(combo_mode)) {
            guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(combo_mode));
            if (sel == 0) mode = WIDGET_MODE_FULL;
            else if (sel == 2) mode = WIDGET_MODE_LABEL;
            else mode = WIDGET_MODE_ICON;
        }

        ShellWidgetConfig *wc = g_new0(ShellWidgetConfig, 1);
        wc->id = g_strdup(id);
        wc->mode = mode;

        if (old_arr) {
            for (guint i = 0; i < old_arr->len; i++) {
                ShellWidgetConfig *old_wc = g_ptr_array_index(old_arr, i);
                if (old_wc && g_strcmp0(old_wc->id, id) == 0) {
                    wc->interval_sec = old_wc->interval_sec;
                    wc->exec_cmd = g_strdup(old_wc->exec_cmd);
                    wc->click_left_cmd = g_strdup(old_wc->click_left_cmd);
                    wc->click_right_cmd = g_strdup(old_wc->click_right_cmd);
                    break;
                }
            }
        }

        g_ptr_array_add(arr, wc);
    }
    return arr;
}

static void
on_apply_widget_layout_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageWidgets *self = (SettingsPageWidgets *)user_data;
    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();

    GPtrArray *new_left = collect_card_widget_configs(self->card_left, config->layout_left);
    GPtrArray *new_center = collect_card_widget_configs(self->card_center, config->layout_center);
    GPtrArray *new_right = collect_card_widget_configs(self->card_right, config->layout_right);

    if (config->layout_left) g_ptr_array_free(config->layout_left, TRUE);
    if (config->layout_center) g_ptr_array_free(config->layout_center, TRUE);
    if (config->layout_right) g_ptr_array_free(config->layout_right, TRUE);

    config->layout_left = new_left;
    config->layout_center = new_center;
    config->layout_right = new_right;

    g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "kajo", "config.json", NULL);
    if (shell_config_save(config, path)) {
        g_message("Saved panel widget layout configuration to %s", path);
        settings_send_ipc_command("reload-config");
    } else {
        g_warning("Failed to save panel widget layout configuration to %s", path);
    }

    shell_config_destroy(config);
}

GtkWidget *
build_page_widgets(void)
{
    SettingsPageWidgets *page = g_object_new(settings_page_widgets_get_type(), NULL);
    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();

    /* 1. LEFT PANEL SECTION */
    if (config->layout_left && config->layout_left->len > 0) {
        for (guint i = 0; i < config->layout_left->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_left, i);
            GtkWidget *row = create_widget_item_row(page, widget_id_to_icon(wc->id), wc->id, wc->mode);
            gtk_box_append(GTK_BOX(page->card_left), row);
        }
    } else {
        GtkWidget *w1 = create_widget_item_row(page, "system-search-symbolic", "launcher", WIDGET_MODE_ICON);
        GtkWidget *w2 = create_widget_item_row(page, "window-new-symbolic", "window", WIDGET_MODE_ICON);
        GtkWidget *w3 = create_widget_item_row(page, "view-grid-symbolic", "workspaces", WIDGET_MODE_ICON);
        gtk_box_append(GTK_BOX(page->card_left), w1);
        gtk_box_append(GTK_BOX(page->card_left), w2);
        gtk_box_append(GTK_BOX(page->card_left), w3);
    }

    /* 2. CENTER PANEL SECTION */
    if (config->layout_center && config->layout_center->len > 0) {
        for (guint i = 0; i < config->layout_center->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_center, i);
            GtkWidget *row = create_widget_item_row(page, widget_id_to_icon(wc->id), wc->id, wc->mode);
            gtk_box_append(GTK_BOX(page->card_center), row);
        }
    } else {
        GtkWidget *w4 = create_widget_item_row(page, "preferences-system-time-symbolic", "clock", WIDGET_MODE_ICON);
        gtk_box_append(GTK_BOX(page->card_center), w4);
    }

    /* 3. RIGHT PANEL SECTION */
    if (config->layout_right && config->layout_right->len > 0) {
        for (guint i = 0; i < config->layout_right->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_right, i);
            GtkWidget *row = create_widget_item_row(page, widget_id_to_icon(wc->id), wc->id, wc->mode);
            gtk_box_append(GTK_BOX(page->card_right), row);
        }
    } else {
        GtkWidget *w5 = create_widget_item_row(page, "audio-volume-high-symbolic", "volume", WIDGET_MODE_ICON);
        GtkWidget *w6 = create_widget_item_row(page, "network-wireless-symbolic", "network", WIDGET_MODE_ICON);
        GtkWidget *w7 = create_widget_item_row(page, "battery-good-symbolic", "battery", WIDGET_MODE_ICON);
        GtkWidget *w8 = create_widget_item_row(page, "notifications-disabled-symbolic", "notifications", WIDGET_MODE_ICON);
        GtkWidget *w9 = create_widget_item_row(page, "emblem-system-symbolic", "control-center", WIDGET_MODE_ICON);
        gtk_box_append(GTK_BOX(page->card_right), w5);
        gtk_box_append(GTK_BOX(page->card_right), w6);
        gtk_box_append(GTK_BOX(page->card_right), w7);
        gtk_box_append(GTK_BOX(page->card_right), w8);
        gtk_box_append(GTK_BOX(page->card_right), w9);
    }

    g_object_set_data(G_OBJECT(page->card_left), "next-card", page->card_center);
    g_object_set_data(G_OBJECT(page->card_center), "next-card", page->card_right);
    g_object_set_data(G_OBJECT(page->card_right), "next-card", page->card_left);

    update_section_counts(page);
    shell_config_destroy(config);

    /* Populate Add Widget bar dropdowns */
    GtkStringList *widget_model = gtk_string_list_new(ALL_WIDGET_IDS);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_add_widget), G_LIST_MODEL(widget_model));
    g_object_unref(widget_model);

    const char *section_items[] = { "Left Section", "Center Section", "Right Section", NULL };
    GtkStringList *section_model = gtk_string_list_new(section_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_add_section), G_LIST_MODEL(section_model));
    g_object_unref(section_model);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_add_section), 2);

    const char *mode_items[] = { "Full (Icon+Text)", "Icon Only", "Text Only", NULL };
    GtkStringList *mode_model = gtk_string_list_new(mode_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_add_mode), G_LIST_MODEL(mode_model));
    g_object_unref(mode_model);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->combo_add_mode), 1);

    g_signal_connect(page->btn_add, "clicked", G_CALLBACK(on_add_widget_clicked), page);
    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_apply_widget_layout_clicked), page);

    return create_settings_page_card("PANEL WIDGET LAYOUT & APPEARANCES", "[ ACTIVE WIDGET LAYOUT ]", GTK_WIDGET(page), NULL);
}
