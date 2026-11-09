#include <gtk/gtk.h>
#include <gio/gio.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "widget.h"
#include "../audio/pa_conn.h"
#include "../shell.h"
#include "../panel.h"
#include "../config.h"
#include "../theme.h"
#include "../icons.h"
#include "../autohide.h"
#include "../dbus_helpers.h"
#include "../hardware/hw_caps.h"
#include "../lock/idle_manager.h"
#include "../notifications/daemon.h"

#define POWER_PROFILES_BUS_NAME  "net.hadess.PowerProfiles"
#define POWER_PROFILES_PATH      "/net/hadess/PowerProfiles"
#define POWER_PROFILES_IFACE     "net.hadess.PowerProfiles"

static inline void
unref_pa_op(pa_operation *op)
{
    if (op)
        pa_operation_unref(op);
}

typedef struct {
    ShellWidget    base;
    GtkWidget     *button;
    GtkWidget     *popover;
    GtkWidget     *icon_img;
    GtkWidget     *label;

    /* Header & Sliders */
    GtkWidget     *status_badge;
    GtkWidget     *vol_scale;
    GtkWidget     *bright_row;
    GtkWidget     *bright_scale;
    GtkWidget     *vol_val_lbl;
    GtkWidget     *bright_val_lbl;
    GtkWidget     *btn_power;
    gboolean       updating_vol;
    gboolean       updating_bright;
    gboolean       updating_tiles;
    gboolean       power_confirm_armed;
    guint          power_confirm_timer_id;

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
    ShellPaConn   pa;
    gchar        *default_sink_name;
    guint8        sink_channels;
    guint32       volume_percent;

    /* State & D-Bus */
    GCancellable *cancellable;
    GDBusProxy    *profiles_proxy;
    GDBusProxy    *nm_proxy;
    gulong         profiles_signal_id;

    gboolean       wifi_enabled;
    gboolean       bt_enabled;
    gboolean       dnd_enabled;
    gboolean       idle_enabled;
    guint          idle_cookie;

    guint          refresh_timer_id;
} ControlCenterWidget;

/* ─── Backlight Helper ─── */

static gint
cc_read_brightness_pct(void)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    return shell_hw_caps_get_display_brightness_pct(caps);
}

static void
cc_set_brightness_pct(gint pct)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    shell_hw_caps_set_display_brightness_pct(caps, pct);
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

    if (info->volume.channels > 0)
        cc->sink_channels = info->volume.channels;

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
    unref_pa_op(pa_context_get_sink_info_by_name(ctx, cc->default_sink_name, cc_sink_info_cb, cc));
}

static void
cc_context_subscribe_cb(pa_context *ctx, pa_subscription_event_type_t t,
                        uint32_t idx G_GNUC_UNUSED, void *userdata)
{
    ControlCenterWidget *cc = userdata;
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK ||
        (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SERVER) {
        unref_pa_op(pa_context_get_server_info(ctx, cc_server_info_cb, cc));
    }
}

static void
cc_context_state_cb(pa_context *ctx, pa_context_state_t state, gpointer userdata)
{
    ControlCenterWidget *cc = userdata;
    if (state == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(ctx, cc_context_subscribe_cb, cc);
        unref_pa_op(pa_context_subscribe(ctx, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
        unref_pa_op(pa_context_get_server_info(ctx, cc_server_info_cb, cc));
    }
}

/* ─── Power Profiles ─── */

static void cc_update_profile_radios(ControlCenterWidget *cc);

static void
cc_profiles_proxy_ready(GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        if (proxy) g_object_unref(proxy);
        return;
    }

    if (!proxy || !user_data) {
        g_clear_error(&error);
        if (proxy) g_object_unref(proxy);
        return;
    }

    ControlCenterWidget *cc = user_data;
    if (cc->profiles_proxy) {
        g_signal_handlers_disconnect_by_data(cc->profiles_proxy, cc);
        g_clear_object(&cc->profiles_proxy);
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
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cc->prof_saver), TRUE);
    else if (g_strcmp0(profile, "balanced") == 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cc->prof_balanced), TRUE);
    else if (g_strcmp0(profile, "performance") == 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cc->prof_perf), TRUE);
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

static void on_prof_saver_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (gtk_toggle_button_get_active(btn)) cc_set_profile(cc, "power-saver");
}

static void on_prof_balanced_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (gtk_toggle_button_get_active(btn)) cc_set_profile(cc, "balanced");
}

static void on_prof_perf_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (gtk_toggle_button_get_active(btn)) cc_set_profile(cc, "performance");
}

