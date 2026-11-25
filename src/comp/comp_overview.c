/* comp_overview.c — Zoomed-out workspace & scrolling-strip overview in s->tree_overlay.
 *
 * Shows all workspaces stacked vertically with live bilinear-scaled mirrors of every
 * window, Aero+Metro workspace bands, active accent framing, spring/curve animated
 * Option A zoom morphing, and full keyboard/pointer navigation.
 */
#include "comp_server.h"

#include "comp_log.h"
#include "core/comp_overview_core.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <talloc.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define OVERVIEW_MAX_BANDS 32
#define OVERVIEW_MAX_CARDS 192

static const float k_backdrop_rgb[3]      = { 0.039216f, 0.039216f, 0.039216f }; /* #0a0a0a */
static const float k_header_bg_rgba[4]    = { 0.078431f, 0.078431f, 0.078431f, 1.0f };  /* #141414 */
static const float k_band_bg_rgba[4]      = { 0.070588f, 0.070588f, 0.070588f, 1.0f };  /* #121212 */
static const float k_card_bg_rgba[4]      = { 0.094118f, 0.094118f, 0.094118f, 1.0f };  /* #181818 */
static const float k_border_dim_rgba[4]   = { 0.168627f, 0.168627f, 0.168627f, 1.0f };  /* #2b2b2b */
static const float k_fg_primary_rgba[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };                 /* #ffffff */
static const float k_fg_secondary_rgba[4] = { 0.627451f, 0.627451f, 0.627451f, 1.0f };  /* #a0a0a0 */
static const float k_default_accent[4]    = { 0.0f, 0.470588f, 0.831373f, 1.0f };       /* #0078d4 */
static const float k_transparent_rgba[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };

typedef enum {
    COMP_OVERVIEW_CLOSED = 0,
    COMP_OVERVIEW_OPENING,
    COMP_OVERVIEW_OPEN,
    COMP_OVERVIEW_CLOSING,
} CompOverviewState;

typedef struct {
    CompServer              *server;
    CompWinId                win_id;
    struct wlr_surface      *surface;
    struct wlr_scene_buffer *thumb_buf;
    CompRect                 slot;
    float                    opacity;
} OverviewMirror;

typedef struct {
    size_t                   band_idx;
    size_t                   ws_index;
    bool                     is_selected;
    struct wlr_scene_rect   *bg;
    struct wlr_scene_rect   *border[4];
    struct wlr_scene_buffer *badge_buf;
    int                      badge_w;
    int                      badge_h;
} OverviewBandNode;

typedef struct {
    CompWinId                id;
    size_t                   card_idx;
    size_t                   ws_index;
    bool                     is_selected;
    bool                     has_live_rect;
    CompRect                 live_rect;
    struct wlr_scene_rect   *bg;
    struct wlr_scene_rect   *border[4];
    struct wlr_scene_buffer *caption_buf;
    OverviewMirror          *mirror;
} OverviewCardNode;

typedef struct CompOverview {
    CompServer            *server;
    CompOverviewState      state;
    size_t                 selected_ws;
    CompWinId              selected_win;
    size_t                 initial_ws;
    CompWinId              initial_win;
    size_t                 anchor_ws;
    double                 scroll_y;

    double                 progress;
    double                 velocity;
    double                 target;
    CompSpring             spring;
    CompOverviewAnimMode   active_mode;
    CompOverviewCurve      active_curve;
    double                 curve_from;
    double                 curve_to;
    double                 curve_start;
    double                 curve_dur_s;
    double                 close_bounce_gain;

    CompOverviewWsBand     bands[OVERVIEW_MAX_BANDS];
    CompOverviewCard       cards[OVERVIEW_MAX_CARDS];
    CompOverviewScene      scene_geom;

    CompWinId              live_ids[OVERVIEW_MAX_CARDS];
    CompRect               live_rects[OVERVIEW_MAX_CARDS];
    size_t                 n_live_rects;

    struct wlr_scene_tree *tree;
    void                  *mirrors_ctx;

    struct wlr_scene_rect   *bg_rect;
    struct wlr_scene_rect   *hdr_rect;
    struct wlr_scene_rect   *hdr_line;
    struct wlr_scene_buffer *hdr_title_buf;
    struct wlr_scene_buffer *hdr_hint_buf;
    int                      hdr_w;
    int                      hdr_h;

    OverviewBandNode       band_nodes[OVERVIEW_MAX_BANDS];
    size_t                 n_band_nodes;
    OverviewCardNode       card_nodes[OVERVIEW_MAX_CARDS];
    size_t                 n_card_nodes;
} CompOverview;

static float
clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void
premul_rgba(const float base[4], float alpha, float out[4])
{
    float a = base[3] * clampf(alpha, 0.0f, 1.0f);
    out[0] = base[0] * a;
    out[1] = base[1] * a;
    out[2] = base[2] * a;
    out[3] = a;
}

static void
overview_destroy_scene(CompOverview *ov)
{
    if (!ov)
        return;
    talloc_free(ov->mirrors_ctx);
    ov->mirrors_ctx = NULL;
    if (ov->server && ov->server->scene && ov->tree) {
        wlr_scene_node_destroy(&ov->tree->node);
        ov->tree = NULL;
    }
    ov->bg_rect       = NULL;
    ov->hdr_rect      = NULL;
    ov->hdr_line      = NULL;
    ov->hdr_title_buf = NULL;
    ov->hdr_hint_buf  = NULL;
    ov->n_band_nodes  = 0;
    ov->n_card_nodes  = 0;
}

static int
overview_destructor(CompOverview *ov)
{
    overview_destroy_scene(ov);
    return 0;
}

