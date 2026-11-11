/* comp_config.c — multi-file TOML configuration loader & merger (pure C). */
#include "comp_config.h"

#include "../../vendor/tomlc17/tomlc17.h"

#include <ctype.h>
#include <dirent.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define COMP_MAX_INCLUDE_DEPTH 4
#define COMP_MAX_LOADED_FILES  64

typedef struct {
    char  *paths[COMP_MAX_LOADED_FILES];
    size_t n;
} IncludeStack;

static bool
parse_hex_rgba(const char *s, float out[4])
{
    if (!s || s[0] != '#')
        return false;
    size_t len = strlen(s + 1);
    if (len != 6 && len != 8)
        return false;
    unsigned int vals[4] = { 0, 0, 0, 255 };
    size_t nbytes = len / 2;
    for (size_t i = 0; i < nbytes; i++) {
        char hex[3] = { s[1 + i * 2], s[2 + i * 2], '\0' };
        if (!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1]))
            return false;
        vals[i] = (unsigned int)strtoul(hex, NULL, 16);
    }
    for (int i = 0; i < 4; i++)
        out[i] = (float)vals[i] / 255.0f;
    return true;
}

static char *
str_lower_dup(const char *s)
{
    if (!s)
        return strdup("");
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[len] = '\0';
    return out;
}

static void
set_str(char **dst, const char *val)
{
    char *copy = strdup(val ? val : "");
    if (!copy)
        return;
    free(*dst);
    *dst = copy;
}

static void
free_argv_list(char ***list, size_t n)
{
    if (!list)
        return;
    for (size_t i = 0; i < n; i++) {
        if (!list[i])
            continue;
        for (char **a = list[i]; *a; a++)
            free(*a);
        free(list[i]);
    }
    free(list);
}

static void
sync_accent_dependents(CompConfig *cfg)
{
    if (cfg->active_border_tracks_accent)
        memcpy(cfg->active_border_rgba, cfg->accent_rgba, sizeof(cfg->accent_rgba));
    if (cfg->switcher_active_tracks_accent)
        memcpy(cfg->switcher_active_rgba, cfg->accent_rgba, sizeof(cfg->accent_rgba));
}

CompConfig *
comp_config_new_defaults(uint32_t mod_mask)
{
    CompConfig *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        return NULL;

    cfg->mod_mask = mod_mask ? mod_mask : COMP_MOD_SUPER;
    comp_layout_config_defaults(&cfg->layout);
    comp_motion_config_defaults(&cfg->motion);
    cfg->border_px = 1;

    (void)parse_hex_rgba("#0078d4", cfg->accent_rgba);
    cfg->active_border_tracks_accent   = true;
    cfg->switcher_active_tracks_accent = true;
    sync_accent_dependents(cfg);
    (void)parse_hex_rgba("#2b2b2b", cfg->inactive_border_rgba);
    (void)parse_hex_rgba("#e81123", cfg->urgent_border_rgba);
    (void)parse_hex_rgba("#e81123", cfg->switcher_urgent_rgba);

    cfg->screenshot_path   = strdup("~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png");
    cfg->screenshot_copy   = true;
    cfg->screenshot_notify = true;
    cfg->screenshot_border = true;

    cfg->xwayland_enabled            = true;
    cfg->xwayland_force_zero_scaling = true;

    cfg->xkb_layout         = strdup("us");
    cfg->xkb_options        = strdup("");
    cfg->numlock_on_startup = true;
    cfg->repeat_rate        = 35;
    cfg->repeat_delay       = 250;

    cfg->touchpad_tap            = true;
    cfg->touchpad_natural_scroll = true;
    cfg->touchpad_dwt            = true;

    cfg->mouse_accel_profile  = strdup("flat");
    cfg->mouse_accel_speed    = 0.0;
    cfg->mouse_natural_scroll = false;

    cfg->touch_enabled          = true;
    cfg->touch_swipe_center     = strdup("launcher");
    cfg->touch_swipe_right      = strdup("control-center");
    cfg->touch_swipe_horizontal = strdup("column");
    cfg->touch_dwell_ms         = 4000;

    cfg->binds = comp_binds_new();
    cfg->rules = comp_rules_new();
    if (!cfg->binds || !cfg->rules) {
        comp_config_free(cfg);
        return NULL;
    }
    return cfg;
}

