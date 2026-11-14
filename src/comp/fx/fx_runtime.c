/* fx_runtime.c — anchor lifecycle, timeline scheduling, and kfx hook dispatch (CB1/CB2). */
#include "fx_internal.h"

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

static const uint32_t k_blank_pixel = 0x00000000u;

static void
blank_buf_destroy(struct wlr_buffer *buffer)
{
    CompFxBlankBuffer *blk = wl_container_of(buffer, blk, base);
    wlr_buffer_finish(buffer);
    free(blk);
}

static bool
blank_buf_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags,
                                void **data, uint32_t *format, size_t *stride)
{
    (void)buffer;
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
    *data   = (void *)&k_blank_pixel;
    *format = DRM_FORMAT_ARGB8888;
    *stride = sizeof(k_blank_pixel);
    return true;
}

static void
blank_buf_end_data_ptr_access(struct wlr_buffer *buffer)
{
    (void)buffer;
}

static const struct wlr_buffer_impl k_blank_buf_impl = {
    .destroy               = blank_buf_destroy,
    .begin_data_ptr_access = blank_buf_begin_data_ptr_access,
    .end_data_ptr_access   = blank_buf_end_data_ptr_access,
};

static bool
anchor_never_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    (void)buffer;
    (void)sx;
    (void)sy;
    return false;
}

static void
damage_slot_anchor(CompFxSlot *slot)
{
    if (!slot || !slot->anchor || !slot->enabled || !slot->surface || !slot->surface->sys)
        return;
    CompFxSystem *sys = slot->surface->sys;
    if (!sys->blank_buf)
        return;
    wlr_scene_buffer_set_buffer(slot->anchor, &sys->blank_buf->base);
}

static void
handle_slot_anchor_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    CompFxSlot *slot = wl_container_of(listener, slot, anchor_destroy);
    wl_list_remove(&slot->anchor_destroy.link);
    wl_list_init(&slot->anchor_destroy.link);
    wlr_kfx_node_hook_finish(&slot->hook);
    slot->anchor  = NULL;
    slot->enabled = false;
}

static void
slot_hook_render(struct wlr_kfx_node_hook *hook, enum wlr_kfx_hook_stage stage,
                 const struct wlr_kfx_render_ctx *ctx)
{
    CompFxSlot *slot = wl_container_of(hook, slot, hook);
    const CompFxEffectDesc *desc = fx_registry_get(slot->kind);
    if (!desc || stage != desc->stage || !slot->surface || !slot->surface->sys)
        return;
    CompFxSystem *sys = slot->surface->sys;
    if (!sys->supported || !sys->cfg.enabled || !slot->enabled)
        return;
    if (desc->draw)
        desc->draw(sys, slot->surface, ctx);
}

static const struct wlr_kfx_node_hook_impl k_slot_hook_impl = {
    .render = slot_hook_render,
};

static void
slot_finish(CompFxSlot *slot)
{
    if (!slot)
        return;
    wlr_kfx_node_hook_finish(&slot->hook);
    wl_list_remove(&slot->anchor_destroy.link);
    wl_list_init(&slot->anchor_destroy.link);
    if (slot->anchor) {
        struct wlr_scene_buffer *buf = slot->anchor;
        slot->anchor = NULL;
        wlr_scene_node_destroy(&buf->node);
    }
    slot->enabled = false;
}

static bool
slot_ensure_created(CompFxSurface *fx, FxSlotKind kind)
{
    CompFxSlot *slot = &fx->slots[kind];
    if (slot->anchor)
        return true;
    if (!fx->parent_tree || !fx->sys || !fx->sys->blank_buf)
        return false;

    const CompFxEffectDesc *desc = fx_registry_get(kind);
    if (!desc)
        return false;

    slot->anchor = wlr_scene_buffer_create(fx->parent_tree, &fx->sys->blank_buf->base);
    if (!slot->anchor)
        return false;
    slot->anchor->point_accepts_input = anchor_never_accepts_input;
    wlr_scene_node_set_enabled(&slot->anchor->node, false);

    if (!wlr_kfx_node_hook_init(&slot->hook, &slot->anchor->node,
                                desc->stage, &k_slot_hook_impl)) {
        wlr_scene_node_destroy(&slot->anchor->node);
        slot->anchor = NULL;
        return false;
    }

    slot->anchor_destroy.notify = handle_slot_anchor_destroy;
    wl_signal_add(&slot->anchor->node.events.destroy, &slot->anchor_destroy);
    slot->enabled = false;
    return true;
}

