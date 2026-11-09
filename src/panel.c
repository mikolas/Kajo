#include "panel.h"
#include "shell.h"
#include "config.h"
#include "autohide.h"
#include "widgets/registry.h"
#include "launcher/launcher_surface.h"
#include "osk/osk.h"
#include "compositor/compositor.h"

#include <gtk4-layer-shell.h>
#include <math.h>
#include <string.h>

static gboolean
is_event_on_panel_surface(GtkEventControllerMotion *controller, ShellPanel *panel)
{
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (!event || !panel || !panel->window)
        return TRUE;

    GdkSurface *ev_surface = gdk_event_get_surface(event);
    GdkSurface *win_surface = gtk_native_get_surface(GTK_NATIVE(panel->window));
    if (ev_surface && win_surface && ev_surface != win_surface)
        return FALSE;

    return TRUE;
}

static void
on_panel_enter(GtkEventControllerMotion *controller,
               gdouble                   x,
               gdouble                   y,
               gpointer                  user_data)
{
    (void)x;
    (void)y;

    ShellPanel *panel = user_data;
    if (!panel || !panel->autohide)
        return;

    if (!is_event_on_panel_surface(controller, panel))
        return;

    shell_autohide_enter(panel->autohide);
}

static void
on_panel_motion(GtkEventControllerMotion *controller,
                gdouble                   x,
                gdouble                   y,
                gpointer                  user_data)
{
    (void)x;
    (void)y;

    ShellPanel *panel = user_data;
    if (!panel || !panel->autohide)
        return;

    if (!is_event_on_panel_surface(controller, panel))
        return;

    shell_autohide_enter(panel->autohide);
}

static void
on_panel_leave(GtkEventControllerMotion *controller,
               gpointer                  user_data)
{
    (void)controller;

    ShellPanel *panel = user_data;
    if (panel && panel->autohide)
        shell_autohide_leave(panel->autohide);
}

static GtkMenuButton *
find_menu_button_recursive(GtkWidget *widget)
{
    if (!widget)
        return NULL;
    if (GTK_IS_MENU_BUTTON(widget))
        return GTK_MENU_BUTTON(widget);

    for (GtkWidget *child = gtk_widget_get_first_child(widget);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkMenuButton *found = find_menu_button_recursive(child);
        if (found)
            return found;
    }
    return NULL;
}

