#include "kajo_config_schema.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

/* ─── Hex Color Validator ─── */

gboolean
kajo_config_is_valid_hex_color(const char *str)
{
    if (!str || strlen(str) != 7 || str[0] != '#')
        return FALSE;
    for (int i = 1; i < 7; i++) {
        if (!g_ascii_isxdigit(str[i]))
            return FALSE;
    }
    return TRUE;
}

/* ─── TOML String Escaping Helpers ─── */

void
kajo_toml_append_quoted_string(GString *out, const char *str)
{
    g_string_append_c(out, '"');
    if (str) {
        for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
            switch (*p) {
            case '\\': g_string_append(out, "\\\\"); break;
            case '"':  g_string_append(out, "\\\""); break;
            case '\b': g_string_append(out, "\\b");  break;
            case '\t': g_string_append(out, "\\t");  break;
            case '\n': g_string_append(out, "\\n");  break;
            case '\f': g_string_append(out, "\\f");  break;
            case '\r': g_string_append(out, "\\r");  break;
            default:
                if (*p < 0x20 || *p == 0x7f) {
                    g_string_append_printf(out, "\\u%04x", (unsigned int)*p);
                } else {
                    g_string_append_c(out, (gchar)*p);
                }
                break;
            }
        }
    }
    g_string_append_c(out, '"');
}

void
kajo_toml_append_literal_or_quoted_string(GString *out, const char *str)
{
    if (!str) {
        g_string_append(out, "\"\"");
        return;
    }
    /* Use single-quoted TOML literal string if str contains quotes/backslashes
     * and no single quotes or control characters. */
    gboolean has_single_quote = FALSE;
    gboolean has_ctrl         = FALSE;
    gboolean has_special      = FALSE;

    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        if (*p == '\'') has_single_quote = TRUE;
        if (*p < 0x20 || *p == 0x7f) has_ctrl = TRUE;
        if (*p == '"' || *p == '\\') has_special = TRUE;
    }

    if (has_special && !has_single_quote && !has_ctrl) {
        g_string_append_c(out, '\'');
        g_string_append(out, str);
        g_string_append_c(out, '\'');
    } else {
        kajo_toml_append_quoted_string(out, str);
    }
}

/* ─── Enum String Helpers ─── */

static const char *
enum_val_to_str(const KajoEnumMap *map, int val, const char *fallback)
{
    if (!map)
        return fallback;
    for (size_t i = 0; map[i].str != NULL; i++) {
        if (map[i].val == val)
            return map[i].str;
    }
    return fallback;
}

static gboolean
enum_str_to_val(const KajoEnumMap *map, const char *str, int *out_val)
{
    if (!map || !str)
        return FALSE;
    for (size_t i = 0; map[i].str != NULL; i++) {
        if (g_ascii_strcasecmp(map[i].str, str) == 0) {
            if (out_val)
                *out_val = map[i].val;
            return TRUE;
        }
    }
    return FALSE;
}

const gchar *
shell_panel_position_to_string(ShellPanelPosition pos)
{
    return (pos == PANEL_POSITION_BOTTOM) ? "bottom" : "top";
}

ShellPanelPosition
shell_panel_position_from_string(const gchar *str)
{
    if (str && g_ascii_strcasecmp(str, "bottom") == 0)
        return PANEL_POSITION_BOTTOM;
    return PANEL_POSITION_TOP;
}

const gchar *
shell_panel_layer_to_string(ShellPanelLayer layer)
{
    switch (layer) {
    case PANEL_LAYER_OVERLAY:    return "overlay";
    case PANEL_LAYER_BOTTOM:     return "bottom";
    case PANEL_LAYER_BACKGROUND: return "background";
    case PANEL_LAYER_TOP:
    default:                     return "top";
    }
}

ShellPanelLayer
shell_panel_layer_from_string(const gchar *str)
{
    if (!str)
        return PANEL_LAYER_TOP;
    if (g_ascii_strcasecmp(str, "overlay") == 0)    return PANEL_LAYER_OVERLAY;
    if (g_ascii_strcasecmp(str, "bottom") == 0)     return PANEL_LAYER_BOTTOM;
    if (g_ascii_strcasecmp(str, "background") == 0) return PANEL_LAYER_BACKGROUND;
    return PANEL_LAYER_TOP;
}

const gchar *
shell_widget_mode_to_string(ShellWidgetMode mode)
{
    switch (mode) {
    case WIDGET_MODE_LABEL: return "label";
    case WIDGET_MODE_FULL:  return "full";
    case WIDGET_MODE_ICON:
    default:                return "icon";
    }
}

ShellWidgetMode
shell_widget_mode_from_string(const gchar *str)
{
    if (!str)
        return WIDGET_MODE_ICON;
    if (g_ascii_strcasecmp(str, "label") == 0) return WIDGET_MODE_LABEL;
    if (g_ascii_strcasecmp(str, "full") == 0)  return WIDGET_MODE_FULL;
    return WIDGET_MODE_ICON;
}

const gchar *
shell_toast_click_action_to_string(ShellToastClickAction act)
{
    return (act == TOAST_CLICK_DISMISS) ? "dismiss" : "focus";
}

ShellToastClickAction
shell_toast_click_action_from_string(const gchar *str)
{
    if (str && g_ascii_strcasecmp(str, "dismiss") == 0)
        return TOAST_CLICK_DISMISS;
    return TOAST_CLICK_FOCUS;
}

const gchar *
shell_screensaver_mode_to_string(ShellScreensaverMode mode)
{
    switch (mode) {
    case SCREENSAVER_MODE_DPMS:     return "dpms";
    case SCREENSAVER_MODE_DISABLED: return "disabled";
    case SCREENSAVER_MODE_OVERLAY:
    default:                        return "overlay";
    }
}

