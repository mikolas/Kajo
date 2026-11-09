#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gio.h>
#include <string.h>

/* ─── Programmatic Bento Page Subclass: 03 / 12 Bluetooth ─── */

typedef struct _SettingsPageBluetooth {
    GtkBox parent_instance;

    GtkWidget *card_adapter;
    GtkWidget *sw_bt;
    GtkWidget *list_devices;

    GtkWidget *card_nearby;
    GtkWidget *btn_scan;
    GtkWidget *list_disc;

    GDBusConnection *sys_bus;
    GCancellable    *cancellable;
    gchar           *adapter_path;
    gchar           *adapter_short;
    gboolean         adapter_powered;
    gboolean         syncing_ui;
    guint            refresh_timer_id;
} SettingsPageBluetooth;

typedef struct _SettingsPageBluetoothClass {
    GtkBoxClass parent_class;
} SettingsPageBluetoothClass;

G_DEFINE_TYPE(SettingsPageBluetooth, settings_page_bluetooth, GTK_TYPE_BOX)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SettingsPageBluetooth, g_object_unref)

static void refresh_bluetooth_state(SettingsPageBluetooth *self);
static void schedule_bt_refresh(SettingsPageBluetooth *self, guint delay_ms);

static void
clear_bt_box(GtkWidget *box)
{
    if (!box)
        return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), child);
}

static const char *
glyph_for_bluez_icon(const char *icon)
{
    if (!icon)
        return "ᛒ";
    if (strstr(icon, "headphone") || strstr(icon, "headset") || strstr(icon, "audio"))
        return "🎧";
    if (strstr(icon, "mouse"))
        return "🖱";
    if (strstr(icon, "keyboard"))
        return "⌨";
    if (strstr(icon, "gaming") || strstr(icon, "joypad") || strstr(icon, "controller"))
        return "🎮";
    if (strstr(icon, "phone"))
        return "📱";
    return "ᛒ";
}

static void
update_bluetooth_telemetry(SettingsPageBluetooth *self,
                           guint                  paired_count,
                           const char            *connected_name)
{
    gboolean has_adapter = (self->adapter_path != NULL);
    const char *adap = (self->adapter_short && self->adapter_short[0])
                           ? self->adapter_short
                           : "NONE";

    if (has_adapter) {
        g_autofree gchar *card_badge = g_strdup_printf(
            "%s • %s", adap, self->adapter_powered ? "ON" : "OFF");
        section_card_set_badge(self->card_adapter, card_badge,
                               self->adapter_powered ? "accent" : NULL);
    } else {
        section_card_set_badge(self->card_adapter, "NO ADAPTER", "warn");
    }

    g_autofree gchar *paired_str = g_strdup_printf(
        "%u %s", paired_count, paired_count == 1 ? "DEVICE" : "DEVICES");

    settings_page_set_counter(GTK_WIDGET(self), 0, "ADAPTER",
                              has_adapter ? adap : "NONE",
                              has_adapter ? NULL : "warn");
    settings_page_set_counter(GTK_WIDGET(self), 1, "POWER",
                              self->adapter_powered ? "ENABLED" : "DISABLED",
                              self->adapter_powered ? "success" : NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "PAIRED", paired_str, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "CONNECTED",
                              (connected_name && connected_name[0]) ? connected_name : "NONE",
                              (connected_name && connected_name[0]) ? "accent" : NULL);

    settings_page_set_nav_badge(
        GTK_WIDGET(self), "bluetooth",
        has_adapter ? (self->adapter_powered ? "ON" : "OFF") : "N/A",
        (has_adapter && self->adapter_powered) ? "accent" : NULL);
}

static void
on_bt_async_op_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    (void)ret;

    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autoptr(SettingsPageBluetooth) self = g_weak_ref_get(ref);
        if (self) {
            refresh_bluetooth_state(self);
            schedule_bt_refresh(self, 1200);
        }
    }
    g_weak_ref_clear(ref);
    g_free(ref);
}

static void
on_bt_power_switch_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageBluetooth *self = user_data;
    if (self->syncing_ui || !self->sys_bus || !self->adapter_path)
        return;

    gboolean state = gtk_switch_get_active(GTK_SWITCH(obj));
    self->adapter_powered = state;

    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    g_dbus_connection_call(
        self->sys_bus,
        "org.bluez",
        self->adapter_path,
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", "org.bluez.Adapter1",
                      "Powered", g_variant_new_boolean(state)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
        self->cancellable, on_bt_async_op_done, ref);
}

