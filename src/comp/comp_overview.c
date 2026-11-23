/* comp_overview.c — Zoomed-out workspace & scrolling-strip overview in s->tree_overlay.
 *
 * Shows all workspaces stacked vertically with live bilinear-scaled mirrors of every
 * window, Aero+Metro workspace bands, active accent framing, and full keyboard/pointer
 * navigation (click or Enter to focus & exit, middle-click or q/Delete to close,
 * arrows/hjkl/wheel to navigate, Escape or toggle-overview to close).
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

static const float k_backdrop_rgba[4]     = { 0.039216f, 0.039216f, 0.039216f, 0.92f }; /* #0a0a0a @ 0.92 */
static const float k_header_bg_rgba[4]    = { 0.078431f, 0.078431f, 0.078431f, 1.0f };  /* #141414 */
static const float k_band_bg_rgba[4]      = { 0.070588f, 0.070588f, 0.070588f, 1.0f };  /* #121212 */
static const float k_card_bg_rgba[4]      = { 0.094118f, 0.094118f, 0.094118f, 1.0f };  /* #181818 */
static const float k_border_dim_rgba[4]   = { 0.168627f, 0.168627f, 0.168627f, 1.0f };  /* #2b2b2b */
static const float k_fg_primary_rgba[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };                 /* #ffffff */
static const float k_fg_secondary_rgba[4] = { 0.627451f, 0.627451f, 0.627451f, 1.0f };  /* #a0a0a0 */
static const float k_default_accent[4]    = { 0.0f, 0.470588f, 0.831373f, 1.0f };       /* #0078d4 */
static const float k_transparent_rgba[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };

typedef struct {
    CompServer              *server;
    CompWinId                win_id;
    struct wlr_surface      *surface;
    struct wlr_scene_buffer *thumb_buf;
    CompRect                 slot;
} OverviewMirror;

typedef struct CompOverview {
    CompServer            *server;
    bool                   active;
    size_t                 selected_ws;
    CompWinId              selected_win;
    size_t                 initial_ws;
    CompWinId              initial_win;
    double                 scroll_y;

    CompOverviewWsBand     bands[OVERVIEW_MAX_BANDS];
    CompOverviewCard       cards[OVERVIEW_MAX_CARDS];
    CompOverviewScene      scene_geom;

    struct wlr_scene_tree *tree;
    void                  *mirrors_ctx;
} CompOverview;

static int
overview_destructor(CompOverview *ov)
{
    if (ov->server && ov->server->scene && ov->tree) {
        wlr_scene_node_destroy(&ov->tree->node);
        ov->tree = NULL;
    }
    return 0;
}

static void
add_border(struct wlr_scene_tree *parent, int x, int y, int w, int h, int b, const float rgba[4])
{
    if (b <= 0 || w <= 0 || h <= 0)
        return;
    struct wlr_scene_rect *t = wlr_scene_rect_create(parent, w, b, rgba);
    struct wlr_scene_rect *d = wlr_scene_rect_create(parent, w, b, rgba);
    struct wlr_scene_rect *l = wlr_scene_rect_create(parent, b, h > 2 * b ? h - 2 * b : 1, rgba);
    struct wlr_scene_rect *r = wlr_scene_rect_create(parent, b, h > 2 * b ? h - 2 * b : 1, rgba);
    if (t) wlr_scene_node_set_position(&t->node, x, y);
    if (d) wlr_scene_node_set_position(&d->node, x, y + h - b);
    if (l) wlr_scene_node_set_position(&l->node, x, y + b);
    if (r) wlr_scene_node_set_position(&r->node, x + w - b, y + b);
}