void
comp_config_free(CompConfig *cfg)
{
    if (!cfg)
        return;
    free(cfg->screenshot_path);
    free(cfg->xkb_layout);
    free(cfg->xkb_options);
    free(cfg->mouse_accel_profile);
    free(cfg->touch_swipe_center);
    free(cfg->touch_swipe_right);
    free(cfg->touch_swipe_horizontal);

    for (size_t i = 0; i < cfg->n_env; i++) {
        free(cfg->env[i].key);
        free(cfg->env[i].value);
    }
    free(cfg->env);

    free_argv_list(cfg->autostart, cfg->n_autostart);

    for (size_t i = 0; i < cfg->n_outputs; i++) {
        free(cfg->outputs[i].match);
        free(cfg->outputs[i].mode);
        free(cfg->outputs[i].transform);
        free(cfg->outputs[i].vrr);
    }
    free(cfg->outputs);

    comp_binds_free(cfg->binds);
    comp_rules_free(cfg->rules);
    free(cfg);
}

static bool
datum_as_double(toml_datum_t d, double *out)
{
    if (d.type == TOML_FP64 && isfinite(d.u.fp64)) {
        *out = d.u.fp64;
        return true;
    }
    if (d.type == TOML_INT64) {
        *out = (double)d.u.int64;
        return true;
    }
    return false;
}

static void
parse_color_or_accent(toml_datum_t d, float out_rgba[4], bool *tracks_accent,
                      const float accent_rgba[4], const char *key,
                      const char *src, CompDiagList *diag)
{
    if (d.type == TOML_UNKNOWN)
        return;
    if (d.type == TOML_STRING && tracks_accent && strcmp(d.u.s, "accent") == 0) {
        *tracks_accent = true;
        memcpy(out_rgba, accent_rgba, 4 * sizeof(float));
        return;
    }
    if (d.type == TOML_STRING && parse_hex_rgba(d.u.s, out_rgba)) {
        if (tracks_accent)
            *tracks_accent = false;
        return;
    }
    comp_diag_add(diag, src, d.lineno, "'%s' must be \"#rrggbb\", \"#rrggbbaa\", or \"accent\"; ignored", key);
}

static void
merge_layout_section(CompConfig *cfg, toml_datum_t tab, const char *src, CompDiagList *diag)
{
    if (tab.type != TOML_TABLE)
        return;
    /* Ignore shell.toml's `[layout]` (`left = [...]`, `center = [...]`, `right = [...]`) */
    if (toml_get(tab, "left").type != TOML_UNKNOWN ||
        toml_get(tab, "center").type != TOML_UNKNOWN ||
        toml_get(tab, "right").type != TOML_UNKNOWN) {
        return;
    }

    double v = 0.0;
    toml_datum_t gap = toml_get(tab, "gap_px");
    if (gap.type != TOML_UNKNOWN) {
        if (datum_as_double(gap, &v) && v >= 0.0 && v <= 200.0)
            cfg->layout.gap = v;
        else
            comp_diag_add(diag, src, gap.lineno, "'gap_px' must be a number in 0..200; ignored");
    }

    toml_datum_t bpx = toml_get(tab, "border_px");
    if (bpx.type != TOML_UNKNOWN) {
        if (bpx.type == TOML_INT64 && bpx.u.int64 >= 0 && bpx.u.int64 <= 32)
            cfg->border_px = (int32_t)bpx.u.int64;
        else
            comp_diag_add(diag, src, bpx.lineno, "'border_px' must be an integer in 0..32; ignored");
    }

    parse_color_or_accent(toml_get(tab, "active_border_color"),
                          cfg->active_border_rgba, &cfg->active_border_tracks_accent,
                          cfg->accent_rgba, "active_border_color", src, diag);
    parse_color_or_accent(toml_get(tab, "inactive_border_color"),
                          cfg->inactive_border_rgba, NULL,
                          cfg->accent_rgba, "inactive_border_color", src, diag);
    parse_color_or_accent(toml_get(tab, "urgent_border_color"),
                          cfg->urgent_border_rgba, NULL,
                          cfg->accent_rgba, "urgent_border_color", src, diag);

    toml_datum_t acs = toml_get(tab, "always_center_single_column");
    if (acs.type == TOML_BOOLEAN)
        cfg->layout.always_center_single_column = acs.u.boolean;

    toml_datum_t cfc = toml_get(tab, "center_focused_column");
    if (cfc.type == TOML_STRING) {
        if (strcmp(cfc.u.s, "never") == 0)
            cfg->layout.center_mode = COMP_CENTER_NEVER;
        else if (strcmp(cfc.u.s, "on-overflow") == 0)
            cfg->layout.center_mode = COMP_CENTER_ON_OVERFLOW;
        else if (strcmp(cfc.u.s, "always") == 0)
            cfg->layout.center_mode = COMP_CENTER_ALWAYS;
        else
            comp_diag_add(diag, src, cfc.lineno,
                          "'center_focused_column' must be \"never\", \"on-overflow\", or \"always\"; ignored");
    }

    toml_datum_t dcw = toml_get(tab, "default_column_width");
    if (dcw.type != TOML_UNKNOWN) {
        if (datum_as_double(dcw, &v) && v >= 0.05 && v <= 1.0)
            cfg->layout.default_width = v;
        else
            comp_diag_add(diag, src, dcw.lineno, "'default_column_width' must be in 0.05..1.0; ignored");
    }

    toml_datum_t pw = toml_get(tab, "preset_widths");
    if (pw.type == TOML_ARRAY && pw.u.arr.size > 0 && pw.u.arr.size <= COMP_LAYOUT_MAX_PRESETS) {
        double tmp[COMP_LAYOUT_MAX_PRESETS];
        bool ok = true;
        for (int i = 0; i < pw.u.arr.size; i++) {
            if (!datum_as_double(pw.u.arr.elem[i], &tmp[i]) || tmp[i] < 0.05 || tmp[i] > 1.0)
                ok = false;
        }
        if (ok) {
            memcpy(cfg->layout.preset_widths, tmp, (size_t)pw.u.arr.size * sizeof(double));
            cfg->layout.n_preset_widths = (size_t)pw.u.arr.size;
        } else {
            comp_diag_add(diag, src, pw.lineno, "'preset_widths' elements must be numbers in 0.05..1.0; ignored");
        }
    }

    toml_datum_t ph = toml_get(tab, "preset_heights");
    if (ph.type == TOML_ARRAY && ph.u.arr.size > 0 && ph.u.arr.size <= COMP_LAYOUT_MAX_PRESETS) {
        double tmp[COMP_LAYOUT_MAX_PRESETS];
        bool ok = true;
        for (int i = 0; i < ph.u.arr.size; i++) {
            if (!datum_as_double(ph.u.arr.elem[i], &tmp[i]) || tmp[i] < 0.1 || tmp[i] > 1.0)
                ok = false;
        }
        if (ok) {
            memcpy(cfg->layout.preset_heights, tmp, (size_t)ph.u.arr.size * sizeof(double));
            cfg->layout.n_preset_heights = (size_t)ph.u.arr.size;
        } else {
            comp_diag_add(diag, src, ph.lineno, "'preset_heights' elements must be numbers in 0.1..1.0; ignored");
        }
    }
}