static void
place_slot_anchor(CompFxSlot *slot, const CompFxSurfaceState *st)
{
    if (!slot || !slot->anchor)
        return;
    if (slot->kind == FX_SLOT_SHADOW) {
        if (st->shadow_below && st->shadow_below->parent == slot->anchor->node.parent)
            wlr_scene_node_place_below(&slot->anchor->node, st->shadow_below);
        else
            wlr_scene_node_lower_to_bottom(&slot->anchor->node);
    } else if (slot->kind == FX_SLOT_DIM) {
        if (st->dim_above && st->dim_above->parent == slot->anchor->node.parent)
            wlr_scene_node_place_above(&slot->anchor->node, st->dim_above);
        else
            wlr_scene_node_raise_to_top(&slot->anchor->node);
    }
}

static void
refresh_surface_slots(CompFxSurface *fx)
{
    if (!fx || !fx->sys)
        return;
    CompFxSystem *sys = fx->sys;

    if (!sys->supported || !sys->cfg.enabled) {
        for (int i = 0; i < FX_SLOT_COUNT; i++) {
            CompFxSlot *slot = &fx->slots[i];
            if (slot->anchor && slot->enabled) {
                wlr_scene_node_set_enabled(&slot->anchor->node, false);
                slot->enabled = false;
            }
        }
        return;
    }

    for (int i = 0; i < FX_SLOT_COUNT; i++) {
        FxSlotKind kind = (FxSlotKind)i;
        const CompFxEffectDesc *desc = fx_registry_get(kind);
        CompFxSlot *slot = &fx->slots[i];
        bool want = desc && desc->should_enable && desc->should_enable(sys, fx);
        if (!want) {
            if (slot->anchor && slot->enabled) {
                wlr_scene_node_set_enabled(&slot->anchor->node, false);
                slot->enabled = false;
            }
            continue;
        }

        if (!slot_ensure_created(fx, kind))
            continue;

        struct wlr_box box = desc->compute_box ? desc->compute_box(sys, fx) : fx->state.outer_box;
        if (box.width <= 0 || box.height <= 0) {
            if (slot->enabled) {
                wlr_scene_node_set_enabled(&slot->anchor->node, false);
                slot->enabled = false;
            }
            continue;
        }

        bool box_changed = (box.x != slot->local_box.x || box.y != slot->local_box.y ||
                            box.width != slot->local_box.width || box.height != slot->local_box.height);
        if (!slot->enabled || box_changed) {
            wlr_scene_node_set_position(&slot->anchor->node, box.x, box.y);
            wlr_scene_buffer_set_dest_size(slot->anchor, box.width, box.height);
            slot->local_box = box;
        }

        place_slot_anchor(slot, &fx->state);

        if (!slot->enabled) {
            wlr_scene_node_set_enabled(&slot->anchor->node, true);
            slot->enabled = true;
        }
    }
}

static bool
motion_animations_enabled(const CompServer *s)
{
    if (!s || !s->config)
        return true;
    return s->config->motion.enabled;
}

static double
motion_slowdown(const CompServer *s)
{
    if (!s || !s->config)
        return 1.0;
    return s->config->motion.slowdown > 0.0 ? s->config->motion.slowdown : 1.0;
}

