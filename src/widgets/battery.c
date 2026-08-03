#include <gtk/gtk.h>
#include <gio/gio.h>
#include "widget.h"
#include "../compositor/compositor.h"

#define UPOWER_BUS_NAME       "org.freedesktop.UPower"
#define UPOWER_DEVICE_PATH    "/org/freedesktop/UPower/devices/DisplayDevice"
#define UPOWER_DEVICE_IFACE   "org.freedesktop.UPower.Device"

#define POWER_PROFILES_BUS_NAME  "net.hadess.PowerProfiles"
#define POWER_PROFILES_PATH      "/net/hadess/PowerProfiles"
#define POWER_PROFILES_IFACE     "net.hadess.PowerProfiles"

/* UPower device state values */
enum {
    UPOWER_STATE_CHARGING    = 1,
    UPOWER_STATE_DISCHARGING = 2,
    UPOWER_STATE_FULL        = 4,
};

typedef struct {
    ShellWidget    base;
    GtkWidget     *button;
    GtkWidget     *popover;
    GtkWidget     *panel_box;
    GtkWidget     *panel_image;
    GtkWidget     *panel_label;
    /* Popover widgets */
    GtkWidget     *pct_label;
    GtkWidget     *pct_bar;
    GtkWidget     *status_label;
    GtkWidget     *time_label;
    GtkWidget     *energy_label;
    GtkWidget     *profile_power_saver;
    GtkWidget     *profile_balanced;
    GtkWidget     *profile_performance;
    GtkWidget     *brightness_scale;
    /* D-Bus */
    guint          timer_id;
    GDBusProxy    *upower_proxy;
    GDBusProxy    *profiles_proxy;
    gulong         upower_signal_id;
    gulong         profiles_signal_id;
    /* Backlight */
    gchar         *backlight_path;
    gint           max_brightness;
    gboolean       updating_profile;
    gboolean       updating_brightness;
    GCancellable  *cancellable;
} BatteryWidget;

/* --- Backlight helpers --- */

static gchar *find_backlight_path(void)
{
    GDir *dir = g_dir_open("/sys/class/backlight", 0, NULL);
    if (!dir)
        return NULL;

    const gchar *entry;
    gchar *result = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        result = g_build_filename("/sys/class/backlight", entry, NULL);
        break;
    }
    g_dir_close(dir);
    return result;
}

static gint read_sysfs_int(const gchar *path)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return -1;
    gint val = atoi(contents);
    g_free(contents);
    return val;
}

static gint battery_read_brightness_pct(BatteryWidget *batt)
{
    if (!batt->backlight_path || batt->max_brightness <= 0)
        return -1;

    gchar *path = g_build_filename(batt->backlight_path, "brightness", NULL);
    gint val = read_sysfs_int(path);
    g_free(path);

    if (val < 0)
        return -1;
    return (val * 100) / batt->max_brightness;
}

static gchar *
get_active_session_path(GDBusConnection *bus)
{
    GError *error = NULL;
    GVariant *res = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.freedesktop.login1.Seat", "ActiveSession"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        NULL,
        &error);

    if (!res) {
        if (error) g_error_free(error);
        return g_strdup("/org/freedesktop/login1/session/auto");
    }

    GVariant *v = NULL;
    g_variant_get(res, "(v)", &v);
    g_variant_unref(res);

    if (!v)
        return g_strdup("/org/freedesktop/login1/session/auto");

    const gchar *session_id = NULL;
    const gchar *session_path = NULL;
    g_variant_get(v, "(&s&o)", &session_id, &session_path);

    gchar *path = g_strdup(session_path);
    g_variant_unref(v);

    if (path && *path)
        return path;
    g_free(path);
    return g_strdup("/org/freedesktop/login1/session/auto");
}

static void battery_set_brightness_pct(BatteryWidget *batt, gint pct)
{
    if (!batt->backlight_path || batt->max_brightness <= 0)
        return;

    gint val = (pct * batt->max_brightness) / 100;
    if (val < 1) val = 1;
    if (val > batt->max_brightness) val = batt->max_brightness;

    gchar *device_name = g_path_get_basename(batt->backlight_path);

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus) {
        gchar *session_path = get_active_session_path(bus);
        g_dbus_connection_call(
            bus,
            "org.freedesktop.login1",
            session_path,
            "org.freedesktop.login1.Session",
            "SetBrightness",
            g_variant_new("(ssu)", "backlight", device_name, (guint32)val),
            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        g_free(session_path);
        g_object_unref(bus);
    }
    g_free(device_name);
}