static void
invoke_device_method(GtkButton *btn, SettingsPageBluetooth *self, const char *method)
{
    const char *dev_path = g_object_get_data(G_OBJECT(btn), "dev-path");
    if (!dev_path || !self || !self->sys_bus)
        return;

    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);

    if (g_strcmp0(method, "RemoveDevice") == 0) {
        g_autofree gchar *adap_fallback = NULL;
        const char *adap_path = self->adapter_path;
        if (!adap_path) {
            const char *dev_sub = strstr(dev_path, "/dev_");
            if (dev_sub) {
                adap_fallback = g_strndup(dev_path, (gsize)(dev_sub - dev_path));
                adap_path = adap_fallback;
            }
        }
        if (!adap_path) {
            g_weak_ref_clear(ref);
            g_free(ref);
            return;
        }
        g_dbus_connection_call(
            self->sys_bus, "org.bluez", adap_path,
            "org.bluez.Adapter1", "RemoveDevice",
            g_variant_new("(o)", dev_path),
            NULL, G_DBUS_CALL_FLAGS_NONE, 15000,
            self->cancellable, on_bt_async_op_done, ref);
        return;
    }

    g_dbus_connection_call(
        self->sys_bus, "org.bluez", dev_path,
        "org.bluez.Device1", method,
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 15000,
        self->cancellable, on_bt_async_op_done, ref);
}

static void
on_bt_device_connect_clicked(GtkButton *btn, gpointer user_data)
{
    invoke_device_method(btn, (SettingsPageBluetooth *)user_data, "Connect");
}

static void
on_bt_device_disconnect_clicked(GtkButton *btn, gpointer user_data)
{
    invoke_device_method(btn, (SettingsPageBluetooth *)user_data, "Disconnect");
}

static void
on_bt_device_forget_clicked(GtkButton *btn, gpointer user_data)
{
    invoke_device_method(btn, (SettingsPageBluetooth *)user_data, "RemoveDevice");
}

static void
on_bt_device_pair_clicked(GtkButton *btn, gpointer user_data)
{
    invoke_device_method(btn, (SettingsPageBluetooth *)user_data, "Connect");
}

static void
trigger_bt_discovery(SettingsPageBluetooth *self)
{
    if (!self || !self->sys_bus || !self->adapter_path)
        return;

    settings_page_set_live_status(GTK_WIDGET(self), "🔍 DISCOVERING",
                                  "Scanning nearby BLE & Classic Bluetooth peripherals…");
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    g_dbus_connection_call(
        self->sys_bus, "org.bluez", self->adapter_path,
        "org.bluez.Adapter1", "StartDiscovery",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 8000,
        self->cancellable, on_bt_async_op_done, ref);
}

static void
on_bt_scan_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    trigger_bt_discovery((SettingsPageBluetooth *)user_data);
}

static void
on_bt_secondary_scan(gpointer user_data)
{
    trigger_bt_discovery((SettingsPageBluetooth *)user_data);
}

