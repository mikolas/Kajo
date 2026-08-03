#include "widget.h"
#include "../dbus_helpers.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NM_DBUS_SERVICE           "org.freedesktop.NetworkManager"
#define NM_DBUS_PATH              "/org/freedesktop/NetworkManager"
#define NM_DBUS_INTERFACE         "org.freedesktop.NetworkManager"
#define NM_DEVICE_INTERFACE       "org.freedesktop.NetworkManager.Device"
#define NM_WIRELESS_INTERFACE     "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_INTERFACE           "org.freedesktop.NetworkManager.AccessPoint"
#define NM_ACTIVE_CONN_INTERFACE  "org.freedesktop.NetworkManager.Connection.Active"
#define NM_IP4_CONFIG_INTERFACE   "org.freedesktop.NetworkManager.IP4Config"

extern const ShellWidgetClass network_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;

    /* Popover UI elements */
    GtkWidget *wifi_switch;
    GtkWidget *active_conn_box;
    GtkWidget *active_ssid_label;
    GtkWidget *active_ip_label;
    GtkWidget *net_speed_label;
    GtkWidget *active_signal_bar;
    GtkWidget *ap_list_box;

    /* Network Throughput State */
    guint64 prev_rx_bytes;
    guint64 prev_tx_bytes;
    gint64 prev_time;

    /* D-Bus proxies */
    GDBusProxy *nm_proxy;
    guint timer_id;

    gboolean wifi_enabled;
    gboolean is_connected;
    gchar *primary_ssid;
    gchar *primary_ip;
    guint8 signal_strength;
    gboolean is_ethernet;
} NetworkWidget;

static void update_network_ui(NetworkWidget *net);
static void refresh_network_state(NetworkWidget *net);

/* ─── D-Bus Helper Functions ─── */

static void
on_wifi_switch_toggled(GtkSwitch *sw G_GNUC_UNUSED, gboolean state, gpointer user_data G_GNUC_UNUSED)
{
    shell_dbus_nm_set_wireless_enabled(state);
}

static void
fetch_primary_connection_info(NetworkWidget *net, const gchar *active_path)
{
    if (active_path == NULL || g_strcmp0(active_path, "/") == 0) {
        net->is_connected = FALSE;
        g_free(net->primary_ssid);
        net->primary_ssid = NULL;
        g_free(net->primary_ip);
        net->primary_ip = NULL;
        net->signal_strength = 0;
        net->is_ethernet = FALSE;
        return;
    }

    GError *error = NULL;
    GDBusProxy *conn_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        NM_DBUS_SERVICE, active_path, NM_ACTIVE_CONN_INTERFACE, NULL, &error);

    if (error != NULL) {
        g_clear_error(&error);
        return;
    }

    GVariant *id_var = g_dbus_proxy_get_cached_property(conn_proxy, "Id");
    GVariant *type_var = g_dbus_proxy_get_cached_property(conn_proxy, "Type");
    GVariant *ip4_var = g_dbus_proxy_get_cached_property(conn_proxy, "Ip4Config");

    if (id_var != NULL) {
        g_free(net->primary_ssid);
        net->primary_ssid = g_variant_dup_string(id_var, NULL);
        g_variant_unref(id_var);
    }

    if (type_var != NULL) {
        const gchar *type_str = g_variant_get_string(type_var, NULL);
        net->is_ethernet = (g_strcmp0(type_str, "802-3-ethernet") == 0);
        g_variant_unref(type_var);
    }

    net->is_connected = TRUE;

    if (ip4_var != NULL) {
        const gchar *ip4_path = g_variant_get_string(ip4_var, NULL);
        if (ip4_path && g_strcmp0(ip4_path, "/") != 0) {
            GDBusProxy *ip_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
                NM_DBUS_SERVICE, ip4_path, NM_IP4_CONFIG_INTERFACE, NULL, NULL);
            if (ip_proxy != NULL) {
                GVariant *addrs_var = g_dbus_proxy_get_cached_property(ip_proxy, "AddressData");
                if (addrs_var != NULL) {
                    if (g_variant_n_children(addrs_var) > 0) {
                        GVariant *child = g_variant_get_child_value(addrs_var, 0);
                        GVariant *address_val = g_variant_lookup_value(child, "address", G_VARIANT_TYPE_STRING);
                        if (address_val != NULL) {
                            g_free(net->primary_ip);
                            net->primary_ip = g_variant_dup_string(address_val, NULL);
                            g_variant_unref(address_val);
                        }
                        g_variant_unref(child);
                    }
                    g_variant_unref(addrs_var);
                }
                g_object_unref(ip_proxy);
            }
        }
        g_variant_unref(ip4_var);
    }

    g_object_unref(conn_proxy);
}

