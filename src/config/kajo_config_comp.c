#include "kajo_config_schema.h"
#include "kajo_config_keys.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

static gboolean
ns_pattern_matches_id(const char *pat, const char *ns_id)
{
    if (!pat || !ns_id)
        return FALSE;
    const char *start = (pat[0] == '^') ? (pat + 1) : pat;
    size_t len = strlen(start);
    if (len > 0 && start[len - 1] == '$')
        len--;
    return strlen(ns_id) == len && strncmp(start, ns_id, len) == 0;
}

static gboolean
layer_table_matches_ns_id(toml_datum_t lr, const char *ns_id)
{
    if (lr.type != TOML_TABLE || !ns_id)
        return FALSE;

    toml_datum_t ns = toml_get(lr, COMP_KEY_NAMESPACE);
    if (ns.type == TOML_UNKNOWN)
        ns = toml_get(lr, "match_namespace");

    if (ns.type == TOML_STRING && ns.u.s) {
        return ns_pattern_matches_id(ns.u.s, ns_id);
    } else if (ns.type == TOML_ARRAY) {
        for (int i = 0; i < ns.u.arr.size; i++) {
            toml_datum_t elem = ns.u.arr.elem[i];
            if (elem.type == TOML_STRING && elem.u.s && ns_pattern_matches_id(elem.u.s, ns_id))
                return TRUE;
        }
    }
    return FALSE;
}

static gboolean
layer_table_matches_any_ns(toml_datum_t lr, const char *const *ns_ids, size_t n_ns)
{
    if (!ns_ids)
        return FALSE;
    for (size_t k = 0; k < n_ns; k++) {
        if (layer_table_matches_ns_id(lr, ns_ids[k]))
            return TRUE;
    }
    return FALSE;
}

static void
apply_comp_table_to_state(toml_datum_t       tab,
                          int               *inout_gap_px,
                          const char *const *ns_ids,
                          int               *inout_ns_opacity_pct,
                          size_t             n_ns)
{
    if (tab.type != TOML_TABLE)
        return;

    if (inout_gap_px) {
        toml_datum_t ltab = toml_get(tab, COMP_SEC_LAYOUT);
        if (ltab.type == TOML_TABLE &&
            toml_get(ltab, COMP_KEY_LAYOUT_LEFT).type == TOML_UNKNOWN &&
            toml_get(ltab, COMP_KEY_LAYOUT_CENTER).type == TOML_UNKNOWN &&
            toml_get(ltab, COMP_KEY_LAYOUT_RIGHT).type == TOML_UNKNOWN) {
            toml_datum_t gv = toml_get(ltab, COMP_KEY_GAP_PX);
            double v = -1.0;
            if (gv.type == TOML_INT64)
                v = (double)gv.u.int64;
            else if (gv.type == TOML_FP64 && isfinite(gv.u.fp64))
                v = gv.u.fp64;
            if (v >= 0.0 && v <= 200.0)
                *inout_gap_px = (int)(v + 0.5);
        }
    }

    if (ns_ids && inout_ns_opacity_pct && n_ns > 0) {
        toml_datum_t l_arr = toml_get(tab, COMP_SEC_LAYER_RULE);
        if (l_arr.type == TOML_ARRAY) {
            for (int li = 0; li < l_arr.u.arr.size; li++) {
                toml_datum_t lr = l_arr.u.arr.elem[li];
                toml_datum_t op_v = toml_get(lr, COMP_KEY_OPACITY);
                double op_val = (op_v.type == TOML_FP64) ? op_v.u.fp64
                              : ((op_v.type == TOML_INT64) ? (double)op_v.u.int64 : -1.0);
                if (op_val < 0.0 || op_val > 1.0)
                    continue;
                int pct = (int)(op_val * 100.0 + 0.5);
                for (size_t k = 0; k < n_ns; k++) {
                    if (layer_table_matches_ns_id(lr, ns_ids[k]))
                        inout_ns_opacity_pct[k] = pct;
                }
            }
        }
    }
}

static void
apply_comp_file_if_exists(const char        *path,
                          int               *inout_gap_px,
                          const char *const *ns_ids,
                          int               *inout_ns_opacity_pct,
                          size_t             n_ns)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    g_autofree gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL) || !contents)
        return;

    toml_result_t r = toml_parse(contents, (int)len);
    if (r.ok) {
        apply_comp_table_to_state(r.toptab, inout_gap_px, ns_ids, inout_ns_opacity_pct, n_ns);
        apply_comp_table_to_state(toml_get(r.toptab, COMP_SEC_COMP),
                                  inout_gap_px, ns_ids, inout_ns_opacity_pct, n_ns);
    }
    toml_free(r);
}

static gint
cmp_str_ptrs(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

gboolean
kajo_config_comp_read_layout_and_layers(const char        *config_dir,
                                        int               *out_gap_px,
                                        const char *const *ns_ids,
                                        int               *inout_ns_opacity_pct,
                                        size_t             n_ns)
{
    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();

    g_autofree gchar *def_comp  = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
    g_autofree gchar *def_rules = kajo_config_find_default_file(COMP_CFG_FILE_RULES);
    apply_comp_file_if_exists(def_comp,  out_gap_px, ns_ids, inout_ns_opacity_pct, n_ns);
    apply_comp_file_if_exists(def_rules, out_gap_px, ns_ids, inout_ns_opacity_pct, n_ns);

    g_autofree gchar *usr_comp  = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
    g_autofree gchar *usr_rules = g_build_filename(dir, COMP_CFG_FILE_RULES, NULL);
    apply_comp_file_if_exists(usr_comp,  out_gap_px, ns_ids, inout_ns_opacity_pct, n_ns);
    apply_comp_file_if_exists(usr_rules, out_gap_px, ns_ids, inout_ns_opacity_pct, n_ns);

    g_autofree gchar *confd_dir = g_build_filename(dir, COMP_CFG_DIR_CONFD, NULL);
    GDir *d = g_dir_open(confd_dir, 0, NULL);
    if (d) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        const gchar *name = NULL;
        while ((name = g_dir_read_name(d)) != NULL) {
            if (name[0] != '.' && g_str_has_suffix(name, COMP_CFG_EXT_TOML))
                g_ptr_array_add(names, g_strdup(name));
        }
        g_dir_close(d);
        g_ptr_array_sort(names, cmp_str_ptrs);
        for (guint i = 0; i < names->len; i++) {
            const gchar *fn = (const gchar *)g_ptr_array_index(names, i);
            g_autofree gchar *p = g_build_filename(confd_dir, fn, NULL);
            apply_comp_file_if_exists(p, out_gap_px, ns_ids, inout_ns_opacity_pct, n_ns);
        }
        g_ptr_array_free(names, TRUE);
    }
    return TRUE;
}

