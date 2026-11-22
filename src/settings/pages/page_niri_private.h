#ifndef KAJO_SETTINGS_PAGE_NIRI_PRIVATE_H
#define KAJO_SETTINGS_PAGE_NIRI_PRIVATE_H

/* Internal to src/settings/pages/: SettingsPageNiri instance struct, shared
 * preset tables, and helpers split between page_niri.c and page_niri_effects.c. */

#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../compositor/compositor.h"
#include <string.h>

#define N_NIRI_NAMESPACES 5
#define N_CURVES          4
#define N_OPEN_PRESETS    7
#define N_CLOSE_PRESETS   7
#define N_RESIZE_PRESETS  4
#define N_COLOR_FILTERS   6

typedef struct {
    const char *id;
    const char *label;
    int         rec_duration_ms;
    guint       rec_curve_idx;
} ShaderPreset;

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

/* Shared read-only tables defined in page_niri_effects.c */
extern const char *const PAGE_NIRI_CURVE_NAMES[N_CURVES + 1];
extern const ShaderPreset PAGE_NIRI_OPEN_PRESETS[N_OPEN_PRESETS];
extern const ShaderPreset PAGE_NIRI_CLOSE_PRESETS[N_CLOSE_PRESETS];
extern const ShaderPreset PAGE_NIRI_RESIZE_PRESETS[N_RESIZE_PRESETS];
extern const char *const PAGE_NIRI_COLOR_FILTER_IDS[N_COLOR_FILTERS];

/* Shared helper defined in page_niri.c */
void page_niri_update_subtitles_and_telemetry(SettingsPageNiri *self);

/* Effects & transition shader module (defined in page_niri_effects.c) */
guint      page_niri_color_filter_index_from_id(const char *id);
void       page_niri_effects_update_row_sensitivity(SettingsPageNiri *self);
void       page_niri_effects_sync_widgets(SettingsPageNiri *self);
void       page_niri_effects_collect_state(SettingsPageNiri *self);
GtkWidget *page_niri_build_card_effects(SettingsPageNiri *self);
GtkWidget *page_niri_build_card_shaders(SettingsPageNiri *self);
void       page_niri_effects_connect_signals(SettingsPageNiri *self);

#endif /* KAJO_SETTINGS_PAGE_NIRI_PRIVATE_H */
