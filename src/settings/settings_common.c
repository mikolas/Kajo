#include "settings_common.h"
#include "../ipc/ipc_client.h"
#include <gio/gunixsocketaddress.h>
#include <string.h>

GtkWidget *
create_settings_page_card(const gchar *title_text, const gchar *badge_text, GtkWidget *body_widget, GtkWidget *footer_widget)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "shell-popover-card");

    /* Zone 1: Header */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "shell-popover-header");
    gtk_widget_set_valign(header, GTK_ALIGN_CENTER);

    GtkWidget *lbl_title = gtk_label_new(title_text);
    gtk_widget_add_css_class(lbl_title, "heading");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl_title, GTK_ALIGN_CENTER);

    GtkWidget *lbl_badge = gtk_label_new(badge_text);
    gtk_widget_add_css_class(lbl_badge, "shell-popover-badge");
    gtk_widget_set_halign(lbl_badge, GTK_ALIGN_END);
    gtk_widget_set_valign(lbl_badge, GTK_ALIGN_CENTER);
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
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_add_css_class(label, "sidebar-label");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(vbox), label);

    if (subtitle_text && subtitle_text[0] != '\0') {
        GtkWidget *sub = gtk_label_new(subtitle_text);
        gtk_widget_add_css_class(sub, "shell-popover-subtitle");
        gtk_widget_set_halign(sub, GTK_ALIGN_START);
        gtk_widget_set_valign(sub, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(vbox), sub);
    }

    GtkWidget *chevron = gtk_label_new("›");
    gtk_widget_add_css_class(chevron, "sidebar-chevron");
    gtk_widget_set_valign(chevron, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(row), icon);
    gtk_box_append(GTK_BOX(row), vbox);
    gtk_box_append(GTK_BOX(row), chevron);

    g_object_set_data_full(G_OBJECT(row), "page-name", g_strdup(page_name), g_free);
    return row;
}

GtkWidget *
create_sidebar_row_with_keywords(const gchar *icon_name, const gchar *label_text, const gchar *subtitle_text, const gchar *page_name, const gchar *keywords)
{
    GtkWidget *row = create_sidebar_row(icon_name, label_text, subtitle_text, page_name);
    if (keywords) {
        g_object_set_data_full(G_OBJECT(row), "keywords", g_strdup(keywords), g_free);
    }
    if (label_text) {
        g_object_set_data_full(G_OBJECT(row), "label-text", g_strdup(label_text), g_free);
    }
    if (subtitle_text) {
        g_object_set_data_full(G_OBJECT(row), "subtitle-text", g_strdup(subtitle_text), g_free);
    }
    return row;
}

GtkWidget *
create_action_row(const gchar *icon_name, const gchar *title, const gchar *subtitle, GtkWidget *control)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "widget-row-card");
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, -1, 48);

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
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(lbl_title), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(lbl_title), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(vbox), lbl_title);

    if (subtitle && subtitle[0] != '\0') {
        GtkWidget *lbl_sub = gtk_label_new(subtitle);
        gtk_widget_add_css_class(lbl_sub, "shell-popover-subtitle");
        gtk_widget_set_halign(lbl_sub, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(lbl_sub), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(lbl_sub), PANGO_WRAP_WORD_CHAR);
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

/* ─── Toggle Card (gallery: "Toggle Cards") ───
 * A feature toggle with a status line and a 4px left indicator bar that lights
 * up while active (see .toggle-card in style.css). It is a real GtkToggleButton:
 * read the state with gtk_toggle_button_get_active(), react with "toggled", and
 * change the status text with toggle_card_set_subtitle(). Use a plain GtkSwitch
 * row (create_action_row) for simple preferences with no status to show. */
GtkWidget *
create_toggle_card(const gchar *icon_name, const gchar *title, const gchar *subtitle, gboolean active)
{
    GtkWidget *btn = gtk_toggle_button_new();
    gtk_widget_add_css_class(btn, "toggle-card");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), active);
    gtk_widget_set_hexpand(btn, TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl_title = gtk_label_new(title);
    gtk_widget_add_css_class(lbl_title, "tile-title");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl_title, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0f);

    GtkWidget *lbl_sub = gtk_label_new(subtitle);
    gtk_widget_add_css_class(lbl_sub, "tile-subtitle");
    gtk_widget_set_halign(lbl_sub, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl_sub, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(lbl_sub), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(lbl_sub), PANGO_WRAP_WORD_CHAR);
    g_object_set_data(G_OBJECT(btn), "tile-subtitle-label", lbl_sub);

    gtk_box_append(GTK_BOX(vbox), lbl_title);
    gtk_box_append(GTK_BOX(vbox), lbl_sub);
    gtk_box_append(GTK_BOX(hbox), icon);
    gtk_box_append(GTK_BOX(hbox), vbox);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);

    return btn;
}

