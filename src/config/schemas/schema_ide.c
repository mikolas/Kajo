#include "../kajo_config_schema.h"
#include <string.h>

static gint
normalize_ide_tab_width(gint val, gint def_val)
{
    if (val == 2 || val == 4 || val == 8)
        return val;
    return def_val;
}

KajoIdeKeyRemap *
kajo_ide_key_remap_new(const char *state, const char *key, const char *action)
{
    KajoIdeKeyRemap *r = g_new0(KajoIdeKeyRemap, 1);
    r->state  = g_strdup(state ? state : "");
    r->key    = g_strdup(key ? key : "");
    r->action = g_strdup(action ? action : "");
    return r;
}

KajoIdeKeyRemap *
kajo_ide_key_remap_copy(const KajoIdeKeyRemap *src)
{
    if (!src)
        return NULL;
    return kajo_ide_key_remap_new(src->state, src->key, src->action);
}

void
kajo_ide_key_remap_free(gpointer ptr)
{
    KajoIdeKeyRemap *r = (KajoIdeKeyRemap *)ptr;
    if (!r)
        return;
    g_free(r->state);
    g_free(r->key);
    g_free(r->action);
    g_free(r);
}

gboolean
kajo_ide_key_remap_equal(const KajoIdeKeyRemap *a, const KajoIdeKeyRemap *b)
{
    if (a == b)
        return TRUE;
    if (!a || !b)
        return FALSE;
    return g_strcmp0(a->state ? a->state : "", b->state ? b->state : "") == 0 &&
           g_strcmp0(a->key ? a->key : "", b->key ? b->key : "") == 0 &&
           g_strcmp0(a->action ? a->action : "", b->action ? b->action : "") == 0;
}

static gint
compare_key_remaps(gconstpointer pa, gconstpointer pb)
{
    const KajoIdeKeyRemap *a = *(const KajoIdeKeyRemap *const *)pa;
    const KajoIdeKeyRemap *b = *(const KajoIdeKeyRemap *const *)pb;
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    gint cmp = g_strcmp0(a->state ? a->state : "", b->state ? b->state : "");
    if (cmp != 0)
        return cmp;
    cmp = g_strcmp0(a->key ? a->key : "", b->key ? b->key : "");
    if (cmp != 0)
        return cmp;
    return g_strcmp0(a->action ? a->action : "", b->action ? b->action : "");
}

static gboolean
is_toml_bare_key(const char *s)
{
    if (!s || !*s)
        return FALSE;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') ||
              (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-'))
            return FALSE;
    }
    return TRUE;
}

static const KajoFieldSpec k_ide_fields[] = {
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_COLOR_SCHEME,
        .type    = KAJO_FIELD_STRING,
        .offset  = offsetof(KajoIdeCfg, color_scheme),
        .def.s   = "kajo-metro-dark",
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_FONT_FAMILY,
        .type    = KAJO_FIELD_STRING,
        .offset  = offsetof(KajoIdeCfg, font_family),
        .def.s   = "Fira Code",
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_FONT_SIZE_PX,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoIdeCfg, font_size_px),
        .def.i   = 13,
        .min_int = 9,
        .max_int = 28,
    },
    {
        .section       = NULL,
        .key           = KAJO_CFG_KEY_IDE_TAB_WIDTH,
        .type          = KAJO_FIELD_INT,
        .offset        = offsetof(KajoIdeCfg, tab_width),
        .def.i         = 4,
        .normalize_int = normalize_ide_tab_width,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_INSERT_SPACES,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, insert_spaces),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_WORD_WRAP,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, word_wrap),
        .def.b   = FALSE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_MARGIN_GUIDE,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, show_margin_guide),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_MINIMAP,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, show_minimap),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_LINE_NUMBERS,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, show_line_numbers),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_RELATIVE_NUMBERS,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, relative_line_numbers),
        .def.b   = FALSE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_CUR_LINE,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, highlight_current_line),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_START_IN_STRIP,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, start_in_strip_mode),
        .def.b   = FALSE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_STRIP_MAGNETIC,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, strip_magnetic_follow),
        .def.b   = TRUE,
    },
};

