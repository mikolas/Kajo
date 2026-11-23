#include "theme_export.h"
#include "theme.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define KAJO_MANAGED_BEGIN "/* >>> KAJO-MANAGED-THEME-BEGIN (do not edit; managed by kajo-settings) >>> */"
#define KAJO_MANAGED_END   "/* <<< KAJO-MANAGED-THEME-END <<< */"
#define KAJO_BACKUP_FILE   "external-theme-backup.json"
#define KAJO_QT_ENV_FILE   "90-kajo-qt.conf"

typedef struct {
    int ar, ag, ab;
    int hr, hg, hb;
    int s0r, s0g, s0b;
    int s1r, s1g, s1b;
    int sr,  sg,  sb;
    int s2r, s2g, s2b;
    int s3r, s3g, s3b;
    int s4r, s4g, s4b;
    int shr, shg, shb;
    int bdsr, bdsg, bdsb;
    int bdr,  bdg,  bdb;
    int bdhr, bdhg, bdhb;
    int sec_v, mut_v, dim_v;
} ResolvedPalette;

static gboolean
parse_hex(const char *hex, int *r, int *g, int *b)
{
    if (!hex || hex[0] != '#' || strlen(hex) != 7)
        return FALSE;
    unsigned int rr = 0, gg = 0, bb = 0;
    if (sscanf(hex + 1, "%02x%02x%02x", &rr, &gg, &bb) != 3)
        return FALSE;
    *r = (int)rr;
    *g = (int)gg;
    *b = (int)bb;
    return TRUE;
}

static int
clamp_byte(int v)
{
    return CLAMP(v, 0, 255);
}

static void
resolve_palette(const KajoThemeCfg *cfg, ResolvedPalette *p)
{
    const char *accent = (cfg && cfg->accent_color && cfg->accent_color[0])
        ? cfg->accent_color : "#0078d4";
    const char *hover = accent;
    for (size_t i = 0; i < SHELL_N_ACCENT_PRESETS; i++) {
        if (g_ascii_strcasecmp(accent, shell_accent_presets[i].accent) == 0) {
            hover = shell_accent_presets[i].hover;
            break;
        }
    }

    p->ar = 0x00; p->ag = 0x78; p->ab = 0xd4;
    parse_hex(accent, &p->ar, &p->ag, &p->ab);
    p->hr = p->ar; p->hg = p->ag; p->hb = p->ab;
    if (!parse_hex(hover, &p->hr, &p->hg, &p->hb) || hover == accent) {
        p->hr = clamp_byte(p->ar + 16);
        p->hg = clamp_byte(p->ag + 16);
        p->hb = clamp_byte(p->ab + 16);
    }

    p->sr = 0x14; p->sg = 0x14; p->sb = 0x14;
    if (cfg && cfg->surface_base)
        parse_hex(cfg->surface_base, &p->sr, &p->sg, &p->sb);

    double sc_raw = cfg ? CLAMP(cfg->surface_contrast, 0.0, 2.0) : 1.0;
    double sc = (sc_raw <= 1.0) ? sc_raw : (1.0 + (sc_raw - 1.0) * 2.5);
    double fg_c = cfg ? CLAMP(cfg->secondary_fg_contrast, 0.5, 1.5) : 1.0;

    p->s0r  = clamp_byte(p->sr - (int)lround(4.0 * sc));
    p->s0g  = clamp_byte(p->sg - (int)lround(4.0 * sc));
    p->s0b  = clamp_byte(p->sb - (int)lround(4.0 * sc));
    p->s1r  = clamp_byte(p->sr - (int)lround(2.0 * sc));
    p->s1g  = clamp_byte(p->sg - (int)lround(2.0 * sc));
    p->s1b  = clamp_byte(p->sb - (int)lround(2.0 * sc));
    p->s2r  = clamp_byte(p->sr + (int)lround(2.0 * sc));
    p->s2g  = clamp_byte(p->sg + (int)lround(2.0 * sc));
    p->s2b  = clamp_byte(p->sb + (int)lround(2.0 * sc));
    p->s3r  = clamp_byte(p->sr + (int)lround(4.0 * sc));
    p->s3g  = clamp_byte(p->sg + (int)lround(4.0 * sc));
    p->s3b  = clamp_byte(p->sb + (int)lround(4.0 * sc));
    p->s4r  = clamp_byte(p->sr + (int)lround(6.0 * sc));
    p->s4g  = clamp_byte(p->sg + (int)lround(6.0 * sc));
    p->s4b  = clamp_byte(p->sb + (int)lround(6.0 * sc));
    p->shr  = clamp_byte(p->sr + (int)lround(14.0 * sc));
    p->shg  = clamp_byte(p->sg + (int)lround(14.0 * sc));
    p->shb  = clamp_byte(p->sb + (int)lround(14.0 * sc));
    p->bdsr = clamp_byte(p->sr + (int)lround(16.0 * sc));
    p->bdsg = clamp_byte(p->sg + (int)lround(16.0 * sc));
    p->bdsb = clamp_byte(p->sb + (int)lround(16.0 * sc));
    p->bdr  = clamp_byte(p->sr + (int)lround(23.0 * sc));
    p->bdg  = clamp_byte(p->sg + (int)lround(23.0 * sc));
    p->bdb  = clamp_byte(p->sb + (int)lround(23.0 * sc));
    p->bdhr = clamp_byte(p->sr + (int)lround(31.0 * sc));
    p->bdhg = clamp_byte(p->sg + (int)lround(31.0 * sc));
    p->bdhb = clamp_byte(p->sb + (int)lround(31.0 * sc));

    p->sec_v = CLAMP((int)lround(160.0 * fg_c), 96, 238);
    p->mut_v = CLAMP((int)lround(153.0 * fg_c), 90, 230);
    p->dim_v = CLAMP((int)lround(136.0 * fg_c), 76, 214);
}

