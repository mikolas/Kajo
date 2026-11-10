/* comp_binds.c — see comp_binds.h. */
#include "comp_binds.h"

#include "../../vendor/tomlc17/tomlc17.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xkbcommon/xkbcommon.h>

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
    { "close-window",      COMP_BIND_CLOSE_WINDOW },
    { "fullscreen-window", COMP_BIND_FULLSCREEN_WINDOW },
    { "next-window",       COMP_BIND_NEXT_WINDOW },
    { "previous-window",   COMP_BIND_PREVIOUS_WINDOW },
    { "set-column-width",  COMP_BIND_SET_COLUMN_WIDTH },
    { "set-window-height", COMP_BIND_SET_WINDOW_HEIGHT },
    { "quit",              COMP_BIND_QUIT },
};

static const char *const k_planned_actions[] = {
    "toggle-overview",                  /* spec §3.3.6 */
    "show-hotkey-overlay",              /* needs text rendering, §3.4.3 */
    "screenshot", "screenshot-window", "screenshot-screen",   /* use kajo-capture via spawn, §3.5 */
    "expand-column-to-available-width",
    "center-visible-columns",
};

const char *
comp_bind_kind_name(CompBindKind kind)
{
    for (size_t i = 0; i < sizeof(k_comp_actions) / sizeof(k_comp_actions[0]); i++)
        if (k_comp_actions[i].kind == kind)
            return k_comp_actions[i].name;
    return kind == COMP_BIND_LAYOUT ? "layout" : (kind == COMP_BIND_SPAWN ? "spawn" : NULL);
}

static void
bind_clear(CompBind *b)
{
    free(b->spec);
    if (b->argv) {
        for (char **a = b->argv; *a; a++)
            free(*a);
        free(b->argv);
    }
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
        { "Shift", COMP_MOD_SHIFT }, { "Ctrl", COMP_MOD_CTRL }, { "Control", COMP_MOD_CTRL },
        { "Alt", COMP_MOD_ALT }, { "Super", COMP_MOD_SUPER }, { "Win", COMP_MOD_SUPER },
        { "Logo", COMP_MOD_SUPER },
    };
    if (n == 3 && strncasecmp(s, "Mod", 3) == 0) {
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
        { "WheelScrollUp", COMP_TRIGGER_WHEEL_UP }, { "WheelScrollDown", COMP_TRIGGER_WHEEL_DOWN },
        { "WheelScrollLeft", COMP_TRIGGER_WHEEL_LEFT }, { "WheelScrollRight", COMP_TRIGGER_WHEEL_RIGHT },
    };
    for (size_t i = 0; i < sizeof(k_wheel) / sizeof(k_wheel[0]); i++) {
        if (strcasecmp(s, k_wheel[i].name) == 0) {
            *out = k_wheel[i].t;
            return true;
        }
    }
    return false;
}

static const char *
trigger_name(CompTrigger t)
{
    switch (t) {
    case COMP_TRIGGER_WHEEL_UP:    return "WheelScrollUp";
    case COMP_TRIGGER_WHEEL_DOWN:  return "WheelScrollDown";
    case COMP_TRIGGER_WHEEL_LEFT:  return "WheelScrollLeft";
    case COMP_TRIGGER_WHEEL_RIGHT: return "WheelScrollRight";
    case COMP_TRIGGER_KEY:
    default:                       return NULL;
    }
}

static char *
normalized_spec(uint32_t mods, CompTrigger trigger, uint32_t keysym)
{
    char buf[160] = "";
    if (mods & COMP_MOD_SUPER) strcat(buf, "Super+");
    if (mods & COMP_MOD_CTRL)  strcat(buf, "Ctrl+");
    if (mods & COMP_MOD_ALT)   strcat(buf, "Alt+");
    if (mods & COMP_MOD_SHIFT) strcat(buf, "Shift+");
    const char *w = trigger_name(trigger);
    if (w) {
        strcat(buf, w);
    } else {
        char name[64];
        if (xkb_keysym_get_name(keysym, name, sizeof(name)) < 0)
            snprintf(name, sizeof(name), "0x%x", keysym);
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
        if (n == 0) {
            /* "Mod++" style: a literal '+' key is written as "plus" */
            return "empty modifier (write the '+' key as \"plus\")";
        }
        uint32_t m;
        if (!mod_from_name(p, n, mod_meaning, &m))
            return "unknown modifier (use Mod, Super, Ctrl, Shift or Alt)";
        if (*mods & m)
            return "modifier given twice";
        *mods |= m;
        p = plus + 1;
    }
    if (!*p)
        return "missing key after the last '+'";
    if (wheel_from_name(p, trigger))
        return NULL;
    xkb_keysym_t sym = xkb_keysym_from_name(p, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol)
        sym = xkb_keysym_from_name(p, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol)
        return "unknown key name (expected an xkb keysym name such as 'h', 'Return', 'Print', 'XF86AudioMute')";
    *keysym = xkb_keysym_to_lower(sym);
    return NULL;
}

