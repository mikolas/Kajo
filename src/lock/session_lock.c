#define _GNU_SOURCE
#include "session_lock.h"
#include "pam_auth.h"
#include "screensaver.h"
#include "../shell.h"
#include "../config.h"
#include "../launcher/launcher_surface.h"

#include <gtk4-session-lock.h>
#include <gio/gio.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    ShellSessionLock *owner;
    GtkWidget        *window;
    GtkWidget        *clock_label;
    GtkWidget        *date_label;
    GtkWidget        *password_entry;
    GtkWidget        *submit_btn;
    GtkWidget        *status_label;
} LockMonitorSurface;

struct _ShellSessionLock {
    ShellApp               *app;
    GtkSessionLockInstance *lock_instance;
    GPtrArray              *surfaces;         /* Array of LockMonitorSurface* */
    guint                   clock_timer_id;
    gboolean                is_locked;
    gboolean                auth_in_progress;
    GCancellable           *cancellable;

    GDBusConnection        *system_bus;
    gchar                  *session_object_path;
    guint                   sleep_sub_id;
    guint                   lock_sub_id;
    guint                   unlock_sub_id;
};

static void
on_lock_window_destroy(GtkWidget *widget, gpointer user_data)
{
    LockMonitorSurface *surf = user_data;
    if (surf && surf->window == widget) {
        surf->window = NULL;
        surf->clock_label = NULL;
        surf->date_label = NULL;
        surf->password_entry = NULL;
        surf->submit_btn = NULL;
        surf->status_label = NULL;
    }
}

static void
lock_monitor_surface_free(gpointer data)
{
    LockMonitorSurface *surf = data;
    if (!surf)
        return;
    if (surf->window) {
        g_signal_handlers_disconnect_by_func(surf->window, on_lock_window_destroy, surf);
        surf->window = NULL;
    }
    g_free(surf);
}

static void
set_logind_locked_hint(ShellSessionLock *self, gboolean locked)
{
    if (!self || !self->system_bus || !self->session_object_path)
        return;

    g_dbus_connection_call(
        self->system_bus,
        "org.freedesktop.login1",
        self->session_object_path,
        "org.freedesktop.login1.Session",
        "SetLockedHint",
        g_variant_new("(b)", locked),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        NULL);
}

static void
update_lock_clocks(ShellSessionLock *self)
{
    if (!self || !self->surfaces)
        return;

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    if (!now)
        return;

    g_autofree gchar *time_str = g_date_time_format(now, "%H : %M");
    g_autofree gchar *raw_date = g_date_time_format(now, "%A, %B %-d");
    g_autofree gchar *date_str = raw_date ? g_utf8_strup(raw_date, -1) : g_strdup("");

    for (guint i = 0; i < self->surfaces->len; i++) {
        LockMonitorSurface *surf = g_ptr_array_index(self->surfaces, i);
        if (!surf || !surf->window)
            continue;
        if (surf->clock_label && time_str)
            gtk_label_set_text(GTK_LABEL(surf->clock_label), time_str);
        if (surf->date_label && date_str)
            gtk_label_set_text(GTK_LABEL(surf->date_label), date_str);
    }
}

static gboolean
on_lock_clock_tick(gpointer user_data)
{
    ShellSessionLock *self = user_data;
    if (!self || !self->is_locked)
        return G_SOURCE_REMOVE;

    update_lock_clocks(self);
    return G_SOURCE_CONTINUE;
}

