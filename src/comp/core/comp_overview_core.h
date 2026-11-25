/* comp_overview_core.h — pure zoomed-out workspace overview geometry & hit-testing (no wlroots/GLib). */
#ifndef KAJO_COMP_OVERVIEW_CORE_H
#define KAJO_COMP_OVERVIEW_CORE_H

#include "comp_layout.h"
#include "comp_spring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMP_OVERVIEW_HEADER_H 40.0
#define COMP_OVERVIEW_PAD_X    24.0
#define COMP_OVERVIEW_PAD_Y    16.0
#define COMP_OVERVIEW_WS_GAP   16.0
#define COMP_OVERVIEW_WS_HDR_H 24.0

typedef struct {
    size_t   ws_index;
    CompRect band;         /* outer workspace band box in output logical coordinates */
    CompRect content;      /* inner window area inside the workspace band */
    double   scale;        /* uniform scale applied to windows in this workspace */
    size_t   window_count; /* visible windows in this workspace */
    bool     is_selected;
} CompOverviewWsBand;

typedef struct {
    CompWinId id;
    size_t    ws_index;
    CompRect  card;        /* scaled card rect (including caption) in output logical coords */
    CompRect  thumb;       /* scaled live window mirror rect inside card */
    CompRect  caption;     /* bottom title bar rect inside card */
    bool      floating;
    bool      is_selected;
} CompOverviewCard;

typedef struct {
    CompOverviewWsBand *bands;
    size_t              n_bands;
    CompOverviewCard   *cards;
    size_t              n_cards;
    double              total_content_h;
    double              max_scroll_y;
} CompOverviewScene;

/* Computes zoomed-out workspace bands and scaled window card rects into caller buffers.
 * `selected_ws` and `selected_win` (0 if empty workspace) mark the highlighted target.
 * Returns total workspace count (bands written = min(n_ws, max_bands)). */
void comp_overview_compute(const CompLayout   *layout,
                           double              out_w,
                           double              out_h,
                           double              scroll_y,
                           size_t              selected_ws,
                           CompWinId           selected_win,
                           CompOverviewWsBand *bands_buf,
                           size_t              max_bands,
                           CompOverviewCard   *cards_buf,
                           size_t              max_cards,
                           CompOverviewScene  *out_scene);

/* Hit-test logical output point (lx, ly) against computed overview scene.
 * Returns true if inside a card (sets *out_win != 0 and *out_ws) or inside a workspace
 * band (sets *out_win = 0 and *out_ws). */
bool comp_overview_hit_test(const CompOverviewScene *scene,
                            double                   lx,
                            double                   ly,
                            size_t                  *out_ws,
                            CompWinId               *out_win);

/* Computes vertical scroll_y clamped so `selected_ws` band stays visible. */
double comp_overview_ensure_ws_visible(const CompOverviewScene *scene,
                                       double                   out_h,
                                       double                   current_scroll_y,
                                       size_t                   selected_ws);

/* ── Overview Toggle Animation Presets, Curves & Pure Interpolation ── */

typedef enum {
    COMP_OVERVIEW_ANIM_SPRING  = 0,
    COMP_OVERVIEW_ANIM_CURVE   = 1,
    COMP_OVERVIEW_ANIM_INSTANT = 2,
} CompOverviewAnimMode;

typedef enum {
    COMP_OVERVIEW_CURVE_EASE_OUT_BACK  = 0,
    COMP_OVERVIEW_CURVE_EASE_OUT_EXPO  = 1,
    COMP_OVERVIEW_CURVE_EASE_OUT_CUBIC = 2,
    COMP_OVERVIEW_CURVE_EASE_OUT_QUAD  = 3,
    COMP_OVERVIEW_CURVE_LINEAR         = 4,
} CompOverviewCurve;

typedef enum {
    COMP_OVERVIEW_PRESET_SPRING_BOUNCE  = 0,
    COMP_OVERVIEW_PRESET_SPRING_SNAPPY  = 1,
    COMP_OVERVIEW_PRESET_SPRING_ELASTIC = 2,
    COMP_OVERVIEW_PRESET_SPRING_SMOOTH  = 3,
    COMP_OVERVIEW_PRESET_EASE_OUT_BACK  = 4,
    COMP_OVERVIEW_PRESET_EASE_OUT_EXPO  = 5,
    COMP_OVERVIEW_PRESET_INSTANT        = 6,
    COMP_OVERVIEW_PRESET_CUSTOM         = 7,
} CompOverviewPreset;

typedef struct {
    CompOverviewPreset   preset;
    CompOverviewAnimMode mode;
    CompSpringParams     spring;
    CompOverviewCurve    curve;
    int32_t              duration_ms;       /* 50..2000 for CURVE mode (0 for INSTANT) */
    double               close_bounce_gain; /* 0.0..1.0 */
} CompOverviewAnimConfig;

typedef struct {
    double y;
    float  alpha;
} CompOverviewHeaderAnim;

typedef struct {
    CompRect rect;
    float    alpha;
    float    badge_alpha;
} CompOverviewBandAnim;

typedef struct {
    CompRect card;
    CompRect thumb;
    CompRect caption;
    float    card_alpha;
    float    chrome_alpha;
    float    thumb_alpha;
} CompOverviewCardAnim;

bool        comp_overview_preset_from_str(const char *s, CompOverviewPreset *out);
const char *comp_overview_preset_to_str(CompOverviewPreset preset);
bool        comp_overview_anim_mode_from_str(const char *s, CompOverviewAnimMode *out);
const char *comp_overview_anim_mode_to_str(CompOverviewAnimMode mode);
bool        comp_overview_curve_from_str(const char *s, CompOverviewCurve *out);
const char *comp_overview_curve_to_str(CompOverviewCurve curve);

void        comp_overview_anim_config_defaults(CompOverviewAnimConfig *cfg);
void        comp_overview_anim_apply_preset(CompOverviewAnimConfig *cfg, CompOverviewPreset preset);
bool        comp_overview_anim_config_valid(const CompOverviewAnimConfig *cfg);

double      comp_overview_eval_curve(CompOverviewCurve curve, double u);
double      comp_overview_eval_curve_deriv(CompOverviewCurve curve, double u, double dur_s);

void        comp_overview_interp_backdrop(double t, float *out_alpha);
void        comp_overview_interp_header(double t, CompOverviewHeaderAnim *out);
void        comp_overview_interp_band(double                    t,
                                      double                    out_w,
                                      double                    out_h,
                                      size_t                    anchor_ws,
                                      const CompOverviewWsBand *anchor_band,
                                      const CompOverviewWsBand *band,
                                      CompOverviewBandAnim     *out);
void        comp_overview_interp_card(double                      t,
                                      double                      out_w,
                                      double                      out_h,
                                      size_t                      anchor_ws,
                                      double                      close_bounce_gain,
                                      const CompOverviewWsBand   *parent_band,
                                      const CompOverviewBandAnim *parent_band_anim,
                                      const CompOverviewCard     *card,
                                      bool                        has_live_rect,
                                      CompRect                    live_rect,
                                      CompOverviewCardAnim       *out);

#endif /* KAJO_COMP_OVERVIEW_CORE_H */