const char *
kajo_theme_export_nearest_gnome_accent(const char *hex_color)
{
    int r = 0x00, g = 0x78, b = 0xd4;
    parse_hex(hex_color, &r, &g, &b);

    static const struct {
        const char *name;
        int r, g, b;
    } gnome_accents[] = {
        { "blue",   0x35, 0x84, 0xe4 },
        { "teal",   0x21, 0x90, 0xa4 },
        { "green",  0x3a, 0x94, 0x4a },
        { "yellow", 0xc8, 0x88, 0x00 },
        { "orange", 0xed, 0x5b, 0x00 },
        { "red",    0xe6, 0x2d, 0x42 },
        { "pink",   0xd5, 0x61, 0x99 },
        { "purple", 0x91, 0x41, 0xac },
        { "slate",  0x6f, 0x83, 0x96 },
    };

    const char *best = "blue";
    int best_dist = G_MAXINT;
    for (gsize i = 0; i < G_N_ELEMENTS(gnome_accents); i++) {
        int dr = r - gnome_accents[i].r;
        int dg = g - gnome_accents[i].g;
        int db = b - gnome_accents[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_dist) {
            best_dist = d;
            best = gnome_accents[i].name;
        }
    }
    return best;
}

char *
kajo_theme_export_generate_gtk4_css(const KajoThemeCfg *cfg)
{
    ResolvedPalette p;
    resolve_palette(cfg, &p);

    return g_strdup_printf(
        "/* Auto-generated by Kajo (kajo-settings -> Theme -> External App Theming). */\n"
        "@define-color accent_bg_color #%02x%02x%02x;\n"
        "@define-color accent_color #%02x%02x%02x;\n"
        "@define-color accent_fg_color #ffffff;\n"
        "@define-color window_bg_color #%02x%02x%02x;\n"
        "@define-color window_fg_color #ffffff;\n"
        "@define-color view_bg_color #%02x%02x%02x;\n"
        "@define-color view_fg_color #ffffff;\n"
        "@define-color headerbar_bg_color #%02x%02x%02x;\n"
        "@define-color headerbar_fg_color #ffffff;\n"
        "@define-color headerbar_border_color #%02x%02x%02x;\n"
        "@define-color headerbar_backdrop_color #%02x%02x%02x;\n"
        "@define-color card_bg_color #%02x%02x%02x;\n"
        "@define-color card_fg_color #ffffff;\n"
        "@define-color dialog_bg_color #%02x%02x%02x;\n"
        "@define-color dialog_fg_color #ffffff;\n"
        "@define-color popover_bg_color #%02x%02x%02x;\n"
        "@define-color popover_fg_color #ffffff;\n"
        "@define-color sidebar_bg_color #%02x%02x%02x;\n"
        "@define-color sidebar_fg_color #ffffff;\n"
        "@define-color sidebar_backdrop_color #%02x%02x%02x;\n"
        "@define-color secondary_sidebar_bg_color #%02x%02x%02x;\n"
        "@define-color secondary_sidebar_fg_color #ffffff;\n"
        "@define-color shade_color rgba(0, 0, 0, 0.36);\n"
        "@define-color borders #%02x%02x%02x;\n\n"
        "window, headerbar, popover > contents, menu, .card, .boxed-list,\n"
        "button:not(.osk-key), entry, searchentry, dropdown > button,\n"
        "listbox > row, listview > row, notebook > header > tabs > tab {\n"
        "    border-radius: 0px;\n"
        "}\n",
        p.ar, p.ag, p.ab,
        p.hr, p.hg, p.hb,
        p.sr, p.sg, p.sb,
        p.s1r, p.s1g, p.s1b,
        p.s0r, p.s0g, p.s0b,
        p.bdr, p.bdg, p.bdb,
        p.s0r, p.s0g, p.s0b,
        p.s2r, p.s2g, p.s2b,
        p.s2r, p.s2g, p.s2b,
        p.s3r, p.s3g, p.s3b,
        p.s0r, p.s0g, p.s0b,
        p.s0r, p.s0g, p.s0b,
        p.s1r, p.s1g, p.s1b,
        p.bdr, p.bdg, p.bdb);
}

