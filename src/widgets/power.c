#include <gtk/gtk.h>
#include <gio/gio.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include "widget.h"
#include "../shell.h"
#include "../compositor/compositor.h"

#define LOGIND_BUS_NAME    "org.freedesktop.login1"
#define LOGIND_OBJECT_PATH "/org/freedesktop/login1"
#define LOGIND_MANAGER_IF  "org.freedesktop.login1.Manager"

typedef struct {
    ShellWidget   base;
    GtkWidget    *button;
    GtkWidget    *icon_img;
    GtkWidget    *label;
    GtkWidget    *popover;
    GtkWidget    *main_box;
    GtkWidget    *confirm_box;
    GtkWidget    *confirm_label;
    GtkWidget    *uptime_label;
    GtkWidget    *settings_btn;
    GDBusProxy   *logind_proxy;
    GCancellable *cancellable;
    const gchar  *pending_action; /* "PowerOff" or "Reboot" */
} PowerWidget;

/* --- Uptime --- */

static void power_update_uptime(PowerWidget *pw)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) {
        gtk_label_set_text(GTK_LABEL(pw->uptime_label), "[ UP -- ]");
        return;
    }

    double uptime_secs = 0;
    if (fscanf(fp, "%lf", &uptime_secs) != 1) {
        fclose(fp);
        gtk_label_set_text(GTK_LABEL(pw->uptime_label), "[ UP -- ]");
        return;
    }
    fclose(fp);

    gint total_mins = (gint)(uptime_secs / 60.0);
    gint days = total_mins / (60 * 24);
    gint hours = (total_mins % (60 * 24)) / 60;
    gint mins = total_mins % 60;

    gchar *text;
    if (days > 0)
        text = g_strdup_printf("[ UP %dD %dH ]", days, hours);
    else if (hours > 0)
        text = g_strdup_printf("[ UP %dH %dM ]", hours, mins);
    else
        text = g_strdup_printf("[ UP %dM ]", mins);

    gtk_label_set_text(GTK_LABEL(pw->uptime_label), text);
    g_free(text);
}

/* --- Actions --- */

static void power_call_logind(PowerWidget *pw, const char *method)
{
    if (!pw->logind_proxy) {
        g_warning("power: logind proxy not available, cannot call %s", method);
        return;
    }

    g_dbus_proxy_call(pw->logind_proxy,
                      method,
                      g_variant_new("(b)", TRUE),
                      G_DBUS_CALL_FLAGS_NONE,
                      -1, NULL, NULL, NULL);
}

static void power_show_main(PowerWidget *pw);

static void on_confirm_yes(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PowerWidget *pw = user_data;
    if (pw->pending_action)
        power_call_logind(pw, pw->pending_action);
    pw->pending_action = NULL;
    gtk_popover_popdown(GTK_POPOVER(pw->popover));
}

static void on_confirm_cancel(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PowerWidget *pw = user_data;
    pw->pending_action = NULL;
    power_show_main(pw);
}

static void power_show_confirm(PowerWidget *pw, const gchar *action)
{
    pw->pending_action = action;
    if (pw->confirm_label) {
        gtk_label_set_text(GTK_LABEL(pw->confirm_label),
                           g_strcmp0(action, "Reboot") == 0
                               ? "Confirm system reboot?"
                               : "Confirm system power off?");
    }
    gtk_widget_set_visible(pw->main_box, FALSE);
    gtk_widget_set_visible(pw->confirm_box, TRUE);
}

static void power_show_main(PowerWidget *pw)
{
    gtk_widget_set_visible(pw->confirm_box, FALSE);
    gtk_widget_set_visible(pw->main_box, TRUE);
    power_update_uptime(pw);
}

static void on_poweroff_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    power_show_confirm((PowerWidget *)user_data, "PowerOff");
}

static void on_reboot_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    power_show_confirm((PowerWidget *)user_data, "Reboot");
}

static void on_suspend_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PowerWidget *pw = user_data;
    power_call_logind(pw, "Suspend");
    gtk_popover_popdown(GTK_POPOVER(pw->popover));
}

static void on_hibernate_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PowerWidget *pw = user_data;
    power_call_logind(pw, "Hibernate");
    gtk_popover_popdown(GTK_POPOVER(pw->popover));
}