static void
set_surfaces_auth_state(ShellSessionLock *self,
                        gboolean          busy,
                        const gchar      *status_msg,
                        gboolean          is_error,
                        gboolean          clear_entries)
{
    if (!self || !self->surfaces)
        return;

    for (guint i = 0; i < self->surfaces->len; i++) {
        LockMonitorSurface *surf = g_ptr_array_index(self->surfaces, i);
        if (!surf || !surf->window)
            continue;

        if (surf->password_entry) {
            gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(surf->password_entry), FALSE);
            gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(surf->password_entry), TRUE);
            if (clear_entries)
                gtk_editable_set_text(GTK_EDITABLE(surf->password_entry), "");
            gtk_widget_set_sensitive(surf->password_entry, !busy);
        }

        if (surf->submit_btn) {
            gtk_widget_set_sensitive(surf->submit_btn, !busy);
            gtk_button_set_label(GTK_BUTTON(surf->submit_btn), busy ? "⋯" : "→");
        }

        if (surf->status_label) {
            gtk_label_set_text(GTK_LABEL(surf->status_label),
                               status_msg ? status_msg : "Press Enter to unlock  •  Esc to clear");
            if (is_error) {
                gtk_widget_add_css_class(surf->status_label, "error");
                gtk_widget_remove_css_class(surf->status_label, "verifying");
            } else if (busy) {
                gtk_widget_add_css_class(surf->status_label, "verifying");
                gtk_widget_remove_css_class(surf->status_label, "error");
            } else {
                gtk_widget_remove_css_class(surf->status_label, "error");
                gtk_widget_remove_css_class(surf->status_label, "verifying");
            }
        }
    }
}

static void
on_pam_auth_complete(GObject      *source_object G_GNUC_UNUSED,
                     GAsyncResult *res,
                     gpointer      user_data)
{
    ShellSessionLock *self = user_data;
    g_autoptr(GError) error = NULL;
    gboolean ok = shell_pam_authenticate_finish(res, &error);

    if (!self)
        return;

    self->auth_in_progress = FALSE;

    if (ok) {
        set_surfaces_auth_state(self, FALSE, "Unlocked", FALSE, TRUE);
        shell_session_lock_unlock(self);
    } else {
        const gchar *err_msg = (error && error->message)
            ? error->message
            : "⚠ Authentication failed — try again";
        g_autofree gchar *formatted = g_str_has_prefix(err_msg, "⚠")
            ? g_strdup(err_msg)
            : g_strdup_printf("⚠ %s", err_msg);

        set_surfaces_auth_state(self, FALSE, formatted, TRUE, TRUE);

        /* Re-focus password entry on the first active surface */
        if (self->surfaces && self->surfaces->len > 0) {
            LockMonitorSurface *first = g_ptr_array_index(self->surfaces, 0);
            if (first && first->password_entry)
                gtk_widget_grab_focus(first->password_entry);
        }
    }
}

static void
submit_password_from_surface(LockMonitorSurface *surf)
{
    if (!surf || !surf->owner || !surf->password_entry)
        return;

    ShellSessionLock *self = surf->owner;
    if (self->auth_in_progress || !self->is_locked)
        return;

    const gchar *pw = gtk_editable_get_text(GTK_EDITABLE(surf->password_entry));
    if (!pw || pw[0] == '\0') {
        set_surfaces_auth_state(self, FALSE, "⚠ Please enter your password", TRUE, FALSE);
        gtk_widget_grab_focus(surf->password_entry);
        return;
    }

    self->auth_in_progress = TRUE;
    gsize pw_len = strlen(pw);
    gchar *pw_copy = g_strdup(pw);
    set_surfaces_auth_state(self, TRUE, "⟳ Verifying credentials...", FALSE, TRUE);

    shell_pam_authenticate_async(pw_copy, self->cancellable, on_pam_auth_complete, self);
    explicit_bzero(pw_copy, pw_len);
    g_free(pw_copy);
}

static void
on_password_entry_activate(GtkWidget *entry G_GNUC_UNUSED, gpointer user_data)
{
    LockMonitorSurface *surf = user_data;
    submit_password_from_surface(surf);
}

static void
on_submit_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    LockMonitorSurface *surf = user_data;
    submit_password_from_surface(surf);
}

static gboolean
password_entry_has_focus(LockMonitorSurface *surf)
{
    if (!surf || !surf->window || !surf->password_entry)
        return FALSE;

    if (gtk_widget_get_focus_child(surf->password_entry) != NULL)
        return TRUE;

    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(surf->window));
    return (focus && (focus == surf->password_entry ||
                      gtk_widget_is_ancestor(focus, surf->password_entry)));
}

static void
focus_password_entry_preserve_cursor(LockMonitorSurface *surf)
{
    if (!surf || !surf->password_entry)
        return;

    if (!password_entry_has_focus(surf)) {
        gtk_widget_grab_focus(surf->password_entry);
        gtk_editable_set_position(GTK_EDITABLE(surf->password_entry), -1);
    }
}

