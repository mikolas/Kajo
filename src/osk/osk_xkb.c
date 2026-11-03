#include "osk_xkb.h"
#include <ctype.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct _ShellOskXkb {
    struct xkb_context *ctx;
    struct xkb_keymap *keymap;
    char *keymap_str;
    uint32_t active_group;

    uint32_t shift_mask;
    uint32_t caps_mask;
    uint32_t ctrl_mask;
    uint32_t alt_mask;
    uint32_t super_mask;
    uint32_t altgr_mask;
};

static uint32_t resolve_mod_mask(struct xkb_keymap *km, const char *name1, const char *name2) {
    if (!km) return 0;
    xkb_mod_index_t idx = xkb_keymap_mod_get_index(km, name1);
    if (idx == XKB_MOD_INVALID && name2) {
        idx = xkb_keymap_mod_get_index(km, name2);
    }
    if (idx == XKB_MOD_INVALID) return 0;
    return (1u << idx);
}

static void update_keymap_caches(ShellOskXkb *self) {
    g_clear_pointer(&self->keymap_str, free);
    if (!self->keymap) return;

    self->keymap_str = xkb_keymap_get_as_string(self->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    self->shift_mask = resolve_mod_mask(self->keymap, XKB_MOD_NAME_SHIFT, "Shift");
    self->caps_mask  = resolve_mod_mask(self->keymap, XKB_MOD_NAME_CAPS, "Lock");
    self->ctrl_mask  = resolve_mod_mask(self->keymap, XKB_MOD_NAME_CTRL, "Control");
    self->alt_mask   = resolve_mod_mask(self->keymap, XKB_MOD_NAME_ALT, "Mod1");
    self->super_mask = resolve_mod_mask(self->keymap, XKB_MOD_NAME_LOGO, "Mod4");
    self->altgr_mask = resolve_mod_mask(self->keymap, "Mod5", "ISO_Level3_Shift");

    uint32_t num_groups = xkb_keymap_num_layouts(self->keymap);
    if (num_groups > 0 && self->active_group >= num_groups) {
        self->active_group = 0;
    }
}

ShellOskXkb *shell_osk_xkb_new(void) {
    ShellOskXkb *self = g_new0(ShellOskXkb, 1);
    self->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    /* Initialize with default system/US fallback until compositor sends keymap */
    shell_osk_xkb_load_from_names(self, NULL, NULL);
    return self;
}

void shell_osk_xkb_free(ShellOskXkb *self) {
    if (!self) return;
    g_clear_pointer(&self->keymap_str, free);
    if (self->keymap) xkb_keymap_unref(self->keymap);
    if (self->ctx) xkb_context_unref(self->ctx);
    g_free(self);
}

gboolean shell_osk_xkb_load_from_string(ShellOskXkb *self, const char *keymap_str) {
    if (!self || !self->ctx || !keymap_str || !*keymap_str) return FALSE;
    struct xkb_keymap *km = xkb_keymap_new_from_string(
        self->ctx, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    if (!km) return FALSE;
    if (self->keymap) xkb_keymap_unref(self->keymap);
    self->keymap = km;
    update_keymap_caches(self);
    return TRUE;
}

gboolean shell_osk_xkb_load_from_names(ShellOskXkb *self, const char *layout, const char *variant) {
    if (!self || !self->ctx) return FALSE;
    struct xkb_rule_names names = {
        .rules = NULL,
        .model = "pc105",
        .layout = layout,
        .variant = variant,
        .options = NULL,
    };
    struct xkb_keymap *km = xkb_keymap_new_from_names(
        self->ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    if (!km) return FALSE;
    if (self->keymap) xkb_keymap_unref(self->keymap);
    self->keymap = km;
    update_keymap_caches(self);
    return TRUE;
}

const char *shell_osk_xkb_get_keymap_string(ShellOskXkb *self) {
    return self ? self->keymap_str : NULL;
}

void shell_osk_xkb_set_group(ShellOskXkb *self, uint32_t group) {
    if (!self || !self->keymap) return;
    uint32_t n = xkb_keymap_num_layouts(self->keymap);
    if (n == 0) {
        self->active_group = 0;
    } else {
        self->active_group = group % n;
    }
}

uint32_t shell_osk_xkb_get_group(ShellOskXkb *self) {
    return self ? self->active_group : 0;
}

uint32_t shell_osk_xkb_get_num_groups(ShellOskXkb *self) {
    if (!self || !self->keymap) return 1;
    uint32_t n = xkb_keymap_num_layouts(self->keymap);
    return n > 0 ? n : 1;
}

const char *shell_osk_xkb_get_group_name(ShellOskXkb *self, uint32_t group) {
    if (!self || !self->keymap) return "US";
    const char *name = xkb_keymap_layout_get_name(self->keymap, group);
    return (name && *name) ? name : "US";
}

void shell_osk_xkb_get_short_group_label(ShellOskXkb *self, char *out_buf, size_t buf_len) {
    if (!out_buf || buf_len == 0) return;
    const char *full = shell_osk_xkb_get_group_name(self, self ? self->active_group : 0);
    if (g_ascii_strncasecmp(full, "English (US)", 12) == 0 ||
        g_ascii_strcasecmp(full, "English") == 0 ||
        g_ascii_strcasecmp(full, "US") == 0) {
        g_strlcpy(out_buf, "US", buf_len);
        return;
    }
    if (g_ascii_strncasecmp(full, "English (UK)", 12) == 0 ||
        g_ascii_strncasecmp(full, "English (British)", 17) == 0) {
        g_strlcpy(out_buf, "GB", buf_len);
        return;
    }
    if (g_ascii_strncasecmp(full, "Swedish", 7) == 0) {
        g_strlcpy(out_buf, "SE", buf_len);
        return;
    }
    if (g_ascii_strncasecmp(full, "German", 6) == 0) {
        g_strlcpy(out_buf, "DE", buf_len);
        return;
    }
    if (g_ascii_strncasecmp(full, "Spanish", 7) == 0) {
        g_strlcpy(out_buf, "ES", buf_len);
        return;
    }
    if (g_ascii_strncasecmp(full, "Polish", 6) == 0) {
        g_strlcpy(out_buf, "PL", buf_len);
        return;
    }
    if (g_ascii_strncasecmp(full, "Estonian", 8) == 0) {
        g_strlcpy(out_buf, "EE", buf_len);
        return;
    }

    char code[3] = { 'U', 'S', '\0' };
    if (full[0] && full[1]) {
        code[0] = (char)g_ascii_toupper(full[0]);
        code[1] = (char)g_ascii_toupper(full[1]);
    }
    g_strlcpy(out_buf, code, buf_len);
}

uint32_t shell_osk_xkb_get_shift_mask(ShellOskXkb *self) { return self ? self->shift_mask : 0; }
uint32_t shell_osk_xkb_get_caps_mask(ShellOskXkb *self)  { return self ? self->caps_mask : 0; }
uint32_t shell_osk_xkb_get_ctrl_mask(ShellOskXkb *self)  { return self ? self->ctrl_mask : 0; }
uint32_t shell_osk_xkb_get_alt_mask(ShellOskXkb *self)   { return self ? self->alt_mask : 0; }
uint32_t shell_osk_xkb_get_super_mask(ShellOskXkb *self) { return self ? self->super_mask : 0; }
uint32_t shell_osk_xkb_get_altgr_mask(ShellOskXkb *self) { return self ? self->altgr_mask : 0; }

static const char *dead_keysym_to_str(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_dead_grave:       return "`";
        case XKB_KEY_dead_acute:       return "´";
        case XKB_KEY_dead_circumflex:  return "^";
        case XKB_KEY_dead_tilde:       return "~";
        case XKB_KEY_dead_macron:      return "¯";
        case XKB_KEY_dead_breve:       return "˘";
        case XKB_KEY_dead_abovedot:    return "˙";
        case XKB_KEY_dead_diaeresis:   return "¨";
        case XKB_KEY_dead_abovering:   return "°";
        case XKB_KEY_dead_doubleacute: return "˝";
        case XKB_KEY_dead_caron:       return "ˇ";
        case XKB_KEY_dead_cedilla:     return "¸";
        default:                       return NULL;
    }
}

static gboolean query_keycode_with_masks(
    ShellOskXkb *self,
    uint32_t evdev_code,
    uint32_t depressed_mods,
    uint32_t locked_mods,
    char *out_buf,
    size_t buf_len,
    gboolean *out_is_dead
) {
    if (out_is_dead) *out_is_dead = FALSE;
    if (!self || !self->keymap || !out_buf || buf_len == 0) return FALSE;
    out_buf[0] = '\0';

    struct xkb_state *state = xkb_state_new(self->keymap);
    if (!state) return FALSE;

    xkb_state_update_mask(state, depressed_mods, 0, locked_mods, 0, 0, self->active_group);
    xkb_keycode_t kc = (xkb_keycode_t)(evdev_code + 8);

    char tmp[32] = {0};
    int len = xkb_state_key_get_utf8(state, kc, tmp, sizeof(tmp));
    if (len > 0 && (unsigned char)tmp[0] >= 0x20 && (unsigned char)tmp[0] != 0x7F) {
        g_strlcpy(out_buf, tmp, buf_len);
        xkb_state_unref(state);
        return TRUE;
    }

    xkb_keysym_t sym = xkb_state_key_get_one_sym(state, kc);
    xkb_state_unref(state);

    const char *dead = dead_keysym_to_str(sym);
    if (dead) {
        if (out_is_dead) *out_is_dead = TRUE;
        g_strlcpy(out_buf, dead, buf_len);
        return TRUE;
    }

    return FALSE;
}

gboolean shell_osk_xkb_get_key_label(
    ShellOskXkb *self,
    uint32_t evdev_code,
    gboolean shift_active,
    gboolean caps_active,
    gboolean altgr_active,
    char *out_buf,
    size_t buf_len
) {
    if (!self) return FALSE;
    uint32_t depressed = 0;
    uint32_t locked = 0;
    if (shift_active) depressed |= self->shift_mask;
    if (altgr_active) depressed |= self->altgr_mask;
    if (caps_active)  locked    |= self->caps_mask;

    return query_keycode_with_masks(self, evdev_code, depressed, locked, out_buf, buf_len, NULL);
}

gboolean shell_osk_xkb_get_key_hint(
    ShellOskXkb *self,
    uint32_t evdev_code,
    gboolean shift_active,
    gboolean altgr_active,
    char *out_buf,
    size_t buf_len
) {
    if (!self || !out_buf || buf_len == 0) return FALSE;
    out_buf[0] = '\0';

    char primary[16] = {0};
    shell_osk_xkb_get_key_label(self, evdev_code, shift_active, FALSE, altgr_active, primary, sizeof(primary));

    /* If AltGr is not active, prefer showing the AltGr symbol on keys that have one (like 2->@, 5->€, e->€, 7->{),
     * or the Shift symbol on number/punctuation keys. */
    if (!altgr_active && self->altgr_mask != 0) {
        char altgr_str[16] = {0};
        if (query_keycode_with_masks(self, evdev_code, self->altgr_mask, 0, altgr_str, sizeof(altgr_str), NULL)) {
            if (g_strcmp0(altgr_str, primary) != 0) {
                /* For number row (2..13) when unshifted, show both Shift & AltGr or Shift if no special AltGr */
                if (!shift_active && evdev_code >= 2 && evdev_code <= 13) {
                    char shift_str[16] = {0};
                    if (query_keycode_with_masks(self, evdev_code, self->shift_mask, 0, shift_str, sizeof(shift_str), NULL) &&
                        g_strcmp0(shift_str, altgr_str) != 0) {
                        g_snprintf(out_buf, buf_len, "%s %s", shift_str, altgr_str);
                        return TRUE;
                    }
                }
                g_strlcpy(out_buf, altgr_str, buf_len);
                return TRUE;
            }
        }
    }

    if (!shift_active) {
        char shift_str[16] = {0};
        if (query_keycode_with_masks(self, evdev_code, self->shift_mask, 0, shift_str, sizeof(shift_str), NULL)) {
            /* Only show Shift hint if it's not simply the uppercase version of primary */
            gchar *up = g_utf8_strup(primary, -1);
            gboolean is_just_upper = (g_strcmp0(up, shift_str) == 0);
            g_free(up);
            if (!is_just_upper && g_strcmp0(shift_str, primary) != 0) {
                g_strlcpy(out_buf, shift_str, buf_len);
                return TRUE;
            }
        }
    }

    return FALSE;
}

ShellOskSymbolBinding shell_osk_xkb_find_symbol(ShellOskXkb *self, const char *utf8_symbol) {
    ShellOskSymbolBinding res = {0, 0, FALSE, FALSE};
    if (!self || !self->keymap || !utf8_symbol || !*utf8_symbol) return res;

    /* Candidate evdev keycodes on standard PC105 keyboard:
     * Number row (2..13), QWERTY row (16..27), Home row (30..41, 43), Bottom row (86, 44..53) */
    static const uint32_t candidate_codes[] = {
        2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
        30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 43,
        86, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53
    };

    uint32_t mod_levels[4] = {
        0,
        self->shift_mask,
        self->altgr_mask,
        self->shift_mask | self->altgr_mask
    };

    ShellOskSymbolBinding dead_fallback = {0, 0, TRUE, FALSE};

    for (size_t m = 0; m < G_N_ELEMENTS(mod_levels); m++) {
        uint32_t mods = mod_levels[m];
        if (m >= 2 && self->altgr_mask == 0) continue;

        for (size_t i = 0; i < G_N_ELEMENTS(candidate_codes); i++) {
            uint32_t code = candidate_codes[i];
            char buf[16] = {0};
            gboolean is_dead = FALSE;
            if (query_keycode_with_masks(self, code, mods, 0, buf, sizeof(buf), &is_dead)) {
                if (g_strcmp0(buf, utf8_symbol) == 0) {
                    if (!is_dead) {
                        res.evdev_code = code;
                        res.mods_mask = mods;
                        res.is_dead_key = FALSE;
                        res.found = TRUE;
                        return res;
                    } else if (!dead_fallback.found) {
                        dead_fallback.evdev_code = code;
                        dead_fallback.mods_mask = mods;
                        dead_fallback.is_dead_key = TRUE;
                        dead_fallback.found = TRUE;
                    }
                }
            }
        }
    }

    if (dead_fallback.found) return dead_fallback;
    return res;
}
