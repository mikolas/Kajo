/* comp_fx_core.c — pure-C effects defaults, colour math, coordinate transforms,
 * and std140 UBO packing. Zero wlroots, Vulkan, or GLib dependencies (CB6). */
#include "comp_fx_core.h"

#include <math.h>
#include <string.h>

static float
clamp01f(float v)
{
    if (!(v > 0.0f))
        return 0.0f;
    if (v >= 1.0f)
        return 1.0f;
    return v;
}

static int32_t
scale_length_i32(int32_t length, int32_t offset, float scale)
{
    double s = (double)scale;
    long long end = llround(((double)offset + (double)length) * s);
    long long beg = llround((double)offset * s);
    return (int32_t)(end - beg);
}

static int
invert_output_transform(int transform)
{
    /* Matches wl_output_transform (0..7): only 90 (1) <-> 270 (3) swap on invert. */
    switch (transform) {
    case 1: return 3;
    case 3: return 1;
    default: return (transform >= 0 && transform <= 7) ? transform : 0;
    }
}

void
comp_fx_config_defaults(CompFxConfig *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->dither  = false;

    cfg->shadow.enabled         = true;
    cfg->shadow.color_rgba[0]   = 0.0f;
    cfg->shadow.color_rgba[1]   = 0.0f;
    cfg->shadow.color_rgba[2]   = 0.0f;
    cfg->shadow.color_rgba[3]   = 1.0f;
    cfg->shadow.unfocused_scale = 0.6;
    cfg->shadow.duration_ms     = 150;
    cfg->shadow.n_layers        = 2;
    cfg->shadow.layers[0]       = (CompFxShadowLayer){
        .sigma    = 6.0,
        .offset_x = 0.0,
        .offset_y = 2.0,
        .opacity  = 0.30,
    };
    cfg->shadow.layers[1]       = (CompFxShadowLayer){
        .sigma    = 24.0,
        .offset_x = 0.0,
        .offset_y = 8.0,
        .opacity  = 0.18,
    };

    cfg->dim.enabled       = false;
    cfg->dim.color_rgba[0] = 0.0f;
    cfg->dim.color_rgba[1] = 0.0f;
    cfg->dim.color_rgba[2] = 0.0f;
    cfg->dim.color_rgba[3] = 1.0f;
    cfg->dim.opacity       = 0.15;
    cfg->dim.duration_ms   = 150;
}

