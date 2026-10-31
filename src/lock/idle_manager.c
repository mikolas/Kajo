#include "idle_manager.h"
#include "screensaver.h"
#include "session_lock.h"
#include "../shell.h"
#include "../config.h"

#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <wayland-client.h>
#include <unistd.h>
#include <string.h>

#include "ext-idle-notify-v1-client-protocol.h"

typedef struct {
    ShellIdleInhibitChangedFunc callback;
    gpointer                    user_data;
} InhibitListenerEntry;

struct _ShellIdleManager {
    ShellApp                        *app;
    struct wl_display               *wl_display;
    struct wl_registry              *wl_registry;
    struct wl_seat                  *wl_seat;
    struct ext_idle_notifier_v1     *idle_notifier;

    struct ext_idle_notification_v1 *screensaver_notif;
    struct ext_idle_notification_v1 *dpms_notif;
    struct ext_idle_notification_v1 *lock_notif;

    gboolean                         is_inhibited;
    int                              logind_inhibit_fd;
    GDBusConnection                 *system_bus;
    GPtrArray                       *listeners;
};

/* --- Wayland Idle Notification Callbacks --- */

static void
on_screensaver_idled(void                            *data,
                     struct ext_idle_notification_v1 *notification G_GNUC_UNUSED)
{
    ShellIdleManager *self = data;
    if (!self || self->is_inhibited || !self->app)
        return;

    /* Do not start screensaver overlay if session is already locked */
    if (self->app->session_lock && shell_session_lock_is_locked(self->app->session_lock))
        return;

    if (self->app->screensaver) {
        shell_screensaver_start(self->app->screensaver);
    }
}

static void
on_screensaver_resumed(void                            *data,
                       struct ext_idle_notification_v1 *notification G_GNUC_UNUSED)
{
    ShellIdleManager *self = data;
    if (!self || !self->app)
        return;

    if (self->app->screensaver && shell_screensaver_is_active(self->app->screensaver)) {
        shell_screensaver_stop(self->app->screensaver);
    } else {
        shell_screensaver_set_dpms(TRUE);
    }
}

static const struct ext_idle_notification_v1_listener screensaver_notif_listener = {
    .idled   = on_screensaver_idled,
    .resumed = on_screensaver_resumed,
};

static void
on_dpms_idled(void                            *data,
              struct ext_idle_notification_v1 *notification G_GNUC_UNUSED)
{
    ShellIdleManager *self = data;
    if (!self || self->is_inhibited)
        return;

    shell_screensaver_set_dpms(FALSE);
}

static void
on_dpms_resumed(void                            *data G_GNUC_UNUSED,
                struct ext_idle_notification_v1 *notification G_GNUC_UNUSED)
{
    shell_screensaver_set_dpms(TRUE);
}

static const struct ext_idle_notification_v1_listener dpms_notif_listener = {
    .idled   = on_dpms_idled,
    .resumed = on_dpms_resumed,
};

static void
on_lock_idled(void                            *data,
              struct ext_idle_notification_v1 *notification G_GNUC_UNUSED)
{
    ShellIdleManager *self = data;
    if (!self || self->is_inhibited || !self->app)
        return;

    if (self->app->session_lock) {
        shell_session_lock_activate(self->app->session_lock);
    }
}

static void
on_lock_resumed(void                            *data G_GNUC_UNUSED,
                struct ext_idle_notification_v1 *notification G_GNUC_UNUSED)
{
    shell_screensaver_set_dpms(TRUE);
}

static const struct ext_idle_notification_v1_listener lock_notif_listener = {
    .idled   = on_lock_idled,
    .resumed = on_lock_resumed,
};

/* --- Notification Lifecycle --- */

static void
clear_idle_notifications(ShellIdleManager *self)
{
    if (!self)
        return;

    if (self->screensaver_notif) {
        ext_idle_notification_v1_destroy(self->screensaver_notif);
        self->screensaver_notif = NULL;
    }
    if (self->dpms_notif) {
        ext_idle_notification_v1_destroy(self->dpms_notif);
        self->dpms_notif = NULL;
    }
    if (self->lock_notif) {
        ext_idle_notification_v1_destroy(self->lock_notif);
        self->lock_notif = NULL;
    }
}