/* ─── Callbacks ─── */

static void
on_vol_changed(GtkRange *range, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc || cc->updating_vol) return;

    guint32 percent = (guint32)gtk_range_get_value(range);
    cc->volume_percent = percent;

    if (cc->pa.ctx && pa_context_get_state(cc->pa.ctx) == PA_CONTEXT_READY) {
        const char *sink = cc->default_sink_name ? cc->default_sink_name : "@DEFAULT_SINK@";
        int channels = cc->sink_channels > 0 ? (int)cc->sink_channels : 2;
        pa_cvolume cv = cc_make_cvolume(percent, channels);
        unref_pa_op(pa_context_set_sink_volume_by_name(cc->pa.ctx, sink, &cv, NULL, NULL));
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
    if (!cc || cc->updating_bright) return;
    gint val = (gint)gtk_range_get_value(range);
    cc_set_brightness_pct(val);
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
cc_update_status_badge(ControlCenterWidget *cc)
{
    if (!cc || !cc->status_badge)
        return;

    gtk_widget_remove_css_class(cc->status_badge, "active");
    gtk_widget_remove_css_class(cc->status_badge, "urgent");
    gtk_widget_remove_css_class(cc->status_badge, "peak-warn");

    if (cc->dnd_enabled) {
        gtk_label_set_text(GTK_LABEL(cc->status_badge), "[ DND MUTED ]");
        gtk_widget_add_css_class(cc->status_badge, "peak-warn");
    } else if (cc->idle_enabled) {
        gtk_label_set_text(GTK_LABEL(cc->status_badge), "[ KEEP AWAKE ]");
        gtk_widget_add_css_class(cc->status_badge, "active");
    } else {
        gtk_label_set_text(GTK_LABEL(cc->status_badge), "[ READY ]");
    }
}

static void
on_wifi_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc || cc->updating_tiles)
        return;
    cc->wifi_enabled = gtk_toggle_button_get_active(btn);
    set_wifi_enabled(cc->wifi_enabled);
    gtk_label_set_text(GTK_LABEL(cc->wifi_sub_lbl),
                       cc->wifi_enabled ? "Enabled" : "Disabled");
}

static void
set_bluetooth_powered(gboolean state)
{
    shell_dbus_bluez_set_powered(state);
}

static void
on_bt_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc || cc->updating_tiles)
        return;
    cc->bt_enabled = gtk_toggle_button_get_active(btn);
    set_bluetooth_powered(cc->bt_enabled);
    gtk_label_set_text(GTK_LABEL(cc->bt_sub_lbl),
                       cc->bt_enabled ? "Enabled" : "Disabled");
}

static void
on_dnd_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc || cc->updating_tiles)
        return;
    cc->dnd_enabled = gtk_toggle_button_get_active(btn);
    if (cc->base.app && cc->base.app->notifications) {
        shell_notification_daemon_set_dnd(cc->base.app->notifications, cc->dnd_enabled);
    }
    gtk_label_set_text(GTK_LABEL(cc->dnd_sub_lbl),
                       cc->dnd_enabled ? "On (Alerts Muted)" : "Off (Alerts On)");
    cc_update_status_badge(cc);
}

static void
update_cc_idle_ui(ControlCenterWidget *cc)
{
    if (!cc || !cc->idle_btn || !cc->idle_sub_lbl)
        return;

    cc->updating_tiles = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cc->idle_btn), cc->idle_enabled);
    cc->updating_tiles = FALSE;
    gtk_label_set_text(GTK_LABEL(cc->idle_sub_lbl),
                       cc->idle_enabled ? "Screen Awake" : "Disabled");
    cc_update_status_badge(cc);
}

static void
on_cc_idle_manager_changed(gboolean inhibited, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc)
        return;
    cc->idle_enabled = inhibited;
    update_cc_idle_ui(cc);
}

static void
on_idle_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc || cc->updating_tiles)
        return;
    cc->idle_enabled = gtk_toggle_button_get_active(btn);
    if (cc->base.app && cc->base.app->idle_manager) {
        shell_idle_manager_set_inhibited(cc->base.app->idle_manager, cc->idle_enabled);
    }
    update_cc_idle_ui(cc);
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
on_action_settings_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    shell_spawn_settings(NULL, cc ? cc->popover : NULL);
}

static void
on_action_lock_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (cc && cc->popover) {
        gtk_popover_popdown(GTK_POPOVER(cc->popover));
    }
    if (cc && cc->base.app) {
        shell_app_lock_session(cc->base.app);
    } else {
        call_logind_manager_method("LockSessions", NULL);
    }
}