static gboolean
block_is_managed_layer_rule(const char *block_str, size_t block_len,
                            const char *const *ns_ids, size_t n_ns)
{
    g_autofree gchar *tmp = g_strndup(block_str, block_len);
    toml_result_t r = toml_parse(tmp, (int)block_len);
    gboolean managed = FALSE;
    if (r.ok) {
        toml_datum_t arr = toml_get(r.toptab, COMP_SEC_LAYER_RULE);
        if (arr.type == TOML_ARRAY && arr.u.arr.size == 1)
            managed = layer_table_matches_any_ns(arr.u.arr.elem[0], ns_ids, n_ns);
    }
    toml_free(r);
    return managed;
}

static GString *
strip_managed_layer_rules_from_doc(const char *doc, const char *const *ns_ids, size_t n_ns)
{
    GString *out = g_string_new(NULL);
    if (!doc || !*doc) {
        g_string_append(out, "# rules.toml — Kajo Compositor Window & Layer Rules\n");
        return out;
    }

    size_t len = strlen(doc);
    size_t block_start = 0;
    size_t pos = 0;

    while (pos <= len) {
        const char *nl = (pos < len) ? memchr(doc + pos, '\n', len - pos) : NULL;
        size_t line_end = nl ? (size_t)(nl - doc) : len;
        size_t next_pos = nl ? (line_end + 1) : (len + 1);

        size_t i = pos;
        while (i < line_end && (doc[i] == ' ' || doc[i] == '\t'))
            i++;
        gboolean is_hdr = (i < line_end && doc[i] == '[');

        if ((is_hdr && pos > block_start) || pos == len) {
            size_t blen = pos - block_start;
            if (blen > 0 && !block_is_managed_layer_rule(doc + block_start, blen, ns_ids, n_ns))
                g_string_append_len(out, doc + block_start, (gssize)blen);
            block_start = pos;
        }
        pos = next_pos;
    }

    while (out->len > 0 && (out->str[out->len - 1] == '\n' || out->str[out->len - 1] == '\r'))
        g_string_truncate(out, out->len - 1);
    g_string_append_c(out, '\n');
    return out;
}

typedef struct {
    gboolean has_blur;
    gboolean blur;
    gboolean has_blur_popups;
    gboolean blur_popups;
    gboolean has_xray;
    gboolean xray;
    gboolean has_shadow;
    gboolean shadow;
    gboolean has_popup_opacity;
    int      opacity_pct;
} ManagedLayerProps;

static void
init_default_managed_layer_props(const char *ns_id, ManagedLayerProps *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->opacity_pct       = 100;
    p->has_popup_opacity = TRUE;
    p->has_blur          = TRUE;
    p->blur              = TRUE;
    p->has_blur_popups   = TRUE;
    p->blur_popups       = TRUE;
    if (g_strcmp0(ns_id, "kajo") == 0) {
        p->has_xray = TRUE;
        p->xray     = TRUE;
    }
}

static void
apply_layer_rules_doc_to_props(const char        *doc,
                               const char *const *ns_ids,
                               ManagedLayerProps *props,
                               size_t             n_ns)
{
    if (!doc || !*doc || !ns_ids || !props || n_ns == 0)
        return;
    toml_result_t r = toml_parse(doc, (int)strlen(doc));
    if (!r.ok) {
        toml_free(r);
        return;
    }
    toml_datum_t tabs[2] = { r.toptab, toml_get(r.toptab, COMP_SEC_COMP) };
    for (int ti = 0; ti < 2; ti++) {
        if (tabs[ti].type != TOML_TABLE)
            continue;
        toml_datum_t l_arr = toml_get(tabs[ti], COMP_SEC_LAYER_RULE);
        if (l_arr.type != TOML_ARRAY)
            continue;
        for (int li = 0; li < l_arr.u.arr.size; li++) {
            toml_datum_t lr = l_arr.u.arr.elem[li];
            if (lr.type != TOML_TABLE)
                continue;
            for (size_t k = 0; k < n_ns; k++) {
                if (!layer_table_matches_ns_id(lr, ns_ids[k]))
                    continue;
                toml_datum_t op_v = toml_get(lr, COMP_KEY_OPACITY);
                double op_val = (op_v.type == TOML_FP64) ? op_v.u.fp64
                              : ((op_v.type == TOML_INT64) ? (double)op_v.u.int64 : -1.0);
                if (op_val >= 0.0 && op_val <= 1.0)
                    props[k].opacity_pct = (int)(op_val * 100.0 + 0.5);

                toml_datum_t pop_v = toml_get(lr, COMP_KEY_POPUP_OPACITY);
                if (pop_v.type == TOML_FP64 || pop_v.type == TOML_INT64)
                    props[k].has_popup_opacity = TRUE;

                toml_datum_t bl_v = toml_get(lr, COMP_KEY_BLUR);
                if (bl_v.type == TOML_BOOLEAN) {
                    props[k].has_blur = TRUE;
                    props[k].blur     = bl_v.u.boolean;
                }
                toml_datum_t bp_v = toml_get(lr, COMP_KEY_BLUR_POPUPS);
                if (bp_v.type == TOML_BOOLEAN) {
                    props[k].has_blur_popups = TRUE;
                    props[k].blur_popups     = bp_v.u.boolean;
                }
                toml_datum_t xr_v = toml_get(lr, COMP_KEY_XRAY);
                if (xr_v.type == TOML_BOOLEAN) {
                    props[k].has_xray = TRUE;
                    props[k].xray     = xr_v.u.boolean;
                }
                toml_datum_t sh_v = toml_get(lr, COMP_KEY_SHADOW);
                if (sh_v.type == TOML_BOOLEAN) {
                    props[k].has_shadow = TRUE;
                    props[k].shadow     = sh_v.u.boolean;
                }
            }
        }
    }
    toml_free(r);
}

