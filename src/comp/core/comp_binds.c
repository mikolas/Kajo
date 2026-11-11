/* comp_binds.c — see comp_binds.h. */
#include "comp_binds.h"

#include "comp_schema_keys.h"
#include "../../vendor/tomlc17/tomlc17.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct CompBinds {
    CompBind *items;
    size_t    n, cap;
};

/* Compositor-level action names. Planned-but-not-implemented names are listed
 * separately so the user gets "not available yet" instead of "unknown". */
static const struct {
    const char  *name;
    CompBindKind kind;
} k_comp_actions[] = {
    { "close-window",                     COMP_BIND_CLOSE_WINDOW },
    { "fullscreen-window",                COMP_BIND_FULLSCREEN_WINDOW },
    { "next-window",                      COMP_BIND_NEXT_WINDOW },
    { "previous-window",                  COMP_BIND_PREVIOUS_WINDOW },
    { "set-column-width",                 COMP_BIND_SET_COLUMN_WIDTH },
    { "set-window-height",                COMP_BIND_SET_WINDOW_HEIGHT },
    { "break-keyboard-shortcuts-inhibit", COMP_BIND_BREAK_SHORTCUTS_INHIBIT },
    { "reload-config",                    COMP_BIND_RELOAD_CONFIG },
    { "quit",                             COMP_BIND_QUIT },
    { "screenshot",                       COMP_BIND_SCREENSHOT },
    { "screenshot-window",                COMP_BIND_SCREENSHOT_WINDOW },
    { "screenshot-screen",                COMP_BIND_SCREENSHOT_SCREEN },
};

static const char *const k_planned_actions[] = {
    "toggle-overview",
    "show-hotkey-overlay",
};

const char *
comp_bind_kind_name(CompBindKind kind)
{
    for (size_t i = 0; i < sizeof(k_comp_actions) / sizeof(k_comp_actions[0]); i++)
        if (k_comp_actions[i].kind == kind)
            return k_comp_actions[i].name;
    return kind == COMP_BIND_LAYOUT ? "layout" : (kind == COMP_BIND_SPAWN ? COMP_KEY_SPAWN : NULL);
}

bool
comp_bind_kind_from_name(const char *name, CompBindKind *out)
{
    if (!name || !out)
        return false;
    for (size_t i = 0; i < sizeof(k_comp_actions) / sizeof(k_comp_actions[0]); i++) {
        if (strcmp(k_comp_actions[i].name, name) == 0) {
            *out = k_comp_actions[i].kind;
            return true;
        }
    }
    if (strcmp(name, COMP_IPC_ACT_SWITCH_WINDOW_NEXT) == 0) {
        *out = COMP_BIND_NEXT_WINDOW;
        return true;
    }
    if (strcmp(name, COMP_IPC_ACT_SWITCH_WINDOW_PREV) == 0) {
        *out = COMP_BIND_PREVIOUS_WINDOW;
        return true;
    }
    if (strcmp(name, COMP_KEY_SPAWN) == 0) {
        *out = COMP_BIND_SPAWN;
        return true;
    }
    return false;
}

static const struct {
    const char     *name;
    CompSwitchScope scope;
} k_switch_scopes[] = {
    { "all",       COMP_SWITCH_SCOPE_ALL },
    { "output",    COMP_SWITCH_SCOPE_OUTPUT },
    { "workspace", COMP_SWITCH_SCOPE_WORKSPACE },
};

bool
comp_switch_scope_from_str(const char *s, CompSwitchScope *out)
{
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(k_switch_scopes) / sizeof(k_switch_scopes[0]); i++) {
        if (strcmp(s, k_switch_scopes[i].name) == 0) {
            *out = k_switch_scopes[i].scope;
            return true;
        }
    }
    return false;
}

const char *
comp_switch_scope_to_str(CompSwitchScope scope)
{
    for (size_t i = 0; i < sizeof(k_switch_scopes) / sizeof(k_switch_scopes[0]); i++) {
        if (k_switch_scopes[i].scope == scope)
            return k_switch_scopes[i].name;
    }
    return "all";
}

