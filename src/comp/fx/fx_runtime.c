/* fx_runtime.c — anchor lifecycle, timeline scheduling, and kfx hook dispatch (CB1/CB2). */
#include "fx_internal.h"

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/color.h>
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
    if (slot->anchor)
        slot->anchor->node.data = NULL;
    slot->magic = 0;
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
    slot->magic = 0;
    wlr_kfx_node_hook_finish(&slot->hook);
    wl_list_remove(&slot->anchor_destroy.link);
    wl_list_init(&slot->anchor_destroy.link);
    if (slot->anchor) {
        struct wlr_scene_buffer *buf = slot->anchor;
        buf->node.data = NULL;
        slot->anchor   = NULL;
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
    slot->magic                       = COMP_FX_SLOT_MAGIC;
    slot->anchor->node.data           = slot;
    slot->anchor->point_accepts_input = anchor_never_accepts_input;
    wlr_scene_node_set_enabled(&slot->anchor->node, false);

    if (!wlr_kfx_node_hook_init(&slot->hook, &slot->anchor->node,
                                desc->stage, &k_slot_hook_impl)) {
        slot->magic             = 0;
        slot->anchor->node.data = NULL;
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
    } else if (slot->kind == FX_SLOT_GLASS) {
        if (st->shadow_below && st->shadow_below->parent == slot->anchor->node.parent)
            wlr_scene_node_place_below(&slot->anchor->node, st->shadow_below);
        else
            wlr_scene_node_lower_to_bottom(&slot->anchor->node);
        CompFxSurface *fx = slot->surface;
        if (fx && fx->slots[FX_SLOT_SHADOW].anchor &&
            fx->slots[FX_SLOT_SHADOW].anchor->node.parent == slot->anchor->node.parent) {
            wlr_scene_node_place_above(&slot->anchor->node,
                                       &fx->slots[FX_SLOT_SHADOW].anchor->node);
        }
    } else if (slot->kind == FX_SLOT_DIM) {
        if (st->dim_above && st->dim_above->parent == slot->anchor->node.parent)
            wlr_scene_node_place_above(&slot->anchor->node, st->dim_above);
        else
            wlr_scene_node_raise_to_top(&slot->anchor->node);
    }
}

static void
output_track_destroy(CompFxOutputTrack *track)
{
    if (!track)
        return;
    wl_list_remove(&track->output_destroy.link);
    wl_list_init(&track->output_destroy.link);
    wl_list_remove(&track->link);
    free(track);
}

static void
handle_output_track_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    CompFxOutputTrack *track = wl_container_of(listener, track, output_destroy);
    output_track_destroy(track);
}

static CompFxOutputTrack *
ensure_output_track(CompFxSystem *sys, struct wlr_scene_output *so)
{
    if (!sys || !so)
        return NULL;
    CompFxOutputTrack *track;
    wl_list_for_each(track, &sys->output_tracks, link) {
        if (track->scene_output == so)
            return track;
    }

    track = calloc(1, sizeof(*track));
    if (!track)
        return NULL;
    track->scene_output = so;
    track->bg_serial    = 1;
    if (so->output) {
        track->last_w         = so->output->width;
        track->last_h         = so->output->height;
        track->last_scale     = so->output->scale;
        track->last_transform = (int)so->output->transform;
    }
    track->output_destroy.notify = handle_output_track_destroy;
    wl_signal_add(&so->events.destroy, &track->output_destroy);
    wl_list_insert(&sys->output_tracks, &track->link);
    return track;
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
        fx_glass_surface_clear_targets(fx);
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
            if (kind == FX_SLOT_GLASS)
                fx_glass_surface_clear_targets(fx);
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
            if (kind == FX_SLOT_GLASS)
                fx_glass_surface_clear_targets(fx);
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
    fx_glass_surface_clear_targets(fx);
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
    if (!fx_transition_init_shaders(sys))
        return false;
    return true;
}

static void
destroy_all_shaders(CompFxSystem *sys)
{
    fx_transition_destroy_shaders(sys);
    for (int i = 0; i < FX_SLOT_COUNT; i++) {
        const CompFxEffectDesc *desc = fx_registry_get((FxSlotKind)i);
        if (desc && desc->destroy_shaders)
            desc->destroy_shaders(sys);
    }
}