static void
append_managed_layer_rules(GString                 *rules_doc,
                           const char *const       *ns_ids,
                           const ManagedLayerProps *props,
                           size_t                   n_ns)
{
    for (size_t i = 0; i < n_ns; i++) {
        char op_buf[32];
        int pct = CLAMP(props[i].opacity_pct, 0, 100);
        g_ascii_formatd(op_buf, sizeof(op_buf), "%.2f", (double)pct / 100.0);
        g_autofree gchar *pat = g_strdup_printf("^%s$", ns_ids[i] ? ns_ids[i] : "");

        g_string_append(rules_doc, "\n[[" COMP_SEC_LAYER_RULE "]]\n" COMP_KEY_NAMESPACE " = [");
        kajo_toml_append_quoted_string(rules_doc, pat);
        g_string_append_printf(rules_doc, "]\n" COMP_KEY_OPACITY " = %s\n", op_buf);
        if (props[i].has_popup_opacity)
            g_string_append_printf(rules_doc, COMP_KEY_POPUP_OPACITY " = %s\n", op_buf);
        if (props[i].has_blur)
            g_string_append_printf(rules_doc, COMP_KEY_BLUR " = %s\n",
                                   props[i].blur ? "true" : "false");
        if (props[i].has_blur_popups)
            g_string_append_printf(rules_doc, COMP_KEY_BLUR_POPUPS " = %s\n",
                                   props[i].blur_popups ? "true" : "false");
        if (props[i].has_xray)
            g_string_append_printf(rules_doc, COMP_KEY_XRAY " = %s\n",
                                   props[i].xray ? "true" : "false");
        if (props[i].has_shadow)
            g_string_append_printf(rules_doc, COMP_KEY_SHADOW " = %s\n",
                                   props[i].shadow ? "true" : "false");
    }
}

gboolean
kajo_config_comp_save_layout_and_layers(const char        *config_dir,
                                        int                gap_px,
                                        const char *const *ns_ids,
                                        const int         *ns_opacity_pct,
                                        size_t             n_ns,
                                        GError           **error)
{
    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create config directory '%s'", dir);
        return FALSE;
    }

    /* 1. Patch [layout].gap_px in comp.toml in place (seeded from defaults/comp.toml if new) */
    g_autofree gchar *comp_path = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
    g_autofree gchar *comp_existing = NULL;
    if (!g_file_get_contents(comp_path, &comp_existing, NULL, NULL)) {
        g_autofree gchar *def_comp = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
        if (def_comp)
            (void)g_file_get_contents(def_comp, &comp_existing, NULL, NULL);
    }
    GString *comp_doc = g_string_new(comp_existing ? comp_existing :
                                     "# comp.toml — Kajo Compositor Configuration\n");
    g_autofree gchar *gap_rhs = g_strdup_printf("%d", CLAMP(gap_px, 0, 200));
    kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_LAYOUT, COMP_KEY_GAP_PX, gap_rhs);
    gboolean ok_comp = g_file_set_contents(comp_path, comp_doc->str, (gssize)comp_doc->len, error);
    g_string_free(comp_doc, TRUE);
    if (!ok_comp)
        return FALSE;

    /* 2. Update [[layer_rule]] blocks in rules.toml while preserving [[rule]] blocks and blur/xray flags */
    if (ns_ids && ns_opacity_pct && n_ns > 0) {
        g_autofree gchar *rules_path = g_build_filename(dir, COMP_CFG_FILE_RULES, NULL);
        g_autofree gchar *def_rules_path = kajo_config_find_default_file(COMP_CFG_FILE_RULES);
        g_autofree gchar *def_rules_str = NULL;
        if (def_rules_path)
            (void)g_file_get_contents(def_rules_path, &def_rules_str, NULL, NULL);

        g_autofree gchar *rules_existing = NULL;
        if (!g_file_get_contents(rules_path, &rules_existing, NULL, NULL) && def_rules_str)
            rules_existing = g_strdup(def_rules_str);

        g_autofree ManagedLayerProps *props = g_new0(ManagedLayerProps, n_ns);
        for (size_t i = 0; i < n_ns; i++)
            init_default_managed_layer_props(ns_ids[i], &props[i]);
        apply_layer_rules_doc_to_props(def_rules_str, ns_ids, props, n_ns);
        apply_layer_rules_doc_to_props(rules_existing, ns_ids, props, n_ns);
        for (size_t i = 0; i < n_ns; i++)
            props[i].opacity_pct = CLAMP(ns_opacity_pct[i], 0, 100);

        GString *rules_doc = strip_managed_layer_rules_from_doc(rules_existing, ns_ids, n_ns);
        append_managed_layer_rules(rules_doc, ns_ids, props, n_ns);
        gboolean ok_rules = g_file_set_contents(rules_path, rules_doc->str, (gssize)rules_doc->len, error);
        g_string_free(rules_doc, TRUE);
        if (!ok_rules)
            return FALSE;
    }

    /* Remove legacy conf.d/40-layer-rules.toml written by older versions */
    g_autofree gchar *legacy_dropin = g_build_filename(dir, COMP_CFG_DIR_CONFD, "40-layer-rules.toml", NULL);
    if (g_file_test(legacy_dropin, G_FILE_TEST_EXISTS))
        g_unlink(legacy_dropin);

    return TRUE;
}

static gboolean
toml_datum_as_double(toml_datum_t d, double *out)
{
    if (d.type == TOML_FP64 && isfinite(d.u.fp64)) {
        *out = d.u.fp64;
        return TRUE;
    }
    if (d.type == TOML_INT64) {
        *out = (double)d.u.int64;
        return TRUE;
    }
    return FALSE;
}