static gboolean
open_panel_widget_popover_by_id(ShellPanel *panel, const gchar *widget_id)
{
    if (!panel || !panel->app || !panel->app->active_widgets || !widget_id)
        return FALSE;

    for (guint i = 0; i < panel->app->active_widgets->len; i++) {
        ShellWidget *w = g_ptr_array_index(panel->app->active_widgets, i);
        if (!w || !w->klass || g_strcmp0(w->klass->id, widget_id) != 0)
            continue;

        GtkWidget *root = w->klass->get_widget(w);
        GtkMenuButton *mb = find_menu_button_recursive(root);
        if (mb) {
            GtkPopover *pop = gtk_menu_button_get_popover(mb);
            if (pop) {
                gtk_popover_popup(pop);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void
close_popovers_recursive(GtkWidget *widget)
{
    if (!widget)
        return;

    if (GTK_IS_MENU_BUTTON(widget)) {
        GtkPopover *pop = gtk_menu_button_get_popover(GTK_MENU_BUTTON(widget));
        if (pop && gtk_widget_get_visible(GTK_WIDGET(pop))) {
            gtk_popover_popdown(pop);
        }
    }

    for (GtkWidget *child = gtk_widget_get_first_child(widget);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        close_popovers_recursive(child);
    }
}

static void
close_all_panel_popovers(ShellPanel *panel)
{
    if (!panel || !panel->container)
        return;
    close_popovers_recursive(panel->container);
}

static void
on_panel_swipe_begin(GtkGesture       *gesture,
                     GdkEventSequence *sequence,
                     gpointer          user_data)
{
    ShellPanel *panel = (ShellPanel *)user_data;
    if (!panel || !panel->app)
        return;

    ShellConfig *cfg = shell_app_get_config(panel->app);
    if (!cfg || !cfg->touch_gestures_enabled)
        return;

    gdouble sx = 0.0, sy = 0.0;
    if (gtk_gesture_get_point(gesture, sequence, &sx, &sy)) {
        panel->swipe_start_x = sx;
    }

    panel->swipe_started_while_hidden = FALSE;
    if (panel->autohide && panel->autohide->enabled) {
        if (panel->autohide->state == AUTOHIDE_HIDDEN ||
            panel->autohide->state == AUTOHIDE_HIDING) {
            panel->swipe_started_while_hidden = TRUE;
        }
        shell_autohide_touch_reveal(panel->autohide, cfg->touch_dwell_ms);
    }
}

static void
on_panel_swipe(GtkGestureSwipe *gesture,
               gdouble          vx,
               gdouble          vy,
               gpointer         user_data)
{
    ShellPanel *panel = (ShellPanel *)user_data;
    if (!panel || !panel->app)
        return;

    ShellConfig *cfg = shell_app_get_config(panel->app);
    if (!cfg || !cfg->touch_gestures_enabled)
        return;

    gdouble abs_vx = fabs(vx);
    gdouble abs_vy = fabs(vy);

    /* Horizontal Swipe across the panel (Column or Workspace switch) */
    if (abs_vx > 1.35 * abs_vy && abs_vx >= 200.0) {
        const gchar *horiz_mode = cfg->touch_swipe_horizontal ? cfg->touch_swipe_horizontal : "column";
        if (g_strcmp0(horiz_mode, "workspace") == 0) {
            if (vx < 0.0)
                niri_send_action("{\"Action\":{\"FocusWorkspaceDown\":{}}}");
            else
                niri_send_action("{\"Action\":{\"FocusWorkspaceUp\":{}}}");
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        } else if (g_strcmp0(horiz_mode, "column") == 0) {
            if (vx < 0.0)
                niri_send_action("{\"Action\":{\"FocusColumnRight\":{}}}");
            else
                niri_send_action("{\"Action\":{\"FocusColumnLeft\":{}}}");
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        }

        if (panel->autohide && panel->autohide->enabled)
            shell_autohide_touch_reveal(panel->autohide, cfg->touch_dwell_ms);
        return;
    }

    /* Vertical Swipe on the panel (Inward reveal/open vs Outward dismiss/hide) */
    if (abs_vy > 1.2 * abs_vx && abs_vy >= 180.0) {
        gboolean is_bottom = (cfg->panel_position == PANEL_POSITION_BOTTOM);
        gboolean inward  = is_bottom ? (vy < 0.0) : (vy > 0.0);
        gboolean outward = !inward;

        if (outward) {
            close_all_panel_popovers(panel);
            if (panel->app->launcher)
                shell_launcher_surface_hide(panel->app->launcher);
            if (panel->app->osk)
                shell_osk_hide(panel->app->osk);
            if (panel->autohide && panel->autohide->enabled)
                shell_autohide_force_hide(panel->autohide);
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            return;
        }

        /* If the panel was hidden and the user did a gentle edge swipe, just reveal the panel */
        if (panel->swipe_started_while_hidden && abs_vy < 750.0) {
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            return;
        }

        int width = panel->window ? gtk_widget_get_width(GTK_WIDGET(panel->window)) : 0;
        gboolean is_right_zone = (width > 0 && panel->swipe_start_x >= (gdouble)width * 0.66);

        if (is_right_zone) {
            const gchar *right_act = cfg->touch_swipe_right ? cfg->touch_swipe_right : "control-center";
            if (g_strcmp0(right_act, "control-center") == 0) {
                if (!open_panel_widget_popover_by_id(panel, "control-center"))
                    open_panel_widget_popover_by_id(panel, "notifications");
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            } else if (g_strcmp0(right_act, "notifications") == 0) {
                open_panel_widget_popover_by_id(panel, "notifications");
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            } else if (g_strcmp0(right_act, "osk") == 0) {
                if (panel->app->osk)
                    shell_osk_toggle(panel->app->osk);
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            }
        } else {
            const gchar *center_act = cfg->touch_swipe_center ? cfg->touch_swipe_center : "launcher";
            if (g_strcmp0(center_act, "launcher") == 0) {
                if (panel->app->launcher)
                    shell_launcher_surface_toggle(panel->app->launcher);
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            } else if (g_strcmp0(center_act, "overview") == 0) {
                niri_send_action("{\"Action\":{\"ToggleOverview\":{}}}");
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            } else if (g_strcmp0(center_act, "osk") == 0) {
                if (panel->app->osk)
                    shell_osk_toggle(panel->app->osk);
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            }
        }
    }
}

static GtkLayerShellLayer
parse_panel_layer(const gchar *layer_str)
{
    if (g_strcmp0(layer_str, "overlay") == 0)
        return GTK_LAYER_SHELL_LAYER_OVERLAY;
    if (g_strcmp0(layer_str, "bottom") == 0)
        return GTK_LAYER_SHELL_LAYER_BOTTOM;
    if (g_strcmp0(layer_str, "background") == 0)
        return GTK_LAYER_SHELL_LAYER_BACKGROUND;
    return GTK_LAYER_SHELL_LAYER_TOP;
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
    g_object_set_data(G_OBJECT(panel->window), "kajo-shell-panel", panel);
    gtk_widget_add_css_class(GTK_WIDGET(panel->window), "shell-panel");

    /* Configure gtk4-layer-shell */
    gtk_layer_init_for_window(panel->window);
    gtk_layer_set_layer(panel->window, parse_panel_layer(config->panel_layer));
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
    g_signal_connect(motion, "motion", G_CALLBACK(on_panel_motion), panel);
    g_signal_connect(motion, "leave", G_CALLBACK(on_panel_leave), panel);
    gtk_widget_add_controller(GTK_WIDGET(panel->window), motion);

    /* Set up touch/swipe gesture controller for panel & edge gestures */
    GtkGesture *swipe = gtk_gesture_swipe_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(swipe), GTK_PHASE_CAPTURE);
    g_signal_connect(swipe, "begin", G_CALLBACK(on_panel_swipe_begin), panel);
    g_signal_connect(swipe, "swipe", G_CALLBACK(on_panel_swipe), panel);
    gtk_widget_add_controller(GTK_WIDGET(panel->window), GTK_EVENT_CONTROLLER(swipe));

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
            gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
        }
    } else if (GTK_IS_BUTTON(widget)) {
        GtkWidget *child = gtk_button_get_child(GTK_BUTTON(widget));
        if (child) {
            gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
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
on_popover_visible_changed(GObject *gobject, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    if (!user_data) return;
    ShellPanel *panel = g_object_get_data(G_OBJECT(user_data), "kajo-shell-panel");
    if (!panel || !panel->autohide) return;

    gboolean visible = gtk_widget_get_visible(GTK_WIDGET(gobject));
    gboolean was_locked = GPOINTER_TO_INT(g_object_get_data(gobject, "kajo-autohide-locked"));

    if (visible && !was_locked) {
        g_object_set_data(gobject, "kajo-autohide-locked", GINT_TO_POINTER(1));
        shell_autohide_lock(panel->autohide);
    } else if (!visible && was_locked) {
        g_object_set_data(gobject, "kajo-autohide-locked", GINT_TO_POINTER(0));
        shell_autohide_unlock(panel->autohide);
    }
}

static void
on_popover_unmapped_or_destroyed(GtkWidget *widget, gpointer user_data)
{
    if (!user_data) return;
    ShellPanel *panel = g_object_get_data(G_OBJECT(user_data), "kajo-shell-panel");
    gboolean was_locked = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "kajo-autohide-locked"));
    if (was_locked) {
        g_object_set_data(G_OBJECT(widget), "kajo-autohide-locked", GINT_TO_POINTER(0));
        if (panel && panel->autohide)
            shell_autohide_unlock(panel->autohide);
    }
}

void
shell_panel_register_popover(ShellPanel *panel, GtkPopover *popover)
{
    if (!panel || !panel->window || !popover) return;
    if (g_object_get_data(G_OBJECT(popover), "kajo-popover-registered"))
        return;
    g_object_set_data(G_OBJECT(popover), "kajo-popover-registered", GINT_TO_POINTER(1));
    g_signal_connect_object(popover, "notify::visible",
                            G_CALLBACK(on_popover_visible_changed), panel->window, 0);
    g_signal_connect_object(popover, "unmap",
                            G_CALLBACK(on_popover_unmapped_or_destroyed), panel->window, 0);
    g_signal_connect_object(popover, "destroy",
                            G_CALLBACK(on_popover_unmapped_or_destroyed), panel->window, 0);
}

void
shell_panel_reload_config(ShellPanel *panel, ShellConfig *config)
{
    if (!panel || !config)
        return;

    if (panel->window) {
        gtk_layer_set_layer(panel->window, parse_panel_layer(config->panel_layer));
        if (config->panel_position == PANEL_POSITION_TOP) {
            gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
            gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        } else {
            gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_TOP, FALSE);
            gtk_layer_set_anchor(panel->window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        }
        gtk_widget_set_size_request(GTK_WIDGET(panel->window), -1,
                                    config->panel_height > 0 ? config->panel_height : 32);
    }

    if (panel->autohide) {
        shell_autohide_reload_config(panel->autohide, config);
    }
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

    if (panel->window) {
        g_object_set_data(G_OBJECT(panel->window), "kajo-shell-panel", NULL);
    }

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