static void
update_system_color_transform(CompFxSystem *sys, bool force_rebuild)
{
    if (!sys)
        return;

    float mat[9];
    bool active = sys->supported && sys->cfg.enabled &&
                  comp_fx_color_filter_matrix(&sys->cfg, mat);
    if (!active) {
        if (sys->color_transform) {
            wlr_color_transform_unref(sys->color_transform);
            sys->color_transform = NULL;
        }
        if (sys->hdr_screenshot_transform) {
            wlr_color_transform_unref(sys->hdr_screenshot_transform);
            sys->hdr_screenshot_transform = NULL;
        }
        sys->has_color_matrix = false;
        return;
    }

    if (!force_rebuild && sys->has_color_matrix && sys->color_transform) {
        bool same = true;
        for (int i = 0; i < 9; i++) {
            if (fabsf(sys->color_matrix[i] - mat[i]) > 1e-5f) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }

    if (sys->hdr_screenshot_transform) {
        wlr_color_transform_unref(sys->hdr_screenshot_transform);
        sys->hdr_screenshot_transform = NULL;
    }

    struct wlr_color_transform *m_tr = wlr_color_transform_init_matrix(mat);
    struct wlr_color_transform *e_tr = wlr_color_transform_init_linear_to_inverse_eotf(
        WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
    struct wlr_color_transform *pipe = NULL;
    if (m_tr && e_tr) {
        struct wlr_color_transform *stages[2] = { m_tr, e_tr };
        pipe = wlr_color_transform_init_pipeline(stages, 2);
    }
    wlr_color_transform_unref(m_tr);
    wlr_color_transform_unref(e_tr);

    wlr_color_transform_unref(sys->color_transform);
    sys->color_transform = pipe;
    if (pipe) {
        memcpy(sys->color_matrix, mat, sizeof(mat));
        sys->has_color_matrix = true;
    } else {
        sys->has_color_matrix = false;
    }
}

bool
fx_compute_slot_pass_boxes(const CompFxSurface *fx,
                           FxSlotKind slot_kind,
                           const struct wlr_kfx_render_ctx *ctx,
                           CompFxRect *out_full_slot_buf,
                           CompFxRect *out_pass_outer_buf,
                           float *out_eff_scale,
                           int *out_transform)
{
    if (!fx || !fx->parent_tree || !ctx || !ctx->output || !ctx->output->output)
        return false;
    if (slot_kind < 0 || slot_kind >= FX_SLOT_COUNT)
        return false;

    int px = 0, py = 0;
    if (!wlr_scene_node_coords(&fx->parent_tree->node, &px, &py))
        return false;

    struct wlr_output *wo = ctx->output->output;
    int trans_w = 0, trans_h = 0;
    wlr_output_transformed_resolution(wo, &trans_w, &trans_h);
    float base_scale = wo->scale > 0.0f ? wo->scale : 1.0f;

    CompFxRect output_logical = {
        .x      = ctx->output->x,
        .y      = ctx->output->y,
        .width  = 0,
        .height = 0,
    };

    const struct wlr_box *slot_local = &fx->slots[slot_kind].local_box;
    CompFxRect slot_logical = {
        .x      = px + slot_local->x,
        .y      = py + slot_local->y,
        .width  = slot_local->width,
        .height = slot_local->height,
    };
    CompFxRect full_slot_buf = comp_fx_transform_box(
        slot_logical, output_logical, base_scale, (int)wo->transform, trans_w, trans_h);

    CompFxRect outer_logical = {
        .x      = px + fx->state.outer_box.x,
        .y      = py + fx->state.outer_box.y,
        .width  = fx->state.outer_box.width,
        .height = fx->state.outer_box.height,
    };
    CompFxRect full_outer_buf = comp_fx_transform_box(
        outer_logical, output_logical, base_scale, (int)wo->transform, trans_w, trans_h);

    double rx = (full_slot_buf.width > 0 && ctx->dst_box.width > 0)
        ? ((double)ctx->dst_box.width / (double)full_slot_buf.width)
        : 1.0;
    double ry = (full_slot_buf.height > 0 && ctx->dst_box.height > 0)
        ? ((double)ctx->dst_box.height / (double)full_slot_buf.height)
        : 1.0;

    if (out_full_slot_buf)
        *out_full_slot_buf = full_slot_buf;

    if (out_pass_outer_buf) {
        if (ctx->dst_box.x == full_slot_buf.x &&
            ctx->dst_box.y == full_slot_buf.y &&
            ctx->dst_box.width == full_slot_buf.width &&
            ctx->dst_box.height == full_slot_buf.height) {
            *out_pass_outer_buf = full_outer_buf;
        } else {
            out_pass_outer_buf->x = (int32_t)lround(
                (double)ctx->dst_box.x + (double)(full_outer_buf.x - full_slot_buf.x) * rx);
            out_pass_outer_buf->y = (int32_t)lround(
                (double)ctx->dst_box.y + (double)(full_outer_buf.y - full_slot_buf.y) * ry);
            out_pass_outer_buf->width  = (int32_t)lround((double)full_outer_buf.width * rx);
            out_pass_outer_buf->height = (int32_t)lround((double)full_outer_buf.height * ry);
        }
    }

    if (out_eff_scale)
        *out_eff_scale = (float)((double)base_scale * 0.5 * (rx + ry));
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
    wl_list_init(&sys->output_tracks);
    wl_list_init(&sys->transitions);
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

    fx_transition_cancel_all(sys);

    CompFxSurface *fx, *tmp;
    wl_list_for_each_safe(fx, tmp, &sys->surfaces, link) {
        wl_list_remove(&fx->tree_destroy.link);
        wl_list_init(&fx->tree_destroy.link);
        fx_glass_surface_clear_targets(fx);
        for (int i = 0; i < FX_SLOT_COUNT; i++)
            slot_finish(&fx->slots[i]);
        wl_list_remove(&fx->link);
        wl_list_init(&fx->link);
        fx->sys         = NULL;
        fx->parent_tree = NULL;
    }

    CompFxOutputTrack *track, *track_tmp;
    wl_list_for_each_safe(track, track_tmp, &sys->output_tracks, link) {
        output_track_destroy(track);
    }

    destroy_all_shaders(sys);

    if (sys->color_transform) {
        wlr_color_transform_unref(sys->color_transform);
        sys->color_transform = NULL;
    }
    if (sys->hdr_screenshot_transform) {
        wlr_color_transform_unref(sys->hdr_screenshot_transform);
        sys->hdr_screenshot_transform = NULL;
    }
    sys->has_color_matrix = false;

    if (sys->blank_buf) {
        wlr_buffer_drop(&sys->blank_buf->base);
        sys->blank_buf = NULL;
    }

    server->fx = NULL;
    free(sys);
}

static void
apply_output_fx_color_settings(CompFxSystem *sys, struct CompOutput *out)
{
    if (!sys || !out || !out->wlr_output)
        return;
    struct wlr_output *wo = out->wlr_output;
    if (!sys->supported || !sys->cfg.enabled) {
        wlr_kfx_output_set_dither(wo, false);
        wlr_kfx_output_set_sdr_white_nits(wo, 0.0f);
        wlr_kfx_output_set_tonemap(wo, false, 0.0f);
        return;
    }

    float sdr_white = (float)sys->cfg.sdr_white_nits;
    bool is_pq = out->hdr_active ||
                 (wo->image_description &&
                  wo->image_description->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
    if (is_pq) {
        float edid_cll = (out->hdr_edid_max_cll > 0.0f)
                             ? out->hdr_edid_max_cll
                             : (wo->image_description
                                    ? (float)wo->image_description->mastering_luminance.max
                                    : 0.0f);
        CompFxHdrLuminance lum;
        comp_fx_clamp_hdr_luminance(edid_cll, out->hdr_edid_max_fall, out->hdr_edid_min_luma,
                                    out->hdr_peak_nits, sys->cfg.sdr_white_nits, &lum);
        sdr_white = lum.sdr_white_nits;
    }

    wlr_kfx_output_set_dither(wo, sys->cfg.dither);
    wlr_kfx_output_set_sdr_white_nits(wo, sdr_white);
    wlr_kfx_output_set_tonemap(wo, sys->cfg.tonemap, (float)sys->cfg.tonemap_peak_nits);
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

    update_system_color_transform(sys, false);

    if (server) {
        struct CompOutput *out;
        wl_list_for_each(out, &server->outputs, link) {
            apply_output_fx_color_settings(sys, out);
        }
    }

    fx_glass_system_invalidate_all(sys);
    CompFxOutputTrack *track;
    wl_list_for_each(track, &sys->output_tracks, link) {
        track->bg_serial++;
    }

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

    fx_transition_cancel_all(sys);

    CompFxSurface *fx;
    wl_list_for_each(fx, &sys->surfaces, link) {
        fx_glass_surface_clear_targets(fx);
    }

    destroy_all_shaders(sys);
    if (sys->color_transform) {
        wlr_color_transform_unref(sys->color_transform);
        sys->color_transform = NULL;
    }
    if (sys->hdr_screenshot_transform) {
        wlr_color_transform_unref(sys->hdr_screenshot_transform);
        sys->hdr_screenshot_transform = NULL;
    }
    sys->has_color_matrix = false;

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
    update_system_color_transform(sys, true);

    CompFxOutputTrack *track;
    wl_list_for_each(track, &sys->output_tracks, link) {
        track->bg_serial++;
    }

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
    apply_output_fx_color_settings(sys, output);
    if (!sys->supported || !sys->cfg.enabled)
        return false;

    CompFxOutputTrack *track = ensure_output_track(sys, output->scene_output);
    if (track && output->wlr_output) {
        struct wlr_output *wo = output->wlr_output;
        if (wo->width != track->last_w ||
            wo->height != track->last_h ||
            fabsf(wo->scale - track->last_scale) > 1e-4f ||
            (int)wo->transform != track->last_transform) {
            track->last_w         = wo->width;
            track->last_h         = wo->height;
            track->last_scale     = wo->scale;
            track->last_transform = (int)wo->transform;
            track->bg_serial++;
        }
    }

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

    if (track && fx_glass_prepare_output(sys, output, track, now_s))
        any_animating = true;

    if (fx_transition_prepare_output(sys, now_s))
        any_animating = true;

    return any_animating;
}

void
comp_fx_background_changed(struct CompOutput *output)
{
    if (!output || !output->server || !output->server->fx || !output->scene_output)
        return;
    CompFxOutputTrack *track = ensure_output_track(output->server->fx, output->scene_output);
    if (track)
        track->bg_serial++;
    if (output->wlr_output)
        wlr_output_schedule_frame(output->wlr_output);
}

struct wlr_color_transform *
comp_fx_output_get_color_transform(struct CompOutput *output)
{
    if (!output || !output->server || !output->server->fx)
        return NULL;
    CompFxSystem *sys = output->server->fx;
    if (!sys->supported || !sys->cfg.enabled)
        return NULL;
    return sys->color_transform;
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
    wl_list_init(&fx->glass_outputs);
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
                         fx->state.rule_shadow != state->rule_shadow ||
                         fx->state.has_rule_blur != state->has_rule_blur ||
                         fx->state.rule_blur != state->rule_blur ||
                         fx->state.has_rule_xray != state->has_rule_xray ||
                         fx->state.rule_xray != state->rule_xray ||
                         fx->state.has_rule_glass_style != state->has_rule_glass_style ||
                         fx->state.rule_glass_style != state->rule_glass_style);
    bool outer_changed = (fx->state.outer_box.x != state->outer_box.x ||
                          fx->state.outer_box.y != state->outer_box.y ||
                          fx->state.outer_box.width != state->outer_box.width ||
                          fx->state.outer_box.height != state->outer_box.height ||
                          fx->state.surface_box.x != state->surface_box.x ||
                          fx->state.surface_box.y != state->surface_box.y ||
                          fx->state.surface_box.width != state->surface_box.width ||
                          fx->state.surface_box.height != state->surface_box.height);
    fx->state = *state;
    if (!state->mapped)
        fx_glass_surface_clear_targets(fx);

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
    fx_glass_surface_clear_targets(fx);
    for (int i = 0; i < FX_SLOT_COUNT; i++)
        slot_finish(&fx->slots[i]);
    wl_list_remove(&fx->link);
    wl_list_init(&fx->link);
    free(fx);
}

uint64_t
comp_fx_surface_get_glass_recomputes(const CompFxSurface *fx)
{
    return fx ? fx->glass_recomputes : 0;
}

bool
comp_fx_is_active(const struct CompServer *server)
{
    return server && server->fx && server->fx->supported && server->fx->cfg.enabled;
}

bool
comp_fx_output_get_hdr_caps(struct wlr_output *output, CompFxOutputHdrCaps *out)
{
    if (!out)
        return false;
    *out = (CompFxOutputHdrCaps){0};
    struct wlr_kfx_output_hdr_caps kcaps = {0};
    if (!output || !wlr_kfx_output_get_hdr_caps(output, &kcaps))
        return false;
    out->supported     = kcaps.supported;
    out->max_cll_nits  = kcaps.max_cll_nits;
    out->max_fall_nits = kcaps.max_fall_nits;
    out->min_luma_nits = kcaps.min_luma_nits;
    return true;
}

float
comp_fx_output_get_hdr_headroom(struct wlr_output *output)
{
    if (!output)
        return 1.0f;
    return wlr_kfx_output_get_hdr_headroom(output);
}

float
comp_fx_output_get_sdr_white_nits(struct wlr_output *output)
{
    if (!output)
        return 203.0f;
    return wlr_kfx_output_get_sdr_white_nits(output);
}

static struct wlr_color_transform *
ensure_hdr_screenshot_transform(CompFxSystem *sys)
{
    if (!sys)
        return NULL;
    if (sys->hdr_screenshot_transform)
        return sys->hdr_screenshot_transform;

    struct wlr_color_transform *e_tr = wlr_color_transform_init_linear_to_inverse_eotf(
        WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR);
    if (!e_tr)
        return NULL;

    if (sys->has_color_matrix) {
        struct wlr_color_transform *m_tr = wlr_color_transform_init_matrix(sys->color_matrix);
        struct wlr_color_transform *pipe = NULL;
        if (m_tr) {
            struct wlr_color_transform *stages[2] = { m_tr, e_tr };
            pipe = wlr_color_transform_init_pipeline(stages, 2);
            wlr_color_transform_unref(m_tr);
        }
        wlr_color_transform_unref(e_tr);
        sys->hdr_screenshot_transform = pipe;
        return pipe;
    }

    sys->hdr_screenshot_transform = e_tr;
    return e_tr;
}

struct wlr_render_pass *
comp_fx_screenshot_begin_hdr_pass(struct CompServer *server, struct wlr_buffer *buffer)
{
    if (!server || !server->renderer || !buffer || !server->fx || !server->fx->supported)
        return NULL;

    double now_s = comp_now_sec();
    struct CompOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        (void)comp_fx_output_prepare(out, now_s);
    }

    struct wlr_color_transform *tr = ensure_hdr_screenshot_transform(server->fx);
    if (!tr)
        return NULL;

    struct wlr_buffer_pass_options pass_opts = {
        .color_transform = tr,
    };
    return wlr_kfx_renderer_begin_buffer_pass(server->renderer, buffer, &pass_opts, true);
}

struct wlr_render_pass *
comp_fx_screenshot_begin_pass(struct CompServer *server, struct wlr_buffer *buffer)
{
    if (!server || !server->renderer || !buffer)
        return NULL;
    bool active = comp_fx_is_active(server);
    if (active) {
        double now_s = comp_now_sec();
        struct CompOutput *out;
        wl_list_for_each(out, &server->outputs, link) {
            (void)comp_fx_output_prepare(out, now_s);
        }
    }
    struct wlr_buffer_pass_options pass_opts = {
        .color_transform = (active && server->fx) ? server->fx->color_transform : NULL,
    };
    struct wlr_render_pass *pass = wlr_kfx_renderer_begin_buffer_pass(
        server->renderer, buffer, pass_opts.color_transform ? &pass_opts : NULL, active);
    if (pass && active)
        wlr_kfx_render_pass_set_dither(pass, server->fx->cfg.dither);
    return pass;
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

    CompFxSlot *matched = fx_slot_from_scene_node(sys, node);
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

    float shot_scale = (float)(scale > 0.0 ? scale : 1.0);
    struct wlr_output dummy_wo = {
        .width     = pass_w,
        .height    = pass_h,
        .scale     = shot_scale,
        .transform = WL_OUTPUT_TRANSFORM_NORMAL,
    };
    wlr_addon_set_init(&dummy_wo.addons);
    struct wlr_scene_output dummy_so = {
        .output = &dummy_wo,
        .x      = origin_lx,
        .y      = origin_ly,
    };

    if (matched->kind != FX_SLOT_GLASS) {
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

    int32_t shot_eff_w = 0, shot_eff_h = 0;
    comp_fx_effective_resolution(pass_w, pass_h, shot_scale, &shot_eff_w, &shot_eff_h);
    struct wlr_box shot_box = {
        .x      = origin_lx,
        .y      = origin_ly,
        .width  = shot_eff_w > 0 ? shot_eff_w : 1,
        .height = shot_eff_h > 0 ? shot_eff_h : 1,
    };
    CompFxRect shot_logical = {
        .x      = origin_lx,
        .y      = origin_ly,
        .width  = shot_box.width,
        .height = shot_box.height,
    };

    struct CompOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (!out->scene_output || !out->wlr_output)
            continue;
        int trans_w = 0, trans_h = 0;
        wlr_output_transformed_resolution(out->wlr_output, &trans_w, &trans_h);
        float os = out->wlr_output->scale > 0.0f ? out->wlr_output->scale : 1.0f;
        int32_t out_eff_w = 0, out_eff_h = 0;
        comp_fx_effective_resolution(trans_w, trans_h, os, &out_eff_w, &out_eff_h);
        struct wlr_box out_box = {
            .x      = out->scene_output->x,
            .y      = out->scene_output->y,
            .width  = out_eff_w,
            .height = out_eff_h,
        };
        struct wlr_box inter = {0};
        if (!wlr_box_intersection(&inter, &out_box, &shot_box))
            continue;

        CompFxRect out_shot_r = comp_fx_transform_box(
            (CompFxRect){ out_box.x, out_box.y, out_box.width, out_box.height },
            shot_logical, shot_scale, WL_OUTPUT_TRANSFORM_NORMAL, pass_w, pass_h);
        struct wlr_box out_shot_box = {
            .x      = out_shot_r.x,
            .y      = out_shot_r.y,
            .width  = out_shot_r.width,
            .height = out_shot_r.height,
        };
        struct wlr_box out_clip = {0};
        if (!wlr_box_intersection(&out_clip, clip_box, &out_shot_box))
            continue;

        pixman_region32_t clip_reg;
        pixman_region32_init_rect(&clip_reg, out_clip.x, out_clip.y,
                                  (unsigned int)out_clip.width,
                                  (unsigned int)out_clip.height);
        struct wlr_kfx_render_ctx ctx = {
            .render_pass = pass,
            .dst_box     = *dst_box,
            .clip        = &clip_reg,
            .output      = out->scene_output,
        };
        desc->draw(sys, matched->surface, &ctx);
        pixman_region32_fini(&clip_reg);
    }
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
struct wlr_color_transform *comp_fx_output_get_color_transform(struct CompOutput *output) {
    (void)output;
    return NULL;
}
CompFxSurface *comp_fx_surface_create(struct CompServer *s, struct wlr_scene_tree *p, CompFxSurfaceKind k) {
    (void)s; (void)p; (void)k; return NULL;
}
void comp_fx_surface_sync(CompFxSurface *fx, const CompFxSurfaceState *state) { (void)fx; (void)state; }
void comp_fx_surface_destroy(CompFxSurface *fx) { (void)fx; }
bool comp_fx_is_active(const struct CompServer *server) { (void)server; return false; }
bool comp_fx_output_get_hdr_caps(struct wlr_output *output, CompFxOutputHdrCaps *out) {
    (void)output;
    if (out)
        *out = (CompFxOutputHdrCaps){0};
    return false;
}
float comp_fx_output_get_hdr_headroom(struct wlr_output *output) { (void)output; return 1.0f; }
float comp_fx_output_get_sdr_white_nits(struct wlr_output *output) { (void)output; return 203.0f; }
struct wlr_render_pass *comp_fx_screenshot_begin_pass(struct CompServer *server, struct wlr_buffer *buffer) {
    if (!server || !server->renderer || !buffer)
        return NULL;
    return wlr_renderer_begin_buffer_pass(server->renderer, buffer, NULL);
}
struct wlr_render_pass *comp_fx_screenshot_begin_hdr_pass(struct CompServer *server, struct wlr_buffer *buffer) {
    (void)server;
    (void)buffer;
    return NULL;
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