/* ───────────────────────── value parsing ───────────────────────── */

static bool
datum_str_eq(toml_datum_t d, const char *s)
{
    return d.type == TOML_STRING && strcmp(d.u.s, s) == 0;
}

/* "+10%", "-10%", "0.1", "-0.25" → proportion. Returns false on bad syntax. */
static bool
parse_delta(const char *s, double *out)
{
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || errno != 0 || !isfinite(v))
        return false;
    while (*end == ' ')
        end++;
    if (*end == '%') {
        v /= 100.0;
        end++;
    }
    while (*end == ' ')
        end++;
    if (*end != '\0' || fabs(v) > 1.0)
        return false;
    *out = v;
    return true;
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
resolve_action(const char *name, CompBind *b, const char *src, int line, CompDiagList *diag)
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
                          b->spec, name);
            return false;
        }
    }
    comp_diag_add(diag, src, line, "'%s': unknown action '%s'", b->spec, name);
    return false;
}

static const char *const k_known_options[] = {
    "action", "spawn", "repeat", "cooldown_ms", "allow_when_locked", "delta", "scope", "filter",
};

/* Fills `b` from the TOML value. On failure returns false and writes a message to diag. */
static bool
parse_bind_value(toml_datum_t v, CompBind *b, const char *src, CompDiagList *diag)
{
    const char *err = NULL;
    int line = v.lineno;
    if (v.type == TOML_STRING) {
        if (!resolve_action(v.u.s, b, src, line, diag))
            return false;
        if (b->kind == COMP_BIND_SET_COLUMN_WIDTH || b->kind == COMP_BIND_SET_WINDOW_HEIGHT) {
            comp_diag_add(diag, src, line,
                          "'%s': '%s' needs an inline table with a delta, e.g. { action = \"%s\", delta = \"+10%%\" }",
                          b->spec, v.u.s, v.u.s);
            return false;
        }
        return true;
    }
    if (v.type != TOML_TABLE) {
        comp_diag_add(diag, src, line, "'%s': value must be an action name or an inline table", b->spec);
        return false;
    }

    for (int i = 0; i < v.u.tab.size; i++) {
        bool known = false;
        for (size_t k = 0; k < sizeof(k_known_options) / sizeof(k_known_options[0]); k++)
            if (strcmp(v.u.tab.key[i], k_known_options[k]) == 0)
                known = true;
        if (!known)
            comp_diag_add(diag, src, line, "'%s': unknown option '%s' ignored", b->spec, v.u.tab.key[i]);
    }

    toml_datum_t action = toml_get(v, "action");
    toml_datum_t spawn  = toml_get(v, "spawn");
    if ((action.type != TOML_UNKNOWN) == (spawn.type != TOML_UNKNOWN)) {
        comp_diag_add(diag, src, line, "'%s': give exactly one of 'action' or 'spawn'", b->spec);
        return false;
    }
    if (spawn.type != TOML_UNKNOWN) {
        b->kind = COMP_BIND_SPAWN;
        b->argv = parse_argv(spawn, &err);
        if (!b->argv) {
            comp_diag_add(diag, src, line, "'%s': %s", b->spec, err);
            return false;
        }
    } else {
        if (action.type != TOML_STRING) {
            comp_diag_add(diag, src, line, "'%s': 'action' must be a string", b->spec);
            return false;
        }
        if (!resolve_action(action.u.s, b, src, line, diag))
            return false;
    }

    toml_datum_t repeat = toml_get(v, "repeat");
    if (repeat.type == TOML_BOOLEAN)
        b->repeat = repeat.u.boolean;
    else if (repeat.type != TOML_UNKNOWN)
        comp_diag_add(diag, src, line, "'%s': 'repeat' must be true or false; using true", b->spec);

    toml_datum_t locked = toml_get(v, "allow_when_locked");
    if (locked.type == TOML_BOOLEAN)
        b->allow_when_locked = locked.u.boolean;
    else if (locked.type != TOML_UNKNOWN)
        comp_diag_add(diag, src, line, "'%s': 'allow_when_locked' must be true or false; using false", b->spec);

    toml_datum_t cool = toml_get(v, "cooldown_ms");
    if (cool.type == TOML_INT64) {
        int64_t c = cool.u.int64;
        if (c < 0 || c > 10000) {
            comp_diag_add(diag, src, line, "'%s': cooldown_ms %lld out of range 0..10000; clamped",
                          b->spec, (long long)c);
            c = c < 0 ? 0 : 10000;
        }
        b->cooldown_ms = (int)c;
    } else if (cool.type != TOML_UNKNOWN) {
        comp_diag_add(diag, src, line, "'%s': 'cooldown_ms' must be an integer; ignored", b->spec);
    }

    toml_datum_t delta = toml_get(v, "delta");
    bool wants_delta = b->kind == COMP_BIND_SET_COLUMN_WIDTH || b->kind == COMP_BIND_SET_WINDOW_HEIGHT;
    if (wants_delta) {
        if (delta.type != TOML_STRING || !parse_delta(delta.u.s, &b->delta)) {
            comp_diag_add(diag, src, line,
                          "'%s': 'delta' must be a string such as \"+10%%\", \"-10%%\" or \"0.1\" (at most 100%%)",
                          b->spec);
            return false;
        }
    } else if (delta.type != TOML_UNKNOWN) {
        comp_diag_add(diag, src, line, "'%s': 'delta' only applies to set-column-width / set-window-height; ignored",
                      b->spec);
    }

    bool switcher = b->kind == COMP_BIND_NEXT_WINDOW || b->kind == COMP_BIND_PREVIOUS_WINDOW;
    toml_datum_t scope = toml_get(v, "scope");
    if (scope.type != TOML_UNKNOWN) {
        if (!switcher)
            comp_diag_add(diag, src, line, "'%s': 'scope' only applies to next-window / previous-window; ignored", b->spec);
        else if (datum_str_eq(scope, "all"))
            b->scope = COMP_SWITCH_SCOPE_ALL;
        else if (datum_str_eq(scope, "output"))
            b->scope = COMP_SWITCH_SCOPE_OUTPUT;
        else if (datum_str_eq(scope, "workspace"))
            b->scope = COMP_SWITCH_SCOPE_WORKSPACE;
        else
            comp_diag_add(diag, src, line, "'%s': scope must be \"all\", \"output\" or \"workspace\"; using \"all\"", b->spec);
    }
    toml_datum_t filter = toml_get(v, "filter");
    if (filter.type != TOML_UNKNOWN) {
        if (!switcher)
            comp_diag_add(diag, src, line, "'%s': 'filter' only applies to next-window / previous-window; ignored", b->spec);
        else if (datum_str_eq(filter, "app-id"))
            b->same_app = true;
        else
            comp_diag_add(diag, src, line, "'%s': filter must be \"app-id\"; ignored", b->spec);
    }
    return true;
}

