#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../dbus_helpers.h"
#include <string.h>

static void
on_bt_power_switch_toggled(GtkSwitch *sw G_GNUC_UNUSED, gboolean state, gpointer user_data G_GNUC_UNUSED)
{
    shell_dbus_bluez_set_powered(state);
}

static void
on_bt_device_connect_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "dev-path");
    if (path) shell_dbus_bluez_device_action(path, "Connect");
}

static void
on_bt_device_disconnect_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "dev-path");
    if (path) shell_dbus_bluez_device_action(path, "Disconnect");
}

static void
on_bt_device_forget_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "dev-path");
    if (path) shell_dbus_bluez_device_action(path, "RemoveDevice");
}

typedef struct {
    GtkWidget *list_devices;
    GtkWidget *list_disc;
} BluetoothScanData;

static void
populate_real_bluetooth_devices(GtkWidget *list_devices, GtkWidget *list_disc)
{
    GPtrArray *devices = shell_dbus_bluez_get_devices();
    guint paired_count = 0;
    guint disc_count = 0;

    for (guint i = 0; i < devices->len; i++) {
        ShellBluetoothDevice *dev = g_ptr_array_index(devices, i);
        if (!dev->name || is_mac_address_string(dev->name)) continue;

        const char *icon_name = "bluetooth-symbolic";
        if (dev->icon) {
            if (strstr(dev->icon, "headphone") || strstr(dev->icon, "headset") || strstr(dev->icon, "audio"))
                icon_name = "audio-headphones-symbolic";
            else if (strstr(dev->icon, "mouse"))
                icon_name = "input-mouse-symbolic";
            else if (strstr(dev->icon, "keyboard"))
                icon_name = "input-keyboard-symbolic";
            else if (strstr(dev->icon, "phone"))
                icon_name = "phone-symbolic";
        }

        if (dev->paired) {
            paired_count++;
            GtkWidget *btn_conn = create_action_button(
                dev->connected ? "Disconnect" : "Connect",
                "dev-path", dev->path,
                G_CALLBACK(dev->connected ? on_bt_device_disconnect_clicked : on_bt_device_connect_clicked), NULL);

            GtkWidget *btn_forget = create_action_button(
                "Forget", "dev-path", dev->path,
                G_CALLBACK(on_bt_device_forget_clicked), NULL);

            GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(btn_box), btn_conn);
            gtk_box_append(GTK_BOX(btn_box), btn_forget);

            gchar *sub = g_strdup_printf("%s | Address: %s", dev->connected ? "Connected" : "Paired (Offline)", dev->path);
            GtkWidget *row = create_simple_item_row(icon_name, dev->name, sub, btn_box);
            g_free(sub);

            gtk_box_append(GTK_BOX(list_devices), row);
        } else {
            disc_count++;
            GtkWidget *btn_pair = create_action_button(
                "Pair Device", "dev-path", dev->path,
                G_CALLBACK(on_bt_device_connect_clicked), NULL);

            gchar *sub = g_strdup_printf("Discovered Nearby Device | %s", dev->path);
            GtkWidget *row = create_simple_item_row(icon_name, dev->name, sub, btn_pair);
            g_free(sub);

            gtk_box_append(GTK_BOX(list_disc), row);
        }
    }

    if (paired_count == 0) {
        gtk_box_append(GTK_BOX(list_devices), create_empty_state_label("No paired Bluetooth devices found on system."));
    }

    if (disc_count == 0) {
        gtk_box_append(GTK_BOX(list_disc), create_empty_state_label("No nearby Bluetooth devices discovered. Click '[ 🔍 SCAN FOR NEARBY BLUETOOTH DEVICES ]'."));
    }

    g_ptr_array_unref(devices);
}
static gboolean
on_bt_scan_refresh_timeout(gpointer user_data)
{
    BluetoothScanData *data = user_data;
    if (data) {
        if (data->list_devices && data->list_disc) {
            GtkWidget *child;
            while ((child = gtk_widget_get_first_child(data->list_devices)) != NULL) {
                gtk_box_remove(GTK_BOX(data->list_devices), child);
            }
            while ((child = gtk_widget_get_first_child(data->list_disc)) != NULL) {
                gtk_box_remove(GTK_BOX(data->list_disc), child);
            }
            populate_real_bluetooth_devices(data->list_devices, data->list_disc);
        }
        if (data->list_devices) g_object_remove_weak_pointer(G_OBJECT(data->list_devices), (gpointer *)&data->list_devices);
        if (data->list_disc) g_object_remove_weak_pointer(G_OBJECT(data->list_disc), (gpointer *)&data->list_disc);
        g_free(data);
    }
    return G_SOURCE_REMOVE;
}

