#include "hw_caps.h"
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ShellHwCaps     *cached_caps = NULL;
static GDBusConnection *cached_system_bus = NULL;
static gchar           *cached_session_path = NULL;

static gchar *
read_trimmed_file(const gchar *path)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;
    g_strstrip(contents);
    return contents;
}

static gint
read_int_file(const gchar *path)
{
    g_autofree gchar *str = read_trimmed_file(path);
    if (!str || !*str)
        return -1;
    return atoi(str);
}

static void
probe_display_backlight(ShellHwCaps *caps)
{
    GDir *dir = g_dir_open("/sys/class/backlight", 0, NULL);
    if (!dir)
        return;

    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *dev_dir = g_build_filename("/sys/class/backlight", entry, NULL);
        g_autofree gchar *max_path = g_build_filename(dev_dir, "max_brightness", NULL);
        gint max_val = read_int_file(max_path);
        if (max_val > 0) {
            caps->has_display_backlight = TRUE;
            caps->display_backlight_dir = g_strdup(dev_dir);
            caps->display_max_brightness = max_val;
            break;
        }
    }
    g_dir_close(dir);
}

static void
probe_iio_sensors(ShellHwCaps *caps)
{
    GDir *dir = g_dir_open("/sys/bus/iio/devices", 0, NULL);
    if (!dir)
        return;

    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_prefix(entry, "iio:device"))
            continue;

        g_autofree gchar *dev_dir = g_build_filename("/sys/bus/iio/devices", entry, NULL);
        g_autofree gchar *name_path = g_build_filename(dev_dir, "name", NULL);
        g_autofree gchar *name = read_trimmed_file(name_path);
        if (!name)
            continue;

        if (g_strcmp0(name, "hinge") == 0) {
            g_autofree gchar *angl_path = g_build_filename(dev_dir, "in_angl0_raw", NULL);
            if (g_file_test(angl_path, G_FILE_TEST_EXISTS)) {
                caps->has_hinge_sensor = TRUE;
                g_free(caps->hinge_angle_path);
                caps->hinge_angle_path = g_strdup(angl_path);
            }
        } else if (g_strcmp0(name, "accel_3d") == 0) {
            caps->has_accel_sensor = TRUE;
            g_free(caps->accel_iio_dir);
            caps->accel_iio_dir = g_strdup(dev_dir);
        }
    }
    g_dir_close(dir);
}

static gboolean
uevent_has_direct_prop(const gchar *dev_dir)
{
    g_autofree gchar *uevent_path = g_build_filename(dev_dir, "uevent", NULL);
    g_autofree gchar *contents = NULL;
    if (!g_file_get_contents(uevent_path, &contents, NULL, NULL))
        return FALSE;

    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines && lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "PROP=")) {
            guint64 prop = g_ascii_strtoull(lines[i] + 5, NULL, 16);
            /* INPUT_PROP_DIRECT is bit 1 (value 0x2) */
            if ((prop & 0x2) != 0 && (prop & 0x1) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

static void
probe_input_devices(ShellHwCaps *caps)
{
    GDir *dir = g_dir_open("/sys/class/input", 0, NULL);
    if (!dir)
        return;

    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_prefix(entry, "input"))
            continue;

        g_autofree gchar *dev_dir = g_build_filename("/sys/class/input", entry, NULL);
        g_autofree gchar *inhibit_path = g_build_filename(dev_dir, "inhibited", NULL);
        if (!g_file_test(inhibit_path, G_FILE_TEST_EXISTS))
            continue;

        g_autofree gchar *name_path = g_build_filename(dev_dir, "name", NULL);
        g_autofree gchar *phys_path = g_build_filename(dev_dir, "phys", NULL);
        g_autofree gchar *name = read_trimmed_file(name_path);
        g_autofree gchar *phys = read_trimmed_file(phys_path);
        if (!name)
            continue;

        /* 1. Built-in AT / Matrix Keyboard */
        if (g_strcmp0(name, "AT Translated Set 2 keyboard") == 0 ||
            (phys && g_str_has_prefix(phys, "isa0060/serio0"))) {
            caps->has_internal_kbd = TRUE;
            g_ptr_array_add(caps->kbd_inhibit_paths, g_strdup(inhibit_path));
            continue;
        }

        /* 2. Built-in Touchpad */
        if (strstr(name, "Touchpad") != NULL || strstr(name, "touchpad") != NULL) {
            caps->has_touchpad = TRUE;
            g_ptr_array_add(caps->touchpad_inhibit_paths, g_strdup(inhibit_path));
            continue;
        }

        /* 3. Built-in Finger Touchscreen (INPUT_PROP_DIRECT, excluding Stylus/Keyboard/Mouse/UNKNOWN) */
        if (uevent_has_direct_prop(dev_dir) &&
            !g_str_has_suffix(name, "Stylus") &&
            !g_str_has_suffix(name, "Pen") &&
            !g_str_has_suffix(name, "Keyboard") &&
            !g_str_has_suffix(name, "Mouse") &&
            !g_str_has_suffix(name, "UNKNOWN")) {
            caps->has_touchscreen = TRUE;
            g_ptr_array_add(caps->touchscreen_inhibit_paths, g_strdup(inhibit_path));
        }
    }
    g_dir_close(dir);
}