char *
kajo_theme_export_generate_gtk3_css(const KajoThemeCfg *cfg)
{
    ResolvedPalette p;
    resolve_palette(cfg, &p);

    return g_strdup_printf(
        "/* Auto-generated by Kajo (kajo-settings -> Theme -> External App Theming). */\n"
        "@define-color theme_bg_color #%02x%02x%02x;\n"
        "@define-color theme_fg_color #ffffff;\n"
        "@define-color theme_base_color #%02x%02x%02x;\n"
        "@define-color theme_text_color #ffffff;\n"
        "@define-color theme_selected_bg_color #%02x%02x%02x;\n"
        "@define-color theme_selected_fg_color #ffffff;\n"
        "@define-color insensitive_bg_color #%02x%02x%02x;\n"
        "@define-color insensitive_fg_color #%02x%02x%02x;\n"
        "@define-color theme_unfocused_bg_color #%02x%02x%02x;\n"
        "@define-color theme_unfocused_fg_color #%02x%02x%02x;\n"
        "@define-color theme_unfocused_base_color #%02x%02x%02x;\n"
        "@define-color theme_unfocused_text_color #%02x%02x%02x;\n"
        "@define-color theme_unfocused_selected_bg_color #%02x%02x%02x;\n"
        "@define-color theme_unfocused_selected_fg_color #ffffff;\n"
        "@define-color borders #%02x%02x%02x;\n"
        "@define-color unfocused_borders #%02x%02x%02x;\n\n"
        "window, headerbar, popover, menu, button, entry, combobox > box > button,\n"
        "notebook > header > tabs > tab {\n"
        "    border-radius: 0px;\n"
        "}\n",
        p.sr, p.sg, p.sb,
        p.s1r, p.s1g, p.s1b,
        p.ar, p.ag, p.ab,
        p.s0r, p.s0g, p.s0b,
        p.dim_v, p.dim_v, p.dim_v,
        p.sr, p.sg, p.sb,
        p.sec_v, p.sec_v, p.sec_v,
        p.s1r, p.s1g, p.s1b,
        p.sec_v, p.sec_v, p.sec_v,
        p.ar, p.ag, p.ab,
        p.bdr, p.bdg, p.bdb,
        p.bdsr, p.bdsg, p.bdsb);
}

char *
kajo_theme_export_generate_qt6ct_colors(const KajoThemeCfg *cfg)
{
    ResolvedPalette p;
    resolve_palette(cfg, &p);

    char c_fg[10], c_dis[10], c_btn[10], c_light[10], c_midlight[10], c_dark[10], c_mid[10];
    char c_base[10], c_win[10], c_acc[10], c_link[10], c_alt[10], c_tip[10], c_ph[10];
    g_snprintf(c_fg,       sizeof(c_fg),       "#ffffffff");
    g_snprintf(c_dis,      sizeof(c_dis),      "#ff%02x%02x%02x", p.dim_v, p.dim_v, p.dim_v);
    g_snprintf(c_btn,      sizeof(c_btn),      "#ff%02x%02x%02x", p.s2r, p.s2g, p.s2b);
    g_snprintf(c_light,    sizeof(c_light),    "#ff%02x%02x%02x", p.bdhr, p.bdhg, p.bdhb);
    g_snprintf(c_midlight, sizeof(c_midlight), "#ff%02x%02x%02x", p.bdr, p.bdg, p.bdb);
    g_snprintf(c_dark,     sizeof(c_dark),     "#ff%02x%02x%02x", p.s0r, p.s0g, p.s0b);
    g_snprintf(c_mid,      sizeof(c_mid),      "#ff%02x%02x%02x", p.bdsr, p.bdsg, p.bdsb);
    g_snprintf(c_base,     sizeof(c_base),     "#ff%02x%02x%02x", p.s1r, p.s1g, p.s1b);
    g_snprintf(c_win,      sizeof(c_win),      "#ff%02x%02x%02x", p.sr, p.sg, p.sb);
    g_snprintf(c_acc,      sizeof(c_acc),      "#ff%02x%02x%02x", p.ar, p.ag, p.ab);
    g_snprintf(c_link,     sizeof(c_link),     "#ff%02x%02x%02x", p.hr, p.hg, p.hb);
    g_snprintf(c_alt,      sizeof(c_alt),      "#ff%02x%02x%02x", p.s2r, p.s2g, p.s2b);
    g_snprintf(c_tip,      sizeof(c_tip),      "#ff%02x%02x%02x", p.s3r, p.s3g, p.s3b);
    g_snprintf(c_ph,       sizeof(c_ph),       "#80%02x%02x%02x", p.mut_v, p.mut_v, p.mut_v);

    return g_strdup_printf(
        "[ColorScheme]\n"
        "active_colors=%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, #ff000000, %s, #ffffffff, %s, %s, %s, #ffffffff, %s, #ffffffff, %s\n"
        "disabled_colors=%s, %s, %s, %s, %s, %s, %s, #ffffffff, %s, %s, %s, #ff000000, %s, %s, %s, %s, %s, #ffffffff, %s, #ffffffff, %s\n"
        "inactive_colors=%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, #ff000000, %s, #ffffffff, %s, %s, %s, #ffffffff, %s, #ffffffff, %s\n",
        c_fg, c_btn, c_light, c_midlight, c_dark, c_mid, c_fg, c_fg, c_fg, c_base, c_win, c_acc, c_link, c_acc, c_alt, c_tip, c_ph,
        c_dis, c_btn, c_light, c_midlight, c_dark, c_mid, c_dis, c_dis, c_base, c_win, c_mid, c_dis, c_link, c_acc, c_alt, c_tip, c_ph,
        c_fg, c_btn, c_light, c_midlight, c_dark, c_mid, c_fg, c_fg, c_fg, c_base, c_win, c_acc, c_link, c_acc, c_alt, c_tip, c_ph);
}

