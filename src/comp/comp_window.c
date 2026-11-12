/* comp_window.c — xdg-shell windows, popups, server-side decorations, 1px borders,
 * CompLayout + CompMotion wiring, click-to-focus, and floating move/resize (Phase 2b).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <talloc.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland/xwayland.h>

#define COMP_BORDER_PX 1
#define COMP_MAX_TABS  16

/* Aero + Metro default border colors (spec §5.2.1 & §3.9 T3):
 *   active:       #0078d4 (0.000, 0.471, 0.831, 1.0)
 *   inactive:     #2b2b2b (0.169, 0.169, 0.169, 1.0)
 *   tab_inactive: #444444 (0.267, 0.267, 0.267, 1.0)
 *   backdrop:     #000000 (0.000, 0.000, 0.000, 1.0)
 */
static const float k_color_active[4]       = { 0.0f, 0.470588f, 0.831373f, 1.0f };
static const float k_color_inactive[4]     = { 0.168627f, 0.168627f, 0.168627f, 1.0f };
static const float k_color_urgent[4]       = { 0.909804f, 0.066667f, 0.137255f, 1.0f };
static const float k_color_tab_inactive[4] = { 0.266667f, 0.266667f, 0.266667f, 1.0f };
static const float k_color_backdrop[4]     = { 0.0f, 0.0f, 0.0f, 1.0f };

typedef struct {
    int32_t        tile_w;
    int32_t        tile_h;
    int32_t        border_px;
    struct wlr_box geom;
    bool           has_mapped_surf;
    bool           fullscreen;
    bool           focused;
    bool           urgent;
    bool           maximized;
    bool           tabbed;
    size_t         tab_count;
    size_t         tab_index;
    float          col_active[4];
    float          col_inactive[4];
    float          col_urgent[4];
} CompBorderState;

typedef struct CompWindow {
    struct wl_list                          link;        /* CompServer.windows (mapped only) */
    CompServer                             *server;
    CompWinId                               id;          /* 0 when unmapped */
    struct wlr_xdg_toplevel                *toplevel;
    struct wlr_xwayland_surface            *xsurf;

    struct wlr_scene_tree                  *scene_tree;  /* node.data == CompWindow* */
    struct wlr_scene_rect                  *backdrop;    /* solid #000000 behind fullscreen */
    struct wlr_scene_tree                  *xdg_tree;    /* client surface + subsurfaces (clipped) */
    struct wlr_scene_rect                  *border[4];   /* top, bottom, left, right 1px rects */
    struct wlr_scene_rect                  *tabs[COMP_MAX_TABS]; /* spec §3.9 T3 tab strip */
    struct wlr_scene_tree                  *popup_tree;  /* xdg_popup trees (unclipped, above borders) */

    struct wlr_foreign_toplevel_handle_v1      *foreign_handle;
    struct wlr_ext_foreign_toplevel_handle_v1  *ext_foreign_handle;
    struct wlr_ext_image_capture_source_v1     *capture_source;
    void                                       *foreign_listeners;

    char                                   *title;
    char                                   *app_id;
    uint64_t                                focus_timestamp;

    bool                                    mapped;
    bool                                    tick_seen;
    bool                                    is_floating;
    bool                                    is_fullscreen;
    bool                                    is_urgent;
    bool                                    manually_floated;
    bool                                    rule_vrr;
    double                                  scroll_factor;
    bool                                    maximized_to_edges;

    int32_t                                 cfg_w;
    int32_t                                 cfg_h;
    int32_t                                 cfg_x;
    int32_t                                 cfg_y;
    bool                                    cfg_activated;
    bool                                    cfg_fullscreen;
    bool                                    cfg_maximized;
    uint32_t                                cfg_tiled;

    bool                                    border_state_valid;
    CompBorderState                         last_border;
    bool                                    has_last_g;
    int32_t                                 last_rw;
    int32_t                                 last_rh;
    CompWindowGeometry                      last_g;

    CompRect                                last_screen_rect;
} CompWindow;

static int32_t
server_border_px(const CompServer *s)
{
    return (s && s->config) ? s->config->border_px : COMP_BORDER_PX;
}

static int32_t
clamp_i32(double v, int32_t lo, int32_t hi)
{
    if (!isfinite(v)) return lo;
    double r = round(v);
    if (r < (double)lo) return lo;
    if (r > (double)hi) return hi;
    return (int32_t)r;
}

/* Spec §3.11 L6: at least clamp(floor(dim / 4), 10, 75) px of a floating window
 * must remain inside the working area on all four edges. */
static CompRect
clamp_floating_rect(CompRect r, double vw, double vh)
{
    if (r.w < 50.0) r.w = 50.0;
    if (r.h < 50.0) r.h = 50.0;
    double vis_w = fmin(75.0, fmax(10.0, floor(r.w / 4.0)));
    double vis_h = fmin(75.0, fmax(10.0, floor(r.h / 4.0)));
    r.x = fmin(vw - vis_w, fmax(vis_w - r.w, r.x));
    r.y = fmin(vh - vis_h, fmax(0.0, r.y));
    return r;
}

#define COMP_MAX_META_LEN 512

static char *
dup_bounded_utf8(void *ctx, const char *s)
{
    if (!s)
        return talloc_strdup(ctx, "");
    size_t len = strnlen(s, COMP_MAX_META_LEN);
    if (len == COMP_MAX_META_LEN && (unsigned char)s[len] != '\0') {
        while (len > 0 && ((unsigned char)s[len] & 0xC0u) == 0x80u)
            len--;
    }
    return talloc_strndup(ctx, s, len);
}

static void
update_window_strings(CompWindow *w)
{
    talloc_free(w->title);
    talloc_free(w->app_id);
    const char *raw_title  = w->toplevel ? w->toplevel->title  : (w->xsurf ? w->xsurf->title : "");
    const char *raw_app_id = w->toplevel ? w->toplevel->app_id : (w->xsurf ? w->xsurf->class : "");
    w->title  = dup_bounded_utf8(w, raw_title);
    w->app_id = dup_bounded_utf8(w, raw_app_id);

    if (w->foreign_handle) {
        wlr_foreign_toplevel_handle_v1_set_title(w->foreign_handle, w->title ? w->title : "");
        wlr_foreign_toplevel_handle_v1_set_app_id(w->foreign_handle, w->app_id ? w->app_id : "");
    }
    if (w->ext_foreign_handle) {
        struct wlr_ext_foreign_toplevel_handle_v1_state st = {
            .title  = w->title ? w->title : "",
            .app_id = w->app_id ? w->app_id : "",
        };
        wlr_ext_foreign_toplevel_handle_v1_update_state(w->ext_foreign_handle, &st);
    }
}