static void
create_border_rects(struct wlr_scene_tree *parent, struct wlr_scene_rect *out[4], const float rgba[4])
{
    for (int i = 0; i < 4; i++)
        out[i] = wlr_scene_rect_create(parent, 1, 1, rgba);
}

static void
update_border_rects(struct wlr_scene_rect *b_rects[4],
                    int x, int y, int w, int h, int b, const float rgba[4])
{
    if (b <= 0 || w <= 0 || h <= 0) {
        for (int i = 0; i < 4; i++) {
            if (b_rects[i])
                wlr_scene_node_set_enabled(&b_rects[i]->node, false);
        }
        return;
    }
    int inner_h = h > 2 * b ? h - 2 * b : 1;
    int bw = w > 0 ? w : 1;
    for (int i = 0; i < 4; i++) {
        if (!b_rects[i])
            continue;
        wlr_scene_node_set_enabled(&b_rects[i]->node, true);
        wlr_scene_rect_set_color(b_rects[i], rgba);
    }
    if (b_rects[0]) {
        wlr_scene_node_set_position(&b_rects[0]->node, x, y);
        wlr_scene_rect_set_size(b_rects[0], bw, b);
    }
    if (b_rects[1]) {
        wlr_scene_node_set_position(&b_rects[1]->node, x, y + h - b);
        wlr_scene_rect_set_size(b_rects[1], bw, b);
    }
    if (b_rects[2]) {
        wlr_scene_node_set_position(&b_rects[2]->node, x, y + b);
        wlr_scene_rect_set_size(b_rects[2], b, inner_h);
    }
    if (b_rects[3]) {
        wlr_scene_node_set_position(&b_rects[3]->node, x + w - b, y + b);
        wlr_scene_rect_set_size(b_rects[3], b, inner_h);
    }
}

static void
update_overview_mirror(OverviewMirror *m)
{
    if (!m || !m->thumb_buf || !m->surface)
        return;
    struct wlr_buffer *buf = m->surface->buffer ? &m->surface->buffer->base : NULL;
    wlr_scene_buffer_set_buffer(m->thumb_buf, buf);
    wlr_scene_buffer_set_opacity(m->thumb_buf, clampf(m->opacity, 0.0f, 1.0f));
    if (!buf || buf->width <= 0 || buf->height <= 0)
        return;

    double sw = (double)m->surface->current.width;
    double sh = (double)m->surface->current.height;
    if (sw <= 0.0 || sh <= 0.0) {
        sw = (double)buf->width;
        sh = (double)buf->height;
    }
    double slot_w = m->slot.w >= 1.0 ? m->slot.w : 1.0;
    double slot_h = m->slot.h >= 1.0 ? m->slot.h : 1.0;
    double scale = fmin(slot_w / sw, slot_h / sh);
    int dw = (int)lround(sw * scale);
    int dh = (int)lround(sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int)slot_w) dw = (int)slot_w;
    if (dh > (int)slot_h) dh = (int)slot_h;

    int tx = (int)lround(m->slot.x) + ((int)lround(slot_w) - dw) / 2;
    int ty = (int)lround(m->slot.y) + ((int)lround(slot_h) - dh) / 2;
    wlr_scene_node_set_position(&m->thumb_buf->node, tx, ty);
    wlr_scene_buffer_set_dest_size(m->thumb_buf, dw, dh);
    wlr_scene_buffer_set_transform(m->thumb_buf, m->surface->current.transform);
    wlr_scene_buffer_set_filter_mode(m->thumb_buf, WLR_SCALE_FILTER_BILINEAR);
}

static void
handle_mirror_commit(void *owner, void *data)
{
    (void)data;
    update_overview_mirror((OverviewMirror *)owner);
}

static const float *
accent_rgba(const CompServer *s)
{
    if (s && s->config)
        return s->config->active_border_rgba;
    return k_default_accent;
}

static double
primary_output_scale(const CompServer *s)
{
    struct wlr_output *out = comp_server_primary_output(s);
    return (out && out->scale > 0.0f) ? (double)out->scale : 1.0;
}

static struct wlr_scene_buffer *
overview_create_label(struct wlr_scene_tree *tree,
                      const char            *text,
                      int                    x,
                      int                    y,
                      int                    w,
                      int                    h,
                      double                 scale,
                      const float            fg[4],
                      const float            bg[4],
                      int                    pad_x)
{
    struct wlr_buffer *buf = comp_text_render_label(text, w, h, scale, fg, bg, NULL, 0, pad_x);
    if (!buf)
        return NULL;
    struct wlr_scene_buffer *sb = wlr_scene_buffer_create(tree, NULL);
    if (sb) {
        wlr_scene_node_set_position(&sb->node, x, y);
        comp_scene_buffer_set_owned_buffer(sb, buf);
        wlr_scene_buffer_set_dest_size(sb, w, h);
    } else {
        wlr_buffer_drop(buf);
    }
    return sb;
}