static void
update_focus_springs(CompFxSurface *fx, double now_s)
{
    CompFxSystem *sys = fx->sys;
    if (!sys)
        return;

    bool is_win = (fx->kind == COMP_FX_SURFACE_WINDOW);
    double sh_target = (!is_win || fx->state.is_focused) ? 1.0 : sys->cfg.shadow.unfocused_scale;
    double dm_target = (!is_win || fx->state.is_focused || !sys->cfg.dim.enabled) ? 0.0 : 1.0;

    CompSpringParams sh_p = {0}, dm_p = {0};
    bool anim_en = motion_animations_enabled(sys->server);
    double slow  = motion_slowdown(sys->server);
    bool sh_anim = anim_en && comp_fx_focus_spring_params(sys->cfg.shadow.duration_ms, slow, &sh_p);
    bool dm_anim = anim_en && comp_fx_focus_spring_params(sys->cfg.dim.duration_ms, slow, &dm_p);

    if (!fx->spring_init || !fx->state.mapped) {
        fx->spring_init   = fx->state.mapped;
        fx->was_focused   = fx->state.is_focused;
        fx->shadow_target = sh_target;
        fx->dim_target    = dm_target;
        CompSpringParams fallback = { .damping_ratio = 1.0, .stiffness = 1000.0, .epsilon = 0.01 };
        comp_spring_init(&fx->shadow_spring, sh_anim ? &sh_p : &fallback, sh_target, sh_target, now_s);
        comp_spring_init(&fx->dim_spring, dm_anim ? &dm_p : &fallback, dm_target, dm_target, now_s);
        fx->slots[FX_SLOT_SHADOW].last_opacity = (float)sh_target;
        fx->slots[FX_SLOT_DIM].last_opacity    = (float)dm_target;
        return;
    }

    if (fabs(sh_target - fx->shadow_target) > 1e-6) {
        fx->shadow_target = sh_target;
        if (sh_anim) {
            fx->shadow_spring.params = sh_p;
            comp_spring_retarget(&fx->shadow_spring, sh_target, now_s);
        } else {
            comp_spring_init(&fx->shadow_spring, &fx->shadow_spring.params, sh_target, sh_target, now_s);
            fx->slots[FX_SLOT_SHADOW].last_opacity = (float)sh_target;
            damage_slot_anchor(&fx->slots[FX_SLOT_SHADOW]);
        }
    }

    if (fabs(dm_target - fx->dim_target) > 1e-6) {
        fx->dim_target = dm_target;
        if (dm_anim) {
            fx->dim_spring.params = dm_p;
            comp_spring_retarget(&fx->dim_spring, dm_target, now_s);
        } else {
            comp_spring_init(&fx->dim_spring, &fx->dim_spring.params, dm_target, dm_target, now_s);
            fx->slots[FX_SLOT_DIM].last_opacity = (float)dm_target;
            damage_slot_anchor(&fx->slots[FX_SLOT_DIM]);
        }
    }

    fx->was_focused = fx->state.is_focused;
}

static void
handle_parent_tree_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    CompFxSurface *fx = wl_container_of(listener, fx, tree_destroy);
    wl_list_remove(&fx->tree_destroy.link);
    wl_list_init(&fx->tree_destroy.link);
    for (int i = 0; i < FX_SLOT_COUNT; i++)
        slot_finish(&fx->slots[i]);
    fx->parent_tree = NULL;
}

static bool
init_all_shaders(CompFxSystem *sys)
{
    for (int i = 0; i < FX_SLOT_COUNT; i++) {
        const CompFxEffectDesc *desc = fx_registry_get((FxSlotKind)i);
        if (desc && desc->init_shaders) {
            if (!desc->init_shaders(sys))
                return false;
        }
    }
    return true;
}

static void
destroy_all_shaders(CompFxSystem *sys)
{
    for (int i = 0; i < FX_SLOT_COUNT; i++) {
        const CompFxEffectDesc *desc = fx_registry_get((FxSlotKind)i);
        if (desc && desc->destroy_shaders)
            desc->destroy_shaders(sys);
    }
}

bool
fx_compute_outer_buf_box(const CompFxSurface *fx,
                         const struct wlr_kfx_render_ctx *ctx,
                         CompFxRect *out_outer_buf,
                         float *out_scale, int *out_transform)
{
    if (!fx || !fx->parent_tree || !ctx || !ctx->output || !ctx->output->output)
        return false;

    int px = 0, py = 0;
    if (!wlr_scene_node_coords(&fx->parent_tree->node, &px, &py))
        return false;

    struct wlr_output *wo = ctx->output->output;
    int trans_w = 0, trans_h = 0;
    wlr_output_transformed_resolution(wo, &trans_w, &trans_h);

    CompFxRect outer_logical = {
        .x      = px + fx->state.outer_box.x,
        .y      = py + fx->state.outer_box.y,
        .width  = fx->state.outer_box.width,
        .height = fx->state.outer_box.height,
    };
    CompFxRect output_logical = {
        .x      = ctx->output->x,
        .y      = ctx->output->y,
        .width  = 0,
        .height = 0,
    };

    if (out_outer_buf) {
        *out_outer_buf = comp_fx_transform_box(
            outer_logical, output_logical, wo->scale, (int)wo->transform, trans_w, trans_h);
    }
    if (out_scale)
        *out_scale = wo->scale > 0.0f ? wo->scale : 1.0f;
    if (out_transform)
        *out_transform = (int)wo->transform;
    return true;
}

