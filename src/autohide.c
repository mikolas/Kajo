#include "autohide.h"
#include "panel.h"
#include "config.h"

#include <gtk4-layer-shell.h>

static void
autohide_set_margin(ShellAutohide *self, gint margin)
{
    GtkWindow *window = shell_panel_get_window(self->panel);
    if (!window)
        return;

    /* Slide the panel in/out via layer-shell margin while keeping exclusive zone at 0
     * (overlay mode) so the compositor does not reflow tiled windows during animations. */
    gtk_layer_set_margin(window, self->edge, margin);
}

static gint
autohide_get_hidden_margin(ShellAutohide *self)
{
    gint strip = self->edge_strip_px > 0 ? self->edge_strip_px : 2;
    if (self->touch_gestures_enabled && strip < 6)
        strip = 6;
    return -(self->panel_height - strip);
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
    gint raw_dur = (self->state == AUTOHIDE_HIDING) ? self->hide_duration : self->reveal_duration;
    gint duration_ms = raw_dur > 0 ? raw_dur : ((self->state == AUTOHIDE_HIDING) ? 200 : 150);
    gdouble duration_us = (gdouble)duration_ms * 1000.0; /* ms to us */

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
    gint raw_dur = (self->state == AUTOHIDE_HIDING) ? self->hide_duration : self->reveal_duration;

    if (container && target_margin == 0) {
        gtk_widget_remove_css_class(container, "autohide-hidden");
        gtk_widget_add_css_class(container, "autohide-visible");
    }

    /* Remove existing tick callback */
    if (window && self->tick_callback_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(window), self->tick_callback_id);
        self->tick_callback_id = 0;
    }

    if (!window || raw_dur <= 0) {
        self->anim_target_margin = target_margin;
        autohide_set_margin(self, target_margin);
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
        return;
    }

    self->anim_start_time = g_get_monotonic_time();
    self->anim_start_margin = gtk_layer_get_margin(window, self->edge);
    self->anim_target_margin = target_margin;

    self->tick_callback_id = gtk_widget_add_tick_callback(
        GTK_WIDGET(window), autohide_tick_callback, self, NULL);
}

static gboolean
on_reveal_timeout(gpointer user_data)
{
    ShellAutohide *self = user_data;

    self->reveal_timeout_id = 0;

    if (!self->enabled || !self->pointer_inside)
        return G_SOURCE_REMOVE;

    if (self->state == AUTOHIDE_HIDDEN || self->state == AUTOHIDE_HIDING) {
        self->state = AUTOHIDE_REVEALING;
        autohide_start_animation(self, 0);
    }

    return G_SOURCE_REMOVE;
}

static gboolean
on_hide_timeout(gpointer user_data)
{
    ShellAutohide *self = user_data;

    self->hide_timeout_id = 0;

    if (!self->enabled || self->popover_count > 0 || self->pointer_inside)
        return G_SOURCE_REMOVE;

    if (self->state == AUTOHIDE_VISIBLE || self->state == AUTOHIDE_REVEALING) {
        self->state = AUTOHIDE_HIDING;
        autohide_start_animation(self, autohide_get_hidden_margin(self));
    }

    return G_SOURCE_REMOVE;
}

static void
schedule_hide_timeout(ShellAutohide *self)
{
    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }
    if (self->hide_delay <= 0) {
        on_hide_timeout(self);
    } else {
        self->hide_timeout_id = g_timeout_add(
            (guint)self->hide_delay, on_hide_timeout, self);
    }
}

