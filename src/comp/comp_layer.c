/* comp_layer.c — zwlr_layer_shell_v1 (v5) surfaces, exclusive zones, layer rules,
 * keyboard interactivity (panel, launcher, OSK, notifications, OSD, screensaver).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <math.h>
#include <string.h>
#include <strings.h>
#include <talloc.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

typedef struct {
    struct wl_list                   link;        /* CompServer.layer_surfaces */
    CompServer                      *server;
    struct wlr_layer_surface_v1     *lsurf;
    struct wlr_scene_layer_surface_v1 *scene_layer;
    struct wl_list                   popups;      /* CompLayerPopup.link */
    enum zwlr_layer_shell_v1_layer   cur_layer;
    bool                             follows_primary;
    bool                             mapped;
} CompLayerSurface;

typedef struct {
    struct wl_list        link;        /* CompLayerSurface.popups */
    CompLayerSurface     *parent_layer;
    struct wlr_xdg_popup *popup;
    struct wlr_scene_tree *popup_tree;
} CompLayerPopup;

static struct wlr_scene_tree *
layer_tree_for(CompServer *s, enum zwlr_layer_shell_v1_layer layer)
{
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return s->tree_background;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return s->tree_bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return s->tree_top;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return s->tree_overlay;
    }
    return s->tree_top;
}

static bool
is_osk_layer_surface(const struct wlr_layer_surface_v1 *lsurf)
{
    if (!lsurf || !lsurf->namespace)
        return false;
    return strcasecmp(lsurf->namespace, "osk") == 0 ||
           strcasecmp(lsurf->namespace, "kajo-osk") == 0;
}

static struct wlr_scene_tree *
layer_tree_for_surface(CompServer *s, const struct wlr_layer_surface_v1 *lsurf,
                       enum zwlr_layer_shell_v1_layer layer)
{
    if (s->locked && s->tree_lock && is_osk_layer_surface(lsurf))
        return s->tree_lock;
    return layer_tree_for(s, layer);
}

static void
set_buf_opacity_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    (void)sx; (void)sy;
    const float *opacity = data;
    wlr_scene_buffer_set_opacity(buffer, *opacity);
}

static void
apply_layer_rules(CompLayerSurface *cls)
{
    CompServer *s = cls->server;
    if (!cls->scene_layer || !cls->scene_layer->tree)
        return;
    const char *ns = cls->lsurf->namespace ? cls->lsurf->namespace : "";
    CompLayerRuleResult rr = {0};
    if (s->rules)
        comp_rules_match_layer(s->rules, ns, &rr);
    float op = rr.has_opacity ? (float)rr.opacity : 1.0f;
    wlr_scene_node_for_each_buffer(&cls->scene_layer->tree->node, set_buf_opacity_iter, &op);

    float pop_op = rr.has_popup_opacity ? (float)rr.popup_opacity : op;
    CompLayerPopup *clp;
    wl_list_for_each(clp, &cls->popups, link) {
        if (clp->popup_tree)
            wlr_scene_node_for_each_buffer(&clp->popup_tree->node, set_buf_opacity_iter, &pop_op);
    }
}

