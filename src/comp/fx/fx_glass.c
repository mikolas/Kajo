/* fx_glass.c — dual-filter Kawase frosted backdrop glass effect (M3, spec §10). */
#include "fx_internal.h"

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

#include "blur_down_frag.h"
#include "blur_up_frag.h"
#include "glass_frag.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#define GLASS_DOWNSCALE 2

static void
glass_output_destroy(CompFxGlassOutput *go)
{
    if (!go)
        return;
    wl_list_remove(&go->output_destroy.link);
    wl_list_init(&go->output_destroy.link);
    if (go->target) {
        wlr_kfx_target_release(go->target);
        go->target = NULL;
    }
    wl_list_remove(&go->link);
    free(go);
}

static void
handle_glass_scene_output_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    CompFxGlassOutput *go = wl_container_of(listener, go, output_destroy);
    glass_output_destroy(go);
}

void
fx_glass_surface_clear_targets(CompFxSurface *fx)
{
    if (!fx)
        return;
    CompFxGlassOutput *go, *tmp;
    wl_list_for_each_safe(go, tmp, &fx->glass_outputs, link) {
        glass_output_destroy(go);
    }
}

void
fx_glass_system_invalidate_all(CompFxSystem *sys)
{
    if (!sys)
        return;
    CompFxSurface *fx;
    wl_list_for_each(fx, &sys->surfaces, link) {
        CompFxGlassOutput *go;
        wl_list_for_each(go, &fx->glass_outputs, link) {
            go->valid = false;
        }
    }
}

static CompFxGlassOutput *
find_glass_output(const CompFxSurface *fx, const struct wlr_scene_output *so)
{
    if (!fx || !so)
        return NULL;
    CompFxGlassOutput *go;
    wl_list_for_each(go, &fx->glass_outputs, link) {
        if (go->scene_output == so)
            return go;
    }
    return NULL;
}

static CompFxGlassOutput *
ensure_glass_output(CompFxSurface *fx, struct wlr_scene_output *so)
{
    CompFxGlassOutput *go = find_glass_output(fx, so);
    if (go)
        return go;

    go = calloc(1, sizeof(*go));
    if (!go)
        return NULL;
    go->surface      = fx;
    go->scene_output = so;
    go->output_destroy.notify = handle_glass_scene_output_destroy;
    wl_signal_add(&so->events.destroy, &go->output_destroy);
    wl_list_insert(&fx->glass_outputs, &go->link);
    return go;
}

static bool
glass_init_shaders(CompFxSystem *sys)
{
    if (!sys || !sys->server || !sys->server->renderer)
        return false;

    if (!sys->blur_down_shader) {
        struct wlr_kfx_shader_layout blur_layout = {
            .n_inputs    = 1,
            .params_size = sizeof(CompFxBlurParams),
        };
        sys->blur_down_shader = wlr_kfx_shader_create(
            sys->server->renderer, blur_down_frag_spv, sizeof(blur_down_frag_spv), &blur_layout);
        if (!sys->blur_down_shader) {
            wlr_log(WLR_ERROR, "kfx: failed to create blur_down fragment shader");
            return false;
        }
    }

    if (!sys->blur_up_shader) {
        struct wlr_kfx_shader_layout blur_layout = {
            .n_inputs    = 1,
            .params_size = sizeof(CompFxBlurParams),
        };
        sys->blur_up_shader = wlr_kfx_shader_create(
            sys->server->renderer, blur_up_frag_spv, sizeof(blur_up_frag_spv), &blur_layout);
        if (!sys->blur_up_shader) {
            wlr_log(WLR_ERROR, "kfx: failed to create blur_up fragment shader");
            return false;
        }
    }

    if (!sys->glass_shader) {
        struct wlr_kfx_shader_layout glass_layout = {
            .n_inputs    = 1,
            .params_size = sizeof(CompFxGlassParams),
        };
        sys->glass_shader = wlr_kfx_shader_create(
            sys->server->renderer, glass_frag_spv, sizeof(glass_frag_spv), &glass_layout);
        if (!sys->glass_shader) {
            wlr_log(WLR_ERROR, "kfx: failed to create glass composite fragment shader");
            return false;
        }
    }

    return true;
}