static void
merge_spring_subtable(CompSpringParams *sp, toml_datum_t sub, const char *name,
                      const char *src, CompDiagList *diag)
{
    if (sub.type != TOML_TABLE)
        return;
    CompSpringParams cand = *sp;
    double v = 0.0;
    toml_datum_t dr = toml_get(sub, "damping_ratio");
    if (dr.type != TOML_UNKNOWN && datum_as_double(dr, &v))
        cand.damping_ratio = v;
    toml_datum_t st = toml_get(sub, "stiffness");
    if (st.type != TOML_UNKNOWN && datum_as_double(st, &v))
        cand.stiffness = v;
    toml_datum_t ep = toml_get(sub, "epsilon");
    if (ep.type != TOML_UNKNOWN && datum_as_double(ep, &v))
        cand.epsilon = v;

    if (comp_spring_params_valid(&cand))
        *sp = cand;
    else
        comp_diag_add(diag, src, sub.lineno, "[animations.%s] parameters out of valid range; ignored", name);
}

static void
merge_animations_section(CompConfig *cfg, toml_datum_t tab, const char *src, CompDiagList *diag)
{
    if (tab.type != TOML_TABLE)
        return;
    toml_datum_t en = toml_get(tab, "enabled");
    if (en.type == TOML_BOOLEAN)
        cfg->motion.enabled = en.u.boolean;

    double v = 0.0;
    toml_datum_t sd = toml_get(tab, "slowdown");
    if (sd.type != TOML_UNKNOWN && datum_as_double(sd, &v) && v >= 0.0 && v <= 100.0)
        cfg->motion.slowdown = v;

    merge_spring_subtable(&cfg->motion.workspace_switch,
                          toml_get(tab, "workspace_switch"), "workspace_switch", src, diag);
    merge_spring_subtable(&cfg->motion.horizontal_scroll,
                          toml_get(tab, "horizontal_scroll"), "horizontal_scroll", src, diag);
    merge_spring_subtable(&cfg->motion.window_movement,
                          toml_get(tab, "window_movement"), "window_movement", src, diag);
}