static void
overview_capture_live_rects(CompOverview *ov, double now_s)
{
    CompServer *s = ov ? ov->server : NULL;
    ov->n_live_rects = 0;
    if (!ov || !s || !s->layout)
        return;

    size_t n_ws = comp_layout_workspace_count(s->layout);
    if (ov->anchor_ws >= n_ws)
        return;

    CompRect work = {0};
    comp_server_working_area(s, &work);
    double full_vw = 0.0, full_vh = 0.0;
    comp_server_viewport_size(s, &full_vw, &full_vh);

    CompWindowGeometry ws_geoms[OVERVIEW_MAX_CARDS];
    size_t n_ws_wins = comp_layout_arrange_workspace(
        s->layout, ov->anchor_ws, work.w > 1.0 ? work.w : full_vw,
        work.h > 1.0 ? work.h : full_vh, ws_geoms, OVERVIEW_MAX_CARDS);
    if (n_ws_wins > OVERVIEW_MAX_CARDS)
        n_ws_wins = OVERVIEW_MAX_CARDS;

    CompWindowGeometry mot_geoms[OVERVIEW_MAX_CARDS];
    size_t n_mot = 0;
    double view_x = 0.0, ws_y = 0.0;
    if (s->motion) {
        n_mot = comp_motion_sample(s->motion, now_s, mot_geoms, OVERVIEW_MAX_CARDS, &view_x, &ws_y);
        if (n_mot > OVERVIEW_MAX_CARDS)
            n_mot = OVERVIEW_MAX_CARDS;
    }

    for (size_t i = 0; i < n_ws_wins && ov->n_live_rects < OVERVIEW_MAX_CARDS; i++) {
        CompWinId id = ws_geoms[i].id;
        CompRect r = {
            .x = ws_geoms[i].fullscreen ? 0.0 : ws_geoms[i].rect.x + work.x - view_x,
            .y = ws_geoms[i].fullscreen ? 0.0 : ws_geoms[i].rect.y + work.y,
            .w = ws_geoms[i].fullscreen ? full_vw : ws_geoms[i].rect.w,
            .h = ws_geoms[i].fullscreen ? full_vh : ws_geoms[i].rect.h,
        };
        for (size_t j = 0; j < n_mot; j++) {
            if (mot_geoms[j].id == id && mot_geoms[j].visible) {
                r.x = mot_geoms[j].fullscreen ? 0.0 : mot_geoms[j].rect.x + work.x;
                r.y = mot_geoms[j].fullscreen ? mot_geoms[j].rect.y : mot_geoms[j].rect.y + work.y;
                r.w = mot_geoms[j].fullscreen ? full_vw : mot_geoms[j].rect.w;
                r.h = mot_geoms[j].fullscreen ? full_vh : mot_geoms[j].rect.h;
                break;
            }
        }
        ov->live_ids[ov->n_live_rects]   = id;
        ov->live_rects[ov->n_live_rects] = r;
        ov->n_live_rects++;
    }
}

static bool
lookup_live_rect(const CompOverview *ov, CompWinId id, CompRect *out_rect)
{
    for (size_t i = 0; i < ov->n_live_rects; i++) {
        if (ov->live_ids[i] == id) {
            if (out_rect)
                *out_rect = ov->live_rects[i];
            return true;
        }
    }
    return false;
}

static void
refresh_card_live_rects(CompOverview *ov)
{
    for (size_t i = 0; i < ov->n_card_nodes; i++) {
        OverviewCardNode *cn = &ov->card_nodes[i];
        cn->has_live_rect = (cn->ws_index == ov->anchor_ws) &&
                            lookup_live_rect(ov, cn->id, &cn->live_rect);
    }
}

static void
overview_build_header(CompOverview *ov, int iw, size_t n_ws, double scale, const float *accent)
{
    int hdr_h = (int)COMP_OVERVIEW_HEADER_H;
    ov->hdr_w = iw;
    ov->hdr_h = hdr_h;

    ov->hdr_rect = wlr_scene_rect_create(ov->tree, iw, hdr_h, k_header_bg_rgba);
    if (ov->hdr_rect)
        wlr_scene_node_set_position(&ov->hdr_rect->node, 0, 0);
    ov->hdr_line = wlr_scene_rect_create(ov->tree, iw, 1, accent);
    if (ov->hdr_line)
        wlr_scene_node_set_position(&ov->hdr_line->node, 0, hdr_h - 1);

    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf),
             "OVERVIEW  •  %zu WORKSPACE%s  •  %zu WINDOW%s",
             n_ws, n_ws == 1 ? "" : "S",
             ov->scene_geom.n_cards, ov->scene_geom.n_cards == 1 ? "" : "S");
    ov->hdr_title_buf = overview_create_label(ov->tree, title_buf, 0, 0, iw / 2, hdr_h - 2, scale,
                                              k_fg_primary_rgba, k_transparent_rgba, 16);

    const char *hint = "CLICK/ENTER FOCUS  •  ARROWS/WHEEL NAV  •  MIDDLE-CLICK CLOSE  •  ESC EXIT";
    int hint_w = iw / 2;
    ov->hdr_hint_buf = overview_create_label(ov->tree, hint, iw - hint_w, 0, hint_w, hdr_h - 2, scale,
                                             k_fg_secondary_rgba, k_transparent_rgba, 16);
}

