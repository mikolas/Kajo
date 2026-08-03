#include "daemon.h"
#include "../compositor/compositor.h"
#include "../shell.h"
#include <gtk4-layer-shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTIFICATIONS_BUS_NAME "org.freedesktop.Notifications"
#define NOTIFICATIONS_OBJECT_PATH "/org/freedesktop/Notifications"
#define NOTIFICATIONS_INTERFACE "org.freedesktop.Notifications"

typedef struct {
    guint32 id;
    gchar *app_name;
    gchar *desktop_entry;
    gchar *summary;
    gchar *body;
    gchar *app_icon;
    gint32 expire_timeout;
    ShellNotificationDaemon *daemon;
    GtkWidget *toast_window;
    guint timer_id;
} ShellNotificationToast;

struct _ShellNotificationDaemon {
    GDBusConnection *bus;
    guint owner_id;
    guint reg_id;
    guint32 next_id;
    gint toast_timeout_ms;
    gpointer app_ptr;               /* ShellApp pointer */
    GPtrArray *toasts;
    GPtrArray *history;             /* ShellNotificationItem */
    GArray    *history_callbacks;   /* CallbackEntry */
};

static const gchar notifications_introspection_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.Notifications'>"
    "    <method name='Notify'>"
    "      <arg type='s' name='app_name' direction='in'/>"
    "      <arg type='u' name='replaces_id' direction='in'/>"
    "      <arg type='s' name='app_icon' direction='in'/>"
    "      <arg type='s' name='summary' direction='in'/>"
    "      <arg type='s' name='body' direction='in'/>"
    "      <arg type='as' name='actions' direction='in'/>"
    "      <arg type='a{sv}' name='hints' direction='in'/>"
    "      <arg type='i' name='expire_timeout' direction='in'/>"
    "      <arg type='u' name='id' direction='out'/>"
    "    </method>"
    "    <method name='CloseNotification'>"
    "      <arg type='u' name='id' direction='in'/>"
    "    </method>"
    "    <method name='GetCapabilities'>"
    "      <arg type='as' name='capabilities' direction='out'/>"
    "    </method>"
    "    <method name='GetServerInformation'>"
    "      <arg type='s' name='name' direction='out'/>"
    "      <arg type='s' name='vendor' direction='out'/>"
    "      <arg type='s' name='version' direction='out'/>"
    "      <arg type='s' name='spec_version' direction='out'/>"
    "    </method>"
    "    <signal name='NotificationClosed'>"
    "      <arg type='u' name='id'/>"
    "      <arg type='u' name='reason'/>"
    "    </signal>"
    "    <signal name='ActionInvoked'>"
    "      <arg type='u' name='id'/>"
    "      <arg type='s' name='action_key'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static void
toast_free(ShellNotificationToast *t)
{
    if (!t) return;
    if (t->timer_id != 0) {
        g_source_remove(t->timer_id);
        t->timer_id = 0;
    }
    if (t->toast_window) {
        gtk_widget_set_visible(GTK_WIDGET(t->toast_window), FALSE);
        gtk_window_destroy(GTK_WINDOW(t->toast_window));
        t->toast_window = NULL;
    }
    g_free(t->app_name);
    g_free(t->desktop_entry);
    g_free(t->summary);
    g_free(t->body);
    g_free(t->app_icon);
    g_free(t);
}