char *
kajo_theme_export_strip_managed_block(const char *existing_content, gboolean *out_had_block)
{
    if (out_had_block)
        *out_had_block = FALSE;
    if (!existing_content)
        return g_strdup("");

    const char *start = strstr(existing_content, KAJO_MANAGED_BEGIN);
    if (!start)
        return g_strdup(existing_content);

    const char *end = strstr(start, KAJO_MANAGED_END);
    if (!end)
        return g_strdup(existing_content);

    end += strlen(KAJO_MANAGED_END);
    while (*end == '\r' || *end == '\n')
        end++;

    if (out_had_block)
        *out_had_block = TRUE;

    GString *out = g_string_new_len(existing_content, (gssize)(start - existing_content));
    while (out->len > 0 && (out->str[out->len - 1] == '\n' || out->str[out->len - 1] == '\r')) {
        if (*end == '\0')
            g_string_truncate(out, out->len - 1);
        else
            break;
    }
    g_string_append(out, end);
    if (out->len > 0 && out->str[out->len - 1] != '\n')
        g_string_append_c(out, '\n');
    return g_string_free(out, FALSE);
}

char *
kajo_theme_export_apply_managed_block(const char *existing_content,
                                      const char *import_filename)
{
    g_autofree char *cleaned = kajo_theme_export_strip_managed_block(existing_content, NULL);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out,
        "%s\n@import url(\"%s\");\n%s\n",
        KAJO_MANAGED_BEGIN,
        import_filename ? import_filename : "kajo-theme.css",
        KAJO_MANAGED_END);

    if (cleaned && cleaned[0] != '\0') {
        const char *p = cleaned;
        while (*p == '\n' || *p == '\r')
            p++;
        if (*p != '\0') {
            g_string_append_c(out, '\n');
            g_string_append(out, p);
        }
    }
    return g_string_free(out, FALSE);
}

gboolean
kajo_theme_export_qt6ct_available(void)
{
    g_autofree char *exe = g_find_program_in_path("qt6ct");
    if (exe)
        return TRUE;
    return g_file_test("/usr/lib/qt6/plugins/platformthemes/libqt6ct.so", G_FILE_TEST_EXISTS) ||
           g_file_test("/usr/lib64/qt6/plugins/platformthemes/libqt6ct.so", G_FILE_TEST_EXISTS);
}

static const char *
resolve_cfg_root(const char *config_dir)
{
    return (config_dir && config_dir[0]) ? config_dir : g_get_user_config_dir();
}

static char *
backup_json_path(const char *config_dir)
{
    return g_build_filename(resolve_cfg_root(config_dir), "kajo", KAJO_BACKUP_FILE, NULL);
}

