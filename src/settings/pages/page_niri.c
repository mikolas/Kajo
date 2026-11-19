#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../compositor/compositor.h"
#include <string.h>

#define N_NIRI_NAMESPACES 5

static const char *const NIRI_NS_IDS[N_NIRI_NAMESPACES] = {
    "kajo",
    "kajo-launcher",
    "kajo-notification",
    "kajo-osk",
    "kajo-osd"
};

static const char *const NIRI_NS_LABELS[] = {
    "kajo (Panel & Popovers)",
    "kajo-launcher (App Launcher)",
    "kajo-notification (Toasts)",
    "kajo-osk (Virtual Keyboard)",
    "kajo-osd (Volume / Brightness HUD)",
    NULL
};

typedef struct {
    const char *id;
    const char *label;
    int         rec_duration_ms;
    guint       rec_curve_idx;
} ShaderPreset;

static const char *const CURVE_NAMES[] = {
    "ease-out-expo",
    "ease-out-cubic",
    "ease-out-quad",
    "linear",
    NULL
};
#define N_CURVES 4

static const ShaderPreset OPEN_PRESETS[] = {
    { "default",          "Default (Scale & Fade)",          150, 0 },
    { "metro-flip",       "Metro Turnstile (3D Flip)",       250, 1 },
    { "expanding-circle", "Iris Circle Reveal",              250, 2 },
    { "crt-on",           "CRT Power-On (Scanline Beam)",    300, 1 },
    { "pixelate-in",      "Pixelate Materialize",            250, 2 },
    { "glitch-in",        "Cyberpunk RGB Glitch",            250, 3 },
    { "curtain-open",     "Vertical Split Curtain",          220, 0 },
};
#define N_OPEN_PRESETS G_N_ELEMENTS(OPEN_PRESETS)

static const ShaderPreset CLOSE_PRESETS[] = {
    { "default",          "Default (Scale & Fade)",          200, 2 },
    { "fall-and-rotate",  "Gravity Fall & Rotate",           350, 3 },
    { "crt-off",          "CRT TV Turn-Off",                 360, 3 },
    { "ash-dissolve",     "Ash / Particle Dissolve",         420, 3 },
    { "vortex-implode",   "Black Hole Vortex",               380, 3 },
    { "pixelate-out",     "Pixelate Dematerialize",          350, 3 },
    { "slice-shatter",    "Horizontal Slice Shred",          350, 3 },
};
#define N_CLOSE_PRESETS G_N_ELEMENTS(CLOSE_PRESETS)

static const ShaderPreset RESIZE_PRESETS[] = {
    { "default",          "Default (Crossfade & Crop)",      0,   0 },
    { "stretch-next",     "Elastic Stretch",                 0,   0 },
    { "pixelate-morph",   "Pixelate Morph",                  0,   0 },
    { "glitch-morph",     "Chromatic Glitch Resize",         0,   0 },
};
#define N_RESIZE_PRESETS G_N_ELEMENTS(RESIZE_PRESETS)

static const char *const COLOR_FILTER_IDS[] = {
    "none",
    "grayscale",
    "night-light",
    "protanopia",
    "deuteranopia",
    "tritanopia",
};
#define N_COLOR_FILTERS G_N_ELEMENTS(COLOR_FILTER_IDS)

static const char *const COLOR_FILTER_LABELS[] = {
    "None (Unfiltered sRGB / BT.2020)",
    "Grayscale (Rec.709 Luminance)",
    "Night Light (Warm White Point)",
    "Protanopia (Red-Weak CVD Aid)",
    "Deuteranopia (Green-Weak CVD Aid)",
    "Tritanopia (Blue-Weak CVD Aid)",
    NULL
};

typedef struct {
    const char *label;
    int         passes;
    double      offset;
} GlassBlurPreset;

static const GlassBlurPreset GLASS_BLUR_PRESETS[] = {
    { "Light",  2, 1.5 },
    { "Medium", 3, 2.0 },
    { "Strong", 4, 2.5 },
    { "Heavy",  5, 3.0 },
};
#define N_GLASS_BLUR_PRESETS 4

static const char *const GLASS_BLUR_LABELS[] = { "Light", "Medium", "Strong", "Heavy", NULL };

static const char *const GLASS_STYLE_IDS[] = {
    "frosted", "liquid", "halation", "fluted", "acrylic", "aero", "dichroic", NULL
};
static const char *const GLASS_STYLE_LABELS[] = {
    "Frosted (Kawase)",
    "Liquid Glass (Refractive)",
    "Halation (Light Scatter)",
    "Fluted (Reeded Lenses)",
    "Acrylic (Etched Micro-Facet)",
    "Aero Sheen (Beveled Crystal)",
    "Dichroic (Iridescent Rim)",
    NULL
};
#define N_GLASS_STYLES 7

static const char *const FLUTE_AXIS_IDS[]    = { "vertical", "horizontal", NULL };
static const char *const FLUTE_AXIS_LABELS[] = { "Vertical", "Horizontal", NULL };

static guint
glass_style_idx_from_id(const char *id)
{
    if (!id || !id[0])
        return 0;
    for (guint i = 0; i < N_GLASS_STYLES; i++) {
        if (g_ascii_strcasecmp(id, GLASS_STYLE_IDS[i]) == 0)
            return i;
    }
    return 0;
}

static const int SHADOW_STRENGTH_PCTS[] = { 60, 100, 145 };
#define N_SHADOW_STRENGTH_PRESETS 3
static const char *const SHADOW_STRENGTH_LABELS[] = { "Subtle", "Standard", "Deep", NULL };

static guint
glass_blur_idx_from_cfg(int passes, double offset)
{
    guint best = 1;
    double best_d = 1e9;
    for (guint i = 0; i < N_GLASS_BLUR_PRESETS; i++) {
        double dp = (double)(passes - GLASS_BLUR_PRESETS[i].passes);
        double doff = offset - GLASS_BLUR_PRESETS[i].offset;
        double d = dp * dp + doff * doff;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static guint
shadow_strength_idx_from_pct(int pct)
{
    if (pct <= 80)
        return 0;
    if (pct >= 122)
        return 2;
    return 1;
}

typedef struct _SettingsPageNiri {
    GtkBox parent_instance;

    GtkWidget *combo_ns;
    GtkWidget *scale_op;
    GtkWidget *scale_gap;
    GtkWidget *scale_border_px;
    GtkWidget *sw_xray;

    GtkWidget *row_open_shader;
    GtkWidget *combo_shader_open;
    GtkWidget *scale_open_dur;
    GtkWidget *combo_open_curve;

    GtkWidget *row_close_shader;
    GtkWidget *combo_shader_close;
    GtkWidget *scale_close_dur;
    GtkWidget *combo_close_curve;

    GtkWidget *combo_shader_resize;

    GtkWidget *tc_glass;
    GtkWidget *tc_shadow;
    GtkWidget *tc_dim;
    GtkWidget *tc_dither;
    GtkWidget *combo_glass_style;
    GtkWidget *seg_glass_blur;
    GtkWidget *scale_glass_bright;
    GtkWidget *scale_glass_sat;
    GtkWidget *entry_glass_tint;
    GtkWidget *scale_glass_tint_alpha;
    GtkWidget *scale_glass_refraction;
    GtkWidget *scale_glass_dispersion;
    GtkWidget *scale_glass_bevel;
    GtkWidget *scale_glass_specular;
    GtkWidget *scale_glass_grain;
    GtkWidget *scale_glass_flute_px;
    GtkWidget *seg_glass_flute_axis;
    GtkWidget *seg_shadow_strength;
    GtkWidget *scale_shadow_unfocused;
    GtkWidget *scale_dim_opacity;
    GtkWidget *row_glass_style;
    GtkWidget *row_glass_blur;
    GtkWidget *row_glass_bright;
    GtkWidget *row_glass_sat;
    GtkWidget *row_glass_tint;
    GtkWidget *row_glass_refraction;
    GtkWidget *row_glass_dispersion;
    GtkWidget *row_glass_bevel;
    GtkWidget *row_glass_specular;
    GtkWidget *row_glass_grain;
    GtkWidget *row_glass_flute;
    GtkWidget *row_shadow_strength;
    GtkWidget *row_shadow_unfocused;
    GtkWidget *row_dim_opacity;
    GtkWidget *combo_color_filter;
    GtkWidget *scale_filter_strength;
    GtkWidget *scale_night_temp;
    GtkWidget *row_filter_strength;
    GtkWidget *row_night_temp;

    int      ns_opacity[N_NIRI_NAMESPACES];
    gboolean ns_xray[N_NIRI_NAMESPACES];
    int      gap_px;

    KajoCompEffectsCfg fx_cfg;
    guint              color_filter_idx;

    guint    open_preset_idx;
    int      open_duration_ms;
    guint    open_curve_idx;

    guint    close_preset_idx;
    int      close_duration_ms;
    guint    close_curve_idx;

    guint    resize_preset_idx;

    guint    preview_spawn_timer_id;
    gboolean syncing_ui;
} SettingsPageNiri;

typedef struct _SettingsPageNiriClass {
    GtkBoxClass parent_class;
} SettingsPageNiriClass;

G_DEFINE_TYPE(SettingsPageNiri, settings_page_niri, GTK_TYPE_BOX)

/* ─── 5-Tier Shader File Source Loader ─── */

static gchar *
load_shader_source(const char *category, const char *preset_id)
{
    if (!category || !preset_id || g_strcmp0(preset_id, "default") == 0)
        return NULL;

    g_autofree gchar *filename = g_strdup_printf("%s.frag", preset_id);
    g_autofree gchar *cwd = g_get_current_dir();

    gchar *candidates[5] = {
        g_build_filename(g_get_user_data_dir(), "kajo", "shaders", category, filename, NULL),
        g_build_filename("/usr/local/share/kajo/shaders", category, filename, NULL),
        g_build_filename("/usr/share/kajo/shaders", category, filename, NULL),
        g_build_filename(cwd, "data", "shaders", category, filename, NULL),
#ifdef KAJO_SOURCE_DIR
        g_build_filename(KAJO_SOURCE_DIR, "data", "shaders", category, filename, NULL),
#else
        NULL,
#endif
    };

    gchar *contents = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
        if (candidates[i] && !contents && g_file_test(candidates[i], G_FILE_TEST_EXISTS)) {
            g_file_get_contents(candidates[i], &contents, NULL, NULL);
        }
        g_free(candidates[i]);
    }

    return contents;
}

/* ─── String-Aware KDL Brace Matcher & Config Parsing Helpers ─── */

static const char *
find_matching_kdl_brace(const char *open_brace)
{
    if (!open_brace || *open_brace != '{')
        return NULL;

    int depth = 0;
    const char *p = open_brace;
    gboolean in_string = FALSE;
    gboolean raw_string = FALSE;

    while (*p) {
        if (in_string) {
            if (!raw_string && *p == '\\' && p[1] != '\0') {
                p += 2;
                continue;
            }
            if (*p == '"')
                in_string = FALSE;
            p++;
            continue;
        }

        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }

        if (*p == '"') {
            in_string = TRUE;
            raw_string = (p > open_brace && p[-1] == 'r');
            p++;
            continue;
        }

        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0)
                return p;
        }
        p++;
    }

    return NULL;
}