static void
update_window_borders(CompWindow *w, int32_t tile_w, int32_t tile_h,
                      const CompWindowGeometry *g)
{
    CompServer *s = w->server;
    int32_t b = server_border_px(s);
    bool fullscreen = g->fullscreen;
    bool focused    = g->focused;
    bool hide_border = fullscreen || w->maximized_to_edges || b <= 0;
    const float *col_active   = (s && s->config) ? s->config->active_border_rgba   : k_color_active;
    const float *col_inactive = (s && s->config) ? s->config->inactive_border_rgba : k_color_inactive;
    const float *col_urgent   = (s && s->config) ? s->config->urgent_border_rgba   : k_color_urgent;

    struct wlr_box geom = {0};
    bool has_mapped_surf = false;
    if (w->toplevel && w->toplevel->base &&
        w->toplevel->base->surface && w->toplevel->base->surface->mapped) {
        has_mapped_surf = true;
        geom = w->toplevel->base->geometry;
    } else if (w->xsurf && w->xsurf->surface && w->xsurf->surface->mapped) {
        has_mapped_surf = true;
    }

    w->last_rw    = tile_w;
    w->last_rh    = tile_h;
    w->last_g     = *g;
    w->has_last_g = true;

    CompBorderState st = {
        .tile_w          = tile_w,
        .tile_h          = tile_h,
        .border_px       = b,
        .geom            = geom,
        .has_mapped_surf = has_mapped_surf,
        .fullscreen      = fullscreen,
        .focused         = focused,
        .urgent          = w->is_urgent,
        .maximized       = w->maximized_to_edges,
        .tabbed          = g->tabbed,
        .tab_count       = g->tab_count,
        .tab_index       = g->tab_index,
    };
    memcpy(st.col_active,   col_active,   sizeof(st.col_active));
    memcpy(st.col_inactive, col_inactive, sizeof(st.col_inactive));
    memcpy(st.col_urgent,   col_urgent,   sizeof(st.col_urgent));

    if (w->border_state_valid && memcmp(&st, &w->last_border, sizeof(st)) == 0)
        return;
    w->last_border        = st;
    w->border_state_valid = true;

    if (w->backdrop) {
        wlr_scene_node_set_enabled(&w->backdrop->node, fullscreen);
        if (fullscreen) {
            wlr_scene_node_set_position(&w->backdrop->node, 0, 0);
            wlr_scene_rect_set_size(w->backdrop, tile_w, tile_h);
        }
    }
    const float *col = focused ? col_active : (w->is_urgent ? col_urgent : col_inactive);
    for (int i = 0; i < 4; i++) {
        if (!w->border[i])
            continue;
        wlr_scene_node_set_enabled(&w->border[i]->node, !hide_border);
        if (!hide_border)
            wlr_scene_rect_set_color(w->border[i], col);
    }
    int32_t cw = hide_border ? (tile_w > 1 ? tile_w : 1)
                             : (tile_w > 2 * b ? tile_w - 2 * b : 1);
    int32_t ch = hide_border ? (tile_h > 1 ? tile_h : 1)
                             : (tile_h > 2 * b ? tile_h - 2 * b : 1);

    if (has_mapped_surf && w->xdg_tree) {
        struct wlr_box clip = {
            .x      = geom.x,
            .y      = geom.y,
            .width  = cw,
            .height = ch,
        };
        wlr_scene_subsurface_tree_set_clip(&w->xdg_tree->node, &clip);
    }

    if (!hide_border) {
        int32_t bw = (geom.width > 0 && geom.width < cw) ? geom.width : cw;
        int32_t bh = (geom.height > 0 && geom.height < ch) ? geom.height : ch;
        /* top, bottom, left, right around client at (b, b) */
        wlr_scene_node_set_position(&w->border[0]->node, 0, 0);
        wlr_scene_rect_set_size(w->border[0], bw + 2 * b, b);

        wlr_scene_node_set_position(&w->border[1]->node, 0, bh + b);
        wlr_scene_rect_set_size(w->border[1], bw + 2 * b, b);

        wlr_scene_node_set_position(&w->border[2]->node, 0, b);
        wlr_scene_rect_set_size(w->border[2], b, bh);

        wlr_scene_node_set_position(&w->border[3]->node, bw + b, b);
        wlr_scene_rect_set_size(w->border[3], b, bh);

        if (w->xdg_tree)
            wlr_scene_node_set_position(&w->xdg_tree->node, b, b);
        if (w->popup_tree)
            wlr_scene_node_set_position(&w->popup_tree->node, b, b);
    } else {
        if (w->xdg_tree)
            wlr_scene_node_set_position(&w->xdg_tree->node, 0, 0);
        if (w->popup_tree)
            wlr_scene_node_set_position(&w->popup_tree->node, 0, 0);
    }

    /* Spec §3.9 T3: 4px tab indicator strip at x = -6, 2px vertical gap */
    size_t n_tabs = (g->tabbed && !fullscreen && g->tab_count > 0) ? g->tab_count : 0;
    if (n_tabs > COMP_MAX_TABS)
        n_tabs = COMP_MAX_TABS;
    int32_t total_gaps = n_tabs > 1 ? (int32_t)(n_tabs - 1) * 2 : 0;
    int32_t avail_h    = tile_h > total_gaps ? tile_h - total_gaps : (int32_t)n_tabs;
    int32_t cur_y      = 0;
    for (size_t i = 0; i < COMP_MAX_TABS; i++) {
        if (i >= n_tabs) {
            if (w->tabs[i])
                wlr_scene_node_set_enabled(&w->tabs[i]->node, false);
            continue;
        }
        if (!w->tabs[i]) {
            w->tabs[i] = wlr_scene_rect_create(w->scene_tree, 4, 1, k_color_tab_inactive);
            if (!w->tabs[i])
                continue;
            if (w->popup_tree)
                wlr_scene_node_place_below(&w->tabs[i]->node, &w->popup_tree->node);
        }
        int32_t seg_h = (i + 1 == n_tabs) ? (tile_h - cur_y) : (avail_h / (int32_t)n_tabs);
        if (seg_h < 1)
            seg_h = 1;
        wlr_scene_node_set_enabled(&w->tabs[i]->node, true);
        wlr_scene_node_set_position(&w->tabs[i]->node, -6, cur_y);
        wlr_scene_rect_set_size(w->tabs[i], 4, seg_h);
        wlr_scene_rect_set_color(w->tabs[i], (i == g->tab_index) ? col_active : k_color_tab_inactive);
        cur_y += seg_h + 2;
    }
}

static void
configure_window_toplevel(CompWindow *w, int32_t tile_w, int32_t tile_h,
                          bool activated, bool fullscreen, bool floating)
{
    bool is_xdg = (w->toplevel && w->toplevel->base && w->toplevel->base->initialized);
    bool is_x11 = (w->xsurf != NULL);
    if (!is_xdg && !is_x11)
        return;

    bool hide_border = fullscreen || w->maximized_to_edges;
    int32_t b = hide_border ? 0 : server_border_px(w->server);
    int32_t cw = tile_w > 2 * b ? tile_w - 2 * b : 1;
    int32_t ch = tile_h > 2 * b ? tile_h - 2 * b : 1;
    uint32_t tiled = (!floating && !fullscreen)
                         ? (WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT)
                         : WLR_EDGE_NONE;

    int32_t xw_x = 0, xw_y = 0;
    if (is_x11 && w->scene_tree) {
        xw_x = w->scene_tree->node.x + b;
        xw_y = w->scene_tree->node.y + b;
    }

    bool changed = (cw != w->cfg_w) || (ch != w->cfg_h) ||
                   (is_x11 && (xw_x != w->cfg_x || xw_y != w->cfg_y)) ||
                   (activated != w->cfg_activated) ||
                   (fullscreen != w->cfg_fullscreen) ||
                   (w->maximized_to_edges != w->cfg_maximized) ||
                   (tiled != w->cfg_tiled);
    if (!changed)
        return;

    w->cfg_w          = cw;
    w->cfg_h          = ch;
    w->cfg_x          = xw_x;
    w->cfg_y          = xw_y;
    w->cfg_activated  = activated;
    w->cfg_fullscreen = fullscreen;
    w->cfg_maximized  = w->maximized_to_edges;
    w->cfg_tiled      = tiled;

    if (is_xdg) {
        wlr_xdg_toplevel_set_tiled(w->toplevel, tiled);
        wlr_xdg_toplevel_set_activated(w->toplevel, activated);
        wlr_xdg_toplevel_set_fullscreen(w->toplevel, fullscreen);
        wlr_xdg_toplevel_set_maximized(w->toplevel, w->maximized_to_edges);
        wlr_xdg_toplevel_set_size(w->toplevel, cw, ch);
    } else if (is_x11) {
        wlr_xwayland_surface_activate(w->xsurf, activated);
        wlr_xwayland_surface_set_fullscreen(w->xsurf, fullscreen);
        wlr_xwayland_surface_set_maximized(w->xsurf, w->maximized_to_edges, w->maximized_to_edges);
        wlr_xwayland_surface_configure(w->xsurf, (int16_t)xw_x, (int16_t)xw_y,
                                       (uint16_t)cw, (uint16_t)ch);
    }

    if (w->foreign_handle) {
        wlr_foreign_toplevel_handle_v1_set_activated(w->foreign_handle, activated);
        wlr_foreign_toplevel_handle_v1_set_fullscreen(w->foreign_handle, fullscreen);
        wlr_foreign_toplevel_handle_v1_set_maximized(w->foreign_handle, w->maximized_to_edges);
    }
}