static void
overview_build_bands(CompOverview *ov, double out_h, double scale, const float *accent)
{
    ov->n_band_nodes = 0;
    for (size_t i = 0; i < ov->scene_geom.n_bands && ov->n_band_nodes < OVERVIEW_MAX_BANDS; i++) {
        const CompOverviewWsBand *b = &ov->scene_geom.bands[i];
        if (b->band.y + b->band.h <= COMP_OVERVIEW_HEADER_H || b->band.y >= out_h)
            continue;

        OverviewBandNode *bn = &ov->band_nodes[ov->n_band_nodes++];
        memset(bn, 0, sizeof(*bn));
        bn->band_idx    = i;
        bn->ws_index    = b->ws_index;
        bn->is_selected = b->is_selected;

        int bx = (int)b->band.x;
        int by = (int)b->band.y;
        int bw = (int)b->band.w;
        int bh = (int)b->band.h;

        bn->bg = wlr_scene_rect_create(ov->tree, bw > 0 ? bw : 1, bh > 0 ? bh : 1, k_band_bg_rgba);
        if (bn->bg)
            wlr_scene_node_set_position(&bn->bg->node, bx, by);
        create_border_rects(ov->tree, bn->border, b->is_selected ? accent : k_border_dim_rgba);
        update_border_rects(bn->border, bx, by, bw, bh, b->is_selected ? 2 : 1,
                            b->is_selected ? accent : k_border_dim_rgba);

        char ws_lbl[64];
        if (b->window_count == 0)
            snprintf(ws_lbl, sizeof(ws_lbl), "WORKSPACE %zu  [ EMPTY ]", b->ws_index + 1);
        else
            snprintf(ws_lbl, sizeof(ws_lbl), "WORKSPACE %zu  (%zu)", b->ws_index + 1, b->window_count);

        int badge_w = 0, badge_h = 0;
        struct wlr_buffer *bbuf = comp_text_render_badge(
            ws_lbl, scale,
            b->is_selected ? k_fg_primary_rgba : k_fg_secondary_rgba,
            b->is_selected ? accent : k_card_bg_rgba,
            b->is_selected ? accent : k_border_dim_rgba,
            8, 3, &badge_w, &badge_h);
        if (bbuf) {
            struct wlr_scene_buffer *sb = wlr_scene_buffer_create(ov->tree, NULL);
            if (sb) {
                wlr_scene_node_set_position(&sb->node, bx + 8, by + 4);
                comp_scene_buffer_set_owned_buffer(sb, bbuf);
                wlr_scene_buffer_set_dest_size(sb, badge_w, badge_h);
                bn->badge_buf = sb;
                bn->badge_w   = badge_w;
                bn->badge_h   = badge_h;
            } else {
                wlr_buffer_drop(bbuf);
            }
        }
    }
}

static void
overview_build_cards(CompOverview *ov, double out_h, double scale, const float *accent)
{
    CompServer *s = ov->server;
    CompWindowInfo infos[OVERVIEW_MAX_CARDS];
    size_t n_infos = comp_window_list_info(s, infos, OVERVIEW_MAX_CARDS);

    ov->n_card_nodes = 0;
    for (size_t i = 0; i < ov->scene_geom.n_cards && ov->n_card_nodes < OVERVIEW_MAX_CARDS; i++) {
        const CompOverviewCard *c = &ov->scene_geom.cards[i];
        if (c->card.y + c->card.h <= COMP_OVERVIEW_HEADER_H || c->card.y >= out_h)
            continue;

        OverviewCardNode *cn = &ov->card_nodes[ov->n_card_nodes++];
        memset(cn, 0, sizeof(*cn));
        cn->id            = c->id;
        cn->card_idx      = i;
        cn->ws_index      = c->ws_index;
        cn->is_selected   = c->is_selected;
        cn->has_live_rect = (c->ws_index == ov->anchor_ws) &&
                            lookup_live_rect(ov, c->id, &cn->live_rect);

        int cx = (int)c->card.x;
        int cy = (int)c->card.y;
        int cw = (int)c->card.w;
        int ch = (int)c->card.h;

        cn->bg = wlr_scene_rect_create(ov->tree, cw > 0 ? cw : 1, ch > 0 ? ch : 1, k_card_bg_rgba);
        if (cn->bg)
            wlr_scene_node_set_position(&cn->bg->node, cx, cy);

        struct CompWindow *win = comp_win_from_id(s, c->id);
        struct wlr_surface *surf = win ? comp_window_get_surface(win) : NULL;
        if (surf && ov->mirrors_ctx) {
            OverviewMirror *m = talloc_zero(ov->mirrors_ctx, OverviewMirror);
            if (m) {
                m->server    = s;
                m->win_id    = c->id;
                m->surface   = surf;
                m->slot      = c->thumb;
                m->opacity   = 1.0f;
                m->thumb_buf = wlr_scene_buffer_create(ov->tree, NULL);
                if (m->thumb_buf) {
                    update_overview_mirror(m);
                    comp_listen(m, &surf->events.commit, handle_mirror_commit);
                }
                cn->mirror = m;
            }
        }

        const char *title = "Window";
        for (size_t j = 0; j < n_infos; j++) {
            if (infos[j].id == c->id) {
                if (infos[j].title && infos[j].title[0] != '\0')
                    title = infos[j].title;
                else if (infos[j].app_id && infos[j].app_id[0] != '\0')
                    title = infos[j].app_id;
                break;
            }
        }

        int cap_w = (int)c->caption.w;
        int cap_h = (int)c->caption.h;
        if (cap_w >= 16 && cap_h >= 12) {
            cn->caption_buf = overview_create_label(
                ov->tree, title, (int)c->caption.x, (int)c->caption.y,
                cap_w, cap_h, scale,
                c->is_selected ? k_fg_primary_rgba : k_fg_secondary_rgba,
                k_header_bg_rgba, 6);
        }

        create_border_rects(ov->tree, cn->border, c->is_selected ? accent : k_border_dim_rgba);
        update_border_rects(cn->border, cx, cy, cw, ch, c->is_selected ? 2 : 1,
                            c->is_selected ? accent : k_border_dim_rgba);
    }
}