gboolean
kajo_theme_export_has_backup(const char *config_dir)
{
    g_autofree char *path = backup_json_path(config_dir);
    return path && g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

char *
kajo_theme_export_detect_leftovers(const char *config_dir)
{
    const char *root = resolve_cfg_root(config_dir);
    static const char *const dirs[] = { "gtk-4.0", "gtk-3.0" };
    static const char *const known[] = { "dank-colors.css", "colors.css", "matugen.css", NULL };

    g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

    for (gsize d = 0; d < G_N_ELEMENTS(dirs); d++) {
        for (gsize f = 0; known[f] != NULL; f++) {
            g_autofree char *p = g_build_filename(root, dirs[d], known[f], NULL);
            if (g_file_test(p, G_FILE_TEST_IS_REGULAR) && !g_hash_table_contains(seen, known[f])) {
                char *copy = g_strdup(known[f]);
                g_hash_table_add(seen, copy);
                g_ptr_array_add(found, copy);
            }
        }
    }

    if (found->len == 0)
        return NULL;

    GString *out = g_string_new(NULL);
    for (guint i = 0; i < found->len; i++) {
        if (i > 0)
            g_string_append(out, ", ");
        g_string_append(out, g_ptr_array_index(found, i));
    }
    return g_string_free(out, FALSE);
}

/* ─── Snapshot Backup & Restore Helpers ─── */

static void
snapshot_file_into_builder(JsonBuilder *b, const char *member, const char *path)
{
    json_builder_set_member_name(b, member);
    json_builder_begin_object(b);
    g_autofree char *data = NULL;
    gsize len = 0;
    if (path && g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
        g_file_get_contents(path, &data, &len, NULL)) {
        g_autofree char *b64 = g_base64_encode((const guchar *)data, len);
        json_builder_set_member_name(b, "existed");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_set_member_name(b, "content_b64");
        json_builder_add_string_value(b, b64 ? b64 : "");
    } else {
        json_builder_set_member_name(b, "existed");
        json_builder_add_boolean_value(b, FALSE);
    }
    json_builder_end_object(b);
}

static gboolean
restore_file_from_object(JsonObject *root, const char *member, const char *path, GError **error)
{
    if (!root || !json_object_has_member(root, member))
        return TRUE;

    JsonObject *obj = json_object_get_object_member(root, member);
    if (!obj)
        return TRUE;

    gboolean existed = json_object_has_member(obj, "existed") &&
                       json_object_get_boolean_member(obj, "existed");
    if (!existed) {
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            g_unlink(path);
        return TRUE;
    }

    const char *b64 = json_object_has_member(obj, "content_b64")
        ? json_object_get_string_member(obj, "content_b64") : "";
    gsize out_len = 0;
    g_autofree guchar *decoded = g_base64_decode(b64 ? b64 : "", &out_len);

    g_autofree char *parent = g_path_get_dirname(path);
    g_mkdir_with_parents(parent, 0755);
    return g_file_set_contents(path, (const char *)(decoded ? decoded : (const guchar *)""),
                               (gssize)out_len, error);
}

static GSettings *
acquire_gnome_interface_settings(void)
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (!src)
        return NULL;
    g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup(
        src, "org.gnome.desktop.interface", TRUE);
    if (!schema)
        return NULL;
    return g_settings_new_full(schema, NULL, NULL);
}

static gboolean
ensure_backup_exists(const char *config_dir, gboolean touch_system_env, GError **error)
{
    g_autofree char *bpath = backup_json_path(config_dir);
    if (g_file_test(bpath, G_FILE_TEST_IS_REGULAR))
        return TRUE; /* Never overwrite original pre-Kajo snapshot! */

    const char *root = resolve_cfg_root(config_dir);
    g_autofree char *p_gtk4_css = g_build_filename(root, "gtk-4.0", "gtk.css", NULL);
    g_autofree char *p_gtk4_ini = g_build_filename(root, "gtk-4.0", "settings.ini", NULL);
    g_autofree char *p_gtk3_css = g_build_filename(root, "gtk-3.0", "gtk.css", NULL);
    g_autofree char *p_gtk3_ini = g_build_filename(root, "gtk-3.0", "settings.ini", NULL);
    g_autofree char *p_qt6ct    = g_build_filename(root, "qt6ct", "qt6ct.conf", NULL);

    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, 1);

    snapshot_file_into_builder(b, "gtk4_css", p_gtk4_css);
    snapshot_file_into_builder(b, "gtk4_settings_ini", p_gtk4_ini);
    snapshot_file_into_builder(b, "gtk3_css", p_gtk3_css);
    snapshot_file_into_builder(b, "gtk3_settings_ini", p_gtk3_ini);
    snapshot_file_into_builder(b, "qt6ct_conf", p_qt6ct);

    json_builder_set_member_name(b, "gsettings");
    json_builder_begin_object(b);
    g_autoptr(GSettings) gs = touch_system_env ? acquire_gnome_interface_settings() : NULL;
    if (gs) {
        GSettingsSchema *schema = NULL;
        g_object_get(gs, "settings-schema", &schema, NULL);
        json_builder_set_member_name(b, "had_schema");
        json_builder_add_boolean_value(b, TRUE);
        if (schema && g_settings_schema_has_key(schema, "color-scheme")) {
            g_autofree char *val = g_settings_get_string(gs, "color-scheme");
            json_builder_set_member_name(b, "color_scheme");
            json_builder_add_string_value(b, val ? val : "default");
        }
        if (schema && g_settings_schema_has_key(schema, "accent-color")) {
            g_autofree char *val = g_settings_get_string(gs, "accent-color");
            json_builder_set_member_name(b, "accent_color");
            json_builder_add_string_value(b, val ? val : "blue");
        }
        if (schema && g_settings_schema_has_key(schema, "gtk-theme")) {
            g_autofree char *val = g_settings_get_string(gs, "gtk-theme");
            json_builder_set_member_name(b, "gtk_theme");
            json_builder_add_string_value(b, val ? val : "Adwaita");
        }
        if (schema)
            g_settings_schema_unref(schema);
    } else {
        json_builder_set_member_name(b, "had_schema");
        json_builder_add_boolean_value(b, FALSE);
    }
    json_builder_end_object(b);

    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    g_autoptr(JsonNode) root_node = json_builder_get_root(b);
    json_generator_set_root(gen, root_node);
    g_autofree char *json_str = json_generator_to_data(gen, NULL);

    g_autofree char *parent = g_path_get_dirname(bpath);
    g_mkdir_with_parents(parent, 0755);
    return g_file_set_contents(bpath, json_str, -1, error);
}