static void
enter_keyboard_surface(CompServer *s, struct wlr_surface *surface)
{
    if (!s || !s->seat)
        return;
    comp_protocols_notify_focus_surface(s, surface);
    if (!surface) {
        if (s->seat->keyboard_state.focused_surface != NULL)
            wlr_seat_keyboard_notify_clear_focus(s->seat);
        return;
    }
    if (s->seat->keyboard_state.focused_surface == surface)
        return;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(s->seat, surface,
                                       kb->keycodes, kb->num_keycodes, &kb->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
    }
}

void
comp_server_restore_keyboard_focus(CompServer *s)
{
    if (!s || !s->seat)
        return;
    if (s->locked) {
        enter_keyboard_surface(s, s->lock_surface);
        return;
    }
    if (s->exclusive_layer_surface) {
        enter_keyboard_surface(s, s->exclusive_layer_surface);
        return;
    }
    CompWindow *w = s->layout ? comp_win_from_id(s, comp_layout_focused_window(s->layout)) : NULL;
    if (w && w->mapped && w->xsurf)
        wlr_xwayland_surface_activate(w->xsurf, true);
    enter_keyboard_surface(s, comp_window_get_surface(w));
}

void
comp_window_tick_frame(CompServer *s, double now_s)
{
    if (!s || !s->motion)
        return;

    CompRect work = {0};
    comp_server_working_area(s, &work);
    double full_vw = 1920.0, full_vh = 1080.0;
    comp_server_viewport_size(s, &full_vw, &full_vh);

    size_t total = comp_layout_window_count(s->layout);
    size_t cap = total + 8;
    CompWindowGeometry stack_geoms[32];
    CompWindowGeometry *geoms = cap <= 32 ? stack_geoms
                                          : talloc_array(s, CompWindowGeometry, (unsigned)cap);
    if (!geoms)
        return;

    double view_x = 0.0, ws_y = 0.0;
    size_t n = comp_motion_sample(s->motion, now_s, geoms, cap, &view_x, &ws_y);
    if (n > cap)
        n = cap;

    CompWindow *w;
    wl_list_for_each(w, &s->windows, link) {
        w->tick_seen = false;
    }

    for (size_t i = 0; i < n; i++) {
        const CompWindowGeometry *g = &geoms[i];
        CompWindow *win = comp_win_from_id(s, g->id);
        if (!win || !win->mapped || !win->scene_tree)
            continue;

        win->last_screen_rect = g->rect;
        if (!g->visible)
            continue;

        win->tick_seen = true;

        int32_t rx = g->fullscreen ? 0 : clamp_i32(g->rect.x + work.x, -32768, 32767);
        int32_t ry = g->fullscreen ? 0 : clamp_i32(g->rect.y + work.y, -32768, 32767);
        int32_t rw = g->fullscreen ? clamp_i32(full_vw, 1, 32767) : clamp_i32(g->rect.w, 1, 32767);
        int32_t rh = g->fullscreen ? clamp_i32(full_vh, 1, 32767) : clamp_i32(g->rect.h, 1, 32767);

        wlr_scene_node_set_enabled(&win->scene_tree->node, true);
        wlr_scene_node_set_position(&win->scene_tree->node, rx, ry);
        update_window_borders(win, rw, rh, g);
    }

    wl_list_for_each(w, &s->windows, link) {
        if (!w->tick_seen && w->scene_tree)
            wlr_scene_node_set_enabled(&w->scene_tree->node, false);
    }

    if (geoms != stack_geoms)
        talloc_free(geoms);
}

void
comp_window_relayout(CompServer *s)
{
    if (!s || !s->layout || !s->motion)
        return;

    CompRect work = {0};
    comp_server_working_area(s, &work);
    double vw = work.w, vh = work.h;
    double full_vw = 1920.0, full_vh = 1080.0;
    comp_server_viewport_size(s, &full_vw, &full_vh);
    double now_s = comp_now_sec();

    comp_motion_sync(s->motion, s->layout, vw, vh, now_s);

    size_t total = comp_layout_window_count(s->layout);
    size_t cap = total + 8;
    CompWindowGeometry stack_geoms[32];
    CompWindowGeometry *geoms = cap <= 32 ? stack_geoms
                                          : talloc_array(s, CompWindowGeometry, (unsigned)cap);
    if (!geoms)
        return;

    size_t n = comp_layout_arrange(s->layout, vw, vh, geoms, cap);
    if (n > cap)
        n = cap;

    for (size_t i = 0; i < n; i++) {
        const CompWindowGeometry *g = &geoms[i];
        CompWindow *w = comp_win_from_id(s, g->id);
        if (!w || !w->mapped)
            continue;

        w->is_floating   = g->floating;
        w->is_fullscreen = g->fullscreen;

        /* Spec §3.10 S6: focused fullscreen window renders in tree_fullscreen;
         * floating windows in tree_floating; tiled windows in tree_tiled. */
        struct wlr_scene_tree *target_parent = s->tree_tiled;
        if (g->fullscreen && g->focused)
            target_parent = s->tree_fullscreen;
        else if (g->floating)
            target_parent = s->tree_floating;

        if (w->scene_tree && w->scene_tree->node.parent != target_parent)
            wlr_scene_node_reparent(&w->scene_tree->node, target_parent);

        int32_t rw = g->fullscreen ? clamp_i32(full_vw, 1, 32767) : clamp_i32(g->rect.w, 1, 32767);
        int32_t rh = g->fullscreen ? clamp_i32(full_vh, 1, 32767) : clamp_i32(g->rect.h, 1, 32767);
        configure_window_toplevel(w, rw, rh, g->focused, g->fullscreen, g->floating);
    }

    CompWinId focused_id = comp_layout_focused_window(s->layout);
    bool focus_changed = (focused_id != s->last_focused_id);
    if (focus_changed) {
        CompWindow *old_w = comp_win_from_id(s, s->last_focused_id);
        if (old_w && old_w->mapped && old_w->cfg_activated) {
            old_w->cfg_activated = false;
            if (old_w->toplevel && old_w->toplevel->base->initialized)
                wlr_xdg_toplevel_set_activated(old_w->toplevel, false);
            if (old_w->foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_activated(old_w->foreign_handle, false);
        }
        s->last_focused_id = focused_id;
        CompWindow *new_w = comp_win_from_id(s, focused_id);
        if (new_w && new_w->mapped) {
            new_w->focus_timestamp = ++s->focus_seq;
            new_w->is_urgent = false;
            if (new_w->is_floating && new_w->scene_tree)
                wlr_scene_node_raise_to_top(&new_w->scene_tree->node);
            if (new_w->foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_activated(new_w->foreign_handle, true);
        }
        comp_server_restore_keyboard_focus(s);
    } else {
        comp_server_restore_keyboard_focus(s);
    }

    size_t active_ws = comp_layout_active_workspace(s->layout);
    bool ws_changed = (active_ws != s->last_active_ws);
    s->last_active_ws = active_ws;

    if (geoms != stack_geoms)
        talloc_free(geoms);

    comp_window_tick_frame(s, now_s);
    comp_input_refresh_pointer_focus(s, (uint32_t)(now_s * 1000.0));
    comp_output_update_vrr(s);
    comp_server_schedule_frames(s);

    if (ws_changed)
        comp_ipc_emit_workspaces(s);
    if (focus_changed)
        comp_ipc_emit_focus(s, focused_id);
}

struct CompWindow *
comp_window_at(CompServer *s, double lx, double ly,
               struct wlr_surface **out_surface, double *out_sx, double *out_sy)
{
    if (out_surface) *out_surface = NULL;
    if (out_sx)      *out_sx = 0.0;
    if (out_sy)      *out_sy = 0.0;
    if (!s || !s->scene)
        return NULL;

    double sx = 0.0, sy = 0.0;
    struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node, lx, ly, &sx, &sy);
    if (!node)
        return NULL;

    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sbuf);
        if (ss && ss->surface) {
            if (out_surface) *out_surface = ss->surface;
            if (out_sx)      *out_sx = sx;
            if (out_sy)      *out_sy = sy;
        }
    }

    for (struct wlr_scene_tree *t = node->parent; t != NULL; t = t->node.parent) {
        if (t->node.data)
            return (CompWindow *)t->node.data;
    }
    return NULL;
}