static gboolean
on_toast_expire_timeout(gpointer user_data)
{
    ShellNotificationToast *toast = user_data;
    if (toast) {
        toast->timer_id = 0;
        if (toast->daemon) {
            shell_notification_daemon_emit_notification_closed(toast->daemon, toast->id, 1 /* expired */);
            if (toast->daemon->toasts) {
                g_ptr_array_remove(toast->daemon->toasts, toast);
            }
        }
        toast_free(toast);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
on_toast_idle_free(gpointer user_data)
{
    ShellNotificationToast *toast = user_data;
    if (toast) {
        toast_free(toast);
    }
    return G_SOURCE_REMOVE;
}

static void
on_toast_card_pressed(GtkGestureClick *gesture G_GNUC_UNUSED, gint n_press G_GNUC_UNUSED, gdouble x G_GNUC_UNUSED, gdouble y G_GNUC_UNUSED, gpointer user_data)
{
    ShellNotificationToast *toast = user_data;
    if (!toast) return;

    if (toast->timer_id) {
        g_source_remove(toast->timer_id);
        toast->timer_id = 0;
    }

    if (toast->toast_window) {
        gtk_widget_set_visible(toast->toast_window, FALSE);
    }

    ShellNotificationDaemon *daemon = toast->daemon;

    if (daemon) {
        /* 1. Emit Freedesktop ActionInvoked D-Bus signal so Teams, Chrome, Firefox, Telegram handle app routing */
        shell_notification_daemon_emit_action_invoked(daemon, toast->id, "default");
        shell_notification_daemon_emit_notification_closed(daemon, toast->id, 2 /* user dismissed */);

        /* 2. Compositor window focus */
        if (daemon->app_ptr) {
            ShellApp *app = (ShellApp *)daemon->app_ptr;
            ShellCompositor *compositor = shell_app_get_compositor(app);
            if (compositor) {
                shell_compositor_focus_app(compositor, toast->app_name, toast->desktop_entry, toast->app_icon);
            }
        }
        if (daemon->toasts) {
            g_ptr_array_remove(daemon->toasts, toast);
        }
    }

    g_idle_add(on_toast_idle_free, toast);
}

static void
create_notification_toast_card(ShellNotificationDaemon *daemon, ShellNotificationToast *toast)
{
    toast->toast_window = gtk_window_new();
    gtk_widget_add_css_class(toast->toast_window, "shell-notification-toast");

    /* Wayland Layer-Shell Top-Right Overlay */
    gtk_layer_init_for_window(GTK_WINDOW(toast->toast_window));
    gtk_layer_set_layer(GTK_WINDOW(toast->toast_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(toast->toast_window), "kajo-notification");
    gtk_layer_set_anchor(GTK_WINDOW(toast->toast_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(toast->toast_window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(toast->toast_window), GTK_LAYER_SHELL_EDGE_TOP, 48);
    gtk_layer_set_margin(GTK_WINDOW(toast->toast_window), GTK_LAYER_SHELL_EDGE_RIGHT, 16);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(toast->toast_window), 0);

    /* Notification Card Layout */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(card, "shell-notification-card");
    gtk_widget_set_size_request(card, 300, -1);

    /* Icon */
    const gchar *icon_name = (toast->app_icon && *toast->app_icon) ? toast->app_icon : "preferences-system-notifications-symbolic";
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);

    /* Text Column */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(vbox, TRUE);

    GtkWidget *app_lbl = gtk_label_new(toast->app_name ? toast->app_name : "System");
    gtk_widget_add_css_class(app_lbl, "shell-popover-section-title");
    gtk_widget_set_halign(app_lbl, GTK_ALIGN_START);

    GtkWidget *summary_lbl = gtk_label_new(toast->summary ? toast->summary : "");
    gtk_widget_add_css_class(summary_lbl, "shell-popover-header");
    gtk_widget_set_halign(summary_lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(summary_lbl), TRUE);

    GtkWidget *body_lbl = gtk_label_new(toast->body ? toast->body : "");
    gtk_widget_set_halign(body_lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(body_lbl), TRUE);

    gtk_box_append(GTK_BOX(vbox), app_lbl);
    gtk_box_append(GTK_BOX(vbox), summary_lbl);
    if (toast->body && *toast->body) {
        gtk_box_append(GTK_BOX(vbox), body_lbl);
    }

    gtk_box_append(GTK_BOX(card), icon);
    gtk_box_append(GTK_BOX(card), vbox);

    /* Click Gesture Controller */
    GtkGestureClick *click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    g_signal_connect(click, "pressed", G_CALLBACK(on_toast_card_pressed), toast);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(click));

    gtk_window_set_child(GTK_WINDOW(toast->toast_window), card);
    gtk_window_present(GTK_WINDOW(toast->toast_window));

    gint timeout = (toast->expire_timeout > 0) ? toast->expire_timeout :
                   (daemon->toast_timeout_ms > 0 ? daemon->toast_timeout_ms : 8000);
    toast->timer_id = g_timeout_add(timeout, on_toast_expire_timeout, toast);
}

static void
handle_notifications_method_call(GDBusConnection *connection,
                                 const gchar *sender,
                                 const gchar *object_path,
                                 const gchar *interface_name,
                                 const gchar *method_name,
                                 GVariant *parameters,
                                 GDBusMethodInvocation *invocation,
                                 gpointer user_data)
{
    ShellNotificationDaemon *daemon = user_data;

    if (g_strcmp0(method_name, "Notify") == 0) {
        const gchar *app_name, *app_icon, *summary, *body;
        guint32 replaces_id;
        gint32 expire_timeout;
        GVariantIter *actions_iter;
        GVariantIter *hints_iter;

        g_variant_get(parameters, "(&su&s&s&sasa{sv}i)",
                      &app_name, &replaces_id, &app_icon, &summary, &body,
                      &actions_iter, &hints_iter, &expire_timeout);

        const gchar *key;
        GVariant *val;
        gchar *desktop_entry = NULL;
        while (g_variant_iter_next(hints_iter, "{&sv}", &key, &val)) {
            if (g_strcmp0(key, "desktop-entry") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
                if (desktop_entry) g_free(desktop_entry);
                desktop_entry = g_variant_dup_string(val, NULL);
            }
            g_variant_unref(val);
        }

        guint32 id = (replaces_id != 0) ? replaces_id : ++daemon->next_id;

        if (replaces_id != 0) {
            shell_notification_daemon_remove_item(daemon, replaces_id);
        }

        ShellNotificationToast *toast = g_new0(ShellNotificationToast, 1);
        toast->id = id;
        toast->app_name = g_strdup(app_name);
        toast->desktop_entry = g_strdup(desktop_entry);
        toast->summary = g_strdup(summary);
        toast->body = g_strdup(body);
        toast->app_icon = g_strdup(app_icon);
        toast->expire_timeout = expire_timeout;
        toast->daemon = daemon;

        g_ptr_array_add(daemon->toasts, toast);
        create_notification_toast_card(daemon, toast);

        /* Add to history stack */
        ShellNotificationItem *item = g_new0(ShellNotificationItem, 1);
        item->id = id;
        item->app_name = g_strdup(app_name);
        item->desktop_entry = g_strdup(desktop_entry);
        item->summary = g_strdup(summary);
        item->body = g_strdup(body);
        item->app_icon = g_strdup(app_icon);
        if (desktop_entry) g_free(desktop_entry);

        GDateTime *now = g_date_time_new_now_local();
        item->timestamp = g_date_time_format(now, "%H:%M");
        g_date_time_unref(now);

        g_ptr_array_insert(daemon->history, 0, item); /* Most recent first */

        /* Emit history callbacks */
        if (daemon->history_callbacks) {
            for (guint i = 0; i < daemon->history_callbacks->len; i++) {
                typedef struct { gpointer cb; gpointer ud; } CBEntry;
                CBEntry *e = &g_array_index(daemon->history_callbacks, CBEntry, i);
                ((NotificationHistoryCallback)e->cb)(daemon, e->ud);
            }
        }

        g_variant_iter_free(actions_iter);
        g_variant_iter_free(hints_iter);

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
    } else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&builder, "s", "body");
        g_variant_builder_add(&builder, "s", "actions");
        g_variant_builder_add(&builder, "s", "icon-static");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
    } else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(ssss)", "kajo", "mikolas", "0.1.0", "1.2"));
    } else if (g_strcmp0(method_name, "CloseNotification") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable notifications_vtable = {
    .method_call = handle_notifications_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

static void
on_notifications_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    ShellNotificationDaemon *daemon = user_data;
    daemon->bus = connection;

    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(notifications_introspection_xml, NULL);
    if (node_info && node_info->interfaces) {
        daemon->reg_id = g_dbus_connection_register_object(
            connection, NOTIFICATIONS_OBJECT_PATH, node_info->interfaces[0],
            &notifications_vtable, daemon, NULL, NULL);
    }
    if (node_info) g_dbus_node_info_unref(node_info);
}

static void notification_item_free(ShellNotificationItem *item);

ShellNotificationDaemon *
shell_notification_daemon_new(void)
{
    ShellNotificationDaemon *daemon = g_new0(ShellNotificationDaemon, 1);
    daemon->next_id = 1;
    daemon->toasts = g_ptr_array_new();
    daemon->history = g_ptr_array_new_with_free_func((GDestroyNotify)notification_item_free);
    typedef struct { gpointer cb; gpointer ud; } CBEntry;
    daemon->history_callbacks = g_array_new(FALSE, FALSE, sizeof(CBEntry));

    daemon->owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION, NOTIFICATIONS_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_REPLACE | G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
        on_notifications_bus_acquired, NULL, NULL, daemon, NULL);

    return daemon;
}