static JsonParser *
load_backup_parser(const char *config_dir)
{
    g_autofree char *bpath = backup_json_path(config_dir);
    if (!g_file_test(bpath, G_FILE_TEST_IS_REGULAR))
        return NULL;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, bpath, NULL)) {
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }
    return parser;
}

static gboolean
write_gtk_settings_prefer_dark(const char *ini_path, GError **error)
{
    g_autofree char *parent = g_path_get_dirname(ini_path);
    g_mkdir_with_parents(parent, 0755);

    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (g_file_test(ini_path, G_FILE_TEST_IS_REGULAR))
        g_key_file_load_from_file(kf, ini_path, G_KEY_FILE_KEEP_COMMENTS, NULL);

    g_key_file_set_boolean(kf, "Settings", "gtk-application-prefer-dark-theme", TRUE);
    return g_key_file_save_to_file(kf, ini_path, error);
}

static gboolean
apply_gtk_layer(const KajoThemeCfg *cfg,
                const char         *config_dir,
                gboolean            touch_system_env,
                GError            **error)
{
    const char *root = resolve_cfg_root(config_dir);

    /* GTK 4 */
    g_autofree char *gtk4_dir = g_build_filename(root, "gtk-4.0", NULL);
    g_mkdir_with_parents(gtk4_dir, 0755);
    g_autofree char *gtk4_kajo = g_build_filename(gtk4_dir, "kajo-theme.css", NULL);
    g_autofree char *gtk4_css_path = g_build_filename(gtk4_dir, "gtk.css", NULL);
    g_autofree char *gtk4_ini_path = g_build_filename(gtk4_dir, "settings.ini", NULL);
    g_autofree char *gtk4_body = kajo_theme_export_generate_gtk4_css(cfg);
    if (!g_file_set_contents(gtk4_kajo, gtk4_body, -1, error))
        return FALSE;

    g_autofree char *existing4 = NULL;
    g_file_get_contents(gtk4_css_path, &existing4, NULL, NULL);
    g_autofree char *merged4 = kajo_theme_export_apply_managed_block(existing4, "kajo-theme.css");
    if (!g_file_set_contents(gtk4_css_path, merged4, -1, error))
        return FALSE;
    if (!write_gtk_settings_prefer_dark(gtk4_ini_path, error))
        return FALSE;

    /* GTK 3 */
    g_autofree char *gtk3_dir = g_build_filename(root, "gtk-3.0", NULL);
    g_mkdir_with_parents(gtk3_dir, 0755);
    g_autofree char *gtk3_kajo = g_build_filename(gtk3_dir, "kajo-theme.css", NULL);
    g_autofree char *gtk3_css_path = g_build_filename(gtk3_dir, "gtk.css", NULL);
    g_autofree char *gtk3_ini_path = g_build_filename(gtk3_dir, "settings.ini", NULL);
    g_autofree char *gtk3_body = kajo_theme_export_generate_gtk3_css(cfg);
    if (!g_file_set_contents(gtk3_kajo, gtk3_body, -1, error))
        return FALSE;

    g_autofree char *existing3 = NULL;
    g_file_get_contents(gtk3_css_path, &existing3, NULL, NULL);
    g_autofree char *merged3 = kajo_theme_export_apply_managed_block(existing3, "kajo-theme.css");
    if (!g_file_set_contents(gtk3_css_path, merged3, -1, error))
        return FALSE;
    if (!write_gtk_settings_prefer_dark(gtk3_ini_path, error))
        return FALSE;

    /* GSettings org.gnome.desktop.interface (dark preference + nearest accent) */
    if (touch_system_env) {
        g_autoptr(GSettings) gs = acquire_gnome_interface_settings();
        if (gs) {
            GSettingsSchema *schema = NULL;
            g_object_get(gs, "settings-schema", &schema, NULL);
            if (schema && g_settings_schema_has_key(schema, "color-scheme"))
                g_settings_set_string(gs, "color-scheme", "prefer-dark");
            if (schema && g_settings_schema_has_key(schema, "accent-color")) {
                const char *gnome_acc = kajo_theme_export_nearest_gnome_accent(
                    cfg ? cfg->accent_color : "#0078d4");
                g_settings_set_string(gs, "accent-color", gnome_acc);
            }
            if (schema)
                g_settings_schema_unref(schema);
            g_settings_sync();
        }
    }

    return TRUE;
}