static void
on_bt_scan_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
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

    if (error) g_clear_error(&error);

    if (om) {
        GList *objects = g_dbus_object_manager_get_objects(om);
        for (GList *l = objects; l != NULL; l = l->next) {
            GDBusObject *obj = G_DBUS_OBJECT(l->data);
            GDBusInterface *iface = g_dbus_object_get_interface(obj, "org.bluez.Adapter1");
            if (iface) {
                const char *adapter_path = g_dbus_object_get_object_path(obj);
                GDBusProxy *adapter_proxy = g_dbus_proxy_new_sync(
                    bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                    "org.bluez", adapter_path, "org.bluez.Adapter1", NULL, NULL);
                if (adapter_proxy) {
                    g_dbus_proxy_call(adapter_proxy, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
                    g_object_unref(adapter_proxy);
                }
                g_object_unref(iface);
            }
        }
        g_list_free_full(objects, g_object_unref);
        g_object_unref(om);
    }
    g_object_unref(bus);

    GtkWidget *list_devices = g_object_get_data(G_OBJECT(btn), "list_devices");
    GtkWidget *list_disc = g_object_get_data(G_OBJECT(btn), "list_disc");

    if (list_devices && list_disc) {
        BluetoothScanData *data = g_new0(BluetoothScanData, 1);
        data->list_devices = list_devices;
        data->list_disc = list_disc;
        g_object_add_weak_pointer(G_OBJECT(list_devices), (gpointer *)&data->list_devices);
        g_object_add_weak_pointer(G_OBJECT(list_disc), (gpointer *)&data->list_disc);
        g_timeout_add(1500, on_bt_scan_refresh_timeout, data);
    }
}


/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageBluetooth {
    GtkBox parent_instance;

    GtkWidget *sw_bt;
    GtkWidget *list_devices;
    GtkWidget *btn_scan;
    GtkWidget *list_disc;
} SettingsPageBluetooth;

typedef struct _SettingsPageBluetoothClass {
    GtkBoxClass parent_class;
} SettingsPageBluetoothClass;

G_DEFINE_TYPE(SettingsPageBluetooth, settings_page_bluetooth, GTK_TYPE_BOX)

static void
settings_page_bluetooth_init(SettingsPageBluetooth *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_bluetooth_class_init(SettingsPageBluetoothClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_bluetooth.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageBluetooth, sw_bt);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageBluetooth, list_devices);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageBluetooth, btn_scan);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageBluetooth, list_disc);
}

GtkWidget *
build_page_bluetooth(void)
{
    SettingsPageBluetooth *page = g_object_new(settings_page_bluetooth_get_type(), NULL);

    g_signal_connect(page->sw_bt, "state-set", G_CALLBACK(on_bt_power_switch_toggled), NULL);

    populate_real_bluetooth_devices(page->list_devices, page->list_disc);

    g_object_set_data(G_OBJECT(page->btn_scan), "list_devices", page->list_devices);
    g_object_set_data(G_OBJECT(page->btn_scan), "list_disc", page->list_disc);

    g_signal_connect(page->btn_scan, "clicked", G_CALLBACK(on_bt_scan_clicked), NULL);

    return create_settings_page_card("BLUEZ 5 BLUETOOTH ADAPTER & DEVICES", "[ LIVE D-BUS ]", GTK_WIDGET(page), NULL);
}
