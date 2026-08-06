#include <gtk/gtk.h>
#include <gio/gio.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "widget.h"
#include "../shell.h"
#include "../panel.h"
#include "../config.h"
#include "../theme.h"
#include "../icons.h"
#include "../autohide.h"
#include "../dbus_helpers.h"

#define POWER_PROFILES_BUS_NAME  "net.hadess.PowerProfiles"
#define POWER_PROFILES_PATH      "/net/hadess/PowerProfiles"
#define POWER_PROFILES_IFACE     "net.hadess.PowerProfiles"

typedef struct {
    ShellWidget    base;
    GtkWidget     *button;
    GtkWidget     *popover;
    GtkWidget     *icon_img;
    GtkWidget     *label;

    /* Sliders */
    GtkWidget     *vol_scale;
    GtkWidget     *bright_scale;
    GtkWidget     *vol_val_lbl;
    GtkWidget     *bright_val_lbl;
    gboolean       updating_vol;
    gboolean       updating_bright;

    /* Quick Toggles (2x2 Grid) */
    GtkWidget     *wifi_btn;
    GtkWidget     *wifi_sub_lbl;
    GtkWidget     *bt_btn;
    GtkWidget     *bt_sub_lbl;
    GtkWidget     *dnd_btn;
    GtkWidget     *dnd_sub_lbl;
    GtkWidget     *idle_btn;
    GtkWidget     *idle_sub_lbl;

    /* Power Profiles */
    GtkWidget     *prof_saver;
    GtkWidget     *prof_balanced;
    GtkWidget     *prof_perf;
    gboolean       updating_profile;

    /* PulseAudio Context */
    pa_glib_mainloop *pa_ml;
    pa_context   *pa_ctx;
    gchar        *default_sink_name;
    guint32       volume_percent;

    /* State & D-Bus */
    GCancellable *cancellable;
    GDBusProxy    *profiles_proxy;
    GDBusProxy    *nm_proxy;
    gulong         profiles_signal_id;
    gchar         *backlight_path;
    gint           max_brightness;

    gboolean       wifi_enabled;
    gboolean       bt_enabled;
    gboolean       dnd_enabled;
    gboolean       idle_enabled;
    guint          idle_cookie;

    guint          refresh_timer_id;
} ControlCenterWidget;

/* ─── Backlight Helper ─── */

static gchar *
cc_find_backlight_path(void)
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

static gint
cc_read_sysfs_int(const gchar *path)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return -1;
    gint val = atoi(contents);
    g_free(contents);
    return val;
}

static gchar *
cc_get_active_session_path(GDBusConnection *bus)
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

static gint
cc_read_brightness_pct(ControlCenterWidget *cc)
{
    if (!cc->backlight_path || cc->max_brightness <= 0)
        return -1;

    gchar *path = g_build_filename(cc->backlight_path, "actual_brightness", NULL);
    gint val = cc_read_sysfs_int(path);
    g_free(path);

    if (val < 0) return -1;
    return (val * 100) / cc->max_brightness;
}

