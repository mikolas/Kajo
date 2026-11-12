/* comp_switcher.c — In-compositor Alt-Tab recent-windows switcher (Phase 3, spec §4.1).
 *
 * Renders in s->tree_overlay:
 *   - Container: #111111 @ 0.94 alpha, 1px solid #2b2b2b border, 16px outer padding,
 *     12px horizontal card gap, centered on the active output.
 *   - Cards: 220x140 logical px (#181818 fill, 0px radius), 2px active border on
 *     selected card (switcher_active_rgba or switcher_urgent_rgba), 1px #2b2b2b
 *     border on unselected cards, 204x108 aspect-preserving live mirror
 *     wlr_scene_buffer of the client surface, and 24px pangocairo title strip
 *     (Fira Code 10pt, #ffffff selected, #aaaaaa unselected).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <talloc.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define SWITCHER_PAD_OUTER   16
#define SWITCHER_CARD_GAP    12
#define SWITCHER_CARD_W     220
#define SWITCHER_CARD_H     140
#define SWITCHER_THUMB_X      8
#define SWITCHER_THUMB_Y      6
#define SWITCHER_THUMB_W    204
#define SWITCHER_THUMB_H    108
#define SWITCHER_TITLE_X      8
#define SWITCHER_TITLE_Y    114
#define SWITCHER_TITLE_W    204
#define SWITCHER_TITLE_H     24
#define SWITCHER_MAX_VIS      7

/* Premultiplied colors */
static const float k_container_bg[4]     = { 0.062667f, 0.062667f, 0.062667f, 0.94f }; /* #111111 @ 0.94 */
static const float k_container_border[4] = { 0.168627f, 0.168627f, 0.168627f, 1.0f };  /* #2b2b2b */
static const float k_card_bg[4]          = { 0.094118f, 0.094118f, 0.094118f, 1.0f };  /* #181818 */
static const float k_thumb_bg[4]         = { 0.066667f, 0.066667f, 0.066667f, 1.0f };  /* #111111 */
static const float k_title_sel[4]        = { 1.0f, 1.0f, 1.0f, 1.0f };                 /* #ffffff */
static const float k_title_unsel[4]      = { 0.666667f, 0.666667f, 0.666667f, 1.0f };  /* #aaaaaa */
static const float k_default_active[4]   = { 1.0f, 0.2f, 0.4f, 1.0f };                 /* #ff3366 */
static const float k_default_urgent[4]   = { 0.909804f, 0.066667f, 0.137255f, 1.0f };  /* #e81123 */

typedef struct {
    CompServer              *server;
    CompWinId                win_id;
    struct wlr_surface      *surface;
    struct wlr_scene_buffer *thumb_buf;
    int                      slot_x;
    int                      slot_y;
} SwitcherCardMirror;

typedef struct CompSwitcher {
    CompServer            *server;
    bool                   active;
    uint32_t               hold_mods;          /* modifier mask that keeps switcher open */
    CompSwitchScope        scope;
    bool                   same_app;
    char                  *filter_app_id;
    CompWinId              initial_focused_id;

    CompWindowInfo        *items;
    size_t                 n_items;
    size_t                 selected_idx;
    size_t                 scroll_offset;

    /* Scene nodes inside s->tree_overlay */
    struct wlr_scene_tree *tree;
    int                    cont_x, cont_y, cont_w, cont_h;
    size_t                 vis_count;
    void                  *mirrors_ctx;
} CompSwitcher;

static int
switcher_destructor(CompSwitcher *sw)
{
    if (sw->server && sw->server->scene && sw->tree) {
        wlr_scene_node_destroy(&sw->tree->node);
        sw->tree = NULL;
    }
    return 0;
}

static void
update_mirror_buffer(SwitcherCardMirror *m)
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

    double scale = fmin((double)SWITCHER_THUMB_W / sw, (double)SWITCHER_THUMB_H / sh);
    int dw = (int)lround(sw * scale);
    int dh = (int)lround(sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > SWITCHER_THUMB_W) dw = SWITCHER_THUMB_W;
    if (dh > SWITCHER_THUMB_H) dh = SWITCHER_THUMB_H;

    int tx = m->slot_x + (SWITCHER_THUMB_W - dw) / 2;
    int ty = m->slot_y + (SWITCHER_THUMB_H - dh) / 2;

    wlr_scene_node_set_position(&m->thumb_buf->node, tx, ty);
    wlr_scene_buffer_set_dest_size(m->thumb_buf, dw, dh);
    wlr_scene_buffer_set_transform(m->thumb_buf, m->surface->current.transform);
    wlr_scene_buffer_set_filter_mode(m->thumb_buf, WLR_SCALE_FILTER_BILINEAR);
}

