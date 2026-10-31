#include "screensaver.h"
#include "../shell.h"
#include "../config.h"
#include "../compositor/compositor.h"

#include <gtk4-layer-shell.h>
#include <math.h>

typedef struct {
    ShellScreensaver *owner;
    GtkWidget        *window;
    GtkWidget        *tile_box;
    GtkWidget        *clock_label;
    GtkWidget        *date_label;
    gboolean          has_initial_motion;
    double            initial_x;
    double            initial_y;
} ScreensaverSurface;

struct _ShellScreensaver {
    ShellApp  *app;
    GPtrArray *surfaces;      /* Array of ScreensaverSurface* */
    guint      tick_timer_id;
    gint64     activated_us;
    guint      drift_step;
    gboolean   is_active;
};

static void
screensaver_surface_free(gpointer data)
{
    ScreensaverSurface *surf = data;
    if (!surf)
        return;
    if (surf->window) {
        gtk_window_destroy(GTK_WINDOW(surf->window));
        surf->window = NULL;
    }
    g_free(surf);
}

void
shell_screensaver_set_dpms(gboolean power_on)
{
    if (power_on) {
        niri_send_action("{\"Action\":{\"PowerOnMonitors\":{}}}");
    } else {
        niri_send_action("{\"Action\":{\"PowerOffMonitors\":{}}}");
    }
}

static void
update_screensaver_labels(ShellScreensaver *self, gboolean apply_drift)
{
    if (!self || !self->surfaces)
        return;

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    if (!now)
        return;

    g_autofree gchar *time_str = g_date_time_format(now, "%H:%M");
    g_autofree gchar *raw_date = g_date_time_format(now, "%A, %B %-d");
    g_autofree gchar *upper_date = raw_date ? g_utf8_strup(raw_date, -1) : g_strdup("");
    g_autofree gchar *sub_str = g_strdup_printf("%s  •  [ KAJO STANDBY ]", upper_date);

    /* Subtle 8-position Lissajous-like drift every 60s for OLED burn-in protection */
    static const int drift_dx[8] = {   0,  96,  64, -80,   0, -96, -64,  80 };
    static const int drift_dy[8] = {   0, -48,  64,  48, -64,  48, -64, -48 };

    int dx = drift_dx[self->drift_step % 8];
    int dy = drift_dy[self->drift_step % 8];

    for (guint i = 0; i < self->surfaces->len; i++) {
        ScreensaverSurface *surf = g_ptr_array_index(self->surfaces, i);
        if (!surf)
            continue;
        if (surf->clock_label && time_str)
            gtk_label_set_text(GTK_LABEL(surf->clock_label), time_str);
        if (surf->date_label && sub_str)
            gtk_label_set_text(GTK_LABEL(surf->date_label), sub_str);

        if (apply_drift && surf->tile_box) {
            gtk_widget_set_margin_start(surf->tile_box, dx > 0 ? dx : 0);
            gtk_widget_set_margin_end(surf->tile_box, dx < 0 ? -dx : 0);
            gtk_widget_set_margin_top(surf->tile_box, dy > 0 ? dy : 0);
            gtk_widget_set_margin_bottom(surf->tile_box, dy < 0 ? -dy : 0);
        }
    }
}

static gboolean
on_screensaver_tick(gpointer user_data)
{
    ShellScreensaver *self = user_data;
    if (!self || !self->is_active)
        return G_SOURCE_REMOVE;

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    gboolean drift = FALSE;
    if (now && g_date_time_get_second(now) == 0) {
        self->drift_step++;
        drift = TRUE;
    }

    update_screensaver_labels(self, drift);
    return G_SOURCE_CONTINUE;
}

static gboolean
on_screensaver_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED,
                           guint                  keyval G_GNUC_UNUSED,
                           guint                  keycode G_GNUC_UNUSED,
                           GdkModifierType        state G_GNUC_UNUSED,
                           gpointer               user_data)
{
    ShellScreensaver *self = user_data;
    if (!self || !self->is_active)
        return FALSE;

    if (g_get_monotonic_time() - self->activated_us < 250000)
        return TRUE;

    shell_screensaver_stop(self);
    return TRUE;
}

static void
on_screensaver_clicked(GtkGestureClick *gesture G_GNUC_UNUSED,
                       gint             n_press G_GNUC_UNUSED,
                       double           x G_GNUC_UNUSED,
                       double           y G_GNUC_UNUSED,
                       gpointer         user_data)
{
    ShellScreensaver *self = user_data;
    if (!self || !self->is_active)
        return;

    shell_screensaver_stop(self);
}

static void
on_screensaver_motion(GtkEventControllerMotion *controller G_GNUC_UNUSED,
                      double                    x,
                      double                    y,
                      gpointer                  user_data)
{
    ScreensaverSurface *surf = user_data;
    if (!surf || !surf->owner || !surf->owner->is_active)
        return;

    /* Ignore synthetic pointer-enter motion immediately upon surface map */
    if (g_get_monotonic_time() - surf->owner->activated_us < 400000) {
        surf->has_initial_motion = TRUE;
        surf->initial_x = x;
        surf->initial_y = y;
        return;
    }

    if (!surf->has_initial_motion) {
        surf->has_initial_motion = TRUE;
        surf->initial_x = x;
        surf->initial_y = y;
        return;
    }

    if (fabs(x - surf->initial_x) > 6.0 || fabs(y - surf->initial_y) > 6.0) {
        shell_screensaver_stop(surf->owner);
    }
}