static void
cc_set_brightness_pct(ControlCenterWidget *cc, gint pct)
{
    if (!cc->backlight_path || cc->max_brightness <= 0)
        return;

    gint val = (pct * cc->max_brightness) / 100;
    if (val < 1) val = 1;
    if (val > cc->max_brightness) val = cc->max_brightness;

    gchar *device_name = g_path_get_basename(cc->backlight_path);

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus) {
        gchar *session_path = cc_get_active_session_path(bus);
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

/* ─── PulseAudio C API Volume Integration ─── */

static pa_cvolume
cc_make_cvolume(guint32 percent, int channels)
{
    pa_cvolume cv;
    pa_cvolume_init(&cv);
    cv.channels = (uint8_t)channels;
    pa_volume_t v = (pa_volume_t)((percent * PA_VOLUME_NORM + 50) / 100);
    pa_cvolume_set(&cv, cv.channels, v);
    return cv;
}

static guint32
cc_percent_from_cvolume(const pa_cvolume *cv)
{
    if (!cv || cv->channels == 0)
        return 0;
    pa_volume_t avg = pa_cvolume_avg(cv);
    return (guint32)((avg * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
}

static void
cc_sink_info_cb(pa_context *ctx G_GNUC_UNUSED, const pa_sink_info *info,
                int eol, void *userdata)
{
    ControlCenterWidget *cc = userdata;
    if (eol > 0 || !info) return;

    if (!cc->default_sink_name || g_strcmp0(info->name, cc->default_sink_name) != 0)
        return;

    guint32 new_percent = cc_percent_from_cvolume(&info->volume);
    cc->volume_percent = new_percent;

    if (cc->vol_scale && GTK_IS_RANGE(cc->vol_scale)) {
        cc->updating_vol = TRUE;
        gtk_range_set_value(GTK_RANGE(cc->vol_scale), (double)new_percent);
        cc->updating_vol = FALSE;
    }

    if (cc->vol_val_lbl && GTK_IS_LABEL(cc->vol_val_lbl)) {
        gchar buf[32];
        snprintf(buf, sizeof(buf), "%u%%", new_percent);
        gtk_label_set_text(GTK_LABEL(cc->vol_val_lbl), buf);
    }
}

static void
cc_server_info_cb(pa_context *ctx, const pa_server_info *info, void *userdata)
{
    ControlCenterWidget *cc = userdata;
    if (!info || !info->default_sink_name) return;

    g_free(cc->default_sink_name);
    cc->default_sink_name = g_strdup(info->default_sink_name);
    pa_context_get_sink_info_by_name(ctx, cc->default_sink_name, cc_sink_info_cb, cc);
}

static void
cc_context_subscribe_cb(pa_context *ctx, pa_subscription_event_type_t t,
                        uint32_t idx G_GNUC_UNUSED, void *userdata)
{
    ControlCenterWidget *cc = userdata;
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK ||
        (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SERVER) {
        pa_context_get_server_info(ctx, cc_server_info_cb, cc);
    }
}

static void
cc_context_state_cb(pa_context *ctx, void *userdata)
{
    ControlCenterWidget *cc = userdata;
    if (pa_context_get_state(ctx) == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(ctx, cc_context_subscribe_cb, cc);
        pa_context_subscribe(ctx, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
        pa_context_get_server_info(ctx, cc_server_info_cb, cc);
    }
}

/* ─── Power Profiles ─── */

static void cc_update_profile_radios(ControlCenterWidget *cc);

static void
cc_profiles_proxy_ready(GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);

    if (!cc || (cc->cancellable && g_cancellable_is_cancelled(cc->cancellable)) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (error) g_clear_error(&error);
        if (proxy) g_object_unref(proxy);
        return;
    }

    if (!proxy) {
        if (error) g_clear_error(&error);
        return;
    }

    cc->profiles_proxy = proxy;
    g_signal_connect_swapped(cc->profiles_proxy, "g-properties-changed", G_CALLBACK(cc_update_profile_radios), cc);
    cc_update_profile_radios(cc);
}

static void
cc_update_profile_radios(ControlCenterWidget *cc)
{
    if (!cc->profiles_proxy)
        return;

    GVariant *v = g_dbus_proxy_get_cached_property(cc->profiles_proxy, "ActiveProfile");
    if (!v)
        return;

    const gchar *profile = g_variant_get_string(v, NULL);

    cc->updating_profile = TRUE;
    if (g_strcmp0(profile, "power-saver") == 0)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(cc->prof_saver), TRUE);
    else if (g_strcmp0(profile, "balanced") == 0)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(cc->prof_balanced), TRUE);
    else if (g_strcmp0(profile, "performance") == 0)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(cc->prof_perf), TRUE);
    cc->updating_profile = FALSE;

    g_variant_unref(v);
}