static void
apply_effects_table_to_state(toml_datum_t        tab,
                             KajoCompEffectsCfg *inout_fx,
                             const char *const  *ns_ids,
                             gboolean           *inout_ns_xray,
                             size_t              n_ns)
{
    if (tab.type != TOML_TABLE)
        return;

    if (inout_fx) {
        toml_datum_t ltab = toml_get(tab, COMP_SEC_LAYOUT);
        if (ltab.type == TOML_TABLE &&
            toml_get(ltab, COMP_KEY_LAYOUT_LEFT).type == TOML_UNKNOWN) {
            toml_datum_t bpx = toml_get(ltab, COMP_KEY_BORDER_PX);
            if (bpx.type == TOML_INT64 && bpx.u.int64 >= 0 && bpx.u.int64 <= 32)
                inout_fx->border_px = (int)bpx.u.int64;
        }

        toml_datum_t fx = toml_get(tab, COMP_SEC_EFFECTS);
        if (fx.type == TOML_TABLE) {
            double dv = 0.0;
            toml_datum_t di = toml_get(fx, COMP_KEY_DITHER);
            if (di.type == TOML_BOOLEAN)
                inout_fx->dither_enabled = di.u.boolean;

            toml_datum_t cf = toml_get(fx, COMP_KEY_COLOR_FILTER);
            if (cf.type == TOML_STRING && cf.u.s && *cf.u.s)
                g_strlcpy(inout_fx->color_filter, cf.u.s, sizeof(inout_fx->color_filter));

            if (toml_datum_as_double(toml_get(fx, COMP_KEY_COLOR_FILTER_STRENGTH), &dv) &&
                dv >= 0.0 && dv <= 1.0)
                inout_fx->color_filter_strength_pct = (int)(dv * 100.0 + 0.5);

            toml_datum_t nlt = toml_get(fx, COMP_KEY_NIGHT_LIGHT_TEMPERATURE);
            if (nlt.type == TOML_INT64 && nlt.u.int64 >= 1500 && nlt.u.int64 <= 6500)
                inout_fx->night_light_temperature = (int)nlt.u.int64;

            toml_datum_t gl = toml_get(fx, COMP_SEC_FX_GLASS);
            if (gl.type == TOML_TABLE) {
                toml_datum_t gen = toml_get(gl, COMP_KEY_ENABLED);
                if (gen.type == TOML_BOOLEAN)
                    inout_fx->glass_enabled = gen.u.boolean;
                toml_datum_t gp = toml_get(gl, COMP_KEY_PASSES);
                if (gp.type == TOML_INT64 && gp.u.int64 >= 1 && gp.u.int64 <= 6)
                    inout_fx->glass_passes = (int)gp.u.int64;
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_OFFSET), &dv) &&
                    dv >= 0.5 && dv <= 8.0)
                    inout_fx->glass_offset = dv;
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_BRIGHTNESS), &dv) &&
                    dv >= 0.0 && dv <= 2.0)
                    inout_fx->glass_brightness_pct = (int)(dv * 100.0 + 0.5);
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_SATURATION), &dv) &&
                    dv >= 0.0 && dv <= 2.0)
                    inout_fx->glass_saturation_pct = (int)(dv * 100.0 + 0.5);
                toml_datum_t gt = toml_get(gl, COMP_KEY_TINT);
                if (gt.type == TOML_STRING && gt.u.s && kajo_config_is_valid_hex_color(gt.u.s))
                    g_strlcpy(inout_fx->glass_tint, gt.u.s, sizeof(inout_fx->glass_tint));
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_TINT_ALPHA), &dv) &&
                    dv >= 0.0 && dv <= 1.0)
                    inout_fx->glass_tint_alpha_pct = (int)(dv * 100.0 + 0.5);
                toml_datum_t gst = toml_get(gl, COMP_KEY_STYLE);
                if (gst.type == TOML_STRING && gst.u.s && gst.u.s[0])
                    g_strlcpy(inout_fx->glass_style, gst.u.s, sizeof(inout_fx->glass_style));
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_REFRACTION), &dv) &&
                    dv >= 0.0 && dv <= 2.0)
                    inout_fx->glass_refraction_pct = (int)(dv * 100.0 + 0.5);
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_DISPERSION), &dv) &&
                    dv >= 0.0 && dv <= 1.0)
                    inout_fx->glass_dispersion_pct = (int)(dv * 100.0 + 0.5);
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_BEVEL_PX), &dv) &&
                    dv >= 2.0 && dv <= 64.0)
                    inout_fx->glass_bevel_px = (int)(dv + 0.5);
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_SPECULAR), &dv) &&
                    dv >= 0.0 && dv <= 2.0)
                    inout_fx->glass_specular_pct = (int)(dv * 100.0 + 0.5);
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_GRAIN), &dv) &&
                    dv >= 0.0 && dv <= 1.0)
                    inout_fx->glass_grain_pct = (int)(dv * 100.0 + 0.5);
                if (toml_datum_as_double(toml_get(gl, COMP_KEY_FLUTE_PX), &dv) &&
                    dv >= 4.0 && dv <= 64.0)
                    inout_fx->glass_flute_px = (int)(dv + 0.5);
                toml_datum_t gfa = toml_get(gl, COMP_KEY_FLUTE_AXIS);
                if (gfa.type == TOML_STRING && gfa.u.s && gfa.u.s[0])
                    g_strlcpy(inout_fx->glass_flute_axis, gfa.u.s, sizeof(inout_fx->glass_flute_axis));
            }
            toml_datum_t sh = toml_get(fx, COMP_SEC_FX_SHADOW);
            if (sh.type == TOML_TABLE) {
                toml_datum_t sen = toml_get(sh, COMP_KEY_ENABLED);
                if (sen.type == TOML_BOOLEAN)
                    inout_fx->shadow_enabled = sen.u.boolean;
                if (toml_datum_as_double(toml_get(sh, COMP_KEY_STRENGTH), &dv) &&
                    dv >= 0.0 && dv <= 2.0)
                    inout_fx->shadow_strength_pct = (int)(dv * 100.0 + 0.5);
                if (toml_datum_as_double(toml_get(sh, COMP_KEY_UNFOCUSED_SCALE), &dv) &&
                    dv >= 0.0 && dv <= 2.0)
                    inout_fx->shadow_unfocused_scale_pct = (int)(dv * 100.0 + 0.5);
            }
            toml_datum_t dm = toml_get(fx, COMP_SEC_FX_DIM);
            if (dm.type == TOML_TABLE) {
                toml_datum_t den = toml_get(dm, COMP_KEY_ENABLED);
                if (den.type == TOML_BOOLEAN)
                    inout_fx->dim_enabled = den.u.boolean;
                if (toml_datum_as_double(toml_get(dm, COMP_KEY_OPACITY), &dv) &&
                    dv >= 0.0 && dv <= 1.0)
                    inout_fx->dim_opacity_pct = (int)(dv * 100.0 + 0.5);
            }
            toml_datum_t tr = toml_get(fx, COMP_SEC_FX_TRANSITIONS);
            if (tr.type == TOML_TABLE) {
                toml_datum_t op = toml_get(tr, COMP_KEY_OPEN_PRESET);
                if (op.type == TOML_STRING && op.u.s && *op.u.s)
                    g_strlcpy(inout_fx->open_preset, op.u.s, sizeof(inout_fx->open_preset));
                toml_datum_t od = toml_get(tr, COMP_KEY_OPEN_DURATION_MS);
                if (od.type == TOML_INT64 && od.u.int64 >= 50 && od.u.int64 <= 2000)
                    inout_fx->open_duration_ms = (int)od.u.int64;
                toml_datum_t oc = toml_get(tr, COMP_KEY_OPEN_CURVE);
                if (oc.type == TOML_STRING && oc.u.s && *oc.u.s)
                    g_strlcpy(inout_fx->open_curve, oc.u.s, sizeof(inout_fx->open_curve));
                toml_datum_t cp = toml_get(tr, COMP_KEY_CLOSE_PRESET);
                if (cp.type == TOML_STRING && cp.u.s && *cp.u.s)
                    g_strlcpy(inout_fx->close_preset, cp.u.s, sizeof(inout_fx->close_preset));
                toml_datum_t cd = toml_get(tr, COMP_KEY_CLOSE_DURATION_MS);
                if (cd.type == TOML_INT64 && cd.u.int64 >= 50 && cd.u.int64 <= 2000)
                    inout_fx->close_duration_ms = (int)cd.u.int64;
                toml_datum_t cc = toml_get(tr, COMP_KEY_CLOSE_CURVE);
                if (cc.type == TOML_STRING && cc.u.s && *cc.u.s)
                    g_strlcpy(inout_fx->close_curve, cc.u.s, sizeof(inout_fx->close_curve));
                toml_datum_t rp = toml_get(tr, COMP_KEY_RESIZE_PRESET);
                if (rp.type == TOML_STRING && rp.u.s && *rp.u.s)
                    g_strlcpy(inout_fx->resize_preset, rp.u.s, sizeof(inout_fx->resize_preset));
            }
        }
    }

    if (ns_ids && inout_ns_xray && n_ns > 0) {
        toml_datum_t l_arr = toml_get(tab, COMP_SEC_LAYER_RULE);
        if (l_arr.type == TOML_ARRAY) {
            for (int li = 0; li < l_arr.u.arr.size; li++) {
                toml_datum_t lr = l_arr.u.arr.elem[li];
                toml_datum_t xr = toml_get(lr, COMP_KEY_XRAY);
                if (xr.type != TOML_BOOLEAN)
                    continue;
                for (size_t k = 0; k < n_ns; k++) {
                    if (layer_table_matches_ns_id(lr, ns_ids[k]))
                        inout_ns_xray[k] = xr.u.boolean;
                }
            }
        }
    }
}