static void
notification_item_free(ShellNotificationItem *item)
{
    if (!item) return;
    g_free(item->app_name);
    g_free(item->desktop_entry);
    g_free(item->summary);
    g_free(item->body);
    g_free(item->app_icon);
    g_free(item->timestamp);
    g_free(item);
}

void
shell_notification_daemon_destroy(ShellNotificationDaemon *daemon)
{
    if (!daemon) return;

    if (daemon->owner_id != 0) {
        g_bus_unown_name(daemon->owner_id);
        daemon->owner_id = 0;
    }
    if (daemon->reg_id > 0 && daemon->bus) {
        g_dbus_connection_unregister_object(daemon->bus, daemon->reg_id);
        daemon->reg_id = 0;
    }

    if (daemon->toasts) {
        for (guint i = 0; i < daemon->toasts->len; i++) {
            ShellNotificationToast *t = g_ptr_array_index(daemon->toasts, i);
            toast_free(t);
        }
        g_ptr_array_free(daemon->toasts, TRUE);
    }

    if (daemon->history) {
        g_ptr_array_free(daemon->history, TRUE);
    }

    if (daemon->history_callbacks) {
        g_array_free(daemon->history_callbacks, TRUE);
    }

    g_free(daemon);
}

const GPtrArray *
shell_notification_daemon_get_history(ShellNotificationDaemon *daemon)
{
    return daemon ? daemon->history : NULL;
}