bool
comp_fx_init(struct CompServer *server)
{
    if (!server)
        return false;
    if (server->fx)
        return true;

    CompFxSystem *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return false;
    sys->server = server;
    wl_list_init(&sys->surfaces);
    comp_fx_config_defaults(&sys->cfg);

    sys->blank_buf = calloc(1, sizeof(*sys->blank_buf));
    if (!sys->blank_buf) {
        free(sys);
        return false;
    }
    wlr_buffer_init(&sys->blank_buf->base, &k_blank_buf_impl, 1, 1);

    sys->supported = server->renderer && wlr_kfx_is_supported(server->renderer);
    if (!sys->supported) {
        wlr_log(WLR_INFO, "kfx: renderer does not support custom shaders (requires Vulkan); effects disabled");
        sys->logged_unsupported = true;
    } else if (!init_all_shaders(sys)) {
        wlr_log(WLR_ERROR, "kfx: shader initialization failed; disabling effects");
        destroy_all_shaders(sys);
        sys->supported = false;
    }

    server->fx = sys;
    return true;
}

void
comp_fx_finish(struct CompServer *server)
{
    if (!server || !server->fx)
        return;
    CompFxSystem *sys = server->fx;

    CompFxSurface *fx, *tmp;
    wl_list_for_each_safe(fx, tmp, &sys->surfaces, link) {
        wl_list_remove(&fx->tree_destroy.link);
        wl_list_init(&fx->tree_destroy.link);
        for (int i = 0; i < FX_SLOT_COUNT; i++)
            slot_finish(&fx->slots[i]);
        wl_list_remove(&fx->link);
        wl_list_init(&fx->link);
        fx->sys         = NULL;
        fx->parent_tree = NULL;
    }

    destroy_all_shaders(sys);

    if (sys->blank_buf) {
        wlr_buffer_drop(&sys->blank_buf->base);
        sys->blank_buf = NULL;
    }

    server->fx = NULL;
    free(sys);
}

void
comp_fx_config_apply(struct CompServer *server, const struct CompConfig *cfg)
{
    if (!server || !server->fx)
        return;
    CompFxSystem *sys = server->fx;
    if (cfg)
        sys->cfg = cfg->fx;
    else
        comp_fx_config_defaults(&sys->cfg);

    double now_s = comp_now_sec();
    CompFxSurface *fx;
    wl_list_for_each(fx, &sys->surfaces, link) {
        update_focus_springs(fx, now_s);
        refresh_surface_slots(fx);
        for (int i = 0; i < FX_SLOT_COUNT; i++) {
            if (fx->slots[i].enabled)
                damage_slot_anchor(&fx->slots[i]);
        }
    }
}

void
comp_fx_renderer_lost(struct CompServer *server)
{
    if (!server || !server->fx)
        return;
    CompFxSystem *sys = server->fx;

    destroy_all_shaders(sys);
    sys->supported = server->renderer && wlr_kfx_is_supported(server->renderer);
    if (sys->supported) {
        if (!init_all_shaders(sys)) {
            destroy_all_shaders(sys);
            sys->supported = false;
        }
    } else if (!sys->logged_unsupported) {
        wlr_log(WLR_INFO, "kfx: new renderer does not support custom shaders; effects disabled");
        sys->logged_unsupported = true;
    }

    CompFxSurface *fx;
    wl_list_for_each(fx, &sys->surfaces, link) {
        refresh_surface_slots(fx);
        for (int i = 0; i < FX_SLOT_COUNT; i++) {
            if (fx->slots[i].enabled)
                damage_slot_anchor(&fx->slots[i]);
        }
    }
}