/* ───────────────────────── top level ───────────────────────── */

static bool
same_trigger(const CompBind *a, uint32_t mods, CompTrigger t, uint32_t keysym)
{
    return a->mods == mods && a->trigger == t && (t != COMP_TRIGGER_KEY || a->keysym == keysym);
}

CompBinds *
comp_binds_parse(const char *src, size_t len, const char *source_name,
                 uint32_t mod_meaning, CompDiagList *diag)
{
    const char *name = source_name ? source_name : "binds.toml";
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
        if (strcmp(res.toptab.u.tab.key[i], "binds") != 0)
            comp_diag_add(diag, name, res.toptab.u.tab.value[i].lineno,
                          "unknown top-level key '%s' ignored (expected [binds])", res.toptab.u.tab.key[i]);
    }
    toml_datum_t tab = toml_get(res.toptab, "binds");
    if (tab.type != TOML_UNKNOWN && tab.type != TOML_TABLE)
        comp_diag_add(diag, name, tab.lineno, "'binds' must be a table");

    for (int i = 0; tab.type == TOML_TABLE && i < tab.u.tab.size; i++) {
        const char *spec = tab.u.tab.key[i];
        toml_datum_t val = tab.u.tab.value[i];
        CompBind b = { .repeat = true, .line = val.lineno };
        const char *err = parse_key_spec(spec, mod_meaning, &b.mods, &b.trigger, &b.keysym);
        if (err) {
            comp_diag_add(diag, name, val.lineno, "'%s': %s", spec, err);
            continue;
        }
        b.spec = normalized_spec(b.mods, b.trigger, b.keysym);
        if (!b.spec) {
            comp_diag_add(diag, name, val.lineno, "out of memory");
            continue;
        }
        if (!parse_bind_value(val, &b, name, diag)) {
            bind_clear(&b);
            continue;
        }
        bool dup = false;
        for (size_t k = 0; k < binds->n; k++) {
            if (same_trigger(&binds->items[k], b.mods, b.trigger, b.keysym)) {
                comp_diag_add(diag, name, val.lineno, "'%s' is the same key as line %d (%s); keeping the first",
                              spec, binds->items[k].line, binds->items[k].spec);
                dup = true;
                break;
            }
        }
        if (dup) {
            bind_clear(&b);
            continue;
        }
        if (binds->n == binds->cap) {
            size_t ncap = binds->cap ? binds->cap * 2 : 32;
            CompBind *p = realloc(binds->items, ncap * sizeof(CompBind));
            if (!p) {
                bind_clear(&b);
                comp_diag_add(diag, name, val.lineno, "out of memory");
                continue;
            }
            binds->items = p;
            binds->cap   = ncap;
        }
        binds->items[binds->n++] = b;
    }
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
