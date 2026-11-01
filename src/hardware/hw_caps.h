#ifndef SHELL_HW_CAPS_H
#define SHELL_HW_CAPS_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KBD_BL_NONE = 0,
    KBD_BL_SYSFS_LEDS, /* Standard /sys/class/leds/<dev>::kbd_backlight */
    KBD_BL_HP_EC       /* HP Embedded Controller /sys/kernel/debug/ec/ec0/io (0x96 HKBT, 0x97 HKBL) */
} ShellKbdBacklightBackend;

typedef struct {
    /* Display Backlight */
    gboolean                  has_display_backlight;
    gchar                    *display_backlight_dir;
    gint                      display_max_brightness;

    /* 360° Foldable IIO Sensors */
    gboolean                  has_hinge_sensor;
    gchar                    *hinge_angle_path;     /* e.g. /sys/bus/iio/devices/iio:deviceX/in_angl0_raw */
    gboolean                  has_accel_sensor;
    gchar                    *accel_iio_dir;        /* e.g. /sys/bus/iio/devices/iio:deviceY */

    /* Input Device Inhibition Sysfs Paths */
    gboolean                  has_internal_kbd;
    GPtrArray                *kbd_inhibit_paths;    /* Array of gchar* (/sys/class/input/inputN/inhibited) */
    gboolean                  has_touchpad;
    GPtrArray                *touchpad_inhibit_paths;
    gboolean                  has_touchscreen;
    GPtrArray                *touchscreen_inhibit_paths;

    /* Keyboard Backlight & Auto-Off Timeout */
    ShellKbdBacklightBackend  kbd_bl_backend;
    gchar                    *kbd_led_dir;          /* e.g. /sys/class/leds/platform::kbd_backlight */
    gboolean                  has_kbd_timeout;
} ShellHwCaps;

/* Returns a process-wide cached hardware capability descriptor (or re-probes if refresh == TRUE) */
const ShellHwCaps *shell_hw_caps_get(gboolean refresh);
void               shell_hw_caps_cleanup(void);

/* Reads the current hinge angle in degrees (0.0 - 360.0). Returns TRUE on success. */
gboolean           shell_hw_caps_read_hinge_angle_deg(const ShellHwCaps *caps, gdouble *out_deg);

/* Display backlight percentage helpers (0 - 100) */
gint               shell_hw_caps_get_display_brightness_pct(const ShellHwCaps *caps);
gboolean           shell_hw_caps_set_display_brightness_pct(const ShellHwCaps *caps, gint pct);

/* Shared RAPL / hwmon / BAT0 power draw reader in Watts */
gdouble            shell_hw_caps_read_rapl_watts(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_HW_CAPS_H */