/* --- Power profiles --- */

static void battery_update_profile_radios(BatteryWidget *batt)
{
    if (!batt->profiles_proxy)
        return;

    GVariant *v = g_dbus_proxy_get_cached_property(batt->profiles_proxy, "ActiveProfile");
    if (!v)
        return;

    const gchar *profile = g_variant_get_string(v, NULL);

    batt->updating_profile = TRUE;
    if (g_strcmp0(profile, "power-saver") == 0)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(batt->profile_power_saver), TRUE);
    else if (g_strcmp0(profile, "balanced") == 0)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(batt->profile_balanced), TRUE);
    else if (g_strcmp0(profile, "performance") == 0)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(batt->profile_performance), TRUE);
    batt->updating_profile = FALSE;

    g_variant_unref(v);
}

static void battery_set_profile(BatteryWidget *batt, const gchar *profile)
{
    if (!batt->profiles_proxy || batt->updating_profile)
        return;

    g_dbus_proxy_call(batt->profiles_proxy,
                      "org.freedesktop.DBus.Properties.Set",
                      g_variant_new("(ssv)",
                                    POWER_PROFILES_IFACE,
                                    "ActiveProfile",
                                    g_variant_new_string(profile)),
                      G_DBUS_CALL_FLAGS_NONE,
                      -1, NULL, NULL, NULL);
}

static void on_profile_power_saver_toggled(GtkCheckButton *btn, gpointer user_data)
{
    BatteryWidget *batt = user_data;
    if (gtk_check_button_get_active(btn))
        battery_set_profile(batt, "power-saver");
}

static void on_profile_balanced_toggled(GtkCheckButton *btn, gpointer user_data)
{
    BatteryWidget *batt = user_data;
    if (gtk_check_button_get_active(btn))
        battery_set_profile(batt, "balanced");
}

static void on_profile_performance_toggled(GtkCheckButton *btn, gpointer user_data)
{
    BatteryWidget *batt = user_data;
    if (gtk_check_button_get_active(btn))
        battery_set_profile(batt, "performance");
}

/* --- Brightness --- */

static void on_brightness_changed(GtkRange *range, gpointer user_data)
{
    BatteryWidget *batt = user_data;
    if (batt->updating_brightness)
        return;
    gint pct = (gint)gtk_range_get_value(range);
    battery_set_brightness_pct(batt, pct);
}

/* --- Battery update --- */

static double battery_read_rapl_watts(void)
{
    static guint64 prev_energy = 0;
    static gint64 prev_time = 0;
    static const char *rapl_paths[] = {
        "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
        "/sys/class/powercap/intel-rapl:0/energy_uj",
        "/sys/class/powercap/amd_energy/energy1_input",
        NULL
    };
    for (int i = 0; rapl_paths[i] != NULL; i++) {
        gchar *contents = NULL;
        if (g_file_get_contents(rapl_paths[i], &contents, NULL, NULL)) {
            guint64 cur_energy = g_ascii_strtoull(contents, NULL, 10);
            g_free(contents);
            if (cur_energy > 0) {
                gint64 now = g_get_monotonic_time();
                if (prev_energy > 0 && prev_time > 0 && cur_energy >= prev_energy) {
                    double dt_sec = (double)(now - prev_time) / 1000000.0;
                    if (dt_sec > 0.1) {
                        double watts = ((double)(cur_energy - prev_energy) / 1000000.0) / dt_sec;
                        prev_energy = cur_energy;
                        prev_time = now;
                        return watts;
                    }
                }
                prev_energy = cur_energy;
                prev_time = now;
            }
        }
    }

    /* Fallback: HWMON instant power inputs (zenpower / CPU power sensors) */
    for (int h = 0; h < 12; h++) {
        gchar name_path[128];
        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/hwmon%d/name", h);
        gchar *sensor_name = NULL;
        if (g_file_get_contents(name_path, &sensor_name, NULL, NULL)) {
            g_strstrip(sensor_name);
            if (g_strcmp0(sensor_name, "amdgpu") == 0 ||
                g_strcmp0(sensor_name, "nvidia") == 0 ||
                g_strcmp0(sensor_name, "nouveau") == 0 ||
                g_strcmp0(sensor_name, "nvme") == 0) {
                g_free(sensor_name);
                continue;
            }
            g_free(sensor_name);
        }

        guint64 total_microwatts = 0;
        int found_rails = 0;

        for (int p = 1; p <= 4; p++) {
            gchar hwmon_path[128];
            snprintf(hwmon_path, sizeof(hwmon_path), "/sys/class/hwmon/hwmon%d/power%d_input", h, p);
            gchar *contents = NULL;
            if (g_file_get_contents(hwmon_path, &contents, NULL, NULL)) {
                guint64 microwatts = g_ascii_strtoull(contents, NULL, 10);
                g_free(contents);
                if (microwatts > 0) {
                    total_microwatts += microwatts;
                    found_rails++;
                }
            }
        }

        if (found_rails > 0 && total_microwatts > 0) {
            return (double)total_microwatts / 1000000.0;
        }
    }

    return 0.0;
}

