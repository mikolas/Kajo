/* comp_overview_core.c — pure zoomed-out workspace overview layout & hit-testing. */
#include "comp_overview_core.h"

#include <math.h>
#include <stdlib.h>
#include <strings.h>

static double
clampd(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool
rect_contains(CompRect r, double x, double y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void
comp_overview_compute(const CompLayout   *layout,
                      double              out_w,
                      double              out_h,
                      double              scroll_y,
                      size_t              selected_ws,
                      CompWinId           selected_win,
                      CompOverviewWsBand *bands_buf,
                      size_t              max_bands,
                      CompOverviewCard   *cards_buf,
                      size_t              max_cards,
                      CompOverviewScene  *out_scene)
{
    if (!out_scene)
        return;
    out_scene->bands           = bands_buf;
    out_scene->n_bands         = 0;
    out_scene->cards           = cards_buf;
    out_scene->n_cards         = 0;
    out_scene->total_content_h = 0.0;
    out_scene->max_scroll_y    = 0.0;

    if (!layout || !(out_w > 0.0) || !(out_h > 0.0) || !isfinite(out_w) || !isfinite(out_h))
        return;

    size_t n_ws = comp_layout_workspace_count(layout);
    if (n_ws == 0)
        return;

    double avail_w = fmax(200.0, out_w - 2.0 * COMP_OVERVIEW_PAD_X);
    double avail_h = fmax(160.0, out_h - COMP_OVERVIEW_HEADER_H - 2.0 * COMP_OVERVIEW_PAD_Y);

    /* Fit ~2.4 workspace bands comfortably on screen, zooming out slightly when 3+ exist. */
    double target_rows = n_ws <= 2 ? (double)n_ws : 2.35;
    double band_h = (avail_h - COMP_OVERVIEW_WS_GAP * (target_rows - 1.0)) / target_rows;
    band_h = clampd(band_h, 150.0, avail_h * 0.48);

    double total_h = (double)n_ws * band_h + (double)(n_ws > 0 ? n_ws - 1 : 0) * COMP_OVERVIEW_WS_GAP;
    double max_scroll = fmax(0.0, total_h - avail_h);
    double sy = clampd(isfinite(scroll_y) ? scroll_y : 0.0, 0.0, max_scroll);

    out_scene->total_content_h = total_h;
    out_scene->max_scroll_y    = max_scroll;

    /* Vertically center bands when they all fit inside the viewport. */
    double base_y = COMP_OVERVIEW_HEADER_H + COMP_OVERVIEW_PAD_Y;
    if (total_h < avail_h)
        base_y += floor((avail_h - total_h) * 0.5);

    CompWindowGeometry tmp_geoms[128];

    for (size_t ws = 0; ws < n_ws && out_scene->n_bands < max_bands; ws++) {
        double by = floor(base_y + (double)ws * (band_h + COMP_OVERVIEW_WS_GAP) - sy);
        CompRect band = {
            .x = COMP_OVERVIEW_PAD_X,
            .y = by,
            .w = avail_w,
            .h = floor(band_h),
        };
        double inner_pad = 10.0;
        CompRect content = {
            .x = band.x + inner_pad,
            .y = band.y + COMP_OVERVIEW_WS_HDR_H + 6.0,
            .w = fmax(64.0, band.w - 2.0 * inner_pad),
            .h = fmax(48.0, band.h - COMP_OVERVIEW_WS_HDR_H - 16.0),
        };

        size_t n_win = comp_layout_arrange_workspace(layout, ws, out_w, out_h, tmp_geoms, 128);
        if (n_win > 128)
            n_win = 128;

        /* Compute bounding box of visible windows on this workspace so the whole strip fits. */
        double min_x = 0.0, max_x = out_w;
        size_t vis_count = 0;
        for (size_t i = 0; i < n_win; i++) {
            if (!tmp_geoms[i].visible)
                continue;
            vis_count++;
            if (tmp_geoms[i].rect.x < min_x)
                min_x = tmp_geoms[i].rect.x;
            double rx = tmp_geoms[i].rect.x + tmp_geoms[i].rect.w;
            if (rx > max_x)
                max_x = rx;
        }
        double span_w = fmax(out_w, max_x - min_x);
        double scale_x = content.w / span_w;
        double scale_y = content.h / out_h;
        double scale   = fmin(scale_x, scale_y);
        if (scale > 0.45)
            scale = 0.45;
        if (scale < 0.08)
            scale = 0.08;

        double scaled_w = span_w * scale;
        double scaled_h = out_h * scale;
        double ox = floor(content.x + (content.w - scaled_w) * 0.5);
        double oy = floor(content.y + (content.h - scaled_h) * 0.5);

        CompOverviewWsBand b = {
            .ws_index     = ws,
            .band         = band,
            .content      = content,
            .scale        = scale,
            .window_count = vis_count,
            .is_selected  = (ws == selected_ws),
        };
        out_scene->bands[out_scene->n_bands++] = b;

        for (size_t i = 0; i < n_win && out_scene->n_cards < max_cards; i++) {
            const CompWindowGeometry *g = &tmp_geoms[i];
            if (!g->visible)
                continue;

            double cx = floor(ox + (g->rect.x - min_x) * scale);
            double cy = floor(oy + g->rect.y * scale);
            double cw = fmax(36.0, floor(g->rect.w * scale));
            double ch = fmax(32.0, floor(g->rect.h * scale));

            double cap_h = ch >= 48.0 ? 20.0 : fmax(12.0, floor(ch * 0.35));
            CompOverviewCard card = {
                .id          = g->id,
                .ws_index    = ws,
                .card        = { cx, cy, cw, ch },
                .thumb       = { cx + 2.0, cy + 2.0, fmax(1.0, cw - 4.0), fmax(1.0, ch - cap_h - 4.0) },
                .caption     = { cx + 2.0, cy + ch - cap_h - 1.0, fmax(1.0, cw - 4.0), cap_h },
                .floating    = g->floating,
                .is_selected = (g->id == selected_win),
            };
            out_scene->cards[out_scene->n_cards++] = card;
        }
    }
}

bool
comp_overview_hit_test(const CompOverviewScene *scene,
                       double                   lx,
                       double                   ly,
                       size_t                  *out_ws,
                       CompWinId               *out_win)
{
    if (out_ws)  *out_ws  = 0;
    if (out_win) *out_win = 0;
    if (!scene)
        return false;

    /* Check cards in reverse order (floating/topmost cards first). */
    for (size_t i = scene->n_cards; i > 0; i--) {
        const CompOverviewCard *c = &scene->cards[i - 1];
        if (rect_contains(c->card, lx, ly)) {
            if (out_ws)  *out_ws  = c->ws_index;
            if (out_win) *out_win = c->id;
            return true;
        }
    }

    for (size_t i = 0; i < scene->n_bands; i++) {
        const CompOverviewWsBand *b = &scene->bands[i];
        if (rect_contains(b->band, lx, ly)) {
            if (out_ws)  *out_ws  = b->ws_index;
            if (out_win) *out_win = 0;
            return true;
        }
    }
    return false;
}

double
comp_overview_ensure_ws_visible(const CompOverviewScene *scene,
                                double                   out_h,
                                double                   current_scroll_y,
                                size_t                   selected_ws)
{
    if (!scene || selected_ws >= scene->n_bands)
        return clampd(current_scroll_y, 0.0, scene ? scene->max_scroll_y : 0.0);

    double top_bound = COMP_OVERVIEW_HEADER_H + COMP_OVERVIEW_PAD_Y;
    double bot_bound = out_h - COMP_OVERVIEW_PAD_Y;
    const CompOverviewWsBand *b = &scene->bands[selected_ws];

    double next = current_scroll_y;
    if (b->band.y < top_bound)
        next -= (top_bound - b->band.y);
    else if (b->band.y + b->band.h > bot_bound)
        next += (b->band.y + b->band.h - bot_bound);

    return clampd(next, 0.0, scene->max_scroll_y);
}

/* ── Overview Toggle Animation Presets, Curves & Pure Interpolation ── */

static const struct {
    const char        *name;
    CompOverviewPreset val;
} k_overview_presets[] = {
    { "spring-bounce",  COMP_OVERVIEW_PRESET_SPRING_BOUNCE },
    { "spring-snappy",  COMP_OVERVIEW_PRESET_SPRING_SNAPPY },
    { "spring-elastic", COMP_OVERVIEW_PRESET_SPRING_ELASTIC },
    { "spring-smooth",  COMP_OVERVIEW_PRESET_SPRING_SMOOTH },
    { "ease-out-back",  COMP_OVERVIEW_PRESET_EASE_OUT_BACK },
    { "ease-out-expo",  COMP_OVERVIEW_PRESET_EASE_OUT_EXPO },
    { "instant",        COMP_OVERVIEW_PRESET_INSTANT },
    { "custom",         COMP_OVERVIEW_PRESET_CUSTOM },
};

static const struct {
    const char          *name;
    CompOverviewAnimMode val;
} k_overview_modes[] = {
    { "spring",  COMP_OVERVIEW_ANIM_SPRING },
    { "curve",   COMP_OVERVIEW_ANIM_CURVE },
    { "instant", COMP_OVERVIEW_ANIM_INSTANT },
};

static const struct {
    const char       *name;
    CompOverviewCurve val;
} k_overview_curves[] = {
    { "ease-out-back",  COMP_OVERVIEW_CURVE_EASE_OUT_BACK },
    { "ease-out-expo",  COMP_OVERVIEW_CURVE_EASE_OUT_EXPO },
    { "ease-out-cubic", COMP_OVERVIEW_CURVE_EASE_OUT_CUBIC },
    { "ease-out-quad",  COMP_OVERVIEW_CURVE_EASE_OUT_QUAD },
    { "linear",         COMP_OVERVIEW_CURVE_LINEAR },
};

bool
comp_overview_preset_from_str(const char *s, CompOverviewPreset *out)
{
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(k_overview_presets) / sizeof(k_overview_presets[0]); i++) {
        if (strcasecmp(s, k_overview_presets[i].name) == 0) {
            *out = k_overview_presets[i].val;
            return true;
        }
    }
    return false;
}

const char *
comp_overview_preset_to_str(CompOverviewPreset preset)
{
    for (size_t i = 0; i < sizeof(k_overview_presets) / sizeof(k_overview_presets[0]); i++) {
        if (k_overview_presets[i].val == preset)
            return k_overview_presets[i].name;
    }
    return "spring-bounce";
}

bool
comp_overview_anim_mode_from_str(const char *s, CompOverviewAnimMode *out)
{
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(k_overview_modes) / sizeof(k_overview_modes[0]); i++) {
        if (strcasecmp(s, k_overview_modes[i].name) == 0) {
            *out = k_overview_modes[i].val;
            return true;
        }
    }
    return false;
}

