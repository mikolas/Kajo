#include "widget.h"
#include "../notifications/daemon.h"
#include "../compositor/compositor.h"
#include "../shell.h"
#include "../icons.h"

typedef struct {
    ShellWidget base;
    GtkWidget  *button;
    GtkWidget  *box;
    GtkWidget  *icon_img;
    GtkWidget  *badge_label;
    GtkWidget  *popover;
    GtkWidget  *list_vbox;
} NotificationsWidget;

static void rebuild_notification_list(NotificationsWidget *nw);

static void
on_clear_all_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    NotificationsWidget *nw = user_data;
    if (nw->base.app && shell_app_get_notification_daemon(nw->base.app)) {
        shell_notification_daemon_clear_history(shell_app_get_notification_daemon(nw->base.app));
    }
}

static void
on_remove_item_clicked(GtkButton *btn, gpointer user_data)
{
    NotificationsWidget *nw = user_data;
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "notification-id"));
    if (id > 0 && nw->base.app && shell_app_get_notification_daemon(nw->base.app)) {
        shell_notification_daemon_remove_item(shell_app_get_notification_daemon(nw->base.app), id);
    }
}

static void
on_card_pressed(GtkGestureClick *gesture, gint n_press G_GNUC_UNUSED, gdouble x G_GNUC_UNUSED, gdouble y G_GNUC_UNUSED, gpointer user_data)
{
    NotificationsWidget *nw = user_data;
    GtkWidget *card = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!card || !nw->base.app) return;

    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(card), "notification-id"));
    const char *app_name = g_object_get_data(G_OBJECT(card), "app-name");
    const char *desktop_entry = g_object_get_data(G_OBJECT(card), "desktop-entry");
    const char *app_icon = g_object_get_data(G_OBJECT(card), "app-icon");

    if (id == 0) return;

    ShellNotificationDaemon *daemon = shell_app_get_notification_daemon(nw->base.app);
    if (daemon) {
        /* 1. Emit Freedesktop ActionInvoked D-Bus signal */
        shell_notification_daemon_emit_action_invoked(daemon, id, "default");
        shell_notification_daemon_emit_notification_closed(daemon, id, 2 /* user dismissed */);
    }

    /* 2. Focus matching application in Niri compositor */
    ShellCompositor *compositor = shell_app_get_compositor(nw->base.app);
    if (compositor) {
        shell_compositor_focus_app(compositor, app_name, desktop_entry, app_icon);
    }

    /* Dismiss notification and popover */
    if (nw->popover) {
        gtk_popover_popdown(GTK_POPOVER(nw->popover));
    }
    if (daemon) {
        shell_notification_daemon_remove_item(daemon, id);
    }
}

static void
rebuild_notification_list(NotificationsWidget *nw)
{
    if (!nw->list_vbox || !nw->base.app) return;

    /* Clear existing rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(nw->list_vbox)) != NULL) {
        gtk_box_remove(GTK_BOX(nw->list_vbox), child);
    }

    ShellNotificationDaemon *daemon = shell_app_get_notification_daemon(nw->base.app);
    const GPtrArray *history = daemon ? shell_notification_daemon_get_history(daemon) : NULL;

    guint count = history ? history->len : 0;
    if (count > 0) {
        gchar *cnt_str = g_strdup_printf("%u", count);
        gtk_label_set_text(GTK_LABEL(nw->badge_label), cnt_str);
        gtk_widget_set_visible(nw->badge_label, TRUE);
        g_free(cnt_str);
    } else {
        gtk_widget_set_visible(nw->badge_label, FALSE);
    }

    if (!history || history->len == 0) {
        GtkWidget *empty_lbl = gtk_label_new("No new notifications");
        gtk_widget_add_css_class(empty_lbl, "shell-popover-section-title");
        gtk_widget_set_margin_top(empty_lbl, 20);
        gtk_widget_set_margin_bottom(empty_lbl, 20);
        gtk_box_append(GTK_BOX(nw->list_vbox), empty_lbl);
        return;
    }

    for (guint i = 0; i < history->len; i++) {
        const ShellNotificationItem *item = g_ptr_array_index(history, i);

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(card, "shell-osd-card");
        gtk_widget_set_margin_bottom(card, 4);

        GtkGesture *click = gtk_gesture_click_new();
        g_object_set_data(G_OBJECT(card), "notification-id", GUINT_TO_POINTER(item->id));
        g_object_set_data_full(G_OBJECT(card), "app-name", g_strdup(item->app_name), g_free);
        g_object_set_data_full(G_OBJECT(card), "desktop-entry", g_strdup(item->desktop_entry), g_free);
        g_object_set_data_full(G_OBJECT(card), "app-icon", g_strdup(item->app_icon), g_free);
        g_signal_connect(click, "pressed", G_CALLBACK(on_card_pressed), nw);
        gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(click));

        /* Icon */
        GtkWidget *icon = shell_icons_create_app_icon_widget(item->app_icon && *item->app_icon ? item->app_icon : "preferences-system-notifications-symbolic", 24);

        /* Text box */
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(vbox, TRUE);

        /* Header row: App Name + Timestamp */
        GtkWidget *hdr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *app_lbl = gtk_label_new(item->app_name ? item->app_name : "System");
        gtk_widget_add_css_class(app_lbl, "shell-popover-section-title");
        gtk_widget_set_halign(app_lbl, GTK_ALIGN_START);

        GtkWidget *time_lbl = gtk_label_new(item->timestamp ? item->timestamp : "");
        gtk_widget_add_css_class(time_lbl, "shell-popover-header");
        gtk_widget_set_halign(time_lbl, GTK_ALIGN_END);
        gtk_widget_set_hexpand(time_lbl, TRUE);

        gtk_box_append(GTK_BOX(hdr_box), app_lbl);
        gtk_box_append(GTK_BOX(hdr_box), time_lbl);

        /* Summary */
        GtkWidget *sum_lbl = gtk_label_new(item->summary ? item->summary : "");
        gtk_widget_add_css_class(sum_lbl, "shell-popover-title");
        gtk_widget_set_halign(sum_lbl, GTK_ALIGN_START);
        gtk_label_set_wrap(GTK_LABEL(sum_lbl), TRUE);

        /* Body */
        GtkWidget *body_lbl = gtk_label_new(item->body ? item->body : "");
        gtk_widget_set_halign(body_lbl, GTK_ALIGN_START);
        gtk_label_set_wrap(GTK_LABEL(body_lbl), TRUE);

        gtk_box_append(GTK_BOX(vbox), hdr_box);
        gtk_box_append(GTK_BOX(vbox), sum_lbl);
        if (item->body && *item->body) {
            gtk_box_append(GTK_BOX(vbox), body_lbl);
        }

        /* Dismiss button X */
        GtkWidget *del_btn = gtk_button_new_with_label("✕");
        gtk_widget_add_css_class(del_btn, "flat");
        g_object_set_data(G_OBJECT(del_btn), "notification-id", GUINT_TO_POINTER(item->id));
        g_signal_connect(del_btn, "clicked", G_CALLBACK(on_remove_item_clicked), nw);

        gtk_box_append(GTK_BOX(card), icon);
        gtk_box_append(GTK_BOX(card), vbox);
        gtk_box_append(GTK_BOX(card), del_btn);

        gtk_box_append(GTK_BOX(nw->list_vbox), card);
    }
}