ShellAutohide *
shell_autohide_new(ShellPanel *panel, const KajoShellCfg *config)
{
    ShellAutohide *self = g_new0(ShellAutohide, 1);

    self->panel           = panel;
    self->enabled         = config->autohide_enabled;
    self->touch_gestures_enabled = config->touch_gestures_enabled;
    self->pointer_inside  = FALSE;
    self->reveal_delay    = config->autohide_reveal_delay >= 0 ? config->autohide_reveal_delay : 150;
    self->hide_delay      = config->autohide_hide_delay >= 0 ? config->autohide_hide_delay : 300;
    self->reveal_duration = config->autohide_reveal_duration >= 0 ? config->autohide_reveal_duration : 150;
    self->hide_duration   = config->autohide_hide_duration >= 0 ? config->autohide_hide_duration : 200;
    self->panel_height    = config->panel_height > 0 ? config->panel_height : 32;
    self->edge_strip_px   = config->autohide_edge_strip_px > 0 ? config->autohide_edge_strip_px : 2;
    self->edge            = (config->panel_position == PANEL_POSITION_BOTTOM)
                            ? GTK_LAYER_SHELL_EDGE_BOTTOM
                            : GTK_LAYER_SHELL_EDGE_TOP;
    self->state           = AUTOHIDE_VISIBLE;
    self->reveal_timeout_id = 0;
    self->hide_timeout_id = 0;
    self->tick_callback_id = 0;
    self->popover_count   = 0;

    if (self->enabled) {
        /* Start hidden (keep active edge strip for motion enter) */
        self->state = AUTOHIDE_HIDDEN;
        self->anim_target_margin = autohide_get_hidden_margin(self);
        autohide_set_margin(self, self->anim_target_margin);
        GtkWidget *container = shell_panel_get_container(self->panel);
        if (container) {
            gtk_widget_remove_css_class(container, "autohide-visible");
            gtk_widget_add_css_class(container, "autohide-hidden");
        }
    }

    return self;
}

void
shell_autohide_reload_config(ShellAutohide *self, const KajoShellCfg *config)
{
    if (!self || !config)
        return;

    GtkLayerShellEdge new_edge = (config->panel_position == PANEL_POSITION_BOTTOM)
                                 ? GTK_LAYER_SHELL_EDGE_BOTTOM
                                 : GTK_LAYER_SHELL_EDGE_TOP;

    GtkWindow *window = shell_panel_get_window(self->panel);
    if (window && new_edge != self->edge) {
        gtk_layer_set_margin(window, self->edge, 0);
    }

    self->edge            = new_edge;
    self->reveal_delay    = config->autohide_reveal_delay >= 0 ? config->autohide_reveal_delay : 150;
    self->hide_delay      = config->autohide_hide_delay >= 0 ? config->autohide_hide_delay : 300;
    self->reveal_duration = config->autohide_reveal_duration >= 0 ? config->autohide_reveal_duration : 150;
    self->hide_duration   = config->autohide_hide_duration >= 0 ? config->autohide_hide_duration : 200;
    self->panel_height    = config->panel_height > 0 ? config->panel_height : 32;
    self->edge_strip_px   = config->autohide_edge_strip_px > 0 ? config->autohide_edge_strip_px : 2;
    self->enabled         = config->autohide_enabled;
    self->touch_gestures_enabled = config->touch_gestures_enabled;

    GtkWidget *container = shell_panel_get_container(self->panel);

    if (!self->enabled) {
        if (self->reveal_timeout_id != 0) {
            g_source_remove(self->reveal_timeout_id);
            self->reveal_timeout_id = 0;
        }
        if (self->hide_timeout_id != 0) {
            g_source_remove(self->hide_timeout_id);
            self->hide_timeout_id = 0;
        }
        if (self->tick_callback_id != 0) {
            if (window)
                gtk_widget_remove_tick_callback(GTK_WIDGET(window), self->tick_callback_id);
            self->tick_callback_id = 0;
        }
        self->state = AUTOHIDE_VISIBLE;
        autohide_set_margin(self, 0);
        if (container) {
            gtk_widget_remove_css_class(container, "autohide-hidden");
            gtk_widget_add_css_class(container, "autohide-visible");
        }
    } else {
        if (self->state == AUTOHIDE_HIDDEN) {
            autohide_set_margin(self, autohide_get_hidden_margin(self));
            if (container) {
                gtk_widget_remove_css_class(container, "autohide-visible");
                gtk_widget_add_css_class(container, "autohide-hidden");
            }
        } else if (self->popover_count == 0 && !self->pointer_inside &&
                   (self->state == AUTOHIDE_VISIBLE || self->state == AUTOHIDE_REVEALING)) {
            schedule_hide_timeout(self);
        }
    }
}