bool
comp_fx_output_prepare(struct CompOutput *output, double now_s)
{
    if (!output || !output->server || !output->server->fx)
        return false;
    CompFxSystem *sys = output->server->fx;
    if (!sys->supported || !sys->cfg.enabled)
        return false;

    bool any_animating = false;
    CompFxSurface *fx;
    wl_list_for_each(fx, &sys->surfaces, link) {
        if (!fx->state.mapped)
            continue;

        if (fx->slots[FX_SLOT_SHADOW].enabled && fx->spring_init) {
            bool done = comp_spring_is_done(&fx->shadow_spring, now_s);
            double val = done ? fx->shadow_target : comp_spring_value(&fx->shadow_spring, now_s);
            float fval = (float)val;
            if (!done || fabsf(fval - fx->slots[FX_SLOT_SHADOW].last_opacity) > 1e-4f) {
                fx->slots[FX_SLOT_SHADOW].last_opacity = fval;
                damage_slot_anchor(&fx->slots[FX_SLOT_SHADOW]);
            }
            if (!done)
                any_animating = true;
        }

        if (fx->spring_init && sys->cfg.dim.enabled && fx->kind == COMP_FX_SURFACE_WINDOW) {
            bool done = comp_spring_is_done(&fx->dim_spring, now_s);
            double val = done ? fx->dim_target : comp_spring_value(&fx->dim_spring, now_s);
            float fval = (float)val;
            bool changed = !done || fabsf(fval - fx->slots[FX_SLOT_DIM].last_opacity) > 1e-4f;
            if (changed) {
                fx->slots[FX_SLOT_DIM].last_opacity = fval;
                refresh_surface_slots(fx);
                if (fx->slots[FX_SLOT_DIM].enabled)
                    damage_slot_anchor(&fx->slots[FX_SLOT_DIM]);
            }
            if (!done)
                any_animating = true;
        }
    }
    return any_animating;
}

void
comp_fx_background_changed(struct CompOutput *output)
{
    (void)output; /* M3 cached backdrop serial hook */
}

CompFxSurface *
comp_fx_surface_create(struct CompServer *server,
                       struct wlr_scene_tree *parent_tree,
                       CompFxSurfaceKind kind)
{
    if (!server || !server->fx || !parent_tree)
        return NULL;
    CompFxSystem *sys = server->fx;

    CompFxSurface *fx = calloc(1, sizeof(*fx));
    if (!fx)
        return NULL;
    fx->sys         = sys;
    fx->parent_tree = parent_tree;
    fx->kind        = kind;
    for (int i = 0; i < FX_SLOT_COUNT; i++) {
        fx->slots[i].surface = fx;
        fx->slots[i].kind    = (FxSlotKind)i;
        wl_list_init(&fx->slots[i].anchor_destroy.link);
    }

    fx->tree_destroy.notify = handle_parent_tree_destroy;
    wl_signal_add(&parent_tree->node.events.destroy, &fx->tree_destroy);
    wl_list_insert(&sys->surfaces, &fx->link);
    return fx;
}

void
comp_fx_surface_sync(CompFxSurface *fx, const CompFxSurfaceState *state)
{
    if (!fx || !fx->sys || !state)
        return;

    bool rule_changed = (fx->state.has_rule_shadow != state->has_rule_shadow ||
                         fx->state.rule_shadow != state->rule_shadow);
    bool outer_changed = (fx->state.outer_box.x != state->outer_box.x ||
                          fx->state.outer_box.y != state->outer_box.y ||
                          fx->state.outer_box.width != state->outer_box.width ||
                          fx->state.outer_box.height != state->outer_box.height);
    fx->state = *state;

    update_focus_springs(fx, comp_now_sec());
    refresh_surface_slots(fx);

    if (rule_changed || outer_changed) {
        for (int i = 0; i < FX_SLOT_COUNT; i++) {
            if (fx->slots[i].enabled)
                damage_slot_anchor(&fx->slots[i]);
        }
    }
}

void
comp_fx_surface_destroy(CompFxSurface *fx)
{
    if (!fx)
        return;
    wl_list_remove(&fx->tree_destroy.link);
    wl_list_init(&fx->tree_destroy.link);
    for (int i = 0; i < FX_SLOT_COUNT; i++)
        slot_finish(&fx->slots[i]);
    wl_list_remove(&fx->link);
    wl_list_init(&fx->link);
    free(fx);
}

bool
comp_fx_is_active(const struct CompServer *server)
{
    return server && server->fx && server->fx->supported && server->fx->cfg.enabled;
}

struct wlr_render_pass *
comp_fx_screenshot_begin_pass(struct CompServer *server, struct wlr_buffer *buffer)
{
    if (!server || !server->renderer || !buffer)
        return NULL;
    return wlr_kfx_renderer_begin_buffer_pass(
        server->renderer, buffer, NULL, comp_fx_is_active(server));
}

