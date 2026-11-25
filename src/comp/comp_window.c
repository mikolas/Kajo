/* comp_window.c — xdg-shell windows, XWayland/foreign toplevel lifecycle,
 * map/unmap, focus, close, rules, and CompLayout bridge (Phase 2b).
 */
#include "comp_window_private.h"

#include "comp_log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <talloc.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/util/edges.h>

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

void
comp_server_keyboard_enter(CompServer *s, struct wlr_surface *surface)
{
    if (!s || !s->seat || !surface)
        return;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(s->seat, surface,
                                       kb->keycodes, kb->num_keycodes, &kb->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
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
    comp_server_keyboard_enter(s, surface);
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

CompWindowGeometry *
comp_window_ensure_geom_scratch(CompServer *s, size_t need)
{
    if (need < 32)
        need = 32;
    if (need <= s->geom_scratch_cap && s->geom_scratch)
        return s->geom_scratch;
    size_t ncap = s->geom_scratch_cap ? s->geom_scratch_cap * 2 : 32;
    while (ncap < need)
        ncap *= 2;
    CompWindowGeometry *nb = talloc_realloc(s, s->geom_scratch, CompWindowGeometry, (unsigned)ncap);
    if (!nb)
        return NULL;
    s->geom_scratch     = nb;
    s->geom_scratch_cap = ncap;
    return s->geom_scratch;
}

void
comp_window_relayout(CompServer *s)
{
    if (!s || !s->layout || !s->motion)
        return;

    CompRect work = {0};
    comp_server_working_area(s, &work);
    double vw = work.w, vh = work.h;
    double full_vw = 0.0, full_vh = 0.0;
    comp_server_viewport_size(s, &full_vw, &full_vh);
    double now_s = comp_now_sec();

    comp_motion_sync(s->motion, s->layout, vw, vh, now_s);

    size_t total = comp_layout_window_count(s->layout);
    size_t cap = total + 8;
    CompWindowGeometry *geoms = comp_window_ensure_geom_scratch(s, cap);
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

        if (w->scene_tree && w->scene_tree->node.parent != target_parent) {
            comp_fx_transition_cancel_tree(s, w->scene_tree);
            wlr_scene_node_reparent(&w->scene_tree->node, target_parent);
        }

        int32_t rx = g->fullscreen ? 0 : comp_window_clamp_i32(g->rect.x + work.x, -32768, 32767);
        int32_t ry = g->fullscreen ? 0 : comp_window_clamp_i32(g->rect.y + work.y, -32768, 32767);
        int32_t rw = g->fullscreen ? comp_window_clamp_i32(full_vw, 1, 32767) : comp_window_clamp_i32(g->rect.w, 1, 32767);
        int32_t rh = g->fullscreen ? comp_window_clamp_i32(full_vh, 1, 32767) : comp_window_clamp_i32(g->rect.h, 1, 32767);
        comp_window_configure_toplevel(w, rx, ry, rw, rh, g->focused, g->fullscreen, g->floating);
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

    comp_window_tick_frame(s, now_s);
    comp_lock_idle_update_inhibitors(s);
    comp_input_refresh_pointer_focus(s, (uint32_t)(now_s * 1000.0));
    bool wants_vrr = comp_window_focused_wants_vrr(s);
    if (focus_changed || wants_vrr != s->last_wants_vrr)
        comp_output_update_vrr(s);
    comp_output_update_hdr_auto(s);
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

bool
comp_window_focused_wants_tearing(const CompServer *s)
{
    if (!s || !s->layout || !s->tearing_mgr)
        return false;
    CompWinId id = comp_layout_focused_window(s->layout);
    CompWindow *w = comp_win_from_id(s, id);
    if (!w || !w->mapped || !w->is_fullscreen)
        return false;
    struct wlr_surface *surf = comp_window_get_surface(w);
    if (!surf)
        return false;
    return wlr_tearing_control_manager_v1_surface_hint_from_surface(s->tearing_mgr, surf) ==
           WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

static bool
surface_tree_is_hdr(struct wlr_surface *surface, int depth)
{
    if (!surface || depth > 16)
        return false;
    const struct wlr_image_description_v1_data *data =
        wlr_surface_get_image_description_v1_data(surface);
    if (data && surface->mapped &&
        (data->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ||
         data->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR ||
         (data->has_mastering_luminance && data->mastering_luminance.max > 250.0f))) {
        return true;
    }
    struct wlr_subsurface *sub;
    wl_list_for_each(sub, &surface->current.subsurfaces_below, current.link) {
        if (sub->surface && surface_tree_is_hdr(sub->surface, depth + 1))
            return true;
    }
    wl_list_for_each(sub, &surface->current.subsurfaces_above, current.link) {
        if (sub->surface && surface_tree_is_hdr(sub->surface, depth + 1))
            return true;
    }
    return false;
}

bool
comp_window_has_visible_hdr_surface(const CompServer *s)
{
    if (!s)
        return false;
    const CompWindow *w;
    wl_list_for_each(w, &s->windows, link) {
        if (!w->mapped || !w->scene_tree || !w->scene_tree->node.enabled)
            continue;
        struct wlr_surface *surf = comp_window_get_surface(w);
        if (surf && surface_tree_is_hdr(surf, 0))
            return true;
    }
    return false;
}

static struct wlr_scene_node *
find_surface_node_recursive(struct wlr_scene_node *node, const struct wlr_surface *target)
{
    if (!node)
        return NULL;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sbuf);
        if (ss && ss->surface == target)
            return node;
        return NULL;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            struct wlr_scene_node *found = find_surface_node_recursive(child, target);
            if (found)
                return found;
        }
    }
    return NULL;
}

bool
comp_server_surface_scene_coords(const CompServer *s, const struct wlr_surface *surface,
                                 int *out_lx, int *out_ly)
{
    if (out_lx) *out_lx = 0;
    if (out_ly) *out_ly = 0;
    if (!s || !s->scene || !surface)
        return false;
    struct wlr_scene_node *node = find_surface_node_recursive(&s->scene->tree.node, surface);
    if (!node)
        return false;
    int lx = 0, ly = 0;
    bool enabled = wlr_scene_node_coords(node, &lx, &ly);
    if (out_lx) *out_lx = lx;
    if (out_ly) *out_ly = ly;
    return enabled;
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
comp_window_get_popup_fx_rules(const struct CompWindow *w,
                               bool *out_has_shadow, bool *out_shadow,
                               bool *out_has_blur, bool *out_blur)
{
    if (out_has_shadow)
        *out_has_shadow = w ? w->has_rule_shadow : false;
    if (out_shadow)
        *out_shadow = w ? w->rule_shadow : false;
    if (out_has_blur)
        *out_has_blur = w ? w->has_rule_blur_popups : false;
    if (out_blur)
        *out_blur = w ? w->rule_blur_popups : false;
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
            if (kb)
                held_mods = comp_input_mods_from_wlr(wlr_keyboard_get_modifiers(kb));
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

    case COMP_BIND_TOGGLE_OVERVIEW:
        comp_overview_toggle(s);
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
        if (w->has_last_g && w->scene_tree && w->scene_tree->node.parent) {
            struct wlr_box close_box = {
                .x      = w->scene_tree->node.x,
                .y      = w->scene_tree->node.y,
                .width  = (w->last_outer_w > 0) ? w->last_outer_w : w->last_rw,
                .height = (w->last_outer_h > 0) ? w->last_outer_h : w->last_rh,
            };
            comp_fx_transition_start_close(s, w->scene_tree,
                                           w->scene_tree->node.parent, close_box);
        }
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
    comp_fx_surface_destroy(w->fx);
    w->fx = NULL;
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
            double full_vw = 0.0, full_vh = 0.0;
            comp_server_viewport_size(s, &full_vw, &full_vh);
            comp_window_configure_toplevel(w, 0, 0, comp_window_clamp_i32(full_vw, 1, 32767),
                                           comp_window_clamp_i32(full_vh, 1, 32767), false, true, false);
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
            int32_t tile_w = comp_window_clamp_i32((work.w - gap) * col_prop - gap, 1, 32767);
            int32_t tile_h = comp_window_clamp_i32(work.h - 2.0 * gap, 1, 32767);
            comp_window_configure_toplevel(w, 0, 0, tile_w, tile_h, false, false, false);
        } else {
            wlr_xdg_toplevel_set_size(w->toplevel, 0, 0);
        }
        return;
    }

    if (w->mapped && w->has_last_g && w->scene_tree) {
        bool transitioning = comp_fx_tree_is_transitioning(s, w->scene_tree);
        if (w->scene_tree->node.enabled || transitioning)
            comp_window_update_borders(w, w->last_rw, w->last_rh, &w->last_g);
        if (transitioning) {
            int32_t lw = (w->last_outer_w > 0) ? w->last_outer_w : w->last_rw;
            int32_t lh = (w->last_outer_h > 0) ? w->last_outer_h : w->last_rh;
            int32_t cap_w = (w->was_resizing && w->resize_target_w > 0) ? w->resize_target_w : lw;
            int32_t cap_h = (w->was_resizing && w->resize_target_h > 0) ? w->resize_target_h : lh;
            struct wlr_box live_box = {
                .x      = w->scene_tree->node.x,
                .y      = w->scene_tree->node.y,
                .width  = lw,
                .height = lh,
            };
            comp_fx_transition_update_live(s, w->scene_tree, live_box, cap_w, cap_h, true);
        }
    }
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

    w->scroll_factor        = rr.has_scroll_factor ? rr.scroll_factor : 1.0;
    w->rule_vrr             = rr.has_vrr ? rr.vrr : false;
    w->has_rule_shadow      = rr.has_shadow;
    w->rule_shadow          = rr.shadow;
    w->has_rule_blur        = rr.has_blur;
    w->rule_blur            = rr.blur;
    w->has_rule_blur_popups = rr.has_blur_popups;
    w->rule_blur_popups     = rr.blur_popups;
    w->has_rule_glass_style = rr.has_glass_style;
    w->rule_glass_style     = rr.glass_style;

    /* Spec §5.2.3 & §3.3 O3: if not manually floated/tiled since map and a newly matching
     * rule, parent relationship, or modal xdg_dialog state requires floating, promote to
     * the floating layer. */
    if (!w->manually_floated && !w->is_floating && should_open_floating(w, &rr)) {
        CompRect work = {0};
        comp_server_working_area(s, &work);
        CompRect r = comp_window_initial_floating_rect(w, &rr, work.w, work.h);
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
        if (w->has_last_g)
            comp_window_update_borders(w, w->last_rw, w->last_rh, &w->last_g);
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
    w->pending_open_transition = true;
    w->was_resizing = false;
    w->manually_floated = false;
    wl_list_insert(&s->windows, &w->link);
    create_foreign_handles(w);

    CompWindowRuleResult rr = {0};
    if (s->rules)
        comp_rules_match_window(s->rules, w->app_id, w->title, &rr);

    w->scroll_factor        = rr.has_scroll_factor ? rr.scroll_factor : 1.0;
    w->rule_vrr             = rr.has_vrr ? rr.vrr : false;
    w->has_rule_shadow      = rr.has_shadow;
    w->rule_shadow          = rr.shadow;
    w->has_rule_blur        = rr.has_blur;
    w->rule_blur            = rr.blur;
    w->has_rule_blur_popups = rr.has_blur_popups;
    w->rule_blur_popups     = rr.blur_popups;
    w->has_rule_glass_style = rr.has_glass_style;
    w->rule_glass_style     = rr.glass_style;

    CompRect work = {0};
    comp_server_working_area(s, &work);
    double vw = work.w, vh = work.h;

    CompWinId prev_focus = comp_layout_focused_window(s->layout);
    bool floating = should_open_floating(w, &rr);
    w->is_floating = floating;

    if (floating) {
        CompRect r = comp_window_initial_floating_rect(w, &rr, vw, vh);
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
    comp_overview_on_layout_changed(s);
    comp_protocols_update_surface_preferred_image_description(s, comp_window_get_surface(w));
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

    if (w->has_last_g && w->scene_tree && w->scene_tree->node.parent) {
        struct wlr_box close_box = {
            .x      = w->scene_tree->node.x,
            .y      = w->scene_tree->node.y,
            .width  = (w->last_outer_w > 0) ? w->last_outer_w : w->last_rw,
            .height = (w->last_outer_h > 0) ? w->last_outer_h : w->last_rh,
        };
        comp_fx_transition_start_close(s, w->scene_tree,
                                       w->scene_tree->node.parent, close_box);
    }

    w->has_last_g              = false;
    w->was_resizing            = false;
    w->pending_open_transition = false;
    w->last_outer_w            = 0;
    w->last_outer_h            = 0;

    if (s->grab_win == w->id)
        comp_window_end_grab(s);

    destroy_foreign_handles(w);
    CompWinId old_id = w->id;
    comp_layout_remove_window(s->layout, w->id);
    comp_win_free_id(s, w->id);
    w->id = 0;
    w->mapped = false;
    wl_list_remove(&w->link);

    if (w->fx)
        comp_fx_surface_sync(w->fx, &(CompFxSurfaceState){ .mapped = false });

    if (w->scene_tree)
        wlr_scene_node_set_enabled(&w->scene_tree->node, false);

    comp_switcher_on_window_unmap(s, old_id);
    comp_window_relayout(s);
    comp_overview_on_layout_changed(s);
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

    if (!comp_window_create_chrome(w, toplevel->base)) {
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
              comp_listen(w, &toplevel->events.request_move, comp_window_handle_request_move) &&
              comp_listen(w, &toplevel->events.request_resize, comp_window_handle_request_resize) &&
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

    if (!comp_window_create_chrome(w, NULL)) {
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
