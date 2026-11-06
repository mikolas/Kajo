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

/* Security class of a Wi-Fi access point, derived from NetworkManager's
 * Flags / WpaFlags / RsnFlags. */
typedef enum {
    SHELL_AP_SEC_OPEN = 0,
    SHELL_AP_SEC_PSK,          /* WPA/WPA2-Personal (also WPA2/WPA3 transitional) */
    SHELL_AP_SEC_SAE,          /* WPA3-Personal only */
    SHELL_AP_SEC_UNSUPPORTED   /* WEP or 802.1X enterprise: needs a profile made elsewhere */
} ShellApSecurity;

typedef struct {
    char *ssid;
    char *path;
    guint8 strength;
    gboolean active;
    ShellApSecurity security;
} ShellAccessPoint;

void shell_access_point_free(ShellAccessPoint *ap);

/* Result callback for shell_dbus_nm_connect_ap_async(). `error_message` is NULL on success. */
typedef void (*ShellNmConnectDone)(const char *error_message, gpointer user_data);

/* TRUE if NetworkManager already has a saved Wi-Fi profile for `ssid`. */
gboolean shell_dbus_nm_has_saved_connection(const char *ssid);

/* Connects to an access point: activates the saved profile for `ssid` when one
 * exists, otherwise creates one (open network, or WPA-PSK/SAE with
 * `passphrase`) via AddAndActivateConnection. The final D-Bus call is
 * asynchronous; `done` is always invoked exactly once (possibly synchronously
 * with an error). */
void shell_dbus_nm_connect_ap_async(const char *ap_path,
                                    const char *ssid,
                                    ShellApSecurity security,
                                    const char *passphrase,
                                    ShellNmConnectDone done,
                                    gpointer user_data);

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
gboolean shell_dbus_bluez_get_powered(void);
gboolean shell_dbus_bluez_device_action(const char *dev_path, const char *action);
void shell_dbus_bluez_device_action_async(const char *dev_path,
                                          const char *action,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
GPtrArray *shell_dbus_bluez_get_devices(void);

/* NetworkManager Helpers */
void shell_dbus_nm_set_wireless_enabled(gboolean enabled);
gboolean shell_dbus_nm_get_wireless_enabled(void);
GPtrArray *shell_dbus_nm_get_access_points(void);

G_END_DECLS

#endif /* SHELL_DBUS_HELPERS_H */