static void
update_overview_mirror(OverviewMirror *m)
{
    if (!m || !m->thumb_buf || !m->surface)
        return;
    struct wlr_buffer *buf = m->surface->buffer ? &m->surface->buffer->base : NULL;
    wlr_scene_buffer_set_buffer(m->thumb_buf, buf);
    if (!buf || buf->width <= 0 || buf->height <= 0)
        return;

    double sw = (double)m->surface->current.width;
    double sh = (double)m->surface->current.height;
    if (sw <= 0.0 || sh <= 0.0) {
        sw = (double)buf->width;
        sh = (double)buf->height;
    }
    double scale = fmin(m->slot.w / sw, m->slot.h / sh);
    int dw = (int)lround(sw * scale);
    int dh = (int)lround(sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int)m->slot.w) dw = (int)m->slot.w;
    if (dh > (int)m->slot.h) dh = (int)m->slot.h;

    int tx = (int)m->slot.x + ((int)m->slot.w - dw) / 2;
    int ty = (int)m->slot.y + ((int)m->slot.h - dh) / 2;
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

static void
overview_place_label(struct wlr_scene_tree *tree,
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
        return;
    struct wlr_scene_buffer *sb = wlr_scene_buffer_create(tree, NULL);
    if (sb) {
        wlr_scene_node_set_position(&sb->node, x, y);
        comp_scene_buffer_set_owned_buffer(sb, buf);
        wlr_scene_buffer_set_dest_size(sb, w, h);
    } else {
        wlr_buffer_drop(buf);
    }
}

static void
overview_build_header(CompOverview *ov, int iw, size_t n_ws, double scale, const float *accent)
{
    int hdr_h = (int)COMP_OVERVIEW_HEADER_H;
    struct wlr_scene_rect *hdr = wlr_scene_rect_create(ov->tree, iw, hdr_h, k_header_bg_rgba);
    if (hdr)
        wlr_scene_node_set_position(&hdr->node, 0, 0);
    struct wlr_scene_rect *hdr_line = wlr_scene_rect_create(ov->tree, iw, 1, accent);
    if (hdr_line)
        wlr_scene_node_set_position(&hdr_line->node, 0, hdr_h - 1);

    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf),
             "OVERVIEW  •  %zu WORKSPACE%s  •  %zu WINDOW%s",
             n_ws, n_ws == 1 ? "" : "S",
             ov->scene_geom.n_cards, ov->scene_geom.n_cards == 1 ? "" : "S");
    overview_place_label(ov->tree, title_buf, 0, 0, iw / 2, hdr_h - 2, scale,
                         k_fg_primary_rgba, k_transparent_rgba, 16);

    const char *hint = "CLICK/ENTER FOCUS  •  ARROWS/WHEEL NAV  •  MIDDLE-CLICK CLOSE  •  ESC EXIT";
    int hint_w = iw / 2;
    overview_place_label(ov->tree, hint, iw - hint_w, 0, hint_w, hdr_h - 2, scale,
                         k_fg_secondary_rgba, k_transparent_rgba, 16);
}

