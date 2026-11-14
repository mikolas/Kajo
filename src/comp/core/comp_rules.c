/* comp_rules.c — see comp_rules.h. */
#define PCRE2_CODE_UNIT_WIDTH 8
#include "comp_rules.h"

#include "comp_schema_keys.h"
#include "../../vendor/tomlc17/tomlc17.h"

#include <limits.h>
#include <math.h>
#include <pcre2.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pcre2_code **re;
    size_t       n;
} Matcher;     /* n == 0: field not given (matches anything) */

typedef struct {
    Matcher app_id, title;
    CompWindowRuleResult set;
} WinRule;

typedef struct {
    Matcher ns;
    CompLayerRuleResult set;
} LayerRule;

struct CompRules {
    WinRule             *win;
    size_t               nwin;
    LayerRule           *layer;
    size_t               nlayer;
    pcre2_match_data    *md;
    pcre2_match_context *mctx;
};

static void
matcher_free(Matcher *m)
{
    for (size_t i = 0; i < m->n; i++)
        pcre2_code_free(m->re[i]);
    free(m->re);
    memset(m, 0, sizeof(*m));
}

void
comp_rules_free(CompRules *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->nwin; i++) {
        matcher_free(&r->win[i].app_id);
        matcher_free(&r->win[i].title);
    }
    for (size_t i = 0; i < r->nlayer; i++)
        matcher_free(&r->layer[i].ns);
    free(r->win);
    free(r->layer);
    pcre2_match_data_free(r->md);
    pcre2_match_context_free(r->mctx);
    free(r);
}

size_t comp_rules_window_count(const CompRules *r) { return r ? r->nwin : 0; }
size_t comp_rules_layer_count(const CompRules *r)  { return r ? r->nlayer : 0; }

/* Compile a non-empty array of regex strings. Returns false (with a diagnostic) on any bad pattern. */
static bool
matcher_build(Matcher *m, toml_datum_t v, bool case_sensitive, const char *field,
              const char *src, int line, CompDiagList *diag)
{
    memset(m, 0, sizeof(*m));
    if (v.type == TOML_UNKNOWN)
        return true;
    if (v.type != TOML_ARRAY || v.u.arr.size <= 0) {
        comp_diag_add(diag, src, v.lineno ? v.lineno : line,
                      "'%s' must be a non-empty array of regex strings, e.g. ['^foo$']", field);
        return false;
    }
    size_t n = (size_t)v.u.arr.size;
    const toml_datum_t *items = v.u.arr.elem;
    m->re = calloc(n, sizeof(pcre2_code *));
    if (!m->re) {
        comp_diag_add(diag, src, line, "out of memory");
        return false;
    }
    uint32_t flags = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF | (case_sensitive ? 0u : PCRE2_CASELESS);
    for (size_t i = 0; i < n; i++) {
        if (items[i].type != TOML_STRING) {
            comp_diag_add(diag, src, line, "'%s' array must contain only strings", field);
            matcher_free(m);
            return false;
        }
        int errcode = 0;
        PCRE2_SIZE erroff = 0;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)items[i].u.s, (PCRE2_SIZE)items[i].u.str.len,
                                       flags, &errcode, &erroff, NULL);
        if (!re) {
            PCRE2_UCHAR msg[160];
            pcre2_get_error_message(errcode, msg, sizeof(msg));
            comp_diag_add(diag, src, line, "'%s' regex '%s' is invalid at offset %zu: %s",
                          field, items[i].u.s, (size_t)erroff, (const char *)msg);
            matcher_free(m);
            return false;
        }
        m->re[m->n++] = re;
    }
    return true;
}

static bool
matcher_matches(const Matcher *m, const char *s, pcre2_match_data *md, pcre2_match_context *mctx)
{
    if (m->n == 0)
        return true;
    if (!s)
        s = "";
    size_t slen = strlen(s);
    if (slen > 1024)
        slen = 1024;
    for (size_t i = 0; i < m->n; i++) {
        int rc = pcre2_match(m->re[i], (PCRE2_SPTR)s, slen, 0, 0, md, mctx);
        if (rc >= 0)
            return true;
    }
    return false;
}

/* ───────────────────────── property helpers ───────────────────────── */