static void
parse_anim_subblock(const char         *anim_block,
                    const char         *subblock_name,
                    const ShaderPreset *presets,
                    guint               n_presets,
                    guint              *out_preset_idx,
                    int                *out_duration_ms,
                    guint              *out_curve_idx)
{
    if (!anim_block || !subblock_name)
        return;

    const char *p = strstr(anim_block, subblock_name);
    if (!p)
        return;

    const char *open_brace = strchr(p, '{');
    if (!open_brace)
        return;

    const char *close_brace = find_matching_kdl_brace(open_brace);
    if (!close_brace)
        return;

    g_autofree gchar *section = g_strndup(open_brace, (gsize)(close_brace - open_brace));

    if (out_duration_ms) {
        const char *dur = strstr(section, "duration-ms ");
        if (dur) {
            int val = (int)g_ascii_strtoll(dur + 12, NULL, 10);
            if (val >= 50 && val <= 2000)
                *out_duration_ms = val;
        }
    }

    if (out_curve_idx) {
        const char *crv = strstr(section, "curve \"");
        if (crv) {
            crv += 7;
            for (guint i = 0; i < N_CURVES; i++) {
                gsize len = strlen(CURVE_NAMES[i]);
                if (strncmp(crv, CURVE_NAMES[i], len) == 0 && crv[len] == '"') {
                    *out_curve_idx = i;
                    break;
                }
            }
        }
    }

    if (out_preset_idx) {
        *out_preset_idx = 0;
        const char *sh = strstr(section, "custom-shader");
        if (sh) {
            for (guint i = 1; i < n_presets; i++) {
                g_autofree gchar *tag_token = g_strdup_printf("kajo-shader: %s", presets[i].id);
                g_autofree gchar *frag_token = g_strdup_printf("/%s.frag", presets[i].id);
                if (strstr(sh, tag_token) || strstr(sh, frag_token)) {
                    *out_preset_idx = i;
                    break;
                }
            }
        }
    }
}

static guint
color_filter_index_from_id(const char *id)
{
    if (!id || !*id)
        return 0;
    if (g_ascii_strcasecmp(id, "night_light") == 0)
        return 2;
    for (guint i = 0; i < N_COLOR_FILTERS; i++) {
        if (g_ascii_strcasecmp(id, COLOR_FILTER_IDS[i]) == 0)
            return i;
    }
    return 0;
}

static void
reset_niri_defaults_in_memory(SettingsPageNiri *self)
{
    for (int i = 0; i < N_NIRI_NAMESPACES; i++) {
        self->ns_opacity[i] = 85;
        self->ns_xray[i]    = (i == 0);
    }
    self->gap_px            = 12;
    self->open_preset_idx   = 0;
    self->open_duration_ms  = 150;
    self->open_curve_idx    = 0;
    self->close_preset_idx  = 0;
    self->close_duration_ms = 200;
    self->close_curve_idx   = 2;
    self->resize_preset_idx = 0;

    self->fx_cfg.border_px                  = 1;
    self->fx_cfg.glass_enabled              = TRUE;
    g_strlcpy(self->fx_cfg.glass_style, "frosted", sizeof(self->fx_cfg.glass_style));
    self->fx_cfg.glass_passes               = 3;
    self->fx_cfg.glass_offset               = 2.0;
    self->fx_cfg.glass_brightness_pct       = 90;
    self->fx_cfg.glass_saturation_pct       = 115;
    g_strlcpy(self->fx_cfg.glass_tint, "#141414", sizeof(self->fx_cfg.glass_tint));
    self->fx_cfg.glass_tint_alpha_pct       = 35;
    self->fx_cfg.glass_refraction_pct       = 45;
    self->fx_cfg.glass_dispersion_pct       = 25;
    self->fx_cfg.glass_bevel_px             = 14;
    self->fx_cfg.glass_specular_pct         = 55;
    self->fx_cfg.glass_grain_pct            = 35;
    self->fx_cfg.glass_flute_px             = 14;
    g_strlcpy(self->fx_cfg.glass_flute_axis, "vertical", sizeof(self->fx_cfg.glass_flute_axis));
    self->fx_cfg.shadow_enabled             = TRUE;
    self->fx_cfg.shadow_strength_pct        = 100;
    self->fx_cfg.shadow_unfocused_scale_pct = 60;
    self->fx_cfg.dim_enabled                = FALSE;
    self->fx_cfg.dim_opacity_pct            = 15;
    self->fx_cfg.dither_enabled             = TRUE;
    g_strlcpy(self->fx_cfg.color_filter, "none", sizeof(self->fx_cfg.color_filter));
    self->fx_cfg.color_filter_strength_pct  = 100;
    self->fx_cfg.night_light_temperature    = 4000;
    self->color_filter_idx                  = 0;
}