static void
setup_idle_notifications(ShellIdleManager *self)
{
    if (!self)
        return;

    clear_idle_notifications(self);

    if (self->is_inhibited || !self->idle_notifier || !self->wl_seat || !self->app)
        return;

    ShellConfig *cfg = shell_app_get_config(self->app);
    if (!cfg)
        return;

    const gchar *mode = cfg->screensaver_mode ? cfg->screensaver_mode : "overlay";
    gint ss_sec   = cfg->screensaver_timeout_sec;
    gint dpms_sec = cfg->dpms_timeout_sec;
    gint lock_sec = cfg->lock_timeout_sec;

    if (g_strcmp0(mode, "disabled") != 0 && ss_sec > 0) {
        self->screensaver_notif = ext_idle_notifier_v1_get_idle_notification(
            self->idle_notifier, (uint32_t)(ss_sec * 1000), self->wl_seat);
        if (self->screensaver_notif) {
            ext_idle_notification_v1_add_listener(
                self->screensaver_notif, &screensaver_notif_listener, self);
        }
    }

    if (dpms_sec > 0) {
        self->dpms_notif = ext_idle_notifier_v1_get_idle_notification(
            self->idle_notifier, (uint32_t)(dpms_sec * 1000), self->wl_seat);
        if (self->dpms_notif) {
            ext_idle_notification_v1_add_listener(
                self->dpms_notif, &dpms_notif_listener, self);
        }
    }

    if (lock_sec > 0) {
        self->lock_notif = ext_idle_notifier_v1_get_idle_notification(
            self->idle_notifier, (uint32_t)(lock_sec * 1000), self->wl_seat);
        if (self->lock_notif) {
            ext_idle_notification_v1_add_listener(
                self->lock_notif, &lock_notif_listener, self);
        }
    }

    if (self->wl_display) {
        wl_display_flush(self->wl_display);
    }
}

/* --- Wayland Registry Listener --- */

static void
on_registry_global(void               *data,
                   struct wl_registry *registry,
                   uint32_t            name,
                   const char         *interface,
                   uint32_t            version)
{
    ShellIdleManager *self = data;
    if (!self || !interface)
        return;

    if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        uint32_t bind_ver = version < 2 ? version : 2;
        self->idle_notifier = wl_registry_bind(
            registry, name, &ext_idle_notifier_v1_interface, bind_ver);
        setup_idle_notifications(self);
    }
}

static void
on_registry_global_remove(void               *data G_GNUC_UNUSED,
                          struct wl_registry *registry G_GNUC_UNUSED,
                          uint32_t            name G_GNUC_UNUSED)
{
}

static const struct wl_registry_listener registry_listener = {
    .global        = on_registry_global,
    .global_remove = on_registry_global_remove,
};

/* --- systemd-logind Idle Inhibitor FD --- */

static void
acquire_logind_inhibitor(ShellIdleManager *self)
{
    if (!self || self->logind_inhibit_fd >= 0)
        return;

    if (!self->system_bus) {
        self->system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    }
    if (!self->system_bus)
        return;

    GError *error = NULL;
    GUnixFDList *fd_list = NULL;
    GVariant *res = g_dbus_connection_call_with_unix_fd_list_sync(
        self->system_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        g_variant_new("(ssss)",
                      "idle",
                      "kajo",
                      "Keep Awake enabled by user",
                      "block"),
        G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        NULL,
        &fd_list,
        NULL,
        &error);

    if (res) {
        gint32 fd_index = -1;
        g_variant_get(res, "(h)", &fd_index);
        if (fd_list && fd_index >= 0) {
            self->logind_inhibit_fd = g_unix_fd_list_get(fd_list, fd_index, NULL);
        }
        g_variant_unref(res);
    } else if (error) {
        g_clear_error(&error);
    }

    if (fd_list)
        g_object_unref(fd_list);
}