static void
append_bluez_device_row(SettingsPageBluetooth *self,
                        const char            *obj_path,
                        GVariant              *dev_props,
                        guint                 *paired_count,
                        guint                 *disc_count,
                        gchar                **first_connected_name)
{
    g_autoptr(GVariant) v_name = g_variant_lookup_value(dev_props, "Name", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) v_alias = g_variant_lookup_value(dev_props, "Alias", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) v_icon = g_variant_lookup_value(dev_props, "Icon", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) v_addr = g_variant_lookup_value(dev_props, "Address", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) v_paired = g_variant_lookup_value(dev_props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    g_autoptr(GVariant) v_conn = g_variant_lookup_value(dev_props, "Connected", G_VARIANT_TYPE_BOOLEAN);

    const char *name_s = v_name ? g_variant_get_string(v_name, NULL) : NULL;
    const char *alias_s = v_alias ? g_variant_get_string(v_alias, NULL) : NULL;
    const char *dev_name = (name_s && name_s[0] != '\0') ? name_s : alias_s;
    if (!dev_name || dev_name[0] == '\0' || is_mac_address_string(dev_name))
        return;

    gboolean paired = v_paired ? g_variant_get_boolean(v_paired) : FALSE;
    gboolean connected = v_conn ? g_variant_get_boolean(v_conn) : FALSE;
    const char *glyph = glyph_for_bluez_icon(v_icon ? g_variant_get_string(v_icon, NULL) : NULL);

    g_autofree gchar *mac_fallback = NULL;
    const char *addr_s = v_addr ? g_variant_get_string(v_addr, NULL) : NULL;
    if (!addr_s || addr_s[0] == '\0') {
        mac_fallback = format_mac_address_from_bluez_path(obj_path);
        addr_s = mac_fallback ? mac_fallback : obj_path;
    }

    if (paired) {
        (*paired_count)++;
        if (connected && !*first_connected_name)
            *first_connected_name = g_strdup(dev_name);

        GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(btn_box, GTK_ALIGN_CENTER);

        GtkWidget *btn_conn = create_action_button(
            connected ? "DISCONNECT" : "CONNECT", "dev-path", obj_path,
            G_CALLBACK(connected ? on_bt_device_disconnect_clicked : on_bt_device_connect_clicked),
            self);
        gtk_widget_add_css_class(btn_conn, "files-nav-btn");

        GtkWidget *btn_forget = create_action_button(
            "FORGET", "dev-path", obj_path,
            G_CALLBACK(on_bt_device_forget_clicked), self);
        gtk_widget_add_css_class(btn_forget, "files-nav-btn");
        gtk_widget_add_css_class(btn_forget, "action-btn-danger");

        gtk_box_append(GTK_BOX(btn_box), btn_conn);
        gtk_box_append(GTK_BOX(btn_box), btn_forget);

        g_autofree gchar *sub = g_strdup_printf(
            "%s • %s", connected ? "Connected" : "Paired (Offline)", addr_s);
        GtkWidget *row = create_boxed_row(glyph, dev_name, sub, btn_box);
        if (connected)
            gtk_widget_add_css_class(row, "active");

        gtk_box_append(GTK_BOX(self->list_devices), row);
    } else {
        (*disc_count)++;
        GtkWidget *btn_pair = create_action_button(
            "PAIR DEVICE", "dev-path", obj_path,
            G_CALLBACK(on_bt_device_pair_clicked), self);
        gtk_widget_add_css_class(btn_pair, "files-nav-btn");
        gtk_widget_add_css_class(btn_pair, "action-btn-primary");

        g_autofree gchar *sub = g_strdup_printf("Discovered Nearby • %s", addr_s);
        gtk_box_append(GTK_BOX(self->list_disc),
                       create_boxed_row(glyph, dev_name, sub, btn_pair));
    }
}

static void
on_managed_objects_ready(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_weak_ref_clear(ref);
        g_free(ref);
        return;
    }

    g_autoptr(SettingsPageBluetooth) self = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self)
        return;

    clear_bt_box(self->list_devices);
    clear_bt_box(self->list_disc);
    g_clear_pointer(&self->adapter_path, g_free);
    g_clear_pointer(&self->adapter_short, g_free);
    self->adapter_powered = FALSE;

    guint paired_count = 0;
    guint disc_count = 0;
    g_autofree gchar *connected_name = NULL;

    if (ret) {
        g_autoptr(GVariant) objects = g_variant_get_child_value(ret, 0);
        GVariantIter obj_iter;
        g_variant_iter_init(&obj_iter, objects);
        const gchar *obj_path = NULL;
        GVariant *ifaces = NULL;

        while (g_variant_iter_next(&obj_iter, "{&o@a{sa{sv}}}", &obj_path, &ifaces)) {
            g_autoptr(GVariant) adap_props = g_variant_lookup_value(
                ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
            if (adap_props && !self->adapter_path) {
                self->adapter_path = g_strdup(obj_path);
                const char *slash = strrchr(obj_path, '/');
                self->adapter_short = g_strdup((slash && slash[1]) ? slash + 1 : obj_path);
                g_autoptr(GVariant) v_pow = g_variant_lookup_value(
                    adap_props, "Powered", G_VARIANT_TYPE_BOOLEAN);
                self->adapter_powered = v_pow ? g_variant_get_boolean(v_pow) : FALSE;
            }

            g_autoptr(GVariant) dev_props = g_variant_lookup_value(
                ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
            if (dev_props) {
                append_bluez_device_row(self, obj_path, dev_props,
                                        &paired_count, &disc_count, &connected_name);
            }
            g_variant_unref(ifaces);
        }
    }

    self->syncing_ui = TRUE;
    gtk_widget_set_sensitive(self->sw_bt, self->adapter_path != NULL);
    gtk_widget_set_sensitive(self->btn_scan, self->adapter_path != NULL && self->adapter_powered);
    gtk_switch_set_active(GTK_SWITCH(self->sw_bt), self->adapter_powered);
    self->syncing_ui = FALSE;

    if (paired_count == 0) {
        gtk_box_append(GTK_BOX(self->list_devices),
                       create_empty_state_label(
                           self->adapter_path
                               ? "No paired Bluetooth devices found."
                               : "No BlueZ Bluetooth adapter detected on system."));
    }
    if (disc_count == 0) {
        gtk_box_append(GTK_BOX(self->list_disc),
                       create_empty_state_label(
                           self->adapter_powered
                               ? "No nearby devices discovered. Click '🔍 SCAN NEARBY'."
                               : "Enable Bluetooth radio power to scan nearby devices."));
    }

    update_bluetooth_telemetry(self, paired_count, connected_name);
    settings_page_set_live_status(GTK_WIDGET(self), "✓ LIVE D-BUS",
                                  "org.bluez • Pairing & power apply immediately");
}

static void
refresh_bluetooth_state(SettingsPageBluetooth *self)
{
    if (!self || !self->sys_bus)
        return;

    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    g_dbus_connection_call(
        self->sys_bus,
        "org.bluez",
        "/",
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        self->cancellable,
        on_managed_objects_ready,
        ref);
}

static gboolean
on_bt_refresh_timeout(gpointer user_data)
{
    SettingsPageBluetooth *self = user_data;
    self->refresh_timer_id = 0;
    refresh_bluetooth_state(self);
    return G_SOURCE_REMOVE;
}

static void
schedule_bt_refresh(SettingsPageBluetooth *self, guint delay_ms)
{
    if (!self)
        return;
    if (self->refresh_timer_id != 0)
        g_source_remove(self->refresh_timer_id);
    self->refresh_timer_id = g_timeout_add(delay_ms, on_bt_refresh_timeout, self);
}

static void
on_bt_sys_bus_ready(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    GDBusConnection *bus = g_bus_get_finish(res, &err);

    if (!bus || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_object(&bus);
        g_weak_ref_clear(ref);
        g_free(ref);
        return;
    }

    g_autoptr(SettingsPageBluetooth) self = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self) {
        g_object_unref(bus);
        return;
    }

    self->sys_bus = bus;
    refresh_bluetooth_state(self);
}