void
toggle_card_set_subtitle(GtkWidget *card, const gchar *subtitle)
{
    GtkWidget *lbl = card ? g_object_get_data(G_OBJECT(card), "tile-subtitle-label") : NULL;
    if (lbl && GTK_IS_LABEL(lbl))
        gtk_label_set_text(GTK_LABEL(lbl), subtitle ? subtitle : "");
}

/* ─── Segmented Group (gallery §04) ───
 * 2-4 mutually exclusive choices as joined toggle buttons, replacing dropdowns
 * for short A/B/C choices. Selection is exclusive via a GtkToggleButton group. */

typedef struct {
    SegmentedChangedFunc cb;
    gpointer             user_data;
    guint                selected;
    gboolean             syncing;
} SegmentedState;

static void
on_segment_toggled(GtkToggleButton *btn, gpointer user_data)
{
    GtkWidget *group = GTK_WIDGET(user_data);
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    if (!st || !gtk_toggle_button_get_active(btn))
        return;

    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "segment-index"));
    if (idx == st->selected)
        return;
    st->selected = idx;
    if (!st->syncing && st->cb)
        st->cb(group, idx, st->user_data);
}

GtkWidget *
create_segmented_group(const char *const labels[], guint selected)
{
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(group, "segmented-group");
    gtk_widget_set_valign(group, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(group, GTK_ALIGN_END);

    SegmentedState *st = g_new0(SegmentedState, 1);
    st->selected = selected;
    g_object_set_data_full(G_OBJECT(group), "segmented-state", st, g_free);

    GtkWidget *first = NULL;
    for (guint i = 0; labels && labels[i]; i++) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(labels[i]);
        gtk_widget_add_css_class(btn, "files-nav-btn");
        gtk_widget_set_focus_on_click(btn, FALSE);
        g_object_set_data(G_OBJECT(btn), "segment-index", GUINT_TO_POINTER(i));
        if (first)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), GTK_TOGGLE_BUTTON(first));
        else
            first = btn;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), i == selected);
        g_signal_connect(btn, "toggled", G_CALLBACK(on_segment_toggled), group);
        gtk_box_append(GTK_BOX(group), btn);
    }
    return group;
}

void
segmented_group_connect(GtkWidget *group, SegmentedChangedFunc cb, gpointer user_data)
{
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    if (!st) return;
    st->cb = cb;
    st->user_data = user_data;
}

guint
segmented_group_get_selected(GtkWidget *group)
{
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    return st ? st->selected : 0;
}

void
segmented_group_set_selected(GtkWidget *group, guint idx)
{
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    if (!st) return;
    st->syncing = TRUE; /* programmatic change: do not call the user callback */
    for (GtkWidget *c = gtk_widget_get_first_child(group); c; c = gtk_widget_get_next_sibling(c)) {
        if (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(c), "segment-index")) == idx)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), TRUE);
    }
    st->selected = idx;
    st->syncing = FALSE;
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
    gtk_widget_add_css_class(btn, "files-nav-btn");

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

gchar *
format_mac_address_from_bluez_path(const char *path)
{
    if (!path) return g_strdup("--");
    const char *dev = strstr(path, "dev_");
    if (dev && strlen(dev) >= 21) {
        dev += 4; /* skip dev_ */
        return g_strdup_printf("%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
            dev[0], dev[1], dev[3], dev[4], dev[6], dev[7],
            dev[9], dev[10], dev[12], dev[13], dev[15], dev[16]);
    }
    return g_strdup(path);
}

gboolean
settings_send_ipc_command(const gchar *command)
{
    return shell_ipc_client_send(command);
}

static ShellTheme *g_shared_settings_theme = NULL;

void
settings_set_global_theme(ShellTheme *theme)
{
    g_shared_settings_theme = theme;
}

void
settings_reload_global_theme(void)
{
    if (g_shared_settings_theme) {
        shell_theme_reload(g_shared_settings_theme);
    }
}