static gboolean
revert_gtk_layer(JsonObject *backup_root,
                 const char *config_dir,
                 gboolean    touch_system_env,
                 GError    **error)
{
    const char *root = resolve_cfg_root(config_dir);
    g_autofree char *p_gtk4_kajo = g_build_filename(root, "gtk-4.0", "kajo-theme.css", NULL);
    g_autofree char *p_gtk4_css  = g_build_filename(root, "gtk-4.0", "gtk.css", NULL);
    g_autofree char *p_gtk4_ini  = g_build_filename(root, "gtk-4.0", "settings.ini", NULL);
    g_autofree char *p_gtk3_kajo = g_build_filename(root, "gtk-3.0", "kajo-theme.css", NULL);
    g_autofree char *p_gtk3_css  = g_build_filename(root, "gtk-3.0", "gtk.css", NULL);
    g_autofree char *p_gtk3_ini  = g_build_filename(root, "gtk-3.0", "settings.ini", NULL);

    if (g_file_test(p_gtk4_kajo, G_FILE_TEST_EXISTS))
        g_unlink(p_gtk4_kajo);
    if (g_file_test(p_gtk3_kajo, G_FILE_TEST_EXISTS))
        g_unlink(p_gtk3_kajo);

    if (backup_root) {
        if (!restore_file_from_object(backup_root, "gtk4_css", p_gtk4_css, error))
            return FALSE;
        if (!restore_file_from_object(backup_root, "gtk4_settings_ini", p_gtk4_ini, error))
            return FALSE;
        if (!restore_file_from_object(backup_root, "gtk3_css", p_gtk3_css, error))
            return FALSE;
        if (!restore_file_from_object(backup_root, "gtk3_settings_ini", p_gtk3_ini, error))
            return FALSE;

        if (touch_system_env && json_object_has_member(backup_root, "gsettings")) {
            JsonObject *gobj = json_object_get_object_member(backup_root, "gsettings");
            gboolean had_schema = gobj && json_object_has_member(gobj, "had_schema") &&
                                  json_object_get_boolean_member(gobj, "had_schema");
            if (had_schema) {
                g_autoptr(GSettings) gs = acquire_gnome_interface_settings();
                if (gs) {
                    GSettingsSchema *schema = NULL;
                    g_object_get(gs, "settings-schema", &schema, NULL);
                    if (schema && g_settings_schema_has_key(schema, "color-scheme") &&
                        json_object_has_member(gobj, "color_scheme")) {
                        g_settings_set_string(gs, "color-scheme",
                                              json_object_get_string_member(gobj, "color_scheme"));
                    }
                    if (schema && g_settings_schema_has_key(schema, "accent-color") &&
                        json_object_has_member(gobj, "accent_color")) {
                        g_settings_set_string(gs, "accent-color",
                                              json_object_get_string_member(gobj, "accent_color"));
                    }
                    if (schema && g_settings_schema_has_key(schema, "gtk-theme") &&
                        json_object_has_member(gobj, "gtk_theme")) {
                        g_settings_set_string(gs, "gtk-theme",
                                              json_object_get_string_member(gobj, "gtk_theme"));
                    }
                    if (schema)
                        g_settings_schema_unref(schema);
                    g_settings_sync();
                }
            }
        }
    } else {
        /* Fallback when no backup snapshot exists: strip only Kajo-managed blocks */
        const char *paths[2] = { p_gtk4_css, p_gtk3_css };
        for (int i = 0; i < 2; i++) {
            g_autofree char *cur = NULL;
            if (g_file_get_contents(paths[i], &cur, NULL, NULL)) {
                gboolean had = FALSE;
                g_autofree char *stripped = kajo_theme_export_strip_managed_block(cur, &had);
                if (had) {
                    g_autofree char *trimmed = g_strdup(stripped);
                    g_strstrip(trimmed);
                    if (trimmed[0] == '\0')
                        g_unlink(paths[i]);
                    else
                        g_file_set_contents(paths[i], stripped, -1, NULL);
                }
            }
        }
    }

    return TRUE;
}

static void
update_systemd_qt_env_async(gboolean enable)
{
    const char *const argv_set[]   = { "systemctl", "--user", "set-environment", "QT_QPA_PLATFORMTHEME=qt6ct", NULL };
    const char *const argv_unset[] = { "systemctl", "--user", "unset-environment", "QT_QPA_PLATFORMTHEME", NULL };
    g_autoptr(GSubprocess) proc = g_subprocess_newv(
        enable ? argv_set : argv_unset,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL);
    (void)proc;
}

