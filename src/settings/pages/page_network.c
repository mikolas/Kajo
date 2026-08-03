#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../dbus_helpers.h"
#include <string.h>

static void
on_net_wifi_switch_toggled(GtkSwitch *sw G_GNUC_UNUSED, gboolean state, gpointer user_data G_GNUC_UNUSED)
{
    shell_dbus_nm_set_wireless_enabled(state);
}

static void
on_net_ap_connect_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const char *ap_path = g_object_get_data(G_OBJECT(btn), "ap-path");
    if (!ap_path) return;

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) return;

    GDBusProxy *nm_proxy = g_dbus_proxy_new_sync(
        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager", NULL, &error);

    if (nm_proxy) {
        gchar *wifi_dev_path = NULL;
        GVariant *res = g_dbus_proxy_call_sync(
            nm_proxy, "GetDevices", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

        if (res) {
            GVariant *dev_arr = g_variant_get_child_value(res, 0);
            GVariantIter iter;
            g_variant_iter_init(&iter, dev_arr);
            const char *dev_path = NULL;

            while (g_variant_iter_loop(&iter, "&o", &dev_path)) {
                GDBusProxy *dev_proxy = g_dbus_proxy_new_sync(
                    bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                    "org.freedesktop.NetworkManager", dev_path,
                    "org.freedesktop.NetworkManager.Device", NULL, NULL);
                if (dev_proxy) {
                    GVariant *type_var = g_dbus_proxy_get_cached_property(dev_proxy, "DeviceType");
                    guint type_val = type_var ? g_variant_get_uint32(type_var) : 0;
                    if (type_var) g_variant_unref(type_var);
                    if (type_val == 2) {
                        wifi_dev_path = g_strdup(dev_path);
                        g_object_unref(dev_proxy);
                        break;
                    }
                    g_object_unref(dev_proxy);
                }
            }
            g_variant_unref(dev_arr);
            g_variant_unref(res);
        }

        const char *target_dev = wifi_dev_path ? wifi_dev_path : "/org/freedesktop/NetworkManager/Devices/2";

        g_dbus_proxy_call(
            nm_proxy, "ActivateConnection",
            g_variant_new("(ooo)", "/", target_dev, ap_path),
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

        g_free(wifi_dev_path);
        g_object_unref(nm_proxy);
    }
    if (error) g_clear_error(&error);
    g_object_unref(bus);
}

typedef struct {
    GtkWidget *card_act;
    GtkWidget *list_scan;
    GtkWidget *sw_wifi;
} NetworkScanData;

static void populate_real_network_data(GtkWidget *card_act, GtkWidget *list_scan, GtkWidget *sw_wifi);
static void on_net_scan_clicked(GtkButton *btn, gpointer user_data);

static gboolean
on_net_scan_refresh_timeout(gpointer user_data)
{
    NetworkScanData *data = user_data;
    if (data) {
        if (data->card_act && data->list_scan && data->sw_wifi) {
            GtkWidget *child;
            while ((child = gtk_widget_get_first_child(data->card_act)) != NULL) {
                gtk_box_remove(GTK_BOX(data->card_act), child);
            }
            while ((child = gtk_widget_get_first_child(data->list_scan)) != NULL) {
                gtk_box_remove(GTK_BOX(data->list_scan), child);
            }
            GtkWidget *btn_scan_net = gtk_button_new_with_label("[ 🔍 REFRESH WI-FI SCAN ]");
            g_object_set_data(G_OBJECT(btn_scan_net), "card_act", data->card_act);
            g_object_set_data(G_OBJECT(btn_scan_net), "list_scan", data->list_scan);
            g_object_set_data(G_OBJECT(btn_scan_net), "sw_wifi", data->sw_wifi);
            g_signal_connect(btn_scan_net, "clicked", G_CALLBACK(on_net_scan_clicked), NULL);
            gtk_box_append(GTK_BOX(data->list_scan), btn_scan_net);

            populate_real_network_data(data->card_act, data->list_scan, data->sw_wifi);
        }
        if (data->card_act) g_object_remove_weak_pointer(G_OBJECT(data->card_act), (gpointer *)&data->card_act);
        if (data->list_scan) g_object_remove_weak_pointer(G_OBJECT(data->list_scan), (gpointer *)&data->list_scan);
        if (data->sw_wifi) g_object_remove_weak_pointer(G_OBJECT(data->sw_wifi), (gpointer *)&data->sw_wifi);
        g_free(data);
    }
    return G_SOURCE_REMOVE;
}

static void
on_net_scan_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *card_act = g_object_get_data(G_OBJECT(btn), "card_act");
    GtkWidget *list_scan = g_object_get_data(G_OBJECT(btn), "list_scan");
    GtkWidget *sw_wifi = g_object_get_data(G_OBJECT(btn), "sw_wifi");

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus) {
        GDBusProxy *nm_proxy = g_dbus_proxy_new_sync(
            bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
            "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
            "org.freedesktop.NetworkManager", NULL, &error);

        if (nm_proxy) {
            GVariant *res = g_dbus_proxy_call_sync(
                nm_proxy, "GetDevices", NULL,
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

            if (res) {
                GVariant *dev_arr = g_variant_get_child_value(res, 0);
                GVariantIter iter;
                g_variant_iter_init(&iter, dev_arr);
                const char *dev_path = NULL;

                while (g_variant_iter_loop(&iter, "o", &dev_path)) {
                    GDBusProxy *dev_proxy = g_dbus_proxy_new_sync(
                        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                        "org.freedesktop.NetworkManager", dev_path,
                        "org.freedesktop.NetworkManager.Device", NULL, NULL);
                    if (dev_proxy) {
                        GVariant *type_var = g_dbus_proxy_get_cached_property(dev_proxy, "DeviceType");
                        guint type_val = type_var ? g_variant_get_uint32(type_var) : 0;
                        if (type_var) g_variant_unref(type_var);

                        if (type_val == 2) {
                            GDBusProxy *wifi_proxy = g_dbus_proxy_new_sync(
                                bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                "org.freedesktop.NetworkManager", dev_path,
                                "org.freedesktop.NetworkManager.Device.Wireless", NULL, NULL);
                            if (wifi_proxy) {
                                g_dbus_proxy_call(
                                    wifi_proxy, "RequestScan",
                                    g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
                                    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
                                g_object_unref(wifi_proxy);
                            }
                            g_object_unref(dev_proxy);
                            break;
                        }
                        g_object_unref(dev_proxy);
                    }
                }
                g_variant_unref(dev_arr);
                g_variant_unref(res);
            }
            g_object_unref(nm_proxy);
        }
        if (error) g_clear_error(&error);
        g_object_unref(bus);
    }

    if (card_act && list_scan && sw_wifi) {
        NetworkScanData *data = g_new0(NetworkScanData, 1);
        data->card_act = card_act;
        data->list_scan = list_scan;
        data->sw_wifi = sw_wifi;
        g_object_add_weak_pointer(G_OBJECT(card_act), (gpointer *)&data->card_act);
        g_object_add_weak_pointer(G_OBJECT(list_scan), (gpointer *)&data->list_scan);
        g_object_add_weak_pointer(G_OBJECT(sw_wifi), (gpointer *)&data->sw_wifi);
        g_timeout_add(1500, on_net_scan_refresh_timeout, data);
    }
}

static void
populate_real_network_data(GtkWidget *card_act, GtkWidget *list_scan, GtkWidget *sw_wifi)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        return;
    }

    GDBusProxy *nm_proxy = g_dbus_proxy_new_sync(
        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager", NULL, &error);

    if (error) g_clear_error(&error);

    if (nm_proxy) {
        GVariant *wifi_var = g_dbus_proxy_get_cached_property(nm_proxy, "WirelessEnabled");
        if (wifi_var) {
            gtk_switch_set_active(GTK_SWITCH(sw_wifi), g_variant_get_boolean(wifi_var));
            g_variant_unref(wifi_var);
        }

        /* Fetch Active Connections */
        GVariant *active_conns = g_dbus_proxy_get_cached_property(nm_proxy, "ActiveConnections");
        gboolean found_active = FALSE;

        if (active_conns) {
            GVariantIter iter;
            g_variant_iter_init(&iter, active_conns);
            const char *conn_path = NULL;

            while (g_variant_iter_loop(&iter, "o", &conn_path)) {
                GDBusProxy *conn_proxy = g_dbus_proxy_new_sync(
                    bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                    "org.freedesktop.NetworkManager", conn_path,
                    "org.freedesktop.NetworkManager.Connection.Active", NULL, NULL);

                if (conn_proxy) {
                    GVariant *id_var = g_dbus_proxy_get_cached_property(conn_proxy, "Id");
                    GVariant *ip4_var = g_dbus_proxy_get_cached_property(conn_proxy, "Ip4Config");

                    const char *id_str = id_var ? g_variant_get_string(id_var, NULL) : "Active Network";
                    const char *ip_str = "--";
                    gchar *ip_alloc = NULL;

                    if (ip4_var) {
                        const char *ip4_path = g_variant_get_string(ip4_var, NULL);
                        if (ip4_path && strcmp(ip4_path, "/") != 0) {
                            GDBusProxy *ip_proxy = g_dbus_proxy_new_sync(
                                bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                "org.freedesktop.NetworkManager", ip4_path,
                                "org.freedesktop.NetworkManager.IP4Config", NULL, NULL);
                            if (ip_proxy) {
                                GVariant *addrs_var = g_dbus_proxy_get_cached_property(ip_proxy, "AddressData");
                                if (addrs_var && g_variant_n_children(addrs_var) > 0) {
                                    GVariant *child = g_variant_get_child_value(addrs_var, 0);
                                    GVariant *addr_val = g_variant_lookup_value(child, "address", G_VARIANT_TYPE_STRING);
                                    if (addr_val) {
                                        ip_alloc = g_strdup(g_variant_get_string(addr_val, NULL));
                                        ip_str = ip_alloc;
                                        g_variant_unref(addr_val);
                                    }
                                    g_variant_unref(child);
                                }
                                if (addrs_var) g_variant_unref(addrs_var);
                                g_object_unref(ip_proxy);
                            }
                        }
                    }

                    gchar *ssid_msg = g_strdup_printf("Connected Network: %s", id_str);
                    gchar *ip_msg = g_strdup_printf("IPv4 Address: %s", ip_str);

                    GtkWidget *lbl_ssid = gtk_label_new(ssid_msg);
                    gtk_widget_set_halign(lbl_ssid, GTK_ALIGN_START);
                    GtkWidget *lbl_ip = gtk_label_new(ip_msg);
                    gtk_widget_set_halign(lbl_ip, GTK_ALIGN_START);

                    gtk_box_append(GTK_BOX(card_act), lbl_ssid);
                    gtk_box_append(GTK_BOX(card_act), lbl_ip);

                    g_free(ssid_msg);
                    g_free(ip_msg);
                    g_free(ip_alloc);
                    if (id_var) g_variant_unref(id_var);
                    if (ip4_var) g_variant_unref(ip4_var);
                    g_object_unref(conn_proxy);
                    found_active = TRUE;
                    break;
                }
            }
            g_variant_unref(active_conns);
        }

        if (!found_active) {
            GtkWidget *lbl_none = gtk_label_new("No active network connection currently established.");
            gtk_widget_set_halign(lbl_none, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(card_act), lbl_none);
        }

        /* Scan Real Access Points on Wi-Fi Device */
        GPtrArray *aps = shell_dbus_nm_get_access_points();
        guint ap_count = 0;

        for (guint i = 0; i < aps->len; i++) {
            ShellAccessPoint *ap = g_ptr_array_index(aps, i);
            if (!ap->ssid || strlen(ap->ssid) == 0) continue;

            ap_count++;
            const char *sec_str = "Encrypted (WPA/WPA2/WPA3)";
            GtkWidget *btn_conn = create_action_button(
                "Connect", "ap-path", ap->path,
                G_CALLBACK(on_net_ap_connect_clicked), NULL);

            gchar *sub = g_strdup_printf("Signal: %u%% | %s", ap->strength, sec_str);
            GtkWidget *row = create_simple_item_row("network-wireless-symbolic", ap->ssid, sub, btn_conn);
            g_free(sub);

            gtk_box_append(GTK_BOX(list_scan), row);
        }
        g_ptr_array_unref(aps);

        if (ap_count == 0) {
            GtkWidget *lbl_empty = gtk_label_new("No Wi-Fi access points found. Click '[ 🔍 REFRESH WI-FI SCAN ]'.");
            gtk_widget_add_css_class(lbl_empty, "dim-label");
            gtk_label_set_xalign(GTK_LABEL(lbl_empty), 0.0f);
            gtk_box_append(GTK_BOX(list_scan), lbl_empty);
        }

        g_object_unref(nm_proxy);
    }
    g_object_unref(bus);
}


/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageNetwork {
    GtkBox parent_instance;

    GtkWidget *sw_wifi;
    GtkWidget *card_act;
    GtkWidget *btn_scan;
    GtkWidget *list_scan;
} SettingsPageNetwork;

typedef struct _SettingsPageNetworkClass {
    GtkBoxClass parent_class;
} SettingsPageNetworkClass;

G_DEFINE_TYPE(SettingsPageNetwork, settings_page_network, GTK_TYPE_BOX)

static void
settings_page_network_init(SettingsPageNetwork *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_network_class_init(SettingsPageNetworkClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_network.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageNetwork, sw_wifi);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNetwork, card_act);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNetwork, btn_scan);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageNetwork, list_scan);
}

GtkWidget *
build_page_network(void)
{
    SettingsPageNetwork *page = g_object_new(settings_page_network_get_type(), NULL);

    g_signal_connect(page->sw_wifi, "state-set", G_CALLBACK(on_net_wifi_switch_toggled), NULL);

    populate_real_network_data(page->card_act, page->list_scan, page->sw_wifi);

    g_object_set_data(G_OBJECT(page->btn_scan), "card_act", page->card_act);
    g_object_set_data(G_OBJECT(page->btn_scan), "list_scan", page->list_scan);
    g_object_set_data(G_OBJECT(page->btn_scan), "sw_wifi", page->sw_wifi);

    g_signal_connect(page->btn_scan, "clicked", G_CALLBACK(on_net_scan_clicked), NULL);

    return create_settings_page_card("NETWORKMANAGER WI-FI & ETHERNET", "[ LIVE D-BUS ]", GTK_WIDGET(page), NULL);
}
