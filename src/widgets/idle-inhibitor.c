#include "widget.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

extern const ShellWidgetClass idle_inhibitor_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;

    GtkWidget *toggle_switch;
    GtkWidget *status_label;

    gboolean is_inhibited;
} IdleInhibitorWidget;

static void update_inhibitor_ui(IdleInhibitorWidget *in);

static void
on_inhibitor_switch_toggled(GtkSwitch *sw, gboolean state, gpointer user_data)
{
    IdleInhibitorWidget *in = user_data;
    in->is_inhibited = state;
    update_inhibitor_ui(in);
}

static void
update_inhibitor_ui(IdleInhibitorWidget *in)
{
    const gchar *icon_name = "tb-coffee-symbolic";

    gtk_image_set_from_icon_name(GTK_IMAGE(in->icon_img), icon_name);
    gtk_label_set_text(GTK_LABEL(in->label), in->is_inhibited ? "Awake" : "Idle");
    shell_widget_apply_mode_visibility(in->base.mode, in->icon_img, in->label);

    if (in->toggle_switch != NULL) {
        g_signal_handlers_block_by_func(in->toggle_switch, on_inhibitor_switch_toggled, in);
        gtk_switch_set_active(GTK_SWITCH(in->toggle_switch), in->is_inhibited);
        g_signal_handlers_unblock_by_func(in->toggle_switch, on_inhibitor_switch_toggled, in);
    }

    if (in->status_label != NULL) {
        gtk_label_set_text(GTK_LABEL(in->status_label),
                           in->is_inhibited ? "[ AWAKE ]" : "[ IDLE ]");
        if (in->is_inhibited)
            gtk_widget_add_css_class(in->status_label, "active");
        else
            gtk_widget_remove_css_class(in->status_label, "active");
    }
}

static GtkWidget *
build_inhibitor_popover(IdleInhibitorWidget *in)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "shell-popover");

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(main_box, "popover-compact");

    /* ─── Zone 1: Header (Title + Status Badge) ─── */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_box, "shell-popover-header");

    GtkWidget *title_label = gtk_label_new("IDLE INHIBITOR");
    gtk_widget_add_css_class(title_label, "shell-popover-title");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_widget_set_hexpand(title_label, TRUE);

    in->status_label = gtk_label_new("[ IDLE ]");
    gtk_widget_add_css_class(in->status_label, "shell-popover-badge");

    gtk_box_append(GTK_BOX(header_box), title_label);
    gtk_box_append(GTK_BOX(header_box), in->status_label);
    gtk_box_append(GTK_BOX(main_box), header_box);

    /* ─── Zone 2: Body (Inhibit Switch Row) ─── */
    GtkWidget *body_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(body_box, "shell-popover-body");

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row_label = gtk_label_new("Prevent Screen Sleep");
    gtk_widget_set_hexpand(row_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(row_label), 0.0f);

    in->toggle_switch = gtk_switch_new();
    g_signal_connect(in->toggle_switch, "state-set",
                     G_CALLBACK(on_inhibitor_switch_toggled), in);

    gtk_box_append(GTK_BOX(row), row_label);
    gtk_box_append(GTK_BOX(row), in->toggle_switch);
    gtk_box_append(GTK_BOX(body_box), row);
    gtk_box_append(GTK_BOX(main_box), body_box);

    /* ─── Zone 3: Footer (Actions) ─── */
    GtkWidget *footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(footer_box, "shell-popover-footer");

    GtkWidget *settings_btn = gtk_button_new_with_label("POWER SETTINGS");
    gtk_button_set_has_frame(GTK_BUTTON(settings_btn), FALSE);
    gtk_widget_set_halign(settings_btn, GTK_ALIGN_END);
    gtk_widget_set_hexpand(settings_btn, TRUE);

    gtk_box_append(GTK_BOX(footer_box), settings_btn);
    gtk_box_append(GTK_BOX(main_box), footer_box);

    gtk_popover_set_child(GTK_POPOVER(popover), main_box);
    return popover;
}

static ShellWidget *
idle_inhibitor_widget_create(ShellCompositor *compositor)
{
    IdleInhibitorWidget *in = g_new0(IdleInhibitorWidget, 1);
    in->base.klass = &idle_inhibitor_widget_class;
    in->compositor = compositor;
    in->is_inhibited = FALSE;

    /* Build Button */
    in->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(in->button), FALSE);
    gtk_widget_add_css_class(in->button, "shell-widget");
    gtk_widget_add_css_class(in->button, "shell-widget-idle-inhibitor");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    in->icon_img = gtk_image_new_from_icon_name("fl-coffee-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(in->icon_img), shell_widget_get_icon_size((ShellWidget *)in));
    in->label = gtk_label_new("Idle");

    gtk_box_append(GTK_BOX(hbox), in->icon_img);
    gtk_box_append(GTK_BOX(hbox), in->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(in->button), hbox);

    /* Build Popover */
    in->popover = build_inhibitor_popover(in);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(in->button), in->popover);

    update_inhibitor_ui(in);

    return (ShellWidget *)in;
}

static void
idle_inhibitor_widget_destroy(ShellWidget *widget)
{
    IdleInhibitorWidget *in = (IdleInhibitorWidget *)widget;
    if (in == NULL)
        return;
    g_free(in);
}

static GtkWidget *
idle_inhibitor_widget_get_widget(ShellWidget *widget)
{
    IdleInhibitorWidget *in = (IdleInhibitorWidget *)widget;
    return in->button;
}

static void
idle_inhibitor_widget_enable(ShellWidget *widget)
{
    IdleInhibitorWidget *in = (IdleInhibitorWidget *)widget;
    if (in) {
        shell_widget_apply_mode_visibility(widget->mode, in->icon_img, in->label);
    }
}

static void
idle_inhibitor_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass idle_inhibitor_widget_class = {
    .id = "idle-inhibitor",
    .name = "Idle Inhibitor",
    .create = idle_inhibitor_widget_create,
    .destroy = idle_inhibitor_widget_destroy,
    .get_widget = idle_inhibitor_widget_get_widget,
    .enable = idle_inhibitor_widget_enable,
    .disable = idle_inhibitor_widget_disable,
};