static void battery_update(BatteryWidget *batt)
{
    if (!batt->upower_proxy) {
        gtk_label_set_text(GTK_LABEL(batt->panel_label), "--");
        return;
    }

    GVariant *v_pct = g_dbus_proxy_get_cached_property(batt->upower_proxy, "Percentage");
    GVariant *v_state = g_dbus_proxy_get_cached_property(batt->upower_proxy, "State");
    GVariant *v_icon = g_dbus_proxy_get_cached_property(batt->upower_proxy, "IconName");
    GVariant *v_tte = g_dbus_proxy_get_cached_property(batt->upower_proxy, "TimeToEmpty");
    GVariant *v_ttf = g_dbus_proxy_get_cached_property(batt->upower_proxy, "TimeToFull");
    GVariant *v_rate = g_dbus_proxy_get_cached_property(batt->upower_proxy, "EnergyRate");

    double percentage = 0.0;
    guint32 state = 0;
    gint64 time_to_empty = 0;
    gint64 time_to_full = 0;
    double energy_rate = 0.0;

    if (v_pct) { percentage = g_variant_get_double(v_pct); g_variant_unref(v_pct); }
    if (v_state) { state = g_variant_get_uint32(v_state); g_variant_unref(v_state); }
    if (v_icon) { g_variant_unref(v_icon); }
    if (v_tte) { time_to_empty = g_variant_get_int64(v_tte); g_variant_unref(v_tte); }
    if (v_ttf) { time_to_full = g_variant_get_int64(v_ttf); g_variant_unref(v_ttf); }
    if (v_rate) { energy_rate = g_variant_get_double(v_rate); g_variant_unref(v_rate); }

    const gchar *icon_name = "tb-battery-symbolic";
    if (state == UPOWER_STATE_CHARGING) {
        icon_name = "tb-battery-charging-symbolic";
    } else if (percentage <= 25) {
        icon_name = "tb-battery-1-symbolic";
    } else if (percentage <= 50) {
        icon_name = "tb-battery-2-symbolic";
    } else if (percentage <= 75) {
        icon_name = "tb-battery-3-symbolic";
    } else {
        icon_name = "tb-battery-symbolic";
    }

    /* Panel */
    gtk_image_set_from_icon_name(GTK_IMAGE(batt->panel_image), icon_name);
    gchar *pct_text = g_strdup_printf("%.0f%%", percentage);
    gtk_label_set_text(GTK_LABEL(batt->panel_label), pct_text);
    g_free(pct_text);
    shell_widget_apply_mode_visibility(batt->base.mode, batt->panel_image, batt->panel_label);

    /* Popover: percentage */
    gchar *pct_detail = g_strdup_printf("Battery: %.0f%%", percentage);
    gtk_label_set_text(GTK_LABEL(batt->pct_label), pct_detail);
    g_free(pct_detail);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(batt->pct_bar), percentage / 100.0);

    /* Status */
    gchar *status_full = NULL;
    if (state == UPOWER_STATE_CHARGING) status_full = g_strdup("Status: Charging");
    else if (state == UPOWER_STATE_DISCHARGING) status_full = g_strdup("Status: Discharging");
    else if (state == UPOWER_STATE_FULL) status_full = g_strdup("Status: Fully Charged (AC)");
    else status_full = g_strdup("Status: Unknown");
    gtk_label_set_text(GTK_LABEL(batt->status_label), status_full);
    g_free(status_full);

    /* Time remaining */
    gint64 time_secs = 0;
    if (state == UPOWER_STATE_DISCHARGING)
        time_secs = time_to_empty;
    else if (state == UPOWER_STATE_CHARGING)
        time_secs = time_to_full;

    if (time_secs > 0) {
        gint hours = (gint)(time_secs / 3600);
        gint minutes = (gint)((time_secs % 3600) / 60);
        const char *time_prefix = (state == UPOWER_STATE_CHARGING) ? "Time to full:" : "Time remaining:";
        gchar *time_str = g_strdup_printf("%s %dh %dm", time_prefix, hours, minutes);
        gtk_label_set_text(GTK_LABEL(batt->time_label), time_str);
        g_free(time_str);
    } else if (state == UPOWER_STATE_FULL) {
        gtk_label_set_text(GTK_LABEL(batt->time_label), "Time remaining: Fully Charged (AC)");
    } else {
        gtk_label_set_text(GTK_LABEL(batt->time_label), "Time remaining: --");
    }
    gtk_widget_set_visible(batt->time_label, TRUE);

    /* Energy rate (power draw / charge rate) */
    if (energy_rate <= 0.01) {
        /* Sysfs fallback for power_now */
        gint sysfs_power = read_sysfs_int("/sys/class/power_supply/BAT0/power_now");
        if (sysfs_power > 0) {
            energy_rate = (double)sysfs_power / 1000000.0;
        }
    }
    if (energy_rate <= 0.01) {
        /* RAPL fallback for AC power */
        energy_rate = battery_read_rapl_watts();
    }

    if (energy_rate > 0.01) {
        gchar *rate_str = g_strdup_printf("Power Draw: %.1f W", energy_rate);
        gtk_label_set_text(GTK_LABEL(batt->energy_label), rate_str);
        g_free(rate_str);
    } else if (state == UPOWER_STATE_FULL) {
        gtk_label_set_text(GTK_LABEL(batt->energy_label), "Power Draw: 0.0 W (AC Power)");
    } else {
        gtk_label_set_text(GTK_LABEL(batt->energy_label), "Power Draw: -- W");
    }
    gtk_widget_set_visible(batt->energy_label, TRUE);
}