static void
arrange_layers_on_box(CompServer *s, struct wlr_output *target_wo, bool is_primary,
                      const struct wlr_box *full_area, struct wlr_box *usable_io,
                      struct wlr_surface **exclusive_surf_io)
{
    static const enum zwlr_layer_shell_v1_layer k_order[4] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };

    /* Pass 1: exclusive-zone surfaces (exclusive_zone > 0) */
    for (size_t li = 0; li < 4; li++) {
        CompLayerSurface *cls;
        wl_list_for_each(cls, &s->layer_surfaces, link) {
            if (!cls->lsurf->initialized || cls->lsurf->current.layer != k_order[li])
                continue;
            struct wlr_output *lo = cls->lsurf->output;
            if (target_wo && lo != target_wo && !(is_primary && (cls->follows_primary || !lo)))
                continue;
            if (cls->lsurf->current.exclusive_zone > 0)
                wlr_scene_layer_surface_v1_configure(cls->scene_layer, full_area, usable_io);
        }
    }

    /* Pass 2: non-exclusive surfaces (exclusive_zone <= 0) & keyboard interactivity */
    for (size_t li = 0; li < 4; li++) {
        CompLayerSurface *cls;
        wl_list_for_each(cls, &s->layer_surfaces, link) {
            if (!cls->lsurf->initialized || cls->lsurf->current.layer != k_order[li])
                continue;
            struct wlr_output *lo = cls->lsurf->output;
            if (target_wo && lo != target_wo && !(is_primary && (cls->follows_primary || !lo)))
                continue;
            if (cls->lsurf->current.exclusive_zone <= 0)
                wlr_scene_layer_surface_v1_configure(cls->scene_layer, full_area, usable_io);
            if (cls->mapped)
                apply_layer_rules(cls);

            if (cls->mapped && !*exclusive_surf_io &&
                (k_order[li] == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ||
                 k_order[li] == ZWLR_LAYER_SHELL_V1_LAYER_TOP) &&
                cls->lsurf->current.keyboard_interactive ==
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
                *exclusive_surf_io = cls->lsurf->surface;
            }
        }
    }
}

void
comp_layer_arrange(CompServer *s)
{
    if (!s)
        return;

    double vw = 0.0, vh = 0.0;
    comp_server_viewport_size(s, &vw, &vh);

    struct wlr_box primary_usable = {
        .x      = 0,
        .y      = 0,
        .width  = (int)vw,
        .height = (int)vh,
    };
    struct wlr_surface *exclusive_surf = NULL;
    struct wlr_output *primary_wo = comp_server_primary_output(s);
    bool arranged_any = false;

    CompLayerSurface *cls;
    wl_list_for_each(cls, &s->layer_surfaces, link) {
        if (cls->follows_primary && primary_wo)
            cls->lsurf->output = primary_wo;
        struct wlr_output *lo = cls->lsurf->output;
        bool on_disabled_output = (!cls->follows_primary && lo && !lo->enabled);
        if (cls->scene_layer && cls->scene_layer->tree) {
            struct wlr_scene_tree *want_parent =
                layer_tree_for_surface(s, cls->lsurf, cls->cur_layer);
            if (want_parent && cls->scene_layer->tree->node.parent != want_parent) {
                wlr_scene_node_reparent(&cls->scene_layer->tree->node, want_parent);
                if (want_parent == s->tree_lock)
                    wlr_scene_node_raise_to_top(&cls->scene_layer->tree->node);
            }
            wlr_scene_node_set_enabled(&cls->scene_layer->tree->node, !on_disabled_output);
        }
        if (on_disabled_output && cls->lsurf->initialized && !cls->lsurf->configured) {
            struct wlr_box dummy_usable = primary_usable;
            wlr_scene_layer_surface_v1_configure(cls->scene_layer, &primary_usable, &dummy_usable);
        }
    }

    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_output *wo = o->wlr_output;
        if (!wo || !wo->enabled)
            continue;
        struct wlr_box full_area = {0};
        comp_server_output_box(s, wo, &full_area);
        struct wlr_box usable = full_area;
        bool is_primary = (wo == primary_wo);
        arrange_layers_on_box(s, wo, is_primary, &full_area, &usable, &exclusive_surf);
        if (is_primary)
            primary_usable = usable;
        arranged_any = true;
    }

    if (!arranged_any) {
        struct wlr_box full_area = primary_usable;
        arrange_layers_on_box(s, NULL, true, &full_area, &primary_usable, &exclusive_surf);
    }

    struct wlr_box usable = primary_usable;

    if (usable.width < 1)  usable.width = 1;
    if (usable.height < 1) usable.height = 1;

    CompRect new_usable = {
        .x = (double)usable.x,
        .y = (double)usable.y,
        .w = (double)usable.width,
        .h = (double)usable.height,
    };

    bool area_changed = !s->has_usable_area ||
                        fabs(new_usable.x - s->usable_area.x) > 0.5 ||
                        fabs(new_usable.y - s->usable_area.y) > 0.5 ||
                        fabs(new_usable.w - s->usable_area.w) > 0.5 ||
                        fabs(new_usable.h - s->usable_area.h) > 0.5;
    s->usable_area     = new_usable;
    s->has_usable_area = true;

    bool focus_changed = (s->exclusive_layer_surface != exclusive_surf);
    s->exclusive_layer_surface = exclusive_surf;

    if (area_changed) {
        comp_window_relayout(s);
        comp_xwayland_update_workarea(s);
    }
    if (focus_changed)
        comp_server_restore_keyboard_focus(s);
}

