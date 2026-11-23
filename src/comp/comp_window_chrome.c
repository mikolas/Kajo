/* comp_window_chrome.c — 1px borders, backdrops, tab strips, per-frame chrome
 * sync, capture boxes, and interactive floating move/resize.
 */
#include "comp_window_private.h"

#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>

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

bool
comp_window_create_chrome(CompWindow *w, struct wlr_xdg_surface *xdg_surf)
{
    CompServer *s = w->server;
    w->scene_tree = wlr_scene_tree_create(s->tree_tiled);
    if (!w->scene_tree)
        return false;
    w->scene_tree->node.data = w;
    wlr_scene_node_set_enabled(&w->scene_tree->node, false);

    w->backdrop = wlr_scene_rect_create(w->scene_tree, 0, 0, k_color_backdrop);
    if (w->backdrop)
        wlr_scene_node_set_enabled(&w->backdrop->node, false);

    if (xdg_surf) {
        w->xdg_tree = wlr_scene_xdg_surface_create(w->scene_tree, xdg_surf);
        if (!w->xdg_tree)
            return false;
    }

    for (int i = 0; i < 4; i++)
        w->border[i] = wlr_scene_rect_create(w->scene_tree, 0, 0, k_color_inactive);

    w->popup_tree = wlr_scene_tree_create(w->scene_tree);
    if (!w->popup_tree)
        return false;

    w->fx = comp_fx_surface_create(s, w->scene_tree, COMP_FX_SURFACE_WINDOW);
    return true;
}

/* Spec §3.11 L6: at least clamp(floor(dim / 4), 10, 75) px of a floating window
 * must remain inside the working area on all four edges. */
CompRect
comp_window_clamp_floating_rect(CompRect r, double vw, double vh)
{
    if (r.w < 50.0) r.w = 50.0;
    if (r.h < 50.0) r.h = 50.0;
    double vis_w = fmin(75.0, fmax(10.0, floor(r.w / 4.0)));
    double vis_h = fmin(75.0, fmax(10.0, floor(r.h / 4.0)));
    r.x = fmin(vw - vis_w, fmax(vis_w - r.w, r.x));
    r.y = fmin(vh - vis_h, fmax(0.0, r.y));
    return r;
}

CompRect
comp_window_initial_floating_rect(const CompWindow *w, const CompWindowRuleResult *rr,
                                  double vw, double vh)
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
    double b = (double)comp_window_server_border_px(w->server);
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
    return comp_window_clamp_floating_rect(r, vw, vh);
}