const char *
comp_overview_anim_mode_to_str(CompOverviewAnimMode mode)
{
    for (size_t i = 0; i < sizeof(k_overview_modes) / sizeof(k_overview_modes[0]); i++) {
        if (k_overview_modes[i].val == mode)
            return k_overview_modes[i].name;
    }
    return "spring";
}

bool
comp_overview_curve_from_str(const char *s, CompOverviewCurve *out)
{
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(k_overview_curves) / sizeof(k_overview_curves[0]); i++) {
        if (strcasecmp(s, k_overview_curves[i].name) == 0) {
            *out = k_overview_curves[i].val;
            return true;
        }
    }
    return false;
}

const char *
comp_overview_curve_to_str(CompOverviewCurve curve)
{
    for (size_t i = 0; i < sizeof(k_overview_curves) / sizeof(k_overview_curves[0]); i++) {
        if (k_overview_curves[i].val == curve)
            return k_overview_curves[i].name;
    }
    return "ease-out-back";
}

void
comp_overview_anim_apply_preset(CompOverviewAnimConfig *cfg, CompOverviewPreset preset)
{
    if (!cfg)
        return;

    cfg->preset = preset;
    switch (preset) {
    case COMP_OVERVIEW_PRESET_SPRING_SNAPPY:
        cfg->mode              = COMP_OVERVIEW_ANIM_SPRING;
        cfg->spring            = (CompSpringParams){ .damping_ratio = 0.80, .stiffness = 620.0, .epsilon = 0.0005 };
        cfg->curve             = COMP_OVERVIEW_CURVE_EASE_OUT_EXPO;
        cfg->duration_ms       = 200;
        cfg->close_bounce_gain = 0.30;
        break;
    case COMP_OVERVIEW_PRESET_SPRING_ELASTIC:
        cfg->mode              = COMP_OVERVIEW_ANIM_SPRING;
        cfg->spring            = (CompSpringParams){ .damping_ratio = 0.48, .stiffness = 320.0, .epsilon = 0.0005 };
        cfg->curve             = COMP_OVERVIEW_CURVE_EASE_OUT_BACK;
        cfg->duration_ms       = 320;
        cfg->close_bounce_gain = 0.85;
        break;
    case COMP_OVERVIEW_PRESET_SPRING_SMOOTH:
        cfg->mode              = COMP_OVERVIEW_ANIM_SPRING;
        cfg->spring            = (CompSpringParams){ .damping_ratio = 1.00, .stiffness = 460.0, .epsilon = 0.0005 };
        cfg->curve             = COMP_OVERVIEW_CURVE_EASE_OUT_CUBIC;
        cfg->duration_ms       = 250;
        cfg->close_bounce_gain = 0.0;
        break;
    case COMP_OVERVIEW_PRESET_EASE_OUT_BACK:
        cfg->mode              = COMP_OVERVIEW_ANIM_CURVE;
        cfg->spring            = (CompSpringParams){ .damping_ratio = 0.62, .stiffness = 380.0, .epsilon = 0.0005 };
        cfg->curve             = COMP_OVERVIEW_CURVE_EASE_OUT_BACK;
        cfg->duration_ms       = 260;
        cfg->close_bounce_gain = 0.50;
        break;
    case COMP_OVERVIEW_PRESET_EASE_OUT_EXPO:
        cfg->mode              = COMP_OVERVIEW_ANIM_CURVE;
        cfg->spring            = (CompSpringParams){ .damping_ratio = 1.00, .stiffness = 460.0, .epsilon = 0.0005 };
        cfg->curve             = COMP_OVERVIEW_CURVE_EASE_OUT_EXPO;
        cfg->duration_ms       = 220;
        cfg->close_bounce_gain = 0.0;
        break;
    case COMP_OVERVIEW_PRESET_INSTANT:
        cfg->mode              = COMP_OVERVIEW_ANIM_INSTANT;
        cfg->spring            = (CompSpringParams){ .damping_ratio = 1.00, .stiffness = 460.0, .epsilon = 0.0005 };
        cfg->curve             = COMP_OVERVIEW_CURVE_LINEAR;
        cfg->duration_ms       = 0;
        cfg->close_bounce_gain = 0.0;
        break;
    case COMP_OVERVIEW_PRESET_CUSTOM:
    case COMP_OVERVIEW_PRESET_SPRING_BOUNCE:
    default:
        if (preset != COMP_OVERVIEW_PRESET_CUSTOM)
            cfg->preset = COMP_OVERVIEW_PRESET_SPRING_BOUNCE;
        cfg->mode              = COMP_OVERVIEW_ANIM_SPRING;
        cfg->spring            = (CompSpringParams){ .damping_ratio = 0.62, .stiffness = 380.0, .epsilon = 0.0005 };
        cfg->curve             = COMP_OVERVIEW_CURVE_EASE_OUT_BACK;
        cfg->duration_ms       = 260;
        cfg->close_bounce_gain = 0.60;
        break;
    }
}

