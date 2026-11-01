#include "posture_manager.h"
#include "../config.h"
#include "../hardware/hw_caps.h"
#include "../hardware/kbd_backlight.h"
#include "../shell.h"
#include <fcntl.h>
#include <gio/gio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SENSOR_PROXY_BUS_NAME "net.hadess.SensorProxy"
#define SENSOR_PROXY_PATH     "/net/hadess/SensorProxy"
#define SENSOR_PROXY_IFACE    "net.hadess.SensorProxy"

#define HINGE_ENTER_FOLDED_DEG 180.0
#define HINGE_EXIT_FOLDED_DEG  168.0

struct _ShellPostureManager {
    ShellApp          *app;
    ShellPostureState  state;
    gdouble            last_hinge_deg;
    guint              timer_id;
    GDBusProxy        *sensor_proxy;
    gulong             sensor_sig_id;
    gboolean           accel_claimed;
    gchar             *last_transform;
    GCancellable      *cancellable;
};

static void
write_inhibit_paths(GPtrArray *paths, gboolean inhibit)
{
    if (!paths)
        return;
    const gchar *val = inhibit ? "1\n" : "0\n";
    size_t len = strlen(val);
    for (guint i = 0; i < paths->len; i++) {
        const gchar *p = g_ptr_array_index(paths, i);
        if (!p)
            continue;
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            ssize_t unused G_GNUC_UNUSED = write(fd, val, len);
            close(fd);
        }
    }
}