static void
handle_mirror_surface_commit(void *owner, void *data)
{
    SwitcherCardMirror *m = owner;
    (void)data;
    update_mirror_buffer(m);
}

static void
add_border_rects(struct wlr_scene_tree *parent, int x, int y, int w, int h,
                 int b, const float rgba[4])
{
    if (b <= 0 || w <= 0 || h <= 0)
        return;
    struct wlr_scene_rect *top    = wlr_scene_rect_create(parent, w, b, rgba);
    struct wlr_scene_rect *bottom = wlr_scene_rect_create(parent, w, b, rgba);
    struct wlr_scene_rect *left   = wlr_scene_rect_create(parent, b, h > 2 * b ? h - 2 * b : 1, rgba);
    struct wlr_scene_rect *right  = wlr_scene_rect_create(parent, b, h > 2 * b ? h - 2 * b : 1, rgba);
    if (top)    wlr_scene_node_set_position(&top->node, x, y);
    if (bottom) wlr_scene_node_set_position(&bottom->node, x, y + h - b);
    if (left)   wlr_scene_node_set_position(&left->node, x, y + b);
    if (right)  wlr_scene_node_set_position(&right->node, x + w - b, y + b);
}

static void
switcher_rebuild_scene(CompSwitcher *sw)
{
    CompServer *s = sw->server;
    if (!s || !s->scene || !s->tree_overlay)
        return;

    talloc_free(sw->mirrors_ctx);
    sw->mirrors_ctx = NULL;
    if (sw->tree) {
        wlr_scene_node_destroy(&sw->tree->node);
        sw->tree = NULL;
    }

    if (!sw->active || sw->n_items == 0) {
        comp_server_schedule_frames(s);
        return;
    }

    struct wlr_output *out = comp_server_primary_output(s);
    struct wlr_box obox = { .x = 0, .y = 0, .width = 1920, .height = 1080 };
    double scale = 1.0;
    if (out) {
        wlr_output_layout_get_box(s->output_layout, out, &obox);
        if (obox.width <= 0 || obox.height <= 0) {
            double vw = 1920.0, vh = 1080.0;
            comp_server_viewport_size(s, &vw, &vh);
            obox.width  = (int)vw;
            obox.height = (int)vh;
        }
        if (out->scale > 0.0f)
            scale = (double)out->scale;
    }

    int max_vis = (obox.width - 64) / (SWITCHER_CARD_W + SWITCHER_CARD_GAP);
    if (max_vis < 1)
        max_vis = 1;
    if (max_vis > SWITCHER_MAX_VIS)
        max_vis = SWITCHER_MAX_VIS;

    size_t vis = sw->n_items < (size_t)max_vis ? sw->n_items : (size_t)max_vis;
    sw->vis_count = vis;

    /* Adjust horizontal scroll window to keep selected_idx visible */
    if (sw->selected_idx < sw->scroll_offset)
        sw->scroll_offset = sw->selected_idx;
    if (sw->selected_idx >= sw->scroll_offset + vis)
        sw->scroll_offset = sw->selected_idx - vis + 1;
    if (sw->scroll_offset + vis > sw->n_items)
        sw->scroll_offset = sw->n_items - vis;

    int cont_w = 2 * SWITCHER_PAD_OUTER + (int)vis * SWITCHER_CARD_W +
                 (int)(vis > 1 ? vis - 1 : 0) * SWITCHER_CARD_GAP;
    int cont_h = 2 * SWITCHER_PAD_OUTER + SWITCHER_CARD_H;
    int cont_x = obox.x + (obox.width - cont_w) / 2;
    int cont_y = obox.y + (obox.height - cont_h) / 2;

    sw->cont_x = cont_x;
    sw->cont_y = cont_y;
    sw->cont_w = cont_w;
    sw->cont_h = cont_h;

    sw->tree = wlr_scene_tree_create(s->tree_overlay);
    if (!sw->tree)
        return;
    wlr_scene_node_set_position(&sw->tree->node, cont_x, cont_y);
    wlr_scene_node_raise_to_top(&sw->tree->node);

    /* Container backdrop (#111111 @ 0.94) + 1px #2b2b2b border */
    struct wlr_scene_rect *bg = wlr_scene_rect_create(sw->tree, cont_w, cont_h, k_container_bg);
    if (bg)
        wlr_scene_node_set_position(&bg->node, 0, 0);
    add_border_rects(sw->tree, 0, 0, cont_w, cont_h, 1, k_container_border);

    sw->mirrors_ctx = talloc_new(sw);

    const float *col_active = (s->config) ? s->config->switcher_active_rgba : k_default_active;
    const float *col_urgent = (s->config) ? s->config->switcher_urgent_rgba : k_default_urgent;

    for (size_t i = 0; i < vis; i++) {
        size_t idx = sw->scroll_offset + i;
        const CompWindowInfo *info = &sw->items[idx];
        bool selected = (idx == sw->selected_idx);

        int cx = SWITCHER_PAD_OUTER + (int)i * (SWITCHER_CARD_W + SWITCHER_CARD_GAP);
        int cy = SWITCHER_PAD_OUTER;

        /* Card background (#181818) */
        struct wlr_scene_rect *card_bg =
            wlr_scene_rect_create(sw->tree, SWITCHER_CARD_W, SWITCHER_CARD_H, k_card_bg);
        if (card_bg)
            wlr_scene_node_set_position(&card_bg->node, cx, cy);

        /* Thumbnail slot backdrop (#111111) */
        struct wlr_scene_rect *tbg =
            wlr_scene_rect_create(sw->tree, SWITCHER_THUMB_W, SWITCHER_THUMB_H, k_thumb_bg);
        if (tbg)
            wlr_scene_node_set_position(&tbg->node, cx + SWITCHER_THUMB_X, cy + SWITCHER_THUMB_Y);

        /* Live surface mirror wlr_scene_buffer */
        struct CompWindow *win = comp_win_from_id(s, info->id);
        struct wlr_surface *surf = win ? comp_window_get_surface(win) : NULL;
        if (surf && sw->mirrors_ctx) {
            struct wlr_scene_buffer *tbuf = wlr_scene_buffer_create(sw->tree, NULL);
            SwitcherCardMirror *m = talloc_zero(sw->mirrors_ctx, SwitcherCardMirror);
            if (tbuf && m) {
                m->server    = s;
                m->win_id    = info->id;
                m->surface   = surf;
                m->thumb_buf = tbuf;
                m->slot_x    = cx + SWITCHER_THUMB_X;
                m->slot_y    = cy + SWITCHER_THUMB_Y;
                update_mirror_buffer(m);
                comp_listen(m, &surf->events.commit, handle_mirror_surface_commit);
            }
        }

        /* Title strip (204x24, Fira Code 10pt, #ffffff selected, #aaaaaa unselected) */
        const char *label = (info->title && *info->title)
                                ? info->title
                                : ((info->app_id && *info->app_id) ? info->app_id : "(untitled)");
        struct wlr_buffer *lbuf = comp_text_render_label(
            label, SWITCHER_TITLE_W, SWITCHER_TITLE_H, scale,
            selected ? k_title_sel : k_title_unsel, NULL, NULL, 0, 2);
        if (lbuf) {
            struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(sw->tree, NULL);
            if (sbuf) {
                comp_scene_buffer_set_owned_buffer(sbuf, lbuf);
                wlr_scene_node_set_position(&sbuf->node, cx + SWITCHER_TITLE_X, cy + SWITCHER_TITLE_Y);
                wlr_scene_buffer_set_dest_size(sbuf, SWITCHER_TITLE_W, SWITCHER_TITLE_H);
            }
            wlr_buffer_drop(lbuf);
        }

        /* Card border: 2px active/urgent on selected card, 1px #2b2b2b/urgent on unselected */
        const float *bcol = selected
                                ? (info->is_urgent ? col_urgent : col_active)
                                : (info->is_urgent ? col_urgent : k_container_border);
        int bpx = (selected || info->is_urgent) ? 2 : 1;
        add_border_rects(sw->tree, cx, cy, SWITCHER_CARD_W, SWITCHER_CARD_H, bpx, bcol);
    }

    comp_server_schedule_frames(s);
}