void
comp_overview_anim_config_defaults(CompOverviewAnimConfig *cfg)
{
    if (!cfg)
        return;
    comp_overview_anim_apply_preset(cfg, COMP_OVERVIEW_PRESET_SPRING_BOUNCE);
}

bool
comp_overview_anim_config_valid(const CompOverviewAnimConfig *cfg)
{
    if (!cfg)
        return false;
    if (!isfinite(cfg->close_bounce_gain) || cfg->close_bounce_gain < 0.0 || cfg->close_bounce_gain > 1.0)
        return false;
    if (!comp_spring_params_valid(&cfg->spring))
        return false;
    if (cfg->mode == COMP_OVERVIEW_ANIM_CURVE) {
        if (cfg->duration_ms < 50 || cfg->duration_ms > 2000)
            return false;
    } else if (cfg->duration_ms < 0 || cfg->duration_ms > 2000) {
        return false;
    }
    return true;
}

double
comp_overview_eval_curve(CompOverviewCurve curve, double u)
{
    u = clampd(u, 0.0, 1.0);
    switch (curve) {
    case COMP_OVERVIEW_CURVE_EASE_OUT_BACK: {
        const double c1 = 1.70158;
        const double c3 = c1 + 1.0;
        double um1 = u - 1.0;
        return 1.0 + c3 * um1 * um1 * um1 + c1 * um1 * um1;
    }
    case COMP_OVERVIEW_CURVE_EASE_OUT_EXPO:
        return (u >= 1.0) ? 1.0 : 1.0 - pow(2.0, -10.0 * u);
    case COMP_OVERVIEW_CURVE_EASE_OUT_CUBIC: {
        double inv = 1.0 - u;
        return 1.0 - inv * inv * inv;
    }
    case COMP_OVERVIEW_CURVE_EASE_OUT_QUAD: {
        double inv = 1.0 - u;
        return 1.0 - inv * inv;
    }
    case COMP_OVERVIEW_CURVE_LINEAR:
    default:
        return u;
    }
}

