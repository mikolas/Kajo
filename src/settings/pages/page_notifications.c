#include "../settings_common.h"
#include "../settings_pages.h"

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageNotifications {
    GtkBox parent_instance;

    GtkWidget *scale_tout;
    GtkWidget *lbl_tout_val;
    GtkWidget *combo_click;
    GtkWidget *scale_hist;
    GtkWidget *lbl_hist_val;
    GtkWidget *btn_save;
} SettingsPageNotifications;

typedef struct _SettingsPageNotificationsClass {
    GtkBoxClass parent_class;
} SettingsPageNotificationsClass;

G_DEFINE_TYPE(SettingsPageNotifications, settings_page_notifications, GTK_TYPE_BOX)

static void
settings_page_notifications_init(SettingsPageNotifications *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_notifications_class_init(SettingsPageNotificationsClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_notifications.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageNotifications, scale_tout);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNotifications, lbl_tout_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNotifications, combo_click);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNotifications, scale_hist);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNotifications, lbl_hist_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNotifications, btn_save);
}

static void
on_scale_tout_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%dms (%ds)", (int)gtk_range_get_value(range), (int)gtk_range_get_value(range) / 1000);
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_scale_hist_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%d items", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_save_notifications_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    g_message("Saved notification settings.");
}

GtkWidget *
build_page_notifications(void)
{
    SettingsPageNotifications *page = g_object_new(settings_page_notifications_get_type(), NULL);

    const char *click_items[] = {
        "Focus Source App Window in Niri (Default)",
        "Dismiss Notification Only",
        "Do Nothing",
        NULL
    };
    GtkStringList *slist = gtk_string_list_new(click_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_click), G_LIST_MODEL(slist));

    gtk_range_set_value(GTK_RANGE(page->scale_tout), 8000);
    g_signal_connect(page->scale_tout, "value-changed", G_CALLBACK(on_scale_tout_changed), page->lbl_tout_val);

    gtk_range_set_value(GTK_RANGE(page->scale_hist), 50);
    g_signal_connect(page->scale_hist, "value-changed", G_CALLBACK(on_scale_hist_changed), page->lbl_hist_val);

    g_signal_connect(page->btn_save, "clicked", G_CALLBACK(on_save_notifications_clicked), NULL);

    return create_settings_page_card("NOTIFICATION TOASTS & OS DAEMON", "[ NOTIFICATION DAEMON ]", GTK_WIDGET(page), NULL);
}