static void
cc_reset_power_confirm(ControlCenterWidget *cc)
{
    if (!cc)
        return;
    if (cc->power_confirm_timer_id > 0) {
        g_source_remove(cc->power_confirm_timer_id);
        cc->power_confirm_timer_id = 0;
    }
    cc->power_confirm_armed = FALSE;
    if (cc->btn_power && GTK_IS_BUTTON(cc->btn_power))
        gtk_button_set_label(GTK_BUTTON(cc->btn_power), "⏻ Power");
}

static gboolean
on_cc_power_confirm_timeout(gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (cc) {
        cc->power_confirm_timer_id = 0;
        cc_reset_power_confirm(cc);
    }
    return G_SOURCE_REMOVE;
}

static void
on_action_suspend_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (cc && cc->popover)
        gtk_popover_popdown(GTK_POPOVER(cc->popover));
    call_logind_manager_method("Suspend", g_variant_new("(b)", TRUE));
}

static void
on_action_power_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc)
        return;
    if (!cc->power_confirm_armed) {
        cc->power_confirm_armed = TRUE;
        if (cc->btn_power && GTK_IS_BUTTON(cc->btn_power))
            gtk_button_set_label(GTK_BUTTON(cc->btn_power), "⚠ CONFIRM?");
        if (cc->power_confirm_timer_id > 0)
            g_source_remove(cc->power_confirm_timer_id);
        cc->power_confirm_timer_id = g_timeout_add_seconds(4, on_cc_power_confirm_timeout, cc);
        return;
    }
    cc_reset_power_confirm(cc);
    if (cc->popover)
        gtk_popover_popdown(GTK_POPOVER(cc->popover));
    call_logind_manager_method("PowerOff", g_variant_new("(b)", TRUE));
}

/* ─── State Refresh ─── */

static void
cc_refresh_dbus_toggles(ControlCenterWidget *cc)
{
    if (!cc)
        return;

    cc->wifi_enabled = shell_dbus_nm_get_wireless_enabled();
    cc->bt_enabled = shell_dbus_bluez_get_powered();

    cc->updating_tiles = TRUE;
    if (cc->wifi_btn && cc->wifi_sub_lbl) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cc->wifi_btn), cc->wifi_enabled);
        gtk_label_set_text(GTK_LABEL(cc->wifi_sub_lbl),
                           cc->wifi_enabled ? "Enabled" : "Disabled");
    }
    if (cc->bt_btn && cc->bt_sub_lbl) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cc->bt_btn), cc->bt_enabled);
        gtk_label_set_text(GTK_LABEL(cc->bt_sub_lbl),
                           cc->bt_enabled ? "Enabled" : "Disabled");
    }
    cc->updating_tiles = FALSE;
}

static void
cc_refresh_state(ControlCenterWidget *cc)
{
    /* Update volume from PulseAudio / PipeWire */
    if (cc->pa.ctx && pa_context_get_state(cc->pa.ctx) == PA_CONTEXT_READY) {
        unref_pa_op(pa_context_get_server_info(cc->pa.ctx, cc_server_info_cb, cc));
    }

    /* Update brightness scale */
    gint bright = cc_read_brightness_pct();
    if (cc->bright_row)
        gtk_widget_set_visible(cc->bright_row, bright >= 0);
    if (bright >= 0) {
        cc->updating_bright = TRUE;
        if (cc->bright_scale && GTK_IS_RANGE(cc->bright_scale)) {
            gtk_range_set_value(GTK_RANGE(cc->bright_scale), (double)MAX(1, bright));
        }
        gchar buf[32];
        snprintf(buf, sizeof(buf), "%d%%", bright);
        if (cc->bright_val_lbl && GTK_IS_LABEL(cc->bright_val_lbl)) {
            gtk_label_set_text(GTK_LABEL(cc->bright_val_lbl), buf);
        }
        cc->updating_bright = FALSE;
    }

    if (cc->base.app && cc->base.app->notifications) {
        cc->dnd_enabled = shell_notification_daemon_get_dnd(cc->base.app->notifications);
    }
    if (cc->dnd_btn && cc->dnd_sub_lbl) {
        cc->updating_tiles = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cc->dnd_btn), cc->dnd_enabled);
        cc->updating_tiles = FALSE;
        gtk_label_set_text(GTK_LABEL(cc->dnd_sub_lbl),
                           cc->dnd_enabled ? "On (Alerts Muted)" : "Off (Alerts On)");
    }

    cc_update_status_badge(cc);
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
    cc_refresh_dbus_toggles(cc);
    cc_refresh_state(cc);
    if (cc->refresh_timer_id == 0) {
        cc->refresh_timer_id = g_timeout_add_seconds(1, on_cc_tick, cc);
    }
}