static gboolean
apply_qt_layer(const KajoThemeCfg *cfg,
               const char         *config_dir,
               gboolean            touch_system_env,
               GError            **error)
{
    const char *root = resolve_cfg_root(config_dir);
    g_autofree char *colors_dir = g_build_filename(root, "qt6ct", "colors", NULL);
    g_mkdir_with_parents(colors_dir, 0755);

    g_autofree char *scheme_path = g_build_filename(colors_dir, "kajo-metro.conf", NULL);
    g_autofree char *scheme_body = kajo_theme_export_generate_qt6ct_colors(cfg);
    if (!g_file_set_contents(scheme_path, scheme_body, -1, error))
        return FALSE;

    g_autofree char *qt6ct_conf = g_build_filename(root, "qt6ct", "qt6ct.conf", NULL);
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (g_file_test(qt6ct_conf, G_FILE_TEST_IS_REGULAR))
        g_key_file_load_from_file(kf, qt6ct_conf, G_KEY_FILE_KEEP_COMMENTS, NULL);

    g_key_file_set_boolean(kf, "Appearance", "custom_palette", TRUE);
    g_key_file_set_string(kf, "Appearance", "color_scheme_path", scheme_path);
    g_key_file_set_string(kf, "Appearance", "style", "Fusion");
    if (!g_key_file_save_to_file(kf, qt6ct_conf, error))
        return FALSE;

    g_autofree char *env_dir = g_build_filename(root, "environment.d", NULL);
    g_mkdir_with_parents(env_dir, 0755);
    g_autofree char *env_file = g_build_filename(env_dir, KAJO_QT_ENV_FILE, NULL);
    static const char *k_env_body =
        "# Auto-generated by Kajo (kajo-settings -> Theme -> External App Theming)\n"
        "QT_QPA_PLATFORMTHEME=qt6ct\n";
    if (!g_file_set_contents(env_file, k_env_body, -1, error))
        return FALSE;

    if (touch_system_env)
        update_systemd_qt_env_async(TRUE);

    return TRUE;
}

static gboolean
revert_qt_layer(JsonObject *backup_root,
                const char *config_dir,
                gboolean    touch_system_env,
                GError    **error)
{
    const char *root = resolve_cfg_root(config_dir);
    g_autofree char *scheme_path = g_build_filename(root, "qt6ct", "colors", "kajo-metro.conf", NULL);
    g_autofree char *qt6ct_conf  = g_build_filename(root, "qt6ct", "qt6ct.conf", NULL);
    g_autofree char *env_file    = g_build_filename(root, "environment.d", KAJO_QT_ENV_FILE, NULL);

    if (g_file_test(scheme_path, G_FILE_TEST_EXISTS))
        g_unlink(scheme_path);
    if (g_file_test(env_file, G_FILE_TEST_EXISTS))
        g_unlink(env_file);

    if (backup_root) {
        if (!restore_file_from_object(backup_root, "qt6ct_conf", qt6ct_conf, error))
            return FALSE;
    }

    if (touch_system_env)
        update_systemd_qt_env_async(FALSE);

    return TRUE;
}

gboolean
kajo_theme_export_sync(const KajoThemeCfg *cfg,
                       const char         *config_dir,
                       gboolean            touch_system_env,
                       GError            **error)
{
    gboolean want_gtk = cfg && cfg->theme_external_gtk;
    gboolean want_qt  = cfg && cfg->theme_external_qt;

    if (!want_gtk && !want_qt) {
        if (!kajo_theme_export_has_backup(config_dir))
            return TRUE; /* Never enabled — touch nothing! */
        return kajo_theme_export_revert_all(config_dir, touch_system_env, error);
    }

    if (!ensure_backup_exists(config_dir, touch_system_env, error))
        return FALSE;

    g_autoptr(JsonParser) parser = load_backup_parser(config_dir);
    JsonObject *backup_root = parser ? json_node_get_object(json_parser_get_root(parser)) : NULL;

    if (want_gtk) {
        if (!apply_gtk_layer(cfg, config_dir, touch_system_env, error))
            return FALSE;
    } else {
        if (!revert_gtk_layer(backup_root, config_dir, touch_system_env, error))
            return FALSE;
    }

    if (want_qt) {
        if (!apply_qt_layer(cfg, config_dir, touch_system_env, error))
            return FALSE;
    } else {
        if (!revert_qt_layer(backup_root, config_dir, touch_system_env, error))
            return FALSE;
    }

    return TRUE;
}

gboolean
kajo_theme_export_revert_all(const char *config_dir,
                             gboolean    touch_system_env,
                             GError    **error)
{
    g_autoptr(JsonParser) parser = load_backup_parser(config_dir);
    JsonObject *backup_root = parser ? json_node_get_object(json_parser_get_root(parser)) : NULL;

    if (!revert_gtk_layer(backup_root, config_dir, touch_system_env, error))
        return FALSE;
    if (!revert_qt_layer(backup_root, config_dir, touch_system_env, error))
        return FALSE;

    g_autofree char *bpath = backup_json_path(config_dir);
    if (bpath && g_file_test(bpath, G_FILE_TEST_EXISTS))
        g_unlink(bpath);

    return TRUE;
}