static void
apply_niri_transform(ShellPostureManager *self, const gchar *transform)
{
    if (!self || !transform)
        return;
    if (g_strcmp0(self->last_transform, transform) == 0)
        return;

    g_free(self->last_transform);
    self->last_transform = g_strdup(transform);

    const gchar *argv[] = { "niri", "msg", "output", "eDP-1", "transform", transform, NULL };
    g_spawn_async(NULL, (gchar **)argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static const gchar *
orientation_str_to_transform(const gchar *orient)
{
    if (!orient)
        return NULL;
    if (g_strcmp0(orient, "normal") == 0)
        return "normal";
    if (g_strcmp0(orient, "left-up") == 0)
        return "90";
    if (g_strcmp0(orient, "bottom-up") == 0)
        return "180";
    if (g_strcmp0(orient, "right-up") == 0)
        return "270";
    return NULL;
}

static const gchar *
read_iio_accel_transform_fallback(const ShellHwCaps *caps)
{
    if (!caps || !caps->has_accel_sensor || !caps->accel_iio_dir)
        return NULL;

    g_autofree gchar *x_path = g_build_filename(caps->accel_iio_dir, "in_accel_x_raw", NULL);
    g_autofree gchar *y_path = g_build_filename(caps->accel_iio_dir, "in_accel_y_raw", NULL);
    g_autofree gchar *x_str = NULL;
    g_autofree gchar *y_str = NULL;

    if (!g_file_get_contents(x_path, &x_str, NULL, NULL) ||
        !g_file_get_contents(y_path, &y_str, NULL, NULL))
        return NULL;

    gdouble ax = g_ascii_strtod(x_str, NULL);
    gdouble ay = g_ascii_strtod(y_str, NULL);

    /* Ignore near-flat deadzone */
    if (fabs(ax) < 350.0 && fabs(ay) < 350.0)
        return NULL;

    if (fabs(ay) >= fabs(ax)) {
        return (ay < 0.0) ? "normal" : "180";
    } else {
        return (ax > 0.0) ? "90" : "270";
    }
}

static void
update_folded_orientation(ShellPostureManager *self)
{
    if (!self || self->state != SHELL_POSTURE_FOLDED)
        return;

    ShellConfig *cfg = shell_app_get_config(self->app);
    if (!cfg || !cfg->posture_auto_rotate)
        return;

    const gchar *transform = NULL;
    if (self->sensor_proxy) {
        GVariant *v = g_dbus_proxy_get_cached_property(self->sensor_proxy, "AccelerometerOrientation");
        if (v) {
            const gchar *orient = g_variant_get_string(v, NULL);
            transform = orientation_str_to_transform(orient);
            g_variant_unref(v);
        }
    }

    if (!transform) {
        const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
        transform = read_iio_accel_transform_fallback(caps);
    }

    if (transform)
        apply_niri_transform(self, transform);
}

static void
set_accel_claimed(ShellPostureManager *self, gboolean claim)
{
    if (!self || !self->sensor_proxy || self->accel_claimed == claim)
        return;

    self->accel_claimed = claim;
    g_dbus_proxy_call(self->sensor_proxy,
                      claim ? "ClaimAccelerometer" : "ReleaseAccelerometer",
                      NULL,
                      G_DBUS_CALL_FLAGS_NONE,
                      1000,
                      NULL,
                      NULL,
                      NULL);
}

static void
apply_posture_effects(ShellPostureManager *self, ShellPostureState new_state, gboolean force)
{
    if (!self)
        return;
    if (!force && self->state == new_state)
        return;

    self->state = new_state;
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    ShellConfig *cfg = shell_app_get_config(self->app);
    if (!caps || !cfg)
        return;

    gboolean is_folded = (new_state == SHELL_POSTURE_FOLDED);

    /* 1. Internal Keyboard & Touchpad inhibition + Keyboard Backlight wake in Laptop mode */
    gboolean inhibit_kbd = is_folded && cfg->posture_disable_kbd_in_tablet;
    write_inhibit_paths(caps->kbd_inhibit_paths, inhibit_kbd);
    write_inhibit_paths(caps->touchpad_inhibit_paths, inhibit_kbd);

    if (!inhibit_kbd && shell_kbd_backlight_available() && cfg->kbd_backlight_level > 0) {
        if (shell_kbd_backlight_get_level() == 0) {
            shell_kbd_backlight_set_level(cfg->kbd_backlight_level);
        }
    }

    /* 2. Finger Touchscreen inhibition in Laptop mode */
    gboolean inhibit_touch = (!is_folded) && cfg->posture_disable_touch_in_laptop;
    write_inhibit_paths(caps->touchscreen_inhibit_paths, inhibit_touch);

    /* 3. Screen Auto-Rotation */
    if (is_folded && cfg->posture_auto_rotate) {
        set_accel_claimed(self, TRUE);
        update_folded_orientation(self);
    } else {
        set_accel_claimed(self, FALSE);
        apply_niri_transform(self, "normal");
    }
}

static gboolean
posture_tick(gpointer user_data)
{
    ShellPostureManager *self = (ShellPostureManager *)user_data;
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps || !caps->has_hinge_sensor)
        return G_SOURCE_CONTINUE;

    gdouble deg = 0.0;
    if (!shell_hw_caps_read_hinge_angle_deg(caps, &deg))
        return G_SOURCE_CONTINUE;

    self->last_hinge_deg = deg;

    if (self->state == SHELL_POSTURE_UNKNOWN) {
        ShellPostureState initial = (deg >= HINGE_ENTER_FOLDED_DEG)
                                        ? SHELL_POSTURE_FOLDED
                                        : SHELL_POSTURE_LAPTOP;
        apply_posture_effects(self, initial, TRUE);
    } else if (self->state == SHELL_POSTURE_LAPTOP && deg >= HINGE_ENTER_FOLDED_DEG) {
        apply_posture_effects(self, SHELL_POSTURE_FOLDED, FALSE);
    } else if (self->state == SHELL_POSTURE_FOLDED && deg <= HINGE_EXIT_FOLDED_DEG) {
        apply_posture_effects(self, SHELL_POSTURE_LAPTOP, FALSE);
    } else if (self->state == SHELL_POSTURE_FOLDED) {
        update_folded_orientation(self);
    }

    return G_SOURCE_CONTINUE;
}

static void
on_sensor_proxy_properties_changed(GDBusProxy *proxy G_GNUC_UNUSED,
                                   GVariant   *changed_properties G_GNUC_UNUSED,
                                   GStrv       invalidated_properties G_GNUC_UNUSED,
                                   gpointer    user_data)
{
    ShellPostureManager *self = (ShellPostureManager *)user_data;
    update_folded_orientation(self);
}

static void
on_sensor_proxy_ready(GObject      *source G_GNUC_UNUSED,
                      GAsyncResult *res,
                      gpointer      user_data)
{
    ShellPostureManager *self = (ShellPostureManager *)user_data;
    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        if (proxy)
            g_object_unref(proxy);
        return;
    }
    if (!proxy) {
        g_clear_error(&error);
        return;
    }

    self->sensor_proxy = proxy;
    self->sensor_sig_id = g_signal_connect(proxy,
                                           "g-properties-changed",
                                           G_CALLBACK(on_sensor_proxy_properties_changed),
                                           self);

    if (self->state == SHELL_POSTURE_FOLDED) {
        ShellConfig *cfg = shell_app_get_config(self->app);
        if (cfg && cfg->posture_auto_rotate) {
            set_accel_claimed(self, TRUE);
            update_folded_orientation(self);
        }
    }
}