static void
ide_init_custom_defaults(void *struct_ptr)
{
    KajoIdeCfg *cfg = (KajoIdeCfg *)struct_ptr;
    cfg->key_remaps = g_ptr_array_new_with_free_func(kajo_ide_key_remap_free);
}

static void
ide_parse_custom_toml(void *struct_ptr, toml_datum_t root_table, gboolean is_overlay)
{
    KajoIdeCfg *cfg = (KajoIdeCfg *)struct_ptr;
    if (!cfg->key_remaps)
        cfg->key_remaps = g_ptr_array_new_with_free_func(kajo_ide_key_remap_free);

    toml_datum_t keys_tab = toml_get(root_table, KAJO_CFG_SEC_IDE_KEYS);
    if (keys_tab.type != TOML_TABLE)
        return;

    if (!is_overlay)
        g_ptr_array_set_size(cfg->key_remaps, 0);

    for (int i = 0; i < keys_tab.u.tab.size; i++) {
        toml_datum_t state_tab = keys_tab.u.tab.value[i];
        if (state_tab.type != TOML_TABLE)
            continue;

        const char *raw_state = keys_tab.u.tab.key[i];
        int raw_state_len = keys_tab.u.tab.len[i];
        if (!raw_state || raw_state_len <= 0)
            continue;
        g_autofree gchar *state = g_strndup(raw_state, (gsize)raw_state_len);
        if (!state[0])
            continue;

        for (int j = 0; j < state_tab.u.tab.size; j++) {
            toml_datum_t act_d = state_tab.u.tab.value[j];
            if (act_d.type != TOML_STRING)
                continue;

            const char *raw_key = state_tab.u.tab.key[j];
            int raw_key_len = state_tab.u.tab.len[j];
            if (!raw_key || raw_key_len <= 0)
                continue;

            g_autofree gchar *key = g_strndup(raw_key, (gsize)raw_key_len);
            if (!key[0])
                continue;

            g_autofree gchar *action = g_strndup(act_d.u.str.ptr ? act_d.u.str.ptr : "",
                                                 (gsize)MAX(0, act_d.u.str.len));

            gint match_idx = -1;
            for (guint k = 0; k < cfg->key_remaps->len; k++) {
                const KajoIdeKeyRemap *existing = g_ptr_array_index(cfg->key_remaps, k);
                if (existing &&
                    g_strcmp0(existing->state, state) == 0 &&
                    g_strcmp0(existing->key, key) == 0) {
                    match_idx = (gint)k;
                    break;
                }
            }

            if (!action[0]) {
                if (match_idx >= 0)
                    g_ptr_array_remove_index(cfg->key_remaps, (guint)match_idx);
            } else if (match_idx >= 0) {
                KajoIdeKeyRemap *existing = g_ptr_array_index(cfg->key_remaps, (guint)match_idx);
                g_free(existing->action);
                existing->action = g_strdup(action);
            } else {
                g_ptr_array_add(cfg->key_remaps, kajo_ide_key_remap_new(state, key, action));
            }
        }
    }

    g_ptr_array_sort(cfg->key_remaps, compare_key_remaps);
}

static void
ide_write_custom_toml(const void *struct_ptr, GString *out)
{
    const KajoIdeCfg *cfg = (const KajoIdeCfg *)struct_ptr;
    if (!cfg || !cfg->key_remaps || cfg->key_remaps->len == 0)
        return;

    GPtrArray *sorted = g_ptr_array_new();
    for (guint i = 0; i < cfg->key_remaps->len; i++) {
        const KajoIdeKeyRemap *r = g_ptr_array_index(cfg->key_remaps, i);
        if (r && r->state && r->state[0] && r->key && r->key[0] && r->action && r->action[0])
            g_ptr_array_add(sorted, (gpointer)r);
    }

    if (sorted->len == 0) {
        g_ptr_array_free(sorted, TRUE);
        return;
    }

    g_ptr_array_sort(sorted, compare_key_remaps);

    const char *cur_state = NULL;
    for (guint i = 0; i < sorted->len; i++) {
        const KajoIdeKeyRemap *r = g_ptr_array_index(sorted, i);
        if (g_strcmp0(cur_state, r->state) != 0) {
            cur_state = r->state;
            g_string_append(out, "\n[" KAJO_CFG_SEC_IDE_KEYS ".");
            if (is_toml_bare_key(cur_state))
                g_string_append(out, cur_state);
            else
                kajo_toml_append_quoted_string(out, cur_state);
            g_string_append(out, "]\n");
        }
        kajo_toml_append_quoted_string(out, r->key);
        g_string_append(out, " = ");
        kajo_toml_append_quoted_string(out, r->action);
        g_string_append_c(out, '\n');
    }

    g_ptr_array_free(sorted, TRUE);
}

