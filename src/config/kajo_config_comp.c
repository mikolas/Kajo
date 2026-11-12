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

    /* 2. Update [[layer_rule]] blocks in rules.toml while preserving [[rule]] blocks */
    if (ns_ids && ns_opacity_pct && n_ns > 0) {
        g_autofree gchar *rules_path = g_build_filename(dir, COMP_CFG_FILE_RULES, NULL);
        g_autofree gchar *rules_existing = NULL;
        if (!g_file_get_contents(rules_path, &rules_existing, NULL, NULL)) {
            g_autofree gchar *def_rules = kajo_config_find_default_file(COMP_CFG_FILE_RULES);
            if (def_rules)
                (void)g_file_get_contents(def_rules, &rules_existing, NULL, NULL);
        }
        GString *rules_doc = strip_managed_layer_rules_from_doc(rules_existing, ns_ids, n_ns);
        for (size_t i = 0; i < n_ns; i++) {
            char op_buf[32];
            int pct = CLAMP(ns_opacity_pct[i], 0, 100);
            g_ascii_formatd(op_buf, sizeof(op_buf), "%.2f", (double)pct / 100.0);
            g_autofree gchar *pat = g_strdup_printf("^%s$", ns_ids[i] ? ns_ids[i] : "");

            g_string_append(rules_doc, "\n[[" COMP_SEC_LAYER_RULE "]]\n" COMP_KEY_NAMESPACE " = [");
            kajo_toml_append_quoted_string(rules_doc, pat);
            g_string_append_printf(rules_doc, "]\n" COMP_KEY_OPACITY " = %s\n", op_buf);
        }
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

    gboolean ok = g_file_set_contents(outputs_path, out_s->str, (gssize)out_s->len, error);
    g_string_free(out_s, TRUE);
    return ok;
}