static void
probe_kbd_backlight(ShellHwCaps *caps)
{
    /* 1. Standard Linux LED class kbd_backlight */
    GDir *dir = g_dir_open("/sys/class/leds", 0, NULL);
    if (dir) {
        const gchar *entry;
        while ((entry = g_dir_read_name(dir)) != NULL) {
            if (strstr(entry, "kbd_backlight") != NULL) {
                g_autofree gchar *led_dir = g_build_filename("/sys/class/leds", entry, NULL);
                g_autofree gchar *max_path = g_build_filename(led_dir, "max_brightness", NULL);
                if (read_int_file(max_path) > 0) {
                    caps->kbd_bl_backend = KBD_BL_SYSFS_LEDS;
                    caps->kbd_led_dir = g_strdup(led_dir);
                    caps->has_kbd_timeout = FALSE;
                    g_dir_close(dir);
                    return;
                }
            }
        }
        g_dir_close(dir);
    }

    /* 2. HP Embedded Controller (HKBL 0x97 / HKBT 0x96 via /sys/kernel/debug/ec/ec0/io) */
    g_autofree gchar *vendor = read_trimmed_file("/sys/class/dmi/id/sys_vendor");
    if (vendor && (g_strcmp0(vendor, "HP") == 0 || g_strcmp0(vendor, "Hewlett-Packard") == 0)) {
        if (g_file_test("/sys/kernel/debug/ec/ec0/io", G_FILE_TEST_EXISTS) ||
            g_file_test("/sys/module/ec_sys", G_FILE_TEST_IS_DIR)) {
            caps->kbd_bl_backend = KBD_BL_HP_EC;
            caps->has_kbd_timeout = TRUE;
            return;
        }
    }

    caps->kbd_bl_backend = KBD_BL_NONE;
    caps->has_kbd_timeout = FALSE;
}

static void
free_hw_caps(ShellHwCaps *caps)
{
    if (!caps)
        return;
    g_free(caps->display_backlight_dir);
    g_free(caps->hinge_angle_path);
    g_free(caps->accel_iio_dir);
    if (caps->kbd_inhibit_paths)
        g_ptr_array_free(caps->kbd_inhibit_paths, TRUE);
    if (caps->touchpad_inhibit_paths)
        g_ptr_array_free(caps->touchpad_inhibit_paths, TRUE);
    if (caps->touchscreen_inhibit_paths)
        g_ptr_array_free(caps->touchscreen_inhibit_paths, TRUE);
    g_free(caps->kbd_led_dir);
    g_free(caps);
}