bool
comp_fx_screenshot_render_node(struct CompServer *server,
                               struct wlr_render_pass *pass,
                               struct wlr_scene_node *node,
                               const struct wlr_box *dst_box,
                               const struct wlr_box *clip_box,
                               int origin_lx, int origin_ly,
                               double scale, int pass_w, int pass_h)
{
    if (!server || !server->fx || !pass || !node || !dst_box || !clip_box)
        return false;
    CompFxSystem *sys = server->fx;

    CompFxSlot *matched = NULL;
    CompFxSurface *fx;
    wl_list_for_each(fx, &sys->surfaces, link) {
        for (int i = 0; i < FX_SLOT_COUNT; i++) {
            CompFxSlot *slot = &fx->slots[i];
            if (slot->anchor && &slot->anchor->node == node) {
                matched = slot;
                break;
            }
        }
        if (matched)
            break;
    }
    if (!matched)
        return false;

    /* Always consume FX anchor nodes so blank_buf is never sampled literally. */
    if (!sys->supported || !sys->cfg.enabled || !matched->enabled)
        return true;
    if (clip_box->width <= 0 || clip_box->height <= 0)
        return true;

    const CompFxEffectDesc *desc = fx_registry_get(matched->kind);
    if (!desc || !desc->draw)
        return true;

    struct wlr_output dummy_wo = {
        .width     = pass_w,
        .height    = pass_h,
        .scale     = (float)(scale > 0.0 ? scale : 1.0),
        .transform = WL_OUTPUT_TRANSFORM_NORMAL,
    };
    struct wlr_scene_output dummy_so = {
        .output = &dummy_wo,
        .x      = origin_lx,
        .y      = origin_ly,
    };
    pixman_region32_t clip_reg;
    pixman_region32_init_rect(&clip_reg, clip_box->x, clip_box->y,
                              (unsigned int)clip_box->width,
                              (unsigned int)clip_box->height);

    struct wlr_kfx_render_ctx ctx = {
        .render_pass = pass,
        .dst_box     = *dst_box,
        .clip        = &clip_reg,
        .output      = &dummy_so,
    };
    desc->draw(sys, matched->surface, &ctx);

    pixman_region32_fini(&clip_reg);
    return true;
}

#else

#include <stddef.h>
#include <wlr/render/wlr_renderer.h>

bool comp_fx_init(struct CompServer *server) { (void)server; return true; }
void comp_fx_finish(struct CompServer *server) { (void)server; }
void comp_fx_config_apply(struct CompServer *server, const struct CompConfig *cfg) { (void)server; (void)cfg; }
void comp_fx_renderer_lost(struct CompServer *server) { (void)server; }
bool comp_fx_output_prepare(struct CompOutput *output, double now_s) { (void)output; (void)now_s; return false; }
void comp_fx_background_changed(struct CompOutput *output) { (void)output; }
CompFxSurface *comp_fx_surface_create(struct CompServer *s, struct wlr_scene_tree *p, CompFxSurfaceKind k) {
    (void)s; (void)p; (void)k; return NULL;
}
void comp_fx_surface_sync(CompFxSurface *fx, const CompFxSurfaceState *state) { (void)fx; (void)state; }
void comp_fx_surface_destroy(CompFxSurface *fx) { (void)fx; }
bool comp_fx_is_active(const struct CompServer *server) { (void)server; return false; }
struct wlr_render_pass *comp_fx_screenshot_begin_pass(struct CompServer *server, struct wlr_buffer *buffer) {
    if (!server || !server->renderer || !buffer)
        return NULL;
    return wlr_renderer_begin_buffer_pass(server->renderer, buffer, NULL);
}
bool comp_fx_screenshot_render_node(struct CompServer *server, struct wlr_render_pass *pass,
                                    struct wlr_scene_node *node, const struct wlr_box *dst_box,
                                    const struct wlr_box *clip_box, int origin_lx, int origin_ly,
                                    double scale, int pass_w, int pass_h) {
    (void)server; (void)pass; (void)node; (void)dst_box; (void)clip_box;
    (void)origin_lx; (void)origin_ly; (void)scale; (void)pass_w; (void)pass_h;
    return false;
}

#endif /* WLR_HAS_KFX */