static ScreensaverSurface *
create_surface_for_monitor(ShellScreensaver *self,
                           GdkMonitor       *monitor,
                           gboolean          is_primary)
{
    ScreensaverSurface *surf = g_new0(ScreensaverSurface, 1);
    surf->owner = self;

    GtkWidget *win = gtk_window_new();
    surf->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "Kajo Screen Saver");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_widget_add_css_class(win, "shell-screensaver-window");

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win), "kajo-screensaver");
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    if (monitor)
        gtk_layer_set_monitor(GTK_WINDOW(win), monitor);

    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

    gtk_layer_set_keyboard_mode(
        GTK_WINDOW(win),
        is_primary ? GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE
                   : GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(root_box, TRUE);
    gtk_widget_set_vexpand(root_box, TRUE);

    /* Center floating clock tile */
    GtkWidget *center_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(center_wrap, TRUE);
    gtk_widget_set_vexpand(center_wrap, TRUE);
    gtk_widget_set_halign(center_wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(center_wrap, GTK_ALIGN_CENTER);

    surf->tile_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(surf->tile_box, "screensaver-floating-tile");

    surf->clock_label = gtk_label_new("00:00");
    gtk_widget_add_css_class(surf->clock_label, "screensaver-clock-label");
    gtk_label_set_xalign(GTK_LABEL(surf->clock_label), 0.0f);

    surf->date_label = gtk_label_new("[ KAJO STANDBY ]");
    gtk_widget_add_css_class(surf->date_label, "screensaver-date-label");
    gtk_label_set_xalign(GTK_LABEL(surf->date_label), 0.0f);

    gtk_box_append(GTK_BOX(surf->tile_box), surf->clock_label);
    gtk_box_append(GTK_BOX(surf->tile_box), surf->date_label);
    gtk_box_append(GTK_BOX(center_wrap), surf->tile_box);
    gtk_box_append(GTK_BOX(root_box), center_wrap);

    /* Subtle bottom resume hint */
    GtkWidget *hint_lbl = gtk_label_new("Move mouse or press any key to resume");
    gtk_widget_add_css_class(hint_lbl, "screensaver-hint-label");
    gtk_widget_set_halign(hint_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(hint_lbl, 36);
    gtk_box_append(GTK_BOX(root_box), hint_lbl);

    gtk_window_set_child(GTK_WINDOW(win), root_box);

    /* Input wake controllers */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_screensaver_key_pressed), self);
    gtk_widget_add_controller(win, key_ctrl);

    GtkGesture *click_ctrl = gtk_gesture_click_new();
    g_signal_connect(click_ctrl, "pressed", G_CALLBACK(on_screensaver_clicked), self);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(click_ctrl));

    GtkEventController *motion_ctrl = gtk_event_controller_motion_new();
    g_signal_connect(motion_ctrl, "motion", G_CALLBACK(on_screensaver_motion), surf);
    gtk_widget_add_controller(win, motion_ctrl);

    return surf;
}

ShellScreensaver *
shell_screensaver_new(ShellApp *app)
{
    ShellScreensaver *self = g_new0(ShellScreensaver, 1);
    self->app = app;
    self->surfaces = g_ptr_array_new_with_free_func(screensaver_surface_free);
    self->tick_timer_id = 0;
    self->is_active = FALSE;
    return self;
}

void
shell_screensaver_start(ShellScreensaver *self)
{
    if (!self || self->is_active)
        return;

    ShellConfig *cfg = self->app ? shell_app_get_config(self->app) : NULL;
    const gchar *mode = (cfg && cfg->screensaver_mode) ? cfg->screensaver_mode : "overlay";

    if (g_strcmp0(mode, "disabled") == 0)
        return;

    if (g_strcmp0(mode, "dpms") == 0) {
        self->is_active = TRUE;
        shell_screensaver_set_dpms(FALSE);
        return;
    }

    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;

    GListModel *monitors = gdk_display_get_monitors(display);
    guint n_monitors = monitors ? g_list_model_get_n_items(monitors) : 0;

    self->is_active = TRUE;
    self->activated_us = g_get_monotonic_time();
    self->drift_step = 0;

    if (n_monitors == 0) {
        ScreensaverSurface *surf = create_surface_for_monitor(self, NULL, TRUE);
        g_ptr_array_add(self->surfaces, surf);
    } else {
        for (guint i = 0; i < n_monitors; i++) {
            GdkMonitor *mon = g_list_model_get_item(monitors, i);
            ScreensaverSurface *surf = create_surface_for_monitor(self, mon, i == 0);
            g_ptr_array_add(self->surfaces, surf);
            if (mon)
                g_object_unref(mon);
        }
    }

    update_screensaver_labels(self, TRUE);

    for (guint i = 0; i < self->surfaces->len; i++) {
        ScreensaverSurface *surf = g_ptr_array_index(self->surfaces, i);
        if (surf && surf->window)
            gtk_window_present(GTK_WINDOW(surf->window));
    }

    if (self->tick_timer_id == 0) {
        self->tick_timer_id = g_timeout_add_seconds(1, on_screensaver_tick, self);
    }
}

void
shell_screensaver_stop(ShellScreensaver *self)
{
    if (!self || !self->is_active)
        return;

    self->is_active = FALSE;

    if (self->tick_timer_id > 0) {
        g_source_remove(self->tick_timer_id);
        self->tick_timer_id = 0;
    }

    if (self->surfaces && self->surfaces->len > 0) {
        g_ptr_array_remove_range(self->surfaces, 0, self->surfaces->len);
    }

    shell_screensaver_set_dpms(TRUE);
}

gboolean
shell_screensaver_is_active(ShellScreensaver *self)
{
    return self ? self->is_active : FALSE;
}

void
shell_screensaver_destroy(ShellScreensaver *self)
{
    if (!self)
        return;

    shell_screensaver_stop(self);
    if (self->surfaces) {
        g_ptr_array_free(self->surfaces, TRUE);
        self->surfaces = NULL;
    }
    g_free(self);
}
