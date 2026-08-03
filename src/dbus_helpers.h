#ifndef SHELL_DBUS_HELPERS_H
#define SHELL_DBUS_HELPERS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
    char *name;
    char *path;
    char *icon;
    char *address;
    gboolean paired;
    gboolean connected;
} ShellBluetoothDevice;

void shell_bluetooth_device_free(ShellBluetoothDevice *dev);

typedef struct {
    char *ssid;
    char *path;
    guint8 strength;
    gboolean active;
} ShellAccessPoint;

void shell_access_point_free(ShellAccessPoint *ap);

/* Set a property synchronously on system or session D-Bus */
gboolean shell_dbus_set_property_sync(GBusType bus_type,
                                      const char *bus_name,
                                      const char *object_path,
                                      const char *interface_name,
                                      const char *property_name,
                                      GVariant *value);

/* Call a method synchronously on system or session D-Bus */
GVariant *shell_dbus_call_method_sync(GBusType bus_type,
                                      const char *bus_name,
                                      const char *object_path,
                                      const char *interface_name,
                                      const char *method_name,
                                      GVariant *parameters);

/* BlueZ Helpers */
void shell_dbus_bluez_set_powered(gboolean powered);
gboolean shell_dbus_bluez_device_action(const char *dev_path, const char *action);
GPtrArray *shell_dbus_bluez_get_devices(void);

/* NetworkManager Helpers */
void shell_dbus_nm_set_wireless_enabled(gboolean enabled);
GPtrArray *shell_dbus_nm_get_access_points(void);

G_END_DECLS

#endif /* SHELL_DBUS_HELPERS_H */