ShellScreensaverMode
shell_screensaver_mode_from_string(const gchar *str)
{
    if (!str)
        return SCREENSAVER_MODE_OVERLAY;
    if (g_ascii_strcasecmp(str, "dpms") == 0)     return SCREENSAVER_MODE_DPMS;
    if (g_ascii_strcasecmp(str, "disabled") == 0) return SCREENSAVER_MODE_DISABLED;
    return SCREENSAVER_MODE_OVERLAY;
}

const gchar *
kajo_mod_filter_mode_to_string(KajoModFilterMode mode)
{
    switch (mode) {
    case KAJO_MOD_FILTER_A1200:  return "a1200";
    case KAJO_MOD_FILTER_NEAR:   return "near";
    case KAJO_MOD_FILTER_LINEAR: return "linear";
    case KAJO_MOD_FILTER_SINC:   return "sinc";
    case KAJO_MOD_FILTER_A500:
    default:                     return "a500";
    }
}

KajoModFilterMode
kajo_mod_filter_mode_from_string(const gchar *str)
{
    if (!str)
        return KAJO_MOD_FILTER_A500;
    if (g_ascii_strcasecmp(str, "a1200") == 0)  return KAJO_MOD_FILTER_A1200;
    if (g_ascii_strcasecmp(str, "near") == 0)   return KAJO_MOD_FILTER_NEAR;
    if (g_ascii_strcasecmp(str, "linear") == 0 ||
        g_ascii_strcasecmp(str, "lin") == 0)    return KAJO_MOD_FILTER_LINEAR;
    if (g_ascii_strcasecmp(str, "sinc") == 0)   return KAJO_MOD_FILTER_SINC;
    return KAJO_MOD_FILTER_A500;
}

const gchar *
kajo_mod_led_mode_to_string(KajoModLedMode mode)
{
    switch (mode) {
    case KAJO_MOD_LED_ON:  return "on";
    case KAJO_MOD_LED_OFF: return "off";
    case KAJO_MOD_LED_AUTO:
    default:               return "auto";
    }
}

KajoModLedMode
kajo_mod_led_mode_from_string(const gchar *str)
{
    if (!str)
        return KAJO_MOD_LED_AUTO;
    if (g_ascii_strcasecmp(str, "on") == 0)  return KAJO_MOD_LED_ON;
    if (g_ascii_strcasecmp(str, "off") == 0) return KAJO_MOD_LED_OFF;
    return KAJO_MOD_LED_AUTO;
}

const gchar *
kajo_mod_sid_model_to_string(KajoModSidModel model)
{
    switch (model) {
    case KAJO_MOD_SID_6581: return "6581";
    case KAJO_MOD_SID_8580: return "8580";
    case KAJO_MOD_SID_AUTO:
    default:                return "auto";
    }
}

KajoModSidModel
kajo_mod_sid_model_from_string(const gchar *str)
{
    if (!str)
        return KAJO_MOD_SID_AUTO;
    if (g_ascii_strcasecmp(str, "6581") == 0) return KAJO_MOD_SID_6581;
    if (g_ascii_strcasecmp(str, "8580") == 0) return KAJO_MOD_SID_8580;
    return KAJO_MOD_SID_AUTO;
}

const gchar *
kajo_mod_end_action_to_string(KajoModEndAction act)
{
    switch (act) {
    case KAJO_MOD_END_LOOP:     return "loop";
    case KAJO_MOD_END_NEXT_MOD: return "next-mod";
    case KAJO_MOD_END_NEXT_SUB:
    default:                    return "next-sub";
    }
}

KajoModEndAction
kajo_mod_end_action_from_string(const gchar *str)
{
    if (!str)
        return KAJO_MOD_END_NEXT_SUB;
    if (g_ascii_strcasecmp(str, "loop") == 0)     return KAJO_MOD_END_LOOP;
    if (g_ascii_strcasecmp(str, "next-mod") == 0) return KAJO_MOD_END_NEXT_MOD;
    return KAJO_MOD_END_NEXT_SUB;
}

/* ─── ShellWidgetConfig Helpers ─── */

ShellWidgetConfig *
shell_widget_config_new(const gchar *id, ShellWidgetMode mode)
{
    ShellWidgetConfig *wc = g_new0(ShellWidgetConfig, 1);
    wc->id   = g_strdup(id ? id : "");
    wc->mode = mode;
    return wc;
}

ShellWidgetConfig *
shell_widget_config_copy(const ShellWidgetConfig *src)
{
    if (!src)
        return NULL;
    ShellWidgetConfig *wc = g_new0(ShellWidgetConfig, 1);
    wc->id              = g_strdup(src->id ? src->id : "");
    wc->mode            = src->mode;
    wc->interval_sec    = src->interval_sec;
    wc->exec_cmd        = src->exec_cmd ? g_strdup(src->exec_cmd) : NULL;
    wc->click_left_cmd  = src->click_left_cmd ? g_strdup(src->click_left_cmd) : NULL;
    wc->click_right_cmd = src->click_right_cmd ? g_strdup(src->click_right_cmd) : NULL;
    return wc;
}

void
shell_widget_config_free(ShellWidgetConfig *wc)
{
    if (!wc)
        return;
    g_free(wc->id);
    g_free(wc->exec_cmd);
    g_free(wc->click_left_cmd);
    g_free(wc->click_right_cmd);
    g_free(wc);
}

gboolean
shell_widget_config_equal(const ShellWidgetConfig *a, const ShellWidgetConfig *b)
{
    if (a == b)
        return TRUE;
    if (!a || !b)
        return FALSE;
    return g_strcmp0(a->id, b->id) == 0 &&
           a->mode == b->mode &&
           a->interval_sec == b->interval_sec &&
           g_strcmp0(a->exec_cmd ? a->exec_cmd : "", b->exec_cmd ? b->exec_cmd : "") == 0 &&
           g_strcmp0(a->click_left_cmd ? a->click_left_cmd : "", b->click_left_cmd ? b->click_left_cmd : "") == 0 &&
           g_strcmp0(a->click_right_cmd ? a->click_right_cmd : "", b->click_right_cmd ? b->click_right_cmd : "") == 0;
}

