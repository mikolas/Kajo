/* comp_overview_core.c — pure zoomed-out workspace overview layout & hit-testing. */
#include "comp_overview_core.h"

#include <math.h>
#include <stdlib.h>

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
