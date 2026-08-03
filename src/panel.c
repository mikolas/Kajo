#include "panel.h"
#include "shell.h"
#include "config.h"
#include "autohide.h"

#include <gtk4-layer-shell.h>

static void
on_panel_enter(GtkEventControllerMotion *controller,
               gdouble                   x,
               gdouble                   y,
               gpointer                  user_data)
{
    (void)controller;
    (void)x;
    (void)y;

    ShellPanel *panel = user_data;
    if (panel->autohide)
        shell_autohide_enter(panel->autohide);
}

static void
on_panel_leave(GtkEventControllerMotion *controller,
               gpointer                  user_data)
{
    (void)controller;

    ShellPanel *panel = user_data;
    if (panel->autohide)
        shell_autohide_leave(panel->autohide);
}

ShellPanel *
shell_panel_new(ShellApp *app)
{
    ShellPanel *panel = g_new0(ShellPanel, 1);
    ShellConfig *config = shell_app_get_config(app);

    panel->app = app;
    panel->widgets = g_ptr_array_new();

    /* Create the panel window */
    panel->window = GTK_WINDOW(gtk_window_new());
    gtk_widget_add_css_class(GTK_WIDGET(panel->window), "shell-panel");

    /* Configure gtk4-layer-shell */
    gtk_layer_init_for_window(panel->window);
    gtk_layer_set_layer(panel->window, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(panel->window, "kajo");

    /* Anchor to top-left and top-right to stretch across the top */
    if (config->panel_position == PANEL_POSITION_TOP) {
        gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    } else {
        gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    }
    gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

    /* Set exclusive zone to 0 (Overlay Mode: panel floats over windows without resizing them) */
    gtk_layer_set_exclusive_zone(panel->window, 0);

    /* Set panel height */
    gtk_widget_set_size_request(GTK_WIDGET(panel->window), -1, config->panel_height);

    /* Create the main GtkCenterBox container */
    panel->container = gtk_center_box_new();
    gtk_widget_set_name(panel->container, "panel-container");

    /* Left box */
    panel->box_left = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    gtk_widget_add_css_class(GTK_WIDGET(panel->box_left), "shell-panel-left");
    gtk_widget_set_halign(GTK_WIDGET(panel->box_left), GTK_ALIGN_START);

    /* Center box */
    panel->box_center = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    gtk_widget_add_css_class(GTK_WIDGET(panel->box_center), "shell-panel-center");
    gtk_widget_set_halign(GTK_WIDGET(panel->box_center), GTK_ALIGN_CENTER);

    /* Right box */
    panel->box_right = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    gtk_widget_add_css_class(GTK_WIDGET(panel->box_right), "shell-panel-right");
    gtk_widget_set_halign(GTK_WIDGET(panel->box_right), GTK_ALIGN_END);

    /* Attach left, center, right to GtkCenterBox */
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(panel->container), GTK_WIDGET(panel->box_left));
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(panel->container), GTK_WIDGET(panel->box_center));
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(panel->container), GTK_WIDGET(panel->box_right));

    gtk_window_set_child(panel->window, panel->container);

    /* Set up motion event controller for autohide */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_panel_enter), panel);
    g_signal_connect(motion, "leave", G_CALLBACK(on_panel_leave), panel);
    gtk_widget_add_controller(GTK_WIDGET(panel->window), motion);

    /* Initialize autohide */
    panel->autohide = shell_autohide_new(panel, config);

    return panel;
}

static void
auto_register_popovers_in_widget(ShellPanel *panel, GtkWidget *widget)
{
    if (!widget) return;

    if (GTK_IS_MENU_BUTTON(widget)) {
        GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(widget));
        if (popover) {
            shell_panel_register_popover(panel, popover);
        }
        GtkWidget *child = gtk_menu_button_get_child(GTK_MENU_BUTTON(widget));
        if (child) {
            gtk_widget_set_halign(child, GTK_ALIGN_FILL);
            gtk_widget_set_valign(child, GTK_ALIGN_FILL);
        }
    }

    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        auto_register_popovers_in_widget(panel, child);
    }
}

void
shell_panel_add_widget(ShellPanel          *panel,
                       PanelWidgetPosition  position,
                       GtkWidget           *widget)
{
    if (!panel || !widget)
        return;

    gtk_widget_set_valign(widget, GTK_ALIGN_FILL);

    g_ptr_array_add(panel->widgets, widget);

    switch (position) {
    case PANEL_WIDGET_LEFT:
        gtk_box_append(panel->box_left, widget);
        break;
    case PANEL_WIDGET_CENTER:
        gtk_box_append(panel->box_center, widget);
        break;
    case PANEL_WIDGET_RIGHT:
        gtk_box_append(panel->box_right, widget);
        break;
    }

    auto_register_popovers_in_widget(panel, widget);
}

static void
on_popover_visible_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data)
{
    ShellPanel *panel = user_data;
    if (!panel || !panel->autohide) return;

    if (gtk_widget_get_visible(GTK_WIDGET(gobject))) {
        shell_autohide_lock(panel->autohide);
    } else {
        shell_autohide_unlock(panel->autohide);
    }
}

void
shell_panel_register_popover(ShellPanel *panel, GtkPopover *popover)
{
    if (!panel || !popover) return;
    g_signal_connect(popover, "notify::visible", G_CALLBACK(on_popover_visible_changed), panel);
}

void
shell_panel_show(ShellPanel *panel)
{
    if (!panel)
        return;
    gtk_window_present(panel->window);
}

void
shell_panel_destroy(ShellPanel *panel)
{
    if (!panel)
        return;

    if (panel->autohide) {
        shell_autohide_destroy(panel->autohide);
        panel->autohide = NULL;
    }

    if (panel->widgets)
        g_ptr_array_free(panel->widgets, TRUE);

    if (panel->window)
        gtk_window_destroy(panel->window);

    g_free(panel);
}

GtkWindow *
shell_panel_get_window(ShellPanel *panel)
{
    return panel ? panel->window : NULL;
}

GtkWidget *
shell_panel_get_container(ShellPanel *panel)
{
    return panel ? panel->container : NULL;
}

ShellAutohide *
shell_panel_get_autohide(ShellPanel *panel)
{
    return panel ? panel->autohide : NULL;
}
