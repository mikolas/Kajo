#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../dbus_helpers.h"
#include <string.h>

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageBluetooth {
    GtkBox parent_instance;

    GtkWidget *sw_bt;
    GtkWidget *list_devices;
    GtkWidget *btn_scan;
    GtkWidget *list_disc;

    guint refresh_timer_id;
} SettingsPageBluetooth;

typedef struct _SettingsPageBluetoothClass {
    GtkBoxClass parent_class;
} SettingsPageBluetoothClass;

G_DEFINE_TYPE(SettingsPageBluetooth, settings_page_bluetooth, GTK_TYPE_BOX)

static void populate_real_bluetooth_devices(SettingsPageBluetooth *self);
static void refresh_bt_lists(SettingsPageBluetooth *self);
static void schedule_bt_refresh(SettingsPageBluetooth *self);

static void
on_bt_power_switch_toggled(GtkSwitch *sw G_GNUC_UNUSED, gboolean state, gpointer user_data G_GNUC_UNUSED)
{
    shell_dbus_bluez_set_powered(state);
}

static void
on_bt_device_action_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *weak_ref = user_data;
    if (source_object && res) {
        GError *error = NULL;
        GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), res, &error);
        if (ret) g_variant_unref(ret);
        if (error) g_clear_error(&error);
    }

    if (weak_ref) {
        SettingsPageBluetooth *self = g_weak_ref_get(weak_ref);
        if (self) {
            refresh_bt_lists(self);
            schedule_bt_refresh(self);
            g_object_unref(self);
        }
        g_weak_ref_clear(weak_ref);
        g_free(weak_ref);
    }
}

static void
run_bt_device_action(GtkButton *btn, const char *action, SettingsPageBluetooth *self)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "dev-path");
    if (!path || !self) return;

    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);

    GWeakRef *weak_ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(weak_ref, self);
    shell_dbus_bluez_device_action_async(path, action, on_bt_device_action_done, weak_ref);
}

static void
on_bt_device_connect_clicked(GtkButton *btn, gpointer user_data)
{
    run_bt_device_action(btn, "Connect", (SettingsPageBluetooth *)user_data);
}

static void
on_bt_device_disconnect_clicked(GtkButton *btn, gpointer user_data)
{
    run_bt_device_action(btn, "Disconnect", (SettingsPageBluetooth *)user_data);
}

static void
on_bt_device_forget_clicked(GtkButton *btn, gpointer user_data)
{
    run_bt_device_action(btn, "RemoveDevice", (SettingsPageBluetooth *)user_data);
}

static void
on_bt_device_pair_clicked(GtkButton *btn, gpointer user_data)
{
    run_bt_device_action(btn, "Connect", (SettingsPageBluetooth *)user_data);
}

static void
populate_real_bluetooth_devices(SettingsPageBluetooth *self)
{
    if (!self || !self->list_devices || !self->list_disc) return;

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

        g_autofree gchar *mac_str = (dev->address && dev->address[0] != '\0')
                                        ? g_strdup(dev->address)
                                        : format_mac_address_from_bluez_path(dev->path);
        const char *addr_display = mac_str ? mac_str : dev->path;

        if (dev->paired) {
            paired_count++;
            GtkWidget *btn_conn = create_action_button(
                dev->connected ? "Disconnect" : "Connect",
                "dev-path", dev->path,
                G_CALLBACK(dev->connected ? on_bt_device_disconnect_clicked : on_bt_device_connect_clicked), self);

            GtkWidget *btn_forget = create_action_button(
                "Forget", "dev-path", dev->path,
                G_CALLBACK(on_bt_device_forget_clicked), self);

            GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(btn_box), btn_conn);
            gtk_box_append(GTK_BOX(btn_box), btn_forget);

            gchar *sub = g_strdup_printf("%s | Address: %s", dev->connected ? "Connected" : "Paired (Offline)", addr_display);
            GtkWidget *row = create_simple_item_row(icon_name, dev->name, sub, btn_box);
            g_free(sub);

            gtk_box_append(GTK_BOX(self->list_devices), row);
        } else {
            disc_count++;
            GtkWidget *btn_pair = create_action_button(
                "Pair Device", "dev-path", dev->path,
                G_CALLBACK(on_bt_device_pair_clicked), self);

            gchar *sub = g_strdup_printf("Discovered Nearby Device | %s", addr_display);
            GtkWidget *row = create_simple_item_row(icon_name, dev->name, sub, btn_pair);
            g_free(sub);

            gtk_box_append(GTK_BOX(self->list_disc), row);
        }
    }

    if (paired_count == 0) {
        gtk_box_append(GTK_BOX(self->list_devices), create_empty_state_label("No paired Bluetooth devices found on system."));
    }

    if (disc_count == 0) {
        gtk_box_append(GTK_BOX(self->list_disc), create_empty_state_label("No nearby Bluetooth devices discovered. Click '🔍 SCAN NEARBY DEVICES'."));
    }

    g_ptr_array_unref(devices);
}

static void
refresh_bt_lists(SettingsPageBluetooth *self)
{
    if (!self || !self->list_devices || !self->list_disc) return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->list_devices)) != NULL) {
        gtk_box_remove(GTK_BOX(self->list_devices), child);
    }
    while ((child = gtk_widget_get_first_child(self->list_disc)) != NULL) {
        gtk_box_remove(GTK_BOX(self->list_disc), child);
    }
    populate_real_bluetooth_devices(self);
}

static gboolean
on_bt_scan_refresh_timeout(gpointer user_data)
{
    SettingsPageBluetooth *self = user_data;
    self->refresh_timer_id = 0;
    refresh_bt_lists(self);
    return G_SOURCE_REMOVE;
}

static void
schedule_bt_refresh(SettingsPageBluetooth *self)
{
    if (!self) return;
    if (self->refresh_timer_id != 0) {
        g_source_remove(self->refresh_timer_id);
        self->refresh_timer_id = 0;
    }
    self->refresh_timer_id = g_timeout_add(1500, on_bt_scan_refresh_timeout, self);
}

static void
on_bt_scan_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageBluetooth *self = user_data;
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

    schedule_bt_refresh(self);
}

static void
settings_page_bluetooth_dispose(GObject *object)
{
    SettingsPageBluetooth *self = (SettingsPageBluetooth *)object;
    if (self->refresh_timer_id != 0) {
        g_source_remove(self->refresh_timer_id);
        self->refresh_timer_id = 0;
    }
    gtk_widget_dispose_template(GTK_WIDGET(object), settings_page_bluetooth_get_type());
    G_OBJECT_CLASS(settings_page_bluetooth_parent_class)->dispose(object);
}

static void
settings_page_bluetooth_init(SettingsPageBluetooth *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_bluetooth_class_init(SettingsPageBluetoothClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_page_bluetooth_dispose;

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

    gtk_switch_set_active(GTK_SWITCH(page->sw_bt), shell_dbus_bluez_get_powered());
    g_signal_connect(page->sw_bt, "state-set", G_CALLBACK(on_bt_power_switch_toggled), NULL);

    populate_real_bluetooth_devices(page);

    g_signal_connect(page->btn_scan, "clicked", G_CALLBACK(on_bt_scan_clicked), page);

    return create_settings_page_card("BLUEZ 5 BLUETOOTH ADAPTER & DEVICES", "[ LIVE D-BUS ]", GTK_WIDGET(page), NULL);
}