static void
free_argv(char **argv)
{
    if (!argv)
        return;
    for (char **a = argv; *a; a++)
        free(*a);
    free(argv);
}

static char **
clone_argv(char *const *src)
{
    if (!src)
        return NULL;
    size_t n = 0;
    while (src[n])
        n++;
    char **dst = calloc(n + 1, sizeof(char *));
    if (!dst)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        dst[i] = strdup(src[i]);
        if (!dst[i]) {
            free_argv(dst);
            return NULL;
        }
    }
    return dst;
}

static void
bind_clear(CompBind *b)
{
    free(b->spec);
    free_argv(b->argv);
    memset(b, 0, sizeof(*b));
}

void
comp_binds_free(CompBinds *binds)
{
    if (!binds)
        return;
    for (size_t i = 0; i < binds->n; i++)
        bind_clear(&binds->items[i]);
    free(binds->items);
    free(binds);
}

size_t
comp_binds_count(const CompBinds *binds)
{
    return binds ? binds->n : 0;
}

const CompBind *
comp_binds_get(const CompBinds *binds, size_t index)
{
    return (binds && index < binds->n) ? &binds->items[index] : NULL;
}

/* ───────────────────────── key spec parsing ───────────────────────── */

static bool
mod_from_name(const char *s, size_t n, uint32_t mod_meaning, uint32_t *out)
{
    static const struct { const char *name; uint32_t mask; } k_mods[] = {
        { "shift", COMP_MOD_SHIFT }, { "ctrl", COMP_MOD_CTRL }, { "control", COMP_MOD_CTRL },
        { "alt", COMP_MOD_ALT }, { "super", COMP_MOD_SUPER }, { "win", COMP_MOD_SUPER },
        { "logo", COMP_MOD_SUPER },
    };
    if (n == 3 && strncasecmp(s, "mod", 3) == 0) {
        *out = mod_meaning;
        return true;
    }
    for (size_t i = 0; i < sizeof(k_mods) / sizeof(k_mods[0]); i++) {
        if (strlen(k_mods[i].name) == n && strncasecmp(s, k_mods[i].name, n) == 0) {
            *out = k_mods[i].mask;
            return true;
        }
    }
    return false;
}

static bool
wheel_from_name(const char *s, CompTrigger *out)
{
    static const struct { const char *name; CompTrigger t; } k_wheel[] = {
        { "wheel-up", COMP_TRIGGER_WHEEL_UP },
        { "wheel-down", COMP_TRIGGER_WHEEL_DOWN },
        { "wheel-left", COMP_TRIGGER_WHEEL_LEFT },
        { "wheel-right", COMP_TRIGGER_WHEEL_RIGHT },
        { "WheelScrollUp", COMP_TRIGGER_WHEEL_UP },
        { "WheelScrollDown", COMP_TRIGGER_WHEEL_DOWN },
        { "WheelScrollLeft", COMP_TRIGGER_WHEEL_LEFT },
        { "WheelScrollRight", COMP_TRIGGER_WHEEL_RIGHT },
    };
    for (size_t i = 0; i < sizeof(k_wheel) / sizeof(k_wheel[0]); i++) {
        if (strcasecmp(s, k_wheel[i].name) == 0) {
            *out = k_wheel[i].t;
            return true;
        }
    }
    return false;
}