static void battery_properties_changed(GDBusProxy *proxy G_GNUC_UNUSED,
                                       GVariant *changed_properties G_GNUC_UNUSED,
                                       GStrv invalidated_properties G_GNUC_UNUSED,
                                       gpointer user_data)
{
    battery_update((BatteryWidget *)user_data);
}

static void profiles_properties_changed(GDBusProxy *proxy G_GNUC_UNUSED,
                                        GVariant *changed_properties G_GNUC_UNUSED,
                                        GStrv invalidated_properties G_GNUC_UNUSED,
                                        gpointer user_data)
{
    battery_update_profile_radios((BatteryWidget *)user_data);
}

/* --- Proxy callbacks --- */

static void battery_upower_proxy_ready(GObject *source G_GNUC_UNUSED,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
    BatteryWidget *batt = user_data;
    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        if (proxy) g_object_unref(proxy);
        return;
    }

    if (!proxy) {
        g_warning("battery: failed to connect to UPower: %s",
                  error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    batt->upower_proxy = proxy;
    batt->upower_signal_id = g_signal_connect(batt->upower_proxy, "g-properties-changed",
                                              G_CALLBACK(battery_properties_changed), batt);
    battery_update(batt);
}

static void battery_profiles_proxy_ready(GObject *source G_GNUC_UNUSED,
                                         GAsyncResult *res,
                                         gpointer user_data)
{
    BatteryWidget *batt = user_data;
    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        if (proxy) g_object_unref(proxy);
        return;
    }

    if (!proxy) {
        g_warning("battery: failed to connect to PowerProfiles: %s",
                  error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    batt->profiles_proxy = proxy;
    batt->profiles_signal_id = g_signal_connect(batt->profiles_proxy, "g-properties-changed",
                                                G_CALLBACK(profiles_properties_changed), batt);
    battery_update_profile_radios(batt);
}

/* --- Widget interface --- */

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellBatteryPopover {
    GtkPopover parent_instance;

    GtkWidget *pct_label;
    GtkWidget *pct_bar;
    GtkWidget *status_label;
    GtkWidget *time_label;
    GtkWidget *energy_label;
    GtkWidget *profile_power_saver;
    GtkWidget *profile_balanced;
    GtkWidget *profile_performance;
    GtkWidget *brightness_scale;
} ShellBatteryPopover;

typedef struct _ShellBatteryPopoverClass {
    GtkPopoverClass parent_class;
} ShellBatteryPopoverClass;

G_DEFINE_TYPE(ShellBatteryPopover, shell_battery_popover, GTK_TYPE_POPOVER)

static void
shell_battery_popover_init(ShellBatteryPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_battery_popover_class_init(ShellBatteryPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/battery_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, pct_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, pct_bar);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, status_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, time_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, energy_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, profile_power_saver);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, profile_balanced);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, profile_performance);
    gtk_widget_class_bind_template_child(widget_class, ShellBatteryPopover, brightness_scale);
}