/* ─── Schema Dispatch Table ─── */

const KajoDomainSchema *
kajo_config_get_domain_schema(KajoConfigDomain domain)
{
    switch (domain) {
    case KAJO_CONFIG_DOMAIN_THEME: return kajo_schema_get_theme();
    case KAJO_CONFIG_DOMAIN_SHELL: return kajo_schema_get_shell();
    case KAJO_CONFIG_DOMAIN_IDE:   return kajo_schema_get_ide();
    case KAJO_CONFIG_DOMAIN_FILES: return kajo_schema_get_files();
    case KAJO_CONFIG_DOMAIN_MOD:   return kajo_schema_get_mod();
    default:                       return NULL;
    }
}

/* ─── Generic Schema Engine Implementation ─── */

void
kajo_cfg_domain_clear(const KajoDomainSchema *schema, void *ptr)
{
    if (!schema || !ptr)
        return;

    char *base = (char *)ptr;
    for (size_t i = 0; i < schema->n_fields; i++) {
        const KajoFieldSpec *f = &schema->fields[i];
        if (f->type == KAJO_FIELD_STRING || f->type == KAJO_FIELD_HEX_COLOR) {
            gchar **sp = (gchar **)(base + f->offset);
            g_clear_pointer(sp, g_free);
        }
    }
    if (schema->free_custom_fields) {
        schema->free_custom_fields(ptr);
    }
}

void
kajo_cfg_domain_init_defaults(const KajoDomainSchema *schema, void *dst)
{
    if (!schema || !dst)
        return;

    kajo_cfg_domain_clear(schema, dst);
    memset(dst, 0, schema->struct_size);

    char *base = (char *)dst;
    for (size_t i = 0; i < schema->n_fields; i++) {
        const KajoFieldSpec *f = &schema->fields[i];
        void *field_ptr = base + f->offset;
        switch (f->type) {
        case KAJO_FIELD_BOOL:
            *(gboolean *)field_ptr = f->def.b;
            break;
        case KAJO_FIELD_INT:
            *(gint *)field_ptr = f->def.i;
            break;
        case KAJO_FIELD_DOUBLE:
            *(gdouble *)field_ptr = f->def.d;
            break;
        case KAJO_FIELD_STRING:
        case KAJO_FIELD_HEX_COLOR:
            *(gchar **)field_ptr = g_strdup(f->def.s ? f->def.s : "");
            break;
        case KAJO_FIELD_ENUM:
            *(gint *)field_ptr = f->def.e;
            break;
        }
    }

    if (schema->init_custom_defaults) {
        schema->init_custom_defaults(dst);
    }
}

void
kajo_cfg_domain_copy_into(const KajoDomainSchema *schema, void *dst, const void *src)
{
    if (!schema || !dst || !src || dst == src)
        return;

    kajo_cfg_domain_clear(schema, dst);
    memset(dst, 0, schema->struct_size);

    char *dbase = (char *)dst;
    const char *sbase = (const char *)src;

    for (size_t i = 0; i < schema->n_fields; i++) {
        const KajoFieldSpec *f = &schema->fields[i];
        void *df = dbase + f->offset;
        const void *sf = sbase + f->offset;
        switch (f->type) {
        case KAJO_FIELD_BOOL:
            *(gboolean *)df = *(const gboolean *)sf;
            break;
        case KAJO_FIELD_INT:
        case KAJO_FIELD_ENUM:
            *(gint *)df = *(const gint *)sf;
            break;
        case KAJO_FIELD_DOUBLE:
            *(gdouble *)df = *(const gdouble *)sf;
            break;
        case KAJO_FIELD_STRING:
        case KAJO_FIELD_HEX_COLOR: {
            const gchar *s = *(const gchar *const *)sf;
            *(gchar **)df = g_strdup(s ? s : (f->def.s ? f->def.s : ""));
            break;
        }
        }
    }

    if (schema->copy_custom_fields) {
        schema->copy_custom_fields(dst, src);
    }
}

void *
kajo_cfg_domain_new_rc_default(const KajoDomainSchema *schema)
{
    if (!schema)
        return NULL;
    void *ptr = g_atomic_rc_box_alloc0(schema->struct_size);
    kajo_cfg_domain_init_defaults(schema, ptr);
    return ptr;
}

void *
kajo_cfg_domain_copy_rc(const KajoDomainSchema *schema, const void *src)
{
    if (!schema)
        return NULL;
    if (!src)
        return kajo_cfg_domain_new_rc_default(schema);
    void *ptr = g_atomic_rc_box_alloc0(schema->struct_size);
    kajo_cfg_domain_copy_into(schema, ptr, src);
    return ptr;
}

void
kajo_cfg_domain_unref_rc(const KajoDomainSchema *schema, const void *rc_ptr)
{
    if (!schema || !rc_ptr)
        return;
    /* Clean up owned heap members inside the final release callback */
    if (schema == kajo_schema_get_theme()) {
        kajo_theme_cfg_unref((const KajoThemeCfg *)rc_ptr);
    } else if (schema == kajo_schema_get_shell()) {
        kajo_shell_cfg_unref((const KajoShellCfg *)rc_ptr);
    } else if (schema == kajo_schema_get_ide()) {
        kajo_ide_cfg_unref((const KajoIdeCfg *)rc_ptr);
    } else if (schema == kajo_schema_get_files()) {
        kajo_files_cfg_unref((const KajoFilesCfg *)rc_ptr);
    } else if (schema == kajo_schema_get_mod()) {
        kajo_mod_cfg_unref((const KajoModCfg *)rc_ptr);
    }
}