static bool
alias_keysym_from_name(const char *s, xkb_keysym_t *out)
{
    static const struct {
        const char  *alias;
        xkb_keysym_t sym;
    } k_aliases[] = {
        { "volume-up",       XKB_KEY_XF86AudioRaiseVolume },
        { "volume-down",     XKB_KEY_XF86AudioLowerVolume },
        { "volume-mute",     XKB_KEY_XF86AudioMute },
        { "mic-mute",        XKB_KEY_XF86AudioMicMute },
        { "brightness-up",   XKB_KEY_XF86MonBrightnessUp },
        { "brightness-down", XKB_KEY_XF86MonBrightnessDown },
        { "media-play",      XKB_KEY_XF86AudioPlay },
        { "media-next",      XKB_KEY_XF86AudioNext },
        { "media-prev",      XKB_KEY_XF86AudioPrev },
        { "page-up",         XKB_KEY_Page_Up },
        { "page-down",       XKB_KEY_Page_Down },
        { "bracket-left",    XKB_KEY_bracketleft },
        { "bracket-right",   XKB_KEY_bracketright },
    };
    for (size_t i = 0; i < sizeof(k_aliases) / sizeof(k_aliases[0]); i++) {
        if (strcasecmp(s, k_aliases[i].alias) == 0) {
            *out = k_aliases[i].sym;
            return true;
        }
    }
    return false;
}

static const char *
trigger_name(CompTrigger t)
{
    switch (t) {
    case COMP_TRIGGER_WHEEL_UP:    return "wheel-up";
    case COMP_TRIGGER_WHEEL_DOWN:  return "wheel-down";
    case COMP_TRIGGER_WHEEL_LEFT:  return "wheel-left";
    case COMP_TRIGGER_WHEEL_RIGHT: return "wheel-right";
    case COMP_TRIGGER_KEY:
    default:                       return NULL;
    }
}

static char *
normalized_spec(uint32_t mods, CompTrigger trigger, uint32_t keysym)
{
    char buf[160] = "";
    if (mods & COMP_MOD_SUPER) strcat(buf, "super+");
    if (mods & COMP_MOD_CTRL)  strcat(buf, "ctrl+");
    if (mods & COMP_MOD_ALT)   strcat(buf, "alt+");
    if (mods & COMP_MOD_SHIFT) strcat(buf, "shift+");
    const char *w = trigger_name(trigger);
    if (w) {
        strcat(buf, w);
    } else {
        char name[64];
        if (xkb_keysym_get_name(keysym, name, sizeof(name)) < 0)
            snprintf(name, sizeof(name), "0x%x", keysym);
        for (char *p = name; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        strncat(buf, name, sizeof(buf) - strlen(buf) - 1);
    }
    return strdup(buf);
}

/* Returns an error message (static) or NULL on success. */
static const char *
parse_key_spec(const char *spec, uint32_t mod_meaning,
               uint32_t *mods, CompTrigger *trigger, uint32_t *keysym)
{
    *mods = 0;
    *trigger = COMP_TRIGGER_KEY;
    *keysym = 0;
    const char *p = spec;
    if (!*p)
        return "empty key";
    for (;;) {
        const char *plus = strchr(p, '+');
        if (!plus)
            break;
        size_t n = (size_t)(plus - p);
        if (n == 0)
            return "empty modifier (write the '+' key as \"plus\")";
        uint32_t m;
        if (!mod_from_name(p, n, mod_meaning, &m))
            return "unknown modifier (use mod, super, ctrl, shift or alt)";
        if (*mods & m)
            return "modifier given twice";
        *mods |= m;
        p = plus + 1;
    }
    if (!*p)
        return "missing key after the last '+'";
    if (wheel_from_name(p, trigger))
        return NULL;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    if (!alias_keysym_from_name(p, &sym)) {
        sym = xkb_keysym_from_name(p, XKB_KEYSYM_NO_FLAGS);
        if (sym == XKB_KEY_NoSymbol)
            sym = xkb_keysym_from_name(p, XKB_KEYSYM_CASE_INSENSITIVE);
    }
    if (sym == XKB_KEY_NoSymbol)
        return "unknown key name (expected an xkb keysym name or alias such as 'h', 'return', 'print', 'volume-mute')";
    *keysym = xkb_keysym_to_lower(sym);
    return NULL;
}

/* ───────────────────────── value parsing ───────────────────────── */

static bool
datum_str_eq(toml_datum_t d, const char *s)
{
    return d.type == TOML_STRING && strcmp(d.u.s, s) == 0;
}

/* "+10%", "-10%", "50%", "+20", "-20", "800", "0.1" → delta + mode (spec §3.7 W4). */
bool
comp_binds_parse_delta(const char *s, double *out, CompDeltaMode *mode_out)
{
    if (!s || !out || !mode_out)
        return false;
    while (*s == ' ')
        s++;
    bool has_sign = (*s == '+' || *s == '-');
    bool has_dot  = (strchr(s, '.') != NULL);
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || errno != 0 || !isfinite(v))
        return false;
    while (*end == ' ')
        end++;
    bool is_pct = false;
    bool is_px  = false;
    if (*end == '%') {
        v /= 100.0;
        is_pct = true;
        end++;
    } else if (strncasecmp(end, "px", 2) == 0) {
        is_px = true;
        end += 2;
    }
    while (*end == ' ')
        end++;
    if (*end != '\0')
        return false;
    if (is_pct || (!is_px && has_dot && fabs(v) <= 1.0)) {
        if (fabs(v) > 1.0 || (!has_sign && v <= 0.0))
            return false;
        *out = v;
        *mode_out = (has_sign || has_dot) ? COMP_DELTA_REL_PROP : COMP_DELTA_SET_PROP;
        return true;
    }
    /* Pixel delta or pixel set size */
    if (fabs(v) > 10000.0 || (!has_sign && v < 1.0))
        return false;
    *out = v;
    *mode_out = has_sign ? COMP_DELTA_REL_PX : COMP_DELTA_SET_PX;
    return true;
}

