/* fx_dim.c — unfocused window dimming overlay effect (M2). */
#include "fx_internal.h"

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

#include "dim_frag.h"

#include <wlr/util/log.h>

static bool
dim_init_shaders(CompFxSystem *sys)
{
    if (!sys || !sys->server || !sys->server->renderer)
        return false;
    if (sys->dim_shader)
        return true;

    struct wlr_kfx_shader_layout layout = {
        .n_inputs    = 0,
        .params_size = sizeof(CompFxDimParams),
    };
    sys->dim_shader = wlr_kfx_shader_create(
        sys->server->renderer, dim_frag_spv, sizeof(dim_frag_spv), &layout);
    if (!sys->dim_shader) {
        wlr_log(WLR_ERROR, "kfx: failed to compile/create dim fragment shader");
        return false;
    }
    return true;
}

static void
dim_destroy_shaders(CompFxSystem *sys)
{
    if (!sys)
        return;
    if (sys->dim_shader) {
        wlr_kfx_shader_destroy(sys->dim_shader);
        sys->dim_shader = NULL;
    }
}

static bool
dim_should_enable(const CompFxSystem *sys, const CompFxSurface *fx)
{
    if (!sys || !fx || !sys->cfg.enabled || !sys->cfg.dim.enabled || !sys->dim_shader)
        return false;
    if (fx->kind != COMP_FX_SURFACE_WINDOW)
        return false;

    const CompFxSurfaceState *st = &fx->state;
    if (!st->mapped || st->is_fullscreen)
        return false;
    if (st->surface_box.width <= 0 || st->surface_box.height <= 0)
        return false;
    if (!(sys->cfg.dim.opacity > 0.0005))
        return false;

    return fx->slots[FX_SLOT_DIM].last_opacity > 0.0005f;
}

static struct wlr_box
dim_compute_box(const CompFxSystem *sys, const CompFxSurface *fx)
{
    (void)sys;
    return fx->state.surface_box;
}

static void
dim_draw(CompFxSystem *sys, CompFxSurface *fx,
         const struct wlr_kfx_render_ctx *ctx)
{
    if (!sys || !sys->dim_shader || !fx || !ctx || !ctx->render_pass)
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

    float strength = fx->slots[FX_SLOT_DIM].last_opacity;
    CompFxDimParams params;
    comp_fx_dim_pack(&sys->cfg.dim, anchor_dst, anchor_dst,
                     strength * (float)sys->cfg.dim.opacity, scale, &params);

    struct wlr_kfx_draw_options opts = {
        .shader     = sys->dim_shader,
        .dst_box    = ctx->dst_box,
        .clip       = ctx->clip,
        .params     = &params,
        .params_len = sizeof(params),
        .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
    };
    wlr_kfx_render_pass_add_draw(ctx->render_pass, &opts);
}

const CompFxEffectDesc k_fx_dim_desc = {
    .kind            = FX_SLOT_DIM,
    .name            = "dim",
    .stage           = WLR_KFX_INSTEAD,
    .init_shaders    = dim_init_shaders,
    .destroy_shaders = dim_destroy_shaders,
    .should_enable   = dim_should_enable,
    .compute_box     = dim_compute_box,
    .draw            = dim_draw,
};

#endif /* WLR_HAS_KFX */