static size_t
collect_candidates(CompServer *s, CompSwitchScope scope, const char *filter_app_id,
                   CompWindowInfo **out_items)
{
    *out_items = NULL;
    if (!s || !s->layout)
        return 0;

    size_t max_w = comp_layout_window_count(s->layout) + 4;
    CompWindowInfo *all = talloc_zero_array(s->switcher, CompWindowInfo, (unsigned)max_w);
    if (!all)
        return 0;

    size_t n_all = comp_window_list_info(s, all, max_w);
    size_t active_ws = comp_layout_active_workspace(s->layout) + 1;

    size_t n = 0;
    for (size_t i = 0; i < n_all; i++) {
        if (scope == COMP_SWITCH_SCOPE_WORKSPACE && all[i].workspace_idx != active_ws)
            continue;
        if (filter_app_id && *filter_app_id) {
            if (!all[i].app_id || strcmp(all[i].app_id, filter_app_id) != 0)
                continue;
        }
        all[n++] = all[i];
    }

    /* Sort descending by focus_timestamp (most recently focused first) */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (all[j].focus_timestamp > all[i].focus_timestamp) {
                CompWindowInfo tmp = all[i];
                all[i] = all[j];
                all[j] = tmp;
            }
        }
    }

    *out_items = all;
    return n;
}