void
comp_window_focus(CompServer *s, struct CompWindow *w)
{
    if (!s || !w || !w->mapped)
        return;
    comp_window_focus_id(s, w->id);
}

void
comp_window_request_activate(CompServer *s, struct CompWindow *w,
                             const struct wlr_xdg_activation_token_v1 *token)
{
    if (!s || s->locked || !w || !w->mapped)
        return;
    CompWinId fid = s->layout ? comp_layout_focused_window(s->layout) : 0;
    if (fid == w->id)
        return;
    bool token_from_focused = (token && token->seat && token->surface &&
                               s->seat && token->surface == s->seat->keyboard_state.focused_surface);
    if (fid == 0 || token_from_focused) {
        comp_window_focus_id(s, w->id);
        return;
    }
    if (!w->is_urgent) {
        w->is_urgent = true;
        comp_window_relayout(s);
        comp_ipc_emit_workspaces(s);
        comp_ipc_emit_windows(s);
    }
}

void
comp_window_focus_id(CompServer *s, CompWinId id)
{
    if (!s || id == 0)
        return;
    CompWindow *w = comp_win_from_id(s, id);
    if (!w || !w->mapped)
        return;
    if (comp_layout_focus_window(s->layout, id)) {
        if (w->is_floating && w->scene_tree)
            wlr_scene_node_raise_to_top(&w->scene_tree->node);
        comp_window_relayout(s);
        comp_ipc_emit_windows(s);
    } else {
        comp_server_restore_keyboard_focus(s);
    }
}

bool
comp_window_focus_app(CompServer *s, const char *app_id)
{
    if (!s || !app_id || !*app_id)
        return false;
    CompWindow *best = NULL;
    CompWindow *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->mapped || !w->app_id)
            continue;
        if (strcasecmp(w->app_id, app_id) == 0) {
            if (!best || w->focus_timestamp >= best->focus_timestamp)
                best = w;
        }
    }
    if (!best)
        return false;
    comp_window_focus_id(s, best->id);
    return true;
}

bool
comp_window_close_id(CompServer *s, CompWinId id)
{
    if (!s)
        return false;
    if (id == 0 && s->layout)
        id = comp_layout_focused_window(s->layout);
    CompWindow *w = comp_win_from_id(s, id);
    if (!w || !w->mapped)
        return false;
    if (w->toplevel) {
        wlr_xdg_toplevel_send_close(w->toplevel);
        return true;
    }
    if (w->xsurf) {
        wlr_xwayland_surface_close(w->xsurf);
        return true;
    }
    return false;
}

double
comp_window_scroll_factor_for_surface(CompServer *s, struct wlr_surface *surface)
{
    if (!s || !surface)
        return 1.0;
    struct wlr_surface *p = surface;
    while (p) {
        struct wlr_xdg_surface *xs = wlr_xdg_surface_try_from_wlr_surface(p);
        if (!xs)
            break;
        if (xs->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xs->data) {
            CompWindow *w = xs->data;
            return w->scroll_factor > 0.0 ? w->scroll_factor : 1.0;
        }
        if (xs->role == WLR_XDG_SURFACE_ROLE_POPUP && xs->popup)
            p = xs->popup->parent;
        else
            break;
    }
    return 1.0;
}

bool
comp_window_focused_wants_vrr(const CompServer *s)
{
    if (!s || !s->layout)
        return false;
    CompWinId id = comp_layout_focused_window(s->layout);
    CompWindow *w = comp_win_from_id(s, id);
    if (!w || !w->mapped)
        return false;
    return w->is_fullscreen || w->rule_vrr;
}

size_t
comp_window_list_info(const CompServer *s, CompWindowInfo *out, size_t max_out)
{
    if (!s)
        return 0;
    CompWinId focused = s->layout ? comp_layout_focused_window(s->layout) : 0;
    size_t count = 0;
    const CompWindow *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->mapped)
            continue;
        if (out && count < max_out) {
            size_t ws_idx = 0;
            if (s->layout && !comp_layout_window_workspace(s->layout, w->id, &ws_idx))
                ws_idx = 0;
            out[count] = (CompWindowInfo){
                .id              = w->id,
                .title           = w->title ? w->title : "",
                .app_id          = w->app_id ? w->app_id : "",
                .workspace_idx   = ws_idx + 1,
                .is_focused      = (w->id == focused),
                .is_floating     = w->is_floating,
                .is_fullscreen   = w->is_fullscreen,
                .is_urgent       = w->is_urgent,
                .focus_timestamp = w->focus_timestamp,
            };
        }
        count++;
    }
    return count;
}

struct wlr_surface *
comp_window_get_surface(const struct CompWindow *w)
{
    if (!w || !w->mapped)
        return NULL;
    if (w->toplevel && w->toplevel->base)
        return w->toplevel->base->surface;
    if (w->xsurf)
        return w->xsurf->surface;
    return NULL;
}

bool
comp_window_get_capture_box(const struct CompWindow *w, bool include_border,
                            struct wlr_scene_node **out_node, struct wlr_box *out_logical_box)
{
    if (!w || !w->mapped || !w->scene_tree || !w->xdg_tree || !out_node || !out_logical_box)
        return false;

    int32_t b = server_border_px(w->server);
    bool hide_border = w->is_fullscreen || w->maximized_to_edges || b <= 0;
    int32_t cw = w->cfg_w > 0 ? w->cfg_w : (int32_t)w->last_screen_rect.w;
    int32_t ch = w->cfg_h > 0 ? w->cfg_h : (int32_t)w->last_screen_rect.h;
    if (w->toplevel && w->toplevel->base && w->toplevel->base->surface &&
        w->toplevel->base->surface->mapped) {
        struct wlr_box geom = w->toplevel->base->geometry;
        if (geom.width > 0 && geom.width < cw)
            cw = geom.width;
        if (geom.height > 0 && geom.height < ch)
            ch = geom.height;
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    if (include_border && !hide_border) {
        *out_node = &w->scene_tree->node;
        *out_logical_box = (struct wlr_box){ .x = 0, .y = 0, .width = cw + 2 * b, .height = ch + 2 * b };
    } else {
        *out_node = &w->xdg_tree->node;
        *out_logical_box = (struct wlr_box){ .x = 0, .y = 0, .width = cw, .height = ch };
    }
    return true;
}

void
comp_window_accept_capture_request(
    CompServer *s,
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req)
{
    if (!s || s->locked || !req || !req->toplevel_handle || !req->toplevel_handle->data)
        return;
    CompWindow *w = req->toplevel_handle->data;
    if (!w->mapped || !w->scene_tree)
        return;
    if (!w->capture_source) {
        w->capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
            &w->scene_tree->node, s->loop, s->allocator, s->renderer);
    }
    if (w->capture_source) {
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
            req, w->capture_source);
    }
}

void
comp_window_destroy_capture_sources(CompServer *s)
{
    if (!s)
        return;
    CompWindow *w;
    wl_list_for_each(w, &s->windows, link) {
        if (w->capture_source && w->scene_tree) {
            wl_signal_emit_mutable(&w->scene_tree->node.events.destroy, NULL);
            w->capture_source = NULL;
        }
    }
}

