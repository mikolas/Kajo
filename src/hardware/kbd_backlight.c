#define _POSIX_C_SOURCE 200809L
#include "kbd_backlight.h"
#include "hw_caps.h"
#include <fcntl.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define HP_EC_IO_PATH        "/sys/kernel/debug/ec/ec0/io"
#define HP_EC_OFFSET_HKBT    0x96  /* 150: Keyboard Backlight Auto-Off Timeout (0x00=Always On, 0x01=30s, 0x03=3m) */
#define HP_EC_OFFSET_HKBL    0x97  /* 151: Keyboard Backlight Brightness Level (0x00=Off, 0x02=Low 50%, 0x01=High 100%) */

static gboolean
hp_ec_read_byte(off_t offset, guint8 *out_val)
{
    int fd = open(HP_EC_IO_PATH, O_RDONLY);
    if (fd < 0)
        return FALSE;

    guint8 val = 0;
    ssize_t n = pread(fd, &val, 1, offset);
    close(fd);
    if (n != 1)
        return FALSE;

    if (out_val)
        *out_val = val;
    return TRUE;
}

static gboolean
hp_ec_write_byte(off_t offset, guint8 val)
{
    int fd = open(HP_EC_IO_PATH, O_WRONLY);
    if (fd < 0)
        return FALSE;

    ssize_t n = pwrite(fd, &val, 1, offset);
    close(fd);
    return (n == 1);
}

static gint
read_sysfs_int(const gchar *path)
{
    g_autofree gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return -1;
    return atoi(contents);
}

gboolean
shell_kbd_backlight_available(void)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    return (caps && caps->kbd_bl_backend != KBD_BL_NONE);
}

gboolean
shell_kbd_backlight_has_timeout(void)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    return (caps && caps->has_kbd_timeout);
}

gint
shell_kbd_backlight_get_max_level(void)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps)
        return 0;

    if (caps->kbd_bl_backend == KBD_BL_HP_EC)
        return 2;

    if (caps->kbd_bl_backend == KBD_BL_SYSFS_LEDS && caps->kbd_led_dir) {
        g_autofree gchar *max_path = g_build_filename(caps->kbd_led_dir, "max_brightness", NULL);
        gint max_val = read_sysfs_int(max_path);
        return (max_val > 0) ? max_val : 2;
    }

    return 0;
}

gint
shell_kbd_backlight_get_level(void)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps)
        return -1;

    if (caps->kbd_bl_backend == KBD_BL_HP_EC) {
        guint8 val = 0;
        if (!hp_ec_read_byte(HP_EC_OFFSET_HKBL, &val))
            return -1;
        /* Per DSDT _Q9B: 0x00 = Off (0%), 0x02 = Low (50%), 0x01 = High (100%) */
        if ((val & 0x03) == 0)
            return 0;
        if ((val & 0x03) == 2)
            return 1;
        return 2;
    }

    if (caps->kbd_bl_backend == KBD_BL_SYSFS_LEDS && caps->kbd_led_dir) {
        g_autofree gchar *cur_path = g_build_filename(caps->kbd_led_dir, "brightness", NULL);
        return read_sysfs_int(cur_path);
    }

    return -1;
}

gboolean
shell_kbd_backlight_set_level(gint level)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps || caps->kbd_bl_backend == KBD_BL_NONE)
        return FALSE;

    gint max_lvl = shell_kbd_backlight_get_max_level();
    gint clamped = CLAMP(level, 0, max_lvl > 0 ? max_lvl : 2);

    if (caps->kbd_bl_backend == KBD_BL_HP_EC) {
        /* Per DSDT _Q9B: 0x00 = Off (0%), 0x02 = Low (50%), 0x01 = High (100%) */
        guint8 ec_val = 0x00;
        if (clamped == 1)
            ec_val = 0x02;
        else if (clamped >= 2)
            ec_val = 0x01;
        return hp_ec_write_byte(HP_EC_OFFSET_HKBL, ec_val);
    }

    if (caps->kbd_bl_backend == KBD_BL_SYSFS_LEDS && caps->kbd_led_dir) {
        /* Try direct sysfs write first */
        g_autofree gchar *cur_path = g_build_filename(caps->kbd_led_dir, "brightness", NULL);
        gchar buf[16];
        int len = snprintf(buf, sizeof(buf), "%d\n", clamped);
        int fd = open(cur_path, O_WRONLY);
        if (fd >= 0) {
            ssize_t n = write(fd, buf, (size_t)len);
            close(fd);
            if (n == len)
                return TRUE;
        }

        /* Fallback to asynchronous UPower KbdBacklight D-Bus call */
        GError *error = NULL;
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (bus) {
            g_dbus_connection_call(
                bus,
                "org.freedesktop.UPower",
                "/org/freedesktop/UPower/KbdBacklight",
                "org.freedesktop.UPower.KbdBacklight",
                "SetBrightness",
                g_variant_new("(i)", clamped),
                NULL,
                G_DBUS_CALL_FLAGS_NONE,
                1000,
                NULL,
                NULL,
                NULL);
            g_object_unref(bus);
            return TRUE;
        }
        g_clear_error(&error);
        return FALSE;
    }

    return FALSE;
}

gint
shell_kbd_backlight_get_timeout_sec(void)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps || !caps->has_kbd_timeout || caps->kbd_bl_backend != KBD_BL_HP_EC)
        return -1;

    guint8 val = 0;
    if (!hp_ec_read_byte(HP_EC_OFFSET_HKBT, &val))
        return -1;

    guint8 code = val & 0x07;
    if (code == 0x00)
        return 0;    /* Always On */
    if (code == 0x01)
        return 30;   /* 30 Seconds */
    if (code == 0x03)
        return 180;  /* 3 Minutes */

    return 30;
}

gboolean
shell_kbd_backlight_set_timeout_sec(gint timeout_sec)
{
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (!caps || !caps->has_kbd_timeout || caps->kbd_bl_backend != KBD_BL_HP_EC)
        return FALSE;

    guint8 code = 0x00;
    if (timeout_sec <= 0)
        code = 0x00; /* Always On (Never Timeout) */
    else if (timeout_sec <= 60)
        code = 0x01; /* 30 Seconds */
    else
        code = 0x03; /* 3 Minutes */

    return hp_ec_write_byte(HP_EC_OFFSET_HKBT, code);
}