double
comp_overview_eval_curve_deriv(CompOverviewCurve curve, double u, double dur_s)
{
    if (!(dur_s > 0.0) || !isfinite(dur_s))
        return 0.0;
    const double eps = 0.002;
    double u0 = clampd(u - eps, 0.0, 1.0);
    double u1 = clampd(u + eps, 0.0, 1.0);
    double du = fmax(1e-5, u1 - u0);
    return ((comp_overview_eval_curve(curve, u1) - comp_overview_eval_curve(curve, u0)) / du) / dur_s;
}

static double
lerpd(double a, double b, double t)
{
    return a + (b - a) * t;
}

static CompRect
lerp_rect(CompRect r0, CompRect r1, double t)
{
    return (CompRect){
        .x = lerpd(r0.x, r1.x, t),
        .y = lerpd(r0.y, r1.y, t),
        .w = lerpd(r0.w, r1.w, t),
        .h = lerpd(r0.h, r1.h, t),
    };
}

void
comp_overview_interp_backdrop(double t, float *out_alpha)
{
    if (!out_alpha)
        return;
    *out_alpha = (float)(clampd(t, 0.0, 1.0) * 0.92);
}

void
comp_overview_interp_header(double t, CompOverviewHeaderAnim *out)
{
    if (!out)
        return;
    double hdr_t = clampd(t, 0.0, 1.06);
    out->y     = -COMP_OVERVIEW_HEADER_H * (1.0 - hdr_t);
    out->alpha = (float)clampd(t * 1.4, 0.0, 1.0);
}

