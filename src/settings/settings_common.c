#include "settings_common.h"
#include <string.h>

GtkWidget *
create_settings_page_card(const gchar *title_text, const gchar *badge_text, GtkWidget *body_widget, GtkWidget *footer_widget)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "shell-popover-card");
    gtk_widget_set_margin_start(card, 12);
    gtk_widget_set_margin_end(card, 12);
    gtk_widget_set_margin_top(card, 12);
    gtk_widget_set_margin_bottom(card, 12);

    /* Zone 1: Header */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "shell-popover-header");

    GtkWidget *lbl_title = gtk_label_new(title_text);
    gtk_widget_add_css_class(lbl_title, "heading");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);

    GtkWidget *lbl_badge = gtk_label_new(badge_text);
    gtk_widget_add_css_class(lbl_badge, "shell-popover-badge");
    gtk_widget_set_halign(lbl_badge, GTK_ALIGN_END);
    gtk_widget_set_hexpand(lbl_badge, TRUE);

    gtk_box_append(GTK_BOX(header), lbl_title);
    gtk_box_append(GTK_BOX(header), lbl_badge);
    gtk_box_append(GTK_BOX(card), header);

    /* Zone 2: Body */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(body, "shell-popover-body");
    if (body_widget) {
        gtk_box_append(GTK_BOX(body), body_widget);
    }
    gtk_box_append(GTK_BOX(card), body);

    /* Zone 3: Footer (Optional) */
    if (footer_widget) {
        GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(footer, "shell-popover-footer");
        gtk_box_append(GTK_BOX(footer), footer_widget);
        gtk_box_append(GTK_BOX(card), footer);
    }

    return card;
}

GtkWidget *
create_sidebar_category_header(const gchar *category_name)
{
    GtkWidget *lbl = gtk_label_new(category_name);
    gtk_widget_add_css_class(lbl, "sidebar-category-header");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_start(lbl, 16);
    gtk_widget_set_margin_top(lbl, 16);
    gtk_widget_set_margin_bottom(lbl, 6);
    return lbl;
}

GtkWidget *
create_sidebar_row(const gchar *icon_name, const gchar *label_text, const gchar *subtitle_text, const gchar *page_name)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_add_css_class(label, "sidebar-label");
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(vbox), label);

    if (subtitle_text && subtitle_text[0] != '\0') {
        GtkWidget *sub = gtk_label_new(subtitle_text);
        gtk_widget_add_css_class(sub, "shell-popover-subtitle");
        gtk_widget_set_halign(sub, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), sub);
    }

    GtkWidget *chevron = gtk_label_new("›");
    gtk_widget_add_css_class(chevron, "sidebar-chevron");

    gtk_box_append(GTK_BOX(row), icon);
    gtk_box_append(GTK_BOX(row), vbox);
    gtk_box_append(GTK_BOX(row), chevron);

    g_object_set_data_full(G_OBJECT(row), "page-name", g_strdup(page_name), g_free);
    return row;
}

GtkWidget *
create_action_row(const gchar *icon_name, const gchar *title, const gchar *subtitle, GtkWidget *control)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "section-card");
    gtk_widget_set_size_request(card, -1, 52);

    if (icon_name && icon_name[0] != '\0') {
        GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(card), icon);
    }

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl_title = gtk_label_new(title);
    gtk_widget_add_css_class(lbl_title, "row-title");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), lbl_title);

    if (subtitle && subtitle[0] != '\0') {
        GtkWidget *lbl_sub = gtk_label_new(subtitle);
        gtk_widget_add_css_class(lbl_sub, "shell-popover-subtitle");
        gtk_widget_set_halign(lbl_sub, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), lbl_sub);
    }

    gtk_box_append(GTK_BOX(card), vbox);

    if (control) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(card), control);
    }

    return card;
}

GtkWidget *
create_string_dropdown(const char *const items[])
{
    GtkStringList *str_list = gtk_string_list_new(items);
    GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(str_list), NULL);
    gtk_widget_set_hexpand(dropdown, TRUE);
    return dropdown;
}

static void
on_tile_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    if (gtk_widget_has_css_class(GTK_WIDGET(btn), "active")) {
        gtk_widget_remove_css_class(GTK_WIDGET(btn), "active");
    } else {
        gtk_widget_add_css_class(GTK_WIDGET(btn), "active");
    }
}

GtkWidget *
create_metro_toggle_tile(const gchar *icon_name, const gchar *title, const gchar *subtitle, gboolean active)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "control-center-tile");
    if (active) {
        gtk_widget_add_css_class(btn, "active");
    }
    gtk_widget_set_hexpand(btn, TRUE);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tile_clicked), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);

    GtkWidget *lbl_title = gtk_label_new(title);
    gtk_widget_add_css_class(lbl_title, "tile-title");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);

    GtkWidget *lbl_sub = gtk_label_new(subtitle);
    gtk_widget_add_css_class(lbl_sub, "tile-subtitle");
    gtk_widget_set_halign(lbl_sub, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(vbox), lbl_title);
    gtk_box_append(GTK_BOX(vbox), lbl_sub);

    gtk_box_append(GTK_BOX(hbox), icon);
    gtk_box_append(GTK_BOX(hbox), vbox);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);

    return btn;
}

GtkWidget *
create_simple_item_row(const char *icon_name, const char *title, const char *subtitle, GtkWidget *action_btn)
{
    return create_action_row(icon_name, title, subtitle, action_btn);
}

GtkWidget *
create_action_button(const char *label, const char *payload_key, const char *payload_val, GCallback callback, gpointer user_data)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "flat");

    if (payload_key && payload_val) {
        g_object_set_data_full(G_OBJECT(btn), payload_key, g_strdup(payload_val), g_free);
    }

    if (callback) {
        g_signal_connect(btn, "clicked", callback, user_data);
    }

    return btn;
}

GtkWidget *
create_empty_state_label(const char *message)
{
    GtkWidget *lbl = gtk_label_new(message);
    gtk_widget_add_css_class(lbl, "empty-state-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    return lbl;
}

gboolean
is_mac_address_string(const char *str)
{
    if (!str || strlen(str) != 17) return FALSE;
    return (str[2] == ':' && str[5] == ':' && str[8] == ':' && str[11] == ':' && str[14] == ':');
}
