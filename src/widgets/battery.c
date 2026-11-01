#include <gtk/gtk.h>
#include <gio/gio.h>
#include "widget.h"
#include "../compositor/compositor.h"
#include "../hardware/hw_caps.h"

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
    gboolean       updating_profile;
    gboolean       updating_brightness;
    GCancellable  *cancellable;
} BatteryWidget;

/* --- Backlight helpers --- */

static gint battery_read_brightness_pct(void)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    return shell_hw_caps_get_display_brightness_pct(caps);
}

static void battery_set_brightness_pct(gint pct)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    shell_hw_caps_set_display_brightness_pct(caps, pct);
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
    if (!batt || batt->updating_brightness)
        return;
    gint pct = (gint)gtk_range_get_value(range);
    battery_set_brightness_pct(pct);
}

/* --- Battery update --- */

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

    const gchar *icon_name = "tb-battery-4-symbolic";
    if (state == UPOWER_STATE_CHARGING) {
        icon_name = "tb-battery-charging-symbolic";
    } else if (percentage <= 10.0) {
        icon_name = "tb-battery-symbolic";
    } else if (percentage <= 32.0) {
        icon_name = "tb-battery-1-symbolic";
    } else if (percentage <= 55.0) {
        icon_name = "tb-battery-2-symbolic";
    } else if (percentage <= 80.0) {
        icon_name = "tb-battery-3-symbolic";
    } else {
        icon_name = "tb-battery-4-symbolic";
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

    /* Energy rate (power draw / charge rate) - only poll RAPL sysfs when popover is mapped */
    if (batt->popover && gtk_widget_get_mapped(batt->popover)) {
        if (energy_rate <= 0.01) {
            energy_rate = shell_hw_caps_read_rapl_watts();
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

    BatteryWidget *batt = user_data;
    batt->upower_proxy = proxy;
    batt->upower_signal_id = g_signal_connect(batt->upower_proxy, "g-properties-changed",
                                              G_CALLBACK(battery_properties_changed), batt);
    battery_update(batt);
}

static void battery_profiles_proxy_ready(GObject *source G_GNUC_UNUSED,
                                         GAsyncResult *res,
                                         gpointer user_data)
{
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

    BatteryWidget *batt = user_data;
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
shell_battery_popover_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), shell_battery_popover_get_type());
    G_OBJECT_CLASS(shell_battery_popover_parent_class)->dispose(object);
}

static void
shell_battery_popover_init(ShellBatteryPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_battery_popover_class_init(ShellBatteryPopoverClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = shell_battery_popover_dispose;

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

static void on_battery_popover_map(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    BatteryWidget *batt = user_data;
    if (!batt)
        return;
    gint cur_bright = battery_read_brightness_pct();
    if (cur_bright >= 0 && batt->brightness_scale && GTK_IS_RANGE(batt->brightness_scale)) {
        batt->updating_brightness = TRUE;
        gtk_range_set_value(GTK_RANGE(batt->brightness_scale), (double)cur_bright);
        batt->updating_brightness = FALSE;
    }
    battery_update(batt);
}

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

    gint cur_bright = battery_read_brightness_pct();
    if (cur_bright >= 0 && batt->brightness_scale && GTK_IS_RANGE(batt->brightness_scale)) {
        batt->updating_brightness = TRUE;
        gtk_range_set_value(GTK_RANGE(batt->brightness_scale), (double)cur_bright);
        batt->updating_brightness = FALSE;
    }

    g_signal_connect(batt->brightness_scale, "value-changed",
                     G_CALLBACK(on_brightness_changed), batt);
    g_signal_connect(GTK_WIDGET(popover), "map",
                     G_CALLBACK(on_battery_popover_map), batt);

    return GTK_WIDGET(popover);
}

static ShellWidget *battery_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    BatteryWidget *batt = g_new0(BatteryWidget, 1);

    /* Panel: icon + label */
    batt->panel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    batt->panel_image = gtk_image_new_from_icon_name("tb-battery-4-symbolic");
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
    if (!batt)
        return;

    batt->updating_profile = TRUE;
    batt->updating_brightness = TRUE;

    if (batt->profile_power_saver)
        g_signal_handlers_disconnect_by_data(batt->profile_power_saver, batt);
    if (batt->profile_balanced)
        g_signal_handlers_disconnect_by_data(batt->profile_balanced, batt);
    if (batt->profile_performance)
        g_signal_handlers_disconnect_by_data(batt->profile_performance, batt);
    if (batt->brightness_scale)
        g_signal_handlers_disconnect_by_data(batt->brightness_scale, batt);
    if (batt->popover)
        g_signal_handlers_disconnect_by_data(batt->popover, batt);

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
        if (batt->panel_image) {
            gtk_image_set_pixel_size(GTK_IMAGE(batt->panel_image), shell_widget_get_icon_size(widget));
        }
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