void
comp_overview_interp_band(double                    t,
                          double                    out_w,
                          double                    out_h,
                          size_t                    anchor_ws,
                          const CompOverviewWsBand *anchor_band,
                          const CompOverviewWsBand *band,
                          CompOverviewBandAnim     *out)
{
    if (!band || !out)
        return;

    bool is_anchor = (band->ws_index == anchor_ws);
    double band_t = fmax(0.0, t);
    CompRect start_rect;
    if (is_anchor) {
        start_rect = (CompRect){ 0.0, 0.0, fmax(1.0, out_w), fmax(1.0, out_h) };
    } else if (anchor_band) {
        double dy = ((double)band->ws_index - (double)anchor_ws) * 90.0;
        start_rect = (CompRect){
            .x = anchor_band->band.x,
            .y = anchor_band->band.y + dy,
            .w = anchor_band->band.w,
            .h = anchor_band->band.h * 0.85,
        };
    } else {
        start_rect = band->band;
    }

    CompRect r = lerp_rect(start_rect, band->band, band_t);
    if (r.w < 24.0) r.w = 24.0;
    if (r.h < 24.0) r.h = 24.0;

    out->rect        = r;
    out->alpha       = is_anchor ? (float)clampd(t * 1.6, 0.0, 1.0)
                                 : (float)clampd((t - 0.12) / 0.65, 0.0, 1.0);
    out->badge_alpha = (float)clampd((t - 0.25) / 0.55, 0.0, 1.0);
}