static void
overview_apply_frame(CompOverview *ov, double t)
{
    if (!ov || !ov->server || !ov->tree || ov->state == COMP_OVERVIEW_CLOSED)
        return;

    double out_w = 0.0, out_h = 0.0;
    comp_server_viewport_size(ov->server, &out_w, &out_h);
    if (out_w < 64.0 || out_h < 64.0)
        return;

    const float *accent = accent_rgba(ov->server);

    if (ov->bg_rect) {
        float bg_a = 0.0f;
        comp_overview_interp_backdrop(t, &bg_a);
        float c[4] = {
            k_backdrop_rgb[0] * bg_a,
            k_backdrop_rgb[1] * bg_a,
            k_backdrop_rgb[2] * bg_a,
            bg_a,
        };
        wlr_scene_rect_set_color(ov->bg_rect, c);
    }

    CompOverviewHeaderAnim ha = {0};
    comp_overview_interp_header(t, &ha);
    int hy = (int)lround(ha.y);
    if (ov->hdr_rect) {
        float c[4];
        premul_rgba(k_header_bg_rgba, ha.alpha, c);
        wlr_scene_node_set_position(&ov->hdr_rect->node, 0, hy);
        wlr_scene_rect_set_color(ov->hdr_rect, c);
    }
    if (ov->hdr_line) {
        float c[4];
        premul_rgba(accent, ha.alpha, c);
        wlr_scene_node_set_position(&ov->hdr_line->node, 0, hy + ov->hdr_h - 1);
        wlr_scene_rect_set_color(ov->hdr_line, c);
    }
    if (ov->hdr_title_buf) {
        wlr_scene_node_set_position(&ov->hdr_title_buf->node, 0, hy);
        wlr_scene_buffer_set_opacity(ov->hdr_title_buf, ha.alpha);
    }
    if (ov->hdr_hint_buf) {
        wlr_scene_node_set_position(&ov->hdr_hint_buf->node, ov->hdr_w - (ov->hdr_w / 2), hy);
        wlr_scene_buffer_set_opacity(ov->hdr_hint_buf, ha.alpha);
    }

    const CompOverviewWsBand *anchor_band =
        (ov->anchor_ws < ov->scene_geom.n_bands)
            ? &ov->scene_geom.bands[ov->anchor_ws]
            : (ov->scene_geom.n_bands > 0 ? &ov->scene_geom.bands[0] : NULL);

    CompOverviewBandAnim band_anims[OVERVIEW_MAX_BANDS];
    memset(band_anims, 0, sizeof(band_anims));

    for (size_t i = 0; i < ov->n_band_nodes; i++) {
        OverviewBandNode *bn = &ov->band_nodes[i];
        const CompOverviewWsBand *b = &ov->scene_geom.bands[bn->band_idx];
        CompOverviewBandAnim ba = {0};
        comp_overview_interp_band(t, out_w, out_h, ov->anchor_ws, anchor_band, b, &ba);
        if (bn->ws_index < OVERVIEW_MAX_BANDS)
            band_anims[bn->ws_index] = ba;

        int bx = (int)lround(ba.rect.x);
        int by = (int)lround(ba.rect.y);
        int bw = (int)lround(ba.rect.w);
        int bh = (int)lround(ba.rect.h);
        if (bw < 1) bw = 1;
        if (bh < 1) bh = 1;

        if (bn->bg) {
            float c[4];
            premul_rgba(k_band_bg_rgba, ba.alpha, c);
            wlr_scene_node_set_position(&bn->bg->node, bx, by);
            wlr_scene_rect_set_size(bn->bg, bw, bh);
            wlr_scene_rect_set_color(bn->bg, c);
        }
        {
            float c[4];
            premul_rgba(bn->is_selected ? accent : k_border_dim_rgba, ba.alpha, c);
            update_border_rects(bn->border, bx, by, bw, bh, bn->is_selected ? 2 : 1, c);
        }
        if (bn->badge_buf) {
            wlr_scene_node_set_position(&bn->badge_buf->node, bx + 8, by + 4);
            wlr_scene_buffer_set_opacity(bn->badge_buf, ba.badge_alpha);
        }
    }

    for (size_t i = 0; i < ov->n_card_nodes; i++) {
        OverviewCardNode *cn = &ov->card_nodes[i];
        const CompOverviewCard *c = &ov->scene_geom.cards[cn->card_idx];
        const CompOverviewWsBand *parent_b =
            (cn->ws_index < ov->scene_geom.n_bands) ? &ov->scene_geom.bands[cn->ws_index] : NULL;
        const CompOverviewBandAnim *parent_ba =
            (cn->ws_index < OVERVIEW_MAX_BANDS) ? &band_anims[cn->ws_index] : NULL;

        CompOverviewCardAnim ca = {0};
        comp_overview_interp_card(t, out_w, out_h, ov->anchor_ws, ov->close_bounce_gain,
                                  parent_b, parent_ba, c,
                                  cn->has_live_rect, cn->live_rect, &ca);

        int cx = (int)lround(ca.card.x);
        int cy = (int)lround(ca.card.y);
        int cw = (int)lround(ca.card.w);
        int ch = (int)lround(ca.card.h);
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;

        if (cn->bg) {
            float col[4];
            premul_rgba(k_card_bg_rgba, ca.card_alpha, col);
            wlr_scene_node_set_position(&cn->bg->node, cx, cy);
            wlr_scene_rect_set_size(cn->bg, cw, ch);
            wlr_scene_rect_set_color(cn->bg, col);
        }
        if (cn->mirror) {
            cn->mirror->slot    = ca.thumb;
            cn->mirror->opacity = ca.thumb_alpha;
            update_overview_mirror(cn->mirror);
        }
        if (cn->caption_buf) {
            int cap_w = (int)lround(ca.caption.w);
            int cap_h = (int)lround(ca.caption.h);
            if (cap_w < 1) cap_w = 1;
            if (cap_h < 1) cap_h = 1;
            wlr_scene_node_set_position(&cn->caption_buf->node,
                                        (int)lround(ca.caption.x), (int)lround(ca.caption.y));
            wlr_scene_buffer_set_dest_size(cn->caption_buf, cap_w, cap_h);
            wlr_scene_buffer_set_opacity(cn->caption_buf, ca.chrome_alpha);
        }
        {
            float col[4];
            premul_rgba(cn->is_selected ? accent : k_border_dim_rgba, ca.chrome_alpha, col);
            update_border_rects(cn->border, cx, cy, cw, ch, cn->is_selected ? 2 : 1, col);
        }
    }
}