void
comp_window_update_grab(CompServer *s)
{
    if (!s || s->grab_win == 0)
        return;
    CompWindow *w = comp_win_from_id(s, s->grab_win);
    if (!w || !w->mapped || !w->is_floating) {
        comp_window_end_grab(s);
        return;
    }

    double dx = s->cursor->x - s->grab_start_cx;
    double dy = s->cursor->y - s->grab_start_cy;
    CompRect r = s->grab_start_rect;
    int32_t b = server_border_px(s);

    if (!s->grab_resizing) {
        r.x += dx;
        r.y += dy;
    } else {
        double min_w = 50.0, min_h = 50.0;
        if (w->toplevel && w->toplevel->current.min_width > 0)
            min_w = (double)w->toplevel->current.min_width + 2 * b;
        else if (w->xsurf && w->xsurf->size_hints && w->xsurf->size_hints->min_width > 0)
            min_w = (double)w->xsurf->size_hints->min_width + 2 * b;
        if (w->toplevel && w->toplevel->current.min_height > 0)
            min_h = (double)w->toplevel->current.min_height + 2 * b;
        else if (w->xsurf && w->xsurf->size_hints && w->xsurf->size_hints->min_height > 0)
            min_h = (double)w->xsurf->size_hints->min_height + 2 * b;

        if (s->grab_edges & WLR_EDGE_LEFT) {
            double new_w = r.w - dx;
            if (new_w < min_w) new_w = min_w;
            r.x += r.w - new_w;
            r.w = new_w;
        } else if (s->grab_edges & WLR_EDGE_RIGHT) {
            r.w += dx;
            if (r.w < min_w) r.w = min_w;
        }
        if (s->grab_edges & WLR_EDGE_TOP) {
            double new_h = r.h - dy;
            if (new_h < min_h) new_h = min_h;
            r.y += r.h - new_h;
            r.h = new_h;
        } else if (s->grab_edges & WLR_EDGE_BOTTOM) {
            r.h += dy;
            if (r.h < min_h) r.h = min_h;
        }
    }

    double vw = 1920.0, vh = 1080.0;
    comp_server_viewport_size(s, &vw, &vh);
    r = clamp_floating_rect(r, vw, vh);

    if (comp_layout_set_floating_rect(s->layout, w->id, r))
        comp_window_relayout(s);
}

void
comp_window_end_grab(CompServer *s)
{
    if (!s || s->grab_win == 0)
        return;
    CompWindow *w = comp_win_from_id(s, s->grab_win);
    if (w && w->mapped && s->grab_resizing && w->toplevel && w->toplevel->base->initialized)
        wlr_xdg_toplevel_set_resizing(w->toplevel, false);
    s->grab_win      = 0;
    s->grab_resizing = false;
    s->grab_edges    = 0;
}

void
comp_window_get_popup_trees(const struct CompWindow *w,
                            struct wlr_scene_tree **out_popup_tree,
                            struct wlr_scene_node **out_xdg_node)
{
    if (out_popup_tree)
        *out_popup_tree = w ? (w->popup_tree ? w->popup_tree : w->xdg_tree) : NULL;
    if (out_xdg_node)
        *out_xdg_node = (w && w->xdg_tree) ? &w->xdg_tree->node : NULL;
}

void
comp_window_resize_column(CompServer *s, double delta, CompDeltaMode mode)
{
    if (!s || !s->layout)
        return;
    CompWindow *fw = comp_win_from_id(s, comp_layout_focused_window(s->layout));
    if (fw)
        fw->maximized_to_edges = false;
    double vw = 1920.0, vh = 1080.0;
    comp_server_viewport_size(s, &vw, &vh);
    double d = delta;
    if (mode == COMP_DELTA_REL_PX && vw > 0.0)
        d = delta / vw;
    if (mode == COMP_DELTA_SET_PROP || mode == COMP_DELTA_SET_PX) {
        double prop = mode == COMP_DELTA_SET_PROP ? delta : (vw > 0.0 ? delta / vw : 0.5);
        comp_layout_adjust_column_width(s->layout, -1.0);
        comp_layout_adjust_column_width(s->layout, prop - 0.05);
    } else {
        comp_layout_adjust_column_width(s->layout, d);
    }
    comp_window_relayout(s);
}

void
comp_window_resize_height(CompServer *s, double delta, CompDeltaMode mode)
{
    if (!s || !s->layout)
        return;
    double vw = 1920.0, vh = 1080.0;
    comp_server_viewport_size(s, &vw, &vh);
    double d = delta;
    if (mode == COMP_DELTA_REL_PX && vh > 0.0)
        d = delta / vh;
    if (mode == COMP_DELTA_SET_PROP || mode == COMP_DELTA_SET_PX) {
        double prop = mode == COMP_DELTA_SET_PROP ? delta : (vh > 0.0 ? delta / vh : 0.5);
        comp_layout_adjust_window_height(s->layout, -1.0);
        comp_layout_adjust_window_height(s->layout, prop - 0.1);
    } else {
        comp_layout_adjust_window_height(s->layout, d);
    }
    comp_window_relayout(s);
}

bool
comp_window_handle_bind(CompServer *s, const CompBind *bind)
{
    if (!s || !bind)
        return false;

    switch (bind->kind) {
    case COMP_BIND_LAYOUT: {
        CompWindow *fw = comp_win_from_id(s, comp_layout_focused_window(s->layout));
        if (fw) {
            if (bind->layout_action == COMP_ACT_TOGGLE_WINDOW_FLOATING)
                fw->manually_floated = true;
            if (bind->layout_action == COMP_ACT_SWITCH_PRESET_COLUMN_WIDTH ||
                bind->layout_action == COMP_ACT_MAXIMIZE_COLUMN ||
                bind->layout_action == COMP_ACT_EXPAND_COLUMN_TO_AVAILABLE) {
                fw->maximized_to_edges = false;
            }
        }
        if (comp_layout_apply(s->layout, bind->layout_action)) {
            comp_window_relayout(s);
            comp_ipc_emit_workspaces(s);
            comp_ipc_emit_windows(s);
        }
        return true;
    }

    case COMP_BIND_SPAWN:
        if (!s->opts.greeter_mode && bind->argv && bind->argv[0])
            comp_spawn(s, (const char *const *)bind->argv);
        return true;

    case COMP_BIND_CLOSE_WINDOW:
        comp_window_close_id(s, 0);
        return true;

    case COMP_BIND_FULLSCREEN_WINDOW:
        if (comp_layout_toggle_fullscreen(s->layout)) {
            comp_window_relayout(s);
            comp_ipc_emit_windows(s);
        }
        return true;

    case COMP_BIND_NEXT_WINDOW:
    case COMP_BIND_PREVIOUS_WINDOW: {
        bool forward = (bind->kind == COMP_BIND_NEXT_WINDOW);
        uint32_t held_mods = 0;
        if (s->seat) {
            struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
            if (kb) {
                uint32_t m = wlr_keyboard_get_modifiers(kb);
                if (m & WLR_MODIFIER_SHIFT) held_mods |= COMP_MOD_SHIFT;
                if (m & WLR_MODIFIER_CTRL)  held_mods |= COMP_MOD_CTRL;
                if (m & WLR_MODIFIER_ALT)   held_mods |= COMP_MOD_ALT;
                if (m & WLR_MODIFIER_LOGO)  held_mods |= COMP_MOD_SUPER;
            }
        }
        comp_switcher_step(s, forward, bind->scope, bind->same_app, held_mods);
        return true;
    }

    case COMP_BIND_SCREENSHOT:
        comp_screenshot_interactive(s);
        return true;

    case COMP_BIND_SCREENSHOT_WINDOW:
        comp_screenshot_window(s);
        return true;

    case COMP_BIND_SCREENSHOT_SCREEN:
        comp_screenshot_screen(s);
        return true;

    case COMP_BIND_SET_COLUMN_WIDTH:
        comp_window_resize_column(s, bind->delta, bind->delta_mode);
        return true;

    case COMP_BIND_SET_WINDOW_HEIGHT:
        comp_window_resize_height(s, bind->delta, bind->delta_mode);
        return true;

    case COMP_BIND_BREAK_SHORTCUTS_INHIBIT:
        comp_protocols_break_shortcuts_inhibit(s);
        return true;

    case COMP_BIND_RELOAD_CONFIG:
        comp_server_reload_config(s);
        return true;

    case COMP_BIND_QUIT:
        if (!s->opts.greeter_mode)
            comp_server_terminate(s, 0);
        return true;
    }
    return false;
}

/* ── xdg_toplevel handlers ── */

static void
handle_foreign_request_activate(void *owner, void *data)
{
    CompWindow *w = owner;
    (void)data;
    if (w && w->mapped && w->server && !w->server->locked)
        comp_window_focus(w->server, w);
}

static void
handle_foreign_request_close(void *owner, void *data)
{
    CompWindow *w = owner;
    (void)data;
    if (w && w->mapped && w->server)
        comp_window_close_id(w->server, w->id);
}