static void
glass_destroy_shaders(CompFxSystem *sys)
{
    if (!sys)
        return;
    if (sys->blur_down_shader) {
        wlr_kfx_shader_destroy(sys->blur_down_shader);
        sys->blur_down_shader = NULL;
    }
    if (sys->blur_up_shader) {
        wlr_kfx_shader_destroy(sys->blur_up_shader);
        sys->blur_up_shader = NULL;
    }
    if (sys->glass_shader) {
        wlr_kfx_shader_destroy(sys->glass_shader);
        sys->glass_shader = NULL;
    }
}

static bool
glass_should_enable(const CompFxSystem *sys, const CompFxSurface *fx)
{
    if (!sys || !fx || !sys->cfg.enabled || !sys->cfg.glass.enabled ||
        !sys->glass_shader || !sys->blur_down_shader || !sys->blur_up_shader)
        return false;

    const CompFxSurfaceState *st = &fx->state;
    if (!st->mapped || st->is_fullscreen)
        return false;
    if (st->surface_box.width <= 0 || st->surface_box.height <= 0)
        return false;

    if (st->has_rule_blur)
        return st->rule_blur;

    switch (fx->kind) {
    case COMP_FX_SURFACE_POPUP:
        return true;
    case COMP_FX_SURFACE_WINDOW:
    case COMP_FX_SURFACE_LAYER:
        return false;
    }
    return false;
}

static struct wlr_box
glass_compute_box(const CompFxSystem *sys, const CompFxSurface *fx)
{
    (void)sys;
    return fx->state.surface_box;
}

static bool
run_blur_step(struct wlr_kfx_shader *shader,
              struct wlr_kfx_target *dst_target, int dst_w, int dst_h,
              struct wlr_kfx_target *src_target, int src_w, int src_h,
              double offset)
{
    struct wlr_render_pass *pass = wlr_kfx_target_begin_pass(dst_target);
    if (!pass)
        return false;

    int tex_w = 0, tex_h = 0;
    wlr_kfx_target_get_size(src_target, &tex_w, &tex_h);

    CompFxRect dst_rect = { .x = 0, .y = 0, .width = dst_w, .height = dst_h };
    CompFxRect src_rect = { .x = 0, .y = 0, .width = src_w, .height = src_h };
    CompFxBlurParams params;
    comp_fx_blur_pack(dst_rect, src_rect, tex_w, tex_h, offset, &params);

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, 0, 0, (unsigned int)dst_w, (unsigned int)dst_h);

    struct wlr_kfx_draw_options opts = {
        .shader     = shader,
        .dst_box    = { .x = 0, .y = 0, .width = dst_w, .height = dst_h },
        .clip       = &clip,
        .inputs     = { src_target, NULL, NULL, NULL },
        .params     = &params,
        .params_len = sizeof(params),
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    };
    wlr_kfx_render_pass_add_draw(pass, &opts);
    pixman_region32_fini(&clip);

    return wlr_render_pass_submit(pass);
}

static int
div_floor_i32(int a, int b)
{
    int q = a / b;
    int r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0)))
        q--;
    return q;
}

static int
div_ceil_i32(int a, int b)
{
    int q = a / b;
    int r = a % b;
    if ((r != 0) && ((r > 0) == (b > 0)))
        q++;
    return q;
}

static CompFxRect
clamp_buf_rect(CompFxRect r, int max_w, int max_h)
{
    int x1 = r.x < 0 ? 0 : r.x;
    int y1 = r.y < 0 ? 0 : r.y;
    int x2 = r.x + r.width;
    int y2 = r.y + r.height;
    if (x2 > max_w) x2 = max_w;
    if (y2 > max_h) y2 = max_h;
    if (x2 <= x1 || y2 <= y1)
        return (CompFxRect){0};
    return (CompFxRect){
        .x      = x1,
        .y      = y1,
        .width  = x2 - x1,
        .height = y2 - y1,
    };
}

