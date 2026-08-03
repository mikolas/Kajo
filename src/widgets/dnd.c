#include "widget.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

extern const ShellWidgetClass dnd_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;

    GtkWidget *dnd_switch;
    GtkWidget *status_label;

    gboolean is_dnd;
} DndWidget;

static void update_dnd_ui(DndWidget *dnd);

static void
on_dnd_switch_toggled(GtkSwitch *sw, gboolean state, gpointer user_data)
{
    DndWidget *dnd = user_data;
    dnd->is_dnd = state;
    update_dnd_ui(dnd);
}

static void
update_dnd_ui(DndWidget *dnd)
{
    const gchar *icon_name = dnd->is_dnd ? "tb-bell-off-symbolic" : "tb-bell-symbolic";

    gtk_image_set_from_icon_name(GTK_IMAGE(dnd->icon_img), icon_name);
    gtk_label_set_text(GTK_LABEL(dnd->label), dnd->is_dnd ? "DND On" : "DND Off");
    shell_widget_apply_mode_visibility(dnd->base.mode, dnd->icon_img, dnd->label);

    if (dnd->dnd_switch != NULL) {
        g_signal_handlers_block_by_func(dnd->dnd_switch, on_dnd_switch_toggled, dnd);
        gtk_switch_set_active(GTK_SWITCH(dnd->dnd_switch), dnd->is_dnd);
        g_signal_handlers_unblock_by_func(dnd->dnd_switch, on_dnd_switch_toggled, dnd);
    }

    if (dnd->status_label != NULL) {
        gtk_label_set_text(GTK_LABEL(dnd->status_label),
                           dnd->is_dnd ? "[ MUTED ]" : "[ ACTIVE ]");
        if (dnd->is_dnd)
            gtk_widget_add_css_class(dnd->status_label, "urgent");
        else
            gtk_widget_remove_css_class(dnd->status_label, "urgent");
    }
}

static GtkWidget *
build_dnd_popover(DndWidget *dnd)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "shell-popover");

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(main_box, "popover-compact");

    /* ─── Zone 1: Header (Title + Status Badge) ─── */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_box, "shell-popover-header");

    GtkWidget *title_label = gtk_label_new("DO NOT DISTURB");
    gtk_widget_add_css_class(title_label, "shell-popover-title");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_widget_set_hexpand(title_label, TRUE);

    dnd->status_label = gtk_label_new("[ ACTIVE ]");
    gtk_widget_add_css_class(dnd->status_label, "shell-popover-badge");

    gtk_box_append(GTK_BOX(header_box), title_label);
    gtk_box_append(GTK_BOX(header_box), dnd->status_label);
    gtk_box_append(GTK_BOX(main_box), header_box);

    /* ─── Zone 2: Body (Mute Notifications Switch) ─── */
    GtkWidget *body_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(body_box, "shell-popover-body");

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row_label = gtk_label_new("Mute Notifications");
    gtk_widget_set_hexpand(row_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(row_label), 0.0f);

    dnd->dnd_switch = gtk_switch_new();
    g_signal_connect(dnd->dnd_switch, "state-set",
                     G_CALLBACK(on_dnd_switch_toggled), dnd);

    gtk_box_append(GTK_BOX(row), row_label);
    gtk_box_append(GTK_BOX(row), dnd->dnd_switch);
    gtk_box_append(GTK_BOX(body_box), row);
    gtk_box_append(GTK_BOX(main_box), body_box);

    /* ─── Zone 3: Footer (Actions) ─── */
    GtkWidget *footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(footer_box, "shell-popover-footer");

    GtkWidget *settings_btn = gtk_button_new_with_label("NOTIFICATION SETTINGS");
    gtk_button_set_has_frame(GTK_BUTTON(settings_btn), FALSE);
    gtk_widget_set_halign(settings_btn, GTK_ALIGN_END);
    gtk_widget_set_hexpand(settings_btn, TRUE);

    gtk_box_append(GTK_BOX(footer_box), settings_btn);
    gtk_box_append(GTK_BOX(main_box), footer_box);

    gtk_popover_set_child(GTK_POPOVER(popover), main_box);
    return popover;
}

static ShellWidget *
dnd_widget_create(ShellCompositor *compositor)
{
    DndWidget *dnd = g_new0(DndWidget, 1);
    dnd->base.klass = &dnd_widget_class;
    dnd->compositor = compositor;
    dnd->is_dnd = FALSE;

    /* Build Button */
    dnd->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(dnd->button), FALSE);
    gtk_widget_add_css_class(dnd->button, "shell-widget");
    gtk_widget_add_css_class(dnd->button, "shell-widget-dnd");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    dnd->icon_img = gtk_image_new_from_icon_name("fl-bell-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(dnd->icon_img), shell_widget_get_icon_size((ShellWidget *)dnd));
    dnd->label = gtk_label_new("DND");

    gtk_box_append(GTK_BOX(hbox), dnd->icon_img);
    gtk_box_append(GTK_BOX(hbox), dnd->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(dnd->button), hbox);

    /* Build Popover */
    dnd->popover = build_dnd_popover(dnd);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(dnd->button), dnd->popover);

    update_dnd_ui(dnd);

    return (ShellWidget *)dnd;
}

static void
dnd_widget_destroy(ShellWidget *widget)
{
    DndWidget *dnd = (DndWidget *)widget;
    if (dnd == NULL)
        return;
    g_free(dnd);
}

static GtkWidget *
dnd_widget_get_widget(ShellWidget *widget)
{
    DndWidget *dnd = (DndWidget *)widget;
    return dnd->button;
}

static void
dnd_widget_enable(ShellWidget *widget)
{
    DndWidget *dnd = (DndWidget *)widget;
    if (dnd) {
        shell_widget_apply_mode_visibility(widget->mode, dnd->icon_img, dnd->label);
    }
}

static void
dnd_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass dnd_widget_class = {
    .id = "dnd",
    .name = "Do Not Disturb",
    .create = dnd_widget_create,
    .destroy = dnd_widget_destroy,
    .get_widget = dnd_widget_get_widget,
    .enable = dnd_widget_enable,
    .disable = dnd_widget_disable,
};