const ShellHwCaps *
shell_hw_caps_get(gboolean refresh)
{
    if (cached_caps && !refresh)
        return cached_caps;

    if (cached_caps) {
        free_hw_caps(cached_caps);
        cached_caps = NULL;
    }

    ShellHwCaps *caps = g_new0(ShellHwCaps, 1);
    caps->kbd_inhibit_paths = g_ptr_array_new_with_free_func(g_free);
    caps->touchpad_inhibit_paths = g_ptr_array_new_with_free_func(g_free);
    caps->touchscreen_inhibit_paths = g_ptr_array_new_with_free_func(g_free);

    probe_display_backlight(caps);
    probe_iio_sensors(caps);
    probe_input_devices(caps);
    probe_kbd_backlight(caps);

    cached_caps = caps;
    return cached_caps;
}

void
shell_hw_caps_cleanup(void)
{
    if (cached_caps) {
        free_hw_caps(cached_caps);
        cached_caps = NULL;
    }
    g_clear_pointer(&cached_session_path, g_free);
    g_clear_object(&cached_system_bus);
}

gboolean
shell_hw_caps_read_hinge_angle_deg(const ShellHwCaps *caps, gdouble *out_deg)
{
    if (!caps || !caps->has_hinge_sensor || !caps->hinge_angle_path)
        return FALSE;

    g_autofree gchar *raw_str = read_trimmed_file(caps->hinge_angle_path);
    if (!raw_str || !*raw_str)
        return FALSE;

    gdouble val = g_ascii_strtod(raw_str, NULL);
    if (out_deg)
        *out_deg = val;
    return TRUE;
}

gint
shell_hw_caps_get_display_brightness_pct(const ShellHwCaps *caps)
{
    if (!caps || !caps->has_display_backlight ||
        !caps->display_backlight_dir || caps->display_max_brightness <= 0)
        return -1;

    g_autofree gchar *cur_path = g_build_filename(caps->display_backlight_dir, "brightness", NULL);
    gint val = read_int_file(cur_path);
    if (val < 0)
        return -1;

    return (val * 100) / caps->display_max_brightness;
}

static gchar *
get_active_login1_session_path(GDBusConnection *bus)
{
    GError *error = NULL;
    GVariant *res = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.freedesktop.login1.Seat", "ActiveSession"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        NULL,
        &error);

    if (!res) {
        g_clear_error(&error);
        return g_strdup("/org/freedesktop/login1/session/auto");
    }

    GVariant *v = NULL;
    g_variant_get(res, "(v)", &v);
    g_variant_unref(res);
    if (!v)
        return g_strdup("/org/freedesktop/login1/session/auto");

    const gchar *session_id = NULL;
    const gchar *session_path = NULL;
    g_variant_get(v, "(&s&o)", &session_id, &session_path);
    gchar *path = g_strdup(session_path);
    g_variant_unref(v);

    if (path && *path)
        return path;
    g_free(path);
    return g_strdup("/org/freedesktop/login1/session/auto");
}

gboolean
shell_hw_caps_set_display_brightness_pct(const ShellHwCaps *caps, gint pct)
{
    if (!caps || !caps->has_display_backlight ||
        !caps->display_backlight_dir || caps->display_max_brightness <= 0)
        return FALSE;

    gint clamped = CLAMP(pct, 1, 100);
    gint val = (clamped * caps->display_max_brightness) / 100;
    if (val < 1)
        val = 1;

    g_autofree gchar *device_name = g_path_get_basename(caps->display_backlight_dir);
    if (!cached_system_bus) {
        GError *error = NULL;
        cached_system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!cached_system_bus) {
            g_clear_error(&error);
            return FALSE;
        }
    }

    if (!cached_session_path) {
        cached_session_path = get_active_login1_session_path(cached_system_bus);
    }

    g_dbus_connection_call(
        cached_system_bus,
        "org.freedesktop.login1",
        cached_session_path,
        "org.freedesktop.login1.Session",
        "SetBrightness",
        g_variant_new("(ssu)", "backlight", device_name, (guint32)val),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    return TRUE;
}