static bool
parse_delta(const char *s, double *out, CompDeltaMode *mode_out)
{
    return comp_binds_parse_delta(s, out, mode_out);
}

static char **
parse_argv(toml_datum_t arr, const char **err)
{
    if (arr.type == TOML_STRING) {
        *err = "spawn takes an array of strings, e.g. [\"kitty\", \"-e\", \"htop\"] (no shell is used)";
        return NULL;
    }
    if (arr.type != TOML_ARRAY || arr.u.arr.size <= 0) {
        *err = "spawn must be a non-empty array of strings";
        return NULL;
    }
    size_t n = (size_t)arr.u.arr.size;
    char **argv = calloc(n + 1, sizeof(char *));
    if (!argv) {
        *err = "out of memory";
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        toml_datum_t e = arr.u.arr.elem[i];
        if (e.type != TOML_STRING || (i == 0 && e.u.str.len == 0)) {
            *err = "spawn array must contain only strings, and the program name must not be empty";
            for (size_t k = 0; k < i; k++)
                free(argv[k]);
            free(argv);
            return NULL;
        }
        argv[i] = strdup(e.u.s);
        if (!argv[i]) {
            *err = "out of memory";
            for (size_t k = 0; k < i; k++)
                free(argv[k]);
            free(argv);
            return NULL;
        }
    }
    return argv;
}

/* Resolve an action name into b->kind / b->layout_action. On failure writes
 * a complete message to diag and returns false. */