static bool
get_bool(toml_datum_t t, const char *key, bool *has, bool *out, const char *src, CompDiagList *diag)
{
    toml_datum_t v = toml_get(t, key);
    if (v.type == TOML_UNKNOWN)
        return true;
    if (v.type != TOML_BOOLEAN) {
        comp_diag_add(diag, src, v.lineno, "'%s' must be true or false; ignored", key);
        return false;
    }
    *has = true;
    *out = v.u.boolean;
    return true;
}

static bool
get_number(toml_datum_t t, const char *key, double lo, double hi, bool *has, double *out,
           const char *src, CompDiagList *diag)
{
    toml_datum_t v = toml_get(t, key);
    if (v.type == TOML_UNKNOWN)
        return true;
    double d;
    if (v.type == TOML_FP64)
        d = v.u.fp64;
    else if (v.type == TOML_INT64)
        d = (double)v.u.int64;
    else {
        comp_diag_add(diag, src, v.lineno, "'%s' must be a number; ignored", key);
        return false;
    }
    if (!isfinite(d) || d < lo || d > hi) {
        comp_diag_add(diag, src, v.lineno, "'%s' = %g is outside %g..%g; ignored", key, d, lo, hi);
        return false;
    }
    *has = true;
    *out = d;
    return true;
}

static void
warn_unknown_keys(toml_datum_t t, const char *const *known, size_t nknown,
                  const char *const *unsupported, size_t nunsup,
                  const char *what, const char *src, CompDiagList *diag)
{
    for (int i = 0; i < t.u.tab.size; i++) {
        const char *k = t.u.tab.key[i];
        bool ok = false;
        for (size_t j = 0; j < nknown; j++)
            if (strcmp(k, known[j]) == 0)
                ok = true;
        if (ok)
            continue;
        bool unsup = false;
        for (size_t j = 0; j < nunsup; j++)
            if (strcmp(k, unsupported[j]) == 0)
                unsup = true;
        comp_diag_add(diag, src, t.u.tab.value[i].lineno,
                      unsup ? "%s '%s' is not supported in this version; ignored"
                            : "unknown %s key '%s' ignored", what, k);
    }
}

static const char *const k_win_keys[] = {
    COMP_KEY_APP_ID, COMP_KEY_TITLE, COMP_KEY_CASE_SENSITIVE, COMP_KEY_FLOATING,
    COMP_KEY_WIDTH, COMP_KEY_FOCUS, COMP_KEY_ANCHOR_CORNER, COMP_KEY_ANCHOR_X,
    COMP_KEY_ANCHOR_Y, COMP_KEY_SCROLL_FACTOR, COMP_KEY_VRR, COMP_KEY_SHADOW,
};
static const char *const k_layer_keys[] = {
    COMP_KEY_NAMESPACE, COMP_KEY_CASE_SENSITIVE, COMP_KEY_OPACITY, COMP_KEY_POPUP_OPACITY,
    COMP_KEY_SHADOW,
};
static const char *const k_layer_unsupported[] = { "blur", "xray", "blur_popups" };   /* planned: ROADMAP.md §3.7.2 */