void
kajo_cfg_domain_apply_toml_table(const KajoDomainSchema *schema,
                                 void                   *dst,
                                 toml_datum_t            tab,
                                 gboolean                is_overlay,
                                 const char             *source_label)
{
    if (!schema || !dst || tab.type != TOML_TABLE)
        return;

    /* If applying a conf.d/ drop-in overlay (or a table with a top-level domain
     * section like [theme], [shell], [ide], [files], [mod]), unwrap that sub-table. */
    toml_datum_t domain_tab = tab;
    if (schema->domain_table_name) {
        toml_datum_t sub = toml_get(tab, schema->domain_table_name);
        if (sub.type == TOML_TABLE) {
            domain_tab = sub;
        }
    }

    char *base = (char *)dst;
    const char *label = source_label ? source_label : schema->filename;

    for (size_t i = 0; i < schema->n_fields; i++) {
        const KajoFieldSpec *f = &schema->fields[i];
        toml_datum_t d;

        if (f->section && *f->section) {
            toml_datum_t sec_tab = toml_get(domain_tab, f->section);
            if (sec_tab.type != TOML_TABLE)
                continue;
            d = toml_get(sec_tab, f->key);
        } else {
            d = toml_get(domain_tab, f->key);
        }

        if (d.type == TOML_UNKNOWN)
            continue;

        void *field_ptr = base + f->offset;

        switch (f->type) {
        case KAJO_FIELD_BOOL:
            if (d.type == TOML_BOOLEAN) {
                *(gboolean *)field_ptr = d.u.boolean ? TRUE : FALSE;
            } else {
                g_warning("Config (%s): expected boolean for '%s%s%s', keeping previous value",
                          label,
                          f->section ? f->section : "",
                          f->section ? "." : "",
                          f->key);
            }
            break;

        case KAJO_FIELD_INT:
            if (d.type == TOML_INT64) {
                gint val = (gint)CLAMP(d.u.int64, (int64_t)G_MININT, (int64_t)G_MAXINT);
                if (f->normalize_int) {
                    val = f->normalize_int(val, f->def.i);
                } else if (f->min_int < f->max_int) {
                    val = CLAMP(val, f->min_int, f->max_int);
                }
                *(gint *)field_ptr = val;
            } else {
                g_warning("Config (%s): expected integer for '%s%s%s', keeping previous value",
                          label,
                          f->section ? f->section : "",
                          f->section ? "." : "",
                          f->key);
            }
            break;

        case KAJO_FIELD_DOUBLE:
            if (d.type == TOML_FP64 || d.type == TOML_INT64) {
                gdouble val = (d.type == TOML_FP64) ? d.u.fp64 : (gdouble)d.u.int64;
                if (isnan(val) || isinf(val)) {
                    val = f->def.d;
                } else if (f->min_dbl < f->max_dbl) {
                    val = CLAMP(val, f->min_dbl, f->max_dbl);
                }
                *(gdouble *)field_ptr = val;
            } else {
                g_warning("Config (%s): expected float for '%s%s%s', keeping previous value",
                          label,
                          f->section ? f->section : "",
                          f->section ? "." : "",
                          f->key);
            }
            break;

        case KAJO_FIELD_STRING:
            if (d.type == TOML_STRING) {
                gchar *dup = g_strndup(d.u.str.ptr ? d.u.str.ptr : "", (gsize)MAX(0, d.u.str.len));
                g_free(*(gchar **)field_ptr);
                *(gchar **)field_ptr = dup;
            } else {
                g_warning("Config (%s): expected string for '%s%s%s', keeping previous value",
                          label,
                          f->section ? f->section : "",
                          f->section ? "." : "",
                          f->key);
            }
            break;

        case KAJO_FIELD_HEX_COLOR:
            if (d.type == TOML_STRING) {
                g_autofree gchar *cand = g_strndup(d.u.str.ptr ? d.u.str.ptr : "", (gsize)MAX(0, d.u.str.len));
                if (kajo_config_is_valid_hex_color(cand)) {
                    gchar *lower = g_ascii_strdown(cand, -1);
                    g_free(*(gchar **)field_ptr);
                    *(gchar **)field_ptr = lower;
                } else {
                    g_warning("Config (%s): invalid hex color '%s' for '%s', keeping previous value",
                              label, cand, f->key);
                }
            } else {
                g_warning("Config (%s): expected hex color string for '%s', keeping previous value",
                          label, f->key);
            }
            break;

        case KAJO_FIELD_ENUM:
            if (d.type == TOML_STRING) {
                g_autofree gchar *cand = g_strndup(d.u.str.ptr ? d.u.str.ptr : "", (gsize)MAX(0, d.u.str.len));
                int eval = f->def.e;
                if (enum_str_to_val(f->enum_map, cand, &eval)) {
                    *(gint *)field_ptr = eval;
                } else {
                    g_warning("Config (%s): unknown enum value '%s' for '%s%s%s', keeping previous value",
                              label, cand,
                              f->section ? f->section : "",
                              f->section ? "." : "",
                              f->key);
                }
            } else {
                g_warning("Config (%s): expected string enum for '%s%s%s', keeping previous value",
                          label,
                          f->section ? f->section : "",
                          f->section ? "." : "",
                          f->key);
            }
            break;
        }
    }

    if (schema->parse_custom_toml) {
        schema->parse_custom_toml(dst, domain_tab, is_overlay);
    }
}