/* ── Layer popup unconstraining ── */

static void
unconstrain_layer_popup(CompLayerPopup *clp)
{
    if (!clp || !clp->popup || !clp->popup->base->initialized || !clp->parent_layer)
        return;

    double vw = 0.0, vh = 0.0;
    comp_server_viewport_size(clp->parent_layer->server, &vw, &vh);

    int lx = 0, ly = 0;
    if (clp->parent_layer->scene_layer && clp->parent_layer->scene_layer->tree)
        wlr_scene_node_coords(&clp->parent_layer->scene_layer->tree->node, &lx, &ly);

    struct wlr_box box = {
        .x      = -lx,
        .y      = -ly,
        .width  = (int)vw,
        .height = (int)vh,
    };
    wlr_xdg_popup_unconstrain_from_box(clp->popup, &box);
}

static int
layer_popup_destructor(CompLayerPopup *clp)
{
    wl_list_remove(&clp->link);
    return 0;
}

static void
handle_layer_popup_commit(void *owner, void *data)
{
    CompLayerPopup *clp = owner;
    (void)data;
    if (clp->popup->base->initial_commit) {
        unconstrain_layer_popup(clp);
        if (clp->parent_layer && clp->parent_layer->mapped)
            apply_layer_rules(clp->parent_layer);
    }
}

static void
handle_layer_popup_map(void *owner, void *data)
{
    CompLayerPopup *clp = owner;
    (void)data;
    if (clp->parent_layer && clp->parent_layer->mapped)
        apply_layer_rules(clp->parent_layer);
}

static void
handle_layer_popup_reposition(void *owner, void *data)
{
    CompLayerPopup *clp = owner;
    (void)data;
    unconstrain_layer_popup(clp);
}

static void
handle_layer_popup_destroy(void *owner, void *data)
{
    CompLayerPopup *clp = owner;
    (void)data;
    talloc_free(clp);
}

static void
handle_layer_new_popup(void *owner, void *data)
{
    CompLayerSurface *cls = owner;
    struct wlr_xdg_popup *popup = data;

    if (!cls->scene_layer || !cls->scene_layer->tree)
        return;
    struct wlr_scene_tree *ptree = wlr_scene_xdg_surface_create(cls->scene_layer->tree, popup->base);

    CompLayerPopup *clp = talloc_zero(cls, CompLayerPopup);
    COMP_WARN_RETURN_VOID(clp != NULL);
    clp->parent_layer = cls;
    clp->popup        = popup;
    clp->popup_tree   = ptree;
    wl_list_insert(&cls->popups, &clp->link);
    talloc_set_destructor(clp, layer_popup_destructor);

    struct wlr_surface *surf = popup->base->surface;
    if (!comp_listen(clp, &surf->events.commit, handle_layer_popup_commit) ||
        !comp_listen(clp, &surf->events.map, handle_layer_popup_map) ||
        !comp_listen(clp, &popup->events.reposition, handle_layer_popup_reposition) ||
        !comp_listen(clp, &popup->events.destroy, handle_layer_popup_destroy)) {
        talloc_free(clp);
    }
}

/* ── Layer surface lifecycle ── */

static int
layer_surface_destructor(CompLayerSurface *cls)
{
    wl_list_remove(&cls->link);
    return 0;
}