static void
load_existing_niri_config(SettingsPageNiri *self)
{
    reset_niri_defaults_in_memory(self);

    const gchar *cfg_dir = g_get_user_config_dir();

    /* 1. Read 40-layer-rules.kdl */
    g_autofree gchar *layer_path = g_build_filename(
        cfg_dir, "niri", "config.d", "40-layer-rules.kdl", NULL);
    g_autofree gchar *layer_contents = NULL;
    if (g_file_get_contents(layer_path, &layer_contents, NULL, NULL) && layer_contents) {
        for (int i = 0; i < N_NIRI_NAMESPACES; i++) {
            g_autofree gchar *needle = g_strdup_printf("namespace=\"%s\"", NIRI_NS_IDS[i]);
            const char *pos = strstr(layer_contents, needle);
            if (!pos)
                continue;

            const char *block_end = strstr(pos, "\n}");
            const char *op_pos = strstr(pos, "opacity ");
            if (op_pos && (!block_end || op_pos < block_end)) {
                double val = g_ascii_strtod(op_pos + 8, NULL);
                if (val > 0.0 && val <= 1.0)
                    self->ns_opacity[i] = (int)(val * 100.0 + 0.5);
            }
            const char *xray_pos = strstr(pos, "xray ");
            if (xray_pos && (!block_end || xray_pos < block_end))
                self->ns_xray[i] = g_str_has_prefix(xray_pos + 5, "true");
        }
    }

    /* 2. Read 00-kajo.kdl for gaps */
    g_autofree gchar *kajo_path = g_build_filename(
        cfg_dir, "niri", "config.d", "00-kajo.kdl", NULL);
    g_autofree gchar *kajo_contents = NULL;
    if (g_file_get_contents(kajo_path, &kajo_contents, NULL, NULL) && kajo_contents) {
        const char *gaps_pos = strstr(kajo_contents, "gaps ");
        if (gaps_pos) {
            int g = (int)g_ascii_strtoll(gaps_pos + 5, NULL, 10);
            if (g >= 0 && g <= 64)
                self->gap_px = g;
        }
    }

    /* 3. Read 30-layout-animations.kdl (or fallback to 00-kajo.kdl) for shader presets */
    g_autofree gchar *anim_path = g_build_filename(
        cfg_dir, "niri", "config.d", "30-layout-animations.kdl", NULL);
    g_autofree gchar *anim_contents = NULL;
    const char *anim_source = NULL;
    if (g_file_get_contents(anim_path, &anim_contents, NULL, NULL) && anim_contents) {
        anim_source = anim_contents;
    } else if (kajo_contents) {
        anim_source = kajo_contents;
    }

    if (anim_source) {
        const char *anim_block = strstr(anim_source, "animations {");
        if (anim_block) {
            parse_anim_subblock(anim_block, "window-open",
                                OPEN_PRESETS, N_OPEN_PRESETS,
                                &self->open_preset_idx,
                                &self->open_duration_ms,
                                &self->open_curve_idx);
            parse_anim_subblock(anim_block, "window-close",
                                CLOSE_PRESETS, N_CLOSE_PRESETS,
                                &self->close_preset_idx,
                                &self->close_duration_ms,
                                &self->close_curve_idx);
            parse_anim_subblock(anim_block, "window-resize",
                                RESIZE_PRESETS, N_RESIZE_PRESETS,
                                &self->resize_preset_idx,
                                NULL, NULL);
        }
    }

    /* 4. Read kajo-comp comp.toml & rules.toml (plus conf.d/ drop-ins) */
    kajo_config_comp_read_layout_and_layers(
        NULL, &self->gap_px, NIRI_NS_IDS, self->ns_opacity, N_NIRI_NAMESPACES);
    kajo_config_comp_read_effects(
        NULL, &self->fx_cfg, NIRI_NS_IDS, self->ns_xray, N_NIRI_NAMESPACES);
    self->color_filter_idx = color_filter_index_from_id(self->fx_cfg.color_filter);

    /* Apply comp.toml [effects.transitions] when present (overrides KDL fallback) */
    for (guint i = 0; i < N_OPEN_PRESETS; i++) {
        if (g_ascii_strcasecmp(OPEN_PRESETS[i].id, self->fx_cfg.open_preset) == 0) {
            self->open_preset_idx = i;
            break;
        }
    }
    if (self->fx_cfg.open_duration_ms >= 50 && self->fx_cfg.open_duration_ms <= 800)
        self->open_duration_ms = self->fx_cfg.open_duration_ms;
    for (guint i = 0; i < N_CURVES; i++) {
        if (g_ascii_strcasecmp(CURVE_NAMES[i], self->fx_cfg.open_curve) == 0) {
            self->open_curve_idx = i;
            break;
        }
    }
    for (guint i = 0; i < N_CLOSE_PRESETS; i++) {
        if (g_ascii_strcasecmp(CLOSE_PRESETS[i].id, self->fx_cfg.close_preset) == 0) {
            self->close_preset_idx = i;
            break;
        }
    }
    if (self->fx_cfg.close_duration_ms >= 50 && self->fx_cfg.close_duration_ms <= 800)
        self->close_duration_ms = self->fx_cfg.close_duration_ms;
    for (guint i = 0; i < N_CURVES; i++) {
        if (g_ascii_strcasecmp(CURVE_NAMES[i], self->fx_cfg.close_curve) == 0) {
            self->close_curve_idx = i;
            break;
        }
    }
    for (guint i = 0; i < N_RESIZE_PRESETS; i++) {
        if (g_ascii_strcasecmp(RESIZE_PRESETS[i].id, self->fx_cfg.resize_preset) == 0) {
            self->resize_preset_idx = i;
            break;
        }
    }
}

/* ─── Non-Destructive KDL Animations Formatter ─── */

static gchar *
build_animations_kdl_block(SettingsPageNiri *self)
{
    GString *s = g_string_new("animations {\n");

    g_string_append(s,
        "    workspace-switch {\n"
        "        spring damping-ratio=0.80 stiffness=523 epsilon=0.0001\n"
        "    }\n");

    /* window-open */
    g_string_append_printf(s,
        "    window-open {\n"
        "        duration-ms %d\n"
        "        curve \"%s\"\n",
        self->open_duration_ms,
        CURVE_NAMES[self->open_curve_idx < N_CURVES ? self->open_curve_idx : 0]);
    if (self->open_preset_idx > 0 && self->open_preset_idx < N_OPEN_PRESETS) {
        const char *id = OPEN_PRESETS[self->open_preset_idx].id;
        g_autofree gchar *open_src = load_shader_source("open", id);
        if (open_src)
            g_string_append_printf(s, "        custom-shader r\"\n// kajo-shader: %s\n%s\"\n", id, open_src);
    }
    g_string_append(s, "    }\n");

    /* window-close */
    g_string_append_printf(s,
        "    window-close {\n"
        "        duration-ms %d\n"
        "        curve \"%s\"\n",
        self->close_duration_ms,
        CURVE_NAMES[self->close_curve_idx < N_CURVES ? self->close_curve_idx : 2]);
    if (self->close_preset_idx > 0 && self->close_preset_idx < N_CLOSE_PRESETS) {
        const char *id = CLOSE_PRESETS[self->close_preset_idx].id;
        g_autofree gchar *close_src = load_shader_source("close", id);
        if (close_src)
            g_string_append_printf(s, "        custom-shader r\"\n// kajo-shader: %s\n%s\"\n", id, close_src);
    }
    g_string_append(s, "    }\n");

    g_string_append(s,
        "    horizontal-view-movement {\n"
        "        spring damping-ratio=0.85 stiffness=423 epsilon=0.0001\n"
        "    }\n"
        "    window-movement {\n"
        "        spring damping-ratio=0.75 stiffness=323 epsilon=0.0001\n"
        "    }\n");

    /* window-resize */
    g_string_append(s,
        "    window-resize {\n"
        "        spring damping-ratio=0.85 stiffness=423 epsilon=0.0001\n");
    if (self->resize_preset_idx > 0 && self->resize_preset_idx < N_RESIZE_PRESETS) {
        const char *id = RESIZE_PRESETS[self->resize_preset_idx].id;
        g_autofree gchar *resize_src = load_shader_source("resize", id);
        if (resize_src)
            g_string_append_printf(s, "        custom-shader r\"\n// kajo-shader: %s\n%s\"\n", id, resize_src);
    }
    g_string_append(s, "    }\n");

    g_string_append(s,
        "    config-notification-open-close {\n"
        "        spring damping-ratio=0.65 stiffness=923 epsilon=0.001\n"
        "    }\n"
        "    screenshot-ui-open {\n"
        "        duration-ms 200\n"
        "        curve \"ease-out-quad\"\n"
        "    }\n"
        "    overview-open-close {\n"
        "        spring damping-ratio=0.85 stiffness=800 epsilon=0.0001\n"
        "    }\n"
        "}\n");

    return g_string_free(s, FALSE);
}

static void
save_animations_to_30_kdl(const gchar *dir, const gchar *anim_block_str)
{
    g_autofree gchar *anim_path = g_build_filename(dir, "30-layout-animations.kdl", NULL);
    g_autofree gchar *existing = NULL;

    if (g_file_get_contents(anim_path, &existing, NULL, NULL) && existing) {
        const char *start = strstr(existing, "animations {");
        if (start) {
            const char *open_brace = strchr(start, '{');
            const char *close_brace = find_matching_kdl_brace(open_brace);
            if (close_brace) {
                const char *end = close_brace + 1;
                if (*end == '\n')
                    end++;

                GString *merged = g_string_new_len(existing, (gssize)(start - existing));
                g_string_append(merged, anim_block_str);
                g_string_append(merged, end);
                g_file_set_contents(anim_path, merged->str, merged->len, NULL);
                g_string_free(merged, TRUE);
                return;
            }
        }

        g_autofree gchar *appended = g_strdup_printf("%s\n%s", existing, anim_block_str);
        g_file_set_contents(anim_path, appended, -1, NULL);
        return;
    }

    g_file_set_contents(anim_path, anim_block_str, -1, NULL);
}

/* ─── UI Sync & Telemetry ─── */