static void
apply_saved_kbd_backlight_config(ShellApp *app)
{
    ShellConfig *cfg = shell_app_get_config(app);
    if (!cfg || !shell_kbd_backlight_available())
        return;

    if (shell_kbd_backlight_has_timeout()) {
        shell_kbd_backlight_set_timeout_sec(cfg->kbd_backlight_timeout_sec);
    }
    if (cfg->kbd_backlight_level > 0 && shell_kbd_backlight_get_level() == 0) {
        shell_kbd_backlight_set_level(cfg->kbd_backlight_level);
    }
}

ShellPostureManager *
shell_posture_manager_new(ShellApp *app)
{
    ShellPostureManager *self = g_new0(ShellPostureManager, 1);
    self->app = app;
    self->state = SHELL_POSTURE_UNKNOWN;
    self->last_hinge_deg = -1.0;

    /* Apply saved keyboard backlight brightness & timeout on startup */
    apply_saved_kbd_backlight_config(app);

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps || (!caps->has_hinge_sensor && !caps->has_accel_sensor)) {
        /* Desktop PC or non-foldable host: zero timers, zero D-Bus proxies */
        return self;
    }

    self->cancellable = g_cancellable_new();
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                             G_DBUS_PROXY_FLAGS_NONE,
                             NULL,
                             SENSOR_PROXY_BUS_NAME,
                             SENSOR_PROXY_PATH,
                             SENSOR_PROXY_IFACE,
                             self->cancellable,
                             on_sensor_proxy_ready,
                             self);

    if (caps->has_hinge_sensor) {
        posture_tick(self);
        self->timer_id = g_timeout_add_seconds(1, posture_tick, self);
    }

    return self;
}

void
shell_posture_manager_reload_config(ShellPostureManager *self)
{
    if (!self)
        return;

    apply_saved_kbd_backlight_config(self->app);

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps || !caps->has_hinge_sensor)
        return;

    if (self->state != SHELL_POSTURE_UNKNOWN) {
        apply_posture_effects(self, self->state, TRUE);
    } else {
        posture_tick(self);
    }
}

void
shell_posture_manager_destroy(ShellPostureManager *self)
{
    if (!self)
        return;

    if (self->timer_id > 0) {
        g_source_remove(self->timer_id);
        self->timer_id = 0;
    }

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }

    /* Safety restore: always un-inhibit all input devices on exit */
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (caps) {
        write_inhibit_paths(caps->kbd_inhibit_paths, FALSE);
        write_inhibit_paths(caps->touchpad_inhibit_paths, FALSE);
        write_inhibit_paths(caps->touchscreen_inhibit_paths, FALSE);
    }

    if (self->sensor_proxy) {
        set_accel_claimed(self, FALSE);
        if (self->sensor_sig_id > 0) {
            g_signal_handler_disconnect(self->sensor_proxy, self->sensor_sig_id);
            self->sensor_sig_id = 0;
        }
        g_clear_object(&self->sensor_proxy);
    }

    g_free(self->last_transform);
    g_free(self);
}