gboolean
kajo_cfg_domain_parse_toml_str(const KajoDomainSchema *schema,
                               void                   *dst,
                               const char             *toml_str,
                               gssize                  len,
                               gboolean                is_overlay,
                               const char             *source_label)
{
    if (!schema || !dst)
        return FALSE;

    if (!is_overlay) {
        kajo_cfg_domain_init_defaults(schema, dst);
    }

    if (!toml_str)
        return FALSE;

    int actual_len = (len < 0) ? (int)strlen(toml_str) : (int)len;
    toml_result_t res = toml_parse(toml_str, actual_len);
    if (!res.ok) {
        g_warning("TOML syntax error in %s: %s",
                  source_label ? source_label : schema->filename,
                  res.errmsg);
        toml_free(res);
        return FALSE;
    }

    kajo_cfg_domain_apply_toml_table(schema, dst, res.toptab, is_overlay, source_label);
    toml_free(res);
    return TRUE;
}

gboolean
kajo_cfg_domain_parse_toml_file(const KajoDomainSchema *schema,
                                void                   *dst,
                                const char             *filepath,
                                gboolean                is_overlay)
{
    if (!schema || !dst)
        return FALSE;

    if (!is_overlay) {
        kajo_cfg_domain_init_defaults(schema, dst);
    }

    if (!filepath || !g_file_test(filepath, G_FILE_TEST_EXISTS))
        return FALSE;

    g_autofree gchar *contents = NULL;
    gsize length = 0;
    g_autoptr(GError) err = NULL;
    if (!g_file_get_contents(filepath, &contents, &length, &err)) {
        g_warning("Failed to read '%s': %s", filepath, err ? err->message : "unknown error");
        return FALSE;
    }

    return kajo_cfg_domain_parse_toml_str(schema, dst, contents, (gssize)length, is_overlay, filepath);
}

static void
format_field_rhs(const KajoFieldSpec *f, const void *field_ptr, GString *out)
{
    switch (f->type) {
    case KAJO_FIELD_BOOL:
        g_string_append(out, (*(const gboolean *)field_ptr) ? "true" : "false");
        break;

    case KAJO_FIELD_INT: {
        gint val = *(const gint *)field_ptr;
        if (f->normalize_int) {
            val = f->normalize_int(val, f->def.i);
        } else if (f->min_int < f->max_int) {
            val = CLAMP(val, f->min_int, f->max_int);
        }
        g_string_append_printf(out, "%d", val);
        break;
    }

    case KAJO_FIELD_DOUBLE: {
        gdouble val = *(const gdouble *)field_ptr;
        if (f->min_dbl < f->max_dbl)
            val = CLAMP(val, f->min_dbl, f->max_dbl);
        char dbuf[64];
        g_ascii_formatd(dbuf, sizeof(dbuf), "%.15g", val);
        g_string_append(out, dbuf);
        break;
    }

    case KAJO_FIELD_STRING: {
        const gchar *s = *(const gchar *const *)field_ptr;
        kajo_toml_append_quoted_string(out, s ? s : (f->def.s ? f->def.s : ""));
        break;
    }

    case KAJO_FIELD_HEX_COLOR: {
        const gchar *s = *(const gchar *const *)field_ptr;
        const gchar *valid = (s && kajo_config_is_valid_hex_color(s))
            ? s : (f->def.s ? f->def.s : "#0078d4");
        g_autofree gchar *lower = g_ascii_strdown(valid, -1);
        kajo_toml_append_quoted_string(out, lower);
        break;
    }

    case KAJO_FIELD_ENUM: {
        gint eval = *(const gint *)field_ptr;
        const char *estr = enum_val_to_str(f->enum_map, eval,
                                           enum_val_to_str(f->enum_map, f->def.e, ""));
        kajo_toml_append_quoted_string(out, estr);
        break;
    }
    }
}

static gboolean
field_values_equal(const KajoFieldSpec *f, const void *fa, const void *fb)
{
    switch (f->type) {
    case KAJO_FIELD_BOOL:
        return (*(const gboolean *)fa ? TRUE : FALSE) == (*(const gboolean *)fb ? TRUE : FALSE);
    case KAJO_FIELD_INT:
    case KAJO_FIELD_ENUM:
        return *(const gint *)fa == *(const gint *)fb;
    case KAJO_FIELD_DOUBLE:
        return fabs(*(const gdouble *)fa - *(const gdouble *)fb) <= 1e-9;
    case KAJO_FIELD_STRING: {
        const gchar *sa = *(const gchar *const *)fa;
        const gchar *sb = *(const gchar *const *)fb;
        return g_strcmp0(sa ? sa : "", sb ? sb : "") == 0;
    }
    case KAJO_FIELD_HEX_COLOR: {
        const gchar *sa = *(const gchar *const *)fa;
        const gchar *sb = *(const gchar *const *)fb;
        return g_ascii_strcasecmp(sa ? sa : "", sb ? sb : "") == 0;
    }
    }
    return TRUE;
}

gchar *
kajo_cfg_domain_serialize_toml(const KajoDomainSchema *schema,
                               const void             *src)
{
    if (!schema || !src)
        return NULL;

    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "# %s — Kajo Desktop Suite Configuration\n", schema->filename);

    const char *base = (const char *)src;
    const char *cur_sec = NULL;
    gboolean wrote_top_keys = FALSE;

    for (size_t i = 0; i < schema->n_fields; i++) {
        const KajoFieldSpec *f = &schema->fields[i];

        if (g_strcmp0(cur_sec, f->section) != 0) {
            if (wrote_top_keys || cur_sec != NULL) {
                g_string_append_c(out, '\n');
            }
            cur_sec = f->section;
            if (cur_sec && *cur_sec) {
                g_string_append_printf(out, "[%s]\n", cur_sec);
            }
        } else if (!cur_sec && !wrote_top_keys) {
            g_string_append_c(out, '\n');
        }

        if (!cur_sec)
            wrote_top_keys = TRUE;

        const void *field_ptr = base + f->offset;
        g_string_append_printf(out, "%-30s = ", f->key);
        format_field_rhs(f, field_ptr, out);
        g_string_append_c(out, '\n');
    }

    if (schema->write_custom_toml) {
        schema->write_custom_toml(src, out);
    }

    return g_string_free(out, FALSE);
}

