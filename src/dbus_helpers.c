#include "dbus_helpers.h"
#include <string.h>

void
shell_bluetooth_device_free(ShellBluetoothDevice *dev)
{
    if (!dev) return;
    g_free(dev->name);
    g_free(dev->path);
    g_free(dev->icon);
    g_free(dev->address);
    g_free(dev);
}

void
shell_access_point_free(ShellAccessPoint *ap)
{
    if (!ap) return;
    g_free(ap->ssid);
    g_free(ap->path);
    g_free(ap);
}

gboolean
shell_dbus_set_property_sync(GBusType bus_type,
                              const char *bus_name,
                              const char *object_path,
                              const char *interface_name,
                              const char *property_name,
                              GVariant *value)
{
    if (value)
        g_variant_ref_sink(value);
    else
        return FALSE;

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(bus_type, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        g_variant_unref(value);
        return FALSE;
    }

    GDBusProxy *proxy = g_dbus_proxy_new_sync(
        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
        bus_name, object_path, interface_name, NULL, &error);

    gboolean success = FALSE;
    if (proxy) {
        GVariant *res = g_dbus_proxy_call_sync(
            proxy, "org.freedesktop.DBus.Properties.Set",
            g_variant_new("(ssv)", interface_name, property_name, value),
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
        if (res) {
            g_variant_unref(res);
            success = TRUE;
        }
        g_object_unref(proxy);
    }

    if (error) g_clear_error(&error);
    g_variant_unref(value);
    g_object_unref(bus);
    return success;
}

GVariant *
shell_dbus_call_method_sync(GBusType bus_type,
                              const char *bus_name,
                              const char *object_path,
                              const char *interface_name,
                              const char *method_name,
                              GVariant *parameters)
{
    if (parameters)
        g_variant_ref_sink(parameters);

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(bus_type, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        if (parameters) g_variant_unref(parameters);
        return NULL;
    }

    GDBusProxy *proxy = g_dbus_proxy_new_sync(
        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
        bus_name, object_path, interface_name, NULL, &error);

    GVariant *result = NULL;
    if (proxy) {
        result = g_dbus_proxy_call_sync(
            proxy, method_name, parameters,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
        g_object_unref(proxy);
    }

    if (error) g_clear_error(&error);
    if (parameters) g_variant_unref(parameters);
    g_object_unref(bus);
    return result;
}

void
shell_dbus_bluez_set_powered(gboolean powered)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        return;
    }

    GDBusObjectManager *om = g_dbus_object_manager_client_new_sync(
        bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "org.bluez", "/", NULL, NULL, NULL, NULL, &error);

    if (om) {
        GList *objects = g_dbus_object_manager_get_objects(om);
        for (GList *l = objects; l != NULL; l = l->next) {
            GDBusObject *obj = G_DBUS_OBJECT(l->data);
            GDBusInterface *iface = g_dbus_object_get_interface(obj, "org.bluez.Adapter1");
            if (iface) {
                GDBusProxy *proxy = G_DBUS_PROXY(iface);
                g_dbus_proxy_call(
                    proxy, "org.freedesktop.DBus.Properties.Set",
                    g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(powered)),
                    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
                g_object_unref(iface);
            }
        }
        g_list_free_full(objects, g_object_unref);
        g_object_unref(om);
    }

    if (error) g_clear_error(&error);
    g_object_unref(bus);
}

gboolean
shell_dbus_bluez_get_powered(void)
{
    gboolean powered = FALSE;
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        return FALSE;
    }

    GDBusObjectManager *om = g_dbus_object_manager_client_new_sync(
        bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "org.bluez", "/", NULL, NULL, NULL, NULL, &error);
    if (error) g_clear_error(&error);

    if (om) {
        GList *objects = g_dbus_object_manager_get_objects(om);
        for (GList *l = objects; l != NULL; l = l->next) {
            GDBusObject *obj = G_DBUS_OBJECT(l->data);
            GDBusInterface *iface = g_dbus_object_get_interface(obj, "org.bluez.Adapter1");
            if (iface) {
                GDBusProxy *proxy = G_DBUS_PROXY(iface);
                GVariant *v_pow = g_dbus_proxy_get_cached_property(proxy, "Powered");
                if (v_pow) {
                    powered = g_variant_get_boolean(v_pow);
                    g_variant_unref(v_pow);
                }
                g_object_unref(iface);
                break;
            }
        }
        g_list_free_full(objects, g_object_unref);
        g_object_unref(om);
    }

    g_object_unref(bus);
    return powered;
}

