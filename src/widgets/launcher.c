#include "widget.h"
#include "../shell.h"
#include "../launcher/launcher_surface.h"
#include "../ipc/socket.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

extern const ShellWidgetClass launcher_widget_class;

typedef struct {
    ShellWidget base;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
} LauncherWidget;

static void
on_launcher_btn_clicked(GtkButton *btn, gpointer user_data)
{
    LauncherWidget *lw = user_data;
    if (lw && lw->base.klass) {
        /* Trigger launcher toggle via IPC/app context */
        shell_ipc_send_command("launcher-toggle");
    }
}

static ShellWidget *
launcher_widget_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    LauncherWidget *lw = g_new0(LauncherWidget, 1);
    lw->base.klass = &launcher_widget_class;

    lw->button = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(lw->button), FALSE);
    gtk_widget_add_css_class(lw->button, "shell-widget");
    gtk_widget_add_css_class(lw->button, "shell-widget-launcher");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    lw->icon_img = gtk_image_new_from_icon_name("fl-apps-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(lw->icon_img), shell_widget_get_icon_size((ShellWidget *)lw));
    lw->label = gtk_label_new("Start");

    gtk_box_append(GTK_BOX(hbox), lw->icon_img);
    gtk_box_append(GTK_BOX(hbox), lw->label);
    gtk_button_set_child(GTK_BUTTON(lw->button), hbox);

    g_signal_connect(lw->button, "clicked", G_CALLBACK(on_launcher_btn_clicked), lw);

    return (ShellWidget *)lw;
}

static void
launcher_widget_destroy(ShellWidget *widget)
{
    LauncherWidget *lw = (LauncherWidget *)widget;
    if (lw == NULL)
        return;
    g_free(lw);
}

static GtkWidget *
launcher_widget_get_widget(ShellWidget *widget)
{
    LauncherWidget *lw = (LauncherWidget *)widget;
    return lw->button;
}

static void
launcher_widget_enable(ShellWidget *widget)
{
    LauncherWidget *lw = (LauncherWidget *)widget;
    if (lw) {
        shell_widget_apply_mode_visibility(widget->mode, lw->icon_img, lw->label);
    }
}

static void
launcher_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass launcher_widget_class = {
    .id = "launcher",
    .name = "Launcher Start Button",
    .create = launcher_widget_create,
    .destroy = launcher_widget_destroy,
    .get_widget = launcher_widget_get_widget,
    .enable = launcher_widget_enable,
    .disable = launcher_widget_disable,
};