static void on_lock_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PowerWidget *pw = user_data;
    if (pw && pw->popover) {
        gtk_popover_popdown(GTK_POPOVER(pw->popover));
    }
    if (pw && pw->base.app) {
        shell_app_lock_session(pw->base.app);
    } else if (pw && pw->logind_proxy) {
        g_dbus_proxy_call(pw->logind_proxy, "LockSessions", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void on_logout_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PowerWidget *pw = user_data;
    if (pw && pw->popover) {
        gtk_popover_popdown(GTK_POPOVER(pw->popover));
    }
    shell_compositor_quit();
}

/* --- Popover opened callback: reset to main view --- */

static void on_popover_show(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    power_show_main((PowerWidget *)user_data);
}

static void on_power_settings_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PowerWidget *pw = user_data;
    shell_spawn_settings("displays", pw ? pw->popover : NULL);
}

/* --- Helper: create icon+label button --- */

static GtkWidget *power_make_button(const gchar *icon_name, const gchar *label_text,
                                    GCallback callback, gpointer user_data)
{
    GtkWidget *btn = gtk_button_new();
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(content_box, GTK_ALIGN_CENTER);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(content_box), icon);
    gtk_box_append(GTK_BOX(content_box), label);
    gtk_button_set_child(GTK_BUTTON(btn), content_box);

    g_signal_connect(btn, "clicked", callback, user_data);
    return btn;
}

/* --- Logind proxy --- */

static void power_logind_proxy_ready(GObject *source G_GNUC_UNUSED,
                                     GAsyncResult *res,
                                     gpointer user_data)
{
    PowerWidget *pw = user_data;
    GError *error = NULL;

    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (error) g_clear_error(&error);
        if (proxy) g_object_unref(proxy);
        return;
    }

    if (!proxy) {
        if (error) {
            g_warning("power: failed to connect to logind: %s", error->message);
            g_clear_error(&error);
        }
        return;
    }

    g_clear_object(&pw->logind_proxy);
    pw->logind_proxy = proxy;
}

/* --- Widget interface --- */

static ShellWidget *power_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    PowerWidget *pw = g_new0(PowerWidget, 1);

    pw->icon_img = gtk_image_new_from_icon_name("tb-power-symbolic");
    pw->label = gtk_label_new("Power");
    GtkWidget *hbox = shell_widget_pack_bar_box(pw->icon_img, pw->label);

    /* Menu button with shutdown icon + optional label */
    pw->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(pw->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(pw->button), hbox);
    gtk_widget_add_css_class(pw->button, "flat");
    gtk_widget_add_css_class(pw->button, "shell-widget");
    gtk_widget_add_css_class(pw->button, "shell-widget-power");

    /* 3-Zone Popover */
    GtkWidget *body = NULL;
    GtkWidget *footer = NULL;
    pw->popover = GTK_WIDGET(shell_widget_create_popover_shell(
        "popover-compact", "SESSION & POWER", "[ UP -- ]",
        &pw->uptime_label, &body, &footer));

    /* Zone 2: Main session/power actions */
    pw->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *btn_lock = power_make_button("fl-lock-symbolic", "Lock Screen",
                                             G_CALLBACK(on_lock_clicked), pw);
    GtkWidget *btn_suspend = power_make_button("fl-sleep-symbolic", "Suspend",
                                                G_CALLBACK(on_suspend_clicked), pw);
    GtkWidget *btn_hibernate = power_make_button("fl-sleep-symbolic", "Hibernate",
                                                  G_CALLBACK(on_hibernate_clicked), pw);
    GtkWidget *btn_logout = power_make_button("fl-logout-symbolic", "Log Out",
                                               G_CALLBACK(on_logout_clicked), pw);
    GtkWidget *btn_reboot = power_make_button("fl-restart-symbolic", "Reboot...",
                                               G_CALLBACK(on_reboot_clicked), pw);
    GtkWidget *btn_poweroff = power_make_button("fl-power-symbolic", "Power Off...",
                                                 G_CALLBACK(on_poweroff_clicked), pw);
    gtk_widget_add_css_class(btn_reboot, "destructive-action");
    gtk_widget_add_css_class(btn_poweroff, "destructive-action");

    gtk_box_append(GTK_BOX(pw->main_box), btn_lock);
    gtk_box_append(GTK_BOX(pw->main_box), btn_suspend);
    gtk_box_append(GTK_BOX(pw->main_box), btn_hibernate);
    gtk_box_append(GTK_BOX(pw->main_box), btn_logout);
    gtk_box_append(GTK_BOX(pw->main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(pw->main_box), btn_reboot);
    gtk_box_append(GTK_BOX(pw->main_box), btn_poweroff);

    /* Confirmation content */
    pw->confirm_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    pw->confirm_label = gtk_label_new("Confirm system power off?");
    gtk_label_set_xalign(GTK_LABEL(pw->confirm_label), 0.0f);
    gtk_widget_add_css_class(pw->confirm_label, "row-title");
    gtk_box_append(GTK_BOX(pw->confirm_box), pw->confirm_label);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(btn_row, GTK_ALIGN_CENTER);
    GtkWidget *btn_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_set_hexpand(btn_cancel, TRUE);
    GtkWidget *btn_yes = gtk_button_new_with_label("CONFIRM");
    gtk_widget_set_hexpand(btn_yes, TRUE);
    gtk_widget_add_css_class(btn_yes, "destructive-action");

    g_signal_connect(btn_yes, "clicked", G_CALLBACK(on_confirm_yes), pw);
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_confirm_cancel), pw);

    gtk_box_append(GTK_BOX(btn_row), btn_cancel);
    gtk_box_append(GTK_BOX(btn_row), btn_yes);
    gtk_box_append(GTK_BOX(pw->confirm_box), btn_row);

    gtk_widget_set_visible(pw->confirm_box, FALSE);

    gtk_box_append(GTK_BOX(body), pw->main_box);
    gtk_box_append(GTK_BOX(body), pw->confirm_box);

    /* Zone 3: Idle & Display Settings shortcut */
    pw->settings_btn = gtk_button_new_with_label("IDLE & LOCK SETTINGS");
    gtk_widget_set_hexpand(pw->settings_btn, TRUE);
    gtk_widget_set_halign(pw->settings_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(pw->settings_btn, GTK_ALIGN_CENTER);
    g_signal_connect(pw->settings_btn, "clicked", G_CALLBACK(on_power_settings_clicked), pw);
    gtk_box_append(GTK_BOX(footer), pw->settings_btn);

    gtk_menu_button_set_popover(GTK_MENU_BUTTON(pw->button), pw->popover);

    g_signal_connect(pw->popover, "show", G_CALLBACK(on_popover_show), pw);

    return (ShellWidget *)pw;
}