static bool
parse_anchor(toml_datum_t t, CompWindowRuleResult *set, const char *src, CompDiagList *diag)
{
    toml_datum_t c  = toml_get(t, COMP_KEY_ANCHOR_CORNER);
    toml_datum_t ax = toml_get(t, COMP_KEY_ANCHOR_X);
    toml_datum_t ay = toml_get(t, COMP_KEY_ANCHOR_Y);
    if (c.type == TOML_UNKNOWN) {
        if (ax.type != TOML_UNKNOWN || ay.type != TOML_UNKNOWN) {
            int ln = ax.type != TOML_UNKNOWN ? ax.lineno : ay.lineno;
            comp_diag_add(diag, src, ln, "anchor_x / anchor_y require anchor_corner; ignored");
            return false;
        }
        return true;
    }
    static const struct { const char *name; CompCorner c; } k_corners[] = {
        { "top-left", COMP_CORNER_TOP_LEFT }, { "top-right", COMP_CORNER_TOP_RIGHT },
        { "bottom-left", COMP_CORNER_BOTTOM_LEFT }, { "bottom-right", COMP_CORNER_BOTTOM_RIGHT },
    };
    bool found = false;
    for (size_t i = 0; c.type == TOML_STRING && i < sizeof(k_corners) / sizeof(k_corners[0]); i++) {
        if (strcmp(c.u.s, k_corners[i].name) == 0) {
            set->corner = k_corners[i].c;
            found = true;
        }
    }
    if (!found) {
        comp_diag_add(diag, src, c.lineno,
                      "anchor_corner must be \"top-left\", \"top-right\", \"bottom-left\" or \"bottom-right\"; ignored");
        return false;
    }
    bool hx = false, hy = false;
    double x = 0.0, y = 0.0;
    if (!get_number(t, COMP_KEY_ANCHOR_X, -10000.0, 10000.0, &hx, &x, src, diag) ||
        !get_number(t, COMP_KEY_ANCHOR_Y, -10000.0, 10000.0, &hy, &y, src, diag))
        return false;
    set->has_anchor = true;
    set->anchor_x = x;
    set->anchor_y = y;
    return true;
}

static void
parse_window_rule(CompRules *r, toml_datum_t t, const char *src, CompDiagList *diag)
{
    WinRule w;
    memset(&w, 0, sizeof(w));
    if (t.type != TOML_TABLE) {
        comp_diag_add(diag, src, t.lineno, "[[rule]] entry must be a table; ignored");
        return;
    }
    warn_unknown_keys(t, k_win_keys, sizeof(k_win_keys) / sizeof(k_win_keys[0]), NULL, 0,
                      COMP_SEC_RULE, src, diag);
    toml_datum_t app = toml_get(t, COMP_KEY_APP_ID), title = toml_get(t, COMP_KEY_TITLE);
    if (app.type == TOML_UNKNOWN && title.type == TOML_UNKNOWN) {
        comp_diag_add(diag, src, t.lineno, "[[rule]] needs app_id and/or title to match on; ignored");
        return;
    }
    bool has_cs = false, case_sensitive = false;
    (void)get_bool(t, COMP_KEY_CASE_SENSITIVE, &has_cs, &case_sensitive, src, diag);
    if (!matcher_build(&w.app_id, app, case_sensitive, COMP_KEY_APP_ID, src, t.lineno, diag) ||
        !matcher_build(&w.title, title, case_sensitive, COMP_KEY_TITLE, src, t.lineno, diag)) {
        matcher_free(&w.app_id);
        matcher_free(&w.title);
        return;
    }
    (void)get_bool(t, COMP_KEY_FLOATING, &w.set.has_floating, &w.set.floating, src, diag);
    (void)get_bool(t, COMP_KEY_FOCUS, &w.set.has_focus, &w.set.focus, src, diag);
    (void)get_bool(t, COMP_KEY_VRR, &w.set.has_vrr, &w.set.vrr, src, diag);
    (void)get_bool(t, COMP_KEY_SHADOW, &w.set.has_shadow, &w.set.shadow, src, diag);
    (void)get_number(t, COMP_KEY_WIDTH, 0.05, 1.0, &w.set.has_width, &w.set.width, src, diag);
    (void)get_number(t, COMP_KEY_SCROLL_FACTOR, 0.01, 10.0, &w.set.has_scroll_factor, &w.set.scroll_factor, src, diag);
    (void)parse_anchor(t, &w.set, src, diag);

    WinRule *p = realloc(r->win, (r->nwin + 1) * sizeof(WinRule));
    if (!p) {
        comp_diag_add(diag, src, t.lineno, "out of memory");
        matcher_free(&w.app_id);
        matcher_free(&w.title);
        return;
    }
    r->win = p;
    r->win[r->nwin++] = w;
}