void
comp_switcher_init(CompServer *s)
{
    if (!s || s->switcher)
        return;
    CompSwitcher *sw = talloc_zero(s, CompSwitcher);
    COMP_WARN_RETURN_VOID(sw != NULL);
    talloc_set_name_const(sw, "CompSwitcher");
    sw->server = s;
    talloc_set_destructor(sw, switcher_destructor);
    s->switcher = sw;
}

bool
comp_switcher_is_active(const CompServer *s)
{
    return s && s->switcher && s->switcher->active;
}

static void
switcher_close(CompSwitcher *sw)
{
    if (!sw)
        return;
    sw->active = false;
    sw->hold_mods = 0;
    talloc_free(sw->items);
    sw->items = NULL;
    sw->n_items = 0;
    talloc_free(sw->filter_app_id);
    sw->filter_app_id = NULL;
    talloc_free(sw->mirrors_ctx);
    sw->mirrors_ctx = NULL;
    if (sw->tree) {
        wlr_scene_node_destroy(&sw->tree->node);
        sw->tree = NULL;
    }
    if (sw->server)
        comp_server_schedule_frames(sw->server);
}

void
comp_switcher_cancel(CompServer *s)
{
    if (!s || !s->switcher || !s->switcher->active)
        return;
    CompSwitcher *sw = s->switcher;
    CompWinId orig = sw->initial_focused_id;
    switcher_close(sw);
    if (orig != 0 && comp_win_from_id(s, orig))
        comp_window_focus_id(s, orig);
}

void
comp_switcher_confirm(CompServer *s)
{
    if (!s || !s->switcher || !s->switcher->active)
        return;
    CompSwitcher *sw = s->switcher;
    CompWinId target = 0;
    if (sw->n_items > 0 && sw->selected_idx < sw->n_items)
        target = sw->items[sw->selected_idx].id;
    switcher_close(sw);
    if (target != 0 && comp_win_from_id(s, target))
        comp_window_focus_id(s, target);
}

void
comp_switcher_step(CompServer *s, bool forward, CompSwitchScope scope,
                   bool same_app, uint32_t held_mods)
{
    if (!s || s->locked)
        return;
    if (!s->switcher)
        comp_switcher_init(s);
    CompSwitcher *sw = s->switcher;
    if (!sw)
        return;

    if (!sw->active) {
        sw->scope     = scope;
        sw->same_app  = same_app;
        sw->hold_mods = held_mods & (COMP_MOD_ALT | COMP_MOD_SUPER | COMP_MOD_CTRL);
        sw->initial_focused_id = s->layout ? comp_layout_focused_window(s->layout) : 0;

        talloc_free(sw->filter_app_id);
        sw->filter_app_id = NULL;
        if (same_app && sw->initial_focused_id != 0) {
            CompWindowInfo info[64];
            size_t ni = comp_window_list_info(s, info, 64);
            for (size_t i = 0; i < ni; i++) {
                if (info[i].id == sw->initial_focused_id && info[i].app_id) {
                    sw->filter_app_id = talloc_strdup(sw, info[i].app_id);
                    break;
                }
            }
        }

        talloc_free(sw->items);
        sw->n_items = collect_candidates(s, scope, sw->filter_app_id, &sw->items);
        if (sw->n_items == 0) {
            switcher_close(sw);
            return;
        }

        /* If no modifier is held and switcher isn't already open, perform an
         * immediate MRU cycle (used by single-shot IPC next-window/previous-window). */
        if (held_mods == 0) {
            CompWinId target = 0;
            if (sw->n_items >= 2)
                target = sw->items[forward ? 1 : (sw->n_items - 1)].id;
            switcher_close(sw);
            if (target != 0 && comp_win_from_id(s, target))
                comp_window_focus_id(s, target);
            return;
        }

        sw->active = true;
        sw->scroll_offset = 0;
        if (sw->n_items == 1) {
            sw->selected_idx = 0;
        } else {
            sw->selected_idx = forward ? 1 : (sw->n_items - 1);
        }
    } else {
        if (sw->n_items > 0) {
            if (forward)
                sw->selected_idx = (sw->selected_idx + 1) % sw->n_items;
            else
                sw->selected_idx = (sw->selected_idx + sw->n_items - 1) % sw->n_items;
        }
    }

    switcher_rebuild_scene(sw);
}