/* Scans a single line for `[section]` header; returns newly allocated section name or NULL. */
static gchar *
parse_line_section_header(const char *line, size_t line_len, gboolean *out_is_array_table)
{
    if (out_is_array_table)
        *out_is_array_table = FALSE;

    size_t i = 0;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= line_len || line[i] != '[')
        return NULL;

    if (i + 1 < line_len && line[i + 1] == '[') {
        if (out_is_array_table)
            *out_is_array_table = TRUE;
        i += 2;
        while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
            i++;
        size_t start = i;
        while (i < line_len && line[i] != ']' && line[i] != ' ' && line[i] != '\t')
            i++;
        return g_strndup(line + start, i - start);
    }

    i++;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    size_t start = i;
    while (i < line_len && line[i] != ']' && line[i] != ' ' && line[i] != '\t')
        i++;
    size_t end = i;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= line_len || line[i] != ']')
        return NULL;
    return g_strndup(line + start, end - start);
}

/* Checks if `line[0..line_len)` is a single-line `key = value [# comment]` for `target_key`.
 * If matched, returns TRUE and sets `*out_val_start` and `*out_val_end` relative to `line`. */
static gboolean
match_line_scalar_key(const char *line,
                      size_t      line_len,
                      const char *target_key,
                      size_t     *out_val_start,
                      size_t     *out_val_end)
{
    size_t i = 0;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= line_len || line[i] == '#' || line[i] == '[')
        return FALSE;

    size_t klen = strlen(target_key);
    if (i + klen > line_len || strncmp(line + i, target_key, klen) != 0)
        return FALSE;
    i += klen;

    while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= line_len || line[i] != '=')
        return FALSE;
    i++;

    while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
        i++;

    size_t val_start = i;
    if (i < line_len && line[i] == '"') {
        i++;
        while (i < line_len) {
            if (line[i] == '\\' && i + 1 < line_len) {
                i += 2;
                continue;
            }
            if (line[i] == '"') {
                i++;
                break;
            }
            i++;
        }
    } else if (i < line_len && line[i] == '\'') {
        i++;
        while (i < line_len && line[i] != '\'')
            i++;
        if (i < line_len && line[i] == '\'')
            i++;
    } else {
        while (i < line_len && line[i] != '#' && line[i] != '\r' && line[i] != '\n')
            i++;
        while (i > val_start && (line[i - 1] == ' ' || line[i - 1] == '\t'))
            i--;
    }

    *out_val_start = val_start;
    *out_val_end   = i;
    return TRUE;
}

static void
patch_or_insert_scalar_in_doc(GString    *doc,
                              const char *section,
                              const char *key,
                              const char *rhs_val)
{
    g_autofree gchar *cur_sec = NULL;
    gboolean cur_is_arr = FALSE;
    gssize first_sec_offset = -1;
    gssize target_sec_insert_offset = -1;
    gboolean seen_target_sec = (section == NULL);

    size_t pos = 0;
    while (pos < doc->len) {
        const char *nl = memchr(doc->str + pos, '\n', doc->len - pos);
        size_t line_end = nl ? (size_t)(nl - doc->str) : doc->len;
        size_t next_pos = nl ? (line_end + 1) : doc->len;
        size_t content_len = line_end - pos;
        if (content_len > 0 && doc->str[pos + content_len - 1] == '\r')
            content_len--;

        gboolean is_arr = FALSE;
        gchar *hdr = parse_line_section_header(doc->str + pos, content_len, &is_arr);
        if (hdr) {
            if (first_sec_offset < 0)
                first_sec_offset = (gssize)pos;
            g_free(cur_sec);
            cur_sec = hdr;
            cur_is_arr = is_arr;
            if (!cur_is_arr && section && g_strcmp0(cur_sec, section) == 0) {
                seen_target_sec = TRUE;
                target_sec_insert_offset = (gssize)next_pos;
            }
            pos = next_pos;
            continue;
        }

        gboolean in_target = (!cur_is_arr && g_strcmp0(cur_sec, section) == 0);
        if (in_target) {
            /* Track non-blank line end inside target section */
            size_t s = 0;
            while (s < content_len && (doc->str[pos + s] == ' ' || doc->str[pos + s] == '\t'))
                s++;
            if (s < content_len) {
                target_sec_insert_offset = (gssize)next_pos;
            }

            size_t v_start = 0, v_end = 0;
            if (match_line_scalar_key(doc->str + pos, content_len, key, &v_start, &v_end)) {
                g_string_erase(doc, (gssize)(pos + v_start), (gssize)(v_end - v_start));
                g_string_insert(doc, (gssize)(pos + v_start), rhs_val);
                return;
            }
        }

        pos = next_pos;
    }

    /* Key was not found in doc: insert it into the appropriate section */
    g_autofree gchar *new_line = g_strdup_printf("%-30s = %s\n", key, rhs_val);
    if (seen_target_sec) {
        if (section == NULL) {
            if (first_sec_offset >= 0) {
                g_string_insert(doc, first_sec_offset, new_line);
            } else {
                if (doc->len > 0 && doc->str[doc->len - 1] != '\n')
                    g_string_append_c(doc, '\n');
                g_string_append(doc, new_line);
            }
        } else if (target_sec_insert_offset >= 0) {
            g_string_insert(doc, target_sec_insert_offset, new_line);
        } else {
            g_string_append(doc, new_line);
        }
    } else {
        if (doc->len > 0 && doc->str[doc->len - 1] != '\n')
            g_string_append_c(doc, '\n');
        if (doc->len > 0)
            g_string_append_c(doc, '\n');
        g_string_append_printf(doc, "[%s]\n%s", section, new_line);
    }
}

