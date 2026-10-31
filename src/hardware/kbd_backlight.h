#ifndef SHELL_KBD_BACKLIGHT_H
#define SHELL_KBD_BACKLIGHT_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns TRUE if any keyboard backlight backend (sysfs LEDs or HP EC) is present */
gboolean shell_kbd_backlight_available(void);

/* Returns TRUE if keyboard backlight auto-off timeout is configurable (e.g. HP EC 0x96 HKBT) */
gboolean shell_kbd_backlight_has_timeout(void);

/* Returns maximum brightness level (e.g. 2 for HP EC: 0=Off, 1=Low, 2=High) */
gint     shell_kbd_backlight_get_max_level(void);

/* Reads current keyboard backlight level (0..max_level, or -1 if unavailable) */
gint     shell_kbd_backlight_get_level(void);

/* Sets keyboard backlight level (0..max_level) */
gboolean shell_kbd_backlight_set_level(gint level);

/* Reads current keyboard backlight auto-off timeout in seconds (0 = Always On, 30 = 30s, 180 = 3m, or -1) */
gint     shell_kbd_backlight_get_timeout_sec(void);

/* Sets keyboard backlight auto-off timeout in seconds (0 = Always On, 30 = 30s, 180 = 3m) */
gboolean shell_kbd_backlight_set_timeout_sec(gint timeout_sec);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_KBD_BACKLIGHT_H */