static bool
resolve_action(const char *name, CompBind *b, const char *label, const char *src, int line, CompDiagList *diag)
{
    CompAction a;
    if (comp_action_from_name(name, &a)) {
        b->kind = COMP_BIND_LAYOUT;
        b->layout_action = a;
        return true;
    }
    for (size_t i = 0; i < sizeof(k_comp_actions) / sizeof(k_comp_actions[0]); i++) {
        if (strcmp(k_comp_actions[i].name, name) == 0) {
            b->kind = k_comp_actions[i].kind;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(k_planned_actions) / sizeof(k_planned_actions[0]); i++) {
        if (strcmp(k_planned_actions[i], name) == 0) {
            comp_diag_add(diag, src, line, "'%s': action '%s' is planned but not available in this version",
                          label, name);
            return false;
        }
    }
    comp_diag_add(diag, src, line, "'%s': unknown action '%s'", label, name);
    return false;
}

static const char *const k_known_options[] = {
    COMP_KEY_KEY, COMP_KEY_KEYS, COMP_KEY_ACTION, COMP_KEY_SPAWN, COMP_KEY_REPEAT,
    COMP_KEY_COOLDOWN_MS, COMP_KEY_ALLOW_LOCKED, COMP_KEY_ALLOW_INHIBITING,
    COMP_KEY_DELTA, COMP_KEY_SCOPE, COMP_KEY_FILTER,
};

static bool
same_trigger(const CompBind *a, uint32_t mods, CompTrigger t, uint32_t keysym)
{
    return a->mods == mods && a->trigger == t && (t != COMP_TRIGGER_KEY || a->keysym == keysym);
}

static void
remove_bind_for_spec(CompBinds *binds, const char *raw_spec, int line,
                     uint32_t mod_meaning, const char *src, CompDiagList *diag)
{
    uint32_t mods = 0, keysym = 0;
    CompTrigger trigger = COMP_TRIGGER_KEY;
    const char *err = parse_key_spec(raw_spec, mod_meaning, &mods, &trigger, &keysym);
    if (err) {
        comp_diag_add(diag, src, line, "'%s': %s", raw_spec, err);
        return;
    }
    for (size_t k = 0; k < binds->n; k++) {
        if (same_trigger(&binds->items[k], mods, trigger, keysym)) {
            bind_clear(&binds->items[k]);
            if (k + 1 < binds->n)
                memmove(&binds->items[k], &binds->items[k + 1], (binds->n - k - 1) * sizeof(CompBind));
            binds->n--;
            return;
        }
    }
}

static void
append_bind_for_spec(CompBinds *binds, const CompBind *proto, const char *raw_spec,
                     int line, uint32_t mod_meaning, bool replace_existing,
                     const char *src, CompDiagList *diag)
{
    CompBind b = *proto;
    b.spec = NULL;
    b.argv = NULL;
    b.line = line;

    const char *err = parse_key_spec(raw_spec, mod_meaning, &b.mods, &b.trigger, &b.keysym);
    if (err) {
        comp_diag_add(diag, src, line, "'%s': %s", raw_spec, err);
        return;
    }
    b.spec = normalized_spec(b.mods, b.trigger, b.keysym);
    if (!b.spec) {
        comp_diag_add(diag, src, line, "out of memory");
        return;
    }
    if (proto->argv) {
        b.argv = clone_argv(proto->argv);
        if (!b.argv) {
            free(b.spec);
            comp_diag_add(diag, src, line, "out of memory");
            return;
        }
    }
    for (size_t k = 0; k < binds->n; k++) {
        if (same_trigger(&binds->items[k], b.mods, b.trigger, b.keysym)) {
            if (replace_existing) {
                bind_clear(&binds->items[k]);
                binds->items[k] = b;
                return;
            }
            comp_diag_add(diag, src, line, "'%s' is the same key as line %d (%s); keeping the first",
                          raw_spec, binds->items[k].line, binds->items[k].spec);
            bind_clear(&b);
            return;
        }
    }
    if (binds->n == binds->cap) {
        size_t ncap = binds->cap ? binds->cap * 2 : 32;
        CompBind *p = realloc(binds->items, ncap * sizeof(CompBind));
        if (!p) {
            bind_clear(&b);
            comp_diag_add(diag, src, line, "out of memory");
            return;
        }
        binds->items = p;
        binds->cap   = ncap;
    }
    binds->items[binds->n++] = b;
}

/* Parses one [[bind]] table entry and appends 1..N triggers into `binds`. */
static void
parse_bind_entry(CompBinds *binds, toml_datum_t v, uint32_t mod_meaning,
                 bool replace_existing, const char *src, CompDiagList *diag)
{
    int line = v.lineno;
    if (v.type != TOML_TABLE) {
        comp_diag_add(diag, src, line, "[[bind]] entry must be a table; ignored");
        return;
    }

    toml_datum_t key_d  = toml_get(v, COMP_KEY_KEY);
    toml_datum_t keys_d = toml_get(v, COMP_KEY_KEYS);
    if ((key_d.type != TOML_UNKNOWN) == (keys_d.type != TOML_UNKNOWN)) {
        comp_diag_add(diag, src, line, "[[bind]] requires exactly one of 'key' or 'keys'");
        return;
    }

    const char *label = "[[bind]]";
    if (key_d.type != TOML_UNKNOWN) {
        if (key_d.type != TOML_STRING) {
            comp_diag_add(diag, src, key_d.lineno, "'key' must be a string, e.g. \"mod+h\"");
            return;
        }
        label = key_d.u.s;
    } else {
        if (keys_d.type != TOML_ARRAY || keys_d.u.arr.size <= 0) {
            comp_diag_add(diag, src, keys_d.lineno, "'keys' must be a non-empty array of strings");
            return;
        }
        for (int i = 0; i < keys_d.u.arr.size; i++) {
            if (keys_d.u.arr.elem[i].type != TOML_STRING) {
                comp_diag_add(diag, src, keys_d.lineno, "'keys' array must contain only strings");
                return;
            }
        }
        label = keys_d.u.arr.elem[0].u.s;
    }

    for (int i = 0; i < v.u.tab.size; i++) {
        bool known = false;
        for (size_t k = 0; k < sizeof(k_known_options) / sizeof(k_known_options[0]); k++)
            if (strcmp(v.u.tab.key[i], k_known_options[k]) == 0)
                known = true;
        if (!known)
            comp_diag_add(diag, src, v.u.tab.value[i].lineno,
                          "'%s': unknown option '%s' ignored", label, v.u.tab.key[i]);
    }

    CompBind proto = { .repeat = true, .allow_inhibiting = true, .line = line };
    const char *err = NULL;

    toml_datum_t action = toml_get(v, COMP_KEY_ACTION);
    toml_datum_t spawn  = toml_get(v, COMP_KEY_SPAWN);
    if ((action.type != TOML_UNKNOWN) == (spawn.type != TOML_UNKNOWN)) {
        comp_diag_add(diag, src, line, "'%s': give exactly one of 'action' or 'spawn'", label);
        return;
    }
    if (spawn.type != TOML_UNKNOWN) {
        proto.kind = COMP_BIND_SPAWN;
        proto.argv = parse_argv(spawn, &err);
        if (!proto.argv) {
            comp_diag_add(diag, src, spawn.lineno, "'%s': %s", label, err);
            return;
        }
    } else {
        if (action.type != TOML_STRING) {
            comp_diag_add(diag, src, action.lineno, "'%s': 'action' must be a string", label);
            return;
        }
        /* Spec §5.1: `action = "none"` unbinds a previously defined chord */
        if (strcmp(action.u.s, COMP_VAL_NONE) == 0) {
            if (key_d.type != TOML_UNKNOWN) {
                remove_bind_for_spec(binds, key_d.u.s, key_d.lineno, mod_meaning, src, diag);
            } else {
                for (int i = 0; i < keys_d.u.arr.size; i++) {
                    toml_datum_t elem = keys_d.u.arr.elem[i];
                    remove_bind_for_spec(binds, elem.u.s, elem.lineno, mod_meaning, src, diag);
                }
            }
            return;
        }
        if (!resolve_action(action.u.s, &proto, label, src, action.lineno, diag))
            return;
        if (proto.kind == COMP_BIND_BREAK_SHORTCUTS_INHIBIT)
            proto.allow_inhibiting = false;
    }

    toml_datum_t repeat = toml_get(v, COMP_KEY_REPEAT);
    if (repeat.type == TOML_BOOLEAN)
        proto.repeat = repeat.u.boolean;
    else if (repeat.type != TOML_UNKNOWN)
        comp_diag_add(diag, src, repeat.lineno, "'%s': 'repeat' must be true or false; using true", label);

    toml_datum_t locked = toml_get(v, COMP_KEY_ALLOW_LOCKED);
    if (locked.type == TOML_BOOLEAN)
        proto.allow_locked = locked.u.boolean;
    else if (locked.type != TOML_UNKNOWN)
        comp_diag_add(diag, src, locked.lineno, "'%s': 'allow_locked' must be true or false; using false", label);

    toml_datum_t inhib = toml_get(v, COMP_KEY_ALLOW_INHIBITING);
    if (inhib.type == TOML_BOOLEAN)
        proto.allow_inhibiting = inhib.u.boolean;
    else if (inhib.type != TOML_UNKNOWN)
        comp_diag_add(diag, src, inhib.lineno, "'%s': 'allow_inhibiting' must be true or false", label);

    toml_datum_t cool = toml_get(v, COMP_KEY_COOLDOWN_MS);
    if (cool.type == TOML_INT64) {
        int64_t c = cool.u.int64;
        if (c < 0 || c > 10000) {
            comp_diag_add(diag, src, cool.lineno, "'%s': cooldown_ms %lld out of range 0..10000; clamped",
                          label, (long long)c);
            c = c < 0 ? 0 : 10000;
        }
        proto.cooldown_ms = (int)c;
    } else if (cool.type != TOML_UNKNOWN) {
        comp_diag_add(diag, src, cool.lineno, "'%s': 'cooldown_ms' must be an integer; ignored", label);
    }

    toml_datum_t delta = toml_get(v, COMP_KEY_DELTA);
    bool wants_delta = proto.kind == COMP_BIND_SET_COLUMN_WIDTH || proto.kind == COMP_BIND_SET_WINDOW_HEIGHT;
    if (wants_delta) {
        if (delta.type != TOML_STRING || !parse_delta(delta.u.s, &proto.delta, &proto.delta_mode)) {
            comp_diag_add(diag, src, delta.type != TOML_UNKNOWN ? delta.lineno : line,
                          "'%s': 'delta' must be a string such as \"+10%%\", \"-10%%\", \"50%%\", \"+20\" or \"800\"",
                          label);
            bind_clear(&proto);
            return;
        }
    } else if (delta.type != TOML_UNKNOWN) {
        comp_diag_add(diag, src, delta.lineno,
                      "'%s': 'delta' only applies to set-column-width / set-window-height; ignored", label);
    }

    bool switcher = proto.kind == COMP_BIND_NEXT_WINDOW || proto.kind == COMP_BIND_PREVIOUS_WINDOW;
    toml_datum_t scope = toml_get(v, COMP_KEY_SCOPE);
    if (scope.type != TOML_UNKNOWN) {
        if (!switcher)
            comp_diag_add(diag, src, scope.lineno, "'%s': 'scope' only applies to next-window / previous-window; ignored", label);
        else if (scope.type != TOML_STRING || !comp_switch_scope_from_str(scope.u.s, &proto.scope))
            comp_diag_add(diag, src, scope.lineno, "'%s': scope must be \"all\", \"output\" or \"workspace\"; using \"all\"", label);
    }
    toml_datum_t filter = toml_get(v, COMP_KEY_FILTER);
    if (filter.type != TOML_UNKNOWN) {
        if (!switcher)
            comp_diag_add(diag, src, filter.lineno, "'%s': 'filter' only applies to next-window / previous-window; ignored", label);
        else if (datum_str_eq(filter, COMP_VAL_APP_ID))
            proto.same_app = true;
        else
            comp_diag_add(diag, src, filter.lineno, "'%s': filter must be \"app-id\"; ignored", label);
    }

    if (key_d.type != TOML_UNKNOWN) {
        append_bind_for_spec(binds, &proto, key_d.u.s, key_d.lineno, mod_meaning, replace_existing, src, diag);
    } else {
        for (int i = 0; i < keys_d.u.arr.size; i++) {
            toml_datum_t elem = keys_d.u.arr.elem[i];
            append_bind_for_spec(binds, &proto, elem.u.s, elem.lineno, mod_meaning, replace_existing, src, diag);
        }
    }
    bind_clear(&proto);
}

/* ───────────────────────── top level ───────────────────────── */

CompBinds *
comp_binds_new(void)
{
    return calloc(1, sizeof(CompBinds));
}

bool
comp_binds_merge_toml(CompBinds *binds, const char *src, size_t len,
                      const char *source_name, uint32_t mod_meaning,
                      bool replace_existing, CompDiagList *diag)
{
    if (!binds)
        return false;
    const char *name = source_name ? source_name : COMP_CFG_FILE_BINDS;
    if (len > (size_t)INT_MAX) {
        comp_diag_add(diag, name, 0, "file too large");
        return false;
    }
    toml_result_t res = toml_parse_named(src ? src : "", (int)len, name);
    if (!res.ok) {
        comp_diag_add(diag, name, 0, "TOML syntax error: %s (keeping previous binds)", res.errmsg);
        toml_free(res);
        return false;
    }
    toml_datum_t arr = toml_get(res.toptab, COMP_SEC_BIND);
    if (arr.type != TOML_UNKNOWN && arr.type != TOML_ARRAY)
        comp_diag_add(diag, name, arr.lineno, "'bind' must be an array of tables ([[bind]])");

    for (int i = 0; arr.type == TOML_ARRAY && i < arr.u.arr.size; i++)
        parse_bind_entry(binds, arr.u.arr.elem[i], mod_meaning, replace_existing, name, diag);

    toml_free(res);
    return true;
}

CompBinds *
comp_binds_parse(const char *src, size_t len, const char *source_name,
                 uint32_t mod_meaning, CompDiagList *diag)
{
    const char *name = source_name ? source_name : COMP_CFG_FILE_BINDS;
    if (len > (size_t)INT_MAX) {
        comp_diag_add(diag, name, 0, "file too large");
        return NULL;
    }
    toml_result_t res = toml_parse_named(src ? src : "", (int)len, name);
    if (!res.ok) {
        comp_diag_add(diag, name, 0, "TOML syntax error: %s (keeping previous binds)", res.errmsg);
        toml_free(res);
        return NULL;
    }
    CompBinds *binds = calloc(1, sizeof(*binds));
    if (!binds) {
        toml_free(res);
        return NULL;
    }

    for (int i = 0; i < res.toptab.u.tab.size; i++) {
        if (strcmp(res.toptab.u.tab.key[i], COMP_SEC_BIND) != 0)
            comp_diag_add(diag, name, res.toptab.u.tab.value[i].lineno,
                          "unknown top-level key '%s' ignored (expected [[bind]])", res.toptab.u.tab.key[i]);
    }
    toml_datum_t arr = toml_get(res.toptab, COMP_SEC_BIND);
    if (arr.type != TOML_UNKNOWN && arr.type != TOML_ARRAY)
        comp_diag_add(diag, name, arr.lineno, "'bind' must be an array of tables ([[bind]])");

    for (int i = 0; arr.type == TOML_ARRAY && i < arr.u.arr.size; i++)
        parse_bind_entry(binds, arr.u.arr.elem[i], mod_meaning, false, name, diag);

    toml_free(res);
    return binds;
}

const CompBind *
comp_binds_lookup_key(const CompBinds *binds, uint32_t mods, uint32_t keysym)
{
    uint32_t sym = xkb_keysym_to_lower(keysym);
    for (size_t i = 0; binds && i < binds->n; i++)
        if (same_trigger(&binds->items[i], mods, COMP_TRIGGER_KEY, sym))
            return &binds->items[i];
    return NULL;
}

const CompBind *
comp_binds_lookup_wheel(const CompBinds *binds, uint32_t mods, CompTrigger wheel)
{
    if (wheel == COMP_TRIGGER_KEY)
        return NULL;
    for (size_t i = 0; binds && i < binds->n; i++)
        if (same_trigger(&binds->items[i], mods, wheel, 0))
            return &binds->items[i];
    return NULL;
}