static void
handle_layer_commit(void *owner, void *data)
{
    CompLayerSurface *cls = owner;
    CompServer *s = cls->server;
    (void)data;

    if (!cls->lsurf->initialized)
        return;

    enum zwlr_layer_shell_v1_layer new_layer = cls->lsurf->current.layer;
    if (new_layer != cls->cur_layer && cls->scene_layer && cls->scene_layer->tree) {
        cls->cur_layer = new_layer;
        wlr_scene_node_reparent(&cls->scene_layer->tree->node,
                                layer_tree_for_surface(s, cls->lsurf, new_layer));
    }

    uint32_t committed = cls->lsurf->current.committed;
    if (cls->lsurf->initial_commit || committed != 0)
        comp_layer_arrange(s);
}

static void
handle_layer_map(void *owner, void *data)
{
    CompLayerSurface *cls = owner;
    (void)data;
    cls->mapped = true;
    apply_layer_rules(cls);
    comp_layer_arrange(cls->server);
}

static void
handle_layer_unmap(void *owner, void *data)
{
    CompLayerSurface *cls = owner;
    (void)data;
    cls->mapped = false;
    comp_layer_arrange(cls->server);
}

static void
handle_layer_destroy(void *owner, void *data)
{
    CompLayerSurface *cls = owner;
    CompServer *s = cls->server;
    (void)data;
    if (s->exclusive_layer_surface == cls->lsurf->surface)
        s->exclusive_layer_surface = NULL;
    talloc_free(cls);
    comp_layer_arrange(s);
}

static void
handle_layer_output_destroy(void *owner, void *data)
{
    CompLayerSurface *cls = owner;
    (void)data;
    if (!cls || !cls->lsurf)
        return;
    cls->lsurf->output = NULL;
    wlr_layer_surface_v1_destroy(cls->lsurf);
}

static void
handle_new_layer_surface(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_layer_surface_v1 *lsurf = data;

    bool follows_primary = (lsurf->output == NULL);
    if (follows_primary) {
        lsurf->output = comp_server_primary_output(s);
        if (!lsurf->output) {
            wlr_layer_surface_v1_destroy(lsurf);
            return;
        }
    }

    CompLayerSurface *cls = talloc_zero(s, CompLayerSurface);
    COMP_WARN_RETURN_VOID(cls != NULL);
    talloc_set_name_const(cls, "CompLayerSurface");
    cls->server          = s;
    cls->lsurf           = lsurf;
    cls->cur_layer       = lsurf->pending.layer;
    cls->follows_primary = follows_primary;
    lsurf->data          = cls;
    wl_list_init(&cls->popups);
    wl_list_insert(&s->layer_surfaces, &cls->link);
    talloc_set_destructor(cls, layer_surface_destructor);

    struct wlr_scene_tree *parent = layer_tree_for_surface(s, lsurf, cls->cur_layer);
    cls->scene_layer = wlr_scene_layer_surface_v1_create(parent, lsurf);
    if (!cls->scene_layer) {
        talloc_free(cls);
        return;
    }

    struct wlr_surface *surf = lsurf->surface;
    bool ok = comp_listen(cls, &surf->events.commit, handle_layer_commit) &&
              comp_listen(cls, &surf->events.map, handle_layer_map) &&
              comp_listen(cls, &surf->events.unmap, handle_layer_unmap) &&
              comp_listen(cls, &lsurf->events.new_popup, handle_layer_new_popup) &&
              comp_listen(cls, &lsurf->events.destroy, handle_layer_destroy);
    if (ok && !follows_primary && lsurf->output)
        ok = comp_listen(cls, &lsurf->output->events.destroy, handle_layer_output_destroy);
    if (!ok) {
        talloc_free(cls);
        return;
    }
}

void
comp_layer_init(CompServer *s)
{
    wl_list_init(&s->layer_surfaces);
    s->layer_shell = wlr_layer_shell_v1_create(s->display, 5);
    if (!s->layer_shell) {
        wlr_log(WLR_ERROR, "could not create zwlr_layer_shell_v1");
        return;
    }
    if (!comp_listen_ctx(s->listeners, s, &s->layer_shell->events.new_surface, handle_new_layer_surface))
        wlr_log(WLR_ERROR, "could not listen for layer_shell surfaces");
}