static void
handle_foreign_request_fullscreen(void *owner, void *data)
{
    CompWindow *w = owner;
    const struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
    if (w && w->mapped && w->server && ev) {
        comp_layout_set_fullscreen(w->server->layout, w->id, ev->fullscreen);
        comp_window_relayout(w->server);
        comp_ipc_emit_windows(w->server);
    }
}

static void
create_foreign_handles(CompWindow *w)
{
    CompServer *s = w->server;
    if (!s || s->opts.greeter_mode)
        return;

    if (s->foreign_toplevel_mgr && !w->foreign_handle) {
        w->foreign_handle = wlr_foreign_toplevel_handle_v1_create(s->foreign_toplevel_mgr);
        if (w->foreign_handle) {
            wlr_foreign_toplevel_handle_v1_set_title(w->foreign_handle, w->title ? w->title : "");
            wlr_foreign_toplevel_handle_v1_set_app_id(w->foreign_handle, w->app_id ? w->app_id : "");
            struct wlr_output *out = comp_server_primary_output(s);
            if (out)
                wlr_foreign_toplevel_handle_v1_output_enter(w->foreign_handle, out);
            w->foreign_listeners = talloc_new(w);
            if (w->foreign_listeners) {
                comp_listen_ctx(w->foreign_listeners, w,
                                &w->foreign_handle->events.request_activate,
                                handle_foreign_request_activate);
                comp_listen_ctx(w->foreign_listeners, w,
                                &w->foreign_handle->events.request_close,
                                handle_foreign_request_close);
                comp_listen_ctx(w->foreign_listeners, w,
                                &w->foreign_handle->events.request_fullscreen,
                                handle_foreign_request_fullscreen);
            }
        }
    }

    if (s->ext_foreign_toplevel_list && !w->ext_foreign_handle) {
        struct wlr_ext_foreign_toplevel_handle_v1_state st = {
            .title  = w->title ? w->title : "",
            .app_id = w->app_id ? w->app_id : "",
        };
        w->ext_foreign_handle =
            wlr_ext_foreign_toplevel_handle_v1_create(s->ext_foreign_toplevel_list, &st);
        if (w->ext_foreign_handle)
            w->ext_foreign_handle->data = w;
    }
}

static void
destroy_foreign_handles(CompWindow *w)
{
    talloc_free(w->foreign_listeners);
    w->foreign_listeners = NULL;
    if (w->foreign_handle) {
        wlr_foreign_toplevel_handle_v1_destroy(w->foreign_handle);
        w->foreign_handle = NULL;
    }
    if (w->ext_foreign_handle) {
        w->ext_foreign_handle->data = NULL;
        wlr_ext_foreign_toplevel_handle_v1_destroy(w->ext_foreign_handle);
        w->ext_foreign_handle = NULL;
    }
}

static int
window_destructor(CompWindow *w)
{
    CompServer *s = w->server;
    destroy_foreign_handles(w);
    if (w->mapped) {
        CompWinId old_id = w->id;
        if (s->grab_win == w->id)
            comp_window_end_grab(s);
        comp_layout_remove_window(s->layout, w->id);
        comp_win_free_id(s, w->id);
        wl_list_remove(&w->link);
        w->mapped = false;
        comp_switcher_on_window_unmap(s, old_id);
    }
    if (w->toplevel && w->toplevel->base)
        w->toplevel->base->data = NULL;
    if (w->xsurf)
        w->xsurf->data = NULL;
    if (s->scene && w->scene_tree) {
        w->scene_tree->node.data = NULL;
        wlr_scene_node_destroy(&w->scene_tree->node);
        w->scene_tree = NULL;
    }
    return 0;
}

static bool
should_open_floating(const CompWindow *w, const CompWindowRuleResult *rr)
{
    if (rr && rr->has_floating)
        return rr->floating;
    if (w->toplevel) {
        const struct wlr_xdg_dialog_v1 *dlg =
            wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(w->toplevel);
        if (w->toplevel->parent != NULL || (dlg != NULL && dlg->modal))
            return true;
        const struct wlr_xdg_toplevel_state *st = &w->toplevel->current;
        return (st->min_width > 0 && st->min_height > 0 &&
                st->min_width == st->max_width && st->min_height == st->max_height);
    }
    if (w->xsurf) {
        if (w->xsurf->parent != NULL || w->xsurf->modal)
            return true;
        const xcb_size_hints_t *sh = w->xsurf->size_hints;
        if (sh && sh->min_width > 0 && sh->min_height > 0 &&
            sh->min_width == sh->max_width && sh->min_height == sh->max_height) {
            return true;
        }
    }
    return false;
}

static void
handle_toplevel_commit(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    (void)data;
    if (w->toplevel->base->initial_commit) {
        update_window_strings(w);
        wlr_xdg_toplevel_set_wm_capabilities(
            w->toplevel,
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);

        CompWindowRuleResult rr = {0};
        if (s && s->rules)
            comp_rules_match_window(s->rules, w->app_id, w->title, &rr);

        if (w->toplevel->requested.fullscreen) {
            double full_vw = 1920.0, full_vh = 1080.0;
            comp_server_viewport_size(s, &full_vw, &full_vh);
            int32_t cw = clamp_i32(full_vw, 1, 32767);
            int32_t ch = clamp_i32(full_vh, 1, 32767);
            w->cfg_w = cw;
            w->cfg_h = ch;
            w->cfg_fullscreen = true;
            wlr_xdg_toplevel_set_fullscreen(w->toplevel, true);
            wlr_xdg_toplevel_set_size(w->toplevel, cw, ch);
        } else if (!should_open_floating(w, &rr)) {
            CompRect work = {0};
            comp_server_working_area(s, &work);
            double gap = (s && s->config) ? s->config->layout.gap : 6.0;
            double col_prop = (rr.has_width && rr.width > 0.0)
                                  ? rr.width
                                  : ((s && s->config && s->config->layout.default_width > 0.0)
                                         ? s->config->layout.default_width
                                         : 0.5);
            if (w->toplevel->requested.maximized)
                col_prop = 1.0;
            int32_t b = server_border_px(s);
            int32_t tile_w = clamp_i32((work.w - gap) * col_prop - gap, 1, 32767);
            int32_t tile_h = clamp_i32(work.h - 2.0 * gap, 1, 32767);
            int32_t cw = tile_w > 2 * b ? tile_w - 2 * b : 1;
            int32_t ch = tile_h > 2 * b ? tile_h - 2 * b : 1;
            uint32_t tiled = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
            w->cfg_w     = cw;
            w->cfg_h     = ch;
            w->cfg_tiled = tiled;
            wlr_xdg_toplevel_set_tiled(w->toplevel, tiled);
            wlr_xdg_toplevel_set_size(w->toplevel, cw, ch);
        } else {
            wlr_xdg_toplevel_set_size(w->toplevel, 0, 0);
        }
        return;
    }

    if (w->mapped && w->has_last_g && w->scene_tree && w->scene_tree->node.enabled)
        update_window_borders(w, w->last_rw, w->last_rh, &w->last_g);
}

static CompRect
initial_floating_rect(const CompWindow *w, const CompWindowRuleResult *rr, double vw, double vh)
{
    double fw = 0.0, fh = 0.0;
    if (w->toplevel && w->toplevel->base) {
        fw = (double)w->toplevel->base->geometry.width;
        fh = (double)w->toplevel->base->geometry.height;
        if (fw <= 0.0 && w->toplevel->base->surface)
            fw = (double)w->toplevel->base->surface->current.width;
        if (fh <= 0.0 && w->toplevel->base->surface)
            fh = (double)w->toplevel->base->surface->current.height;
    } else if (w->xsurf) {
        fw = (double)w->xsurf->width;
        fh = (double)w->xsurf->height;
    }
    if (fw <= 0.0) fw = fmin(800.0, round(vw * 0.6));
    if (fh <= 0.0) fh = fmin(600.0, round(vh * 0.6));
    double b = (double)server_border_px(w->server);
    fw += 2.0 * b;
    fh += 2.0 * b;

    double fx = round((vw - fw) * 0.5);
    double fy = round((vh - fh) * 0.5);

    const CompWindow *pw = NULL;
    if (w->toplevel && w->toplevel->parent && w->toplevel->parent->base)
        pw = w->toplevel->parent->base->data;
    else if (w->xsurf && w->xsurf->parent)
        pw = w->xsurf->parent->data;
    if (pw && pw->mapped) {
        fx = round(pw->last_screen_rect.x + (pw->last_screen_rect.w - fw) * 0.5);
        fy = round(pw->last_screen_rect.y + (pw->last_screen_rect.h - fh) * 0.5);
    }

    if (rr && rr->has_anchor) {
        switch (rr->corner) {
        case COMP_CORNER_TOP_LEFT:
            fx = rr->anchor_x;
            fy = rr->anchor_y;
            break;
        case COMP_CORNER_TOP_RIGHT:
            fx = vw - fw - rr->anchor_x;
            fy = rr->anchor_y;
            break;
        case COMP_CORNER_BOTTOM_LEFT:
            fx = rr->anchor_x;
            fy = vh - fh - rr->anchor_y;
            break;
        case COMP_CORNER_BOTTOM_RIGHT:
            fx = vw - fw - rr->anchor_x;
            fy = vh - fh - rr->anchor_y;
            break;
        }
    }

    CompRect r = { .x = fx, .y = fy, .w = fw, .h = fh };
    return clamp_floating_rect(r, vw, vh);
}