static void
on_notification_history_changed(ShellNotificationDaemon *daemon, gpointer user_data)
{
    NotificationsWidget *nw = user_data;
    rebuild_notification_list(nw);
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellNotificationsPopover {
    GtkPopover parent_instance;

    GtkWidget *list_vbox;
    GtkWidget *clear_btn;
} ShellNotificationsPopover;

typedef struct _ShellNotificationsPopoverClass {
    GtkPopoverClass parent_class;
} ShellNotificationsPopoverClass;

G_DEFINE_TYPE(ShellNotificationsPopover, shell_notifications_popover, GTK_TYPE_POPOVER)

static void
shell_notifications_popover_init(ShellNotificationsPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_notifications_popover_class_init(ShellNotificationsPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/notifications_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellNotificationsPopover, list_vbox);
    gtk_widget_class_bind_template_child(widget_class, ShellNotificationsPopover, clear_btn);
}

static GtkWidget *
build_notifications_popover(NotificationsWidget *nw)
{
    ShellNotificationsPopover *popover = g_object_new(shell_notifications_popover_get_type(), NULL);

    nw->list_vbox = popover->list_vbox;
    g_signal_connect(popover->clear_btn, "clicked", G_CALLBACK(on_clear_all_clicked), nw);

    return GTK_WIDGET(popover);
}

static ShellWidget *
notifications_widget_create(ShellCompositor *compositor)
{
    NotificationsWidget *nw = g_new0(NotificationsWidget, 1);
    nw->base.mode = WIDGET_MODE_ICON;

    nw->button = gtk_menu_button_new();
    gtk_widget_add_css_class(nw->button, "shell-widget");
    gtk_widget_add_css_class(nw->button, "shell-widget-notifications");

    nw->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    nw->icon_img = shell_icons_create_app_icon_widget("preferences-system-notifications-symbolic", 16);

    nw->badge_label = gtk_label_new("");
    gtk_widget_add_css_class(nw->badge_label, "shell-widget-label");
    gtk_widget_set_visible(nw->badge_label, FALSE);

    gtk_box_append(GTK_BOX(nw->box), nw->icon_img);
    gtk_box_append(GTK_BOX(nw->box), nw->badge_label);

    gtk_menu_button_set_child(GTK_MENU_BUTTON(nw->button), nw->box);

    nw->popover = build_notifications_popover(nw);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(nw->button), nw->popover);

    return (ShellWidget *)nw;
}

static void
notifications_widget_destroy(ShellWidget *widget)
{
    NotificationsWidget *nw = (NotificationsWidget *)widget;
    if (nw) {
        if (nw->base.app && shell_app_get_notification_daemon(nw->base.app)) {
            shell_notification_daemon_remove_callbacks(
                shell_app_get_notification_daemon(nw->base.app), nw);
        }
        g_free(nw);
    }
}

static GtkWidget *
notifications_widget_get_widget(ShellWidget *widget)
{
    NotificationsWidget *nw = (NotificationsWidget *)widget;
    return nw->button;
}

static void
notifications_widget_enable(ShellWidget *widget)
{
    NotificationsWidget *nw = (NotificationsWidget *)widget;
    if (nw->base.app && shell_app_get_notification_daemon(nw->base.app)) {
        shell_notification_daemon_on_history_changed(
            shell_app_get_notification_daemon(nw->base.app),
            on_notification_history_changed, nw);
        rebuild_notification_list(nw);
    }
}

static void
notifications_widget_disable(ShellWidget *widget)
{
}

const ShellWidgetClass notifications_widget_class = {
    .id = "notifications",
    .name = "Notification Center",
    .create = notifications_widget_create,
    .destroy = notifications_widget_destroy,
    .get_widget = notifications_widget_get_widget,
    .enable = notifications_widget_enable,
    .disable = notifications_widget_disable,
};