void
shell_notification_daemon_clear_history(ShellNotificationDaemon *daemon)
{
    if (!daemon || !daemon->history) return;

    g_ptr_array_set_size(daemon->history, 0);

    /* Emit callbacks */
    if (daemon->history_callbacks) {
        for (guint i = 0; i < daemon->history_callbacks->len; i++) {
            typedef struct { gpointer cb; gpointer ud; } CBEntry;
            CBEntry *e = &g_array_index(daemon->history_callbacks, CBEntry, i);
            ((NotificationHistoryCallback)e->cb)(daemon, e->ud);
        }
    }
}

void
shell_notification_daemon_remove_item(ShellNotificationDaemon *daemon, guint32 id)
{
    if (!daemon || !daemon->history) return;

    for (guint i = 0; i < daemon->history->len; i++) {
        ShellNotificationItem *item = g_ptr_array_index(daemon->history, i);
        if (item->id == id) {
            g_ptr_array_remove_index(daemon->history, i);
            break;
        }
    }

    /* Emit callbacks */
    if (daemon->history_callbacks) {
        for (guint i = 0; i < daemon->history_callbacks->len; i++) {
            typedef struct { gpointer cb; gpointer ud; } CBEntry;
            CBEntry *e = &g_array_index(daemon->history_callbacks, CBEntry, i);
            ((NotificationHistoryCallback)e->cb)(daemon, e->ud);
        }
    }
}

void
shell_notification_daemon_on_history_changed(ShellNotificationDaemon *daemon, NotificationHistoryCallback cb, gpointer user_data)
{
    if (!daemon || !cb) return;
    typedef struct { gpointer cb; gpointer ud; } CBEntry;
    CBEntry entry = { (gpointer)cb, user_data };
    g_array_append_val(daemon->history_callbacks, entry);
}

void
shell_notification_daemon_remove_callbacks(ShellNotificationDaemon *daemon, gpointer user_data)
{
    if (!daemon || !daemon->history_callbacks || !user_data) return;
    for (guint i = 0; i < daemon->history_callbacks->len; ) {
        typedef struct { gpointer cb; gpointer ud; } CBEntry;
        CBEntry *e = &g_array_index(daemon->history_callbacks, CBEntry, i);
        if (e->ud == user_data) {
            g_array_remove_index(daemon->history_callbacks, i);
        } else {
            i++;
        }
    }
}

void
shell_notification_daemon_set_toast_timeout(ShellNotificationDaemon *daemon, gint timeout_ms)
{
    if (daemon) {
        daemon->toast_timeout_ms = timeout_ms;
    }
}

void
shell_notification_daemon_set_app(ShellNotificationDaemon *daemon, gpointer app_ptr)
{
    if (daemon) {
        daemon->app_ptr = app_ptr;
    }
}

void
shell_notification_daemon_emit_action_invoked(ShellNotificationDaemon *daemon, guint32 id, const gchar *action_key)
{
    if (!daemon || !daemon->bus) return;
    g_dbus_connection_emit_signal(
        daemon->bus, NULL,
        NOTIFICATIONS_OBJECT_PATH,
        NOTIFICATIONS_BUS_NAME,
        "ActionInvoked",
        g_variant_new("(us)", id, action_key ? action_key : "default"),
        NULL);
}

void
shell_notification_daemon_emit_notification_closed(ShellNotificationDaemon *daemon, guint32 id, guint32 reason)
{
    if (!daemon || !daemon->bus) return;
    g_dbus_connection_emit_signal(
        daemon->bus, NULL,
        NOTIFICATIONS_OBJECT_PATH,
        NOTIFICATIONS_BUS_NAME,
        "NotificationClosed",
        g_variant_new("(uu)", id, reason),
        NULL);
}