static void
update_niri_subtitles_and_telemetry(SettingsPageNiri *self)
{
    if (!self)
        return;

    guint ns_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
    if (ns_idx >= N_NIRI_NAMESPACES)
        ns_idx = 0;

    guint open_crv = self->open_curve_idx < N_CURVES ? self->open_curve_idx : 0;
    guint close_crv = self->close_curve_idx < N_CURVES ? self->close_curve_idx : 2;
    guint open_idx = self->open_preset_idx < N_OPEN_PRESETS ? self->open_preset_idx : 0;
    guint close_idx = self->close_preset_idx < N_CLOSE_PRESETS ? self->close_preset_idx : 0;
    guint resize_idx = self->resize_preset_idx < N_RESIZE_PRESETS ? self->resize_preset_idx : 0;

    if (self->row_open_shader) {
        g_autofree gchar *sub = g_strdup_printf("%dms • %s", self->open_duration_ms, CURVE_NAMES[open_crv]);
        boxed_row_set_subtitle(self->row_open_shader, sub);
    }
    if (self->row_close_shader) {
        g_autofree gchar *sub = g_strdup_printf("%dms • %s", self->close_duration_ms, CURVE_NAMES[close_crv]);
        boxed_row_set_subtitle(self->row_close_shader, sub);
    }

    guint cf_idx = self->color_filter_idx < N_COLOR_FILTERS ? self->color_filter_idx : 0;

    g_autofree gchar *c0 = g_strdup_printf("%s (%d%%)", NIRI_NS_IDS[ns_idx], self->ns_opacity[ns_idx]);
    g_autofree gchar *c1 = g_strdup_printf("%dPX", self->gap_px);
    g_autofree gchar *c2 = g_strdup_printf(
        "%s%s%s",
        self->fx_cfg.glass_enabled ? "GLASS " : "",
        self->fx_cfg.shadow_enabled ? "SHADOW " : "",
        self->fx_cfg.dither_enabled ? "DITHER" : (self->fx_cfg.dim_enabled ? "DIM" : "OFF"));
    g_strstrip(c2);
    if (c2[0] == '\0') {
        g_free(c2);
        c2 = g_strdup("OFF");
    }
    g_autofree gchar *cf_up = g_ascii_strup(COLOR_FILTER_IDS[cf_idx], -1);
    g_autofree gchar *c3 = (cf_idx == 2)
        ? g_strdup_printf("%s (%dK)", cf_up, self->fx_cfg.night_light_temperature)
        : g_strdup(cf_up);

    settings_page_set_counter(GTK_WIDGET(self), 0, "NAMESPACE", c0, "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "GAPS", c1, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "EFFECTS", c2,
                              (self->fx_cfg.glass_enabled || self->fx_cfg.shadow_enabled) ? "accent" : NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "COLOUR STAGE", c3,
                              cf_idx > 0 ? "accent" : NULL);

    gboolean has_glsl = (open_idx > 0 || close_idx > 0 || resize_idx > 0 || cf_idx > 0);
    settings_page_set_nav_badge(
        GTK_WIDGET(self), "niri", has_glsl ? "KFX" : "TOML", has_glsl ? "accent" : NULL);
}

static void
update_fx_row_sensitivity(SettingsPageNiri *self)
{
    if (!self)
        return;
    gboolean g_on = self->fx_cfg.glass_enabled;
    gboolean s_on = self->fx_cfg.shadow_enabled;
    gboolean d_on = self->fx_cfg.dim_enabled;
    guint    st   = glass_style_idx_from_id(self->fx_cfg.glass_style);
    if (self->row_glass_style)       gtk_widget_set_sensitive(self->row_glass_style, g_on);
    if (self->row_glass_blur)        gtk_widget_set_sensitive(self->row_glass_blur, g_on);
    if (self->row_glass_bright)      gtk_widget_set_sensitive(self->row_glass_bright, g_on);
    if (self->row_glass_sat)         gtk_widget_set_sensitive(self->row_glass_sat, g_on);
    if (self->row_glass_tint)        gtk_widget_set_sensitive(self->row_glass_tint, g_on);
    if (self->row_glass_refraction) {
        gtk_widget_set_visible(self->row_glass_refraction, st == 1 || st == 3);
        gtk_widget_set_sensitive(self->row_glass_refraction, g_on);
    }
    if (self->row_glass_dispersion) {
        gtk_widget_set_visible(self->row_glass_dispersion, st == 1 || st == 3 || st == 6);
        gtk_widget_set_sensitive(self->row_glass_dispersion, g_on);
    }
    if (self->row_glass_bevel) {
        gtk_widget_set_visible(self->row_glass_bevel, st == 1 || st == 5 || st == 6);
        gtk_widget_set_sensitive(self->row_glass_bevel, g_on);
    }
    if (self->row_glass_specular) {
        gtk_widget_set_visible(self->row_glass_specular, st != 0);
        gtk_widget_set_sensitive(self->row_glass_specular, g_on);
    }
    if (self->row_glass_grain) {
        gtk_widget_set_visible(self->row_glass_grain, st == 2 || st == 4);
        gtk_widget_set_sensitive(self->row_glass_grain, g_on);
    }
    if (self->row_glass_flute) {
        gtk_widget_set_visible(self->row_glass_flute, st == 3);
        gtk_widget_set_sensitive(self->row_glass_flute, g_on);
    }
    if (self->row_shadow_strength)   gtk_widget_set_sensitive(self->row_shadow_strength, s_on);
    if (self->row_shadow_unfocused)  gtk_widget_set_sensitive(self->row_shadow_unfocused, s_on);
    if (self->row_dim_opacity)       gtk_widget_set_sensitive(self->row_dim_opacity, d_on);
    if (self->row_filter_strength)   gtk_widget_set_sensitive(self->row_filter_strength, self->color_filter_idx != 0);
    if (self->row_night_temp)        gtk_widget_set_sensitive(self->row_night_temp, self->color_filter_idx == 2);
}

static void
sync_niri_widgets_from_state(SettingsPageNiri *self)
{
    if (!self)
        return;

    guint ns_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
    if (ns_idx >= N_NIRI_NAMESPACES)
        ns_idx = 0;

    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_op), self->ns_opacity[ns_idx]);
    gtk_switch_set_active(GTK_SWITCH(self->sw_xray), self->ns_xray[ns_idx]);
    gtk_range_set_value(GTK_RANGE(self->scale_gap), self->gap_px);
    if (self->scale_border_px)
        gtk_range_set_value(GTK_RANGE(self->scale_border_px), self->fx_cfg.border_px);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_shader_open), self->open_preset_idx);
    gtk_range_set_value(GTK_RANGE(self->scale_open_dur), self->open_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_open_curve), self->open_curve_idx);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_shader_close), self->close_preset_idx);
    gtk_range_set_value(GTK_RANGE(self->scale_close_dur), self->close_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_close_curve), self->close_curve_idx);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_shader_resize), self->resize_preset_idx);

    if (self->tc_glass)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tc_glass), self->fx_cfg.glass_enabled);
    if (self->tc_shadow)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tc_shadow), self->fx_cfg.shadow_enabled);
    if (self->tc_dim)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tc_dim), self->fx_cfg.dim_enabled);
    if (self->tc_dither)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tc_dither), self->fx_cfg.dither_enabled);

    if (self->combo_glass_style)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_glass_style),
                                   glass_style_idx_from_id(self->fx_cfg.glass_style));
    if (self->seg_glass_blur)
        segmented_group_set_selected(self->seg_glass_blur,
            glass_blur_idx_from_cfg(self->fx_cfg.glass_passes, self->fx_cfg.glass_offset));
    if (self->scale_glass_bright)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_bright), self->fx_cfg.glass_brightness_pct);
    if (self->scale_glass_sat)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_sat), self->fx_cfg.glass_saturation_pct);
    if (self->entry_glass_tint)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_glass_tint),
                              self->fx_cfg.glass_tint[0] ? self->fx_cfg.glass_tint : "#141414");
    if (self->scale_glass_tint_alpha)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_tint_alpha), self->fx_cfg.glass_tint_alpha_pct);
    if (self->scale_glass_refraction)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_refraction), self->fx_cfg.glass_refraction_pct);
    if (self->scale_glass_dispersion)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_dispersion), self->fx_cfg.glass_dispersion_pct);
    if (self->scale_glass_bevel)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_bevel), self->fx_cfg.glass_bevel_px);
    if (self->scale_glass_specular)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_specular), self->fx_cfg.glass_specular_pct);
    if (self->scale_glass_grain)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_grain), self->fx_cfg.glass_grain_pct);
    if (self->scale_glass_flute_px)
        gtk_range_set_value(GTK_RANGE(self->scale_glass_flute_px), self->fx_cfg.glass_flute_px);
    if (self->seg_glass_flute_axis) {
        guint ax = (g_ascii_strcasecmp(self->fx_cfg.glass_flute_axis, "horizontal") == 0) ? 1 : 0;
        segmented_group_set_selected(self->seg_glass_flute_axis, ax);
    }
    if (self->seg_shadow_strength)
        segmented_group_set_selected(self->seg_shadow_strength,
            shadow_strength_idx_from_pct(self->fx_cfg.shadow_strength_pct));
    if (self->scale_shadow_unfocused)
        gtk_range_set_value(GTK_RANGE(self->scale_shadow_unfocused), self->fx_cfg.shadow_unfocused_scale_pct);
    if (self->scale_dim_opacity)
        gtk_range_set_value(GTK_RANGE(self->scale_dim_opacity), self->fx_cfg.dim_opacity_pct);

    if (self->combo_color_filter)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_color_filter), self->color_filter_idx);
    if (self->scale_filter_strength)
        gtk_range_set_value(GTK_RANGE(self->scale_filter_strength), self->fx_cfg.color_filter_strength_pct);
    if (self->scale_night_temp)
        gtk_range_set_value(GTK_RANGE(self->scale_night_temp), self->fx_cfg.night_light_temperature);
    update_fx_row_sensitivity(self);
    self->syncing_ui = FALSE;

    update_niri_subtitles_and_telemetry(self);
}