static void
apply_effects_file_if_exists(const char         *path,
                             KajoCompEffectsCfg *inout_fx,
                             const char *const  *ns_ids,
                             gboolean           *inout_ns_xray,
                             size_t              n_ns)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    g_autofree gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL) || !contents)
        return;

    toml_result_t r = toml_parse(contents, (int)len);
    if (r.ok) {
        apply_effects_table_to_state(r.toptab, inout_fx, ns_ids, inout_ns_xray, n_ns);
        apply_effects_table_to_state(toml_get(r.toptab, COMP_SEC_COMP),
                                     inout_fx, ns_ids, inout_ns_xray, n_ns);
    }
    toml_free(r);
}

gboolean
kajo_config_comp_read_effects(const char         *config_dir,
                              KajoCompEffectsCfg *out_fx,
                              const char *const  *ns_ids,
                              gboolean           *inout_ns_xray,
                              size_t              n_ns)
{
    if (out_fx) {
        out_fx->border_px                  = 1;
        out_fx->glass_enabled              = TRUE;
        g_strlcpy(out_fx->glass_style, "frosted", sizeof(out_fx->glass_style));
        out_fx->glass_passes               = 3;
        out_fx->glass_offset               = 2.0;
        out_fx->glass_brightness_pct       = 90;
        out_fx->glass_saturation_pct       = 115;
        g_strlcpy(out_fx->glass_tint, "#141414", sizeof(out_fx->glass_tint));
        out_fx->glass_tint_alpha_pct       = 35;
        out_fx->glass_refraction_pct       = 45;
        out_fx->glass_dispersion_pct       = 25;
        out_fx->glass_bevel_px             = 14;
        out_fx->glass_specular_pct         = 55;
        out_fx->glass_grain_pct            = 35;
        out_fx->glass_flute_px             = 14;
        g_strlcpy(out_fx->glass_flute_axis, "vertical", sizeof(out_fx->glass_flute_axis));
        out_fx->shadow_enabled             = TRUE;
        out_fx->shadow_strength_pct        = 100;
        out_fx->shadow_unfocused_scale_pct = 60;
        out_fx->dim_enabled                = FALSE;
        out_fx->dim_opacity_pct            = 15;
        out_fx->dither_enabled             = TRUE;
        g_strlcpy(out_fx->color_filter, "none", sizeof(out_fx->color_filter));
        out_fx->color_filter_strength_pct  = 100;
        out_fx->night_light_temperature    = 4000;
        g_strlcpy(out_fx->open_preset, "default", sizeof(out_fx->open_preset));
        out_fx->open_duration_ms           = 150;
        g_strlcpy(out_fx->open_curve, "ease-out-expo", sizeof(out_fx->open_curve));
        g_strlcpy(out_fx->close_preset, "default", sizeof(out_fx->close_preset));
        out_fx->close_duration_ms          = 200;
        g_strlcpy(out_fx->close_curve, "ease-out-quad", sizeof(out_fx->close_curve));
        g_strlcpy(out_fx->resize_preset, "default", sizeof(out_fx->resize_preset));
    }
    if (ns_ids && inout_ns_xray) {
        for (size_t i = 0; i < n_ns; i++)
            inout_ns_xray[i] = (g_strcmp0(ns_ids[i], "kajo") == 0);
    }

    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();

    g_autofree gchar *def_comp  = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
    g_autofree gchar *def_rules = kajo_config_find_default_file(COMP_CFG_FILE_RULES);
    apply_effects_file_if_exists(def_comp,  out_fx, ns_ids, inout_ns_xray, n_ns);
    apply_effects_file_if_exists(def_rules, out_fx, ns_ids, inout_ns_xray, n_ns);

    g_autofree gchar *usr_comp  = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
    g_autofree gchar *usr_rules = g_build_filename(dir, COMP_CFG_FILE_RULES, NULL);
    apply_effects_file_if_exists(usr_comp,  out_fx, ns_ids, inout_ns_xray, n_ns);
    apply_effects_file_if_exists(usr_rules, out_fx, ns_ids, inout_ns_xray, n_ns);

    g_autofree gchar *confd_dir = g_build_filename(dir, COMP_CFG_DIR_CONFD, NULL);
    GDir *d = g_dir_open(confd_dir, 0, NULL);
    if (d) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        const gchar *name = NULL;
        while ((name = g_dir_read_name(d)) != NULL) {
            if (name[0] != '.' && g_str_has_suffix(name, COMP_CFG_EXT_TOML))
                g_ptr_array_add(names, g_strdup(name));
        }
        g_dir_close(d);
        g_ptr_array_sort(names, cmp_str_ptrs);
        for (guint i = 0; i < names->len; i++) {
            const gchar *fn = (const gchar *)g_ptr_array_index(names, i);
            g_autofree gchar *p = g_build_filename(confd_dir, fn, NULL);
            apply_effects_file_if_exists(p, out_fx, ns_ids, inout_ns_xray, n_ns);
        }
        g_ptr_array_free(names, TRUE);
    }
    return TRUE;
}