static void
merge_env_section(CompConfig *cfg, toml_datum_t tab, const char *src, CompDiagList *diag)
{
    if (tab.type != TOML_TABLE)
        return;
    for (int i = 0; i < tab.u.tab.size; i++) {
        const char *k = tab.u.tab.key[i];
        toml_datum_t v = tab.u.tab.value[i];
        if (v.type != TOML_STRING) {
            comp_diag_add(diag, src, v.lineno, "[env] '%s' must be a string; ignored", k);
            continue;
        }
        bool found = false;
        for (size_t j = 0; j < cfg->n_env; j++) {
            if (strcmp(cfg->env[j].key, k) == 0) {
                set_str(&cfg->env[j].value, v.u.s);
                found = true;
                break;
            }
        }
        if (!found) {
            CompEnvVar *p = realloc(cfg->env, (cfg->n_env + 1) * sizeof(CompEnvVar));
            if (!p)
                continue;
            cfg->env = p;
            cfg->env[cfg->n_env].key   = strdup(k);
            cfg->env[cfg->n_env].value = strdup(v.u.s);
            cfg->n_env++;
        }
    }
}

static void
merge_autostart_section(CompConfig *cfg, toml_datum_t tab, const char *src, CompDiagList *diag)
{
    if (tab.type != TOML_TABLE)
        return;
    toml_datum_t rst = toml_get(tab, "reset");
    if (rst.type == TOML_BOOLEAN && rst.u.boolean) {
        free_argv_list(cfg->autostart, cfg->n_autostart);
        cfg->autostart   = NULL;
        cfg->n_autostart = 0;
    }

    toml_datum_t cmds = toml_get(tab, "commands");
    if (cmds.type != TOML_ARRAY)
        return;
    for (int i = 0; i < cmds.u.arr.size; i++) {
        toml_datum_t entry = cmds.u.arr.elem[i];
        if (entry.type != TOML_ARRAY || entry.u.arr.size <= 0) {
            comp_diag_add(diag, src, entry.lineno, "[autostart].commands element must be a non-empty string array");
            continue;
        }
        size_t argc = (size_t)entry.u.arr.size;
        char **argv = calloc(argc + 1, sizeof(char *));
        if (!argv)
            continue;
        bool ok = true;
        for (size_t a = 0; a < argc; a++) {
            toml_datum_t s = entry.u.arr.elem[a];
            if (s.type != TOML_STRING || (a == 0 && s.u.str.len == 0)) {
                ok = false;
                break;
            }
            argv[a] = strdup(s.u.s);
            if (!argv[a]) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            for (size_t a = 0; a < argc; a++)
                free(argv[a]);
            free(argv);
            comp_diag_add(diag, src, entry.lineno, "[autostart].commands entry must contain only non-empty program strings");
            continue;
        }
        char ***p = realloc(cfg->autostart, (cfg->n_autostart + 1) * sizeof(char **));
        if (!p) {
            for (size_t a = 0; a < argc; a++)
                free(argv[a]);
            free(argv);
            continue;
        }
        cfg->autostart = p;
        cfg->autostart[cfg->n_autostart++] = argv;
    }
}

static void
merge_input_section(CompConfig *cfg, toml_datum_t tab, const char *src, CompDiagList *diag)
{
    if (tab.type != TOML_TABLE)
        return;

    toml_datum_t kb = toml_get(tab, "keyboard");
    if (kb.type == TOML_TABLE) {
        toml_datum_t xl = toml_get(kb, "xkb_layout");
        if (xl.type == TOML_STRING)
            set_str(&cfg->xkb_layout, xl.u.s);
        toml_datum_t xo = toml_get(kb, "xkb_options");
        if (xo.type == TOML_STRING)
            set_str(&cfg->xkb_options, xo.u.s);
        toml_datum_t nl = toml_get(kb, "numlock_on_startup");
        if (nl.type == TOML_BOOLEAN)
            cfg->numlock_on_startup = nl.u.boolean;
        toml_datum_t rr = toml_get(kb, "repeat_rate");
        if (rr.type == TOML_INT64 && rr.u.int64 >= 1 && rr.u.int64 <= 200)
            cfg->repeat_rate = (int32_t)rr.u.int64;
        else if (rr.type != TOML_UNKNOWN)
            comp_diag_add(diag, src, rr.lineno, "'repeat_rate' must be an integer in 1..200; ignored");
        toml_datum_t rd = toml_get(kb, "repeat_delay");
        if (rd.type == TOML_INT64 && rd.u.int64 >= 50 && rd.u.int64 <= 2000)
            cfg->repeat_delay = (int32_t)rd.u.int64;
        else if (rd.type != TOML_UNKNOWN)
            comp_diag_add(diag, src, rd.lineno, "'repeat_delay' must be an integer in 50..2000; ignored");
    }

    toml_datum_t tp = toml_get(tab, "touchpad");
    if (tp.type == TOML_TABLE) {
        toml_datum_t tap = toml_get(tp, "tap");
        if (tap.type == TOML_BOOLEAN)
            cfg->touchpad_tap = tap.u.boolean;
        toml_datum_t ns = toml_get(tp, "natural_scroll");
        if (ns.type == TOML_BOOLEAN)
            cfg->touchpad_natural_scroll = ns.u.boolean;
        toml_datum_t dwt = toml_get(tp, "dwt");
        if (dwt.type == TOML_BOOLEAN)
            cfg->touchpad_dwt = dwt.u.boolean;
    }

    toml_datum_t ms = toml_get(tab, "mouse");
    if (ms.type == TOML_TABLE) {
        toml_datum_t ap = toml_get(ms, "accel_profile");
        if (ap.type == TOML_STRING)
            set_str(&cfg->mouse_accel_profile, ap.u.s);
        double spd = 0.0;
        toml_datum_t as = toml_get(ms, "accel_speed");
        if (as.type != TOML_UNKNOWN && datum_as_double(as, &spd) && spd >= -1.0 && spd <= 1.0)
            cfg->mouse_accel_speed = spd;
        toml_datum_t ns = toml_get(ms, "natural_scroll");
        if (ns.type == TOML_BOOLEAN)
            cfg->mouse_natural_scroll = ns.u.boolean;
    }
}

