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

    static const struct {
        const char *prefix;
        gboolean    exact;
        const char *code;
    } label_map[] = {
        { "English (US)",      FALSE, "US" },
        { "English",           TRUE,  "US" },
        { "US",                TRUE,  "US" },
        { "English (UK)",      FALSE, "GB" },
        { "English (British)", FALSE, "GB" },
        { "Finnish",           FALSE, "FI" },
        { "Russian",           FALSE, "RU" },
        { "Swedish",           FALSE, "SE" },
        { "German",            FALSE, "DE" },
        { "French",            FALSE, "FR" },
        { "Spanish",           FALSE, "ES" },
        { "Italian",           FALSE, "IT" },
        { "Portuguese",        FALSE, "PT" },
        { "Dutch",             FALSE, "NL" },
        { "Norwegian",         FALSE, "NO" },
        { "Danish",            FALSE, "DK" },
        { "Polish",            FALSE, "PL" },
        { "Estonian",          FALSE, "EE" },
        { "Ukrainian",         FALSE, "UA" },
        { "Czech",             FALSE, "CZ" },
        { "Hungarian",         FALSE, "HU" },
        { "Turkish",           FALSE, "TR" },
        { "Greek",             FALSE, "GR" },
        { "Japanese",          FALSE, "JP" },
        { "Korean",            FALSE, "KR" },
        { "Arabic",            FALSE, "AR" },
        { "Hebrew",            FALSE, "HE" },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(label_map); i++) {
        gboolean match = label_map[i].exact
            ? (g_ascii_strcasecmp(full, label_map[i].prefix) == 0)
            : (g_ascii_strncasecmp(full, label_map[i].prefix, strlen(label_map[i].prefix)) == 0);
        if (match) {
            g_strlcpy(out_buf, label_map[i].code, buf_len);
            return;
        }
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

static gboolean query_keycode_in_group(
    ShellOskXkb *self,
    uint32_t group,
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

    uint32_t n = xkb_keymap_num_layouts(self->keymap);
    if (n > 0 && group >= n) return FALSE;

    struct xkb_state *state = xkb_state_new(self->keymap);
    if (!state) return FALSE;

    xkb_state_update_mask(state, depressed_mods, 0, locked_mods, 0, 0, group);
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

static gboolean query_keycode_with_masks(
    ShellOskXkb *self,
    uint32_t evdev_code,
    uint32_t depressed_mods,
    uint32_t locked_mods,
    char *out_buf,
    size_t buf_len,
    gboolean *out_is_dead
) {
    return query_keycode_in_group(
        self,
        self ? self->active_group : 0,
        evdev_code,
        depressed_mods,
        locked_mods,
        out_buf,
        buf_len,
        out_is_dead
    );
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

static gboolean find_symbol_in_group(
    ShellOskXkb *self,
    uint32_t group,
    const char *utf8_symbol,
    ShellOskSymbolBinding *out_direct,
    ShellOskSymbolBinding *inout_dead_fallback
) {
    /* Candidate evdev keycodes on standard PC105 keyboard:
     * Number row (2..13, 41), QWERTY row (16..27), Home row (30..40, 43), Bottom row (86, 44..53) */
    static const uint32_t candidate_codes[] = {
        41, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
        30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 43,
        86, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53
    };

    uint32_t mod_levels[4] = {
        0,
        self->shift_mask,
        self->altgr_mask,
        self->shift_mask | self->altgr_mask
    };

    for (size_t m = 0; m < G_N_ELEMENTS(mod_levels); m++) {
        uint32_t mods = mod_levels[m];
        if (m >= 2 && self->altgr_mask == 0) continue;

        for (size_t i = 0; i < G_N_ELEMENTS(candidate_codes); i++) {
            uint32_t code = candidate_codes[i];
            char buf[16] = {0};
            gboolean is_dead = FALSE;
            if (query_keycode_in_group(self, group, code, mods, 0, buf, sizeof(buf), &is_dead)) {
                if (g_strcmp0(buf, utf8_symbol) == 0) {
                    if (!is_dead) {
                        out_direct->evdev_code = code;
                        out_direct->mods_mask = mods;
                        out_direct->group = group;
                        out_direct->is_dead_key = FALSE;
                        out_direct->found = TRUE;
                        return TRUE;
                    } else if (inout_dead_fallback && !inout_dead_fallback->found) {
                        inout_dead_fallback->evdev_code = code;
                        inout_dead_fallback->mods_mask = mods;
                        inout_dead_fallback->group = group;
                        inout_dead_fallback->is_dead_key = TRUE;
                        inout_dead_fallback->found = TRUE;
                    }
                }
            }
        }
    }

    return FALSE;
}

ShellOskSymbolBinding shell_osk_xkb_find_symbol(ShellOskXkb *self, const char *utf8_symbol) {
    ShellOskSymbolBinding res = {0, 0, 0, FALSE, FALSE};
    if (!self || !self->keymap || !utf8_symbol || !*utf8_symbol) return res;

    ShellOskSymbolBinding dead_fallback = {0, 0, 0, TRUE, FALSE};
    if (find_symbol_in_group(self, self->active_group, utf8_symbol, &res, &dead_fallback)) {
        return res;
    }

    uint32_t num_groups = shell_osk_xkb_get_num_groups(self);
    for (uint32_t g = 0; g < num_groups; g++) {
        if (g == self->active_group) continue;
        if (find_symbol_in_group(self, g, utf8_symbol, &res, &dead_fallback)) {
            return res;
        }
    }

    if (dead_fallback.found) return dead_fallback;
    return res;
}

static uint32_t resolve_evdev_by_name(struct xkb_keymap *km, const char *name, uint32_t fallback) {
    if (!km || !name) return fallback;
    xkb_keycode_t kc = xkb_keymap_key_by_name(km, name);
    if (kc != XKB_KEYCODE_INVALID && kc >= 8) {
        return (uint32_t)(kc - 8);
    }
    return fallback;
}

static void row_spec_push(ShellOskRowSpec *row, uint32_t evdev_code) {
    if (!row || evdev_code == 0 || row->count >= SHELL_OSK_MAX_ROW_KEYS) return;
    row->evdev_codes[row->count++] = evdev_code;
}

static gboolean is_alpha_key(
    struct xkb_state *state_base,
    struct xkb_state *state_shift,
    uint32_t evdev_code
) {
    if (evdev_code == 0) return FALSE;
    xkb_keycode_t kc = (xkb_keycode_t)(evdev_code + 8);
    struct xkb_state *states[2] = { state_base, state_shift };
    for (size_t i = 0; i < 2; i++) {
        if (!states[i]) continue;
        char buf[32] = {0};
        int len = xkb_state_key_get_utf8(states[i], kc, buf, sizeof(buf));
        if (len > 0 && g_utf8_validate(buf, len, NULL)) {
            gunichar ch = g_utf8_get_char(buf);
            if (g_unichar_isalpha(ch)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean has_ascii_char(
    struct xkb_state *state_base,
    struct xkb_state *state_shift,
    uint32_t evdev_code,
    char target
) {
    if (evdev_code == 0) return FALSE;
    xkb_keycode_t kc = (xkb_keycode_t)(evdev_code + 8);
    struct xkb_state *states[2] = { state_base, state_shift };
    for (size_t i = 0; i < 2; i++) {
        if (!states[i]) continue;
        char buf[32] = {0};
        int len = xkb_state_key_get_utf8(states[i], kc, buf, sizeof(buf));
        if (len == 1 && buf[0] == target) {
            return TRUE;
        }
    }
    return FALSE;
}

void shell_osk_xkb_compute_alpha_layout(ShellOskXkb *self, ShellOskAlphaLayout *out_layout) {
    if (!out_layout) return;
    memset(out_layout, 0, sizeof(*out_layout));

    if (!self || !self->keymap) {
        for (uint32_t c = 2; c <= 12; c++) row_spec_push(&out_layout->rows[0], c);
        for (uint32_t c = 16; c <= 25; c++) row_spec_push(&out_layout->rows[1], c);
        for (uint32_t c = 30; c <= 40; c++) row_spec_push(&out_layout->rows[2], c);
        for (uint32_t c = 44; c <= 53; c++) row_spec_push(&out_layout->rows[3], c);
        return;
    }

    struct xkb_state *state_base = xkb_state_new(self->keymap);
    struct xkb_state *state_shift = xkb_state_new(self->keymap);
    if (state_base) {
        xkb_state_update_mask(state_base, 0, 0, 0, 0, 0, self->active_group);
    }
    if (state_shift) {
        xkb_state_update_mask(state_shift, self->shift_mask, 0, 0, 0, 0, self->active_group);
    }

    static const struct { const char *name; uint32_t fb; } row_e_core[] = {
        { "AE01", 2 }, { "AE02", 3 }, { "AE03", 4 }, { "AE04", 5 },
        { "AE05", 6 }, { "AE06", 7 }, { "AE07", 8 }, { "AE08", 9 },
        { "AE09", 10 }, { "AE10", 11 }, { "AE11", 12 }
    };
    static const struct { const char *name; uint32_t fb; } row_d_core[] = {
        { "AD01", 16 }, { "AD02", 17 }, { "AD03", 18 }, { "AD04", 19 },
        { "AD05", 20 }, { "AD06", 21 }, { "AD07", 22 }, { "AD08", 23 },
        { "AD09", 24 }, { "AD10", 25 }
    };
    static const struct { const char *name; uint32_t fb; } row_c_core[] = {
        { "AC01", 30 }, { "AC02", 31 }, { "AC03", 32 }, { "AC04", 33 },
        { "AC05", 34 }, { "AC06", 35 }, { "AC07", 36 }, { "AC08", 37 },
        { "AC09", 38 }, { "AC10", 39 }, { "AC11", 40 }
    };
    static const struct { const char *name; uint32_t fb; } row_b_core[] = {
        { "AB01", 44 }, { "AB02", 45 }, { "AB03", 46 }, { "AB04", 47 },
        { "AB05", 48 }, { "AB06", 49 }, { "AB07", 50 }, { "AB08", 51 },
        { "AB09", 52 }, { "AB10", 53 }
    };

    /* Row 0: optional TLDE + AE01..AE11 + optional AE12 */
    uint32_t tlde = resolve_evdev_by_name(self->keymap, "TLDE", 41);
    if (is_alpha_key(state_base, state_shift, tlde)) {
        row_spec_push(&out_layout->rows[0], tlde);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(row_e_core); i++) {
        row_spec_push(&out_layout->rows[0],
                      resolve_evdev_by_name(self->keymap, row_e_core[i].name, row_e_core[i].fb));
    }
    uint32_t ae12 = resolve_evdev_by_name(self->keymap, "AE12", 13);
    if (is_alpha_key(state_base, state_shift, ae12)) {
        row_spec_push(&out_layout->rows[0], ae12);
    }

    /* Row 1: AD01..AD10 + optional AD11/AD12 */
    for (size_t i = 0; i < G_N_ELEMENTS(row_d_core); i++) {
        row_spec_push(&out_layout->rows[1],
                      resolve_evdev_by_name(self->keymap, row_d_core[i].name, row_d_core[i].fb));
    }
    uint32_t ad11 = resolve_evdev_by_name(self->keymap, "AD11", 26);
    uint32_t ad12 = resolve_evdev_by_name(self->keymap, "AD12", 27);
    gboolean ad11_alpha = is_alpha_key(state_base, state_shift, ad11);
    gboolean ad12_alpha = is_alpha_key(state_base, state_shift, ad12);
    if (ad11_alpha || ad12_alpha) {
        row_spec_push(&out_layout->rows[1], ad11);
    }
    if (ad12_alpha) {
        row_spec_push(&out_layout->rows[1], ad12);
    }

    /* Row 2: optional LSGT + AC01..AC11 + optional BKSL */
    uint32_t lsgt = resolve_evdev_by_name(self->keymap, "LSGT", 86);
    uint32_t ab08 = resolve_evdev_by_name(self->keymap, "AB08", 51);
    uint32_t ab09 = resolve_evdev_by_name(self->keymap, "AB09", 52);
    uint32_t ab10 = resolve_evdev_by_name(self->keymap, "AB10", 53);
    gboolean ab_has_lt = has_ascii_char(state_base, state_shift, ab08, '<') ||
                         has_ascii_char(state_base, state_shift, ab09, '<') ||
                         has_ascii_char(state_base, state_shift, ab10, '<');
    if (is_alpha_key(state_base, state_shift, lsgt) ||
        (has_ascii_char(state_base, state_shift, lsgt, '<') && !ab_has_lt)) {
        row_spec_push(&out_layout->rows[2], lsgt);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(row_c_core); i++) {
        row_spec_push(&out_layout->rows[2],
                      resolve_evdev_by_name(self->keymap, row_c_core[i].name, row_c_core[i].fb));
    }
    uint32_t bksl = resolve_evdev_by_name(self->keymap, "BKSL", 43);
    if (is_alpha_key(state_base, state_shift, bksl)) {
        row_spec_push(&out_layout->rows[2], bksl);
    }

    /* Row 3: AB01..AB10 */
    for (size_t i = 0; i < G_N_ELEMENTS(row_b_core); i++) {
        row_spec_push(&out_layout->rows[3],
                      resolve_evdev_by_name(self->keymap, row_b_core[i].name, row_b_core[i].fb));
    }

    if (state_base) xkb_state_unref(state_base);
    if (state_shift) xkb_state_unref(state_shift);
}