gboolean
shell_dbus_bluez_device_action(const char *dev_path, const char *action)
{
    if (!dev_path || !action) return FALSE;
    GError *error = NULL;

    if (g_strcmp0(action, "RemoveDevice") == 0) {
        gchar *adapter_path = NULL;
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (bus) {
            GDBusProxy *dev_proxy = g_dbus_proxy_new_sync(
                bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                "org.bluez", dev_path, "org.bluez.Device1", NULL, NULL);
            if (dev_proxy) {
                GVariant *prop = g_dbus_proxy_get_cached_property(dev_proxy, "Adapter");
                if (prop) {
                    adapter_path = g_strdup(g_variant_get_string(prop, NULL));
                    g_variant_unref(prop);
                }
                g_object_unref(dev_proxy);
            }
            g_object_unref(bus);
        }

        if (!adapter_path) {
            const char *dev_sub = strstr(dev_path, "/dev_");
            if (dev_sub) {
                adapter_path = g_strndup(dev_path, dev_sub - dev_path);
            }
        }

        if (adapter_path) {
            GVariant *res = shell_dbus_call_method_sync(
                G_BUS_TYPE_SYSTEM, "org.bluez", adapter_path,
                "org.bluez.Adapter1", "RemoveDevice", g_variant_new("(o)", dev_path));
            g_free(adapter_path);
            if (res) {
                g_variant_unref(res);
                return TRUE;
            }
        }
        if (error) g_clear_error(&error);
        return FALSE;
    }

    GVariant *res = shell_dbus_call_method_sync(
        G_BUS_TYPE_SYSTEM, "org.bluez", dev_path,
        "org.bluez.Device1", action, NULL);

    if (res) {
        g_variant_unref(res);
        return TRUE;
    }
    if (error) g_clear_error(&error);
    return FALSE;
}

void
shell_dbus_bluez_device_action_async(const char *dev_path,
                                     const char *action,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
    if (!dev_path || !action) {
        if (callback) callback(NULL, NULL, user_data);
        return;
    }

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        if (callback) callback(NULL, NULL, user_data);
        return;
    }

    if (g_strcmp0(action, "RemoveDevice") == 0) {
        gchar *adapter_path = NULL;
        GDBusProxy *dev_proxy = g_dbus_proxy_new_sync(
            bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
            "org.bluez", dev_path, "org.bluez.Device1", NULL, NULL);
        if (dev_proxy) {
            GVariant *prop = g_dbus_proxy_get_cached_property(dev_proxy, "Adapter");
            if (prop) {
                adapter_path = g_strdup(g_variant_get_string(prop, NULL));
                g_variant_unref(prop);
            }
            g_object_unref(dev_proxy);
        }

        if (!adapter_path) {
            const char *dev_sub = strstr(dev_path, "/dev_");
            if (dev_sub) {
                adapter_path = g_strndup(dev_path, dev_sub - dev_path);
            }
        }

        if (adapter_path) {
            g_dbus_connection_call(
                bus, "org.bluez", adapter_path,
                "org.bluez.Adapter1", "RemoveDevice",
                g_variant_new("(o)", dev_path),
                NULL, G_DBUS_CALL_FLAGS_NONE, 15000, NULL, callback, user_data);
            g_free(adapter_path);
        } else if (callback) {
            callback(NULL, NULL, user_data);
        }
        g_object_unref(bus);
        return;
    }

    g_dbus_connection_call(
        bus, "org.bluez", dev_path,
        "org.bluez.Device1", action,
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 15000, NULL, callback, user_data);
    g_object_unref(bus);
}

