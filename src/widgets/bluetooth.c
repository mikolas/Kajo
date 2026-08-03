#include "widget.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shell.h"
#include "../config.h"
#include "../icons.h"
#include "../autohide.h"
#include "../dbus_helpers.h"

#define BLUEZ_SERVICE           "org.bluez"
#define BLUEZ_ADAPTER_INTERFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_INTERFACE  "org.bluez.Device1"
#define BLUEZ_AGENT_INTERFACE   "org.bluez.Agent1"
#define BLUEZ_AGENT_MGR_IFACE   "org.bluez.AgentManager1"
#define AGENT_PATH              "/org/bluez/desktop_shell_agent"

extern const ShellWidgetClass bluetooth_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;

    /* Popover Controls */
    GtkWidget *power_switch;
    GtkWidget *scan_btn;
    GtkWidget *status_label;
    GtkWidget *paired_list_box;
    GtkWidget *discovered_list_box;
    GtkWidget *paired_section_label;
    GtkWidget *discovered_section_label;
    GtkWidget *devices_scroll;

    GDBusObjectManager *object_manager;
    GDBusProxy *adapter_proxy;
    GDBusConnection *bus;
    guint agent_reg_id;

    gboolean is_powered;
    gboolean is_discovering;
    gchar *connected_device_name;
    guint connected_count;
} BluetoothWidget;

static const char agent_introspection_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg direction='in' type='o' name='device'/>"
    "      <arg direction='out' type='s' name='pincode'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg direction='in' type='o' name='device'/>"
    "      <arg direction='in' type='s' name='pincode'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg direction='in' type='o' name='device'/>"
    "      <arg direction='out' type='u' name='passkey'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg direction='in' type='o' name='device'/>"
    "      <arg direction='in' type='u' name='passkey'/>"
    "      <arg direction='in' type='q' name='entered'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg direction='in' type='o' name='device'/>"
    "      <arg direction='in' type='u' name='passkey'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg direction='in' type='o' name='device'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg direction='in' type='o' name='device'/>"
    "      <arg direction='in' type='s' name='uuid'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

static void update_bluetooth_ui(BluetoothWidget *bt);
static void refresh_devices_list(BluetoothWidget *bt);
static void refresh_bluetooth_state(BluetoothWidget *bt);
static void register_agent(BluetoothWidget *bt);

static gboolean
is_mac_address_format(const char *str)
{
    if (!str) return FALSE;
    size_t len = strlen(str);
    if (len != 17) return FALSE;

    for (size_t i = 0; i < len; i++) {
        if ((i + 1) % 3 == 0) {
            if (str[i] != ':' && str[i] != '-') return FALSE;
        } else {
            if (!g_ascii_isxdigit(str[i])) return FALSE;
        }
    }
    return TRUE;
}

