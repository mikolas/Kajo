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
    GtkWidget *btn_apply;
} SettingsPageWidgets;

typedef struct _SettingsPageWidgetsClass {
    GtkBoxClass parent_class;
} SettingsPageWidgetsClass;

G_DEFINE_TYPE(SettingsPageWidgets, settings_page_widgets, GTK_TYPE_BOX)

static void
settings_page_widgets_init(SettingsPageWidgets *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_widgets_class_init(SettingsPageWidgetsClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_widgets.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, card_left);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, lbl_left);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, card_center);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, lbl_center);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, card_right);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, lbl_right);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageWidgets, btn_apply);
}

static void
on_widget_up_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent)) return;

    GtkWidget *prev = gtk_widget_get_prev_sibling(row);
    if (prev) {
        GtkWidget *prev_prev = gtk_widget_get_prev_sibling(prev);
        if (prev_prev) {
            g_object_ref(row);
            gtk_box_remove(GTK_BOX(parent), row);
            gtk_box_insert_child_after(GTK_BOX(parent), row, prev_prev);
            g_object_unref(row);
        }
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
        g_object_ref(row);
        gtk_box_remove(GTK_BOX(parent), row);
        gtk_box_insert_child_after(GTK_BOX(parent), row, next);
        g_object_unref(row);
    }
}

static void
on_widget_move_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent)) return;

    GtkWidget *next_card = g_object_get_data(G_OBJECT(parent), "next-card");
    if (next_card && GTK_IS_BOX(next_card)) {
        g_object_ref(row);
        gtk_box_remove(GTK_BOX(parent), row);
        gtk_box_append(GTK_BOX(next_card), row);
        g_object_unref(row);
    }
}

static void
on_widget_remove_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (!parent || !GTK_IS_BOX(parent)) return;

    gtk_box_remove(GTK_BOX(parent), row);
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
    return "view-grid-symbolic";
}

static GtkWidget *
create_widget_item_row(const char *icon_name, const char *title, ShellWidgetMode mode)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(row, "widget-row-card");

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
on_apply_widget_layout_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    g_message("Saved panel widget layout configuration.");
}

GtkWidget *
build_page_widgets(void)
{
    SettingsPageWidgets *page = g_object_new(settings_page_widgets_get_type(), NULL);
    ShellConfig *config = shell_config_load(NULL);

    /* 1. LEFT PANEL SECTION */
    guint left_count = config->layout_left ? config->layout_left->len : 0;
    gchar *left_title = g_strdup_printf("🔵 1. LEFT PANEL SECTION (%u Active Widgets)", left_count);
    gtk_label_set_text(GTK_LABEL(page->lbl_left), left_title);
    g_free(left_title);

    if (config->layout_left && config->layout_left->len > 0) {
        for (guint i = 0; i < config->layout_left->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_left, i);
            GtkWidget *row = create_widget_item_row(widget_id_to_icon(wc->id), wc->id, wc->mode);
            gtk_box_append(GTK_BOX(page->card_left), row);
        }
    } else {
        GtkWidget *w1 = create_widget_item_row("system-search-symbolic", "launcher", WIDGET_MODE_ICON);
        GtkWidget *w2 = create_widget_item_row("window-new-symbolic", "window", WIDGET_MODE_ICON);
        GtkWidget *w3 = create_widget_item_row("view-grid-symbolic", "workspaces", WIDGET_MODE_ICON);
        gtk_box_append(GTK_BOX(page->card_left), w1);
        gtk_box_append(GTK_BOX(page->card_left), w2);
        gtk_box_append(GTK_BOX(page->card_left), w3);
    }

    /* 2. CENTER PANEL SECTION */
    guint center_count = config->layout_center ? config->layout_center->len : 0;
    gchar *center_title = g_strdup_printf("🟠 2. CENTER PANEL SECTION (%u Active Widgets)", center_count);
    gtk_label_set_text(GTK_LABEL(page->lbl_center), center_title);
    g_free(center_title);

    if (config->layout_center && config->layout_center->len > 0) {
        for (guint i = 0; i < config->layout_center->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_center, i);
            GtkWidget *row = create_widget_item_row(widget_id_to_icon(wc->id), wc->id, wc->mode);
            gtk_box_append(GTK_BOX(page->card_center), row);
        }
    } else {
        GtkWidget *w4 = create_widget_item_row("preferences-system-time-symbolic", "clock", WIDGET_MODE_ICON);
        gtk_box_append(GTK_BOX(page->card_center), w4);
    }

    /* 3. RIGHT PANEL SECTION */
    guint right_count = config->layout_right ? config->layout_right->len : 0;
    gchar *right_title = g_strdup_printf("🟢 3. RIGHT PANEL SECTION (%u Active Widgets)", right_count);
    gtk_label_set_text(GTK_LABEL(page->lbl_right), right_title);
    g_free(right_title);

    if (config->layout_right && config->layout_right->len > 0) {
        for (guint i = 0; i < config->layout_right->len; i++) {
            ShellWidgetConfig *wc = g_ptr_array_index(config->layout_right, i);
            GtkWidget *row = create_widget_item_row(widget_id_to_icon(wc->id), wc->id, wc->mode);
            gtk_box_append(GTK_BOX(page->card_right), row);
        }
    } else {
        GtkWidget *w5 = create_widget_item_row("audio-volume-high-symbolic", "volume", WIDGET_MODE_ICON);
        GtkWidget *w6 = create_widget_item_row("network-wireless-symbolic", "network", WIDGET_MODE_ICON);
        GtkWidget *w7 = create_widget_item_row("battery-good-symbolic", "battery", WIDGET_MODE_ICON);
        GtkWidget *w8 = create_widget_item_row("notifications-disabled-symbolic", "notifications", WIDGET_MODE_ICON);
        GtkWidget *w9 = create_widget_item_row("emblem-system-symbolic", "control-center", WIDGET_MODE_ICON);
        gtk_box_append(GTK_BOX(page->card_right), w5);
        gtk_box_append(GTK_BOX(page->card_right), w6);
        gtk_box_append(GTK_BOX(page->card_right), w7);
        gtk_box_append(GTK_BOX(page->card_right), w8);
        gtk_box_append(GTK_BOX(page->card_right), w9);
    }

    g_object_set_data(G_OBJECT(page->card_left), "next-card", page->card_center);
    g_object_set_data(G_OBJECT(page->card_center), "next-card", page->card_right);
    g_object_set_data(G_OBJECT(page->card_right), "next-card", page->card_left);

    shell_config_destroy(config);

    g_signal_connect(page->btn_apply, "clicked", G_CALLBACK(on_apply_widget_layout_clicked), NULL);

    return create_settings_page_card("PANEL WIDGET LAYOUT & APPEARANCES", "[ ACTIVE WIDGET LAYOUT ]", GTK_WIDGET(page), NULL);
}
