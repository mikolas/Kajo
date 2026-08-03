#include "autohide.h"
#include "panel.h"
#include "config.h"

#include <gtk4-layer-shell.h>

static void
autohide_set_margin(ShellAutohide *self, gint margin)
{
    GtkWindow *window = shell_panel_get_window(self->panel);
    gtk_layer_set_margin(window, self->edge, margin);

    /* NOTE: exclusive zone is NOT updated here per-frame.
     * Updating it on every frame causes compositor layout reflows (niri
     * recalculates all window bounds) 60+ times/sec, leading to frame drops.
     * Instead, exclusive zone is set only at animation boundaries:
     *   - On reveal START: set to panel_height (so windows reflow once)
     *   - On hide END: set to 0 (windows reclaim space once)
     * See shell_autohide_trigger() and the tick callback completion. */
}

static gboolean
autohide_tick_callback(GtkWidget     *widget,
                       GdkFrameClock *frame_clock,
                       gpointer       user_data)
{
    (void)widget;

    ShellAutohide *self = user_data;
    gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    gint64 elapsed = now - self->anim_start_time;
    gdouble duration_us = (gdouble)self->reveal_duration * 1000.0; /* ms to us */

    gdouble progress = CLAMP((gdouble)elapsed / duration_us, 0.0, 1.0);

    /* Ease-out cubic */
    gdouble eased = 1.0 - (1.0 - progress) * (1.0 - progress) * (1.0 - progress);

    gint margin = self->anim_start_margin +
        (gint)((gdouble)(self->anim_target_margin - self->anim_start_margin) * eased);

    autohide_set_margin(self, margin);

    GtkWidget *container = shell_panel_get_container(self->panel);

    if (progress >= 1.0) {
        self->tick_callback_id = 0;

        if (self->state == AUTOHIDE_REVEALING) {
            self->state = AUTOHIDE_VISIBLE;
            if (container) {
                gtk_widget_remove_css_class(container, "autohide-hidden");
                gtk_widget_add_css_class(container, "autohide-visible");
            }
        } else if (self->state == AUTOHIDE_HIDING) {
            self->state = AUTOHIDE_HIDDEN;
            if (container) {
                gtk_widget_remove_css_class(container, "autohide-visible");
                gtk_widget_add_css_class(container, "autohide-hidden");
            }
        }

        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
autohide_start_animation(ShellAutohide *self, gint target_margin)
{
    GtkWindow *window = shell_panel_get_window(self->panel);
    GtkWidget *container = shell_panel_get_container(self->panel);

    if (container && target_margin == 0) {
        gtk_widget_remove_css_class(container, "autohide-hidden");
        gtk_widget_add_css_class(container, "autohide-visible");
    }

    /* Remove existing tick callback */
    if (self->tick_callback_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(window), self->tick_callback_id);
        self->tick_callback_id = 0;
    }

    self->anim_start_time = g_get_monotonic_time();
    self->anim_start_margin = gtk_layer_get_margin(window, self->edge);
    self->anim_target_margin = target_margin;

    self->tick_callback_id = gtk_widget_add_tick_callback(
        GTK_WIDGET(window), autohide_tick_callback, self, NULL);
}

static gboolean
on_hide_timeout(gpointer user_data)
{
    ShellAutohide *self = user_data;

    self->hide_timeout_id = 0;

    if (self->popover_count > 0)
        return G_SOURCE_REMOVE;

    if (self->state == AUTOHIDE_VISIBLE) {
        self->state = AUTOHIDE_HIDING;
        autohide_start_animation(self, -(self->panel_height - 2));
    }

    return G_SOURCE_REMOVE;
}

ShellAutohide *
shell_autohide_new(ShellPanel *panel, ShellConfig *config)
{
    ShellAutohide *self = g_new0(ShellAutohide, 1);

    self->panel           = panel;
    self->enabled         = config->autohide_enabled;
    self->hide_delay      = config->autohide_hide_delay;
    self->reveal_duration = config->autohide_reveal_duration;
    self->panel_height    = config->panel_height;
    self->edge            = (config->panel_position == PANEL_POSITION_BOTTOM)
                            ? GTK_LAYER_SHELL_EDGE_BOTTOM
                            : GTK_LAYER_SHELL_EDGE_TOP;
    self->state           = AUTOHIDE_VISIBLE;
    self->hide_timeout_id = 0;
    self->tick_callback_id = 0;
    self->popover_count   = 0;

    if (self->enabled) {
        /* Start hidden (keep 2px active edge at top for motion enter) */
        self->state = AUTOHIDE_HIDDEN;
        autohide_set_margin(self, -(self->panel_height - 2));
        GtkWidget *container = shell_panel_get_container(self->panel);
        if (container) {
            gtk_widget_remove_css_class(container, "autohide-visible");
            gtk_widget_add_css_class(container, "autohide-hidden");
        }
    }

    return self;
}

void
shell_autohide_lock(ShellAutohide *self)
{
    if (!self)
        return;

    self->popover_count++;

    /* Cancel any pending timers */
    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }

    /* Force visible if currently hiding or hidden */
    if (self->state == AUTOHIDE_HIDDEN || self->state == AUTOHIDE_HIDING) {
        self->state = AUTOHIDE_REVEALING;
        autohide_start_animation(self, 0);
    }
}

void
shell_autohide_unlock(ShellAutohide *self)
{
    if (!self)
        return;

    if (self->popover_count > 0) {
        self->popover_count--;
    }
}

void
shell_autohide_enter(ShellAutohide *self)
{
    if (!self || !self->enabled)
        return;

    /* Cancel any pending hide timer */
    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }

    /* Reveal immediately when mouse enters trigger edge */
    if (self->state == AUTOHIDE_HIDDEN || self->state == AUTOHIDE_HIDING) {
        self->state = AUTOHIDE_REVEALING;
        autohide_start_animation(self, 0);
    }
}

void
shell_autohide_leave(ShellAutohide *self)
{
    if (!self || !self->enabled)
        return;

    if (self->popover_count > 0)
        return;

    if (self->state == AUTOHIDE_VISIBLE || self->state == AUTOHIDE_REVEALING) {
        if (self->hide_timeout_id == 0) {
            self->hide_timeout_id = g_timeout_add(
                (guint)self->hide_delay, on_hide_timeout, self);
        }
    }
}

void
shell_autohide_destroy(ShellAutohide *self)
{
    if (!self)
        return;

    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }

    if (self->tick_callback_id != 0) {
        GtkWindow *window = shell_panel_get_window(self->panel);
        gtk_widget_remove_tick_callback(GTK_WIDGET(window), self->tick_callback_id);
        self->tick_callback_id = 0;
    }

    g_free(self);
}
