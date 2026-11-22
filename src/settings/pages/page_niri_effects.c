#include "page_niri_private.h"

const char *const PAGE_NIRI_CURVE_NAMES[N_CURVES + 1] = {
    "ease-out-expo",
    "ease-out-cubic",
    "ease-out-quad",
    "linear",
    NULL
};

const ShaderPreset PAGE_NIRI_OPEN_PRESETS[N_OPEN_PRESETS] = {
    { "default",          "Default (Scale & Fade)",          150, 0 },
    { "metro-flip",       "Metro Turnstile (3D Flip)",       250, 1 },
    { "expanding-circle", "Iris Circle Reveal",              250, 2 },
    { "crt-on",           "CRT Power-On (Scanline Beam)",    300, 1 },
    { "pixelate-in",      "Pixelate Materialize",            250, 2 },
    { "glitch-in",        "Cyberpunk RGB Glitch",            250, 3 },
    { "curtain-open",     "Vertical Split Curtain",          220, 0 },
};

const ShaderPreset PAGE_NIRI_CLOSE_PRESETS[N_CLOSE_PRESETS] = {
    { "default",          "Default (Scale & Fade)",          200, 2 },
    { "fall-and-rotate",  "Gravity Fall & Rotate",           350, 3 },
    { "crt-off",          "CRT TV Turn-Off",                 360, 3 },
    { "ash-dissolve",     "Ash / Particle Dissolve",         420, 3 },
    { "vortex-implode",   "Black Hole Vortex",               380, 3 },
    { "pixelate-out",     "Pixelate Dematerialize",          350, 3 },
    { "slice-shatter",    "Horizontal Slice Shred",          350, 3 },
};

const ShaderPreset PAGE_NIRI_RESIZE_PRESETS[N_RESIZE_PRESETS] = {
    { "default",          "Default (Crossfade & Crop)",      0,   0 },
    { "stretch-next",     "Elastic Stretch",                 0,   0 },
    { "pixelate-morph",   "Pixelate Morph",                  0,   0 },
    { "glitch-morph",     "Chromatic Glitch Resize",         0,   0 },
};

const char *const PAGE_NIRI_COLOR_FILTER_IDS[N_COLOR_FILTERS] = {
    "none",
    "grayscale",
    "night-light",
    "protanopia",
    "deuteranopia",
    "tritanopia",
};

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

static const int SHADOW_STRENGTH_PCTS[] = { 60, 100, 145 };
#define N_SHADOW_STRENGTH_PRESETS 3
static const char *const SHADOW_STRENGTH_LABELS[] = { "Subtle", "Standard", "Deep", NULL };

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

guint
page_niri_color_filter_index_from_id(const char *id)
{
    if (!id || !*id)
        return 0;
    if (g_ascii_strcasecmp(id, "night_light") == 0)
        return 2;
    for (guint i = 0; i < N_COLOR_FILTERS; i++) {
        if (g_ascii_strcasecmp(id, PAGE_NIRI_COLOR_FILTER_IDS[i]) == 0)
            return i;
    }
    return 0;
}

/* ─── Sensitivity, Widget Sync & State Collection ─── */

void
page_niri_effects_update_row_sensitivity(SettingsPageNiri *self)
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

void
page_niri_effects_sync_widgets(SettingsPageNiri *self)
{
    if (!self)
        return;

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
    page_niri_effects_update_row_sensitivity(self);
}

void
page_niri_effects_collect_state(SettingsPageNiri *self)
{
    if (!self)
        return;

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
        g_strlcpy(self->fx_cfg.color_filter, PAGE_NIRI_COLOR_FILTER_IDS[cf_idx],
                  sizeof(self->fx_cfg.color_filter));
    }
    if (self->scale_filter_strength)
        self->fx_cfg.color_filter_strength_pct = (int)gtk_range_get_value(GTK_RANGE(self->scale_filter_strength));
    if (self->scale_night_temp)
        self->fx_cfg.night_light_temperature = (int)gtk_range_get_value(GTK_RANGE(self->scale_night_temp));
}

/* ─── Signal Handlers: Transition Shaders ─── */

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
    self->open_duration_ms = PAGE_NIRI_OPEN_PRESETS[idx].rec_duration_ms;
    self->open_curve_idx   = PAGE_NIRI_OPEN_PRESETS[idx].rec_curve_idx;

    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_open_dur), self->open_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_open_curve), self->open_curve_idx);
    self->syncing_ui = FALSE;

    page_niri_update_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_open_dur_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    self->open_duration_ms = (int)gtk_range_get_value(range);
    page_niri_update_subtitles_and_telemetry(self);
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
    page_niri_update_subtitles_and_telemetry(self);
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
    self->close_duration_ms = PAGE_NIRI_CLOSE_PRESETS[idx].rec_duration_ms;
    self->close_curve_idx   = PAGE_NIRI_CLOSE_PRESETS[idx].rec_curve_idx;

    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_close_dur), self->close_duration_ms);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_close_curve), self->close_curve_idx);
    self->syncing_ui = FALSE;

    page_niri_update_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_close_dur_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;

    self->close_duration_ms = (int)gtk_range_get_value(range);
    page_niri_update_subtitles_and_telemetry(self);
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
    page_niri_update_subtitles_and_telemetry(self);
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
    page_niri_update_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