static void
cc_set_profile(ControlCenterWidget *cc, const gchar *profile)
{
    if (!cc->profiles_proxy || cc->updating_profile)
        return;

    g_dbus_proxy_call(cc->profiles_proxy,
                      "org.freedesktop.DBus.Properties.Set",
                      g_variant_new("(ssv)", POWER_PROFILES_IFACE,
                                    "ActiveProfile", g_variant_new_string(profile)),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_prof_saver_toggled(GtkCheckButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (gtk_check_button_get_active(btn)) cc_set_profile(cc, "power-saver");
}

static void on_prof_balanced_toggled(GtkCheckButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (gtk_check_button_get_active(btn)) cc_set_profile(cc, "balanced");
}

static void on_prof_perf_toggled(GtkCheckButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (gtk_check_button_get_active(btn)) cc_set_profile(cc, "performance");
}

/* ─── Callbacks ─── */

static void
on_vol_changed(GtkRange *range, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (cc->updating_vol) return;

    guint32 percent = (guint32)gtk_range_get_value(range);
    cc->volume_percent = percent;

    if (cc->pa_ctx && pa_context_get_state(cc->pa_ctx) == PA_CONTEXT_READY && cc->default_sink_name) {
        pa_cvolume cv = cc_make_cvolume(percent, 2);
        pa_context_set_sink_volume_by_name(cc->pa_ctx, cc->default_sink_name, &cv, NULL, NULL);
    }

    gchar buf[32];
    snprintf(buf, sizeof(buf), "%u%%", percent);
    if (cc->vol_val_lbl && GTK_IS_LABEL(cc->vol_val_lbl)) {
        gtk_label_set_text(GTK_LABEL(cc->vol_val_lbl), buf);
    }
}

static void
on_bright_changed(GtkRange *range, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (cc->updating_bright) return;
    gint val = (gint)gtk_range_get_value(range);
    cc_set_brightness_pct(cc, val);
    gchar buf[32];
    snprintf(buf, sizeof(buf), "%d%%", val);
    gtk_label_set_text(GTK_LABEL(cc->bright_val_lbl), buf);
}

static void
set_wifi_enabled(gboolean state)
{
    shell_dbus_nm_set_wireless_enabled(state);
}

static void
on_wifi_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    cc->wifi_enabled = !cc->wifi_enabled;
    set_wifi_enabled(cc->wifi_enabled);
    if (cc->wifi_enabled) {
        gtk_widget_add_css_class(cc->wifi_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->wifi_sub_lbl), "Enabled");
    } else {
        gtk_widget_remove_css_class(cc->wifi_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->wifi_sub_lbl), "Disabled");
    }
}

static void
set_bluetooth_powered(gboolean state)
{
    shell_dbus_bluez_set_powered(state);
}

static void
on_bt_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    cc->bt_enabled = !cc->bt_enabled;
    set_bluetooth_powered(cc->bt_enabled);
    if (cc->bt_enabled) {
        gtk_widget_add_css_class(cc->bt_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->bt_sub_lbl), "Enabled");
    } else {
        gtk_widget_remove_css_class(cc->bt_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->bt_sub_lbl), "Disabled");
    }
}

static void
on_dnd_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    cc->dnd_enabled = !cc->dnd_enabled;
    if (cc->dnd_enabled) {
        gtk_widget_add_css_class(cc->dnd_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->dnd_sub_lbl), "On (Alerts Muted)");
    } else {
        gtk_widget_remove_css_class(cc->dnd_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->dnd_sub_lbl), "Off (Alerts On)");
    }
}

static void
on_idle_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    cc->idle_enabled = !cc->idle_enabled;
    if (cc->idle_enabled) {
        gtk_widget_add_css_class(cc->idle_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->idle_sub_lbl), "Screen Awake");
    } else {
        gtk_widget_remove_css_class(cc->idle_btn, "active");
        gtk_label_set_text(GTK_LABEL(cc->idle_sub_lbl), "Disabled");
    }
}

static void
call_logind_manager_method(const char *method_name, GVariant *parameters)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        return;
    }
    g_dbus_connection_call(
        bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        method_name,
        parameters,
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_object_unref(bus);
}

static void
on_action_settings_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    if (!g_spawn_command_line_async("./builddir/kajo-settings", NULL)) {
        g_spawn_command_line_async("kajo-settings", NULL);
    }
}

static void
on_action_lock_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    call_logind_manager_method("LockSessions", NULL);
}

static void
on_action_suspend_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    call_logind_manager_method("Suspend", g_variant_new("(b)", TRUE));
}