/* Agent D-Bus Method Call Handler */
static void
handle_agent_method_call(GDBusConnection *connection,
                        const gchar *sender,
                        const gchar *object_path,
                        const gchar *interface_name,
                        const gchar *method_name,
                        GVariant *parameters,
                        GDBusMethodInvocation *invocation,
                        gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path;
    (void)interface_name; (void)parameters; (void)user_data;

    if (g_strcmp0(method_name, "RequestConfirmation") == 0 ||
        g_strcmp0(method_name, "RequestAuthorization") == 0 ||
        g_strcmp0(method_name, "AuthorizeService") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_strcmp0(method_name, "RequestPinCode") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "0000"));
    } else if (g_strcmp0(method_name, "RequestPasskey") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 0));
    } else {
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable agent_vtable = {
    handle_agent_method_call, NULL, NULL, { 0 }
};

static void
register_agent(BluetoothWidget *bt)
{
    if (bt->bus == NULL) return;

    GError *error = NULL;
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(agent_introspection_xml, NULL);
    if (!node_info) return;

    bt->agent_reg_id = g_dbus_connection_register_object(
        bt->bus,
        AGENT_PATH,
        node_info->interfaces[0],
        &agent_vtable,
        bt,
        NULL,
        &error);

    g_dbus_node_info_unref(node_info);

    if (error) {
        g_clear_error(&error);
        return;
    }

    GDBusProxy *agent_mgr = g_dbus_proxy_new_sync(
        bt->bus,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        BLUEZ_SERVICE,
        "/org/bluez",
        BLUEZ_AGENT_MGR_IFACE,
        NULL, NULL);

    if (agent_mgr) {
        g_dbus_proxy_call(agent_mgr, "RegisterAgent",
                          g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        g_dbus_proxy_call(agent_mgr, "RequestDefaultAgent",
                          g_variant_new("(o)", AGENT_PATH),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        g_object_unref(agent_mgr);
    }
}

static void
on_power_switch_toggled(GtkSwitch *sw G_GNUC_UNUSED, gboolean state, gpointer user_data G_GNUC_UNUSED)
{
    shell_dbus_bluez_set_powered(state);
}

static void
on_scan_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    BluetoothWidget *bt = user_data;
    if (bt->adapter_proxy == NULL) return;

    if (!bt->is_discovering) {
        /* Set Discovery Filter to Active Dual-Mode (BR/EDR + LE) */
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Transport", g_variant_new_string("auto"));
        GVariant *filter = g_variant_builder_end(&b);

        g_dbus_proxy_call(bt->adapter_proxy, "SetDiscoveryFilter",
                          g_variant_new("(@a{sv})", filter),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

        g_dbus_proxy_call(bt->adapter_proxy, "StartDiscovery", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        bt->is_discovering = TRUE;
    } else {
        g_dbus_proxy_call(bt->adapter_proxy, "StopDiscovery", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        bt->is_discovering = FALSE;
    }
    update_bluetooth_ui(bt);
}

static void
on_connect_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "dev-path");
    BluetoothWidget *bt = g_object_get_data(G_OBJECT(btn), "bt-widget");
    if (path && bt && bt->bus) {
        g_dbus_connection_call(bt->bus, BLUEZ_SERVICE, path,
                               BLUEZ_DEVICE_INTERFACE, "Connect",
                               NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_disconnect_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "dev-path");
    BluetoothWidget *bt = g_object_get_data(G_OBJECT(btn), "bt-widget");
    if (path && bt && bt->bus) {
        g_dbus_connection_call(bt->bus, BLUEZ_SERVICE, path,
                               BLUEZ_DEVICE_INTERFACE, "Disconnect",
                               NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_pair_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "dev-path");
    BluetoothWidget *bt = g_object_get_data(G_OBJECT(btn), "bt-widget");
    if (path && bt && bt->bus) {
        g_dbus_connection_call(bt->bus, BLUEZ_SERVICE, path,
                               BLUEZ_DEVICE_INTERFACE, "Pair",
                               NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
clear_list_box(GtkWidget *list_box)
{
    if (list_box == NULL) return;
    GtkWidget *child = gtk_widget_get_first_child(list_box);
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(list_box), child);
        child = next;
    }
}

static void
refresh_devices_list(BluetoothWidget *bt)
{
    if (bt->object_manager == NULL || bt->paired_list_box == NULL) return;

    clear_list_box(bt->paired_list_box);
    clear_list_box(bt->discovered_list_box);

    g_free(bt->connected_device_name);
    bt->connected_device_name = NULL;
    bt->connected_count = 0;

    guint paired_count = 0;
    guint discovered_count = 0;

    if (!bt->is_powered) {
        gtk_widget_set_visible(bt->paired_section_label, FALSE);
        gtk_widget_set_visible(bt->paired_list_box, FALSE);
        gtk_widget_set_visible(bt->discovered_section_label, FALSE);
        gtk_widget_set_visible(bt->discovered_list_box, FALSE);
        return;
    }

    gtk_widget_set_visible(bt->paired_section_label, TRUE);
    gtk_widget_set_visible(bt->paired_list_box, TRUE);
    gtk_widget_set_visible(bt->discovered_section_label, TRUE);
    gtk_widget_set_visible(bt->discovered_list_box, TRUE);

    GPtrArray *devices = shell_dbus_bluez_get_devices();
    for (guint i = 0; i < devices->len; i++) {
        ShellBluetoothDevice *dev = g_ptr_array_index(devices, i);
        if (!dev->name || is_mac_address_format(dev->name)) continue;

        if (dev->connected) {
            bt->connected_count++;
            if (!bt->connected_device_name) {
                bt->connected_device_name = g_strdup(dev->name);
            }
        }

        if (dev->paired) {
            paired_count++;
            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_add_css_class(row_box, "shell-row-item");
            if (dev->connected) {
                gtk_widget_add_css_class(row_box, "connected");
            }

            GtkWidget *icon = gtk_image_new_from_icon_name(dev->connected ? "fl-bluetooth-symbolic" : "fl-bluetooth-off-symbolic");
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
            gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(row_box), icon);

            GtkWidget *lbl_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_hexpand(lbl_vbox, TRUE);

            GtkWidget *name_lbl = gtk_label_new(dev->name);
            gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_tooltip_text(name_lbl, dev->name);
            gtk_widget_set_hexpand(name_lbl, TRUE);
            gtk_box_append(GTK_BOX(lbl_vbox), name_lbl);

            GtkWidget *sub_lbl = gtk_label_new(dev->connected ? "Connected" : "Paired");
            gtk_label_set_xalign(GTK_LABEL(sub_lbl), 0.0);
            gtk_widget_add_css_class(sub_lbl, "shell-popover-subtitle");
            gtk_box_append(GTK_BOX(lbl_vbox), sub_lbl);

            gtk_box_append(GTK_BOX(row_box), lbl_vbox);

            GtkWidget *act_btn = gtk_button_new_with_label(dev->connected ? "Disconnect" : "Connect");
            gtk_widget_set_valign(act_btn, GTK_ALIGN_CENTER);
            if (dev->connected) {
                gtk_widget_add_css_class(act_btn, "destructive-action");
            }
            g_object_set_data_full(G_OBJECT(act_btn), "dev-path", g_strdup(dev->path), g_free);
            g_object_set_data(G_OBJECT(act_btn), "bt-widget", bt);
            g_signal_connect(act_btn, "clicked",
                             G_CALLBACK(dev->connected ? on_disconnect_clicked : on_connect_clicked), NULL);
            gtk_box_append(GTK_BOX(row_box), act_btn);
            gtk_list_box_append(GTK_LIST_BOX(bt->paired_list_box), row_box);
        } else {
            discovered_count++;
            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_add_css_class(row_box, "shell-device-row");

            GtkWidget *icon = gtk_image_new_from_icon_name("fl-bluetooth-off-symbolic");
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
            gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(row_box), icon);

            GtkWidget *lbl_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_hexpand(lbl_vbox, TRUE);

            GtkWidget *name_lbl = gtk_label_new(dev->name);
            gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_tooltip_text(name_lbl, dev->name);
            gtk_widget_set_hexpand(name_lbl, TRUE);
            gtk_box_append(GTK_BOX(lbl_vbox), name_lbl);

            GtkWidget *sub_lbl = gtk_label_new("Discovered");
            gtk_label_set_xalign(GTK_LABEL(sub_lbl), 0.0);
            gtk_widget_add_css_class(sub_lbl, "shell-popover-subtitle");
            gtk_box_append(GTK_BOX(lbl_vbox), sub_lbl);

            gtk_box_append(GTK_BOX(row_box), lbl_vbox);

            GtkWidget *act_btn = gtk_button_new_with_label("Pair");
            gtk_widget_set_valign(act_btn, GTK_ALIGN_CENTER);
            g_object_set_data_full(G_OBJECT(act_btn), "dev-path", g_strdup(dev->path), g_free);
            g_object_set_data(G_OBJECT(act_btn), "bt-widget", bt);
            g_signal_connect(act_btn, "clicked", G_CALLBACK(on_pair_clicked), NULL);
            gtk_box_append(GTK_BOX(row_box), act_btn);
            gtk_list_box_append(GTK_LIST_BOX(bt->discovered_list_box), row_box);
        }
    }
    g_ptr_array_unref(devices);

    /* Display Placeholders when empty */
    if (paired_count == 0) {
        GtkWidget *ph = gtk_label_new("No paired devices");
        gtk_widget_add_css_class(ph, "shell-device-placeholder");
        gtk_label_set_xalign(GTK_LABEL(ph), 0.0);
        gtk_list_box_append(GTK_LIST_BOX(bt->paired_list_box), ph);
    }

    if (discovered_count == 0) {
        GtkWidget *ph = gtk_label_new(bt->is_discovering ? "Searching for devices..." : "Click 'Scan ↻' to search");
        gtk_widget_add_css_class(ph, "shell-device-placeholder");
        gtk_label_set_xalign(GTK_LABEL(ph), 0.0);
        gtk_list_box_append(GTK_LIST_BOX(bt->discovered_list_box), ph);
    }
}

static gboolean
is_popover_open(BluetoothWidget *bt)
{
    if (bt == NULL || bt->popover == NULL) return FALSE;
    return gtk_widget_get_mapped(bt->popover) || gtk_widget_get_visible(bt->popover);
}

static void
on_object_added_or_removed(GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    (void)manager; (void)object;
    BluetoothWidget *bt = user_data;
    update_bluetooth_ui(bt);
    if (is_popover_open(bt)) {
        refresh_devices_list(bt);
    }
}

static void
on_interface_proxy_properties_changed(GDBusObjectManager *manager,
                                       GDBusObjectProxy *object_proxy,
                                       GDBusProxy *interface_proxy,
                                       GVariant *changed_properties,
                                       const gchar *const *invalidated_properties,
                                       gpointer user_data)
{
    (void)manager; (void)object_proxy; (void)interface_proxy;
    (void)changed_properties; (void)invalidated_properties;
    BluetoothWidget *bt = user_data;
    update_bluetooth_ui(bt);
    if (is_popover_open(bt)) {
        refresh_devices_list(bt);
    }
}

static void
on_popover_visibility_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    GtkPopover *popover = GTK_POPOVER(gobject);
    BluetoothWidget *bt = user_data;

    if (gtk_widget_get_mapped(GTK_WIDGET(popover)) || gtk_widget_get_visible(GTK_WIDGET(popover))) {
        /* Popover opened: refresh state and devices list once */
        refresh_bluetooth_state(bt);
    } else {
        /* Popover closed: stop active discovery if running */
        if (bt->is_discovering && bt->adapter_proxy != NULL) {
            g_dbus_proxy_call(bt->adapter_proxy, "StopDiscovery", NULL,
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
            bt->is_discovering = FALSE;
        }
        update_bluetooth_ui(bt);
    }
}

static void
refresh_bluetooth_state(BluetoothWidget *bt)
{
    if (bt->bus == NULL) {
        GError *error = NULL;
        bt->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (error) {
            g_clear_error(&error);
            bt->is_powered = FALSE;
            update_bluetooth_ui(bt);
            return;
        }
        register_agent(bt);
    }

    if (bt->object_manager == NULL) {
        GError *error = NULL;
        bt->object_manager = g_dbus_object_manager_client_new_sync(
            bt->bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
            BLUEZ_SERVICE, "/", NULL, NULL, NULL, NULL, &error);
        if (bt->object_manager) {
            g_signal_connect(bt->object_manager, "object-added", G_CALLBACK(on_object_added_or_removed), bt);
            g_signal_connect(bt->object_manager, "object-removed", G_CALLBACK(on_object_added_or_removed), bt);
            g_signal_connect(bt->object_manager, "interface-proxy-properties-changed", G_CALLBACK(on_interface_proxy_properties_changed), bt);
        } else if (error) {
            g_clear_error(&error);
        }
    }

    if (bt->adapter_proxy == NULL) {
        gchar *adapter_path = g_strdup("/org/bluez/hci0");
        if (bt->object_manager) {
            GList *objects = g_dbus_object_manager_get_objects(bt->object_manager);
            for (GList *l = objects; l != NULL; l = l->next) {
                GDBusObject *obj = G_DBUS_OBJECT(l->data);
                GDBusInterface *iface = g_dbus_object_get_interface(obj, BLUEZ_ADAPTER_INTERFACE);
                if (iface) {
                    g_free(adapter_path);
                    adapter_path = g_strdup(g_dbus_object_get_object_path(obj));
                    g_object_unref(iface);
                    break;
                }
            }
            g_list_free_full(objects, g_object_unref);
        }

        GError *error = NULL;
        bt->adapter_proxy = g_dbus_proxy_new_sync(
            bt->bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
            BLUEZ_SERVICE, adapter_path, BLUEZ_ADAPTER_INTERFACE, NULL, &error);
        g_free(adapter_path);

        if (error) {
            g_clear_error(&error);
            bt->is_powered = FALSE;
            update_bluetooth_ui(bt);
            return;
        }
    }

    GVariant *powered_var = g_dbus_proxy_get_cached_property(bt->adapter_proxy, "Powered");
    if (powered_var != NULL) {
        bt->is_powered = g_variant_get_boolean(powered_var);
        g_variant_unref(powered_var);
    } else {
        bt->is_powered = FALSE;
    }

    GVariant *disc_var = g_dbus_proxy_get_cached_property(bt->adapter_proxy, "Discovering");
    if (disc_var != NULL) {
        bt->is_discovering = g_variant_get_boolean(disc_var);
        g_variant_unref(disc_var);
    }

    if (is_popover_open(bt)) {
        refresh_devices_list(bt);
    }
    update_bluetooth_ui(bt);
}

static void
update_bluetooth_ui(BluetoothWidget *bt)
{
    const gchar *icon_name;
    gchar label_text[128];

    if (!bt->is_powered) {
        icon_name = "tb-bluetooth-off-symbolic";
        snprintf(label_text, sizeof(label_text), "Off");
    } else if (bt->connected_count > 0 && bt->connected_device_name != NULL) {
        icon_name = "tb-bluetooth-symbolic";
        snprintf(label_text, sizeof(label_text), "%s", bt->connected_device_name);
    } else {
        icon_name = "tb-bluetooth-symbolic";
        snprintf(label_text, sizeof(label_text), "On");
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(bt->icon_img), icon_name);
    gtk_label_set_text(GTK_LABEL(bt->label), label_text);
    shell_widget_apply_mode_visibility(bt->base.mode, bt->icon_img, bt->label);

    if (bt->power_switch != NULL) {
        g_signal_handlers_block_by_func(bt->power_switch, on_power_switch_toggled, bt);
        gtk_switch_set_active(GTK_SWITCH(bt->power_switch), bt->is_powered);
        g_signal_handlers_unblock_by_func(bt->power_switch, on_power_switch_toggled, bt);
    }

    if (bt->status_label != NULL) {
        if (!bt->is_powered) {
            gtk_label_set_text(GTK_LABEL(bt->status_label), "[ OFF ]");
            gtk_widget_add_css_class(bt->status_label, "urgent");
        } else if (bt->is_discovering) {
            gtk_label_set_text(GTK_LABEL(bt->status_label), "[ SCANNING... ]");
            gtk_widget_remove_css_class(bt->status_label, "urgent");
        } else {
            gchar *b = (bt->connected_count > 0) ? g_strdup_printf("[ %u CONNECTED ]", bt->connected_count) : g_strdup("[ READY ]");
            gtk_label_set_text(GTK_LABEL(bt->status_label), b);
            gtk_widget_remove_css_class(bt->status_label, "urgent");
            g_free(b);
        }
    }

    if (bt->scan_btn != NULL) {
        gtk_button_set_label(GTK_BUTTON(bt->scan_btn), bt->is_discovering ? "STOP SCAN" : "RE-SCAN");
    }
}

static void
on_launch_bluetooth_settings_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    if (!g_spawn_command_line_async("./builddir/kajo-settings --page bluetooth", NULL)) {
        g_spawn_command_line_async("kajo-settings --page bluetooth", NULL);
    }
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellBluetoothPopover {
    GtkPopover parent_instance;

    GtkWidget *status_label;
    GtkWidget *power_switch;
    GtkWidget *devices_scroll;
    GtkWidget *paired_section_label;
    GtkWidget *paired_list_box;
    GtkWidget *discovered_section_label;
    GtkWidget *discovered_list_box;
    GtkWidget *scan_btn;
    GtkWidget *settings_btn;
} ShellBluetoothPopover;

typedef struct _ShellBluetoothPopoverClass {
    GtkPopoverClass parent_class;
} ShellBluetoothPopoverClass;

G_DEFINE_TYPE(ShellBluetoothPopover, shell_bluetooth_popover, GTK_TYPE_POPOVER)

static void
shell_bluetooth_popover_init(ShellBluetoothPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_bluetooth_popover_class_init(ShellBluetoothPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/bluetooth_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, status_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, power_switch);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, devices_scroll);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, paired_section_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, paired_list_box);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, discovered_section_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, discovered_list_box);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, scan_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellBluetoothPopover, settings_btn);
}