static void
parse_layer_rule(CompRules *r, toml_datum_t t, const char *src, CompDiagList *diag)
{
    LayerRule l;
    memset(&l, 0, sizeof(l));
    if (t.type != TOML_TABLE) {
        comp_diag_add(diag, src, t.lineno, "[[layer_rule]] entry must be a table; ignored");
        return;
    }
    warn_unknown_keys(t, k_layer_keys, sizeof(k_layer_keys) / sizeof(k_layer_keys[0]),
                      k_layer_unsupported, sizeof(k_layer_unsupported) / sizeof(k_layer_unsupported[0]),
                      COMP_SEC_LAYER_RULE, src, diag);
    toml_datum_t ns = toml_get(t, COMP_KEY_NAMESPACE);
    if (ns.type == TOML_UNKNOWN) {
        comp_diag_add(diag, src, t.lineno, "[[layer_rule]] needs a namespace to match on; ignored");
        return;
    }
    bool has_cs = false, case_sensitive = false;
    (void)get_bool(t, COMP_KEY_CASE_SENSITIVE, &has_cs, &case_sensitive, src, diag);
    if (!matcher_build(&l.ns, ns, case_sensitive, COMP_KEY_NAMESPACE, src, t.lineno, diag))
        return;
    (void)get_number(t, COMP_KEY_OPACITY, 0.0, 1.0, &l.set.has_opacity, &l.set.opacity, src, diag);
    (void)get_number(t, COMP_KEY_POPUP_OPACITY, 0.0, 1.0, &l.set.has_popup_opacity, &l.set.popup_opacity, src, diag);
    (void)get_bool(t, COMP_KEY_SHADOW, &l.set.has_shadow, &l.set.shadow, src, diag);

    LayerRule *p = realloc(r->layer, (r->nlayer + 1) * sizeof(LayerRule));
    if (!p) {
        comp_diag_add(diag, src, t.lineno, "out of memory");
        matcher_free(&l.ns);
        return;
    }
    r->layer = p;
    r->layer[r->nlayer++] = l;
}

static void
for_each_entry(CompRules *r, toml_datum_t v, const char *key, const char *src, CompDiagList *diag,
               void (*fn)(CompRules *, toml_datum_t, const char *, CompDiagList *))
{
    if (v.type == TOML_UNKNOWN)
        return;
    if (v.type != TOML_ARRAY) {
        comp_diag_add(diag, src, v.lineno, "'%s' must be an array of tables ([[%s]]); ignored", key, key);
        return;
    }
    for (int i = 0; i < v.u.arr.size; i++)
        fn(r, v.u.arr.elem[i], src, diag);
}

static void
clear_window_rules(CompRules *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->nwin; i++) {
        matcher_free(&r->win[i].app_id);
        matcher_free(&r->win[i].title);
    }
    free(r->win);
    r->win  = NULL;
    r->nwin = 0;
}

CompRules *
comp_rules_new(void)
{
    CompRules *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->md   = pcre2_match_data_create(1, NULL);
    r->mctx = pcre2_match_context_create(NULL);
    if (!r->md || !r->mctx) {
        pcre2_match_data_free(r->md);
        pcre2_match_context_free(r->mctx);
        free(r);
        return NULL;
    }
    pcre2_set_match_limit(r->mctx, 10000);
    pcre2_set_depth_limit(r->mctx, 250);
    return r;
}

static void
merge_rules_from_table(CompRules *r, toml_datum_t tab, const char *name, CompDiagList *diag)
{
    if (tab.type != TOML_TABLE)
        return;
    toml_datum_t rr = toml_get(tab, COMP_KEY_RESET_RULES);
    if (rr.type == TOML_BOOLEAN && rr.u.boolean)
        clear_window_rules(r);
    else if (rr.type != TOML_UNKNOWN && rr.type != TOML_BOOLEAN)
        comp_diag_add(diag, name, rr.lineno, "'reset_rules' must be true or false; ignored");

    for_each_entry(r, toml_get(tab, COMP_SEC_RULE), COMP_SEC_RULE, name, diag, parse_window_rule);
    for_each_entry(r, toml_get(tab, COMP_SEC_LAYER_RULE), COMP_SEC_LAYER_RULE, name, diag, parse_layer_rule);
}