static void
overview_build_bands(CompOverview *ov, double out_h, double scale, const float *accent)
{
    for (size_t i = 0; i < ov->scene_geom.n_bands; i++) {
        const CompOverviewWsBand *b = &ov->scene_geom.bands[i];
        if (b->band.y + b->band.h <= COMP_OVERVIEW_HEADER_H || b->band.y >= out_h)
            continue;

        int bx = (int)b->band.x;
        int by = (int)b->band.y;
        int bw = (int)b->band.w;
        int bh = (int)b->band.h;

        struct wlr_scene_rect *brect = wlr_scene_rect_create(ov->tree, bw, bh, k_band_bg_rgba);
        if (brect)
            wlr_scene_node_set_position(&brect->node, bx, by);
        add_border(ov->tree, bx, by, bw, bh, b->is_selected ? 2 : 1,
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

    for (size_t i = 0; i < ov->scene_geom.n_cards; i++) {
        const CompOverviewCard *c = &ov->scene_geom.cards[i];
        if (c->card.y + c->card.h <= COMP_OVERVIEW_HEADER_H || c->card.y >= out_h)
            continue;

        int cx = (int)c->card.x;
        int cy = (int)c->card.y;
        int cw = (int)c->card.w;
        int ch = (int)c->card.h;

        struct wlr_scene_rect *crect = wlr_scene_rect_create(ov->tree, cw, ch, k_card_bg_rgba);
        if (crect)
            wlr_scene_node_set_position(&crect->node, cx, cy);

        struct CompWindow *win = comp_win_from_id(s, c->id);
        struct wlr_surface *surf = win ? comp_window_get_surface(win) : NULL;
        if (surf && ov->mirrors_ctx) {
            OverviewMirror *m = talloc_zero(ov->mirrors_ctx, OverviewMirror);
            if (m) {
                m->server    = s;
                m->win_id    = c->id;
                m->surface   = surf;
                m->slot      = c->thumb;
                m->thumb_buf = wlr_scene_buffer_create(ov->tree, NULL);
                if (m->thumb_buf) {
                    update_overview_mirror(m);
                    comp_listen(m, &surf->events.commit, handle_mirror_commit);
                }
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
            overview_place_label(ov->tree, title, (int)c->caption.x, (int)c->caption.y,
                                 cap_w, cap_h, scale,
                                 c->is_selected ? k_fg_primary_rgba : k_fg_secondary_rgba,
                                 k_header_bg_rgba, 6);
        }

        add_border(ov->tree, cx, cy, cw, ch, c->is_selected ? 2 : 1,
                   c->is_selected ? accent : k_border_dim_rgba);
    }
}

static void
overview_rebuild_scene(CompOverview *ov)
{
    CompServer *s = ov ? ov->server : NULL;
    if (!ov || !s || !s->scene || !s->tree_overlay || !s->layout)
        return;

    talloc_free(ov->mirrors_ctx);
    ov->mirrors_ctx = NULL;
    if (ov->tree) {
        wlr_scene_node_destroy(&ov->tree->node);
        ov->tree = NULL;
    }
    if (!ov->active)
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

    struct wlr_scene_rect *bg = wlr_scene_rect_create(ov->tree, iw, ih, k_backdrop_rgba);
    if (bg)
        wlr_scene_node_set_position(&bg->node, 0, 0);

    overview_build_header(ov, iw, n_ws, scale, accent);
    overview_build_bands(ov, out_h, scale, accent);
    overview_build_cards(ov, out_h, scale, accent);

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
    talloc_set_destructor(ov, overview_destructor);
    s->overview = ov;
}

bool
comp_overview_is_active(const CompServer *s)
{
    return s && s->overview && s->overview->active;
}

void
comp_overview_open(CompServer *s)
{
    if (!s || s->locked || s->opts.greeter_mode)
        return;
    if (!s->overview)
        comp_overview_init(s);
    CompOverview *ov = s->overview;
    if (!ov || ov->active)
        return;

    if (comp_switcher_is_active(s))
        comp_switcher_cancel(s);
    if (s->grab_win != 0)
        comp_window_end_grab(s);

    ov->active       = true;
    ov->initial_ws   = comp_layout_active_workspace(s->layout);
    ov->initial_win  = comp_layout_focused_window(s->layout);
    ov->selected_ws  = ov->initial_ws;
    ov->selected_win = ov->initial_win;
    ov->scroll_y     = 0.0;

    if (s->seat)
        wlr_seat_pointer_clear_focus(s->seat);
    if (s->cursor && s->cursor_mgr)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");

    overview_rebuild_scene(ov);
}

void
comp_overview_close(CompServer *s, bool commit)
{
    if (!s || !s->overview || !s->overview->active)
        return;
    CompOverview *ov = s->overview;
    size_t target_ws = commit ? ov->selected_ws : ov->initial_ws;
    CompWinId target_win = commit ? ov->selected_win : ov->initial_win;

    ov->active = false;
    talloc_free(ov->mirrors_ctx);
    ov->mirrors_ctx = NULL;
    if (ov->tree) {
        wlr_scene_node_destroy(&ov->tree->node);
        ov->tree = NULL;
    }

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
    } else {
        comp_server_restore_keyboard_focus(s);
    }

    comp_input_refresh_pointer_focus(s, (uint32_t)(comp_now_sec() * 1000.0));
    comp_server_schedule_frames(s);
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

void
comp_overview_on_layout_changed(CompServer *s)
{
    if (!comp_overview_is_active(s))
        return;
    CompOverview *ov = s->overview;
    if (ov->selected_win != 0 && !comp_layout_contains(s->layout, ov->selected_win))
        ov->selected_win = 0;
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