static void
merge_output_array(CompConfig *cfg, toml_datum_t arr, const char *src, CompDiagList *diag)
{
    if (arr.type == TOML_UNKNOWN)
        return;
    if (arr.type != TOML_ARRAY) {
        comp_diag_add(diag, src, arr.lineno, "'output' must be an array of tables ([[output]]); ignored");
        return;
    }

    for (int i = 0; i < arr.u.arr.size; i++) {
        toml_datum_t t = arr.u.arr.elem[i];
        if (t.type != TOML_TABLE)
            continue;
        toml_datum_t m = toml_get(t, "match");
        if (m.type != TOML_STRING || m.u.str.len == 0) {
            comp_diag_add(diag, src, t.lineno, "[[output]] requires a non-empty 'match' string; ignored");
            continue;
        }
        char *low_match = str_lower_dup(m.u.s);
        if (!low_match)
            continue;

        CompOutputRule *rule = NULL;
        for (size_t j = 0; j < cfg->n_outputs; j++) {
            if (strcmp(cfg->outputs[j].match, low_match) == 0) {
                rule = &cfg->outputs[j];
                break;
            }
        }
        if (!rule) {
            CompOutputRule *p = realloc(cfg->outputs, (cfg->n_outputs + 1) * sizeof(CompOutputRule));
            if (!p) {
                free(low_match);
                continue;
            }
            cfg->outputs = p;
            rule = &cfg->outputs[cfg->n_outputs++];
            memset(rule, 0, sizeof(*rule));
            rule->match = low_match;
        } else {
            free(low_match);
        }

        toml_datum_t mode = toml_get(t, "mode");
        if (mode.type == TOML_STRING && mode.u.str.len > 0) {
            rule->has_mode = true;
            set_str(&rule->mode, mode.u.s);
        }
        double sc = 1.0;
        toml_datum_t scale = toml_get(t, "scale");
        if (scale.type != TOML_UNKNOWN) {
            if (datum_as_double(scale, &sc) && sc >= 0.5 && sc <= 4.0) {
                rule->has_scale = true;
                rule->scale     = sc;
            } else {
                comp_diag_add(diag, src, scale.lineno, "[[output]] 'scale' must be in 0.5..4.0; ignored");
            }
        }
        toml_datum_t tr = toml_get(t, "transform");
        if (tr.type == TOML_STRING) {
            rule->has_transform = true;
            set_str(&rule->transform, tr.u.s);
        }
        toml_datum_t vrr = toml_get(t, "vrr");
        if (vrr.type == TOML_STRING) {
            if (strcmp(vrr.u.s, "off") == 0 || strcmp(vrr.u.s, "on") == 0 ||
                strcmp(vrr.u.s, "on-demand") == 0) {
                rule->has_vrr = true;
                set_str(&rule->vrr, vrr.u.s);
            } else {
                comp_diag_add(diag, src, vrr.lineno,
                              "[[output]] 'vrr' must be \"off\", \"on\", or \"on-demand\"; ignored");
            }
        }
    }
}