static void
overview_rebuild_scene(CompOverview *ov)
{
    CompServer *s = ov ? ov->server : NULL;
    if (!ov || !s || !s->scene || !s->tree_overlay || !s->layout)
        return;

    overview_destroy_scene(ov);
    if (ov->state == COMP_OVERVIEW_CLOSED)
        return;

    double out_w = 0.0, out_h = 0.0;
    comp_server_viewport_size(s, &out_w, &out_h);
    if (out_w < 64.0 || out_h < 64.0)
        return;

    size_t n_ws = comp_layout_workspace_count(s->layout);
    if (n_ws == 0)
        return;
    if (ov->selected_ws >= n_ws)
        ov->selected_ws = n_ws - 1;

    comp_overview_compute(s->layout, out_w, out_h, ov->scroll_y,
                          ov->selected_ws, ov->selected_win,
                          ov->bands, OVERVIEW_MAX_BANDS,
                          ov->cards, OVERVIEW_MAX_CARDS,
                          &ov->scene_geom);
    double clamped_sy = comp_overview_ensure_ws_visible(&ov->scene_geom, out_h, ov->scroll_y, ov->selected_ws);
    if (fabs(clamped_sy - ov->scroll_y) > 0.5) {
        ov->scroll_y = clamped_sy;
        comp_overview_compute(s->layout, out_w, out_h, ov->scroll_y,
                              ov->selected_ws, ov->selected_win,
                              ov->bands, OVERVIEW_MAX_BANDS,
                              ov->cards, OVERVIEW_MAX_CARDS,
                              &ov->scene_geom);
    }

    ov->tree = wlr_scene_tree_create(s->tree_overlay);
    if (!ov->tree)
        return;
    ov->mirrors_ctx = talloc_new(ov);

    int iw = (int)out_w;
    int ih = (int)out_h;
    double scale = primary_output_scale(s);
    const float *accent = accent_rgba(s);

    float initial_bg[4] = {
        k_backdrop_rgb[0] * 0.92f,
        k_backdrop_rgb[1] * 0.92f,
        k_backdrop_rgb[2] * 0.92f,
        0.92f,
    };
    ov->bg_rect = wlr_scene_rect_create(ov->tree, iw, ih, initial_bg);
    if (ov->bg_rect)
        wlr_scene_node_set_position(&ov->bg_rect->node, 0, 0);

    overview_build_header(ov, iw, n_ws, scale, accent);
    overview_build_bands(ov, out_h, scale, accent);
    overview_build_cards(ov, out_h, scale, accent);

    overview_apply_frame(ov, ov->progress);
    comp_server_schedule_frames(s);
}

static void
overview_start_segment(CompOverview *ov, double to_target, double now_s)
{
    CompServer *s = ov ? ov->server : NULL;
    if (!ov || !s)
        return;

    bool enabled = !s->config || s->config->motion.enabled;
    double slowdown = (s->config && s->config->motion.slowdown >= 0.0) ? s->config->motion.slowdown : 1.0;
    CompOverviewAnimConfig anim;
    if (s->config)
        anim = s->config->overview_anim;
    else
        comp_overview_anim_config_defaults(&anim);

    if (!enabled || slowdown <= 1e-6 || anim.mode == COMP_OVERVIEW_ANIM_INSTANT) {
        ov->progress = to_target;
        ov->velocity = 0.0;
        ov->target   = to_target;
        if (to_target >= 0.5) {
            ov->state = COMP_OVERVIEW_OPEN;
            overview_apply_frame(ov, 1.0);
        } else {
            ov->state = COMP_OVERVIEW_CLOSED;
            overview_destroy_scene(ov);
        }
        comp_server_schedule_frames(s);
        return;
    }

    ov->target            = to_target;
    ov->active_mode       = anim.mode;
    ov->active_curve      = anim.curve;
    ov->close_bounce_gain = anim.close_bounce_gain;

    if (anim.mode == COMP_OVERVIEW_ANIM_SPRING) {
        CompSpringParams sp = anim.spring;
        if (fabs(slowdown - 1.0) > 1e-9)
            sp.stiffness = sp.stiffness / (slowdown * slowdown);
        comp_spring_launch(&ov->spring, &sp, ov->progress, to_target, ov->velocity, now_s);
    } else {
        double dur = (double)anim.duration_ms / 1000.0;
        if (dur < 0.01)
            dur = 0.01;
        ov->curve_from  = ov->progress;
        ov->curve_to    = to_target;
        ov->curve_start = now_s;
        ov->curve_dur_s = dur * slowdown;
    }

    ov->state = (to_target >= 0.5) ? COMP_OVERVIEW_OPENING : COMP_OVERVIEW_CLOSING;
    overview_apply_frame(ov, ov->progress);
    comp_server_schedule_frames(s);
}

static CompWinId
first_win_on_ws(const CompOverview *ov, size_t ws)
{
    for (size_t i = 0; i < ov->scene_geom.n_cards; i++) {
        if (ov->scene_geom.cards[i].ws_index == ws)
            return ov->scene_geom.cards[i].id;
    }
    return 0;
}

void
comp_overview_init(CompServer *s)
{
    if (!s || s->overview)
        return;
    CompOverview *ov = talloc_zero(s, CompOverview);
    COMP_WARN_RETURN_VOID(ov != NULL);
    talloc_set_name_const(ov, "CompOverview");
    ov->server = s;
    ov->state  = COMP_OVERVIEW_CLOSED;
    talloc_set_destructor(ov, overview_destructor);
    s->overview = ov;
}

bool
comp_overview_is_active(const CompServer *s)
{
    return s && s->overview &&
           (s->overview->state == COMP_OVERVIEW_OPENING || s->overview->state == COMP_OVERVIEW_OPEN);
}

bool
comp_overview_is_animating(const CompServer *s)
{
    return s && s->overview &&
           (s->overview->state == COMP_OVERVIEW_OPENING || s->overview->state == COMP_OVERVIEW_CLOSING);
}