/* ─── Signal Handlers: Compositor Effects & Colour Stage ─── */

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
    page_niri_effects_update_row_sensitivity(self);
    page_niri_update_subtitles_and_telemetry(self);
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
    page_niri_effects_update_row_sensitivity(self);
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
    g_strlcpy(self->fx_cfg.color_filter, PAGE_NIRI_COLOR_FILTER_IDS[idx],
              sizeof(self->fx_cfg.color_filter));
    if (self->row_filter_strength)
        gtk_widget_set_sensitive(self->row_filter_strength, idx != 0);
    if (self->row_night_temp)
        gtk_widget_set_sensitive(self->row_night_temp, idx == 2);
    page_niri_update_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_filter_strength_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.color_filter_strength_pct = (int)gtk_range_get_value(range);
    page_niri_update_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_scale_night_temp_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageNiri *self = (SettingsPageNiri *)user_data;
    if (self->syncing_ui)
        return;
    self->fx_cfg.night_light_temperature = (int)gtk_range_get_value(range);
    page_niri_update_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

/* ─── Card Builders ─── */

static void
build_fx_toggle_grid(SettingsPageNiri *self, GtkWidget *card_fx)
{
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
}

static void
build_fx_glass_rows(SettingsPageNiri *self, GtkWidget *card_fx)
{
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
        "[effects.glass] compositor backdrop tint & strength (hex synced from Theme)",
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
}

static void
build_fx_shadow_dim_color_rows(SettingsPageNiri *self, GtkWidget *card_fx)
{
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
}

GtkWidget *
page_niri_build_card_effects(SettingsPageNiri *self)
{
    /* ─── Card 2: Compositor Effects & Colour Stage (comp.toml [effects]) ─── */
    GtkWidget *card_fx = create_section_card(
        SETTINGS_CARD_RIGHT,
        "2. COMPOSITOR EFFECTS & COLOUR STAGE",
        "comp.toml [effects]",
        "accent",
        NULL);

    build_fx_toggle_grid(self, card_fx);
    build_fx_glass_rows(self, card_fx);
    build_fx_shadow_dim_color_rows(self, card_fx);
    return card_fx;
}

GtkWidget *
page_niri_build_card_shaders(SettingsPageNiri *self)
{
    /* ─── Card 3: Window Transition Shaders ─── */
    GtkWidget *card_shaders = create_section_card(
        SETTINGS_CARD_CENTER,
        "3. WINDOW TRANSITION SHADERS",
        "comp.toml + KDL",
        "peak-warn",
        NULL);

    const char *open_labels[N_OPEN_PRESETS + 1];
    for (guint i = 0; i < N_OPEN_PRESETS; i++)
        open_labels[i] = PAGE_NIRI_OPEN_PRESETS[i].label;
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

    self->combo_open_curve = create_string_dropdown(PAGE_NIRI_CURVE_NAMES);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        NULL,
        "Open Easing Curve",
        "Transition interpolation curve",
        self->combo_open_curve));

    const char *close_labels[N_CLOSE_PRESETS + 1];
    for (guint i = 0; i < N_CLOSE_PRESETS; i++)
        close_labels[i] = PAGE_NIRI_CLOSE_PRESETS[i].label;
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

    self->combo_close_curve = create_string_dropdown(PAGE_NIRI_CURVE_NAMES);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        NULL,
        "Close Easing Curve",
        "Transition interpolation curve",
        self->combo_close_curve));

    const char *resize_labels[N_RESIZE_PRESETS + 1];
    for (guint i = 0; i < N_RESIZE_PRESETS; i++)
        resize_labels[i] = PAGE_NIRI_RESIZE_PRESETS[i].label;
    resize_labels[N_RESIZE_PRESETS] = NULL;

    self->combo_shader_resize = create_string_dropdown(resize_labels);
    gtk_box_append(GTK_BOX(card_shaders), create_boxed_row(
        "↔",
        "Window Resize Shader Preset",
        "Column width transition morph",
        self->combo_shader_resize));

    return card_shaders;
}

void
page_niri_effects_connect_signals(SettingsPageNiri *self)
{
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
}