static void
save_and_reload_niri_config(SettingsPageNiri *self)
{
    guint active_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
    if (active_idx >= N_NIRI_NAMESPACES)
        active_idx = 0;

    self->ns_opacity[active_idx] = (int)gtk_range_get_value(GTK_RANGE(self->scale_op));
    self->ns_xray[active_idx]    = gtk_switch_get_active(GTK_SWITCH(self->sw_xray));
    self->gap_px                 = (int)gtk_range_get_value(GTK_RANGE(self->scale_gap));

    self->open_preset_idx   = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_shader_open));
    self->open_duration_ms  = (int)gtk_range_get_value(GTK_RANGE(self->scale_open_dur));
    self->open_curve_idx    = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_open_curve));

    self->close_preset_idx  = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_shader_close));
    self->close_duration_ms = (int)gtk_range_get_value(GTK_RANGE(self->scale_close_dur));
    self->close_curve_idx   = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_close_curve));

    self->resize_preset_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_shader_resize));

    if (self->tc_glass)
        self->fx_cfg.glass_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_glass));
    if (self->tc_shadow)
        self->fx_cfg.shadow_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_shadow));
    if (self->tc_dim)
        self->fx_cfg.dim_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_dim));
    if (self->tc_dither)
        self->fx_cfg.dither_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_dither));
    if (self->combo_color_filter) {
        guint cf_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_color_filter));
        if (cf_idx >= N_COLOR_FILTERS)
            cf_idx = 0;
        self->color_filter_idx = cf_idx;
        g_strlcpy(self->fx_cfg.color_filter, COLOR_FILTER_IDS[cf_idx], sizeof(self->fx_cfg.color_filter));
    }
    if (self->scale_filter_strength)
        self->fx_cfg.color_filter_strength_pct = (int)gtk_range_get_value(GTK_RANGE(self->scale_filter_strength));
    if (self->scale_night_temp)
        self->fx_cfg.night_light_temperature = (int)gtk_range_get_value(GTK_RANGE(self->scale_night_temp));

    const gchar *xdg_config = g_get_user_config_dir();
    g_autofree gchar *dir = g_build_filename(xdg_config, "niri", "config.d", NULL);
    g_mkdir_with_parents(dir, 0755);

    /* 1. Write 40-layer-rules.kdl */
    g_autofree gchar *layer_path = g_build_filename(dir, "40-layer-rules.kdl", NULL);
    GString *layer_kdl = g_string_new(
        "// Layer Surface Rules (Managed by kajo-settings)\n");

    for (int i = 0; i < N_NIRI_NAMESPACES; i++) {
        char op_buf[32];
        g_ascii_formatd(op_buf, sizeof(op_buf), "%.2f", self->ns_opacity[i] / 100.0);
        g_string_append_printf(
            layer_kdl,
            "\nlayer-rule {\n"
            "    match namespace=\"%s\"\n"
            "    opacity %s\n\n"
            "    background-effect {\n"
            "        blur true\n"
            "        xray %s\n"
            "    }\n\n"
            "    popups {\n"
            "        opacity %s\n"
            "        background-effect {\n"
            "            blur true\n"
            "        }\n"
            "    }\n"
            "}\n",
            NIRI_NS_IDS[i],
            op_buf,
            self->ns_xray[i] ? "true" : "false",
            op_buf);
    }

    g_file_set_contents(layer_path, layer_kdl->str, layer_kdl->len, NULL);
    g_string_free(layer_kdl, TRUE);

    /* 2. Update 30-layout-animations.kdl */
    g_autofree gchar *anim_block_str = build_animations_kdl_block(self);
    save_animations_to_30_kdl(dir, anim_block_str);

    /* 3. Write 00-kajo.kdl */
    g_autofree gchar *kajo_path = g_build_filename(dir, "00-kajo.kdl", NULL);
    char active_op_buf[32];
    g_ascii_formatd(active_op_buf, sizeof(active_op_buf), "%.2f", self->ns_opacity[active_idx] / 100.0);

    g_autoptr(KajoThemeCfgRef) cfg = kajo_config_store_acquire_theme(NULL);
    const char *accent_hex = (cfg && cfg->accent_color && cfg->accent_color[0] != '\0')
        ? cfg->accent_color
        : "#0078d4";

    g_autofree gchar *kdl_content = g_strdup_printf(
        "// Managed by kajo-settings — DO NOT EDIT DIRECTLY\n\n"
        "layer-rule {\n"
        "    match namespace=\"%s\"\n"
        "    opacity %s\n"
        "    background-effect {\n"
        "        blur true\n"
        "        xray %s\n"
        "    }\n"
        "}\n\n"
        "layout {\n"
        "    gaps %d\n"
        "    focus-ring {\n"
        "        width 2\n"
        "        active-color \"%s\"\n"
        "    }\n"
        "}\n\n"
        "%s\n",
        NIRI_NS_IDS[active_idx],
        active_op_buf,
        self->ns_xray[active_idx] ? "true" : "false",
        self->gap_px,
        accent_hex,
        anim_block_str);

    g_file_set_contents(kajo_path, kdl_content, -1, NULL);

    /* 4. Patch kajo-comp comp.toml ([layout], [effects], [effects.transitions]) and rules.toml */
    guint op_i = self->open_preset_idx < N_OPEN_PRESETS ? self->open_preset_idx : 0;
    guint oc_i = self->open_curve_idx < N_CURVES ? self->open_curve_idx : 0;
    guint cp_i = self->close_preset_idx < N_CLOSE_PRESETS ? self->close_preset_idx : 0;
    guint cc_i = self->close_curve_idx < N_CURVES ? self->close_curve_idx : 2;
    guint rp_i = self->resize_preset_idx < N_RESIZE_PRESETS ? self->resize_preset_idx : 0;

    g_strlcpy(self->fx_cfg.open_preset, OPEN_PRESETS[op_i].id, sizeof(self->fx_cfg.open_preset));
    self->fx_cfg.open_duration_ms = self->open_duration_ms;
    g_strlcpy(self->fx_cfg.open_curve, CURVE_NAMES[oc_i], sizeof(self->fx_cfg.open_curve));
    g_strlcpy(self->fx_cfg.close_preset, CLOSE_PRESETS[cp_i].id, sizeof(self->fx_cfg.close_preset));
    self->fx_cfg.close_duration_ms = self->close_duration_ms;
    g_strlcpy(self->fx_cfg.close_curve, CURVE_NAMES[cc_i], sizeof(self->fx_cfg.close_curve));
    g_strlcpy(self->fx_cfg.resize_preset, RESIZE_PRESETS[rp_i].id, sizeof(self->fx_cfg.resize_preset));

    kajo_config_comp_save_layout_and_layers(
        NULL, self->gap_px, NIRI_NS_IDS, self->ns_opacity, N_NIRI_NAMESPACES, NULL);
    kajo_config_comp_save_effects(
        NULL, &self->fx_cfg, NIRI_NS_IDS, self->ns_xray, N_NIRI_NAMESPACES, NULL);

    shell_compositor_reload_config();

    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

/* ─── UI Signal Handlers ─── */

static void
on_combo_ns_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx >= N_NIRI_NAMESPACES)
        idx = 0;

    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_op), self->ns_opacity[idx]);
    gtk_switch_set_active(GTK_SWITCH(self->sw_xray), self->ns_xray[idx]);
    self->syncing_ui = FALSE;

    update_niri_subtitles_and_telemetry(self);
}