void
comp_overview_open(CompServer *s)
{
    if (!s || s->locked || s->opts.greeter_mode)
        return;
    if (!s->overview)
        comp_overview_init(s);
    CompOverview *ov = s->overview;
    if (!ov || ov->state == COMP_OVERVIEW_OPENING || ov->state == COMP_OVERVIEW_OPEN)
        return;

    if (comp_switcher_is_active(s))
        comp_switcher_cancel(s);
    if (s->grab_win != 0)
        comp_window_end_grab(s);

    double now_s = comp_now_sec();
    if (ov->state == COMP_OVERVIEW_CLOSED) {
        ov->initial_ws   = comp_layout_active_workspace(s->layout);
        ov->initial_win  = comp_layout_focused_window(s->layout);
        ov->selected_ws  = ov->initial_ws;
        ov->selected_win = ov->initial_win;
        ov->anchor_ws    = ov->initial_ws;
        ov->scroll_y     = 0.0;
        ov->progress     = 0.0;
        ov->velocity     = 0.0;
        ov->state        = COMP_OVERVIEW_OPENING;

        overview_capture_live_rects(ov, now_s);
        overview_rebuild_scene(ov);
    }

    if (s->seat)
        wlr_seat_pointer_clear_focus(s->seat);
    if (s->cursor && s->cursor_mgr)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");

    overview_start_segment(ov, 1.0, now_s);
}

void
comp_overview_close(CompServer *s, bool commit)
{
    if (!s || !s->overview || s->overview->state == COMP_OVERVIEW_CLOSED)
        return;
    CompOverview *ov = s->overview;
    if (ov->state == COMP_OVERVIEW_CLOSING && !commit)
        return;

    size_t target_ws = commit ? ov->selected_ws : ov->initial_ws;
    CompWinId target_win = commit ? ov->selected_win : ov->initial_win;
    double now_s = comp_now_sec();

    if (commit && s->layout) {
        bool changed = false;
        if (target_win != 0 && comp_layout_contains(s->layout, target_win)) {
            changed = comp_layout_focus_window(s->layout, target_win);
        } else if (target_ws < comp_layout_workspace_count(s->layout)) {
            changed = comp_layout_focus_workspace_index(s->layout, target_ws);
        }
        if (changed) {
            comp_window_relayout(s);
            comp_ipc_emit_workspaces(s);
            comp_ipc_emit_windows(s);
        } else {
            comp_server_restore_keyboard_focus(s);
        }
        ov->anchor_ws = target_ws;
        overview_capture_live_rects(ov, now_s);
        refresh_card_live_rects(ov);
    } else {
        comp_server_restore_keyboard_focus(s);
        ov->anchor_ws = ov->initial_ws;
    }

    comp_input_refresh_pointer_focus(s, (uint32_t)(now_s * 1000.0));
    overview_start_segment(ov, 0.0, now_s);
}

void
comp_overview_close_immediate(CompServer *s)
{
    if (!s || !s->overview)
        return;
    CompOverview *ov = s->overview;
    ov->state    = COMP_OVERVIEW_CLOSED;
    ov->progress = 0.0;
    ov->velocity = 0.0;
    ov->target   = 0.0;
    overview_destroy_scene(ov);
    comp_server_restore_keyboard_focus(s);
    comp_input_refresh_pointer_focus(s, (uint32_t)(comp_now_sec() * 1000.0));
}

void
comp_overview_toggle(CompServer *s)
{
    if (!s)
        return;
    if (comp_overview_is_active(s))
        comp_overview_close(s, false);
    else
        comp_overview_open(s);
}

bool
comp_overview_tick_frame(CompServer *s, double now_s)
{
    if (!s || !s->overview)
        return false;
    CompOverview *ov = s->overview;
    if (ov->state != COMP_OVERVIEW_OPENING && ov->state != COMP_OVERVIEW_CLOSING)
        return false;

    bool done = false;
    if (ov->active_mode == COMP_OVERVIEW_ANIM_SPRING) {
        ov->progress = comp_spring_value(&ov->spring, now_s);
        ov->velocity = comp_spring_velocity(&ov->spring, now_s);
        done = comp_spring_is_done(&ov->spring, now_s);
        if (ov->state == COMP_OVERVIEW_CLOSING &&
            ov->close_bounce_gain <= 0.001 &&
            ov->progress <= 0.0) {
            done = true;
        }
    } else if (ov->active_mode == COMP_OVERVIEW_ANIM_CURVE) {
        double dt = now_s - ov->curve_start;
        double u  = (ov->curve_dur_s > 0.0) ? (dt / ov->curve_dur_s) : 1.0;
        if (u < 0.0) u = 0.0;
        if (u >= 1.0) {
            u    = 1.0;
            done = true;
        }
        double k = comp_overview_eval_curve(ov->active_curve, u);
        double span = ov->curve_to - ov->curve_from;
        ov->progress = ov->curve_from + span * k;
        ov->velocity = done ? 0.0 : span * comp_overview_eval_curve_deriv(ov->active_curve, u, ov->curve_dur_s);
        if (ov->state == COMP_OVERVIEW_CLOSING &&
            ov->close_bounce_gain <= 0.001 &&
            ov->progress <= 0.0) {
            done = true;
        }
    } else {
        done = true;
    }

    if (done) {
        if (ov->state == COMP_OVERVIEW_OPENING) {
            ov->progress = 1.0;
            ov->velocity = 0.0;
            ov->state    = COMP_OVERVIEW_OPEN;
            overview_apply_frame(ov, 1.0);
        } else {
            ov->progress = 0.0;
            ov->velocity = 0.0;
            ov->state    = COMP_OVERVIEW_CLOSED;
            overview_destroy_scene(ov);
        }
        return false;
    }

    overview_apply_frame(ov, ov->progress);
    return true;
}