static bool
recompute_glass_target(CompFxSystem *sys,
                       CompFxSurface *fx,
                       CompFxGlassOutput *go,
                       struct CompOutput *output,
                       const struct wlr_box *sample_logical_box,
                       bool is_xray)
{
    struct wlr_scene_output *so = output->scene_output;
    struct wlr_output *wo = output->wlr_output;
    struct wlr_renderer *r = sys->server->renderer;
    if (!so || !wo || !r)
        return false;

    int trans_w = 0, trans_h = 0;
    wlr_output_transformed_resolution(wo, &trans_w, &trans_h);
    if (trans_w <= 0 || trans_h <= 0)
        return false;

    float scale = wo->scale > 0.0f ? wo->scale : 1.0f;
    int eff_w = (int)ceil((double)trans_w / (double)scale);
    int eff_h = (int)ceil((double)trans_h / (double)scale);
    if (eff_w <= 0 || eff_h <= 0)
        return false;

    struct wlr_box out_logical = {
        .x      = so->x,
        .y      = so->y,
        .width  = eff_w,
        .height = eff_h,
    };
    struct wlr_box clamped_sample = {0};
    if (!wlr_box_intersection(&clamped_sample, sample_logical_box, &out_logical))
        return false;

    CompFxRect buf_box = comp_fx_transform_box(
        (CompFxRect){ clamped_sample.x, clamped_sample.y, clamped_sample.width, clamped_sample.height },
        (CompFxRect){ so->x, so->y, eff_w, eff_h },
        scale, (int)wo->transform, trans_w, trans_h);
    buf_box = clamp_buf_rect(buf_box, trans_w, trans_h);
    if (buf_box.width <= 0 || buf_box.height <= 0)
        return false;

    int req_w = div_ceil_i32(buf_box.x + buf_box.width, GLASS_DOWNSCALE) -
                div_floor_i32(buf_box.x, GLASS_DOWNSCALE) + 2;
    int req_h = div_ceil_i32(buf_box.y + buf_box.height, GLASS_DOWNSCALE) -
                div_floor_i32(buf_box.y, GLASS_DOWNSCALE) + 2;
    if (req_w < 1) req_w = 1;
    if (req_h < 1) req_h = 1;

    if (go->target) {
        int cur_w = 0, cur_h = 0;
        wlr_kfx_target_get_size(go->target, &cur_w, &cur_h);
        if (cur_w < req_w || cur_h < req_h ||
            (cur_w > req_w + 256 && cur_h > req_h + 256)) {
            wlr_kfx_target_release(go->target);
            go->target = NULL;
        }
    }
    if (!go->target) {
        go->target = wlr_kfx_target_acquire(r, req_w, req_h);
        if (!go->target)
            return false;
    }

    struct wlr_scene_node *boundary = NULL;
    if (is_xray && sys->server && sys->server->tree_tiled) {
        boundary = &sys->server->tree_tiled->node;
    } else if (fx->slots[FX_SLOT_SHADOW].anchor && fx->slots[FX_SLOT_SHADOW].enabled) {
        boundary = &fx->slots[FX_SLOT_SHADOW].anchor->node;
    } else if (fx->slots[FX_SLOT_GLASS].anchor) {
        boundary = &fx->slots[FX_SLOT_GLASS].anchor->node;
    }
    if (!boundary)
        return false;

    struct wlr_box t0_src = {0};
    if (!wlr_kfx_scene_output_render_below(so, boundary, sample_logical_box,
                                           go->target, GLASS_DOWNSCALE, &t0_src)) {
        return false;
    }
    go->t0_src_box = t0_src;

    int32_t passes = sys->cfg.glass.passes;
    if (passes < 1) passes = 1;
    if (passes > COMP_FX_GLASS_MAX_PASSES) passes = COMP_FX_GLASS_MAX_PASSES;
    int chain_steps = (passes > 1) ? (passes - 1) : 1;

    if (sys->cfg.glass.offset > 0.0 && t0_src.width > 0 && t0_src.height > 0) {
        struct wlr_kfx_target *levels[COMP_FX_GLASS_MAX_PASSES] = {0};
        int lw[COMP_FX_GLASS_MAX_PASSES] = {0};
        int lh[COMP_FX_GLASS_MAX_PASSES] = {0};

        levels[0] = go->target;
        lw[0]     = t0_src.width;
        lh[0]     = t0_src.height;

        bool ok = true;
        for (int i = 1; i <= chain_steps; i++) {
            lw[i] = (lw[i - 1] + 1) / 2;
            lh[i] = (lh[i - 1] + 1) / 2;
            if (lw[i] < 1) lw[i] = 1;
            if (lh[i] < 1) lh[i] = 1;
            levels[i] = wlr_kfx_target_acquire(r, lw[i], lh[i]);
            if (!levels[i]) {
                ok = false;
                break;
            }
        }

        if (ok) {
            for (int i = 0; i < chain_steps && ok; i++) {
                ok = run_blur_step(sys->blur_down_shader,
                                   levels[i + 1], lw[i + 1], lh[i + 1],
                                   levels[i], lw[i], lh[i],
                                   sys->cfg.glass.offset);
            }
            for (int i = chain_steps - 1; i >= 0 && ok; i--) {
                ok = run_blur_step(sys->blur_up_shader,
                                   levels[i], lw[i], lh[i],
                                   levels[i + 1], lw[i + 1], lh[i + 1],
                                   sys->cfg.glass.offset);
            }
        }

        for (int i = 1; i <= chain_steps; i++) {
            if (levels[i])
                wlr_kfx_target_release(levels[i]);
        }
        if (!ok)
            return false;
    }

    fx->glass_recomputes++;
    return true;
}