/* --- Widget interface --- */

static GtkWidget *battery_build_popover_content(BatteryWidget *batt)
{
    ShellBatteryPopover *popover = g_object_new(shell_battery_popover_get_type(), NULL);

    batt->pct_label = popover->pct_label;
    batt->pct_bar = popover->pct_bar;
    batt->status_label = popover->status_label;
    batt->time_label = popover->time_label;
    batt->energy_label = popover->energy_label;
    batt->profile_power_saver = popover->profile_power_saver;
    batt->profile_balanced = popover->profile_balanced;
    batt->profile_performance = popover->profile_performance;
    batt->brightness_scale = popover->brightness_scale;

    if (batt->profile_balanced && batt->profile_power_saver) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(batt->profile_balanced),
                                   GTK_CHECK_BUTTON(batt->profile_power_saver));
    }
    if (batt->profile_performance && batt->profile_power_saver) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(batt->profile_performance),
                                   GTK_CHECK_BUTTON(batt->profile_power_saver));
    }

    g_signal_connect(batt->profile_power_saver, "toggled",
                     G_CALLBACK(on_profile_power_saver_toggled), batt);
    g_signal_connect(batt->profile_balanced, "toggled",
                     G_CALLBACK(on_profile_balanced_toggled), batt);
    g_signal_connect(batt->profile_performance, "toggled",
                     G_CALLBACK(on_profile_performance_toggled), batt);

    gint cur_bright = battery_read_brightness_pct(batt);
    if (cur_bright >= 0 && batt->brightness_scale && GTK_IS_RANGE(batt->brightness_scale)) {
        batt->updating_brightness = TRUE;
        gtk_range_set_value(GTK_RANGE(batt->brightness_scale), (double)cur_bright);
        batt->updating_brightness = FALSE;
    }

    g_signal_connect(batt->brightness_scale, "value-changed",
                     G_CALLBACK(on_brightness_changed), batt);

    return GTK_WIDGET(popover);
}

static ShellWidget *battery_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    BatteryWidget *batt = g_new0(BatteryWidget, 1);

    /* Backlight setup */
    batt->backlight_path = find_backlight_path();
    if (batt->backlight_path) {
        gchar *max_path = g_build_filename(batt->backlight_path, "max_brightness", NULL);
        batt->max_brightness = read_sysfs_int(max_path);
        g_free(max_path);
    }

    /* Panel: icon + label */
    batt->panel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    batt->panel_image = gtk_image_new_from_icon_name("fl-battery-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(batt->panel_image), shell_widget_get_icon_size((ShellWidget *)batt));
    batt->panel_label = gtk_label_new("--");
    gtk_box_append(GTK_BOX(batt->panel_box), batt->panel_image);
    gtk_box_append(GTK_BOX(batt->panel_box), batt->panel_label);

    /* Menu button */
    batt->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(batt->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(batt->button), batt->panel_box);
    gtk_widget_add_css_class(batt->button, "flat");
    gtk_widget_add_css_class(batt->button, "shell-widget");
    gtk_widget_add_css_class(batt->button, "shell-widget-battery");

    /* Popover */
    batt->popover = battery_build_popover_content(batt);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(batt->button), batt->popover);

    return (ShellWidget *)batt;
}