static gboolean
on_lock_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED,
                    guint                  keyval,
                    guint                  keycode G_GNUC_UNUSED,
                    GdkModifierType        state G_GNUC_UNUSED,
                    gpointer               user_data)
{
    LockMonitorSurface *surf = user_data;
    if (!surf || !surf->owner)
        return FALSE;

    if (keyval == GDK_KEY_Escape) {
        if (surf->password_entry && !surf->owner->auth_in_progress) {
            set_surfaces_auth_state(surf->owner, FALSE, NULL, FALSE, TRUE);
            focus_password_entry_preserve_cursor(surf);
        }
        return TRUE;
    }

    focus_password_entry_preserve_cursor(surf);
    return FALSE;
}

static void
on_screen_off_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    shell_screensaver_set_dpms(FALSE);
}

static void
on_lock_suspend_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ShellSessionLock *self = user_data;
    if (!self || !self->system_bus)
        return;

    g_dbus_connection_call(
        self->system_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Suspend",
        g_variant_new("(b)", TRUE),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        NULL);
}

static void
on_lock_window_mapped(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    LockMonitorSurface *surf = user_data;
    if (surf && surf->password_entry) {
        gtk_widget_grab_focus(surf->password_entry);
    }
}

static void
on_lock_monitor(GtkSessionLockInstance *instance,
                GdkMonitor             *monitor,
                gpointer                user_data)
{
    ShellSessionLock *self = user_data;
    if (!self || !monitor)
        return;

    LockMonitorSurface *surf = g_new0(LockMonitorSurface, 1);
    surf->owner = self;

    GtkWidget *win = gtk_window_new();
    surf->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "Kajo Session Lock");
    gtk_widget_add_css_class(win, "shell-lock-window");
    g_signal_connect(win, "destroy", G_CALLBACK(on_lock_window_destroy), surf);
    g_signal_connect(win, "map", G_CALLBACK(on_lock_window_mapped), surf);

    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(root_box, TRUE);
    gtk_widget_set_vexpand(root_box, TRUE);

    /* Center Column */
    GtkWidget *center_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_hexpand(center_box, TRUE);
    gtk_widget_set_vexpand(center_box, TRUE);
    gtk_widget_set_halign(center_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(center_box, GTK_ALIGN_CENTER);

    /* Hero Clock & Date */
    GtkWidget *clock_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(clock_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(clock_box, 12);

    surf->clock_label = gtk_label_new("00 : 00");
    gtk_widget_add_css_class(surf->clock_label, "lock-clock-label");
    gtk_widget_set_halign(surf->clock_label, GTK_ALIGN_CENTER);

    surf->date_label = gtk_label_new("");
    gtk_widget_add_css_class(surf->date_label, "lock-date-label");
    gtk_widget_set_halign(surf->date_label, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(clock_box), surf->clock_label);
    gtk_box_append(GTK_BOX(clock_box), surf->date_label);
    gtk_box_append(GTK_BOX(center_box), clock_box);

    /* Metro Lock Card */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(card, "lock-card");
    gtk_widget_set_size_request(card, 380, -1);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);

    /* User Avatar Badge */
    GtkWidget *avatar_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(avatar_wrap, "lock-avatar-ring");
    gtk_widget_set_halign(avatar_wrap, GTK_ALIGN_CENTER);

    GtkWidget *user_icon = gtk_image_new_from_icon_name("tb-user-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(user_icon), 32);
    gtk_widget_set_halign(user_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(user_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(user_icon, TRUE);
    gtk_widget_set_vexpand(user_icon, TRUE);
    gtk_box_append(GTK_BOX(avatar_wrap), user_icon);
    gtk_box_append(GTK_BOX(card), avatar_wrap);

    /* Username & Status Badge */
    const gchar *username = g_get_user_name();
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0] != '\0')
        username = pw->pw_name;

    GtkWidget *user_lbl = gtk_label_new(username ? username : "user");
    gtk_widget_add_css_class(user_lbl, "lock-username-label");
    gtk_widget_set_halign(user_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), user_lbl);

    GtkWidget *badge_lbl = gtk_label_new("[ SESSION LOCKED ]");
    gtk_widget_add_css_class(badge_lbl, "lock-badge-label");
    gtk_widget_set_halign(badge_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), badge_lbl);

    /* Password Entry + Submit Row */
    GtkWidget *input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(input_row, 6);

    surf->password_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(surf->password_entry), TRUE);
    g_object_set(surf->password_entry, "placeholder-text", "Enter password...", NULL);
    gtk_widget_add_css_class(surf->password_entry, "lock-password-entry");
    gtk_widget_set_hexpand(surf->password_entry, TRUE);
    g_signal_connect(surf->password_entry, "activate", G_CALLBACK(on_password_entry_activate), surf);

    surf->submit_btn = gtk_button_new_with_label("→");
    gtk_widget_add_css_class(surf->submit_btn, "lock-submit-btn");
    gtk_widget_set_focus_on_click(surf->submit_btn, FALSE);
    g_signal_connect(surf->submit_btn, "clicked", G_CALLBACK(on_submit_btn_clicked), surf);

    gtk_box_append(GTK_BOX(input_row), surf->password_entry);
    gtk_box_append(GTK_BOX(input_row), surf->submit_btn);
    gtk_box_append(GTK_BOX(card), input_row);

    /* Status / Error Label */
    surf->status_label = gtk_label_new("Press Enter to unlock  •  Esc to clear");
    gtk_widget_add_css_class(surf->status_label, "lock-status-label");
    gtk_widget_set_halign(surf->status_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), surf->status_label);

    gtk_box_append(GTK_BOX(center_box), card);
    gtk_box_append(GTK_BOX(root_box), center_box);

    /* Bottom Footer Actions */
    GtkWidget *footer_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(footer_bar, "lock-footer-bar");
    gtk_widget_set_halign(footer_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(footer_bar, 32);

    GtkWidget *btn_screen_off = gtk_button_new_with_label("[ 🌙 SCREEN OFF ]");
    gtk_widget_add_css_class(btn_screen_off, "lock-action-btn");
    gtk_widget_set_focus_on_click(btn_screen_off, FALSE);
    g_signal_connect(btn_screen_off, "clicked", G_CALLBACK(on_screen_off_clicked), self);

    GtkWidget *btn_suspend = gtk_button_new_with_label("[ ⏻ SUSPEND ]");
    gtk_widget_add_css_class(btn_suspend, "lock-action-btn");
    gtk_widget_set_focus_on_click(btn_suspend, FALSE);
    g_signal_connect(btn_suspend, "clicked", G_CALLBACK(on_lock_suspend_clicked), self);

    gtk_box_append(GTK_BOX(footer_bar), btn_screen_off);
    gtk_box_append(GTK_BOX(footer_bar), btn_suspend);
    gtk_box_append(GTK_BOX(root_box), footer_bar);

    gtk_window_set_child(GTK_WINDOW(win), root_box);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_lock_key_pressed), surf);
    gtk_widget_add_controller(win, key_ctrl);

    g_ptr_array_add(self->surfaces, surf);
    update_lock_clocks(self);

    /* Must be called with an unrealized GtkWindow; gtk4-session-lock presents it */
    gtk_session_lock_instance_assign_window_to_monitor(instance, GTK_WINDOW(win), monitor);
}