bool
fx_glass_prepare_output(CompFxSystem *sys, struct CompOutput *output,
                        CompFxOutputTrack *track, double now_s)
{
    if (!sys || !output || !output->scene_output || !output->wlr_output || !track)
        return false;

    struct wlr_scene_output *so = output->scene_output;
    struct wlr_output *wo = output->wlr_output;
    int trans_w = 0, trans_h = 0;
    wlr_output_transformed_resolution(wo, &trans_w, &trans_h);
    if (trans_w <= 0 || trans_h <= 0)
        return false;

    float scale = wo->scale > 0.0f ? wo->scale : 1.0f;
    int eff_w = (int)ceil((double)trans_w / (double)scale);
    int eff_h = (int)ceil((double)trans_h / (double)scale);
    if (eff_w <= 0 || eff_h <= 0)
        return false;

    struct wlr_box out_logical = {
        .x      = so->x,
        .y      = so->y,
        .width  = eff_w,
        .height = eff_h,
    };

    bool any_throttled = false;
    CompFxSurface *fx;
    wl_list_for_each_reverse(fx, &sys->surfaces, link) {
        if (!fx->state.mapped || !fx->slots[FX_SLOT_GLASS].enabled ||
            !fx->slots[FX_SLOT_GLASS].anchor || !fx->parent_tree) {
            if (!wl_list_empty(&fx->glass_outputs))
                fx_glass_surface_clear_targets(fx);
            continue;
        }

        int px = 0, py = 0;
        if (!wlr_scene_node_coords(&fx->parent_tree->node, &px, &py))
            continue;

        struct wlr_box glass_box = {
            .x      = px + fx->state.surface_box.x,
            .y      = py + fx->state.surface_box.y,
            .width  = fx->state.surface_box.width,
            .height = fx->state.surface_box.height,
        };
        struct wlr_box inter = {0};
        if (!wlr_box_intersection(&inter, &glass_box, &out_logical))
            continue;

        CompFxRect sample_r = comp_fx_glass_sample_box(
            (CompFxRect){ glass_box.x, glass_box.y, glass_box.width, glass_box.height },
            &sys->cfg.glass);
        struct wlr_box sample_box = {
            .x      = sample_r.x,
            .y      = sample_r.y,
            .width  = sample_r.width,
            .height = sample_r.height,
        };

        CompFxGlassOutput *go = ensure_glass_output(fx, so);
        if (!go)
            continue;

        bool geom_changed = !go->valid ||
                            go->last_logical_box.x != glass_box.x ||
                            go->last_logical_box.y != glass_box.y ||
                            go->last_logical_box.width != glass_box.width ||
                            go->last_logical_box.height != glass_box.height;
        bool is_xray = fx->state.has_rule_xray && fx->state.rule_xray;
        bool need_recompute = false;

        if (is_xray) {
            go->dirty_pending = false;
            if (geom_changed || go->built_bg_serial != track->bg_serial)
                need_recompute = true;
        } else {
            if (wlr_kfx_scene_output_damage_intersects(so, &sample_box))
                go->dirty_pending = true;
            bool damaged = go->dirty_pending;
            if (!geom_changed && damaged && sys->cfg.glass.max_recompute_hz > 0 &&
                go->last_recompute_s > 0.0) {
                double min_dt = 1.0 / (double)sys->cfg.glass.max_recompute_hz;
                if ((now_s - go->last_recompute_s) < min_dt) {
                    damaged       = false;
                    any_throttled = true;
                }
            }
            if (geom_changed || damaged)
                need_recompute = true;
        }

        if (need_recompute) {
            if (recompute_glass_target(sys, fx, go, output, &sample_box, is_xray)) {
                go->valid            = true;
                go->dirty_pending    = false;
                go->last_logical_box = glass_box;
                go->built_bg_serial  = track->bg_serial;
                go->last_recompute_s = now_s;
                wlr_kfx_scene_output_damage_box(so, &glass_box);
            }
        }
    }

    return any_throttled;
}