static void
refresh_network_state(NetworkWidget *net)
{
    if (net->nm_proxy == NULL)
        return;

    GVariant *wireless_var = g_dbus_proxy_get_cached_property(net->nm_proxy, "WirelessEnabled");
    if (wireless_var != NULL) {
        net->wifi_enabled = g_variant_get_boolean(wireless_var);
        g_variant_unref(wireless_var);
    }

    GVariant *primary_var = g_dbus_proxy_get_cached_property(net->nm_proxy, "PrimaryConnection");
    if (primary_var != NULL) {
        const gchar *primary_path = g_variant_get_string(primary_var, NULL);
        fetch_primary_connection_info(net, primary_path);
        g_variant_unref(primary_var);
    } else {
        net->is_connected = FALSE;
    }

    update_network_ui(net);
}

static void
update_network_ui(NetworkWidget *net)
{
    /* 1. Update Panel Icon & Label */
    const gchar *icon_name;
    gchar label_text[128];

    if (!net->is_connected) {
        icon_name = "tb-wifi-off-symbolic";
        snprintf(label_text, sizeof(label_text), "Offline");
    } else if (net->is_ethernet) {
        icon_name = "tb-wifi-symbolic";
        snprintf(label_text, sizeof(label_text), "%s",
                 net->primary_ssid ? net->primary_ssid : "Ethernet");
    } else {
        icon_name = "tb-wifi-symbolic";
        snprintf(label_text, sizeof(label_text), "%s",
                 net->primary_ssid ? net->primary_ssid : "Wi-Fi");
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(net->icon_img), icon_name);
    gtk_label_set_text(GTK_LABEL(net->label), label_text);
    shell_widget_apply_mode_visibility(net->base.mode, net->icon_img, net->label);

    /* 2. Update Popover Controls */
    if (net->wifi_switch != NULL) {
        g_signal_handlers_block_by_func(net->wifi_switch, on_wifi_switch_toggled, net);
        gtk_switch_set_active(GTK_SWITCH(net->wifi_switch), net->wifi_enabled);
        g_signal_handlers_unblock_by_func(net->wifi_switch, on_wifi_switch_toggled, net);
    }

    if (net->active_ssid_label != NULL) {
        gtk_label_set_text(GTK_LABEL(net->active_ssid_label),
                           net->primary_ssid ? net->primary_ssid : "Not Connected");
    }

    if (net->active_ip_label != NULL) {
        gchar ip_buf[64];
        snprintf(ip_buf, sizeof(ip_buf), "IP: %s",
                 net->primary_ip ? net->primary_ip : "--");
        gtk_label_set_text(GTK_LABEL(net->active_ip_label), ip_buf);
    }

    if (net->net_speed_label != NULL) {
        FILE *fp = fopen("/proc/net/dev", "r");
        if (fp) {
            char line[256];
            guint64 total_rx = 0, total_tx = 0;
            if (fgets(line, sizeof(line), fp)) {}
            if (fgets(line, sizeof(line), fp)) {}

            while (fgets(line, sizeof(line), fp)) {
                gchar *colon = strchr(line, ':');
                if (!colon) continue;
                *colon = '\0';
                gchar *iface = g_strstrip(line);
                if (g_strcmp0(iface, "lo") == 0) continue;

                guint64 rx = 0, tx = 0, dummy = 0;
                if (sscanf(colon + 1, "%lu %lu %lu %lu %lu %lu %lu %lu %lu",
                           &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx) >= 9) {
                    total_rx += rx;
                    total_tx += tx;
                }
            }
            fclose(fp);

            gint64 now = g_get_monotonic_time();
            if (net->prev_time > 0 && net->prev_rx_bytes <= total_rx) {
                double dt = (double)(now - net->prev_time) / 1000000.0;
                if (dt > 0.1) {
                    double rx_speed = (double)(total_rx - net->prev_rx_bytes) / dt;
                    double tx_speed = (double)(total_tx - net->prev_tx_bytes) / dt;

                    gchar *rx_str = rx_speed >= 1024 * 1024 ? g_strdup_printf("%.1f MB/s", rx_speed / (1024.0 * 1024.0)) : g_strdup_printf("%.0f KB/s", rx_speed / 1024.0);
                    gchar *tx_str = tx_speed >= 1024 * 1024 ? g_strdup_printf("%.1f MB/s", tx_speed / (1024.0 * 1024.0)) : g_strdup_printf("%.0f KB/s", tx_speed / 1024.0);

                    gchar *spd_buf = g_strdup_printf("Speed: ⬇ %s   ⬆ %s", rx_str, tx_str);
                    gtk_label_set_text(GTK_LABEL(net->net_speed_label), spd_buf);

                    g_free(rx_str);
                    g_free(tx_str);
                    g_free(spd_buf);
                }
            }

            net->prev_rx_bytes = total_rx;
            net->prev_tx_bytes = total_tx;
            net->prev_time = now;
        }
    }
}

static gboolean
on_timer_tick(gpointer user_data)
{
    NetworkWidget *net = user_data;
    refresh_network_state(net);
    return G_SOURCE_CONTINUE;
}