static void
on_action_power_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    call_logind_manager_method("PowerOff", g_variant_new("(b)", TRUE));
}

/* ─── State Refresh ─── */

static void
cc_refresh_state(ControlCenterWidget *cc)
{
    /* Update volume from PulseAudio / PipeWire */
    if (cc->pa_ctx && pa_context_get_state(cc->pa_ctx) == PA_CONTEXT_READY) {
        pa_context_get_server_info(cc->pa_ctx, cc_server_info_cb, cc);
    }

    /* Update brightness scale */
    gint bright = cc_read_brightness_pct(cc);
    if (bright >= 0) {
        cc->updating_bright = TRUE;
        if (cc->bright_scale && GTK_IS_RANGE(cc->bright_scale)) {
            gtk_range_set_value(GTK_RANGE(cc->bright_scale), (double)bright);
        }
        gchar buf[32];
        snprintf(buf, sizeof(buf), "%d%%", bright);
        if (cc->bright_val_lbl && GTK_IS_LABEL(cc->bright_val_lbl)) {
            gtk_label_set_text(GTK_LABEL(cc->bright_val_lbl), buf);
        }
        cc->updating_bright = FALSE;
    }

    cc_update_profile_radios(cc);
    shell_widget_apply_mode_visibility(cc->base.mode, cc->icon_img, cc->label);
}

static gboolean
on_cc_tick(gpointer user_data)
{
    ControlCenterWidget *cc = (ControlCenterWidget *)user_data;
    if (!cc || !cc->popover || !gtk_widget_get_mapped(cc->popover))
        return G_SOURCE_CONTINUE;

    cc_refresh_state(cc);
    return G_SOURCE_CONTINUE;
}

static void
on_cc_popover_map(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc) return;
    shell_autohide_lock(shell_panel_get_autohide(cc->base.app ? shell_app_get_panel(cc->base.app) : NULL));
    cc_refresh_state(cc);
}

static void
on_cc_popover_closed(GtkPopover *popover G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc) return;
    shell_autohide_unlock(shell_panel_get_autohide(cc->base.app ? shell_app_get_panel(cc->base.app) : NULL));
}

/* ─── Tile Creation Helper ─── */

static GtkWidget *
create_toggle_tile(const gchar *icon_name, const gchar *title, const gchar *sub_default,
                   GtkWidget **out_sub_lbl, GCallback callback, gpointer user_data)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "control-center-tile");
    gtk_widget_set_hexpand(btn, TRUE);
    gtk_widget_set_size_request(btn, 140, 52);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t_lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t_lbl), 0.0);
    gtk_widget_add_css_class(t_lbl, "tile-title");

    GtkWidget *s_lbl = gtk_label_new(sub_default);
    gtk_label_set_xalign(GTK_LABEL(s_lbl), 0.0);
    gtk_widget_add_css_class(s_lbl, "tile-subtitle");
    *out_sub_lbl = s_lbl;

    gtk_box_append(GTK_BOX(vbox), t_lbl);
    gtk_box_append(GTK_BOX(vbox), s_lbl);

    gtk_box_append(GTK_BOX(hbox), icon);
    gtk_box_append(GTK_BOX(hbox), vbox);

    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    g_signal_connect(btn, "clicked", callback, user_data);

    return btn;
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellControlCenterPopover {
    GtkPopover parent_instance;

    GtkWidget *vol_scale;
    GtkWidget *vol_val_lbl;
    GtkWidget *bright_scale;
    GtkWidget *bright_val_lbl;
    GtkWidget *toggle_grid;
    GtkWidget *prof_saver;
    GtkWidget *prof_balanced;
    GtkWidget *prof_perf;
    GtkWidget *btn_settings;
    GtkWidget *btn_lock;
    GtkWidget *btn_suspend;
    GtkWidget *btn_power;
} ShellControlCenterPopover;

typedef struct _ShellControlCenterPopoverClass {
    GtkPopoverClass parent_class;
} ShellControlCenterPopoverClass;

G_DEFINE_TYPE(ShellControlCenterPopover, shell_control_center_popover, GTK_TYPE_POPOVER)