static void power_destroy(ShellWidget *widget)
{
    PowerWidget *pw = (PowerWidget *)widget;
    if (!pw)
        return;
    if (pw->settings_btn)
        g_signal_handlers_disconnect_by_data(pw->settings_btn, pw);
    if (pw->main_box) {
        for (GtkWidget *child = gtk_widget_get_first_child(pw->main_box);
             child != NULL;
             child = gtk_widget_get_next_sibling(child)) {
            g_signal_handlers_disconnect_by_data(child, pw);
        }
    }
    if (pw->confirm_box) {
        for (GtkWidget *child = gtk_widget_get_first_child(pw->confirm_box);
             child != NULL;
             child = gtk_widget_get_next_sibling(child)) {
            g_signal_handlers_disconnect_by_data(child, pw);
            for (GtkWidget *sub = gtk_widget_get_first_child(child);
                 sub != NULL;
                 sub = gtk_widget_get_next_sibling(sub)) {
                g_signal_handlers_disconnect_by_data(sub, pw);
            }
        }
    }
    if (pw->popover)
        g_signal_handlers_disconnect_by_data(pw->popover, pw);
    if (pw->cancellable) {
        g_cancellable_cancel(pw->cancellable);
        g_object_unref(pw->cancellable);
        pw->cancellable = NULL;
    }
    if (pw->logind_proxy)
        g_object_unref(pw->logind_proxy);
    g_free(pw);
}

static GtkWidget *power_get_widget(ShellWidget *widget)
{
    return ((PowerWidget *)widget)->button;
}

static void power_enable(ShellWidget *widget)
{
    PowerWidget *pw = (PowerWidget *)widget;

    if (pw->icon_img)
        gtk_image_set_pixel_size(GTK_IMAGE(pw->icon_img), shell_widget_get_icon_size(widget));
    shell_widget_apply_mode_visibility(widget->mode, pw->icon_img, pw->label);

    if (!pw->logind_proxy && !pw->cancellable) {
        pw->cancellable = g_cancellable_new();
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 NULL,
                                 LOGIND_BUS_NAME,
                                 LOGIND_OBJECT_PATH,
                                 LOGIND_MANAGER_IF,
                                 pw->cancellable,
                                 power_logind_proxy_ready,
                                 pw);
    }

    power_update_uptime(pw);
}

static void power_disable(ShellWidget *widget)
{
    PowerWidget *pw = (PowerWidget *)widget;
    if (pw->cancellable) {
        g_cancellable_cancel(pw->cancellable);
        g_clear_object(&pw->cancellable);
    }
    if (pw->logind_proxy)
        g_clear_object(&pw->logind_proxy);
}

const ShellWidgetClass power_widget_class = {
    .id         = "power",
    .name       = "Power",
    .create     = power_create,
    .destroy    = power_destroy,
    .get_widget = power_get_widget,
    .enable     = power_enable,
    .disable    = power_disable,
};