static void
ide_copy_custom_fields(void *dst_ptr, const void *src_ptr)
{
    KajoIdeCfg *dst = (KajoIdeCfg *)dst_ptr;
    const KajoIdeCfg *src = (const KajoIdeCfg *)src_ptr;
    dst->key_remaps = g_ptr_array_new_with_free_func(kajo_ide_key_remap_free);
    if (!src || !src->key_remaps)
        return;
    for (guint i = 0; i < src->key_remaps->len; i++) {
        const KajoIdeKeyRemap *r = g_ptr_array_index(src->key_remaps, i);
        if (r)
            g_ptr_array_add(dst->key_remaps, kajo_ide_key_remap_copy(r));
    }
    g_ptr_array_sort(dst->key_remaps, compare_key_remaps);
}

static gboolean
ide_equal_custom_fields(const void *a_ptr, const void *b_ptr)
{
    const KajoIdeCfg *a = (const KajoIdeCfg *)a_ptr;
    const KajoIdeCfg *b = (const KajoIdeCfg *)b_ptr;
    if (a == b)
        return TRUE;
    if (!a || !b)
        return FALSE;

    const GPtrArray *aa = a->key_remaps;
    const GPtrArray *ba = b->key_remaps;
    guint la = aa ? aa->len : 0;
    guint lb = ba ? ba->len : 0;
    if (la != lb)
        return FALSE;
    if (la == 0)
        return TRUE;

    GPtrArray *sa = g_ptr_array_sized_new(la);
    GPtrArray *sb = g_ptr_array_sized_new(lb);
    for (guint i = 0; i < la; i++) {
        g_ptr_array_add(sa, g_ptr_array_index(aa, i));
        g_ptr_array_add(sb, g_ptr_array_index(ba, i));
    }
    g_ptr_array_sort(sa, compare_key_remaps);
    g_ptr_array_sort(sb, compare_key_remaps);

    gboolean eq = TRUE;
    for (guint i = 0; i < la; i++) {
        const KajoIdeKeyRemap *ra = g_ptr_array_index(sa, i);
        const KajoIdeKeyRemap *rb = g_ptr_array_index(sb, i);
        if (!kajo_ide_key_remap_equal(ra, rb)) {
            eq = FALSE;
            break;
        }
    }

    g_ptr_array_free(sa, TRUE);
    g_ptr_array_free(sb, TRUE);
    return eq;
}

static void
ide_free_custom_fields(void *struct_ptr)
{
    KajoIdeCfg *cfg = (KajoIdeCfg *)struct_ptr;
    if (!cfg)
        return;
    if (cfg->key_remaps) {
        g_ptr_array_free(cfg->key_remaps, TRUE);
        cfg->key_remaps = NULL;
    }
}

static const KajoDomainSchema k_ide_schema = {
    .domain               = KAJO_CONFIG_DOMAIN_IDE,
    .filename             = KAJO_CFG_FILE_IDE,
    .domain_table_name    = KAJO_CFG_DOMAIN_IDE,
    .struct_size          = sizeof(KajoIdeCfg),
    .fields               = k_ide_fields,
    .n_fields             = G_N_ELEMENTS(k_ide_fields),
    .init_custom_defaults = ide_init_custom_defaults,
    .parse_custom_toml    = ide_parse_custom_toml,
    .write_custom_toml    = ide_write_custom_toml,
    .copy_custom_fields   = ide_copy_custom_fields,
    .equal_custom_fields  = ide_equal_custom_fields,
    .free_custom_fields   = ide_free_custom_fields,
};

const KajoDomainSchema *
kajo_schema_get_ide(void)
{
    return &k_ide_schema;
}
