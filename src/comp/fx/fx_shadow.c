/* fx_shadow.c — analytic Gaussian drop-shadow effect (M2, spec §11). */
#include "fx_internal.h"

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

#include "shadow_frag.h"

#include <wlr/util/log.h>

static bool
shadow_init_shaders(CompFxSystem *sys)
{
    if (!sys || !sys->server || !sys->server->renderer)
        return false;
    if (sys->shadow_shader)
        return true;

    struct wlr_kfx_shader_layout layout = {
        .n_inputs    = 0,
        .params_size = sizeof(CompFxShadowParams),
    };
    sys->shadow_shader = wlr_kfx_shader_create(
        sys->server->renderer, shadow_frag_spv, sizeof(shadow_frag_spv), &layout);
    if (!sys->shadow_shader) {
        wlr_log(WLR_ERROR, "kfx: failed to compile/create shadow fragment shader");
        return false;
    }
    return true;
}

static void
shadow_destroy_shaders(CompFxSystem *sys)
{
    if (!sys)
        return;
    if (sys->shadow_shader) {
        wlr_kfx_shader_destroy(sys->shadow_shader);
        sys->shadow_shader = NULL;
    }
}

static bool
shadow_should_enable(const CompFxSystem *sys, const CompFxSurface *fx)
{
    if (!sys || !fx || !sys->cfg.enabled || !sys->cfg.shadow.enabled ||
        !sys->shadow_shader || sys->cfg.shadow.n_layers == 0)
        return false;

    const CompFxSurfaceState *st = &fx->state;
    if (!st->mapped || st->is_fullscreen)
        return false;
    if (st->outer_box.width <= 0 || st->outer_box.height <= 0)
        return false;

    if (st->has_rule_shadow)
        return st->rule_shadow;

    switch (fx->kind) {
    case COMP_FX_SURFACE_WINDOW:
        return st->is_floating;
    case COMP_FX_SURFACE_POPUP:
        return true;
    case COMP_FX_SURFACE_LAYER:
        return !st->is_background_or_bottom;
    }
    return false;
}

static struct wlr_box
shadow_compute_box(const CompFxSystem *sys, const CompFxSurface *fx)
{
    CompFxRect outer = {
        .x      = fx->state.outer_box.x,
        .y      = fx->state.outer_box.y,
        .width  = fx->state.outer_box.width,
        .height = fx->state.outer_box.height,
    };
    CompFxRect r = comp_fx_shadow_anchor_box(outer, &sys->cfg.shadow);
    return (struct wlr_box){
        .x      = r.x,
        .y      = r.y,
        .width  = r.width,
        .height = r.height,
    };
}

static void
shadow_draw(CompFxSystem *sys, CompFxSurface *fx,
            const struct wlr_kfx_render_ctx *ctx)
{
    if (!sys || !sys->shadow_shader || !fx || !ctx || !ctx->render_pass)
        return;

    CompFxRect outer_buf = {0};
    float scale = 1.0f;
    int transform = 0;
    if (!fx_compute_outer_buf_box(fx, ctx, &outer_buf, &scale, &transform))
        return;

    CompFxRect anchor_dst = {
        .x      = ctx->dst_box.x,
        .y      = ctx->dst_box.y,
        .width  = ctx->dst_box.width,
        .height = ctx->dst_box.height,
    };

    float strength = fx->slots[FX_SLOT_SHADOW].last_opacity;
    CompFxShadowParams params;
    comp_fx_shadow_pack(&sys->cfg.shadow, anchor_dst, outer_buf,
                        strength, scale, transform, &params);

    struct wlr_kfx_draw_options opts = {
        .shader     = sys->shadow_shader,
        .dst_box    = ctx->dst_box,
        .clip       = ctx->clip,
        .params     = &params,
        .params_len = sizeof(params),
        .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
    };
    wlr_kfx_render_pass_add_draw(ctx->render_pass, &opts);
}

const CompFxEffectDesc k_fx_shadow_desc = {
    .kind            = FX_SLOT_SHADOW,
    .name            = "shadow",
    .stage           = WLR_KFX_INSTEAD,
    .init_shaders    = shadow_init_shaders,
    .destroy_shaders = shadow_destroy_shaders,
    .should_enable   = shadow_should_enable,
    .compute_box     = shadow_compute_box,
    .draw            = shadow_draw,
};

#endif /* WLR_HAS_KFX */