gboolean
kajo_config_comp_save_effects(const char               *config_dir,
                              const KajoCompEffectsCfg *fx,
                              const char *const        *ns_ids,
                              const gboolean           *ns_xray,
                              size_t                    n_ns,
                              GError                  **error)
{
    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create config directory '%s'", dir);
        return FALSE;
    }

    if (fx) {
        g_autofree gchar *comp_path = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
        g_autofree gchar *comp_existing = NULL;
        if (!g_file_get_contents(comp_path, &comp_existing, NULL, NULL)) {
            g_autofree gchar *def_comp = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
            if (def_comp)
                (void)g_file_get_contents(def_comp, &comp_existing, NULL, NULL);
        }
        GString *comp_doc = g_string_new(comp_existing ? comp_existing :
                                         "# comp.toml — Kajo Compositor Configuration\n");

        g_autofree gchar *bpx_rhs = g_strdup_printf("%d", CLAMP(fx->border_px, 0, 32));
        kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_LAYOUT, COMP_KEY_BORDER_PX, bpx_rhs);

        kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_EFFECTS, COMP_KEY_DITHER,
                                         fx->dither_enabled ? "true" : "false");

        GString *cf_rhs = g_string_new(NULL);
        kajo_toml_append_quoted_string(cf_rhs, fx->color_filter[0] ? fx->color_filter : "none");
        kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_EFFECTS, COMP_KEY_COLOR_FILTER,
                                         cf_rhs->str);
        g_string_free(cf_rhs, TRUE);

        char num_buf[32];
        int str_pct = CLAMP(fx->color_filter_strength_pct, 0, 100);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)str_pct / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_EFFECTS, COMP_KEY_COLOR_FILTER_STRENGTH,
                                         num_buf);

        g_autofree gchar *temp_rhs = g_strdup_printf("%d", CLAMP(fx->night_light_temperature, 1500, 6500));
        kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_EFFECTS, COMP_KEY_NIGHT_LIGHT_TEMPERATURE,
                                         temp_rhs);

        const char *sec_glass = COMP_SEC_EFFECTS "." COMP_SEC_FX_GLASS;
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass,
                                         COMP_KEY_ENABLED, fx->glass_enabled ? "true" : "false");
        g_autofree gchar *gp_rhs = g_strdup_printf("%d", CLAMP(fx->glass_passes, 1, 6));
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_PASSES, gp_rhs);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", CLAMP(fx->glass_offset, 0.5, 8.0));
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_OFFSET, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->glass_brightness_pct, 50, 150) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_BRIGHTNESS, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->glass_saturation_pct, 50, 200) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_SATURATION, num_buf);
        if (kajo_config_is_valid_hex_color(fx->glass_tint)) {
            GString *gt_rhs = g_string_new(NULL);
            kajo_toml_append_quoted_string(gt_rhs, fx->glass_tint);
            kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_TINT, gt_rhs->str);
            g_string_free(gt_rhs, TRUE);
        }
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->glass_tint_alpha_pct, 0, 100) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_TINT_ALPHA, num_buf);
        {
            GString *gs_rhs = g_string_new(NULL);
            kajo_toml_append_quoted_string(gs_rhs, fx->glass_style[0] ? fx->glass_style : "frosted");
            kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_STYLE, gs_rhs->str);
            g_string_free(gs_rhs, TRUE);
        }
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->glass_refraction_pct, 0, 200) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_REFRACTION, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->glass_dispersion_pct, 0, 100) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_DISPERSION, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.1f", (double)CLAMP(fx->glass_bevel_px, 2, 64));
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_BEVEL_PX, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->glass_specular_pct, 0, 200) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_SPECULAR, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->glass_grain_pct, 0, 100) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_GRAIN, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.1f", (double)CLAMP(fx->glass_flute_px, 4, 64));
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_FLUTE_PX, num_buf);
        {
            GString *fa_rhs = g_string_new(NULL);
            kajo_toml_append_quoted_string(fa_rhs, fx->glass_flute_axis[0] ? fx->glass_flute_axis : "vertical");
            kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_FLUTE_AXIS, fa_rhs->str);
            g_string_free(fa_rhs, TRUE);
        }

        const char *sec_shadow = COMP_SEC_EFFECTS "." COMP_SEC_FX_SHADOW;
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_shadow,
                                         COMP_KEY_ENABLED, fx->shadow_enabled ? "true" : "false");
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->shadow_strength_pct, 0, 200) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_shadow, COMP_KEY_STRENGTH, num_buf);
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->shadow_unfocused_scale_pct, 0, 200) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_shadow, COMP_KEY_UNFOCUSED_SCALE, num_buf);

        const char *sec_dim = COMP_SEC_EFFECTS "." COMP_SEC_FX_DIM;
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_dim,
                                         COMP_KEY_ENABLED, fx->dim_enabled ? "true" : "false");
        g_ascii_formatd(num_buf, sizeof(num_buf), "%.2f", (double)CLAMP(fx->dim_opacity_pct, 0, 100) / 100.0);
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_dim, COMP_KEY_OPACITY, num_buf);

        const char *sec_tr = COMP_SEC_EFFECTS "." COMP_SEC_FX_TRANSITIONS;
        GString *tr_rhs = g_string_new(NULL);
        kajo_toml_append_quoted_string(tr_rhs, fx->open_preset[0] ? fx->open_preset : "default");
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_tr, COMP_KEY_OPEN_PRESET, tr_rhs->str);
        g_autofree gchar *od_rhs = g_strdup_printf("%d", CLAMP(fx->open_duration_ms > 0 ? fx->open_duration_ms : 150, 50, 2000));
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_tr, COMP_KEY_OPEN_DURATION_MS, od_rhs);
        g_string_truncate(tr_rhs, 0);
        kajo_toml_append_quoted_string(tr_rhs, fx->open_curve[0] ? fx->open_curve : "ease-out-expo");
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_tr, COMP_KEY_OPEN_CURVE, tr_rhs->str);
        g_string_truncate(tr_rhs, 0);
        kajo_toml_append_quoted_string(tr_rhs, fx->close_preset[0] ? fx->close_preset : "default");
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_tr, COMP_KEY_CLOSE_PRESET, tr_rhs->str);
        g_autofree gchar *cd_rhs = g_strdup_printf("%d", CLAMP(fx->close_duration_ms > 0 ? fx->close_duration_ms : 200, 50, 2000));
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_tr, COMP_KEY_CLOSE_DURATION_MS, cd_rhs);
        g_string_truncate(tr_rhs, 0);
        kajo_toml_append_quoted_string(tr_rhs, fx->close_curve[0] ? fx->close_curve : "ease-out-quad");
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_tr, COMP_KEY_CLOSE_CURVE, tr_rhs->str);
        g_string_truncate(tr_rhs, 0);
        kajo_toml_append_quoted_string(tr_rhs, fx->resize_preset[0] ? fx->resize_preset : "default");
        kajo_toml_patch_or_insert_scalar(comp_doc, sec_tr, COMP_KEY_RESIZE_PRESET, tr_rhs->str);
        g_string_free(tr_rhs, TRUE);

        gboolean ok_comp = g_file_set_contents(comp_path, comp_doc->str, (gssize)comp_doc->len, error);
        g_string_free(comp_doc, TRUE);
        if (!ok_comp)
            return FALSE;
    }

    if (ns_ids && ns_xray && n_ns > 0) {
        g_autofree gchar *rules_path = g_build_filename(dir, COMP_CFG_FILE_RULES, NULL);
        g_autofree gchar *def_rules_path = kajo_config_find_default_file(COMP_CFG_FILE_RULES);
        g_autofree gchar *def_rules_str = NULL;
        if (def_rules_path)
            (void)g_file_get_contents(def_rules_path, &def_rules_str, NULL, NULL);

        g_autofree gchar *rules_existing = NULL;
        if (!g_file_get_contents(rules_path, &rules_existing, NULL, NULL) && def_rules_str)
            rules_existing = g_strdup(def_rules_str);

        g_autofree ManagedLayerProps *props = g_new0(ManagedLayerProps, n_ns);
        for (size_t i = 0; i < n_ns; i++)
            init_default_managed_layer_props(ns_ids[i], &props[i]);
        apply_layer_rules_doc_to_props(def_rules_str, ns_ids, props, n_ns);
        apply_layer_rules_doc_to_props(rules_existing, ns_ids, props, n_ns);
        for (size_t i = 0; i < n_ns; i++) {
            props[i].has_xray = TRUE;
            props[i].xray     = ns_xray[i] ? TRUE : FALSE;
        }

        GString *rules_doc = strip_managed_layer_rules_from_doc(rules_existing, ns_ids, n_ns);
        append_managed_layer_rules(rules_doc, ns_ids, props, n_ns);
        gboolean ok_rules = g_file_set_contents(rules_path, rules_doc->str, (gssize)rules_doc->len, error);
        g_string_free(rules_doc, TRUE);
        if (!ok_rules)
            return FALSE;
    }

    return TRUE;
}

