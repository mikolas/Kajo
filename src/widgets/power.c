#include <gtk/gtk.h>
#include <gio/gio.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include "widget.h"
#include "../compositor/compositor.h"

#define LOGIND_BUS_NAME    "org.freedesktop.login1"
#define LOGIND_OBJECT_PATH "/org/freedesktop/login1"
#define LOGIND_MANAGER_IF  "org.freedesktop.login1.Manager"

typedef struct {
    ShellWidget  base;
    GtkWidget   *button;
    GtkWidget   *popover;
    GtkWidget   *main_box;
    GtkWidget   *confirm_box;
    GtkWidget   *uptime_label;
    GDBusProxy  *logind_proxy;
    GCancellable *cancellable;
    const gchar *pending_action; /* "PowerOff" or "Reboot" */
} PowerWidget;

/* --- Uptime --- */

static void power_update_uptime(PowerWidget *pw)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) {
        gtk_label_set_text(GTK_LABEL(pw->uptime_label), "Uptime: unknown");
        return;
    }

    double uptime_secs = 0;
    if (fscanf(fp, "%lf", &uptime_secs) != 1) {
        fclose(fp);
        gtk_label_set_text(GTK_LABEL(pw->uptime_label), "Uptime: unknown");
        return;
    }
    fclose(fp);

    gint total_mins = (gint)(uptime_secs / 60.0);
    gint days = total_mins / (60 * 24);
    gint hours = (total_mins % (60 * 24)) / 60;
    gint mins = total_mins % 60;

    gchar *text;
    if (days > 0)
        text = g_strdup_printf("Uptime: %dd %dh %dm", days, hours, mins);
    else if (hours > 0)
        text = g_strdup_printf("Uptime: %dh %dm", hours, mins);
    else
        text = g_strdup_printf("Uptime: %dm", mins);

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
    if (pw->logind_proxy) {
        g_dbus_proxy_call(pw->logind_proxy, "LockSessions", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
    gtk_popover_popdown(GTK_POPOVER(pw->popover));
}

static void on_logout_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    pid_t ppid = getppid();
    if (ppid > 1)
        kill(ppid, SIGTERM);
}

/* --- Popover opened callback: reset to main view --- */

static void on_popover_show(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    power_show_main((PowerWidget *)user_data);
}

/* --- Helper: create icon+label button --- */

static GtkWidget *power_make_button(const gchar *icon_name, const gchar *label_text,
                                    GCallback callback, gpointer user_data)
{
    GtkWidget *btn = gtk_button_new();
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    GtkWidget *label = gtk_label_new(label_text);

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

    pw->logind_proxy = proxy;
}

/* --- Widget interface --- */

static ShellWidget *power_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    PowerWidget *pw = g_new0(PowerWidget, 1);

    /* Menu button with shutdown icon */
    pw->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(pw->button), FALSE);
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pw->button), "tb-power-symbolic");
    gtk_widget_add_css_class(pw->button, "flat");
    gtk_widget_add_css_class(pw->button, "shell-widget");
    gtk_widget_add_css_class(pw->button, "shell-widget-power");

    /* Popover wrapper box (holds both main_box and confirm_box) */
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(wrapper, 12);
    gtk_widget_set_margin_end(wrapper, 12);
    gtk_widget_set_margin_top(wrapper, 12);
    gtk_widget_set_margin_bottom(wrapper, 12);

    /* Main content */
    pw->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *btn_lock = power_make_button("fl-lock-symbolic", "Lock Screen",
                                             G_CALLBACK(on_lock_clicked), pw);
    GtkWidget *btn_suspend = power_make_button("fl-sleep-symbolic", "Suspend",
                                                G_CALLBACK(on_suspend_clicked), pw);
    GtkWidget *btn_hibernate = power_make_button("fl-sleep-symbolic", "Hibernate",
                                                  G_CALLBACK(on_hibernate_clicked), pw);
    GtkWidget *btn_reboot = power_make_button("fl-restart-symbolic", "Reboot",
                                               G_CALLBACK(on_reboot_clicked), pw);
    GtkWidget *btn_poweroff = power_make_button("fl-power-symbolic", "Power Off",
                                                 G_CALLBACK(on_poweroff_clicked), pw);
    GtkWidget *btn_logout = power_make_button("fl-logout-symbolic", "Log Out",
                                               G_CALLBACK(on_logout_clicked), pw);

    gtk_box_append(GTK_BOX(pw->main_box), btn_lock);
    gtk_box_append(GTK_BOX(pw->main_box), btn_suspend);
    gtk_box_append(GTK_BOX(pw->main_box), btn_hibernate);
    gtk_box_append(GTK_BOX(pw->main_box), btn_reboot);
    gtk_box_append(GTK_BOX(pw->main_box), btn_poweroff);
    gtk_box_append(GTK_BOX(pw->main_box), btn_logout);

    gtk_box_append(GTK_BOX(pw->main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    pw->uptime_label = gtk_label_new("Uptime: --");
    gtk_label_set_xalign(GTK_LABEL(pw->uptime_label), 0.0);
    gtk_box_append(GTK_BOX(pw->main_box), pw->uptime_label);

    /* Confirmation content */
    pw->confirm_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *confirm_label = gtk_label_new("Are you sure?");
    gtk_widget_add_css_class(confirm_label, "heading");
    gtk_box_append(GTK_BOX(pw->confirm_box), confirm_label);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_yes = gtk_button_new_with_label("Yes");
    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(btn_yes, "destructive-action");

    g_signal_connect(btn_yes, "clicked", G_CALLBACK(on_confirm_yes), pw);
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_confirm_cancel), pw);

    gtk_box_append(GTK_BOX(btn_row), btn_yes);
    gtk_box_append(GTK_BOX(btn_row), btn_cancel);
    gtk_box_append(GTK_BOX(pw->confirm_box), btn_row);

    gtk_widget_set_visible(pw->confirm_box, FALSE);

    gtk_box_append(GTK_BOX(wrapper), pw->main_box);
    gtk_box_append(GTK_BOX(wrapper), pw->confirm_box);

    /* Popover */
    pw->popover = gtk_popover_new();
    gtk_widget_add_css_class(pw->popover, "shell-popover");
    gtk_popover_set_child(GTK_POPOVER(pw->popover), wrapper);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(pw->button), pw->popover);

    g_signal_connect(pw->popover, "show", G_CALLBACK(on_popover_show), pw);

    return (ShellWidget *)pw;
}

static void power_destroy(ShellWidget *widget)
{
    PowerWidget *pw = (PowerWidget *)widget;
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

    if (!pw->logind_proxy) {
        if (!pw->cancellable) {
            pw->cancellable = g_cancellable_new();
        }
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