static void
on_cc_popover_unmap(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    ControlCenterWidget *cc = user_data;
    if (!cc) return;
    cc_reset_power_confirm(cc);
    if (cc->refresh_timer_id > 0) {
        g_source_remove(cc->refresh_timer_id);
        cc->refresh_timer_id = 0;
    }
    if (cc->power_confirm_timer_id > 0) {
        g_source_remove(cc->power_confirm_timer_id);
        cc->power_confirm_timer_id = 0;
    }
}

/* ─── Tile Creation Helper (Canonical GtkToggleButton .toggle-card) ─── */

static GtkWidget *
create_toggle_tile(const gchar *icon_name, const gchar *title, const gchar *sub_default,
                   gboolean initial_active, GtkWidget **out_sub_lbl,
                   GCallback callback, gpointer user_data)
{
    GtkWidget *btn = gtk_toggle_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), initial_active);
    gtk_widget_add_css_class(btn, "toggle-card");
    gtk_widget_set_hexpand(btn, TRUE);
    gtk_widget_set_size_request(btn, 140, 52);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(vbox, TRUE);
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
    g_signal_connect(btn, "toggled", callback, user_data);

    return btn;
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellControlCenterPopover {
    GtkPopover parent_instance;

    GtkWidget *status_badge;
    GtkWidget *vol_scale;
    GtkWidget *vol_val_lbl;
    GtkWidget *bright_row;
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
shell_control_center_popover_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), shell_control_center_popover_get_type());
    G_OBJECT_CLASS(shell_control_center_popover_parent_class)->dispose(object);
}