GPtrArray *
shell_dbus_bluez_get_devices(void)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)shell_bluetooth_device_free);
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        return arr;
    }

    GDBusObjectManager *om = g_dbus_object_manager_client_new_sync(
        bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "org.bluez", "/", NULL, NULL, NULL, NULL, &error);

    if (om) {
        GList *objects = g_dbus_object_manager_get_objects(om);
        for (GList *l = objects; l != NULL; l = l->next) {
            GDBusObject *obj = G_DBUS_OBJECT(l->data);
            GDBusInterface *iface = g_dbus_object_get_interface(obj, "org.bluez.Device1");
            if (!iface) continue;

            GDBusProxy *proxy = G_DBUS_PROXY(iface);
            const char *obj_path = g_dbus_object_get_object_path(obj);

            GVariant *name_var = g_dbus_proxy_get_cached_property(proxy, "Name");
            GVariant *alias_var = g_dbus_proxy_get_cached_property(proxy, "Alias");
            GVariant *icon_var = g_dbus_proxy_get_cached_property(proxy, "Icon");
            GVariant *addr_var = g_dbus_proxy_get_cached_property(proxy, "Address");
            GVariant *paired_var = g_dbus_proxy_get_cached_property(proxy, "Paired");
            GVariant *conn_var = g_dbus_proxy_get_cached_property(proxy, "Connected");

            const char *name_str = name_var ? g_variant_get_string(name_var, NULL) : NULL;
            const char *alias_str = alias_var ? g_variant_get_string(alias_var, NULL) : NULL;

            const char *dev_name = (name_str && strlen(name_str) > 0) ? name_str : alias_str;
            if (dev_name && strlen(dev_name) > 0) {
                ShellBluetoothDevice *dev = g_new0(ShellBluetoothDevice, 1);
                dev->name = g_strdup(dev_name);
                dev->path = g_strdup(obj_path);
                dev->icon = icon_var ? g_variant_dup_string(icon_var, NULL) : g_strdup("bluetooth-symbolic");
                dev->address = addr_var ? g_variant_dup_string(addr_var, NULL) : NULL;
                dev->paired = paired_var ? g_variant_get_boolean(paired_var) : FALSE;
                dev->connected = conn_var ? g_variant_get_boolean(conn_var) : FALSE;
                g_ptr_array_add(arr, dev);
            }

            if (name_var) g_variant_unref(name_var);
            if (alias_var) g_variant_unref(alias_var);
            if (icon_var) g_variant_unref(icon_var);
            if (addr_var) g_variant_unref(addr_var);
            if (paired_var) g_variant_unref(paired_var);
            if (conn_var) g_variant_unref(conn_var);
            g_object_unref(iface);
        }
        g_list_free_full(objects, g_object_unref);
        g_object_unref(om);
    }

    if (error) g_clear_error(&error);
    g_object_unref(bus);
    return arr;
}

void
shell_dbus_nm_set_wireless_enabled(gboolean enabled)
{
    shell_dbus_set_property_sync(
        G_BUS_TYPE_SYSTEM, "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
        "WirelessEnabled", g_variant_new_boolean(enabled));
}

gboolean
shell_dbus_nm_get_wireless_enabled(void)
{
    gboolean enabled = FALSE;
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        return FALSE;
    }

    GDBusProxy *nm_proxy = g_dbus_proxy_new_sync(
        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager", NULL, &error);
    if (error) g_clear_error(&error);

    if (nm_proxy) {
        GVariant *wifi_var = g_dbus_proxy_get_cached_property(nm_proxy, "WirelessEnabled");
        if (wifi_var) {
            enabled = g_variant_get_boolean(wifi_var);
            g_variant_unref(wifi_var);
        }
        g_object_unref(nm_proxy);
    }

    g_object_unref(bus);
    return enabled;
}