static void
update_window_tab_strip(CompWindow *w, int32_t tile_h, const CompWindowGeometry *g,
                        const float col_active[4])
{
    /* Spec §3.9 T3: 4px tab indicator strip at x = -6, 2px vertical gap */
    size_t n_tabs = (g->tabbed && !g->fullscreen && g->tab_count > 0) ? g->tab_count : 0;
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

void
comp_window_update_borders(CompWindow *w, int32_t tile_w, int32_t tile_h,
                           const CompWindowGeometry *g)
{
    CompServer *s = w->server;
    int32_t b = comp_window_server_border_px(s);
    bool fullscreen  = g->fullscreen;
    bool focused     = g->focused;
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
        .tile_w               = tile_w,
        .tile_h               = tile_h,
        .border_px            = b,
        .geom                 = geom,
        .has_mapped_surf      = has_mapped_surf,
        .fullscreen           = fullscreen,
        .focused              = focused,
        .urgent               = w->is_urgent,
        .maximized            = w->maximized_to_edges,
        .tabbed               = g->tabbed,
        .resizing             = g->resizing && !fullscreen,
        .floating             = g->floating,
        .has_rule_shadow      = w->has_rule_shadow,
        .rule_shadow          = w->rule_shadow,
        .has_rule_blur        = w->has_rule_blur,
        .rule_blur            = w->rule_blur,
        .has_rule_glass_style = w->has_rule_glass_style,
        .rule_glass_style     = w->rule_glass_style,
        .mapped               = w->mapped,
        .tab_count            = g->tab_count,
        .tab_index            = g->tab_index,
    };
    memcpy(st.col_active,   col_active,   sizeof(st.col_active));
    memcpy(st.col_inactive, col_inactive, sizeof(st.col_inactive));
    memcpy(st.col_urgent,   col_urgent,   sizeof(st.col_urgent));

    if (w->border_state_valid && memcmp(&st, &w->last_border, sizeof(st)) == 0)
        return;
    w->last_border        = st;
    w->border_state_valid = true;

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

    /* M5: while the displayed size animates, the client buffer may already be smaller
     * than the box (shrinking): fill the box with the backdrop instead of showing what
     * is behind the window. Fullscreen uses the backdrop for the whole output. */
    if (w->backdrop) {
        bool show = fullscreen || st.resizing;
        wlr_scene_node_set_enabled(&w->backdrop->node, show);
        if (fullscreen) {
            wlr_scene_node_set_position(&w->backdrop->node, 0, 0);
            wlr_scene_rect_set_size(w->backdrop, tile_w, tile_h);
        } else if (show) {
            int32_t off = hide_border ? 0 : b;
            wlr_scene_node_set_position(&w->backdrop->node, off, off);
            wlr_scene_rect_set_size(w->backdrop, cw, ch);
        }
    }

    if (has_mapped_surf && w->xdg_tree) {
        struct wlr_box clip = {
            .x      = geom.x,
            .y      = geom.y,
            .width  = cw,
            .height = ch,
        };
        wlr_scene_subsurface_tree_set_clip(&w->xdg_tree->node, &clip);
    }

    int32_t outer_w = cw;
    int32_t outer_h = ch;
    int32_t surf_x  = 0;
    int32_t surf_y  = 0;
    int32_t surf_w  = cw;
    int32_t surf_h  = ch;

    if (!hide_border) {
        /* Borders hug a client that is smaller than its tile, except while the size animates (M5) */
        int32_t bw = (!st.resizing && geom.width > 0 && geom.width < cw) ? geom.width : cw;
        int32_t bh = (!st.resizing && geom.height > 0 && geom.height < ch) ? geom.height : ch;
        outer_w = bw + 2 * b;
        outer_h = bh + 2 * b;
        surf_x  = b;
        surf_y  = b;
        surf_w  = bw;
        surf_h  = bh;
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

    update_window_tab_strip(w, tile_h, g, col_active);

    w->last_outer_w = outer_w;
    w->last_outer_h = outer_h;

    if (w->fx) {
        CompFxSurfaceState fx_st = {
            .outer_box            = { 0, 0, outer_w, outer_h },
            .surface_box          = { surf_x, surf_y, surf_w, surf_h },
            .shadow_below         = w->backdrop ? &w->backdrop->node
                                                : (w->xdg_tree ? &w->xdg_tree->node : NULL),
            .dim_above            = w->xdg_tree ? &w->xdg_tree->node : NULL,
            .has_rule_shadow      = w->has_rule_shadow,
            .rule_shadow          = w->rule_shadow,
            .has_rule_blur        = w->has_rule_blur,
            .rule_blur            = w->rule_blur,
            .has_rule_glass_style = w->has_rule_glass_style,
            .rule_glass_style     = w->rule_glass_style,
            .is_floating          = g->floating,
            .is_fullscreen        = fullscreen,
            .is_focused           = focused,
            .mapped               = w->mapped,
        };
        comp_fx_surface_sync(w->fx, &fx_st);
    }
}

void
comp_window_configure_toplevel(CompWindow *w, int32_t tile_x, int32_t tile_y,
                               int32_t tile_w, int32_t tile_h,
                               bool activated, bool fullscreen, bool floating)
{
    bool is_xdg = (w->toplevel && w->toplevel->base && w->toplevel->base->initialized);
    bool is_x11 = (w->xsurf != NULL);
    if (!is_xdg && !is_x11)
        return;

    bool hide_border = fullscreen || w->maximized_to_edges;
    int32_t b = hide_border ? 0 : comp_window_server_border_px(w->server);
    int32_t cw = tile_w > 2 * b ? tile_w - 2 * b : 1;
    int32_t ch = tile_h > 2 * b ? tile_h - 2 * b : 1;
    uint32_t tiled = (!floating && !fullscreen)
                         ? (WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT)
                         : WLR_EDGE_NONE;

    int32_t xw_x = tile_x + b;
    int32_t xw_y = tile_y + b;

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

void
comp_window_tick_frame(CompServer *s, double now_s)
{
    if (!s || !s->motion)
        return;

    CompRect work = {0};
    comp_server_working_area(s, &work);
    double full_vw = 0.0, full_vh = 0.0;
    comp_server_viewport_size(s, &full_vw, &full_vh);

    size_t total = comp_layout_window_count(s->layout);
    size_t cap = total + 8;
    CompWindowGeometry *geoms = comp_window_ensure_geom_scratch(s, cap);
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

        int32_t rx = g->fullscreen ? 0 : comp_window_clamp_i32(g->rect.x + work.x, -32768, 32767);
        /* WS7: fullscreen windows scroll with their workspace on the strip (rect.y is 0 when settled) */
        int32_t ry = g->fullscreen ? comp_window_clamp_i32(g->rect.y, -32768, 32767)
                                   : comp_window_clamp_i32(g->rect.y + work.y, -32768, 32767);
        int32_t rw = g->fullscreen ? comp_window_clamp_i32(full_vw, 1, 32767) : comp_window_clamp_i32(g->rect.w, 1, 32767);
        int32_t rh = g->fullscreen ? comp_window_clamp_i32(full_vh, 1, 32767) : comp_window_clamp_i32(g->rect.h, 1, 32767);

        int32_t prev_ow = (win->last_outer_w > 0) ? win->last_outer_w : win->last_rw;
        int32_t prev_oh = (win->last_outer_h > 0) ? win->last_outer_h : win->last_rh;
        struct wlr_box prev_box = {
            .x      = win->scene_tree->node.x,
            .y      = win->scene_tree->node.y,
            .width  = prev_ow,
            .height = prev_oh,
        };
        struct wlr_box curr_box = { .x = rx, .y = ry, .width = rw, .height = rh };

        bool already_trans = comp_fx_tree_is_transitioning(s, win->scene_tree);
        bool same_layer    = !g->fullscreen && !win->was_fullscreen && (g->floating == win->was_floating);
        bool start_resize  = !win->pending_open_transition &&
                             !already_trans &&
                             win->has_last_g &&
                             same_layer &&
                             !win->was_resizing && g->resizing &&
                             prev_box.width > 0 && prev_box.height > 0 &&
                             win->scene_tree->node.parent != NULL;

        if (start_resize) {
            int32_t b = (g->fullscreen || win->maximized_to_edges) ? 0 : comp_window_server_border_px(s);
            win->resize_start_w  = prev_box.width;
            win->resize_start_h  = prev_box.height;
            win->resize_target_w = (win->cfg_w > 0) ? (win->cfg_w + 2 * b) : rw;
            win->resize_target_h = (win->cfg_h > 0) ? (win->cfg_h + 2 * b) : rh;
            /* Capture pre-resize snapshot BEFORE updating borders/clip to the new size. */
            comp_fx_transition_start_resize(s, win->scene_tree,
                                            win->scene_tree->node.parent,
                                            prev_box, curr_box);
        }

        wlr_scene_node_set_position(&win->scene_tree->node, rx, ry);
        comp_window_update_borders(win, rw, rh, g);

        struct wlr_box live_box = {
            .x      = rx,
            .y      = ry,
            .width  = (win->last_outer_w > 0) ? win->last_outer_w : rw,
            .height = (win->last_outer_h > 0) ? win->last_outer_h : rh,
        };

        if (win->pending_open_transition) {
            win->pending_open_transition = false;
            if (live_box.width > 0 && live_box.height > 0 && win->scene_tree->node.parent) {
                comp_fx_transition_start_open(s, win->scene_tree,
                                              win->scene_tree->node.parent, live_box);
            }
        }

        bool transitioning = comp_fx_tree_is_transitioning(s, win->scene_tree);
        if (transitioning) {
            bool synced_resize = false;
            if (win->was_resizing || start_resize) {
                double dw = fabs((double)win->resize_target_w - (double)win->resize_start_w);
                double dh = fabs((double)win->resize_target_h - (double)win->resize_start_h);
                double prog = 1.0;
                if (g->resizing) {
                    if (dw >= dh && dw > 1.0)
                        prog = fabs((double)rw - (double)win->resize_start_w) / dw;
                    else if (dh > 1.0)
                        prog = fabs((double)rh - (double)win->resize_start_h) / dh;
                    else
                        prog = 0.0;
                }
                synced_resize = comp_fx_transition_sync_resize(
                    s, win->scene_tree, live_box, prog, !g->resizing);
                if (synced_resize && !g->resizing)
                    transitioning = false;
            }
            if (!synced_resize) {
                comp_fx_transition_update_live(s, win->scene_tree, live_box,
                                               live_box.width, live_box.height, false);
            }
        }

        win->was_resizing   = g->resizing;
        win->was_fullscreen = g->fullscreen;
        win->was_floating   = g->floating;
        wlr_scene_node_set_enabled(&win->scene_tree->node, !transitioning);
    }

    wl_list_for_each(w, &s->windows, link) {
        if (!w->tick_seen) {
            /* Do not play a deferred Open animation later if a window mapped off-screen,
             * and cancel any in-flight transition so its anchor never bleeds across workspaces. */
            w->pending_open_transition = false;
            if (w->scene_tree) {
                comp_fx_transition_cancel_tree(s, w->scene_tree);
                wlr_scene_node_set_enabled(&w->scene_tree->node, false);
            }
        }
    }
}

bool
comp_window_get_capture_box(const struct CompWindow *w, bool include_border,
                            struct wlr_scene_node **out_node, struct wlr_box *out_logical_box)
{
    if (!w || !w->mapped || !w->scene_tree || !w->xdg_tree || !out_node || !out_logical_box)
        return false;

    int32_t b = comp_window_server_border_px(w->server);
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

/* ── Interactive move / resize & column / height resizing ── */

static bool
validate_grab_serial(CompServer *s, CompWindow *w, uint32_t serial)
{
    if (!s || !s->seat || !w)
        return false;
    struct wlr_surface *surf = comp_window_get_surface(w);
    if (!surf)
        return false;
    return wlr_seat_validate_pointer_grab_serial(s->seat, surf, serial) ||
           wlr_seat_validate_touch_grab_serial(s->seat, surf, serial, NULL);
}

void
comp_window_handle_request_move(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    const struct wlr_xdg_toplevel_move_event *ev = data;
    /* Spec §3.11 L8: interactive move is supported on floating windows. */
    if (!w->mapped || !w->is_floating || !ev || !validate_grab_serial(s, w, ev->serial))
        return;
    s->grab_win        = w->id;
    s->grab_mode       = COMP_GRAB_FLOATING_MOVE;
    s->grab_resizing   = false;
    s->grab_from_mod   = false;
    s->grab_button     = 0;
    s->grab_edges      = 0;
    s->grab_start_cx   = s->cursor->x;
    s->grab_start_cy   = s->cursor->y;
    s->grab_last_step_x = s->cursor->x;
    s->grab_last_step_y = s->cursor->y;
    s->grab_start_rect = w->last_screen_rect;
}

void
comp_window_handle_request_resize(void *owner, void *data)
{
    CompWindow *w = owner;
    CompServer *s = w->server;
    const struct wlr_xdg_toplevel_resize_event *ev = data;
    if (!w->mapped || !w->is_floating || !ev || !validate_grab_serial(s, w, ev->serial))
        return;
    s->grab_win        = w->id;
    s->grab_mode       = COMP_GRAB_FLOATING_RESIZE;
    s->grab_resizing   = true;
    s->grab_from_mod   = false;
    s->grab_button     = 0;
    s->grab_edges      = ev->edges;
    s->grab_start_cx   = s->cursor->x;
    s->grab_start_cy   = s->cursor->y;
    s->grab_last_step_x = s->cursor->x;
    s->grab_last_step_y = s->cursor->y;
    s->grab_start_rect = w->last_screen_rect;
    if (w->toplevel && w->toplevel->base->initialized)
        wlr_xdg_toplevel_set_resizing(w->toplevel, true);
}

bool
comp_window_begin_mod_grab(CompServer *s, struct CompWindow *w, bool resize, uint32_t button)
{
    if (!s || !w || !w->mapped || !s->cursor || w->is_fullscreen)
        return false;

    s->grab_win         = w->id;
    s->grab_from_mod    = true;
    s->grab_button      = button;
    s->grab_start_cx    = s->cursor->x;
    s->grab_start_cy    = s->cursor->y;
    s->grab_last_step_x = s->cursor->x;
    s->grab_last_step_y = s->cursor->y;
    s->grab_start_rect  = w->last_screen_rect;
    s->grab_edges       = 0;

    if (w->is_floating) {
        s->grab_mode     = resize ? COMP_GRAB_FLOATING_RESIZE : COMP_GRAB_FLOATING_MOVE;
        s->grab_resizing = resize;
        if (resize) {
            CompRect work = {0};
            comp_server_working_area(s, &work);
            double rel_x = (s->cursor->x - work.x) - w->last_screen_rect.x;
            double rel_y = (s->cursor->y - work.y) - w->last_screen_rect.y;
            s->grab_edges = (rel_x < w->last_screen_rect.w * 0.5 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
                            (rel_y < w->last_screen_rect.h * 0.5 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
            if (w->toplevel && w->toplevel->base->initialized)
                wlr_xdg_toplevel_set_resizing(w->toplevel, true);
        }
    } else {
        s->grab_mode        = resize ? COMP_GRAB_TILED_RESIZE : COMP_GRAB_TILED_MOVE;
        s->grab_resizing    = resize;
        s->grab_start_col_w = w->last_screen_rect.w > 1.0 ? w->last_screen_rect.w : 600.0;
        if (resize) {
            w->maximized_to_edges = false;
            if (w->toplevel && w->toplevel->base->initialized)
                wlr_xdg_toplevel_set_resizing(w->toplevel, true);
        }
    }

    if (s->cursor_mgr) {
        const char *cname = resize
            ? (w->is_floating ? "se-resize" : "ew-resize")
            : "grabbing";
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, cname);
    }
    return true;
}

static void
update_tiled_grab(CompServer *s, CompWindow *w)
{
    CompRect work = {0};
    comp_server_working_area(s, &work);
    double vw = work.w > 1.0 ? work.w : 1280.0;
    double vh = work.h > 1.0 ? work.h : 720.0;
    double gap = (s->config && s->config->layout.gap >= 0.0) ? s->config->layout.gap : 6.0;

    if (s->grab_mode == COMP_GRAB_TILED_RESIZE) {
        double dx = s->cursor->x - s->grab_start_cx;
        double min_w = fmax(160.0, vw * 0.10);
        double max_w = fmax(min_w, vw - 2.0 * gap);
        double target_px = s->grab_start_col_w + dx;
        if (target_px < min_w) target_px = min_w;
        if (target_px > max_w) target_px = max_w;
        double denom = vw - gap;
        double prop = denom > 1.0 ? (target_px + gap) / denom : 0.5;
        if (comp_layout_set_column_width(s->layout, prop)) {
            comp_window_relayout(s);
            comp_ipc_emit_windows(s);
        }
        return;
    }

    /* COMP_GRAB_TILED_MOVE: reorder columns left/right & move across workspaces up/down */
    size_t col = 0;
    size_t ws_idx = comp_layout_active_workspace(s->layout);
    size_t n_cols = comp_layout_column_count(s->layout, ws_idx);
    if (!comp_layout_focused_column(s->layout, &col)) {
        (void)w;
        return;
    }

    double dx_step = s->cursor->x - s->grab_last_step_x;
    double dy_step = s->cursor->y - s->grab_last_step_y;

    double right_thresh = 100.0;
    if (col + 1 < n_cols) {
        double nx = 0.0, nw = 0.0;
        if (comp_layout_column_bounds(s->layout, ws_idx, col + 1, vw, &nx, &nw))
            right_thresh = fmin(140.0, fmax(48.0, nw * 0.45));
    }
    double left_thresh = 100.0;
    if (col > 0) {
        double px = 0.0, pw = 0.0;
        if (comp_layout_column_bounds(s->layout, ws_idx, col - 1, vw, &px, &pw))
            left_thresh = fmin(140.0, fmax(48.0, pw * 0.45));
    }

    bool changed = false;
    if (dx_step > right_thresh && col + 1 < n_cols) {
        if (comp_layout_apply(s->layout, COMP_ACT_MOVE_COLUMN_RIGHT)) {
            s->grab_last_step_x = s->cursor->x;
            changed = true;
        }
    } else if (dx_step < -left_thresh && col > 0) {
        if (comp_layout_apply(s->layout, COMP_ACT_MOVE_COLUMN_LEFT)) {
            s->grab_last_step_x = s->cursor->x;
            changed = true;
        }
    }

    double ws_thresh = fmax(110.0, vh * 0.22);
    if (dy_step > ws_thresh) {
        if (comp_layout_apply(s->layout, COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_DOWN)) {
            s->grab_last_step_y = s->cursor->y;
            changed = true;
        }
    } else if (dy_step < -ws_thresh) {
        if (comp_layout_apply(s->layout, COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_UP)) {
            s->grab_last_step_y = s->cursor->y;
            changed = true;
        }
    }

    if (changed) {
        comp_window_relayout(s);
        comp_ipc_emit_workspaces(s);
        comp_ipc_emit_windows(s);
    }
}

void
comp_window_update_grab(CompServer *s)
{
    if (!s || s->grab_win == 0 || s->grab_mode == COMP_GRAB_NONE)
        return;
    CompWindow *w = comp_win_from_id(s, s->grab_win);
    if (!w || !w->mapped) {
        comp_window_end_grab(s);
        return;
    }

    if (s->grab_mode == COMP_GRAB_TILED_MOVE || s->grab_mode == COMP_GRAB_TILED_RESIZE) {
        if (w->is_floating || w->is_fullscreen) {
            comp_window_end_grab(s);
            return;
        }
        update_tiled_grab(s, w);
        return;
    }

    if (!w->is_floating) {
        comp_window_end_grab(s);
        return;
    }

    double dx = s->cursor->x - s->grab_start_cx;
    double dy = s->cursor->y - s->grab_start_cy;
    CompRect r = s->grab_start_rect;
    int32_t b = comp_window_server_border_px(s);

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

    CompRect work = {0};
    comp_server_working_area(s, &work);
    r = comp_window_clamp_floating_rect(r, work.w, work.h);

    if (comp_layout_set_floating_rect(s->layout, w->id, r))
        comp_window_relayout(s);
}

void
comp_window_end_grab(CompServer *s)
{
    if (!s || (s->grab_win == 0 && s->grab_mode == COMP_GRAB_NONE))
        return;
    CompWindow *w = comp_win_from_id(s, s->grab_win);
    if (w && w->mapped && s->grab_resizing && w->toplevel && w->toplevel->base->initialized)
        wlr_xdg_toplevel_set_resizing(w->toplevel, false);
    if (s->grab_from_mod && s->cursor && s->cursor_mgr)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
    s->grab_win      = 0;
    s->grab_mode     = COMP_GRAB_NONE;
    s->grab_resizing = false;
    s->grab_from_mod = false;
    s->grab_button   = 0;
    s->grab_edges    = 0;
}

static void
resize_layout_axis(CompServer *s, double axis_px, double min_prop,
                   double delta, CompDeltaMode mode,
                   bool (*adjust_fn)(CompLayout *, double))
{
    double d = delta;
    if (mode == COMP_DELTA_REL_PX && axis_px > 0.0)
        d = delta / axis_px;
    if (mode == COMP_DELTA_SET_PROP || mode == COMP_DELTA_SET_PX) {
        double prop = (mode == COMP_DELTA_SET_PROP)
                          ? delta
                          : (axis_px > 0.0 ? delta / axis_px : 0.5);
        adjust_fn(s->layout, -1.0);
        adjust_fn(s->layout, prop - min_prop);
    } else {
        adjust_fn(s->layout, d);
    }
    comp_window_relayout(s);
}

void
comp_window_resize_column(CompServer *s, double delta, CompDeltaMode mode)
{
    if (!s || !s->layout)
        return;
    CompWindow *fw = comp_win_from_id(s, comp_layout_focused_window(s->layout));
    if (fw)
        fw->maximized_to_edges = false;
    double vw = 0.0, vh = 0.0;
    comp_server_viewport_size(s, &vw, &vh);
    resize_layout_axis(s, vw, 0.05, delta, mode, comp_layout_adjust_column_width);
}

void
comp_window_resize_height(CompServer *s, double delta, CompDeltaMode mode)
{
    if (!s || !s->layout)
        return;
    double vw = 0.0, vh = 0.0;
    comp_server_viewport_size(s, &vw, &vh);
    resize_layout_axis(s, vh, 0.1, delta, mode, comp_layout_adjust_window_height);
}
