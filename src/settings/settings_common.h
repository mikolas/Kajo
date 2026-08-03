#ifndef DESKTOP_SETTINGS_COMMON_H
#define DESKTOP_SETTINGS_COMMON_H

#include <gtk/gtk.h>
#include "../config.h"
#include "../theme.h"
#include "../compositor/compositor.h"

G_BEGIN_DECLS

typedef struct {
    GtkWidget *window;
    GtkWidget *sidebar_list;
    GtkWidget *stack;
    GtkWidget *search_entry;
    GtkWidget *back_button;
    GtkWidget *paned;
    GtkWidget *sidebar_vbox;
    gboolean   is_narrow_mode;
} SettingsApp;

GtkWidget *create_settings_page_card(const gchar *title_text, const gchar *badge_text, GtkWidget *body_widget, GtkWidget *footer_widget);
GtkWidget *create_sidebar_category_header(const gchar *category_name);
GtkWidget *create_sidebar_row(const gchar *icon_name, const gchar *label_text, const gchar *subtitle_text, const gchar *page_name);
GtkWidget *create_action_row(const gchar *icon_name, const gchar *title, const gchar *subtitle, GtkWidget *control);
GtkWidget *create_string_dropdown(const char *const items[]);
GtkWidget *create_metro_toggle_tile(const gchar *icon_name, const gchar *title, const gchar *subtitle, gboolean active);
GtkWidget *create_simple_item_row(const char *icon_name, const char *title, const char *subtitle, GtkWidget *action_btn);
GtkWidget *create_action_button(const char *label, const char *payload_key, const char *payload_val, GCallback callback, gpointer user_data);
GtkWidget *create_empty_state_label(const char *message);

gboolean is_mac_address_string(const char *str);

G_END_DECLS

#endif /* DESKTOP_SETTINGS_COMMON_H */