static void
on_scale_op_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
    if (idx < N_NIRI_NAMESPACES)
        self->ns_opacity[idx] = (int)gtk_range_get_value(range);

    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_sw_xray_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ns));
    if (idx < N_NIRI_NAMESPACES)
        self->ns_xray[idx] = gtk_switch_get_active(GTK_SWITCH(self->sw_xray));

    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_gap_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    self->gap_px = (int)gtk_range_get_value(range);
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_combo_shader_open_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx >= N_OPEN_PRESETS)
        idx = 0;
    self->open_preset_idx  = idx;
    self->open_duration_ms = OPEN_PRESETS[idx].rec_duration_ms;
    self->open_curve_idx   = OPEN_PRESETS[idx].rec_curve_idx;

    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_open_dur), self->open_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_open_curve), self->open_curve_idx);
    self->syncing_ui = FALSE;

    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_open_dur_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    self->open_duration_ms = (int)gtk_range_get_value(range);
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_combo_open_curve_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx < N_CURVES)
        self->open_curve_idx = idx;
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_combo_shader_close_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx >= N_CLOSE_PRESETS)
        idx = 0;
    self->close_preset_idx  = idx;
    self->close_duration_ms = CLOSE_PRESETS[idx].rec_duration_ms;
    self->close_curve_idx   = CLOSE_PRESETS[idx].rec_curve_idx;

    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_close_dur), self->close_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_close_curve), self->close_curve_idx);
    self->syncing_ui = FALSE;

    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_close_dur_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    self->close_duration_ms = (int)gtk_range_get_value(range);
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_combo_close_curve_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx < N_CURVES)
        self->close_curve_idx = idx;
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_combo_shader_resize_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx < N_RESIZE_PRESETS)
        self->resize_preset_idx = idx;
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_fx_toggle_changed(GtkToggleButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.glass_enabled  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_glass));
    self->fx_cfg.shadow_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_shadow));
    self->fx_cfg.dim_enabled    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_dim));
    self->fx_cfg.dither_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tc_dither));
    update_fx_row_sensitivity(self);
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_border_px_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.border_px = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_seg_glass_blur_changed(GtkWidget *group G_GNUC_UNUSED, guint selected, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui || selected >= N_GLASS_BLUR_PRESETS)
        return;
    self->fx_cfg.glass_passes = GLASS_BLUR_PRESETS[selected].passes;
    self->fx_cfg.glass_offset = GLASS_BLUR_PRESETS[selected].offset;
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_bright_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.glass_brightness_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_sat_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.glass_saturation_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_entry_glass_tint_changed(GtkEditable *editable, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    const gchar *txt = gtk_editable_get_text(editable);
    if (!kajo_config_is_valid_hex_color(txt))
        return;
    g_strlcpy(self->fx_cfg.glass_tint, txt, sizeof(self->fx_cfg.glass_tint));
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_tint_alpha_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.glass_tint_alpha_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_combo_glass_style_changed(GtkDropDown *dd, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui)
        return;
    guint idx = gtk_drop_down_get_selected(dd);
    if (idx >= N_GLASS_STYLES)
        idx = 0;
    g_strlcpy(self->fx_cfg.glass_style, GLASS_STYLE_IDS[idx], sizeof(self->fx_cfg.glass_style));
    update_fx_row_sensitivity(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_refraction_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui)
        return;
    self->fx_cfg.glass_refraction_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_dispersion_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui)
        return;
    self->fx_cfg.glass_dispersion_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_bevel_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui)
        return;
    self->fx_cfg.glass_bevel_px = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_specular_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui)
        return;
    self->fx_cfg.glass_specular_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_grain_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui)
        return;
    self->fx_cfg.glass_grain_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_glass_flute_px_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui)
        return;
    self->fx_cfg.glass_flute_px = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_seg_glass_flute_axis_changed(GtkWidget *group G_GNUC_UNUSED, guint selected, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self || self->syncing_ui || selected > 1)
        return;
    g_strlcpy(self->fx_cfg.glass_flute_axis, FLUTE_AXIS_IDS[selected], sizeof(self->fx_cfg.glass_flute_axis));
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_seg_shadow_strength_changed(GtkWidget *group G_GNUC_UNUSED, guint selected, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui || selected >= N_SHADOW_STRENGTH_PRESETS)
        return;
    self->fx_cfg.shadow_strength_pct = SHADOW_STRENGTH_PCTS[selected];
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_shadow_unfocused_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.shadow_unfocused_scale_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_dim_opacity_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.dim_opacity_pct = (int)gtk_range_get_value(range);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_combo_color_filter_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx >= N_COLOR_FILTERS)
        idx = 0;
    self->color_filter_idx = idx;
    g_strlcpy(self->fx_cfg.color_filter, COLOR_FILTER_IDS[idx], sizeof(self->fx_cfg.color_filter));
    if (self->row_filter_strength)
        gtk_widget_set_sensitive(self->row_filter_strength, idx != 0);
    if (self->row_night_temp)
        gtk_widget_set_sensitive(self->row_night_temp, idx == 2);
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_filter_strength_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.color_filter_strength_pct = (int)gtk_range_get_value(range);
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_night_temp_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.night_light_temperature = (int)gtk_range_get_value(range);
    update_niri_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

/* ─── Live Transition Preview Window (Open -> Resize -> Close) ─── */

typedef struct {
    GtkWindow *win;
    guint      resize_timer_id;
    guint      close_timer_id;
} PreviewWindowCtx;

static void
on_preview_window_destroy(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    PreviewWindowCtx *ctx = (PreviewWindowCtx *)user_data;
    if (!ctx)
        return;
    if (ctx->resize_timer_id > 0)
        g_source_remove(ctx->resize_timer_id);
    if (ctx->close_timer_id > 0)
        g_source_remove(ctx->close_timer_id);
    g_free(ctx);
}