static void
shell_control_center_popover_init(ShellControlCenterPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_control_center_popover_class_init(ShellControlCenterPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/control_center_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, vol_scale);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, vol_val_lbl);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, bright_scale);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, bright_val_lbl);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, toggle_grid);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, prof_saver);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, prof_balanced);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, prof_perf);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, btn_settings);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, btn_lock);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, btn_suspend);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, btn_power);
}

/* ─── Widget Construction ─── */

static ShellWidget *
control_center_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    ControlCenterWidget *cc = g_new0(ControlCenterWidget, 1);
    cc->wifi_enabled = TRUE;
    cc->bt_enabled = TRUE;

    /* Backlight setup */
    cc->backlight_path = cc_find_backlight_path();
    if (cc->backlight_path) {
        gchar *max_path = g_build_filename(cc->backlight_path, "max_brightness", NULL);
        cc->max_brightness = cc_read_sysfs_int(max_path);
        g_free(max_path);
    }

    /* Panel Button */
    GtkWidget *panel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    cc->icon_img = gtk_image_new_from_icon_name("fl-sliders-symbolic");
    cc->label = gtk_label_new("Control Center");
    gtk_box_append(GTK_BOX(panel_box), cc->icon_img);
    gtk_box_append(GTK_BOX(panel_box), cc->label);

    cc->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(cc->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(cc->button), panel_box);
    gtk_widget_add_css_class(cc->button, "flat");
    gtk_widget_add_css_class(cc->button, "shell-widget");
    gtk_widget_add_css_class(cc->button, "shell-widget-control-center");

    /* Instantiate Declarative Popover */
    ShellControlCenterPopover *popover = g_object_new(shell_control_center_popover_get_type(), NULL);
    cc->popover = GTK_WIDGET(popover);

    cc->vol_scale = popover->vol_scale;
    cc->vol_val_lbl = popover->vol_val_lbl;
    cc->bright_scale = popover->bright_scale;
    cc->bright_val_lbl = popover->bright_val_lbl;
    cc->prof_saver = popover->prof_saver;
    cc->prof_balanced = popover->prof_balanced;
    cc->prof_perf = popover->prof_perf;

    g_signal_connect(cc->vol_scale, "value-changed", G_CALLBACK(on_vol_changed), cc);
    g_signal_connect(cc->bright_scale, "value-changed", G_CALLBACK(on_bright_changed), cc);

    /* Attach Quick Toggles to Grid */
    cc->wifi_btn = create_toggle_tile("tb-wifi-symbolic", "Wi-Fi", "Connected",
                                     &cc->wifi_sub_lbl, G_CALLBACK(on_wifi_clicked), cc);
    gtk_widget_add_css_class(cc->wifi_btn, "active");

    cc->bt_btn = create_toggle_tile("tb-bluetooth-symbolic", "Bluetooth", "On",
                                   &cc->bt_sub_lbl, G_CALLBACK(on_bt_clicked), cc);
    gtk_widget_add_css_class(cc->bt_btn, "active");

    cc->dnd_btn = create_toggle_tile("tb-bell-symbolic", "Do Not Disturb", "Off (Alerts On)",
                                    &cc->dnd_sub_lbl, G_CALLBACK(on_dnd_clicked), cc);

    cc->idle_btn = create_toggle_tile("tb-coffee-symbolic", "Keep Awake", "Disabled",
                                     &cc->idle_sub_lbl, G_CALLBACK(on_idle_clicked), cc);

    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->wifi_btn, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->bt_btn, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->dnd_btn, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->idle_btn, 1, 1, 1, 1);

    if (cc->prof_balanced && cc->prof_saver) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(cc->prof_balanced),
                                   GTK_CHECK_BUTTON(cc->prof_saver));
    }
    if (cc->prof_perf && cc->prof_saver) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(cc->prof_perf),
                                   GTK_CHECK_BUTTON(cc->prof_saver));
    }

    g_signal_connect(cc->prof_saver, "toggled", G_CALLBACK(on_prof_saver_toggled), cc);
    g_signal_connect(cc->prof_balanced, "toggled", G_CALLBACK(on_prof_balanced_toggled), cc);
    g_signal_connect(cc->prof_perf, "toggled", G_CALLBACK(on_prof_perf_toggled), cc);

    g_signal_connect(popover->btn_settings, "clicked", G_CALLBACK(on_action_settings_clicked), cc);
    g_signal_connect(popover->btn_lock, "clicked", G_CALLBACK(on_action_lock_clicked), cc);
    g_signal_connect(popover->btn_suspend, "clicked", G_CALLBACK(on_action_suspend_clicked), cc);
    g_signal_connect(popover->btn_power, "clicked", G_CALLBACK(on_action_power_clicked), cc);

    gtk_menu_button_set_popover(GTK_MENU_BUTTON(cc->button), cc->popover);
    g_signal_connect(cc->popover, "map", G_CALLBACK(on_cc_popover_map), cc);
    g_signal_connect(cc->popover, "closed", G_CALLBACK(on_cc_popover_closed), cc);

    /* Connect PulseAudio C API */
    cc->pa_ml = pa_glib_mainloop_new(NULL);
    if (cc->pa_ml) {
        pa_mainloop_api *api = pa_glib_mainloop_get_api(cc->pa_ml);
        cc->pa_ctx = pa_context_new(api, "shell-control-center");
        if (cc->pa_ctx) {
            pa_context_set_state_callback(cc->pa_ctx, cc_context_state_cb, cc);
            pa_context_connect(cc->pa_ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
        }
    }

    cc->cancellable = g_cancellable_new();
    g_dbus_proxy_new_for_bus(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles",
        "net.hadess.PowerProfiles", cc->cancellable, cc_profiles_proxy_ready, cc);

    return (ShellWidget *)cc;
}