float
comp_fx_srgb_to_linear(float v)
{
    float c = clamp01f(v);
    if (c <= 0.04045f)
        return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

void
comp_fx_rgba_srgb_to_linear(const float srgb[4], float out_lin[4])
{
    if (!srgb || !out_lin)
        return;
    out_lin[0] = comp_fx_srgb_to_linear(srgb[0]);
    out_lin[1] = comp_fx_srgb_to_linear(srgb[1]);
    out_lin[2] = comp_fx_srgb_to_linear(srgb[2]);
    out_lin[3] = clamp01f(srgb[3]);
}

CompFxMargins
comp_fx_shadow_reach(const CompFxShadowConfig *cfg)
{
    CompFxMargins m = {0, 0, 0, 0};
    if (!cfg)
        return m;
    size_t count = cfg->n_layers;
    if (count > COMP_FX_SHADOW_MAX_LAYERS)
        count = COMP_FX_SHADOW_MAX_LAYERS;

    for (size_t i = 0; i < count; i++) {
        const CompFxShadowLayer *l = &cfg->layers[i];
        if (!(l->sigma > 0.0) || !(l->opacity > 0.0))
            continue;
        double r = 3.0 * l->sigma;
        int32_t left   = (int32_t)ceil(fmax(0.0, r - l->offset_x));
        int32_t right  = (int32_t)ceil(fmax(0.0, r + l->offset_x));
        int32_t top    = (int32_t)ceil(fmax(0.0, r - l->offset_y));
        int32_t bottom = (int32_t)ceil(fmax(0.0, r + l->offset_y));
        if (left   > m.left)   m.left   = left;
        if (right  > m.right)  m.right  = right;
        if (top    > m.top)    m.top    = top;
        if (bottom > m.bottom) m.bottom = bottom;
    }
    return m;
}

CompFxRect
comp_fx_shadow_anchor_box(CompFxRect outer_box, const CompFxShadowConfig *cfg)
{
    if (outer_box.width <= 0 || outer_box.height <= 0 || !cfg)
        return (CompFxRect){0};
    CompFxMargins m = comp_fx_shadow_reach(cfg);
    return (CompFxRect){
        .x      = outer_box.x - m.left,
        .y      = outer_box.y - m.top,
        .width  = outer_box.width + m.left + m.right,
        .height = outer_box.height + m.top + m.bottom,
    };
}

double
comp_fx_shadow_coverage(double x, double y,
                        double x0, double y0, double x1, double y1,
                        double sigma)
{
    if (!(sigma > 0.0) || x1 <= x0 || y1 <= y0)
        return 0.0;
    const double inv_s2 = 1.0 / (sigma * 1.4142135623730950488);
    double cx = 0.5 * (erf((x - x0) * inv_s2) - erf((x - x1) * inv_s2));
    double cy = 0.5 * (erf((y - y0) * inv_s2) - erf((y - y1) * inv_s2));
    double cov = cx * cy;
    if (!(cov > 0.0))
        return 0.0;
    if (cov > 1.0)
        return 1.0;
    return cov;
}

CompFxRect
comp_fx_transform_box(CompFxRect box_logical, CompFxRect output_logical,
                      float scale, int transform,
                      int32_t trans_width, int32_t trans_height)
{
    double s = (scale > 0.0f) ? (double)scale : 1.0;
    int32_t lx = box_logical.x - output_logical.x;
    int32_t ly = box_logical.y - output_logical.y;

    CompFxRect scaled = {
        .x      = (int32_t)llround((double)lx * s),
        .y      = (int32_t)llround((double)ly * s),
        .width  = scale_length_i32(box_logical.width, lx, (float)s),
        .height = scale_length_i32(box_logical.height, ly, (float)s),
    };

    int inv = invert_output_transform(transform);
    CompFxRect dst = {0};
    if (inv % 2 == 0) {
        dst.width  = scaled.width;
        dst.height = scaled.height;
    } else {
        dst.width  = scaled.height;
        dst.height = scaled.width;
    }

    switch (inv) {
    case 0: /* NORMAL */
        dst.x = scaled.x;
        dst.y = scaled.y;
        break;
    case 1: /* 90 */
        dst.x = trans_height - scaled.y - scaled.height;
        dst.y = scaled.x;
        break;
    case 2: /* 180 */
        dst.x = trans_width - scaled.x - scaled.width;
        dst.y = trans_height - scaled.y - scaled.height;
        break;
    case 3: /* 270 */
        dst.x = scaled.y;
        dst.y = trans_width - scaled.x - scaled.width;
        break;
    case 4: /* FLIPPED */
        dst.x = trans_width - scaled.x - scaled.width;
        dst.y = scaled.y;
        break;
    case 5: /* FLIPPED_90 */
        dst.x = scaled.y;
        dst.y = scaled.x;
        break;
    case 6: /* FLIPPED_180 */
        dst.x = scaled.x;
        dst.y = trans_height - scaled.y - scaled.height;
        break;
    case 7: /* FLIPPED_270 */
        dst.x = trans_height - scaled.y - scaled.height;
        dst.y = trans_width - scaled.x - scaled.width;
        break;
    default:
        dst.x = scaled.x;
        dst.y = scaled.y;
        break;
    }
    return dst;
}

void
comp_fx_transform_offset(double off_x, double off_y, float scale, int transform,
                         float *out_x, float *out_y)
{
    double s = (scale > 0.0f) ? (double)scale : 1.0;
    double sx = off_x * s;
    double sy = off_y * s;
    int inv = invert_output_transform(transform);
    double rx = sx;
    double ry = sy;

    switch (inv) {
    case 0: rx =  sx; ry =  sy; break;
    case 1: rx = -sy; ry =  sx; break;
    case 2: rx = -sx; ry = -sy; break;
    case 3: rx =  sy; ry = -sx; break;
    case 4: rx = -sx; ry =  sy; break;
    case 5: rx =  sy; ry =  sx; break;
    case 6: rx =  sx; ry = -sy; break;
    case 7: rx = -sy; ry = -sx; break;
    default: break;
    }

    if (out_x) *out_x = (float)rx;
    if (out_y) *out_y = (float)ry;
}

void
comp_fx_shadow_pack(const CompFxShadowConfig *cfg,
                    CompFxRect anchor_dst_box,
                    CompFxRect outer_buf_box,
                    float opacity, float scale, int transform,
                    CompFxShadowParams *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    float s = (scale > 0.0f) ? scale : 1.0f;

    out->common.dst_origin[0] = (float)anchor_dst_box.x;
    out->common.dst_origin[1] = (float)anchor_dst_box.y;
    out->common.dst_size[0]   = (float)anchor_dst_box.width;
    out->common.dst_size[1]   = (float)anchor_dst_box.height;
    out->common.opacity       = clamp01f(opacity);
    out->common.scale         = s;
    out->common.hdr_headroom  = 1.0f;

    out->outer_rect[0] = (float)outer_buf_box.x;
    out->outer_rect[1] = (float)outer_buf_box.y;
    out->outer_rect[2] = (float)(outer_buf_box.x + outer_buf_box.width);
    out->outer_rect[3] = (float)(outer_buf_box.y + outer_buf_box.height);

    if (!cfg)
        return;

    comp_fx_rgba_srgb_to_linear(cfg->color_rgba, out->color_lin);

    size_t count = cfg->n_layers;
    if (count > COMP_FX_SHADOW_MAX_LAYERS)
        count = COMP_FX_SHADOW_MAX_LAYERS;

    uint32_t active = 0;
    for (size_t i = 0; i < count; i++) {
        const CompFxShadowLayer *l = &cfg->layers[i];
        if (!(l->sigma > 0.0) || !(l->opacity > 0.0))
            continue;
        float ox = 0.0f, oy = 0.0f;
        comp_fx_transform_offset(l->offset_x, l->offset_y, s, transform, &ox, &oy);
        out->layers[active][0] = ox;
        out->layers[active][1] = oy;
        out->layers[active][2] = (float)(l->sigma * (double)s);
        out->layers[active][3] = clamp01f((float)l->opacity);
        active++;
    }
    out->common.flags = active;
}

void
comp_fx_dim_pack(const CompFxDimConfig *cfg,
                 CompFxRect dst_box,
                 CompFxRect outer_buf_box,
                 float opacity, float scale,
                 CompFxDimParams *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    float s = (scale > 0.0f) ? scale : 1.0f;

    out->common.dst_origin[0] = (float)dst_box.x;
    out->common.dst_origin[1] = (float)dst_box.y;
    out->common.dst_size[0]   = (float)dst_box.width;
    out->common.dst_size[1]   = (float)dst_box.height;
    out->common.opacity       = clamp01f(opacity);
    out->common.scale         = s;
    out->common.hdr_headroom  = 1.0f;
    out->common.flags         = 0;

    out->outer_rect[0] = (float)outer_buf_box.x;
    out->outer_rect[1] = (float)outer_buf_box.y;
    out->outer_rect[2] = (float)(outer_buf_box.x + outer_buf_box.width);
    out->outer_rect[3] = (float)(outer_buf_box.y + outer_buf_box.height);

    if (cfg)
        comp_fx_rgba_srgb_to_linear(cfg->color_rgba, out->color_lin);
}

bool
comp_fx_focus_spring_params(int32_t duration_ms, double slowdown, CompSpringParams *out)
{
    if (!out)
        return false;
    if (duration_ms <= 0 || !(slowdown > 0.0))
        return false;
    double dur_s = ((double)duration_ms * slowdown) / 1000.0;
    if (!(dur_s > 0.001))
        return false;
    double omega0 = 6.6 / dur_s;
    out->damping_ratio = 1.0;
    out->stiffness     = omega0 * omega0;
    out->epsilon       = 0.005;
    return comp_spring_params_valid(out);
}