GPtrArray *
shell_dbus_nm_get_access_points(void)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)shell_access_point_free);
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        if (error) g_clear_error(&error);
        return arr;
    }

    GDBusProxy *nm_proxy = g_dbus_proxy_new_sync(
        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager", NULL, &error);

    if (nm_proxy) {
        GVariant *devs_var = g_dbus_proxy_call_sync(
            nm_proxy, "GetDevices", NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);

        if (devs_var) {
            GVariantIter *iter = NULL;
            g_variant_get(devs_var, "(ao)", &iter);
            const char *dev_path = NULL;
            while (g_variant_iter_next(iter, "&o", &dev_path)) {
                GDBusProxy *dev_proxy = g_dbus_proxy_new_sync(
                    bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                    "org.freedesktop.NetworkManager", dev_path,
                    "org.freedesktop.NetworkManager.Device", NULL, NULL);

                if (!dev_proxy) continue;

                GVariant *type_var = g_dbus_proxy_get_cached_property(dev_proxy, "DeviceType");
                guint32 dev_type = type_var ? g_variant_get_uint32(type_var) : 0;
                if (type_var) g_variant_unref(type_var);

                if (dev_type == 2) { /* NM_DEVICE_TYPE_WIFI */
                    GDBusProxy *wifi_proxy = g_dbus_proxy_new_sync(
                        bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                        "org.freedesktop.NetworkManager", dev_path,
                        "org.freedesktop.NetworkManager.Device.Wireless", NULL, NULL);

                    if (wifi_proxy) {
                        GVariant *active_ap_var = g_dbus_proxy_get_cached_property(wifi_proxy, "ActiveAccessPoint");
                        const char *active_ap_path = active_ap_var ? g_variant_get_string(active_ap_var, NULL) : NULL;

                        GVariant *aps_var = g_dbus_proxy_call_sync(
                            wifi_proxy, "GetAccessPoints", NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);

                        if (aps_var) {
                            GVariantIter *ap_iter = NULL;
                            g_variant_get(aps_var, "(ao)", &ap_iter);
                            const char *ap_path = NULL;
                            while (g_variant_iter_next(ap_iter, "&o", &ap_path)) {
                                GDBusProxy *ap_proxy = g_dbus_proxy_new_sync(
                                    bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                    "org.freedesktop.NetworkManager", ap_path,
                                    "org.freedesktop.NetworkManager.AccessPoint", NULL, NULL);

                                if (ap_proxy) {
                                    GVariant *ssid_var = g_dbus_proxy_get_cached_property(ap_proxy, "Ssid");
                                    GVariant *strength_var = g_dbus_proxy_get_cached_property(ap_proxy, "Strength");

                                    if (ssid_var) {
                                        gsize n_elms = 0;
                                        const guint8 *bytes = g_variant_get_fixed_array(ssid_var, &n_elms, sizeof(guint8));
                                        if (n_elms > 0) {
                                            char *ssid_str = g_strndup((const char *)bytes, n_elms);
                                            if (ssid_str && strlen(ssid_str) > 0) {
                                                ShellAccessPoint *ap = g_new0(ShellAccessPoint, 1);
                                                ap->ssid = ssid_str;
                                                ap->path = g_strdup(ap_path);
                                                ap->strength = strength_var ? g_variant_get_byte(strength_var) : 0;
                                                ap->active = (active_ap_path && g_strcmp0(ap_path, active_ap_path) == 0);
                                                g_ptr_array_add(arr, ap);
                                            } else {
                                                g_free(ssid_str);
                                            }
                                        }
                                        g_variant_unref(ssid_var);
                                    }
                                    if (strength_var) g_variant_unref(strength_var);
                                    g_object_unref(ap_proxy);
                                }
                            }
                            g_variant_iter_free(ap_iter);
                            g_variant_unref(aps_var);
                        }
                        if (active_ap_var) g_variant_unref(active_ap_var);
                        g_object_unref(wifi_proxy);
                    }
                }
                g_object_unref(dev_proxy);
            }
            g_variant_iter_free(iter);
            g_variant_unref(devs_var);
        }
        g_object_unref(nm_proxy);
    }

    if (error) g_clear_error(&error);
    g_object_unref(bus);
    return arr;
}