static char *
read_file_bytes(const char *path, size_t *out_len)
{
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0 || sz > 16 * 1024 * 1024L || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

static bool
merge_toml_internal(CompConfig *cfg, const char *src, size_t len,
                    const char *source_name, const char *base_dir,
                    bool replace_binds, int depth, IncludeStack *stack,
                    CompDiagList *diag);

static void
expand_includes(CompConfig *cfg, toml_datum_t inc, const char *source_name,
                const char *base_dir, int depth, IncludeStack *stack, CompDiagList *diag)
{
    if (inc.type == TOML_UNKNOWN)
        return;
    if (depth >= COMP_MAX_INCLUDE_DEPTH) {
        comp_diag_add(diag, source_name, inc.lineno,
                      "maximum include depth (%d) exceeded; ignoring further includes",
                      COMP_MAX_INCLUDE_DEPTH);
        return;
    }
    if (inc.type != TOML_ARRAY) {
        comp_diag_add(diag, source_name, inc.lineno, "'include' must be an array of glob strings; ignored");
        return;
    }

    for (int i = 0; i < inc.u.arr.size; i++) {
        toml_datum_t pat_d = inc.u.arr.elem[i];
        if (pat_d.type != TOML_STRING || pat_d.u.str.len == 0) {
            comp_diag_add(diag, source_name, pat_d.lineno, "'include' element must be a non-empty string");
            continue;
        }
        char full_pat[PATH_MAX];
        if (pat_d.u.s[0] == '/' || !base_dir || !base_dir[0])
            snprintf(full_pat, sizeof(full_pat), "%s", pat_d.u.s);
        else
            snprintf(full_pat, sizeof(full_pat), "%s/%s", base_dir, pat_d.u.s);

        glob_t g = {0};
        if (glob(full_pat, 0, NULL, &g) == 0) {
            for (size_t gi = 0; gi < g.gl_pathc; gi++) {
                const char *p = g.gl_pathv[gi];
                bool seen = false;
                if (stack) {
                    for (size_t si = 0; si < stack->n; si++) {
                        if (strcmp(stack->paths[si], p) == 0) {
                            seen = true;
                            break;
                        }
                    }
                }
                if (seen) {
                    comp_diag_add(diag, source_name, pat_d.lineno, "cyclic include '%s' skipped", p);
                    continue;
                }
                size_t flen = 0;
                char *fbuf = read_file_bytes(p, &flen);
                if (!fbuf)
                    continue;
                if (stack && stack->n < COMP_MAX_LOADED_FILES)
                    stack->paths[stack->n++] = strdup(p);

                char inc_dir[PATH_MAX];
                snprintf(inc_dir, sizeof(inc_dir), "%s", p);
                char *slash = strrchr(inc_dir, '/');
                if (slash)
                    *slash = '\0';

                (void)merge_toml_internal(cfg, fbuf, flen, p, inc_dir, true, depth + 1, stack, diag);
                free(fbuf);
            }
        }
        globfree(&g);
    }
}

static bool
merge_toml_internal(CompConfig *cfg, const char *src, size_t len,
                    const char *source_name, const char *base_dir,
                    bool replace_binds, int depth, IncludeStack *stack,
                    CompDiagList *diag)
{
    const char *name = source_name ? source_name : "comp.toml";
    if (len > (size_t)INT_MAX) {
        comp_diag_add(diag, name, 0, "file too large");
        return false;
    }
    toml_result_t res = toml_parse_named(src ? src : "", (int)len, name);
    if (!res.ok) {
        comp_diag_add(diag, name, 0, "TOML syntax error: %s", res.errmsg);
        toml_free(res);
        return false;
    }

    /* 1. Top-level accent_color (from theme.toml) */
    toml_datum_t acc = toml_get(res.toptab, "accent_color");
    if (acc.type == TOML_STRING && parse_hex_rgba(acc.u.s, cfg->accent_rgba))
        sync_accent_dependents(cfg);

    /* 2. [layout], [animations], [switcher], [screenshots], [xwayland], [env], [autostart], [input], [touch] */
    merge_layout_section(cfg, toml_get(res.toptab, "layout"), name, diag);
    merge_animations_section(cfg, toml_get(res.toptab, "animations"), name, diag);

    toml_datum_t sw = toml_get(res.toptab, "switcher");
    if (sw.type == TOML_TABLE) {
        parse_color_or_accent(toml_get(sw, "active_color"),
                              cfg->switcher_active_rgba, &cfg->switcher_active_tracks_accent,
                              cfg->accent_rgba, "switcher.active_color", name, diag);
        parse_color_or_accent(toml_get(sw, "urgent_color"),
                              cfg->switcher_urgent_rgba, NULL,
                              cfg->accent_rgba, "switcher.urgent_color", name, diag);
    }

    toml_datum_t ss = toml_get(res.toptab, "screenshots");
    if (ss.type == TOML_TABLE) {
        toml_datum_t p = toml_get(ss, "path");
        if (p.type == TOML_STRING)
            set_str(&cfg->screenshot_path, p.u.s);
        toml_datum_t c = toml_get(ss, "copy_to_clipboard");
        if (c.type == TOML_BOOLEAN)
            cfg->screenshot_copy = c.u.boolean;
        toml_datum_t n = toml_get(ss, "show_notification");
        if (n.type == TOML_BOOLEAN)
            cfg->screenshot_notify = n.u.boolean;
        toml_datum_t b = toml_get(ss, "include_window_border");
        if (b.type == TOML_BOOLEAN)
            cfg->screenshot_border = b.u.boolean;
    }

    toml_datum_t xw = toml_get(res.toptab, "xwayland");
    if (xw.type == TOML_TABLE) {
        toml_datum_t en = toml_get(xw, "enabled");
        if (en.type == TOML_BOOLEAN)
            cfg->xwayland_enabled = en.u.boolean;
        toml_datum_t fz = toml_get(xw, "force_zero_scaling");
        if (fz.type == TOML_BOOLEAN)
            cfg->xwayland_force_zero_scaling = fz.u.boolean;
    }

    merge_env_section(cfg, toml_get(res.toptab, "env"), name, diag);
    merge_autostart_section(cfg, toml_get(res.toptab, "autostart"), name, diag);
    merge_input_section(cfg, toml_get(res.toptab, "input"), name, diag);

    toml_datum_t tch = toml_get(res.toptab, "touch");
    if (tch.type == TOML_TABLE) {
        toml_datum_t en = toml_get(tch, "enabled");
        if (en.type == TOML_BOOLEAN)
            cfg->touch_enabled = en.u.boolean;
        toml_datum_t sc = toml_get(tch, "swipe_center");
        if (sc.type == TOML_STRING)
            set_str(&cfg->touch_swipe_center, sc.u.s);
        toml_datum_t sr = toml_get(tch, "swipe_right");
        if (sr.type == TOML_STRING)
            set_str(&cfg->touch_swipe_right, sr.u.s);
        toml_datum_t sh = toml_get(tch, "swipe_horizontal");
        if (sh.type == TOML_STRING)
            set_str(&cfg->touch_swipe_horizontal, sh.u.s);
        toml_datum_t dw = toml_get(tch, "dwell_ms");
        if (dw.type == TOML_INT64 && dw.u.int64 >= 200 && dw.u.int64 <= 30000)
            cfg->touch_dwell_ms = (int32_t)dw.u.int64;
    }

    /* 3. [[output]], [[bind]], [[rule]], [[layer_rule]] */
    merge_output_array(cfg, toml_get(res.toptab, "output"), name, diag);

    if (toml_get(res.toptab, "bind").type != TOML_UNKNOWN) {
        (void)comp_binds_merge_toml(cfg->binds, src, len, name, cfg->mod_mask, replace_binds, diag);
    }
    if (toml_get(res.toptab, "rule").type != TOML_UNKNOWN ||
        toml_get(res.toptab, "layer_rule").type != TOML_UNKNOWN ||
        toml_get(res.toptab, "reset_rules").type != TOML_UNKNOWN) {
        (void)comp_rules_merge_toml(cfg->rules, src, len, name, diag);
    }

    /* 4. Top-level `include = [...]` globs */
    expand_includes(cfg, toml_get(res.toptab, "include"), name, base_dir, depth, stack, diag);

    toml_free(res);
    return true;
}

bool
comp_config_merge_toml(CompConfig *cfg, const char *src, size_t len,
                       const char *source_name, const char *base_dir,
                       bool replace_binds, CompDiagList *diag)
{
    if (!cfg)
        return false;
    IncludeStack stack = {0};
    bool ok = merge_toml_internal(cfg, src, len, source_name, base_dir, replace_binds, 0, &stack, diag);
    for (size_t i = 0; i < stack.n; i++)
        free(stack.paths[i]);
    return ok;
}

static bool
load_one_file_with_fallback(CompConfig *cfg, const char *config_dir, const char *defaults_dir,
                            const char *filename, const char *explicit_path,
                            bool replace_binds, bool fail_on_syntax_error,
                            IncludeStack *stack, CompDiagList *diag)
{
    char path[PATH_MAX];
    char dir[PATH_MAX];
    const char *chosen = NULL;

    if (explicit_path && explicit_path[0]) {
        chosen = explicit_path;
        snprintf(dir, sizeof(dir), "%s", explicit_path);
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = '\0';
        else
            snprintf(dir, sizeof(dir), ".");
    } else {
        struct stat st;
        if (config_dir && config_dir[0]) {
            snprintf(path, sizeof(path), "%s/%s", config_dir, filename);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                chosen = path;
                snprintf(dir, sizeof(dir), "%s", config_dir);
            }
        }
        if (!chosen && defaults_dir && defaults_dir[0]) {
            snprintf(path, sizeof(path), "%s/%s", defaults_dir, filename);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                chosen = path;
                snprintf(dir, sizeof(dir), "%s", defaults_dir);
            }
        }
    }

    if (!chosen)
        return true; /* optional file absent */

    size_t len = 0;
    char *buf = read_file_bytes(chosen, &len);
    if (!buf)
        return !explicit_path; /* explicit --config path must be readable */

    if (stack && stack->n < COMP_MAX_LOADED_FILES)
        stack->paths[stack->n++] = strdup(chosen);

    bool ok = merge_toml_internal(cfg, buf, len, chosen, dir, replace_binds, 0, stack, diag);
    free(buf);
    return fail_on_syntax_error ? ok : true;
}