static void
control_center_destroy(ShellWidget *widget)
{
    ControlCenterWidget *cc = (ControlCenterWidget *)widget;
    if (cc->cancellable) {
        g_cancellable_cancel(cc->cancellable);
        g_clear_object(&cc->cancellable);
    }
    if (cc->refresh_timer_id > 0)
        g_source_remove(cc->refresh_timer_id);
    if (cc->profiles_proxy) {
        g_signal_handlers_disconnect_by_data(cc->profiles_proxy, cc);
        g_object_unref(cc->profiles_proxy);
        cc->profiles_proxy = NULL;
    }
    if (cc->backlight_path)
        g_free(cc->backlight_path);

    if (cc->pa_ctx) {
        pa_context_set_state_callback(cc->pa_ctx, NULL, NULL);
        pa_context_set_subscribe_callback(cc->pa_ctx, NULL, NULL);
        pa_context_disconnect(cc->pa_ctx);
        pa_context_unref(cc->pa_ctx);
        cc->pa_ctx = NULL;
    }
    if (cc->pa_ml) {
        pa_glib_mainloop_free(cc->pa_ml);
        cc->pa_ml = NULL;
    }
    g_free(cc->default_sink_name);
    g_free(cc);
}

static GtkWidget *
control_center_get_widget(ShellWidget *widget)
{
    return ((ControlCenterWidget *)widget)->button;
}

static void
control_center_enable(ShellWidget *widget)
{
    ControlCenterWidget *cc = (ControlCenterWidget *)widget;
    if (cc) {
        shell_widget_apply_mode_visibility(widget->mode, cc->icon_img, cc->label);
        gtk_image_set_pixel_size(GTK_IMAGE(cc->icon_img), shell_widget_get_icon_size(widget));
    }
    if (cc->refresh_timer_id == 0) {
        cc_refresh_state(cc);
        cc->refresh_timer_id = g_timeout_add_seconds(1, on_cc_tick, cc);
    }
}

static void
control_center_disable(ShellWidget *widget)
{
    ControlCenterWidget *cc = (ControlCenterWidget *)widget;
    if (cc->refresh_timer_id > 0) {
        g_source_remove(cc->refresh_timer_id);
        cc->refresh_timer_id = 0;
    }
}

const ShellWidgetClass control_center_widget_class = {
    .id         = "control-center",
    .name       = "Control Center",
    .create     = control_center_create,
    .destroy    = control_center_destroy,
    .get_widget = control_center_get_widget,
    .enable     = control_center_enable,
    .disable    = control_center_disable,
};