static void
on_session_locked(GtkSessionLockInstance *instance G_GNUC_UNUSED,
                  gpointer                user_data)
{
    ShellSessionLock *self = user_data;
    if (!self)
        return;

    self->is_locked = TRUE;
    set_logind_locked_hint(self, TRUE);
    update_lock_clocks(self);

    if (self->clock_timer_id == 0) {
        self->clock_timer_id = g_timeout_add_seconds(1, on_lock_clock_tick, self);
    }
}

static void
on_session_lock_failed(GtkSessionLockInstance *instance G_GNUC_UNUSED,
                       gpointer                user_data)
{
    ShellSessionLock *self = user_data;
    if (!self)
        return;

    g_warning("session-lock: failed to acquire ext-session-lock-v1 lock");
    self->is_locked = FALSE;
    self->auth_in_progress = FALSE;

    if (self->clock_timer_id > 0) {
        g_source_remove(self->clock_timer_id);
        self->clock_timer_id = 0;
    }

    if (self->surfaces && self->surfaces->len > 0) {
        g_ptr_array_remove_range(self->surfaces, 0, self->surfaces->len);
    }
}

static void
on_session_unlocked(GtkSessionLockInstance *instance G_GNUC_UNUSED,
                    gpointer                user_data)
{
    ShellSessionLock *self = user_data;
    if (!self)
        return;

    self->is_locked = FALSE;
    self->auth_in_progress = FALSE;
    set_logind_locked_hint(self, FALSE);
    shell_screensaver_set_dpms(TRUE);

    if (self->clock_timer_id > 0) {
        g_source_remove(self->clock_timer_id);
        self->clock_timer_id = 0;
    }

    if (self->surfaces && self->surfaces->len > 0) {
        g_ptr_array_remove_range(self->surfaces, 0, self->surfaces->len);
    }
}