void
comp_overview_interp_card(double                      t,
                          double                      out_w,
                          double                      out_h,
                          size_t                      anchor_ws,
                          double                      close_bounce_gain,
                          const CompOverviewWsBand   *parent_band,
                          const CompOverviewBandAnim *parent_band_anim,
                          const CompOverviewCard     *card,
                          bool                        has_live_rect,
                          CompRect                    live_rect,
                          CompOverviewCardAnim       *out)
{
    if (!card || !out)
        return;

    double chrome_a = clampd((t - 0.35) / 0.60, 0.0, 1.0);
    double cap_h = card->caption.h;

    if (card->ws_index == anchor_ws && has_live_rect) {
        CompRect cur_thumb;
        if (t >= 0.0) {
            cur_thumb = lerp_rect(live_rect, card->thumb, t);
        } else if (close_bounce_gain > 0.0) {
            double punch = 1.0 + fabs(t) * 0.22 * clampd(close_bounce_gain, 0.0, 1.0);
            double cx = out_w * 0.5;
            double cy = out_h * 0.5;
            cur_thumb = (CompRect){
                .x = cx + (live_rect.x - cx) * punch,
                .y = cy + (live_rect.y - cy) * punch,
                .w = live_rect.w * punch,
                .h = live_rect.h * punch,
            };
        } else {
            cur_thumb = live_rect;
        }
        if (cur_thumb.w < 16.0) cur_thumb.w = 16.0;
        if (cur_thumb.h < 16.0) cur_thumb.h = 16.0;

        double scaled_cap = cap_h * chrome_a;
        CompRect cur_card = {
            .x = cur_thumb.x - 2.0 * chrome_a,
            .y = cur_thumb.y - 2.0 * chrome_a,
            .w = cur_thumb.w + 4.0 * chrome_a,
            .h = cur_thumb.h + 3.0 * chrome_a + scaled_cap,
        };
        if (cur_card.w < 20.0) cur_card.w = 20.0;
        if (cur_card.h < 20.0) cur_card.h = 20.0;

        out->card         = cur_card;
        out->thumb        = cur_thumb;
        out->caption      = (CompRect){
            .x = cur_card.x + 2.0,
            .y = cur_card.y + fmax(0.0, cur_card.h - cap_h - 1.0),
            .w = fmax(1.0, cur_card.w - 4.0),
            .h = cap_h,
        };
        out->card_alpha   = (float)chrome_a;
        out->chrome_alpha = (float)chrome_a;
        out->thumb_alpha  = (t >= 0.0 || close_bounce_gain > 0.001)
            ? 1.0f
            : (float)clampd(1.0 + t * 5.0, 0.0, 1.0);
    } else {
        CompRect br = parent_band_anim ? parent_band_anim->rect
                                       : (parent_band ? parent_band->band : card->card);
        double pw = (parent_band && parent_band->band.w > 1.0) ? parent_band->band.w : 1.0;
        double ph = (parent_band && parent_band->band.h > 1.0) ? parent_band->band.h : 1.0;
        double px = parent_band ? parent_band->band.x : br.x;
        double py = parent_band ? parent_band->band.y : br.y;
        double sx = br.w / pw;
        double sy = br.h / ph;

        CompRect cur_card = {
            .x = br.x + (card->card.x - px) * sx,
            .y = br.y + (card->card.y - py) * sy,
            .w = fmax(20.0, card->card.w * sx),
            .h = fmax(20.0, card->card.h * sy),
        };
        CompRect cur_thumb = {
            .x = cur_card.x + 2.0,
            .y = cur_card.y + 2.0,
            .w = fmax(16.0, cur_card.w - 4.0),
            .h = fmax(12.0, cur_card.h - cap_h - 4.0),
        };
        float card_a = (float)clampd((t - 0.14) / 0.65, 0.0, 1.0);
        out->card         = cur_card;
        out->thumb        = cur_thumb;
        out->caption      = (CompRect){
            .x = cur_card.x + 2.0,
            .y = cur_card.y + fmax(0.0, cur_card.h - cap_h - 1.0),
            .w = fmax(1.0, cur_card.w - 4.0),
            .h = cap_h,
        };
        out->card_alpha   = card_a;
        out->thumb_alpha  = card_a;
        out->chrome_alpha = card_a * (float)chrome_a;
    }
}