gdouble
shell_hw_caps_read_rapl_watts(void)
{
    static guint64 prev_energy = 0;
    static gint64  prev_time   = 0;
    static gdouble last_watts  = 0.0;

    static const char *rapl_paths[] = {
        "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
        "/sys/class/powercap/intel-rapl:0/energy_uj",
        "/sys/class/powercap/amd_energy/energy1_input",
        "/sys/class/powercap/intel-rapl/intel-rapl:1/energy_uj",
        NULL
    };

    for (int i = 0; rapl_paths[i] != NULL; i++) {
        gchar *contents = NULL;
        if (g_file_get_contents(rapl_paths[i], &contents, NULL, NULL)) {
            guint64 cur_energy = g_ascii_strtoull(contents, NULL, 10);
            g_free(contents);
            if (cur_energy > 0) {
                gint64 now = g_get_monotonic_time();
                if (prev_energy > 0 && prev_time > 0 && cur_energy >= prev_energy) {
                    gdouble dt_sec = (gdouble)(now - prev_time) / 1000000.0;
                    if (dt_sec > 0.1) {
                        last_watts  = ((gdouble)(cur_energy - prev_energy) / 1000000.0) / dt_sec;
                        prev_energy = cur_energy;
                        prev_time   = now;
                        return last_watts;
                    }
                    return last_watts;
                }
                prev_energy = cur_energy;
                prev_time   = now;
                break;
            }
        }
    }

    /* Fallback 1: HWMON instant power inputs (skipping GPU/NVMe sensors) */
    for (int h = 0; h < 12; h++) {
        gchar name_path[128];
        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/hwmon%d/name", h);
        gchar *sensor_name = NULL;
        if (g_file_get_contents(name_path, &sensor_name, NULL, NULL)) {
            g_strstrip(sensor_name);
            if (g_strcmp0(sensor_name, "amdgpu") == 0 ||
                g_strcmp0(sensor_name, "nvidia") == 0 ||
                g_strcmp0(sensor_name, "nouveau") == 0 ||
                g_strcmp0(sensor_name, "nvme") == 0) {
                g_free(sensor_name);
                continue;
            }
            g_free(sensor_name);
        }

        guint64 total_microwatts = 0;
        int found_rails = 0;

        for (int p = 1; p <= 4; p++) {
            gchar hwmon_path[128];
            snprintf(hwmon_path, sizeof(hwmon_path), "/sys/class/hwmon/hwmon%d/power%d_input", h, p);
            gchar *contents = NULL;
            if (g_file_get_contents(hwmon_path, &contents, NULL, NULL)) {
                guint64 microwatts = g_ascii_strtoull(contents, NULL, 10);
                g_free(contents);
                if (microwatts > 0) {
                    total_microwatts += microwatts;
                    found_rails++;
                }
            }
        }

        if (found_rails > 0 && total_microwatts > 0)
            return (gdouble)total_microwatts / 1000000.0;
    }

    /* Fallback 2: Laptop power supply battery power_now */
    static const char *bat_paths[] = {
        "/sys/class/power_supply/BAT0/power_now",
        "/sys/class/power_supply/BAT1/power_now",
        NULL
    };
    for (int b = 0; bat_paths[b] != NULL; b++) {
        gchar *contents = NULL;
        if (g_file_get_contents(bat_paths[b], &contents, NULL, NULL)) {
            guint64 microwatts = g_ascii_strtoull(contents, NULL, 10);
            g_free(contents);
            if (microwatts > 0)
                return (gdouble)microwatts / 1000000.0;
        }
    }

    return 0.0;
}
