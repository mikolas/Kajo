#pragma once

#include <glib.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

typedef struct _ShellOskXkb ShellOskXkb;

typedef struct {
    uint32_t evdev_code;
    uint32_t mods_mask;
    gboolean is_dead_key;
    gboolean found;
} ShellOskSymbolBinding;

ShellOskXkb *shell_osk_xkb_new(void);
void shell_osk_xkb_free(ShellOskXkb *self);

/* Load keymap either from raw XKB_KEYMAP_FORMAT_TEXT_V1 string (from wl_keyboard::keymap)
 * or from RMLVO layout names (e.g. "fi", "us"). */
gboolean shell_osk_xkb_load_from_string(ShellOskXkb *self, const char *keymap_str);
gboolean shell_osk_xkb_load_from_names(ShellOskXkb *self, const char *layout, const char *variant);

const char *shell_osk_xkb_get_keymap_string(ShellOskXkb *self);

void shell_osk_xkb_set_group(ShellOskXkb *self, uint32_t group);
uint32_t shell_osk_xkb_get_group(ShellOskXkb *self);
uint32_t shell_osk_xkb_get_num_groups(ShellOskXkb *self);
const char *shell_osk_xkb_get_group_name(ShellOskXkb *self, uint32_t group);
void shell_osk_xkb_get_short_group_label(ShellOskXkb *self, char *out_buf, size_t buf_len);

/* Modifier bitmasks in the current XKB keymap */
uint32_t shell_osk_xkb_get_shift_mask(ShellOskXkb *self);
uint32_t shell_osk_xkb_get_caps_mask(ShellOskXkb *self);
uint32_t shell_osk_xkb_get_ctrl_mask(ShellOskXkb *self);
uint32_t shell_osk_xkb_get_alt_mask(ShellOskXkb *self);
uint32_t shell_osk_xkb_get_super_mask(ShellOskXkb *self);
uint32_t shell_osk_xkb_get_altgr_mask(ShellOskXkb *self);

/* Resolve UTF-8 keycap label for an evdev keycode given active modifiers */
gboolean shell_osk_xkb_get_key_label(
    ShellOskXkb *self,
    uint32_t evdev_code,
    gboolean shift_active,
    gboolean caps_active,
    gboolean altgr_active,
    char *out_buf,
    size_t buf_len
);

/* Resolve secondary hint label (e.g. Shift or AltGr symbol) for a keycap */
gboolean shell_osk_xkb_get_key_hint(
    ShellOskXkb *self,
    uint32_t evdev_code,
    gboolean shift_active,
    gboolean altgr_active,
    char *out_buf,
    size_t buf_len
);

/* Reverse-resolve a desired UTF-8 symbol (e.g. "@", "€", "{", "[", "]", "}", "~")
 * to its (evdev_code, mods_mask) in the currently active layout group. */
ShellOskSymbolBinding shell_osk_xkb_find_symbol(ShellOskXkb *self, const char *utf8_symbol);