gboolean
kajo_config_comp_save_inactive_border_color(const char *config_dir,
                                            const char *hex_color,
                                            GError    **error)
{
    if (!hex_color || !kajo_config_is_valid_hex_color(hex_color))
        return FALSE;

    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create config directory '%s'", dir);
        return FALSE;
    }

    g_autofree gchar *comp_path = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
    g_autofree gchar *comp_existing = NULL;
    if (!g_file_get_contents(comp_path, &comp_existing, NULL, NULL)) {
        g_autofree gchar *def_comp = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
        if (def_comp)
            (void)g_file_get_contents(def_comp, &comp_existing, NULL, NULL);
    }

    GString *comp_doc = g_string_new(comp_existing ? comp_existing : "");
    GString *rhs = g_string_new(NULL);
    kajo_toml_append_quoted_string(rhs, hex_color);
    kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_LAYOUT,
                                     COMP_KEY_INACTIVE_BORDER_COLOR, rhs->str);
    g_string_free(rhs, TRUE);

    gboolean ok = g_file_set_contents(comp_path, comp_doc->str, (gssize)comp_doc->len, error);
    g_string_free(comp_doc, TRUE);
    return ok;
}

gboolean
kajo_config_comp_save_glass_tint(const char *config_dir,
                                 const char *hex_color,
                                 GError    **error)
{
    if (!hex_color || !kajo_config_is_valid_hex_color(hex_color))
        return FALSE;

    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create config directory '%s'", dir);
        return FALSE;
    }

    g_autofree gchar *comp_path = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
    g_autofree gchar *comp_existing = NULL;
    if (!g_file_get_contents(comp_path, &comp_existing, NULL, NULL)) {
        g_autofree gchar *def_comp = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
        if (def_comp)
            (void)g_file_get_contents(def_comp, &comp_existing, NULL, NULL);
    }

    const char *sec_glass = COMP_SEC_EFFECTS "." COMP_SEC_FX_GLASS;
    GString *comp_doc = g_string_new(comp_existing ? comp_existing : "");
    GString *rhs = g_string_new(NULL);
    kajo_toml_append_quoted_string(rhs, hex_color);
    kajo_toml_patch_or_insert_scalar(comp_doc, sec_glass, COMP_KEY_TINT, rhs->str);
    g_string_free(rhs, TRUE);

    gboolean ok = g_file_set_contents(comp_path, comp_doc->str, (gssize)comp_doc->len, error);
    g_string_free(comp_doc, TRUE);
    return ok;
}

static gboolean
output_rule_match_equals(const char *cand, const char *out_name, const char *make_model)
{
    if (!cand || !*cand)
        return FALSE;
    if (out_name && *out_name && g_ascii_strcasecmp(cand, out_name) == 0)
        return TRUE;
    if (make_model && *make_model && g_ascii_strcasecmp(cand, make_model) == 0)
        return TRUE;
    return FALSE;
}

static void
append_output_kv_quoted(GString *out, const char *key, const char *val)
{
    g_string_append_printf(out, "%s = ", key);
    kajo_toml_append_quoted_string(out, val);
    g_string_append_c(out, '\n');
}

static gboolean
block_is_matching_output_rule(const char *block_str, size_t block_len,
                              const char *out_name, const char *make_model)
{
    g_autofree gchar *tmp = g_strndup(block_str, block_len);
    toml_result_t r = toml_parse(tmp, (int)block_len);
    gboolean matched = FALSE;
    if (r.ok) {
        toml_datum_t arr = toml_get(r.toptab, COMP_SEC_OUTPUT);
        if (arr.type == TOML_ARRAY && arr.u.arr.size == 1 && arr.u.arr.elem[0].type == TOML_TABLE) {
            toml_datum_t mv = toml_get(arr.u.arr.elem[0], COMP_KEY_MATCH);
            const char *m_str = (mv.type == TOML_STRING && mv.u.s) ? mv.u.s : "";
            matched = (!*m_str || output_rule_match_equals(m_str, out_name, make_model));
        }
    }
    toml_free(r);
    return matched;
}