static void
reapply_dynamic_rules_for_window(CompWindow *w)
{
    CompServer *s = w->server;
    if (!w->mapped)
        return;

    CompWindowRuleResult rr = {0};
    if (s->rules)
        comp_rules_match_window(s->rules, w->app_id, w->title, &rr);

    w->scroll_factor = rr.has_scroll_factor ? rr.scroll_factor : 1.0;
    w->rule_vrr      = rr.has_vrr ? rr.vrr : false;

    /* Spec §5.2.3 & §3.3 O3: if not manually floated/tiled since map and a newly matching
     * rule, parent relationship, or modal xdg_dialog state requires floating, promote to
     * the floating layer. */
    if (!w->manually_floated && !w->is_floating && should_open_floating(w, &rr)) {
        CompRect work = {0};
        comp_server_working_area(s, &work);
        CompRect r = initial_floating_rect(w, &rr, work.w, work.h);
        CompWinId prev_focus = comp_layout_focused_window(s->layout);
        comp_layout_focus_window(s->layout, w->id);
        if (comp_layout_apply(s->layout, COMP_ACT_TOGGLE_WINDOW_FLOATING)) {
            w->is_floating = true;
            comp_layout_set_floating_rect(s->layout, w->id, r);
        }
        if (prev_focus != 0 && prev_focus != w->id)
            comp_layout_focus_window(s->layout, prev_focus);
        comp_window_relayout(s);
    } else {
        comp_output_update_vrr(s);
    }
}

void
comp_window_on_dialog_state_changed(struct wlr_xdg_toplevel *toplevel)
{
    if (!toplevel || !toplevel->base || !toplevel->base->data)
        return;
    CompWindow *w = toplevel->base->data;
    CompServer *s = w->server;
    if (!w->mapped) {
        if (toplevel->base->initialized && w->cfg_tiled != WLR_EDGE_NONE) {
            CompWindowRuleResult rr = {0};
            if (s && s->rules)
                comp_rules_match_window(s->rules, w->app_id, w->title, &rr);
            if (should_open_floating(w, &rr)) {
                w->cfg_w     = 0;
                w->cfg_h     = 0;
                w->cfg_tiled = WLR_EDGE_NONE;
                wlr_xdg_toplevel_set_tiled(toplevel, WLR_EDGE_NONE);
                wlr_xdg_toplevel_set_size(toplevel, 0, 0);
            }
        }
        return;
    }
    reapply_dynamic_rules_for_window(w);
    if (s)
        comp_ipc_emit_windows(s);
}

void
comp_window_reapply_rules(CompServer *s)
{
    if (!s)
        return;
    CompWindow *w;
    wl_list_for_each(w, &s->windows, link) {
        if (w->mapped)
            reapply_dynamic_rules_for_window(w);
    }
}

static void
handle_toplevel_map(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    (void)data;

    if (w->mapped)
        return;

    update_window_strings(w);
    w->id = comp_win_alloc_id(s, w);
    COMP_WARN_RETURN_VOID(w->id != 0);
    w->mapped = true;
    w->manually_floated = false;
    wl_list_insert(&s->windows, &w->link);
    create_foreign_handles(w);

    CompWindowRuleResult rr = {0};
    if (s->rules)
        comp_rules_match_window(s->rules, w->app_id, w->title, &rr);

    w->scroll_factor = rr.has_scroll_factor ? rr.scroll_factor : 1.0;
    w->rule_vrr      = rr.has_vrr ? rr.vrr : false;

    CompRect work = {0};
    comp_server_working_area(s, &work);
    double vw = work.w, vh = work.h;

    CompWinId prev_focus = comp_layout_focused_window(s->layout);
    bool floating = should_open_floating(w, &rr);
    w->is_floating = floating;

    if (floating) {
        CompRect r = initial_floating_rect(w, &rr, vw, vh);
        comp_layout_add_floating(s->layout, w->id, r);
        if (w->scene_tree) {
            wlr_scene_node_reparent(&w->scene_tree->node, s->tree_floating);
            wlr_scene_node_raise_to_top(&w->scene_tree->node);
        }
    } else {
        comp_layout_add_window(s->layout, w->id);
        if (rr.has_width) {
            comp_layout_adjust_column_width(s->layout, -1.0);
            comp_layout_adjust_column_width(s->layout, rr.width - 0.05);
        }
    }

    bool want_fullscreen = (w->toplevel && w->toplevel->requested.fullscreen) ||
                           (w->xsurf && w->xsurf->fullscreen);
    bool want_maximized  = (w->toplevel && w->toplevel->requested.maximized) ||
                           (w->xsurf && (w->xsurf->maximized_horz || w->xsurf->maximized_vert));
    if (want_fullscreen)
        comp_layout_set_fullscreen(s->layout, w->id, true);
    if (want_maximized) {
        if (floating)
            w->maximized_to_edges = true;
        else
            comp_layout_apply(s->layout, COMP_ACT_MAXIMIZE_COLUMN);
    }

    if (rr.has_focus && !rr.focus && prev_focus != 0)
        comp_layout_focus_window(s->layout, prev_focus);

    comp_window_relayout(s);
    comp_ipc_emit_workspaces(s);
    comp_ipc_emit_windows(s);
}

static void
handle_toplevel_unmap(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    (void)data;

    if (!w->mapped)
        return;

    if (s->grab_win == w->id)
        comp_window_end_grab(s);

    destroy_foreign_handles(w);
    CompWinId old_id = w->id;
    comp_layout_remove_window(s->layout, w->id);
    comp_win_free_id(s, w->id);
    w->id = 0;
    w->mapped = false;
    wl_list_remove(&w->link);

    if (w->scene_tree)
        wlr_scene_node_set_enabled(&w->scene_tree->node, false);

    comp_switcher_on_window_unmap(s, old_id);
    comp_window_relayout(s);
    comp_ipc_emit_workspaces(s);
    comp_ipc_emit_windows(s);
}

static void
handle_toplevel_destroy(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    (void)data;
    bool was_mapped = w->mapped;
    talloc_free(w);
    if (was_mapped) {
        comp_window_relayout(s);
        comp_ipc_emit_workspaces(s);
        comp_ipc_emit_windows(s);
    }
}

