#include "../settings_common.h"
#include "../settings_pages.h"
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

typedef struct _SettingsPageNiri {
    GtkBox parent_instance;

    GtkWidget *combo_ns;
    GtkWidget *scale_op;
    GtkWidget *scale_gap;
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

    int      ns_opacity[N_NIRI_NAMESPACES];
    gboolean ns_xray[N_NIRI_NAMESPACES];
    int      gap_px;

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

static void
reset_niri_defaults_in_memory(SettingsPageNiri *self)
{
    for (int i = 0; i < N_NIRI_NAMESPACES; i++) {
        self->ns_opacity[i] = 85;
        self->ns_xray[i]    = FALSE;
    }
    self->gap_px            = 12;
    self->open_preset_idx   = 0;
    self->open_duration_ms  = 150;
    self->open_curve_idx    = 0;
    self->close_preset_idx  = 0;
    self->close_duration_ms = 200;
    self->close_curve_idx   = 2;
    self->resize_preset_idx = 0;
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

    g_autofree gchar *c0 = g_strdup_printf("%s (%d%%)", NIRI_NS_IDS[ns_idx], self->ns_opacity[ns_idx]);
    g_autofree gchar *c1 = g_strdup_printf("%dPX", self->gap_px);
    g_autofree gchar *c2 = g_ascii_strup(OPEN_PRESETS[open_idx].id, -1);
    g_autofree gchar *c3 = g_ascii_strup(CLOSE_PRESETS[close_idx].id, -1);

    settings_page_set_counter(GTK_WIDGET(self), 0, "NAMESPACE", c0, "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "GAPS", c1, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "OPEN", c2, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "CLOSE", c3, NULL);

    gboolean has_glsl = (open_idx > 0 || close_idx > 0 || resize_idx > 0);
    settings_page_set_nav_badge(
        GTK_WIDGET(self), "niri", has_glsl ? "GLSL" : "KDL", has_glsl ? "accent" : NULL);
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

    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_shader_open), self->open_preset_idx);
    gtk_range_set_value(GTK_RANGE(self->scale_open_dur), self->open_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_open_curve), self->open_curve_idx);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_shader_close), self->close_preset_idx);
    gtk_range_set_value(GTK_RANGE(self->scale_close_dur), self->close_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_close_curve), self->close_curve_idx);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_shader_resize), self->resize_preset_idx);
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

    ShellConfig *cfg = shell_config_load(NULL);
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

    if (cfg)
        shell_config_destroy(cfg);

    g_file_set_contents(kajo_path, kdl_content, -1, NULL);
    niri_reload_config();

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

    GtkWidget *title = gtk_label_new("✦ NIRI SHADER TRANSITION TEST");
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

    /* ─── Card 1: Layer Surface Rules & Workspace Gaps ─── */
    GtkWidget *card_layer = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. LAYER SURFACE RULES & WORKSPACE GAPS",
        "40-layer-rules.kdl",
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
        "Niri layout { gaps N } in pixels",
        w_gap));

    self->sw_xray = gtk_switch_new();
    gtk_box_append(GTK_BOX(card_layer), create_boxed_row(
        "✨",
        "Blur XRay (Wallpaper Through Blur)",
        "Sample desktop backdrop behind translucent surfaces",
        self->sw_xray));

    /* ─── Card 2: Window Transition Shaders (GLSL) ─── */
    GtkWidget *card_shaders = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. WINDOW TRANSITION SHADERS (GLSL)",
        "30-layout-animations.kdl",
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
        "Niri interpolation curve",
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
        "Niri interpolation curve",
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

    settings_bento_add_pair(GTK_WIDGET(self), card_layer, card_shaders);

    sync_niri_widgets_from_state(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);

    g_signal_connect(self->combo_ns, "notify::selected", G_CALLBACK(on_combo_ns_changed), self);
    g_signal_connect(self->scale_op, "value-changed", G_CALLBACK(on_scale_op_changed), self);
    g_signal_connect(self->scale_gap, "value-changed", G_CALLBACK(on_scale_gap_changed), self);
    g_signal_connect(self->sw_xray, "notify::active", G_CALLBACK(on_sw_xray_changed), self);

    g_signal_connect(self->combo_shader_open, "notify::selected", G_CALLBACK(on_combo_shader_open_changed), self);
    g_signal_connect(self->scale_open_dur, "value-changed", G_CALLBACK(on_scale_open_dur_changed), self);
    g_signal_connect(self->combo_open_curve, "notify::selected", G_CALLBACK(on_combo_open_curve_changed), self);

    g_signal_connect(self->combo_shader_close, "notify::selected", G_CALLBACK(on_combo_shader_close_changed), self);
    g_signal_connect(self->scale_close_dur, "value-changed", G_CALLBACK(on_scale_close_dur_changed), self);
    g_signal_connect(self->combo_close_curve, "notify::selected", G_CALLBACK(on_combo_close_curve_changed), self);

    g_signal_connect(self->combo_shader_resize, "notify::selected", G_CALLBACK(on_combo_shader_resize_changed), self);
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