static GString *
strip_matching_output_rules_from_doc(const char *doc, const char *out_name, const char *make_model)
{
    GString *out = g_string_new(NULL);
    if (!doc || !*doc) {
        g_string_append(out, "# Managed by kajo-settings — unattached host outputs preserved\n");
        return out;
    }

    size_t len = strlen(doc);
    size_t block_start = 0;
    size_t pos = 0;

    while (pos <= len) {
        const char *nl = (pos < len) ? memchr(doc + pos, '\n', len - pos) : NULL;
        size_t line_end = nl ? (size_t)(nl - doc) : len;
        size_t next_pos = nl ? (line_end + 1) : (len + 1);

        size_t i = pos;
        while (i < line_end && (doc[i] == ' ' || doc[i] == '\t'))
            i++;
        gboolean is_hdr = (i < line_end && doc[i] == '[');

        if ((is_hdr && pos > block_start) || pos == len) {
            size_t blen = pos - block_start;
            if (blen > 0 && !block_is_matching_output_rule(doc + block_start, blen, out_name, make_model))
                g_string_append_len(out, doc + block_start, (gssize)blen);
            block_start = pos;
        }
        pos = next_pos;
    }

    while (out->len > 0 && (out->str[out->len - 1] == '\n' || out->str[out->len - 1] == '\r'))
        g_string_truncate(out, out->len - 1);
    if (out->len == 0)
        g_string_append(out, "# Managed by kajo-settings — unattached host outputs preserved");
    g_string_append_c(out, '\n');
    return out;
}

gboolean
kajo_config_comp_save_output_rule(const char *config_dir,
                                  const char *out_name,
                                  const char *make_model,
                                  const char *mode_str,
                                  const char *scale_buf,
                                  const char *transform_str,
                                  const char *vrr_str,
                                  const char *hdr_str,
                                  int         hdr_peak_nits,
                                  GError    **error)
{
    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create config directory '%s'", dir);
        return FALSE;
    }

    g_autofree gchar *outputs_path = g_build_filename(dir, COMP_CFG_FILE_OUTPUTS, NULL);
    g_autofree gchar *existing = NULL;
    (void)g_file_get_contents(outputs_path, &existing, NULL, NULL);
    GString *out_s = strip_matching_output_rules_from_doc(existing, out_name, make_model);

    const char *norm_mode = (g_strcmp0(mode_str, "auto") == 0) ? COMP_VAL_PREFERRED : mode_str;
    g_string_append(out_s, "\n[[" COMP_SEC_OUTPUT "]]\n");
    append_output_kv_quoted(out_s, COMP_KEY_MATCH, out_name ? out_name : "");
    if (norm_mode && *norm_mode)
        append_output_kv_quoted(out_s, COMP_KEY_MODE, norm_mode);
    if (scale_buf && *scale_buf)
        g_string_append_printf(out_s, "%s = %s\n", COMP_KEY_SCALE, scale_buf);
    if (transform_str && *transform_str)
        append_output_kv_quoted(out_s, COMP_KEY_TRANSFORM, transform_str);
    if (vrr_str && *vrr_str)
        append_output_kv_quoted(out_s, COMP_KEY_VRR, vrr_str);
    if (hdr_str && *hdr_str)
        append_output_kv_quoted(out_s, COMP_KEY_HDR, hdr_str);
    if (hdr_peak_nits >= 100 && hdr_peak_nits <= 10000)
        g_string_append_printf(out_s, "%s = %d\n", COMP_KEY_HDR_PEAK_NITS, hdr_peak_nits);

    gboolean ok = g_file_set_contents(outputs_path, out_s->str, (gssize)out_s->len, error);
    g_string_free(out_s, TRUE);
    return ok;
}

int
kajo_config_comp_read_sdr_white_nits(const char *config_dir)
{
    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();
    g_autofree gchar *comp_path = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
    g_autofree gchar *comp_text = NULL;
    if (!g_file_get_contents(comp_path, &comp_text, NULL, NULL)) {
        g_autofree gchar *def_comp = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
        if (def_comp)
            (void)g_file_get_contents(def_comp, &comp_text, NULL, NULL);
    }

    int result = 203;
    if (!comp_text || !*comp_text)
        return result;

    toml_result_t r = toml_parse(comp_text, (int)strlen(comp_text));
    if (r.ok) {
        toml_datum_t fx = toml_get(r.toptab, COMP_SEC_EFFECTS);
        double dv = 0.0;
        if (fx.type == TOML_TABLE &&
            toml_datum_as_double(toml_get(fx, COMP_KEY_SDR_WHITE_NITS), &dv) &&
            dv >= 80.0 && dv <= 1000.0) {
            result = (int)(dv + 0.5);
        }
    }
    toml_free(r);
    return result;
}

gboolean
kajo_config_comp_save_sdr_white_nits(const char *config_dir,
                                     int         sdr_white_nits,
                                     GError    **error)
{
    if (sdr_white_nits != 0 && (sdr_white_nits < 80 || sdr_white_nits > 1000)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "sdr_white_nits must be 0 or in 80..1000 (got %d)", sdr_white_nits);
        return FALSE;
    }

    g_autofree gchar *dir = (config_dir && *config_dir)
        ? g_strdup(config_dir)
        : kajo_config_resolve_user_dir();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create config directory '%s'", dir);
        return FALSE;
    }

    g_autofree gchar *comp_path = g_build_filename(dir, COMP_CFG_FILE_COMP, NULL);
    g_autofree gchar *comp_existing = NULL;
    if (!g_file_get_contents(comp_path, &comp_existing, NULL, NULL)) {
        g_autofree gchar *def_comp = kajo_config_find_default_file(COMP_CFG_FILE_COMP);
        if (def_comp)
            (void)g_file_get_contents(def_comp, &comp_existing, NULL, NULL);
    }

    GString *comp_doc = g_string_new(comp_existing ? comp_existing : "");
    char val_buf[32];
    snprintf(val_buf, sizeof(val_buf), "%d.0", sdr_white_nits);
    kajo_toml_patch_or_insert_scalar(comp_doc, COMP_SEC_EFFECTS, COMP_KEY_SDR_WHITE_NITS, val_buf);

    gboolean ok = g_file_set_contents(comp_path, comp_doc->str, (gssize)comp_doc->len, error);
    g_string_free(comp_doc, TRUE);
    return ok;
}