static void
shell_control_center_popover_init(ShellControlCenterPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_control_center_popover_class_init(ShellControlCenterPopoverClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = shell_control_center_popover_dispose;

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/control_center_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, status_badge);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, vol_scale);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, vol_val_lbl);
    gtk_widget_class_bind_template_child(widget_class, ShellControlCenterPopover, bright_row);
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
    cc->sink_channels = 2;
    cc->wifi_enabled = TRUE;
    cc->bt_enabled = TRUE;

    /* Panel Button */
    cc->icon_img = gtk_image_new_from_icon_name("fl-sliders-symbolic");
    cc->label = gtk_label_new("Control Center");
    GtkWidget *panel_box = shell_widget_pack_bar_box(cc->icon_img, cc->label);

    cc->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(cc->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(cc->button), panel_box);
    gtk_widget_add_css_class(cc->button, "flat");
    gtk_widget_add_css_class(cc->button, "shell-widget");
    gtk_widget_add_css_class(cc->button, "shell-widget-control-center");

    /* Instantiate Declarative Popover */
    ShellControlCenterPopover *popover = g_object_new(shell_control_center_popover_get_type(), NULL);
    cc->popover = GTK_WIDGET(popover);

    cc->status_badge = popover->status_badge;
    cc->vol_scale = popover->vol_scale;
    cc->vol_val_lbl = popover->vol_val_lbl;
    cc->bright_row = popover->bright_row;
    cc->bright_scale = popover->bright_scale;
    cc->bright_val_lbl = popover->bright_val_lbl;
    cc->prof_saver = popover->prof_saver;
    cc->prof_balanced = popover->prof_balanced;
    cc->prof_perf = popover->prof_perf;
    cc->btn_power = popover->btn_power;

    g_signal_connect(cc->vol_scale, "value-changed", G_CALLBACK(on_vol_changed), cc);
    g_signal_connect(cc->bright_scale, "value-changed", G_CALLBACK(on_bright_changed), cc);

    /* Attach Quick Toggles to Grid */
    cc->wifi_btn = create_toggle_tile("tb-wifi-symbolic", "Wi-Fi", "Enabled",
                                      TRUE, &cc->wifi_sub_lbl, G_CALLBACK(on_wifi_toggled), cc);

    cc->bt_btn = create_toggle_tile("tb-bluetooth-symbolic", "Bluetooth", "Enabled",
                                    TRUE, &cc->bt_sub_lbl, G_CALLBACK(on_bt_toggled), cc);

    cc->dnd_btn = create_toggle_tile("tb-bell-symbolic", "Do Not Disturb", "Off (Alerts On)",
                                     FALSE, &cc->dnd_sub_lbl, G_CALLBACK(on_dnd_toggled), cc);

    cc->idle_btn = create_toggle_tile("tb-coffee-symbolic", "Keep Awake", "Disabled",
                                      FALSE, &cc->idle_sub_lbl, G_CALLBACK(on_idle_toggled), cc);

    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->wifi_btn, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->bt_btn, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->dnd_btn, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(popover->toggle_grid), cc->idle_btn, 1, 1, 1, 1);

    if (cc->prof_balanced && cc->prof_saver) {
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(cc->prof_balanced),
                                    GTK_TOGGLE_BUTTON(cc->prof_saver));
    }
    if (cc->prof_perf && cc->prof_saver) {
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(cc->prof_perf),
                                    GTK_TOGGLE_BUTTON(cc->prof_saver));
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
    g_signal_connect(cc->popover, "unmap", G_CALLBACK(on_cc_popover_unmap), cc);

    /* Connect PulseAudio C API */
    shell_pa_conn_open(&cc->pa, "shell-control-center", cc_context_state_cb, NULL, cc);

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
    if (!cc)
        return;

    cc->updating_vol = TRUE;
    cc->updating_bright = TRUE;
    cc->updating_profile = TRUE;

    if (cc->vol_scale)
        g_signal_handlers_disconnect_by_data(cc->vol_scale, cc);
    if (cc->bright_scale)
        g_signal_handlers_disconnect_by_data(cc->bright_scale, cc);
    if (cc->prof_saver)
        g_signal_handlers_disconnect_by_data(cc->prof_saver, cc);
    if (cc->prof_balanced)
        g_signal_handlers_disconnect_by_data(cc->prof_balanced, cc);
    if (cc->prof_perf)
        g_signal_handlers_disconnect_by_data(cc->prof_perf, cc);
    if (cc->wifi_btn)
        g_signal_handlers_disconnect_by_data(cc->wifi_btn, cc);
    if (cc->bt_btn)
        g_signal_handlers_disconnect_by_data(cc->bt_btn, cc);
    if (cc->dnd_btn)
        g_signal_handlers_disconnect_by_data(cc->dnd_btn, cc);
    if (cc->idle_btn)
        g_signal_handlers_disconnect_by_data(cc->idle_btn, cc);
    if (cc->popover) {
        g_signal_handlers_disconnect_by_data(cc->popover, cc);
        ShellControlCenterPopover *pop = (ShellControlCenterPopover *)cc->popover;
        if (pop->btn_settings)
            g_signal_handlers_disconnect_by_data(pop->btn_settings, cc);
        if (pop->btn_lock)
            g_signal_handlers_disconnect_by_data(pop->btn_lock, cc);
        if (pop->btn_suspend)
            g_signal_handlers_disconnect_by_data(pop->btn_suspend, cc);
        if (pop->btn_power)
            g_signal_handlers_disconnect_by_data(pop->btn_power, cc);
    }

    if (cc->cancellable) {
        g_cancellable_cancel(cc->cancellable);
        g_clear_object(&cc->cancellable);
    }
    if (cc->refresh_timer_id > 0) {
        g_source_remove(cc->refresh_timer_id);
        cc->refresh_timer_id = 0;
    }
    if (cc->power_confirm_timer_id > 0) {
        g_source_remove(cc->power_confirm_timer_id);
        cc->power_confirm_timer_id = 0;
    }
    if (cc->profiles_proxy) {
        g_signal_handlers_disconnect_by_data(cc->profiles_proxy, cc);
        g_object_unref(cc->profiles_proxy);
        cc->profiles_proxy = NULL;
    }

    shell_pa_conn_close(&cc->pa);
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
        if (widget->app && widget->app->idle_manager) {
            cc->idle_enabled = shell_idle_manager_get_inhibited(widget->app->idle_manager);
            shell_idle_manager_add_listener(widget->app->idle_manager,
                                            on_cc_idle_manager_changed, cc);
            update_cc_idle_ui(cc);
        }
        cc_refresh_state(cc);
    }
}

static void
control_center_disable(ShellWidget *widget)
{
    ControlCenterWidget *cc = (ControlCenterWidget *)widget;
    if (cc && widget && widget->app && widget->app->idle_manager) {
        shell_idle_manager_remove_listener(widget->app->idle_manager,
                                           on_cc_idle_manager_changed, cc);
    }
    if (cc && cc->refresh_timer_id > 0) {
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
