/* comp_overview_core.h — pure zoomed-out workspace overview geometry & hit-testing (no wlroots/GLib). */
#ifndef KAJO_COMP_OVERVIEW_CORE_H
#define KAJO_COMP_OVERVIEW_CORE_H

#include "comp_layout.h"

#include <stdbool.h>
#include <stddef.h>

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

#endif /* KAJO_COMP_OVERVIEW_CORE_H */
