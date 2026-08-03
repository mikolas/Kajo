#ifndef SHELL_NOTIFICATIONS_DAEMON_H
#define SHELL_NOTIFICATIONS_DAEMON_H

#include <gio/gio.h>
#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellNotificationDaemon ShellNotificationDaemon;

typedef struct {
    guint32 id;
    gchar *app_name;
    gchar *desktop_entry;
    gchar *summary;
    gchar *body;
    gchar *app_icon;
    gchar *timestamp;
} ShellNotificationItem;

typedef void (*NotificationHistoryCallback)(ShellNotificationDaemon *daemon, gpointer user_data);

ShellNotificationDaemon *shell_notification_daemon_new(void);
void                     shell_notification_daemon_destroy(ShellNotificationDaemon *daemon);

const GPtrArray         *shell_notification_daemon_get_history(ShellNotificationDaemon *daemon);
void                     shell_notification_daemon_clear_history(ShellNotificationDaemon *daemon);
void                     shell_notification_daemon_remove_item(ShellNotificationDaemon *daemon, guint32 id);
void                     shell_notification_daemon_on_history_changed(ShellNotificationDaemon *daemon, NotificationHistoryCallback cb, gpointer user_data);
void                     shell_notification_daemon_remove_callbacks(ShellNotificationDaemon *daemon, gpointer user_data);
void                     shell_notification_daemon_set_toast_timeout(ShellNotificationDaemon *daemon, gint timeout_ms);
void                     shell_notification_daemon_set_app(ShellNotificationDaemon *daemon, gpointer app_ptr);
void                     shell_notification_daemon_emit_action_invoked(ShellNotificationDaemon *daemon, guint32 id, const gchar *action_key);
void                     shell_notification_daemon_emit_notification_closed(ShellNotificationDaemon *daemon, guint32 id, guint32 reason);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_NOTIFICATIONS_DAEMON_H */