static void
release_logind_inhibitor(ShellIdleManager *self)
{
    if (!self)
        return;

    if (self->logind_inhibit_fd >= 0) {
        close(self->logind_inhibit_fd);
        self->logind_inhibit_fd = -1;
    }
}

/* --- Public API --- */

ShellIdleManager *
shell_idle_manager_new(ShellApp *app)
{
    ShellIdleManager *self = g_new0(ShellIdleManager, 1);
    self->app = app;
    self->is_inhibited = FALSE;
    self->logind_inhibit_fd = -1;
    self->listeners = g_ptr_array_new_with_free_func(g_free);

    GdkDisplay *display = gdk_display_get_default();
    if (display && GDK_IS_WAYLAND_DISPLAY(display)) {
        self->wl_display = gdk_wayland_display_get_wl_display(display);
        GdkSeat *seat = gdk_display_get_default_seat(display);
        if (seat && GDK_IS_WAYLAND_SEAT(seat)) {
            self->wl_seat = gdk_wayland_seat_get_wl_seat(seat);
        }

        if (self->wl_display) {
            self->wl_registry = wl_display_get_registry(self->wl_display);
            if (self->wl_registry) {
                wl_registry_add_listener(self->wl_registry, &registry_listener, self);
                wl_display_roundtrip(self->wl_display);
            }
        }
    }

    return self;
}

void
shell_idle_manager_reload_config(ShellIdleManager *self)
{
    if (!self)
        return;
    setup_idle_notifications(self);
}

void
shell_idle_manager_set_inhibited(ShellIdleManager *self, gboolean inhibited)
{
    if (!self || self->is_inhibited == inhibited)
        return;

    self->is_inhibited = inhibited;

    if (inhibited) {
        acquire_logind_inhibitor(self);
        clear_idle_notifications(self);
    } else {
        release_logind_inhibitor(self);
        setup_idle_notifications(self);
    }

    if (self->listeners) {
        for (guint i = 0; i < self->listeners->len; i++) {
            InhibitListenerEntry *entry = g_ptr_array_index(self->listeners, i);
            if (entry && entry->callback) {
                entry->callback(self->is_inhibited, entry->user_data);
            }
        }
    }
}

gboolean
shell_idle_manager_get_inhibited(ShellIdleManager *self)
{
    return self ? self->is_inhibited : FALSE;
}

void
shell_idle_manager_add_listener(ShellIdleManager           *self,
                                ShellIdleInhibitChangedFunc callback,
                                gpointer                    user_data)
{
    if (!self || !callback || !self->listeners)
        return;

    InhibitListenerEntry *entry = g_new0(InhibitListenerEntry, 1);
    entry->callback = callback;
    entry->user_data = user_data;
    g_ptr_array_add(self->listeners, entry);
}

void
shell_idle_manager_remove_listener(ShellIdleManager           *self,
                                   ShellIdleInhibitChangedFunc callback,
                                   gpointer                    user_data)
{
    if (!self || !self->listeners)
        return;

    for (guint i = 0; i < self->listeners->len; i++) {
        InhibitListenerEntry *entry = g_ptr_array_index(self->listeners, i);
        if (entry && entry->callback == callback && entry->user_data == user_data) {
            g_ptr_array_remove_index(self->listeners, i);
            return;
        }
    }
}

void
shell_idle_manager_destroy(ShellIdleManager *self)
{
    if (!self)
        return;

    clear_idle_notifications(self);
    release_logind_inhibitor(self);

    if (self->idle_notifier) {
        ext_idle_notifier_v1_destroy(self->idle_notifier);
        self->idle_notifier = NULL;
    }

    if (self->wl_registry) {
        wl_registry_destroy(self->wl_registry);
        self->wl_registry = NULL;
    }

    if (self->system_bus) {
        g_object_unref(self->system_bus);
        self->system_bus = NULL;
    }

    if (self->listeners) {
        g_ptr_array_free(self->listeners, TRUE);
        self->listeners = NULL;
    }

    g_free(self);
}