static void
settings_page_bluetooth_dispose(GObject *object)
{
    SettingsPageBluetooth *self = (SettingsPageBluetooth *)object;

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    if (self->refresh_timer_id != 0) {
        g_source_remove(self->refresh_timer_id);
        self->refresh_timer_id = 0;
    }

    g_clear_object(&self->sys_bus);
    g_clear_pointer(&self->adapter_path, g_free);
    g_clear_pointer(&self->adapter_short, g_free);

    G_OBJECT_CLASS(settings_page_bluetooth_parent_class)->dispose(object);
}

static void
settings_page_bluetooth_init(SettingsPageBluetooth *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));

    self->cancellable = g_cancellable_new();
}

static void
settings_page_bluetooth_class_init(SettingsPageBluetoothClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_bluetooth_dispose;
}

GtkWidget *
build_page_bluetooth(void)
{
    SettingsPageBluetooth *self = g_object_new(settings_page_bluetooth_get_type(), NULL);

    /* ─── Left Card: BlueZ 5 Adapter & Paired Devices ─── */
    self->card_adapter = create_section_card(
        SETTINGS_CARD_LEFT, "1. BLUEZ 5 ADAPTER & PAIRED DEVICES",
        "QUERYING", NULL, NULL);

    self->sw_bt = gtk_switch_new();
    g_signal_connect(self->sw_bt, "notify::active", G_CALLBACK(on_bt_power_switch_changed), self);
    gtk_box_append(GTK_BOX(self->card_adapter),
                   create_boxed_row("ᛒ", "Bluetooth Radio Power",
                                    "org.bluez.Adapter1 Powered state",
                                    self->sw_bt));

    self->list_devices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(self->list_devices),
                   create_empty_state_label("Querying paired Bluetooth peripherals…"));
    gtk_box_append(GTK_BOX(self->card_adapter), self->list_devices);

    /* ─── Right Card: Nearby Discoverable Devices ─── */
    self->btn_scan = gtk_button_new_with_label("🔍 SCAN NEARBY");
    gtk_widget_add_css_class(self->btn_scan, "files-nav-btn");
    gtk_widget_set_valign(self->btn_scan, GTK_ALIGN_CENTER);
    g_signal_connect(self->btn_scan, "clicked", G_CALLBACK(on_bt_scan_clicked), self);

    self->card_nearby = create_section_card(
        SETTINGS_CARD_RIGHT, "2. NEARBY DISCOVERABLE DEVICES",
        NULL, NULL, self->btn_scan);
    gtk_widget_set_vexpand(self->card_nearby, TRUE);

    self->list_disc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(self->list_disc),
                   create_empty_state_label("Click '🔍 SCAN NEARBY' to discover peripherals."));
    gtk_box_append(GTK_BOX(self->card_nearby), self->list_disc);

    settings_bento_add_pair(GTK_WIDGET(self), self->card_adapter, self->card_nearby);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "🔍 SCAN NEARBY",
        on_bt_secondary_scan,
        NULL,
        NULL,
        self);
    settings_page_set_live_status(GTK_WIDGET(self), "✓ LIVE D-BUS",
                                  "org.bluez • Pairing & power apply immediately");

    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    g_bus_get(G_BUS_TYPE_SYSTEM, self->cancellable, on_bt_sys_bus_ready, ref);

    return GTK_WIDGET(self);
}