static gboolean
on_preview_resize_timeout(gpointer user_data)
{
    PreviewWindowCtx *ctx = (PreviewWindowCtx *)user_data;
    ctx->resize_timer_id = 0;
    if (ctx->win && GTK_IS_WINDOW(ctx->win)) {
        gtk_window_set_default_size(ctx->win, 580, 340);
        gtk_widget_set_size_request(GTK_WIDGET(ctx->win), 580, 340);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
on_preview_close_timeout(gpointer user_data)
{
    PreviewWindowCtx *ctx = (PreviewWindowCtx *)user_data;
    ctx->close_timer_id = 0;
    if (ctx->win && GTK_IS_WINDOW(ctx->win)) {
        gtk_window_close(ctx->win);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
spawn_shader_preview_window(gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    self->preview_spawn_timer_id = 0;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "KAJO SHADER PREVIEW");
    gtk_window_set_default_size(GTK_WINDOW(win), 420, 240);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_widget_add_css_class(win, "shell-settings-window");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(vbox, 24);
    gtk_widget_set_margin_bottom(vbox, 24);
    gtk_widget_set_margin_start(vbox, 24);
    gtk_widget_set_margin_end(vbox, 24);

    GtkWidget *title = gtk_label_new("✦ WINDOW TRANSITION TEST");
    gtk_widget_add_css_class(title, "settings-header-title");

    guint open_idx   = self->open_preset_idx < N_OPEN_PRESETS ? self->open_preset_idx : 0;
    guint resize_idx = self->resize_preset_idx < N_RESIZE_PRESETS ? self->resize_preset_idx : 0;
    guint close_idx  = self->close_preset_idx < N_CLOSE_PRESETS ? self->close_preset_idx : 0;

    g_autofree gchar *info_text = g_strdup_printf(
        "Open: %s (%dms)\nResize: %s\nClose: %s (%dms)",
        OPEN_PRESETS[open_idx].label, self->open_duration_ms,
        RESIZE_PRESETS[resize_idx].label,
        CLOSE_PRESETS[close_idx].label, self->close_duration_ms);
    GtkWidget *sub = gtk_label_new(info_text);
    gtk_label_set_justify(GTK_LABEL(sub), GTK_JUSTIFY_CENTER);

    gtk_box_append(GTK_BOX(vbox), title);
    gtk_box_append(GTK_BOX(vbox), sub);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    PreviewWindowCtx *ctx = g_new0(PreviewWindowCtx, 1);
    ctx->win = GTK_WINDOW(win);
    guint resize_delay_ms = (guint)MAX(self->open_duration_ms + 350, 750);
    guint close_delay_ms  = resize_delay_ms + 850;
    ctx->resize_timer_id = g_timeout_add(resize_delay_ms, on_preview_resize_timeout, ctx);
    ctx->close_timer_id  = g_timeout_add(close_delay_ms, on_preview_close_timeout, ctx);
    g_signal_connect(win, "destroy", G_CALLBACK(on_preview_window_destroy), ctx);

    gtk_window_present(GTK_WINDOW(win));
    return G_SOURCE_REMOVE;
}

static void
action_test_niri_transition(gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self)
        return;

    save_and_reload_niri_config(self);

    g_clear_handle_id(&self->preview_spawn_timer_id, g_source_remove);
    self->preview_spawn_timer_id = g_timeout_add(120, spawn_shader_preview_window, self);
}

static void
action_revert_niri(gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self)
        return;
    load_existing_niri_config(self);
    sync_niri_widgets_from_state(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
action_save_niri(gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (!self)
        return;
    save_and_reload_niri_config(self);
}

static void
on_niri_page_mapped(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    update_niri_subtitles_and_telemetry((SettingsPageNiri *)widget);
}

static void
settings_page_niri_dispose(GObject *object)
{
    SettingsPageNiri *self = (SettingsPageNiri *)object;
    self->syncing_ui = TRUE;
    g_clear_handle_id(&self->preview_spawn_timer_id, g_source_remove);
    G_OBJECT_CLASS(settings_page_niri_parent_class)->dispose(object);
}

static void
settings_page_niri_init(SettingsPageNiri *self)
{
    reset_niri_defaults_in_memory(self);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_niri_class_init(SettingsPageNiriClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_niri_dispose;
}

GtkWidget *
build_page_niri(void)
{
    SettingsPageNiri *self = g_object_new(settings_page_niri_get_type(), NULL);
    load_existing_niri_config(self);

    /* ─── Card 1: Layer Surface Rules & Layout ─── */
    GtkWidget *card_layer = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. LAYER SURFACE RULES & LAYOUT",
        "rules.toml",
        "accent",
        NULL);

    self->combo_ns = create_string_dropdown(NIRI_NS_LABELS);
    gtk_box_append(GTK_BOX(card_layer), create_boxed_row(
        "🗔",
        "Target Layer Namespace",
        "Wayland layer-shell surface namespace",
        self->combo_ns));

    GtkWidget *w_op = create_scale_with_badge(20, 100, 1, 85, "%.0f%%", &self->scale_op, NULL);
    gtk_box_append(GTK_BOX(card_layer), create_boxed_row(
        "◐",
        "Surface Layer Opacity",
        "Compositor opacity rule for selected namespace",
        w_op));

    GtkWidget *w_gap = create_scale_with_badge(0, 32, 1, 12, "%.0fpx", &self->scale_gap, NULL);
    gtk_box_append(GTK_BOX(card_layer), create_boxed_row(
        "⊞",
        "Workspace Window Gaps",
        "Compositor [layout] gap_px in pixels",
        w_gap));

    GtkWidget *w_bpx = create_scale_with_badge(0, 4, 1, 1, "%.0fpx", &self->scale_border_px, NULL);
    gtk_box_append(GTK_BOX(card_layer), create_boxed_row(
        "▢",
        "Window Border Width",
        "Compositor [layout] border_px (0–4px, default 1px)",
        w_bpx));

    self->sw_xray = gtk_switch_new();
    gtk_box_append(GTK_BOX(card_layer), create_boxed_row(
        "✨",
        "Blur XRay (Wallpaper Through Blur)",
        "Sample desktop backdrop behind translucent surfaces",
        self->sw_xray));

    /* ─── Card 2: Compositor Effects & Colour Stage (comp.toml [effects]) ─── */
    GtkWidget *card_fx = create_section_card(
        SETTINGS_CARD_RIGHT,
        "2. COMPOSITOR EFFECTS & COLOUR STAGE",
        "comp.toml [effects]",
        "accent",
        NULL);

    GtkWidget *fx_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(fx_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(fx_grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(fx_grid), TRUE);

    self->tc_glass = create_toggle_card(
        "weather-fog-symbolic",
        "Backdrop Glass Blur",
        "Dual-Kawase frosted blur",
        self->fx_cfg.glass_enabled);
    gtk_grid_attach(GTK_GRID(fx_grid), self->tc_glass, 0, 0, 1, 1);

    self->tc_shadow = create_toggle_card(
        "view-restore-symbolic",
        "Analytic Soft Shadows",
        "2-layer Gaussian drop shadow",
        self->fx_cfg.shadow_enabled);
    gtk_grid_attach(GTK_GRID(fx_grid), self->tc_shadow, 1, 0, 1, 1);

    self->tc_dim = create_toggle_card(
        "display-brightness-symbolic",
        "Unfocused Window Dim",
        "Dimming on inactive windows",
        self->fx_cfg.dim_enabled);
    gtk_grid_attach(GTK_GRID(fx_grid), self->tc_dim, 0, 1, 1, 1);

    self->tc_dither = create_toggle_card(
        "preferences-color-symbolic",
        "Blue-Noise Dither",
        "16×16 ±0.5 LSB output dither",
        self->fx_cfg.dither_enabled);
    gtk_grid_attach(GTK_GRID(fx_grid), self->tc_dither, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(card_fx), fx_grid);

    self->combo_glass_style = create_string_dropdown(GLASS_STYLE_LABELS);
    gtk_widget_set_size_request(self->combo_glass_style, 230, -1);
    self->row_glass_style = create_boxed_row(
        NULL,
        "Glass Material Style",
        "[effects.glass] optical shader material (Frosted / Liquid / Halation / Fluted / Acrylic / Aero / Dichroic)",
        self->combo_glass_style);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_style);

    self->seg_glass_blur = create_segmented_group(GLASS_BLUR_LABELS, 1);
    self->row_glass_blur = create_boxed_row(
        NULL,
        "Glass Blur Strength",
        "Dual-Kawase pass depth & sample offset",
        self->seg_glass_blur);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_blur);

    GtkWidget *w_gb = create_scale_with_badge(50, 150, 5, 90, "%.0f%%", &self->scale_glass_bright, NULL);
    self->row_glass_bright = create_boxed_row(
        NULL,
        "Glass Brightness",
        "[effects.glass] brightness multiplier (50–150%)",
        w_gb);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_bright);

    GtkWidget *w_gs = create_scale_with_badge(50, 200, 5, 115, "%.0f%%", &self->scale_glass_sat, NULL);
    self->row_glass_sat = create_boxed_row(
        NULL,
        "Glass Saturation",
        "[effects.glass] saturation multiplier (50–200%)",
        w_gs);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_sat);

    GtkWidget *gt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(gt_box, GTK_ALIGN_CENTER);
    self->entry_glass_tint = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(self->entry_glass_tint), 7);
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_glass_tint), 8);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_glass_tint), 9);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_glass_tint), "#141414");
    gtk_widget_set_valign(self->entry_glass_tint, GTK_ALIGN_CENTER);
    GtkWidget *w_gta = create_scale_with_badge(0, 100, 5, 35, "%.0f%%", &self->scale_glass_tint_alpha, NULL);
    gtk_box_append(GTK_BOX(gt_box), self->entry_glass_tint);
    gtk_box_append(GTK_BOX(gt_box), w_gta);
    self->row_glass_tint = create_boxed_row(
        NULL,
        "Glass Tint & Strength",
        "[effects.glass] tint hex & tint_alpha blend",
        gt_box);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_tint);

    GtkWidget *w_gref = create_scale_with_badge(0, 200, 5, 45, "%.0f%%", &self->scale_glass_refraction, NULL);
    self->row_glass_refraction = create_boxed_row(
        NULL,
        "Optical Refraction",
        "[effects.glass] Snell UV bending strength (Liquid / Fluted)",
        w_gref);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_refraction);

    GtkWidget *w_gdisp = create_scale_with_badge(0, 100, 5, 25, "%.0f%%", &self->scale_glass_dispersion, NULL);
    self->row_glass_dispersion = create_boxed_row(
        NULL,
        "Chromatic Dispersion / Iridescence",
        "[effects.glass] RGB prism split & thin-film shift (0–100%)",
        w_gdisp);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_dispersion);

    GtkWidget *w_gbev = create_scale_with_badge(2, 64, 2, 14, "%.0f px", &self->scale_glass_bevel, NULL);
    self->row_glass_bevel = create_boxed_row(
        NULL,
        "Bevel Width",
        "[effects.glass] chamfered 0px-corner optical rim width (2–64 px)",
        w_gbev);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_bevel);

    GtkWidget *w_gspec = create_scale_with_badge(0, 200, 5, 55, "%.0f%%", &self->scale_glass_specular, NULL);
    self->row_glass_specular = create_boxed_row(
        NULL,
        "Specular / Sheen Intensity",
        "[effects.glass] linear Fresnel rim, ridge & sheen brightness (0–200%)",
        w_gspec);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_specular);

    GtkWidget *w_ggr = create_scale_with_badge(0, 100, 5, 35, "%.0f%%", &self->scale_glass_grain, NULL);
    self->row_glass_grain = create_boxed_row(
        NULL,
        "Scatter / Etch Roughness",
        "[effects.glass] Halation wide-glow scatter or Acrylic micro-facet grain (0–100%)",
        w_ggr);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_grain);

    GtkWidget *fl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(fl_box, GTK_ALIGN_CENTER);
    self->seg_glass_flute_axis = create_segmented_group(FLUTE_AXIS_LABELS, 0);
    GtkWidget *w_gfp = create_scale_with_badge(4, 64, 2, 14, "%.0f px", &self->scale_glass_flute_px, NULL);
    gtk_box_append(GTK_BOX(fl_box), self->seg_glass_flute_axis);
    gtk_box_append(GTK_BOX(fl_box), w_gfp);
    self->row_glass_flute = create_boxed_row(
        NULL,
        "Fluted Rib Axis & Pitch",
        "[effects.glass] cylindrical lenslet orientation & spacing (4–64 px)",
        fl_box);
    gtk_box_append(GTK_BOX(card_fx), self->row_glass_flute);

    self->seg_shadow_strength = create_segmented_group(SHADOW_STRENGTH_LABELS, 1);
    self->row_shadow_strength = create_boxed_row(
        NULL,
        "Shadow Strength",
        "[effects.shadow] strength preset (Subtle / Standard / Deep)",
        self->seg_shadow_strength);
    gtk_box_append(GTK_BOX(card_fx), self->row_shadow_strength);

    GtkWidget *w_sh_un = create_scale_with_badge(0, 100, 5, 60, "%.0f%%", &self->scale_shadow_unfocused, NULL);
    self->row_shadow_unfocused = create_boxed_row(
        NULL,
        "Unfocused Shadow Scale",
        "[effects.shadow] unfocused_scale for inactive windows",
        w_sh_un);
    gtk_box_append(GTK_BOX(card_fx), self->row_shadow_unfocused);

    GtkWidget *w_dim_op = create_scale_with_badge(0, 60, 5, 15, "%.0f%%", &self->scale_dim_opacity, NULL);
    self->row_dim_opacity = create_boxed_row(
        NULL,
        "Unfocused Dim Opacity",
        "[effects.dim] scrim opacity on inactive windows (0–60%)",
        w_dim_op);
    gtk_box_append(GTK_BOX(card_fx), self->row_dim_opacity);

    self->combo_color_filter = create_string_dropdown(COLOR_FILTER_LABELS);
    gtk_box_append(GTK_BOX(card_fx), create_boxed_row(
        "◐",
        "Output Colour Filter",
        "Linear-light 3×3 matrix filter in output stage",
        self->combo_color_filter));

    GtkWidget *w_cf_str = create_scale_with_badge(
        10, 100, 5, 100, "%.0f%%", &self->scale_filter_strength, NULL);
    self->row_filter_strength = create_boxed_row(
        NULL,
        "Colour Filter Strength",
        "Blend ratio between unfiltered identity and active colour matrix",
        w_cf_str);
    gtk_box_append(GTK_BOX(card_fx), self->row_filter_strength);

    GtkWidget *w_nl_temp = create_scale_with_badge(
        1800, 6500, 100, 4000, "%.0fK", &self->scale_night_temp, NULL);
    self->row_night_temp = create_boxed_row(
        "☀",
        "Night Light Temperature",
        "Blackbody white-point temperature in Kelvin (1800K–6500K)",
        w_nl_temp);
    gtk_box_append(GTK_BOX(card_fx), self->row_night_temp);

    settings_bento_add_pair(GTK_WIDGET(self), card_layer, card_fx);

    /* ─── Card 3: Window Transition Shaders ─── */
    GtkWidget *card_shaders = create_section_card(
        SETTINGS_CARD_CENTER,
        "3. WINDOW TRANSITION SHADERS",
        "comp.toml + KDL",
        "peak-warn",
        NULL);

    const char *open_labels[N_OPEN_PRESETS + 1];
    for (guint i = 0; i < N_OPEN_PRESETS; i++)
        open_labels[i] = OPEN_PRESETS[i].label;
    open_labels[N_OPEN_PRESETS] = NULL;

    self->combo_shader_open = create_string_dropdown(open_labels);
    self->row_open_shader = create_boxed_row(
        "✦",
        "Window Open Shader Preset",
        "150ms • ease-out-expo",
        self->combo_shader_open);
    gtk_box_append(GTK_BOX(card_shaders), self->row_open_shader);

    GtkWidget *w_open_dur = create_scale_with_badge(
        50, 800, 10, 150, "%.0fms", &self->scale_open_dur, NULL);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        NULL,
        "Open Duration",
        "Animation duration in milliseconds",
        w_open_dur));

    self->combo_open_curve = create_string_dropdown(CURVE_NAMES);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        NULL,
        "Open Easing Curve",
        "Transition interpolation curve",
        self->combo_open_curve));

    const char *close_labels[N_CLOSE_PRESETS + 1];
    for (guint i = 0; i < N_CLOSE_PRESETS; i++)
        close_labels[i] = CLOSE_PRESETS[i].label;
    close_labels[N_CLOSE_PRESETS] = NULL;

    self->combo_shader_close = create_string_dropdown(close_labels);
    self->row_close_shader = create_boxed_row(
        "✦",
        "Window Close Shader Preset",
        "200ms • ease-out-quad",
        self->combo_shader_close);
    gtk_box_append(GTK_BOX(card_shaders), self->row_close_shader);

    GtkWidget *w_close_dur = create_scale_with_badge(
        50, 800, 10, 200, "%.0fms", &self->scale_close_dur, NULL);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        NULL,
        "Close Duration",
        "Animation duration in milliseconds",
        w_close_dur));

    self->combo_close_curve = create_string_dropdown(CURVE_NAMES);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        NULL,
        "Close Easing Curve",
        "Transition interpolation curve",
        self->combo_close_curve));

    const char *resize_labels[N_RESIZE_PRESETS + 1];
    for (guint i = 0; i < N_RESIZE_PRESETS; i++)
        resize_labels[i] = RESIZE_PRESETS[i].label;
    resize_labels[N_RESIZE_PRESETS] = NULL;

    self->combo_shader_resize = create_string_dropdown(resize_labels);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        "↔",
        "Window Resize Shader Preset",
        "Column width transition morph",
        self->combo_shader_resize));

    settings_bento_add_full(GTK_WIDGET(self), card_shaders);

    sync_niri_widgets_from_state(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);

    g_signal_connect(self->combo_ns, "notify::selected", G_CALLBACK(on_combo_ns_changed), self);
    g_signal_connect(self->scale_op, "value-changed", G_CALLBACK(on_scale_op_changed), self);
    g_signal_connect(self->scale_gap, "value-changed", G_CALLBACK(on_scale_gap_changed), self);
    g_signal_connect(self->scale_border_px, "value-changed", G_CALLBACK(on_scale_border_px_changed), self);
    g_signal_connect(self->sw_xray, "notify::active", G_CALLBACK(on_sw_xray_changed), self);

    g_signal_connect(self->combo_shader_open, "notify::selected", G_CALLBACK(on_combo_shader_open_changed), self);
    g_signal_connect(self->scale_open_dur, "value-changed", G_CALLBACK(on_scale_open_dur_changed), self);
    g_signal_connect(self->combo_open_curve, "notify::selected", G_CALLBACK(on_combo_open_curve_changed), self);

    g_signal_connect(self->combo_shader_close, "notify::selected", G_CALLBACK(on_combo_shader_close_changed), self);
    g_signal_connect(self->scale_close_dur, "value-changed", G_CALLBACK(on_scale_close_dur_changed), self);
    g_signal_connect(self->combo_close_curve, "notify::selected", G_CALLBACK(on_combo_close_curve_changed), self);

    g_signal_connect(self->combo_shader_resize, "notify::selected", G_CALLBACK(on_combo_shader_resize_changed), self);

    g_signal_connect(self->tc_glass, "toggled", G_CALLBACK(on_fx_toggle_changed), self);
    g_signal_connect(self->tc_shadow, "toggled", G_CALLBACK(on_fx_toggle_changed), self);
    g_signal_connect(self->tc_dim, "toggled", G_CALLBACK(on_fx_toggle_changed), self);
    g_signal_connect(self->tc_dither, "toggled", G_CALLBACK(on_fx_toggle_changed), self);
    g_signal_connect(self->combo_glass_style, "notify::selected", G_CALLBACK(on_combo_glass_style_changed), self);
    segmented_group_connect(self->seg_glass_blur, on_seg_glass_blur_changed, self);
    g_signal_connect(self->scale_glass_bright, "value-changed", G_CALLBACK(on_scale_glass_bright_changed), self);
    g_signal_connect(self->scale_glass_sat, "value-changed", G_CALLBACK(on_scale_glass_sat_changed), self);
    g_signal_connect(self->entry_glass_tint, "changed", G_CALLBACK(on_entry_glass_tint_changed), self);
    g_signal_connect(self->scale_glass_tint_alpha, "value-changed", G_CALLBACK(on_scale_glass_tint_alpha_changed), self);
    g_signal_connect(self->scale_glass_refraction, "value-changed", G_CALLBACK(on_scale_glass_refraction_changed), self);
    g_signal_connect(self->scale_glass_dispersion, "value-changed", G_CALLBACK(on_scale_glass_dispersion_changed), self);
    g_signal_connect(self->scale_glass_bevel, "value-changed", G_CALLBACK(on_scale_glass_bevel_changed), self);
    g_signal_connect(self->scale_glass_specular, "value-changed", G_CALLBACK(on_scale_glass_specular_changed), self);
    g_signal_connect(self->scale_glass_grain, "value-changed", G_CALLBACK(on_scale_glass_grain_changed), self);
    g_signal_connect(self->scale_glass_flute_px, "value-changed", G_CALLBACK(on_scale_glass_flute_px_changed), self);
    segmented_group_connect(self->seg_glass_flute_axis, on_seg_glass_flute_axis_changed, self);
    segmented_group_connect(self->seg_shadow_strength, on_seg_shadow_strength_changed, self);
    g_signal_connect(self->scale_shadow_unfocused, "value-changed", G_CALLBACK(on_scale_shadow_unfocused_changed), self);
    g_signal_connect(self->scale_dim_opacity, "value-changed", G_CALLBACK(on_scale_dim_opacity_changed), self);
    g_signal_connect(self->combo_color_filter, "notify::selected", G_CALLBACK(on_combo_color_filter_changed), self);
    g_signal_connect(self->scale_filter_strength, "value-changed", G_CALLBACK(on_scale_filter_strength_changed), self);
    g_signal_connect(self->scale_night_temp, "value-changed", G_CALLBACK(on_scale_night_temp_changed), self);
    g_signal_connect(self, "map", G_CALLBACK(on_niri_page_mapped), NULL);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "▶ TEST TRANSITION",
        action_test_niri_transition,
        action_revert_niri,
        action_save_niri,
        self);

    return GTK_WIDGET(self);
}