static int
cmp_str_ptr(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

CompConfig *
comp_config_load_dir(const char *config_dir, const char *defaults_dir,
                     const char *override_comp_path, uint32_t mod_mask,
                     CompDiagList *diag)
{
    CompConfig *cfg = comp_config_new_defaults(mod_mask);
    if (!cfg)
        return NULL;

    IncludeStack stack = {0};
    bool ok = true;

    /* 0. System/repo defaults (non-fatal if absent) */
    if (defaults_dir && defaults_dir[0]) {
        (void)load_one_file_with_fallback(cfg, NULL, defaults_dir, "comp.toml",
                                          NULL, true, false, &stack, diag);
        (void)load_one_file_with_fallback(cfg, NULL, defaults_dir, "binds.toml",
                                          NULL, true, false, &stack, diag);
        (void)load_one_file_with_fallback(cfg, NULL, defaults_dir, "rules.toml",
                                          NULL, true, false, &stack, diag);
    }

    /* 1. Suite theme.toml & shell.toml (non-fatal if absent) */
    (void)load_one_file_with_fallback(cfg, config_dir, NULL, "theme.toml",
                                      NULL, true, false, &stack, diag);
    (void)load_one_file_with_fallback(cfg, config_dir, NULL, "shell.toml",
                                      NULL, true, false, &stack, diag);

    /* 2. Base compositor files: comp.toml, outputs.toml, binds.toml, rules.toml.
     * Syntax error in a base file rejects the reload (retaining previous snapshot). */
    if (!load_one_file_with_fallback(cfg, config_dir, NULL, "comp.toml",
                                     override_comp_path, true, true, &stack, diag) ||
        !load_one_file_with_fallback(cfg, config_dir, NULL, "outputs.toml",
                                     NULL, true, true, &stack, diag) ||
        !load_one_file_with_fallback(cfg, config_dir, NULL, "binds.toml",
                                     NULL, true, true, &stack, diag) ||
        !load_one_file_with_fallback(cfg, config_dir, NULL, "rules.toml",
                                     NULL, true, true, &stack, diag)) {
        ok = false;
    }

    /* 3. Drop-ins in <config_dir>/conf.d/<name>.toml in lexicographic order.
     * Syntax error in one drop-in logs a diagnostic and skips only that drop-in. */
    if (ok && config_dir && config_dir[0]) {
        char confd_dir[PATH_MAX];
        snprintf(confd_dir, sizeof(confd_dir), "%s/conf.d", config_dir);
        DIR *d = opendir(confd_dir);
        if (d) {
            char **names = NULL;
            size_t n_names = 0;
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                size_t nl = strlen(de->d_name);
                if (nl <= 5 || strcmp(de->d_name + nl - 5, ".toml") != 0 || de->d_name[0] == '.')
                    continue;
                char **p = realloc(names, (n_names + 1) * sizeof(char *));
                if (!p)
                    break;
                names = p;
                names[n_names++] = strdup(de->d_name);
            }
            closedir(d);

            if (n_names > 1)
                qsort(names, n_names, sizeof(char *), cmp_str_ptr);

            for (size_t i = 0; i < n_names; i++) {
                char drop_path[PATH_MAX];
                int n = snprintf(drop_path, sizeof(drop_path), "%s/%s", confd_dir, names[i]);
                if (n > 0 && (size_t)n < sizeof(drop_path)) {
                    size_t len = 0;
                    char *buf = read_file_bytes(drop_path, &len);
                    if (buf) {
                        (void)merge_toml_internal(cfg, buf, len, drop_path, confd_dir, true, 0, &stack, diag);
                        free(buf);
                    }
                }
                free(names[i]);
            }
            free(names);
        }
    }

    for (size_t i = 0; i < stack.n; i++)
        free(stack.paths[i]);

    if (!ok) {
        comp_config_free(cfg);
        return NULL;
    }
    return cfg;
}

const CompOutputRule *
comp_config_match_output(const CompConfig *cfg, const char *connector_name, const char *make_model)
{
    if (!cfg || cfg->n_outputs == 0)
        return NULL;
    const CompOutputRule *matched = NULL;
    for (size_t i = 0; i < cfg->n_outputs; i++) {
        const char *m = cfg->outputs[i].match;
        if (!m)
            continue;
        if ((connector_name && strcasecmp(m, connector_name) == 0) ||
            (make_model && strcasecmp(m, make_model) == 0)) {
            matched = &cfg->outputs[i];
        }
    }
    return matched;
}
