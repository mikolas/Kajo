#include "../src/widgets/registry.h"
#include "../src/shell.h"
#include "../src/panel.h"
#include "../src/autohide.h"
#include "../src/config.h"
#include "../src/osd/osd.h"
#include "../src/ipc/socket.h"
#include "../src/notifications/daemon.h"
#include "../src/launcher/launcher_surface.h"
#include "../src/lock/idle_manager.h"
#include <glib.h>

/* Stubs for shell_app_* entry points referenced by widget callbacks */
ShellConfig *
shell_app_get_config(ShellApp *self)
{
    return self ? self->config : NULL;
}

ShellCompositor *
shell_app_get_compositor(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

ShellOSD *
shell_app_get_osd(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_osd_show_volume(ShellOSD *osd G_GNUC_UNUSED, gint percent G_GNUC_UNUSED, gboolean muted G_GNUC_UNUSED)
{
}

gboolean
shell_ipc_send_command(const gchar *command G_GNUC_UNUSED)
{
    return FALSE;
}

ShellPanel *
shell_app_get_panel(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

ShellAutohide *
shell_panel_get_autohide(ShellPanel *panel G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_autohide_lock(ShellAutohide *self G_GNUC_UNUSED)
{
}

void
shell_autohide_unlock(ShellAutohide *self G_GNUC_UNUSED)
{
}

ShellNotificationDaemon *
shell_app_get_notification_daemon(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_app_lock_session(ShellApp *self G_GNUC_UNUSED)
{
}

void
shell_launcher_surface_toggle(ShellLauncherSurface *self G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_clear_history(ShellNotificationDaemon *daemon G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_remove_item(ShellNotificationDaemon *daemon G_GNUC_UNUSED, guint32 id G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_on_history_changed(ShellNotificationDaemon    *daemon G_GNUC_UNUSED,
                                             NotificationHistoryCallback cb G_GNUC_UNUSED,
                                             gpointer                    user_data G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_remove_callbacks(ShellNotificationDaemon *daemon G_GNUC_UNUSED,
                                           gpointer                 user_data G_GNUC_UNUSED)
{
}

const GPtrArray *
shell_notification_daemon_get_history(ShellNotificationDaemon *daemon G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_notification_daemon_emit_action_invoked(ShellNotificationDaemon *daemon G_GNUC_UNUSED,
                                              guint32                  id G_GNUC_UNUSED,
                                              const gchar             *action_key G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_emit_notification_closed(ShellNotificationDaemon *daemon G_GNUC_UNUSED,
                                                   guint32                  id G_GNUC_UNUSED,
                                                   guint32                  reason G_GNUC_UNUSED)
{
}

void
shell_idle_manager_set_inhibited(ShellIdleManager *self G_GNUC_UNUSED, gboolean inhibited G_GNUC_UNUSED)
{
}

gboolean
shell_idle_manager_get_inhibited(ShellIdleManager *self G_GNUC_UNUSED)
{
    return FALSE;
}

void
shell_idle_manager_add_listener(ShellIdleManager           *self G_GNUC_UNUSED,
                                ShellIdleInhibitChangedFunc callback G_GNUC_UNUSED,
                                gpointer                    user_data G_GNUC_UNUSED)
{
}

void
shell_idle_manager_remove_listener(ShellIdleManager           *self G_GNUC_UNUSED,
                                   ShellIdleInhibitChangedFunc callback G_GNUC_UNUSED,
                                   gpointer                    user_data G_GNUC_UNUSED)
{
}

static void
test_widget_registry_integrity(void)
{
    ShellWidgetRegistry *reg = shell_widget_registry_new();
    g_assert_nonnull(reg);
    g_assert_nonnull(reg->classes);
    g_assert_cmpuint(reg->classes->len, >=, 20);

    /* Rule #7: Every registered ShellWidgetClass MUST define non-NULL .id, .name, and vtable methods */
    for (guint i = 0; i < reg->classes->len; i++) {
        const ShellWidgetClass *cls = g_ptr_array_index(reg->classes, i);
        g_assert_nonnull(cls);
        g_assert_nonnull(cls->id);
        g_assert_cmpstr(cls->id, !=, "");
        g_assert_nonnull(cls->name);
        g_assert_cmpstr(cls->name, !=, "");
        g_assert_nonnull(cls->create);
        g_assert_nonnull(cls->destroy);
        g_assert_nonnull(cls->get_widget);
        g_assert_nonnull(cls->enable);
        g_assert_nonnull(cls->disable);

        /* Verify lookup by id returns the exact class */
        const ShellWidgetClass *found = shell_widget_registry_find(reg, cls->id);
        g_assert_true(found == cls);
    }

    /* Verify key built-in widgets are registered */
    g_assert_nonnull(shell_widget_registry_find(reg, "launcher"));
    g_assert_nonnull(shell_widget_registry_find(reg, "control-center"));
    g_assert_nonnull(shell_widget_registry_find(reg, "idle-inhibitor"));
    g_assert_nonnull(shell_widget_registry_find(reg, "power"));
    g_assert_nonnull(shell_widget_registry_find(reg, "clock"));
    g_assert_nonnull(shell_widget_registry_find(reg, "network"));
    g_assert_nonnull(shell_widget_registry_find(reg, "bluetooth"));
    g_assert_nonnull(shell_widget_registry_find(reg, "volume"));
    g_assert_null(shell_widget_registry_find(reg, "nonexistent-widget-id"));

    shell_widget_registry_destroy(reg);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/registry/widget-classes-integrity", test_widget_registry_integrity);

    return g_test_run();
}