void
shell_autohide_lock(ShellAutohide *self)
{
    if (!self)
        return;

    self->popover_count++;

    if (!self->enabled)
        return;

    /* Cancel any pending timers */
    if (self->reveal_timeout_id != 0) {
        g_source_remove(self->reveal_timeout_id);
        self->reveal_timeout_id = 0;
    }
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

    if (!self->enabled)
        return;

    if (self->popover_count == 0) {
        self->pointer_inside = FALSE;
        if (self->state == AUTOHIDE_VISIBLE || self->state == AUTOHIDE_REVEALING) {
            schedule_hide_timeout(self);
        }
    }
}

void
shell_autohide_enter(ShellAutohide *self)
{
    if (!self)
        return;

    self->pointer_inside = TRUE;

    if (!self->enabled)
        return;

    /* Cancel any pending hide timer */
    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }

    if (self->state == AUTOHIDE_HIDING) {
        if (self->reveal_timeout_id != 0) {
            g_source_remove(self->reveal_timeout_id);
            self->reveal_timeout_id = 0;
        }
        self->state = AUTOHIDE_REVEALING;
        autohide_start_animation(self, 0);
    } else if (self->state == AUTOHIDE_HIDDEN) {
        if (self->reveal_delay <= 0) {
            self->state = AUTOHIDE_REVEALING;
            autohide_start_animation(self, 0);
        } else if (self->reveal_timeout_id == 0) {
            self->reveal_timeout_id = g_timeout_add(
                (guint)self->reveal_delay, on_reveal_timeout, self);
        }
    }
}

void
shell_autohide_leave(ShellAutohide *self)
{
    if (!self)
        return;

    self->pointer_inside = FALSE;

    if (self->reveal_timeout_id != 0) {
        g_source_remove(self->reveal_timeout_id);
        self->reveal_timeout_id = 0;
    }

    if (!self->enabled)
        return;

    if (self->popover_count > 0)
        return;

    if (self->state == AUTOHIDE_VISIBLE || self->state == AUTOHIDE_REVEALING) {
        schedule_hide_timeout(self);
    }
}

void
shell_autohide_touch_reveal(ShellAutohide *self, gint dwell_ms)
{
    if (!self || !self->enabled)
        return;

    self->pointer_inside = FALSE;

    if (self->reveal_timeout_id != 0) {
        g_source_remove(self->reveal_timeout_id);
        self->reveal_timeout_id = 0;
    }
    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }

    if (self->state == AUTOHIDE_HIDDEN || self->state == AUTOHIDE_HIDING) {
        self->state = AUTOHIDE_REVEALING;
        autohide_start_animation(self, 0);
    }

    if (self->popover_count == 0) {
        guint timeout = (guint)MAX(dwell_ms, self->hide_delay);
        self->hide_timeout_id = g_timeout_add(timeout, on_hide_timeout, self);
    }
}

void
shell_autohide_force_hide(ShellAutohide *self)
{
    if (!self || !self->enabled)
        return;

    self->pointer_inside = FALSE;

    if (self->reveal_timeout_id != 0) {
        g_source_remove(self->reveal_timeout_id);
        self->reveal_timeout_id = 0;
    }
    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }

    if (self->popover_count > 0)
        return;

    if (self->state == AUTOHIDE_VISIBLE || self->state == AUTOHIDE_REVEALING) {
        self->state = AUTOHIDE_HIDING;
        autohide_start_animation(self, autohide_get_hidden_margin(self));
    }
}

void
shell_autohide_destroy(ShellAutohide *self)
{
    if (!self)
        return;

    if (self->reveal_timeout_id != 0) {
        g_source_remove(self->reveal_timeout_id);
        self->reveal_timeout_id = 0;
    }

    if (self->hide_timeout_id != 0) {
        g_source_remove(self->hide_timeout_id);
        self->hide_timeout_id = 0;
    }

    if (self->tick_callback_id != 0) {
        GtkWindow *window = shell_panel_get_window(self->panel);
        if (window)
            gtk_widget_remove_tick_callback(GTK_WIDGET(window), self->tick_callback_id);
        self->tick_callback_id = 0;
    }

    g_free(self);
}