static gboolean battery_tick(gpointer user_data)
{
    BatteryWidget *batt = (BatteryWidget *)user_data;
    battery_update(batt);
    return G_SOURCE_CONTINUE;
}

static void battery_destroy(ShellWidget *widget)
{
    BatteryWidget *batt = (BatteryWidget *)widget;

    if (batt->timer_id > 0) {
        g_source_remove(batt->timer_id);
        batt->timer_id = 0;
    }
    if (batt->cancellable) {
        g_cancellable_cancel(batt->cancellable);
        g_object_unref(batt->cancellable);
        batt->cancellable = NULL;
    }
    if (batt->upower_proxy) {
        if (batt->upower_signal_id > 0)
            g_signal_handler_disconnect(batt->upower_proxy, batt->upower_signal_id);
        g_object_unref(batt->upower_proxy);
    }
    if (batt->profiles_proxy) {
        if (batt->profiles_signal_id > 0)
            g_signal_handler_disconnect(batt->profiles_proxy, batt->profiles_signal_id);
        g_object_unref(batt->profiles_proxy);
    }
    g_free(batt->backlight_path);
    g_free(batt);
}

static GtkWidget *battery_get_widget(ShellWidget *widget)
{
    return ((BatteryWidget *)widget)->button;
}

static void battery_enable(ShellWidget *widget)
{
    BatteryWidget *batt = (BatteryWidget *)widget;
    if (batt) {
        shell_widget_apply_mode_visibility(widget->mode, batt->panel_image, batt->panel_label);
        if (!batt->cancellable || g_cancellable_is_cancelled(batt->cancellable)) {
            if (batt->cancellable) g_object_unref(batt->cancellable);
            batt->cancellable = g_cancellable_new();
        }
    }

    if (!batt->upower_proxy) {
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 NULL,
                                 UPOWER_BUS_NAME,
                                 UPOWER_DEVICE_PATH,
                                 UPOWER_DEVICE_IFACE,
                                 batt->cancellable,
                                 battery_upower_proxy_ready,
                                 batt);
    }

    if (!batt->profiles_proxy) {
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 NULL,
                                 POWER_PROFILES_BUS_NAME,
                                 POWER_PROFILES_PATH,
                                 POWER_PROFILES_IFACE,
                                 batt->cancellable,
                                 battery_profiles_proxy_ready,
                                 batt);
    }

    if (batt->timer_id == 0) {
        battery_update(batt);
        batt->timer_id = g_timeout_add_seconds(3, battery_tick, batt);
    }
}

static void battery_disable(ShellWidget *widget)
{
    BatteryWidget *batt = (BatteryWidget *)widget;

    if (batt->timer_id > 0) {
        g_source_remove(batt->timer_id);
        batt->timer_id = 0;
    }

    if (batt->cancellable) {
        g_cancellable_cancel(batt->cancellable);
        g_clear_object(&batt->cancellable);
    }

    if (batt->upower_proxy) {
        if (batt->upower_signal_id > 0) {
            g_signal_handler_disconnect(batt->upower_proxy, batt->upower_signal_id);
            batt->upower_signal_id = 0;
        }
        g_clear_object(&batt->upower_proxy);
    }
    if (batt->profiles_proxy) {
        if (batt->profiles_signal_id > 0) {
            g_signal_handler_disconnect(batt->profiles_proxy, batt->profiles_signal_id);
            batt->profiles_signal_id = 0;
        }
        g_clear_object(&batt->profiles_proxy);
    }
}

const ShellWidgetClass battery_widget_class = {
    .id         = "battery",
    .name       = "Battery",
    .create     = battery_create,
    .destroy    = battery_destroy,
    .get_widget = battery_get_widget,
    .enable     = battery_enable,
    .disable    = battery_disable,
};