static void
handle_request_maximize(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    (void)data;
    if (w->mapped && s && !w->is_floating) {
        w->maximized_to_edges = false;
        CompWinId prev_focus = comp_layout_focused_window(s->layout);
        comp_layout_focus_window(s->layout, w->id);
        comp_layout_apply(s->layout, COMP_ACT_MAXIMIZE_COLUMN);
        if (prev_focus != 0 && prev_focus != w->id)
            comp_layout_focus_window(s->layout, prev_focus);
        comp_window_relayout(s);
        comp_ipc_emit_windows(s);
    } else {
        w->maximized_to_edges = w->toplevel->requested.maximized;
        if (w->mapped && s)
            comp_window_relayout(s);
    }
    if (w->toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(w->toplevel->base);
}

static void
handle_request_fullscreen(void *owner, void *data)
{
    CompWindow *w = owner;
    (void)data;
    if (w->mapped) {
        comp_layout_set_fullscreen(w->server->layout, w->id, w->toplevel->requested.fullscreen);
        comp_window_relayout(w->server);
        comp_ipc_emit_windows(w->server);
    }
    if (w->toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(w->toplevel->base);
}

static void
handle_request_minimize(void *owner, void *data)
{
    CompWindow *w = owner;
    (void)data;
    /* Scrolling-tiling does not minimize; acknowledge with a configure event. */
    if (w->toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(w->toplevel->base);
}

static void
handle_request_move(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    (void)data;
    /* Spec §3.11 L8: interactive move is supported on floating windows. */
    if (!w->mapped || !w->is_floating)
        return;
    s->grab_win      = w->id;
    s->grab_resizing = false;
    s->grab_edges    = 0;
    s->grab_start_cx = s->cursor->x;
    s->grab_start_cy = s->cursor->y;
    s->grab_start_rect = w->last_screen_rect;
}

static void
handle_request_resize(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    const struct wlr_xdg_toplevel_resize_event *ev = data;
    if (!w->mapped || !w->is_floating || !ev)
        return;
    s->grab_win      = w->id;
    s->grab_resizing = true;
    s->grab_edges    = ev->edges;
    s->grab_start_cx = s->cursor->x;
    s->grab_start_cy = s->cursor->y;
    s->grab_start_rect = w->last_screen_rect;
    if (w->toplevel->base->initialized)
        wlr_xdg_toplevel_set_resizing(w->toplevel, true);
}

static void
handle_set_parent(void *owner, void *data)
{
    CompWindow *w = owner;
    (void)data;
    if (w && w->toplevel)
        comp_window_on_dialog_state_changed(w->toplevel);
}

static void
handle_set_metadata(void *owner, void *data)
{
    CompWindow *w = owner;
    (void)data;
    update_window_strings(w);
    if (w->mapped) {
        reapply_dynamic_rules_for_window(w);
        comp_ipc_emit_windows(w->server);
    }
}

static void
handle_new_toplevel(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_xdg_toplevel *toplevel = data;

    CompWindow *w = talloc_zero(s, CompWindow);
    COMP_WARN_RETURN_VOID(w != NULL);
    talloc_set_name_const(w, "CompWindow");
    w->server        = s;
    w->toplevel      = toplevel;
    w->scroll_factor = 1.0;
    toplevel->base->data = w;
    talloc_set_destructor(w, window_destructor);

    w->scene_tree = wlr_scene_tree_create(s->tree_tiled);
    if (!w->scene_tree) {
        talloc_free(w);
        return;
    }
    w->scene_tree->node.data = w;
    wlr_scene_node_set_enabled(&w->scene_tree->node, false);

    w->backdrop = wlr_scene_rect_create(w->scene_tree, 0, 0, k_color_backdrop);
    if (w->backdrop)
        wlr_scene_node_set_enabled(&w->backdrop->node, false);

    w->xdg_tree = wlr_scene_xdg_surface_create(w->scene_tree, toplevel->base);
    if (!w->xdg_tree) {
        talloc_free(w);
        return;
    }

    for (int i = 0; i < 4; i++)
        w->border[i] = wlr_scene_rect_create(w->scene_tree, 0, 0, k_color_inactive);

    w->popup_tree = wlr_scene_tree_create(w->scene_tree);
    if (!w->popup_tree) {
        talloc_free(w);
        return;
    }

    update_window_strings(w);

    struct wlr_surface *surf = toplevel->base->surface;
    bool ok = comp_listen(w, &surf->events.commit, handle_toplevel_commit) &&
              comp_listen(w, &surf->events.map, handle_toplevel_map) &&
              comp_listen(w, &surf->events.unmap, handle_toplevel_unmap) &&
              comp_listen(w, &toplevel->events.destroy, handle_toplevel_destroy) &&
              comp_listen(w, &toplevel->events.request_maximize, handle_request_maximize) &&
              comp_listen(w, &toplevel->events.request_fullscreen, handle_request_fullscreen) &&
              comp_listen(w, &toplevel->events.request_minimize, handle_request_minimize) &&
              comp_listen(w, &toplevel->events.request_move, handle_request_move) &&
              comp_listen(w, &toplevel->events.request_resize, handle_request_resize) &&
              comp_listen(w, &toplevel->events.set_parent, handle_set_parent) &&
              comp_listen(w, &toplevel->events.set_title, handle_set_metadata) &&
              comp_listen(w, &toplevel->events.set_app_id, handle_set_metadata);
    if (!ok)
        talloc_free(w);
}

/* ── XWayland managed window hooks (called from comp_xwayland.c) ── */

struct CompWindow *
comp_window_create_xwayland(CompServer *s, struct wlr_xwayland_surface *xsurf)
{
    if (!s || !xsurf)
        return NULL;
    CompWindow *w = talloc_zero(s, CompWindow);
    COMP_WARN_RETURN(w != NULL, NULL);
    talloc_set_name_const(w, "CompWindow");
    w->server        = s;
    w->xsurf         = xsurf;
    w->scroll_factor = 1.0;
    xsurf->data      = w;
    talloc_set_destructor(w, window_destructor);

    w->scene_tree = wlr_scene_tree_create(s->tree_tiled);
    if (!w->scene_tree) {
        talloc_free(w);
        return NULL;
    }
    w->scene_tree->node.data = w;
    wlr_scene_node_set_enabled(&w->scene_tree->node, false);

    w->backdrop = wlr_scene_rect_create(w->scene_tree, 0, 0, k_color_backdrop);
    if (w->backdrop)
        wlr_scene_node_set_enabled(&w->backdrop->node, false);

    for (int i = 0; i < 4; i++)
        w->border[i] = wlr_scene_rect_create(w->scene_tree, 0, 0, k_color_inactive);

    w->popup_tree = wlr_scene_tree_create(w->scene_tree);
    if (!w->popup_tree) {
        talloc_free(w);
        return NULL;
    }
    update_window_strings(w);
    return w;
}

void
comp_window_map_xwayland(struct CompWindow *w)
{
    if (!w || w->mapped || !w->xsurf || !w->xsurf->surface || !w->scene_tree)
        return;
    if (!w->xdg_tree) {
        w->xdg_tree = wlr_scene_subsurface_tree_create(w->scene_tree, w->xsurf->surface);
        if (!w->xdg_tree)
            return;
        if (w->border[0])
            wlr_scene_node_place_below(&w->xdg_tree->node, &w->border[0]->node);
    }
    w->border_state_valid = false;
    handle_toplevel_map(w, NULL);
}

void
comp_window_unmap_xwayland(struct CompWindow *w)
{
    if (!w)
        return;
    handle_toplevel_unmap(w, NULL);
    if (w->server && w->server->scene && w->xdg_tree) {
        wlr_scene_node_destroy(&w->xdg_tree->node);
        w->xdg_tree = NULL;
    }
    w->border_state_valid = false;
}

void
comp_window_update_xwayland_metadata(struct CompWindow *w)
{
    if (!w)
        return;
    handle_set_metadata(w, NULL);
}

void
comp_window_set_xwayland_fullscreen(struct CompWindow *w, bool fullscreen)
{
    if (!w || !w->mapped || !w->server)
        return;
    comp_layout_set_fullscreen(w->server->layout, w->id, fullscreen);
    comp_window_relayout(w->server);
    comp_ipc_emit_windows(w->server);
}

void
comp_window_set_xwayland_maximized(struct CompWindow *w, bool maximized)
{
    if (!w || !w->server)
        return;
    w->maximized_to_edges = maximized;
    if (w->mapped) {
        comp_window_relayout(w->server);
        comp_ipc_emit_windows(w->server);
    }
}

void
comp_window_init(CompServer *s)
{
    s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
    if (!s->xdg_shell) {
        wlr_log(WLR_ERROR, "could not create xdg_shell");
        return;
    }
    if (!comp_listen_ctx(s->listeners, s, &s->xdg_shell->events.new_toplevel, handle_new_toplevel))
        wlr_log(WLR_ERROR, "could not register xdg-shell listeners");
}