static GtkWidget *
build_bluetooth_popover(BluetoothWidget *bt)
{
    ShellBluetoothPopover *popover = g_object_new(shell_bluetooth_popover_get_type(), NULL);
    g_signal_connect(popover, "notify::visible", G_CALLBACK(on_popover_visibility_changed), bt);

    bt->status_label = popover->status_label;
    bt->power_switch = popover->power_switch;
    bt->devices_scroll = popover->devices_scroll;
    bt->paired_section_label = popover->paired_section_label;
    bt->paired_list_box = popover->paired_list_box;
    bt->discovered_section_label = popover->discovered_section_label;
    bt->discovered_list_box = popover->discovered_list_box;
    bt->scan_btn = popover->scan_btn;

    g_signal_connect(bt->power_switch, "state-set",
                     G_CALLBACK(on_power_switch_toggled), bt);

    g_signal_connect(bt->scan_btn, "clicked",
                     G_CALLBACK(on_scan_clicked), bt);

    g_signal_connect(popover->settings_btn, "clicked",
                     G_CALLBACK(on_launch_bluetooth_settings_clicked), NULL);

    return GTK_WIDGET(popover);
}

static ShellWidget *
bluetooth_widget_create(ShellCompositor *compositor)
{
    BluetoothWidget *bt = g_new0(BluetoothWidget, 1);
    bt->base.klass = &bluetooth_widget_class;
    bt->compositor = compositor;
    bt->is_powered = TRUE;

    /* Build Panel Button */
    bt->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(bt->button), FALSE);
    gtk_widget_add_css_class(bt->button, "shell-widget");
    gtk_widget_add_css_class(bt->button, "shell-widget-bluetooth");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    bt->icon_img = gtk_image_new_from_icon_name("fl-bluetooth-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(bt->icon_img), shell_widget_get_icon_size((ShellWidget *)bt));
    bt->label = gtk_label_new("Bluetooth");

    gtk_box_append(GTK_BOX(hbox), bt->icon_img);
    gtk_box_append(GTK_BOX(hbox), bt->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(bt->button), hbox);

    /* Build Popover */
    bt->popover = build_bluetooth_popover(bt);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(bt->button), bt->popover);

    refresh_bluetooth_state(bt);

    return (ShellWidget *)bt;
}