bool
comp_rules_merge_toml(CompRules *r, const char *src, size_t len,
                      const char *source_name, CompDiagList *diag)
{
    if (!r)
        return false;
    const char *name = source_name ? source_name : COMP_CFG_FILE_RULES;
    if (len > (size_t)INT_MAX) {
        comp_diag_add(diag, name, 0, "file too large");
        return false;
    }
    toml_result_t res = toml_parse_named(src ? src : "", (int)len, name);
    if (!res.ok) {
        comp_diag_add(diag, name, 0, "TOML syntax error: %s (keeping previous rules)", res.errmsg);
        toml_free(res);
        return false;
    }
    merge_rules_from_table(r, res.toptab, name, diag);
    merge_rules_from_table(r, toml_get(res.toptab, COMP_SEC_COMP), name, diag);
    toml_free(res);
    return true;
}

CompRules *
comp_rules_parse(const char *src, size_t len, const char *source_name, CompDiagList *diag)
{
    const char *name = source_name ? source_name : COMP_CFG_FILE_RULES;
    if (len > (size_t)INT_MAX) {
        comp_diag_add(diag, name, 0, "file too large");
        return NULL;
    }
    toml_result_t res = toml_parse_named(src ? src : "", (int)len, name);
    if (!res.ok) {
        comp_diag_add(diag, name, 0, "TOML syntax error: %s (keeping previous rules)", res.errmsg);
        toml_free(res);
        return NULL;
    }
    CompRules *r = comp_rules_new();
    if (!r) {
        toml_free(res);
        return NULL;
    }
    for (int i = 0; i < res.toptab.u.tab.size; i++) {
        const char *k = res.toptab.u.tab.key[i];
        if (strcmp(k, COMP_SEC_RULE) != 0 && strcmp(k, COMP_SEC_LAYER_RULE) != 0 &&
            strcmp(k, COMP_KEY_RESET_RULES) != 0)
            comp_diag_add(diag, name, res.toptab.u.tab.value[i].lineno,
                          "unknown top-level key '%s' ignored (expected [[rule]] or [[layer_rule]])", k);
    }
    toml_datum_t rr = toml_get(res.toptab, COMP_KEY_RESET_RULES);
    if (rr.type != TOML_UNKNOWN && rr.type != TOML_BOOLEAN)
        comp_diag_add(diag, name, rr.lineno, "'reset_rules' must be true or false; ignored");

    for_each_entry(r, toml_get(res.toptab, COMP_SEC_RULE), COMP_SEC_RULE, name, diag, parse_window_rule);
    for_each_entry(r, toml_get(res.toptab, COMP_SEC_LAYER_RULE), COMP_SEC_LAYER_RULE, name, diag, parse_layer_rule);
    toml_free(res);
    return r;
}

/* ───────────────────────── matching ───────────────────────── */

void
comp_rules_match_window(const CompRules *r, const char *app_id, const char *title, CompWindowRuleResult *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; r && i < r->nwin; i++) {
        const WinRule *w = &r->win[i];
        if (!matcher_matches(&w->app_id, app_id, r->md, r->mctx) ||
            !matcher_matches(&w->title, title, r->md, r->mctx))
            continue;
        const CompWindowRuleResult *s = &w->set;
        if (s->has_floating)      { out->has_floating = true; out->floating = s->floating; }
        if (s->has_width)         { out->has_width = true; out->width = s->width; }
        if (s->has_focus)         { out->has_focus = true; out->focus = s->focus; }
        if (s->has_anchor)        { out->has_anchor = true; out->corner = s->corner;
                                    out->anchor_x = s->anchor_x; out->anchor_y = s->anchor_y; }
        if (s->has_scroll_factor) { out->has_scroll_factor = true; out->scroll_factor = s->scroll_factor; }
        if (s->has_vrr)           { out->has_vrr = true; out->vrr = s->vrr; }
        if (s->has_shadow)        { out->has_shadow = true; out->shadow = s->shadow; }
    }
}

void
comp_rules_match_layer(const CompRules *r, const char *ns, CompLayerRuleResult *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; r && i < r->nlayer; i++) {
        const LayerRule *l = &r->layer[i];
        if (!matcher_matches(&l->ns, ns, r->md, r->mctx))
            continue;
        if (l->set.has_opacity)       { out->has_opacity = true; out->opacity = l->set.opacity; }
        if (l->set.has_popup_opacity) { out->has_popup_opacity = true; out->popup_opacity = l->set.popup_opacity; }
        if (l->set.has_shadow)        { out->has_shadow = true; out->shadow = l->set.shadow; }
    }
}