static void
replace_or_append_custom_layout_in_doc(const KajoDomainSchema *schema,
                                       const void             *new_draft,
                                       GString                *doc)
{
    if (!schema->write_custom_toml)
        return;

    /* Find `[layout]` or `[[layout.*]]` span in `doc` and replace it */
    gssize layout_start = -1;
    gssize layout_end   = (gssize)doc->len;

    size_t pos = 0;
    while (pos < doc->len) {
        const char *nl = memchr(doc->str + pos, '\n', doc->len - pos);
        size_t line_end = nl ? (size_t)(nl - doc->str) : doc->len;
        size_t next_pos = nl ? (line_end + 1) : doc->len;
        size_t content_len = line_end - pos;
        if (content_len > 0 && doc->str[pos + content_len - 1] == '\r')
            content_len--;

        gboolean is_arr = FALSE;
        g_autofree gchar *hdr = parse_line_section_header(doc->str + pos, content_len, &is_arr);
        if (hdr) {
            gboolean is_layout = (g_strcmp0(hdr, KAJO_CFG_SEC_LAYOUT) == 0 ||
                                  g_str_has_prefix(hdr, "layout."));
            if (is_layout && layout_start < 0) {
                layout_start = (gssize)pos;
            } else if (!is_layout && layout_start >= 0) {
                layout_end = (gssize)pos;
                break;
            }
        }
        pos = next_pos;
    }

    GString *custom_block = g_string_new(NULL);
    schema->write_custom_toml(new_draft, custom_block);
    /* Trim leading newline if replacing in-place */
    const char *block_str = custom_block->str;
    if (layout_start >= 0 && block_str[0] == '\n')
        block_str++;

    if (layout_start >= 0) {
        g_string_erase(doc, layout_start, layout_end - layout_start);
        g_string_insert(doc, layout_start, block_str);
    } else {
        if (doc->len > 0 && doc->str[doc->len - 1] != '\n')
            g_string_append_c(doc, '\n');
        g_string_append(doc, custom_block->str);
    }
    g_string_free(custom_block, TRUE);
}

gchar *
kajo_cfg_domain_patch_toml(const KajoDomainSchema *schema,
                           const void             *old_base,
                           const void             *new_draft,
                           const char             *existing_toml)
{
    if (!schema || !old_base || !new_draft)
        return NULL;

    GString *doc = g_string_new(existing_toml ? existing_toml : "");
    if (!existing_toml || !*existing_toml) {
        g_string_append_printf(doc, "# %s — Kajo Desktop Suite Configuration\n", schema->filename);
    }

    const char *obase = (const char *)old_base;
    const char *nbase = (const char *)new_draft;

    for (size_t i = 0; i < schema->n_fields; i++) {
        const KajoFieldSpec *f = &schema->fields[i];
        const void *fo = obase + f->offset;
        const void *fn = nbase + f->offset;

        if (!field_values_equal(f, fo, fn)) {
            GString *rhs = g_string_new(NULL);
            format_field_rhs(f, fn, rhs);
            patch_or_insert_scalar_in_doc(doc, f->section, f->key, rhs->str);
            g_string_free(rhs, TRUE);
        }
    }

    if (schema->write_custom_toml && schema->equal_custom_fields) {
        if (!schema->equal_custom_fields(old_base, new_draft)) {
            replace_or_append_custom_layout_in_doc(schema, new_draft, doc);
        }
    }

    return g_string_free(doc, FALSE);
}

/* Parses `text` into a fresh domain struct without logging syntax errors.
 * Returns FALSE if the text is not valid TOML. */
static gboolean
parse_text_quiet(const KajoDomainSchema *schema, void *dst, const char *text)
{
    kajo_cfg_domain_init_defaults(schema, dst);
    toml_result_t res = toml_parse(text, (int)strlen(text));
    if (!res.ok) {
        toml_free(res);
        return FALSE;
    }
    kajo_cfg_domain_apply_toml_table(schema, dst, res.toptab, FALSE, "patched result");
    toml_free(res);
    return TRUE;
}

/* TRUE when `patched` parses and yields exactly the values `new_draft` would
 * yield from a freshly serialized file (so clamping is accounted for). */
static gboolean
patched_text_matches_draft(const KajoDomainSchema *schema,
                           const char             *patched,
                           const void             *new_draft)
{
    g_autofree gchar *expected_text = kajo_cfg_domain_serialize_toml(schema, new_draft);
    if (!expected_text)
        return FALSE;

    void *expected = g_malloc0(schema->struct_size);
    void *got      = g_malloc0(schema->struct_size);
    gboolean ok = parse_text_quiet(schema, expected, expected_text) &&
                  parse_text_quiet(schema, got, patched) &&
                  kajo_cfg_domain_equal(schema, expected, got);
    kajo_cfg_domain_clear(schema, expected);
    kajo_cfg_domain_clear(schema, got);
    g_free(expected);
    g_free(got);
    return ok;
}

gboolean
kajo_cfg_domain_save_patched(const KajoDomainSchema *schema,
                             const void             *old_base,
                             const void             *new_draft,
                             const char             *existing_toml,
                             const char             *filepath,
                             GError                **error)
{
    if (!schema || !old_base || !new_draft || !filepath)
        return FALSE;

    g_autofree gchar *dir = g_path_get_dirname(filepath);
    if (dir && g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create directory '%s'", dir);
        return FALSE;
    }

    g_autofree gchar *patched = kajo_cfg_domain_patch_toml(schema, old_base, new_draft, existing_toml);
    if (!patched)
        return FALSE;

    if (patched_text_matches_draft(schema, patched, new_draft))
        return g_file_set_contents(filepath, patched, -1, error);

    /* The in-place patcher only understands plain `key = value` lines inside
     * `[section]` headers. Quoted keys, dotted keys and inline tables make it
     * emit a duplicate definition. Never write such a file: keep the user's
     * original text in `<file>.bak` and write a freshly serialized file. */
    g_autofree gchar *regenerated = kajo_cfg_domain_serialize_toml(schema, new_draft);
    if (!regenerated)
        return FALSE;

    if (existing_toml && *existing_toml) {
        g_autofree gchar *bak_path = g_strconcat(filepath, ".bak", NULL);
        g_autoptr(GError) bak_err = NULL;
        if (!g_file_set_contents(bak_path, existing_toml, -1, &bak_err)) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                        "Could not back up '%s' before regenerating it (%s); not saving",
                        filepath, bak_err ? bak_err->message : "unknown error");
            return FALSE;
        }
        g_warning("Config: could not update '%s' in place (it uses key syntax the editor "
                  "does not preserve); wrote a regenerated file, previous contents are in '%s'",
                  filepath, bak_path);
    }

    return g_file_set_contents(filepath, regenerated, -1, error);
}