void
comp_switcher_on_window_unmap(CompServer *s, CompWinId id)
{
    if (!s || !s->switcher || !s->switcher->active)
        return;
    CompSwitcher *sw = s->switcher;
    if (sw->initial_focused_id == id)
        sw->initial_focused_id = 0;

    talloc_free(sw->items);
    sw->n_items = collect_candidates(s, sw->scope, sw->filter_app_id, &sw->items);
    if (sw->n_items == 0) {
        switcher_close(sw);
        return;
    }
    if (sw->selected_idx >= sw->n_items)
        sw->selected_idx = sw->n_items - 1;
    switcher_rebuild_scene(sw);
}

void
comp_switcher_handle_modifiers(CompServer *s, uint32_t mods)
{
    if (!s || !s->switcher || !s->switcher->active)
        return;
    CompSwitcher *sw = s->switcher;
    if (sw->hold_mods != 0 && (mods & sw->hold_mods) == 0)
        comp_switcher_confirm(s);
}

bool
comp_switcher_handle_key(CompServer *s, uint32_t mods, uint32_t keysym)
{
    if (!s || !s->switcher || !s->switcher->active)
        return false;
    CompSwitcher *sw = s->switcher;

    switch (keysym) {
    case XKB_KEY_Escape:
        comp_switcher_cancel(s);
        return true;

    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        comp_switcher_confirm(s);
        return true;

    case XKB_KEY_Tab:
    case XKB_KEY_grave:
        comp_switcher_step(s, (mods & COMP_MOD_SHIFT) == 0, sw->scope, sw->same_app, sw->hold_mods);
        return true;

    case XKB_KEY_ISO_Left_Tab:
    case XKB_KEY_asciitilde:
        comp_switcher_step(s, false, sw->scope, sw->same_app, sw->hold_mods);
        return true;

    case XKB_KEY_Right:
    case XKB_KEY_Down:
    case XKB_KEY_l:
    case XKB_KEY_j:
        comp_switcher_step(s, true, sw->scope, sw->same_app, sw->hold_mods);
        return true;

    case XKB_KEY_Left:
    case XKB_KEY_Up:
    case XKB_KEY_h:
    case XKB_KEY_k:
        comp_switcher_step(s, false, sw->scope, sw->same_app, sw->hold_mods);
        return true;

    default:
        return true; /* swallow other keys while switcher is open */
    }
}

bool
comp_switcher_handle_click(CompServer *s, double lx, double ly)
{
    if (!s || !s->switcher || !s->switcher->active)
        return false;
    CompSwitcher *sw = s->switcher;

    for (size_t i = 0; i < sw->vis_count; i++) {
        int cx = sw->cont_x + SWITCHER_PAD_OUTER + (int)i * (SWITCHER_CARD_W + SWITCHER_CARD_GAP);
        int cy = sw->cont_y + SWITCHER_PAD_OUTER;
        if (lx >= (double)cx && lx < (double)(cx + SWITCHER_CARD_W) &&
            ly >= (double)cy && ly < (double)(cy + SWITCHER_CARD_H)) {
            sw->selected_idx = sw->scroll_offset + i;
            comp_switcher_confirm(s);
            return true;
        }
    }
    comp_switcher_cancel(s);
    return true;
}