static void
bluetooth_widget_destroy(ShellWidget *widget)
{
    BluetoothWidget *bt = (BluetoothWidget *)widget;
    if (bt == NULL) return;

    if (bt->agent_reg_id > 0 && bt->bus != NULL) {
        g_dbus_connection_unregister_object(bt->bus, bt->agent_reg_id);
    }
    if (bt->object_manager != NULL) {
        g_signal_handlers_disconnect_by_data(bt->object_manager, bt);
        g_object_unref(bt->object_manager);
        bt->object_manager = NULL;
    }
    if (bt->adapter_proxy != NULL) {
        g_object_unref(bt->adapter_proxy);
    }
    if (bt->bus != NULL) {
        g_object_unref(bt->bus);
    }

    g_free(bt->connected_device_name);
    g_free(bt);
}

static GtkWidget *
bluetooth_widget_get_widget(ShellWidget *widget)
{
    BluetoothWidget *bt = (BluetoothWidget *)widget;
    return bt->button;
}

static void
bluetooth_widget_enable(ShellWidget *widget)
{
    BluetoothWidget *bt = (BluetoothWidget *)widget;
    if (bt) {
        shell_widget_apply_mode_visibility(widget->mode, bt->icon_img, bt->label);
    }
}

static void
bluetooth_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass bluetooth_widget_class = {
    .id = "bluetooth",
    .name = "Bluetooth",
    .create = bluetooth_widget_create,
    .destroy = bluetooth_widget_destroy,
    .get_widget = bluetooth_widget_get_widget,
    .enable = bluetooth_widget_enable,
    .disable = bluetooth_widget_disable,
};