static void
on_launch_network_settings_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    if (!g_spawn_command_line_async("./builddir/kajo-settings --page network", NULL)) {
        g_spawn_command_line_async("kajo-settings --page network", NULL);
    }
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellNetworkPopover {
    GtkPopover parent_instance;

    GtkWidget *wifi_switch;
    GtkWidget *active_ssid_label;
    GtkWidget *active_ip_label;
    GtkWidget *net_speed_label;
    GtkWidget *status_badge;
    GtkWidget *settings_btn;
} ShellNetworkPopover;

typedef struct _ShellNetworkPopoverClass {
    GtkPopoverClass parent_class;
} ShellNetworkPopoverClass;

G_DEFINE_TYPE(ShellNetworkPopover, shell_network_popover, GTK_TYPE_POPOVER)

static void
shell_network_popover_init(ShellNetworkPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_network_popover_class_init(ShellNetworkPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/network_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellNetworkPopover, wifi_switch);
    gtk_widget_class_bind_template_child(widget_class, ShellNetworkPopover, active_ssid_label);
    gtk_widget_class_bind_template_child(widget_class, ShellNetworkPopover, active_ip_label);
    gtk_widget_class_bind_template_child(widget_class, ShellNetworkPopover, net_speed_label);
    gtk_widget_class_bind_template_child(widget_class, ShellNetworkPopover, status_badge);
    gtk_widget_class_bind_template_child(widget_class, ShellNetworkPopover, settings_btn);
}

/* ─── Popover Builder ─── */

static GtkWidget *
build_network_popover(NetworkWidget *net)
{
    ShellNetworkPopover *popover = g_object_new(shell_network_popover_get_type(), NULL);

    net->wifi_switch = popover->wifi_switch;
    net->active_ssid_label = popover->active_ssid_label;
    net->active_ip_label = popover->active_ip_label;
    net->net_speed_label = popover->net_speed_label;

    g_signal_connect(popover->wifi_switch, "state-set",
                     G_CALLBACK(on_wifi_switch_toggled), net);

    g_signal_connect(popover->settings_btn, "clicked",
                     G_CALLBACK(on_launch_network_settings_clicked), NULL);

    return GTK_WIDGET(popover);
}

/* ─── ShellWidget Interface ─── */

static ShellWidget *
network_widget_create(ShellCompositor *compositor)
{
    NetworkWidget *net = g_new0(NetworkWidget, 1);
    net->base.klass = &network_widget_class;
    net->compositor = compositor;
    net->wifi_enabled = TRUE;

    /* Build Panel Button */
    net->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(net->button), FALSE);
    gtk_widget_add_css_class(net->button, "shell-widget");
    gtk_widget_add_css_class(net->button, "shell-widget-network");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    net->icon_img = gtk_image_new_from_icon_name("fl-wifi-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(net->icon_img), shell_widget_get_icon_size((ShellWidget *)net));
    net->label = gtk_label_new("Wi-Fi");

    gtk_box_append(GTK_BOX(hbox), net->icon_img);
    gtk_box_append(GTK_BOX(hbox), net->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(net->button), hbox);

    /* Build Popover */
    net->popover = build_network_popover(net);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(net->button), net->popover);

    /* Init NetworkManager D-Bus Proxy */
    GError *error = NULL;
    net->nm_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        NM_DBUS_SERVICE, NM_DBUS_PATH, NM_DBUS_INTERFACE, NULL, &error);

    if (error != NULL) {
        g_clear_error(&error);
    }

    refresh_network_state(net);
    net->timer_id = g_timeout_add_seconds(3, on_timer_tick, net);

    return (ShellWidget *)net;
}

static void
network_widget_destroy(ShellWidget *widget)
{
    NetworkWidget *net = (NetworkWidget *)widget;
    if (net == NULL)
        return;

    if (net->timer_id != 0) {
        g_source_remove(net->timer_id);
        net->timer_id = 0;
    }

    if (net->nm_proxy != NULL) {
        g_object_unref(net->nm_proxy);
        net->nm_proxy = NULL;
    }

    g_free(net->primary_ssid);
    g_free(net->primary_ip);
    g_free(net);
}

static GtkWidget *
network_widget_get_widget(ShellWidget *widget)
{
    NetworkWidget *net = (NetworkWidget *)widget;
    return net->button;
}

static void
network_widget_enable(ShellWidget *widget)
{
    NetworkWidget *net = (NetworkWidget *)widget;
    if (net) {
        shell_widget_apply_mode_visibility(widget->mode, net->icon_img, net->label);
        if (net->timer_id == 0) {
            net->timer_id = g_timeout_add_seconds(3, on_timer_tick, net);
        }
    }
}

static void
network_widget_disable(ShellWidget *widget)
{
    NetworkWidget *net = (NetworkWidget *)widget;
    if (net && net->timer_id > 0) {
        g_source_remove(net->timer_id);
        net->timer_id = 0;
    }
}

const ShellWidgetClass network_widget_class = {
    .id = "network",
    .name = "Network & Wi-Fi",
    .create = network_widget_create,
    .destroy = network_widget_destroy,
    .get_widget = network_widget_get_widget,
    .enable = network_widget_enable,
    .disable = network_widget_disable,
};
