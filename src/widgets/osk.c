#include "widget.h"
#include "../shell.h"
#include "../osk/osk.h"
#include "../ipc/socket.h"
#include <gtk/gtk.h>

extern const ShellWidgetClass osk_widget_class;

typedef struct {
    ShellWidget base;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
} OskWidget;

static void
on_osk_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    OskWidget *ow = user_data;
    if (!ow || !ow->base.klass)
        return;

    if (ow->base.app && ow->base.app->osk) {
        shell_osk_toggle(ow->base.app->osk);
    } else {
        shell_ipc_send_command("toggle-osk");
    }
}

static ShellWidget *
osk_widget_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    OskWidget *ow = g_new0(OskWidget, 1);
    ow->base.klass = &osk_widget_class;

    ow->button = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(ow->button), FALSE);
    gtk_widget_set_tooltip_text(ow->button, "Toggle Virtual Keyboard");
    gtk_widget_add_css_class(ow->button, "shell-widget");
    gtk_widget_add_css_class(ow->button, "shell-widget-osk");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ow->icon_img = gtk_image_new_from_icon_name("input-keyboard-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ow->icon_img), shell_widget_get_icon_size((ShellWidget *)ow));
    ow->label = gtk_label_new("Keyboard");

    gtk_box_append(GTK_BOX(hbox), ow->icon_img);
    gtk_box_append(GTK_BOX(hbox), ow->label);
    gtk_button_set_child(GTK_BUTTON(ow->button), hbox);

    g_signal_connect(ow->button, "clicked", G_CALLBACK(on_osk_btn_clicked), ow);

    return (ShellWidget *)ow;
}

static void
osk_widget_destroy(ShellWidget *widget)
{
    OskWidget *ow = (OskWidget *)widget;
    if (ow == NULL)
        return;
    if (ow->button)
        g_signal_handlers_disconnect_by_data(ow->button, ow);
    g_free(ow);
}

static GtkWidget *
osk_widget_get_widget(ShellWidget *widget)
{
    OskWidget *ow = (OskWidget *)widget;
    return ow->button;
}

static void
osk_widget_enable(ShellWidget *widget)
{
    OskWidget *ow = (OskWidget *)widget;
    if (ow) {
        if (ow->icon_img)
            gtk_image_set_pixel_size(GTK_IMAGE(ow->icon_img), shell_widget_get_icon_size(widget));
        shell_widget_apply_mode_visibility(widget->mode, ow->icon_img, ow->label);
    }
}

static void
osk_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass osk_widget_class = {
    .id = "osk",
    .name = "Virtual Keyboard",
    .create = osk_widget_create,
    .destroy = osk_widget_destroy,
    .get_widget = osk_widget_get_widget,
    .enable = osk_widget_enable,
    .disable = osk_widget_disable,
};