static void
on_logind_prepare_for_sleep(GDBusConnection *connection G_GNUC_UNUSED,
                            const gchar     *sender_name G_GNUC_UNUSED,
                            const gchar     *object_path G_GNUC_UNUSED,
                            const gchar     *interface_name G_GNUC_UNUSED,
                            const gchar     *signal_name G_GNUC_UNUSED,
                            GVariant        *parameters,
                            gpointer         user_data)
{
    ShellSessionLock *self = user_data;
    if (!self || !parameters)
        return;

    gboolean going_to_sleep = FALSE;
    g_variant_get(parameters, "(b)", &going_to_sleep);

    if (going_to_sleep) {
        ShellConfig *cfg = self->app ? shell_app_get_config(self->app) : NULL;
        if (!cfg || cfg->lock_on_suspend) {
            shell_session_lock_activate(self);
        }
    } else {
        shell_screensaver_set_dpms(TRUE);
    }
}

static void
on_logind_session_lock_signal(GDBusConnection *connection G_GNUC_UNUSED,
                              const gchar     *sender_name G_GNUC_UNUSED,
                              const gchar     *object_path G_GNUC_UNUSED,
                              const gchar     *interface_name G_GNUC_UNUSED,
                              const gchar     *signal_name,
                              GVariant        *parameters G_GNUC_UNUSED,
                              gpointer         user_data)
{
    ShellSessionLock *self = user_data;
    if (!self || !signal_name)
        return;

    if (g_strcmp0(signal_name, "Lock") == 0) {
        shell_session_lock_activate(self);
    } else if (g_strcmp0(signal_name, "Unlock") == 0) {
        shell_session_lock_unlock(self);
    }
}

static void
setup_logind_integration(ShellSessionLock *self)
{
    GError *error = NULL;
    self->system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!self->system_bus) {
        if (error)
            g_clear_error(&error);
        return;
    }

    /* Resolve current login1 session object path */
    GVariant *res = g_dbus_connection_call_sync(
        self->system_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "GetSessionByPID",
        g_variant_new("(u)", (guint32)getpid()),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        NULL,
        &error);

    if (res) {
        const gchar *obj_path = NULL;
        g_variant_get(res, "(&o)", &obj_path);
        if (obj_path && *obj_path)
            self->session_object_path = g_strdup(obj_path);
        g_variant_unref(res);
    } else if (error) {
        g_clear_error(&error);
    }

    if (!self->session_object_path) {
        self->session_object_path = g_strdup("/org/freedesktop/login1/session/auto");
    }

    /* Subscribe to PrepareForSleep(b) */
    self->sleep_sub_id = g_dbus_connection_signal_subscribe(
        self->system_bus,
        "org.freedesktop.login1",
        "org.freedesktop.login1.Manager",
        "PrepareForSleep",
        "/org/freedesktop/login1",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_logind_prepare_for_sleep,
        self,
        NULL);

    /* Subscribe to Session.Lock and Session.Unlock */
    const gchar *sub_path = (g_strcmp0(self->session_object_path, "/org/freedesktop/login1/session/auto") == 0)
        ? NULL
        : self->session_object_path;

    self->lock_sub_id = g_dbus_connection_signal_subscribe(
        self->system_bus,
        "org.freedesktop.login1",
        "org.freedesktop.login1.Session",
        "Lock",
        sub_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_logind_session_lock_signal,
        self,
        NULL);

    self->unlock_sub_id = g_dbus_connection_signal_subscribe(
        self->system_bus,
        "org.freedesktop.login1",
        "org.freedesktop.login1.Session",
        "Unlock",
        sub_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_logind_session_lock_signal,
        self,
        NULL);
}