void
comp_overview_on_layout_changed(CompServer *s)
{
    if (!s || !s->overview || s->overview->state == COMP_OVERVIEW_CLOSED)
        return;
    CompOverview *ov = s->overview;
    if (ov->selected_win != 0 && !comp_layout_contains(s->layout, ov->selected_win))
        ov->selected_win = 0;
    overview_capture_live_rects(ov, comp_now_sec());
    overview_rebuild_scene(ov);
}

bool
comp_overview_handle_key(CompServer *s, uint32_t mods, uint32_t keysym)
{
    if (!comp_overview_is_active(s))
        return false;
    CompOverview *ov = s->overview;
    (void)mods;

    if (keysym == XKB_KEY_Escape) {
        comp_overview_close(s, false);
        return true;
    }
    if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter || keysym == XKB_KEY_space) {
        comp_overview_close(s, true);
        return true;
    }
    if (keysym == XKB_KEY_q || keysym == XKB_KEY_Delete || keysym == XKB_KEY_BackSpace) {
        if (ov->selected_win != 0) {
            comp_window_close_id(s, ov->selected_win);
            overview_rebuild_scene(ov);
        }
        return true;
    }
    if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
        size_t idx = (size_t)(keysym - XKB_KEY_1);
        if (idx < comp_layout_workspace_count(s->layout)) {
            ov->selected_ws  = idx;
            ov->selected_win = first_win_on_ws(ov, idx);
            overview_rebuild_scene(ov);
        }
        return true;
    }

    size_t n_ws = comp_layout_workspace_count(s->layout);
    if (keysym == XKB_KEY_Up || keysym == XKB_KEY_k) {
        if (ov->selected_ws > 0) {
            ov->selected_ws--;
            ov->selected_win = first_win_on_ws(ov, ov->selected_ws);
            overview_rebuild_scene(ov);
        }
        return true;
    }
    if (keysym == XKB_KEY_Down || keysym == XKB_KEY_j) {
        if (ov->selected_ws + 1 < n_ws) {
            ov->selected_ws++;
            ov->selected_win = first_win_on_ws(ov, ov->selected_ws);
            overview_rebuild_scene(ov);
        }
        return true;
    }
    if (keysym == XKB_KEY_Left || keysym == XKB_KEY_h ||
        keysym == XKB_KEY_Right || keysym == XKB_KEY_l) {
        bool right = (keysym == XKB_KEY_Right || keysym == XKB_KEY_l);
        CompWinId ws_ids[OVERVIEW_MAX_CARDS];
        size_t n = 0;
        size_t cur = 0;
        bool found = false;
        for (size_t i = 0; i < ov->scene_geom.n_cards && n < OVERVIEW_MAX_CARDS; i++) {
            if (ov->scene_geom.cards[i].ws_index == ov->selected_ws) {
                if (ov->scene_geom.cards[i].id == ov->selected_win) {
                    cur = n;
                    found = true;
                }
                ws_ids[n++] = ov->scene_geom.cards[i].id;
            }
        }
        if (n > 0) {
            if (!found)
                ov->selected_win = ws_ids[0];
            else if (right && cur + 1 < n)
                ov->selected_win = ws_ids[cur + 1];
            else if (!right && cur > 0)
                ov->selected_win = ws_ids[cur - 1];
            overview_rebuild_scene(ov);
        }
        return true;
    }
    return true; /* consume other keys while overview is open */
}

bool
comp_overview_handle_motion(CompServer *s, double lx, double ly)
{
    if (!comp_overview_is_active(s))
        return false;
    CompOverview *ov = s->overview;
    size_t hit_ws = ov->selected_ws;
    CompWinId hit_win = ov->selected_win;
    if (comp_overview_hit_test(&ov->scene_geom, lx, ly, &hit_ws, &hit_win)) {
        if (hit_ws != ov->selected_ws || hit_win != ov->selected_win) {
            ov->selected_ws  = hit_ws;
            ov->selected_win = hit_win;
            overview_rebuild_scene(ov);
        }
    }
    return true;
}

bool
comp_overview_handle_button(CompServer *s, uint32_t button, bool pressed, double lx, double ly)
{
    if (!comp_overview_is_active(s))
        return false;
    if (!pressed)
        return true;
    CompOverview *ov = s->overview;
    size_t hit_ws = 0;
    CompWinId hit_win = 0;
    if (!comp_overview_hit_test(&ov->scene_geom, lx, ly, &hit_ws, &hit_win)) {
        if (button == BTN_LEFT || button == BTN_RIGHT)
            comp_overview_close(s, false);
        return true;
    }

    if (button == BTN_MIDDLE && hit_win != 0) {
        comp_window_close_id(s, hit_win);
        overview_rebuild_scene(ov);
        return true;
    }
    if (button == BTN_LEFT) {
        ov->selected_ws  = hit_ws;
        ov->selected_win = hit_win;
        comp_overview_close(s, true);
        return true;
    }
    if (button == BTN_RIGHT) {
        comp_overview_close(s, false);
        return true;
    }
    return true;
}

bool
comp_overview_handle_axis(CompServer *s, double delta_y)
{
    if (!comp_overview_is_active(s))
        return false;
    CompOverview *ov = s->overview;
    if (fabs(delta_y) < 1e-4)
        return true;
    ov->scroll_y += delta_y * 3.0;
    if (ov->scroll_y < 0.0)
        ov->scroll_y = 0.0;
    if (ov->scroll_y > ov->scene_geom.max_scroll_y)
        ov->scroll_y = ov->scene_geom.max_scroll_y;
    overview_rebuild_scene(ov);
    return true;
}