gboolean
kajo_cfg_domain_save_atomic(const KajoDomainSchema *schema,
                            const void             *src,
                            const char             *filepath,
                            GError                **error)
{
    if (!schema || !src || !filepath)
        return FALSE;

    g_autofree gchar *dir = g_path_get_dirname(filepath);
    if (dir && g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to create directory '%s'", dir);
        return FALSE;
    }

    g_autofree gchar *toml_text = kajo_cfg_domain_serialize_toml(schema, src);
    if (!toml_text)
        return FALSE;

    return g_file_set_contents(filepath, toml_text, -1, error);
}

gboolean
kajo_cfg_domain_equal(const KajoDomainSchema *schema,
                      const void             *a,
                      const void             *b)
{
    if (a == b)
        return TRUE;
    if (!schema || !a || !b)
        return FALSE;

    const char *ba = (const char *)a;
    const char *bb = (const char *)b;

    for (size_t i = 0; i < schema->n_fields; i++) {
        const KajoFieldSpec *f = &schema->fields[i];
        if (!field_values_equal(f, ba + f->offset, bb + f->offset))
            return FALSE;
    }

    if (schema->equal_custom_fields) {
        if (!schema->equal_custom_fields(a, b))
            return FALSE;
    }

    return TRUE;
}

gchar *
kajo_config_find_default_file(const char *filename)
{
    if (!filename || !*filename)
        return NULL;

    const gchar *env_defaults = g_getenv("KAJO_DEFAULTS_DIR");
    if (env_defaults && *env_defaults) {
        gchar *env_path = g_build_filename(env_defaults, filename, NULL);
        if (g_file_test(env_path, G_FILE_TEST_EXISTS))
            return env_path;
        g_free(env_path);
    }

    gchar *local_share = g_build_filename(g_get_user_data_dir(), "kajo", "defaults", filename, NULL);
    if (g_file_test(local_share, G_FILE_TEST_EXISTS))
        return local_share;
    g_free(local_share);

    gchar *usr_local = g_build_filename("/usr/local/share/kajo/defaults", filename, NULL);
    if (g_file_test(usr_local, G_FILE_TEST_EXISTS))
        return usr_local;
    g_free(usr_local);

    gchar *usr_share = g_build_filename("/usr/share/kajo/defaults", filename, NULL);
    if (g_file_test(usr_share, G_FILE_TEST_EXISTS))
        return usr_share;
    g_free(usr_share);

    return g_build_filename("/usr/share/kajo/defaults", filename, NULL);
}

/* ─── Refcounted & Mutable Per-Domain Lifecycle Wrappers ─── */

#define DEFINE_DOMAIN_LIFECYCLE(Type, prefix, get_schema_fn)                   \
    static void prefix##_clear_cb(gpointer ptr)                                \
    {                                                                          \
        kajo_cfg_domain_clear(get_schema_fn(), ptr);                           \
    }                                                                          \
    const Type *prefix##_ref(const Type *cfg)                                  \
    {                                                                          \
        return cfg ? (const Type *)g_atomic_rc_box_acquire((gpointer)cfg) : NULL; \
    }                                                                          \
    void prefix##_unref(const Type *cfg)                                       \
    {                                                                          \
        if (cfg)                                                               \
            g_atomic_rc_box_release_full((gpointer)cfg, prefix##_clear_cb);    \
    }                                                                          \
    Type *prefix##_new_default(void)                                           \
    {                                                                          \
        const KajoDomainSchema *s = get_schema_fn();                           \
        Type *cfg = (Type *)g_malloc0(s->struct_size);                         \
        kajo_cfg_domain_init_defaults(s, cfg);                                 \
        return cfg;                                                            \
    }                                                                          \
    Type *prefix##_copy(const Type *src)                                       \
    {                                                                          \
        if (!src)                                                              \
            return prefix##_new_default();                                     \
        const KajoDomainSchema *s = get_schema_fn();                           \
        Type *cfg = (Type *)g_malloc0(s->struct_size);                         \
        kajo_cfg_domain_copy_into(s, cfg, src);                                \
        return cfg;                                                            \
    }                                                                          \
    void prefix##_free(Type *cfg)                                              \
    {                                                                          \
        if (!cfg)                                                              \
            return;                                                            \
        kajo_cfg_domain_clear(get_schema_fn(), cfg);                           \
        g_free(cfg);                                                           \
    }

DEFINE_DOMAIN_LIFECYCLE(KajoThemeCfg, kajo_theme_cfg, kajo_schema_get_theme)
DEFINE_DOMAIN_LIFECYCLE(KajoShellCfg, kajo_shell_cfg, kajo_schema_get_shell)
DEFINE_DOMAIN_LIFECYCLE(KajoIdeCfg,   kajo_ide_cfg,   kajo_schema_get_ide)
DEFINE_DOMAIN_LIFECYCLE(KajoFilesCfg, kajo_files_cfg, kajo_schema_get_files)
DEFINE_DOMAIN_LIFECYCLE(KajoModCfg,   kajo_mod_cfg,   kajo_schema_get_mod)