ShellSessionLock *
shell_session_lock_new(ShellApp *app)
{
    ShellSessionLock *self = g_new0(ShellSessionLock, 1);
    self->app = app;
    self->surfaces = g_ptr_array_new_with_free_func(lock_monitor_surface_free);
    self->cancellable = g_cancellable_new();
    self->is_locked = FALSE;
    self->auth_in_progress = FALSE;

    if (gtk_session_lock_is_supported()) {
        self->lock_instance = gtk_session_lock_instance_new();
        g_signal_connect(self->lock_instance, "monitor", G_CALLBACK(on_lock_monitor), self);
        g_signal_connect(self->lock_instance, "locked", G_CALLBACK(on_session_locked), self);
        g_signal_connect(self->lock_instance, "failed", G_CALLBACK(on_session_lock_failed), self);
        g_signal_connect(self->lock_instance, "unlocked", G_CALLBACK(on_session_unlocked), self);
    } else {
        g_warning("session-lock: compositor does not support ext-session-lock-v1");
    }

    setup_logind_integration(self);
    return self;
}

void
shell_session_lock_activate(ShellSessionLock *self)
{
    if (!self || !self->lock_instance)
        return;

    if (self->is_locked || gtk_session_lock_instance_is_locked(self->lock_instance))
        return;

    /* Dismiss screen saver overlay and launcher before locking */
    if (self->app) {
        if (self->app->screensaver && shell_screensaver_is_active(self->app->screensaver)) {
            shell_screensaver_stop(self->app->screensaver);
        }
        if (self->app->launcher) {
            shell_launcher_surface_hide(self->app->launcher);
        }
    }

    self->is_locked = TRUE;
    self->auth_in_progress = FALSE;
    gtk_window_set_interactive_debugging(FALSE);

    if (!gtk_session_lock_instance_lock(self->lock_instance)) {
        self->is_locked = FALSE;
        g_warning("session-lock: immediate failure starting session lock");
    }
}

void
shell_session_lock_unlock(ShellSessionLock *self)
{
    if (!self || !self->lock_instance)
        return;

    if (gtk_session_lock_instance_is_locked(self->lock_instance)) {
        gtk_session_lock_instance_unlock(self->lock_instance);
    }
    self->is_locked = FALSE;
    self->auth_in_progress = FALSE;
}

gboolean
shell_session_lock_is_locked(ShellSessionLock *self)
{
    if (!self || !self->lock_instance)
        return FALSE;
    return self->is_locked || gtk_session_lock_instance_is_locked(self->lock_instance);
}

void
shell_session_lock_destroy(ShellSessionLock *self)
{
    if (!self)
        return;

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_object_unref(self->cancellable);
        self->cancellable = NULL;
    }

    if (self->clock_timer_id > 0) {
        g_source_remove(self->clock_timer_id);
        self->clock_timer_id = 0;
    }

    if (self->system_bus) {
        if (self->sleep_sub_id > 0)
            g_dbus_connection_signal_unsubscribe(self->system_bus, self->sleep_sub_id);
        if (self->lock_sub_id > 0)
            g_dbus_connection_signal_unsubscribe(self->system_bus, self->lock_sub_id);
        if (self->unlock_sub_id > 0)
            g_dbus_connection_signal_unsubscribe(self->system_bus, self->unlock_sub_id);
        g_object_unref(self->system_bus);
        self->system_bus = NULL;
    }

    g_free(self->session_object_path);
    self->session_object_path = NULL;

    if (self->surfaces) {
        g_ptr_array_free(self->surfaces, TRUE);
        self->surfaces = NULL;
    }

    if (self->lock_instance) {
        g_object_unref(self->lock_instance);
        self->lock_instance = NULL;
    }

    g_free(self);
}