static void
glass_draw(CompFxSystem *sys, CompFxSurface *fx,
           const struct wlr_kfx_render_ctx *ctx)
{
    if (!sys || !sys->glass_shader || !fx || !ctx || !ctx->render_pass)
        return;

    CompFxGlassOutput *go = find_glass_output(fx, ctx->output);
    if (!go || !go->valid || !go->target ||
        go->t0_src_box.width <= 0 || go->t0_src_box.height <= 0)
        return;

    int tex_w = 0, tex_h = 0;
    wlr_kfx_target_get_size(go->target, &tex_w, &tex_h);

    float scale = 1.0f;
    if (ctx->output && ctx->output->output && ctx->output->output->scale > 0.0f)
        scale = ctx->output->output->scale;

    CompFxRect dst_rect = {
        .x      = ctx->dst_box.x,
        .y      = ctx->dst_box.y,
        .width  = ctx->dst_box.width,
        .height = ctx->dst_box.height,
    };
    CompFxRect full_glass_buf = {0};
    if (fx_compute_slot_pass_boxes(fx, FX_SLOT_GLASS, ctx,
                                   &full_glass_buf, NULL, NULL, NULL) &&
        full_glass_buf.width > 0 && full_glass_buf.height > 0) {
        dst_rect = full_glass_buf;
    }

    CompFxRect t0_src_rect = {
        .x      = go->t0_src_box.x,
        .y      = go->t0_src_box.y,
        .width  = go->t0_src_box.width,
        .height = go->t0_src_box.height,
    };

    CompFxGlassParams params;
    comp_fx_glass_pack(&sys->cfg.glass, dst_rect, t0_src_rect,
                       tex_w, tex_h, GLASS_DOWNSCALE, 1.0f, scale, &params);

    struct wlr_kfx_draw_options opts = {
        .shader     = sys->glass_shader,
        .dst_box    = ctx->dst_box,
        .clip       = ctx->clip,
        .inputs     = { go->target, NULL, NULL, NULL },
        .params     = &params,
        .params_len = sizeof(params),
        .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
    };
    wlr_kfx_render_pass_add_draw(ctx->render_pass, &opts);
}

const CompFxEffectDesc k_fx_glass_desc = {
    .kind            = FX_SLOT_GLASS,
    .name            = "glass",
    .stage           = WLR_KFX_INSTEAD,
    .init_shaders    = glass_init_shaders,
    .destroy_shaders = glass_destroy_shaders,
    .should_enable   = glass_should_enable,
    .compute_box     = glass_compute_box,
    .draw            = glass_draw,
};

#endif /* WLR_HAS_KFX */
