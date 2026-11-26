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
    cfg->dither  = true;

    cfg->shadow.enabled         = true;
    cfg->shadow.color_rgba[0]   = 0.0f;
    cfg->shadow.color_rgba[1]   = 0.0f;
    cfg->shadow.color_rgba[2]   = 0.0f;
    cfg->shadow.color_rgba[3]   = 1.0f;
    cfg->shadow.unfocused_scale = 0.6;
    cfg->shadow.strength        = 1.0;
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

    cfg->glass.enabled          = true;
    cfg->glass.style            = COMP_FX_GLASS_FROSTED;
    cfg->glass.passes           = 3;
    cfg->glass.offset           = 2.0;
    cfg->glass.saturation       = 1.15;
    cfg->glass.brightness       = 0.90;
    cfg->glass.tint_rgba[0]     = 20.0f / 255.0f; /* #141414 */
    cfg->glass.tint_rgba[1]     = 20.0f / 255.0f;
    cfg->glass.tint_rgba[2]     = 20.0f / 255.0f;
    cfg->glass.tint_rgba[3]     = 1.0f;
    cfg->glass.tint_alpha       = 0.35;
    cfg->glass.max_recompute_hz = 0;
    cfg->glass.refraction       = 0.45;
    cfg->glass.dispersion       = 0.25;
    cfg->glass.bevel_px         = 14.0;
    cfg->glass.specular         = 0.55;
    cfg->glass.grain            = 0.35;
    cfg->glass.flute_px         = 14.0;
    cfg->glass.flute_axis       = COMP_FX_FLUTE_VERTICAL;

    cfg->color_filter            = COMP_FX_FILTER_NONE;
    cfg->color_filter_strength   = 1.0;
    cfg->night_light_temperature = 4000;
    cfg->sdr_white_nits          = 0.0;
    cfg->tonemap                 = true;
    cfg->tonemap_peak_nits       = 0.0;

    cfg->transitions.enabled            = true;
    cfg->transitions.open_preset        = COMP_FX_OPEN_DEFAULT;
    cfg->transitions.open_duration_ms   = 150;
    cfg->transitions.open_curve         = COMP_FX_CURVE_EASE_OUT_EXPO;
    cfg->transitions.close_preset       = COMP_FX_CLOSE_DEFAULT;
    cfg->transitions.close_duration_ms  = 200;
    cfg->transitions.close_curve        = COMP_FX_CURVE_EASE_OUT_QUAD;
    cfg->transitions.resize_preset      = COMP_FX_RESIZE_DEFAULT;
    cfg->transitions.resize_duration_ms = 180;
}

static bool
ascii_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

bool
comp_fx_color_filter_from_str(const char *s, CompFxColorFilter *out)
{
    if (!s || !out)
        return false;
    if (ascii_ieq(s, "none") || ascii_ieq(s, "off")) {
        *out = COMP_FX_FILTER_NONE;
        return true;
    }
    if (ascii_ieq(s, "grayscale") || ascii_ieq(s, "greyscale")) {
        *out = COMP_FX_FILTER_GRAYSCALE;
        return true;
    }
    if (ascii_ieq(s, "night-light") || ascii_ieq(s, "night_light")) {
        *out = COMP_FX_FILTER_NIGHT_LIGHT;
        return true;
    }
    if (ascii_ieq(s, "protanopia")) {
        *out = COMP_FX_FILTER_PROTANOPIA;
        return true;
    }
    if (ascii_ieq(s, "deuteranopia")) {
        *out = COMP_FX_FILTER_DEUTERANOPIA;
        return true;
    }
    if (ascii_ieq(s, "tritanopia")) {
        *out = COMP_FX_FILTER_TRITANOPIA;
        return true;
    }
    return false;
}

const char *
comp_fx_color_filter_to_str(CompFxColorFilter filter)
{
    switch (filter) {
    case COMP_FX_FILTER_NONE:         return "none";
    case COMP_FX_FILTER_GRAYSCALE:    return "grayscale";
    case COMP_FX_FILTER_NIGHT_LIGHT:  return "night-light";
    case COMP_FX_FILTER_PROTANOPIA:   return "protanopia";
    case COMP_FX_FILTER_DEUTERANOPIA: return "deuteranopia";
    case COMP_FX_FILTER_TRITANOPIA:   return "tritanopia";
    }
    return "none";
}

float
comp_fx_srgb_to_linear(float v)
{
    float c = clamp01f(v);
    if (c <= 0.04045f)
        return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

float
comp_fx_linear_to_srgb(float v)
{
    float c = clamp01f(v);
    if (c <= 0.0031308f)
        return c * 12.92f;
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

float
comp_fx_linear_to_gamma22(float v)
{
    float c = clamp01f(v);
    return powf(c, 1.0f / 2.2f);
}

float
comp_fx_linear_to_pq(float v)
{
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float m  = 78.84375f;
    const float n  = 0.1593017578125f;
    float c = clamp01f(v);
    float pow_n = powf(c, n);
    return powf((c1 + c2 * pow_n) / (1.0f + c3 * pow_n), m);
}

float
comp_fx_bt2390_pq(float e1, float max_pq)
{
    float e = clamp01f(e1);
    float mp = clamp01f(max_pq);
    float ks = fmaxf(1.5f * mp - 0.5f, 0.0f);
    if (e <= ks || mp >= 1.0f || ks >= 1.0f)
        return fminf(e, mp);
    float t  = clamp01f((e - ks) / (1.0f - ks));
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    return h00 * ks + h10 * (1.0f - ks) + h01 * mp;
}

static void
night_light_kelvin_to_linear_gains(int32_t temp_k, float out_rgb[3])
{
    int32_t k = temp_k;
    if (k < 1500)
        k = 1500;
    if (k > 6500)
        k = 6500;
    if (k >= 6500) {
        out_rgb[0] = 1.0f;
        out_rgb[1] = 1.0f;
        out_rgb[2] = 1.0f;
        return;
    }

    double t = (double)k / 100.0;
    double r_srgb = 1.0;
    double g_srgb = (99.4708025861 * log(t) - 161.1195681661) / 255.0;
    double b_srgb = (t <= 19.0)
        ? 0.0
        : ((138.5177312231 * log(t - 10.0) - 305.0447927307) / 255.0);

    out_rgb[0] = comp_fx_srgb_to_linear((float)r_srgb);
    out_rgb[1] = comp_fx_srgb_to_linear((float)g_srgb);
    out_rgb[2] = comp_fx_srgb_to_linear((float)b_srgb);
}

bool
comp_fx_color_filter_matrix(const CompFxConfig *cfg, float out_matrix[9])
{
    static const float k_identity[9] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
    /* Rec.709 / sRGB linear luminance weights (row sum = 1.0). */
    static const float k_grayscale[9] = {
        0.2126f, 0.7152f, 0.0722f,
        0.2126f, 0.7152f, 0.0722f,
        0.2126f, 0.7152f, 0.0722f,
    };
    /* Linear sRGB CVD compensation / Daltonization matrices (neutral-white invariant: each row sums to 1.0). */
    static const float k_protanopia[9] = {
        0.56667f, 0.43333f, 0.00000f,
        0.55833f, 0.44167f, 0.00000f,
        0.00000f, 0.24167f, 0.75833f,
    };
    static const float k_deuteranopia[9] = {
        0.62500f, 0.37500f, 0.00000f,
        0.70000f, 0.30000f, 0.00000f,
        0.00000f, 0.30000f, 0.70000f,
    };
    static const float k_tritanopia[9] = {
        0.95000f, 0.05000f, 0.00000f,
        0.00000f, 0.43333f, 0.56667f,
        0.00000f, 0.47500f, 0.52500f,
    };

    if (!out_matrix)
        return false;
    memcpy(out_matrix, k_identity, sizeof(k_identity));
    if (!cfg || !cfg->enabled || cfg->color_filter == COMP_FX_FILTER_NONE)
        return false;

    float strength = clamp01f((float)cfg->color_filter_strength);
    if (strength <= 1e-5f)
        return false;

    float target[9];
    switch (cfg->color_filter) {
    case COMP_FX_FILTER_GRAYSCALE:
        memcpy(target, k_grayscale, sizeof(target));
        break;
    case COMP_FX_FILTER_NIGHT_LIGHT: {
        float gains[3] = { 1.0f, 1.0f, 1.0f };
        night_light_kelvin_to_linear_gains(cfg->night_light_temperature, gains);
        if (fabsf(gains[0] - 1.0f) <= 1e-5f &&
            fabsf(gains[1] - 1.0f) <= 1e-5f &&
            fabsf(gains[2] - 1.0f) <= 1e-5f) {
            return false;
        }
        memset(target, 0, sizeof(target));
        target[0] = gains[0];
        target[4] = gains[1];
        target[8] = gains[2];
        break;
    }
    case COMP_FX_FILTER_PROTANOPIA:
        memcpy(target, k_protanopia, sizeof(target));
        break;
    case COMP_FX_FILTER_DEUTERANOPIA:
        memcpy(target, k_deuteranopia, sizeof(target));
        break;
    case COMP_FX_FILTER_TRITANOPIA:
        memcpy(target, k_tritanopia, sizeof(target));
        break;
    case COMP_FX_FILTER_NONE:
    default:
        return false;
    }

    for (int i = 0; i < 9; i++) {
        out_matrix[i] = (1.0f - strength) * k_identity[i] + strength * target[i];
    }
    return true;
}

void
comp_fx_color_filter_apply_linear(const float matrix[9],
                                  const float in_rgb[3],
                                  float out_rgb[3])
{
    if (!matrix || !in_rgb || !out_rgb)
        return;
    float r = in_rgb[0];
    float g = in_rgb[1];
    float b = in_rgb[2];
    out_rgb[0] = fmaxf(0.0f, matrix[0] * r + matrix[1] * g + matrix[2] * b);
    out_rgb[1] = fmaxf(0.0f, matrix[3] * r + matrix[4] * g + matrix[5] * b);
    out_rgb[2] = fmaxf(0.0f, matrix[6] * r + matrix[7] * g + matrix[8] * b);
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

bool
comp_fx_glass_style_from_str(const char *s, CompFxGlassStyle *out)
{
    if (!s || !out)
        return false;
    if (ascii_ieq(s, "frosted") || ascii_ieq(s, "default")) {
        *out = COMP_FX_GLASS_FROSTED;
        return true;
    }
    if (ascii_ieq(s, "liquid")) {
        *out = COMP_FX_GLASS_LIQUID;
        return true;
    }
    if (ascii_ieq(s, "halation")) {
        *out = COMP_FX_GLASS_HALATION;
        return true;
    }
    if (ascii_ieq(s, "fluted")) {
        *out = COMP_FX_GLASS_FLUTED;
        return true;
    }
    if (ascii_ieq(s, "acrylic")) {
        *out = COMP_FX_GLASS_ACRYLIC;
        return true;
    }
    if (ascii_ieq(s, "aero")) {
        *out = COMP_FX_GLASS_AERO;
        return true;
    }
    if (ascii_ieq(s, "dichroic")) {
        *out = COMP_FX_GLASS_DICHROIC;
        return true;
    }
    return false;
}

const char *
comp_fx_glass_style_to_str(CompFxGlassStyle style)
{
    switch (style) {
    case COMP_FX_GLASS_FROSTED:  return "frosted";
    case COMP_FX_GLASS_LIQUID:   return "liquid";
    case COMP_FX_GLASS_HALATION: return "halation";
    case COMP_FX_GLASS_FLUTED:   return "fluted";
    case COMP_FX_GLASS_ACRYLIC:  return "acrylic";
    case COMP_FX_GLASS_AERO:     return "aero";
    case COMP_FX_GLASS_DICHROIC: return "dichroic";
    }
    return "frosted";
}

bool
comp_fx_glass_style_is_dual_input(CompFxGlassStyle style)
{
    return style == COMP_FX_GLASS_LIQUID || style == COMP_FX_GLASS_HALATION;
}

bool
comp_fx_flute_axis_from_str(const char *s, CompFxFluteAxis *out)
{
    if (!s || !out)
        return false;
    if (ascii_ieq(s, "vertical")) {
        *out = COMP_FX_FLUTE_VERTICAL;
        return true;
    }
    if (ascii_ieq(s, "horizontal")) {
        *out = COMP_FX_FLUTE_HORIZONTAL;
        return true;
    }
    return false;
}

const char *
comp_fx_flute_axis_to_str(CompFxFluteAxis axis)
{
    switch (axis) {
    case COMP_FX_FLUTE_VERTICAL:   return "vertical";
    case COMP_FX_FLUTE_HORIZONTAL: return "horizontal";
    }
    return "vertical";
}

int32_t
comp_fx_glass_sample_reach_for_style(const CompFxGlassConfig *cfg, CompFxGlassStyle style)
{
    if (!cfg || !cfg->enabled)
        return 0;

    int32_t passes = cfg->passes;
    if (passes < 0)
        passes = 0;
    if (passes > COMP_FX_GLASS_MAX_PASSES)
        passes = COMP_FX_GLASS_MAX_PASSES;

    double r_blur = 0.0;
    if (passes > 0 && cfg->offset > 0.0) {
        int32_t eff_passes = passes;
        if (style == COMP_FX_GLASS_HALATION && eff_passes < 4)
            eff_passes = 4;
        r_blur = (double)(1 << eff_passes) * cfg->offset;
    }

    double r_opt = 0.0;
    double ref   = cfg->refraction > 0.0 ? cfg->refraction : 0.0;
    double disp  = cfg->dispersion > 0.0 ? cfg->dispersion : 0.0;
    double bev   = cfg->bevel_px > 0.0 ? cfg->bevel_px : 0.0;
    double flute = cfg->flute_px > 0.0 ? cfg->flute_px : 0.0;
    double grain = cfg->grain > 0.0 ? cfg->grain : 0.0;

    switch (style) {
    case COMP_FX_GLASS_LIQUID:
        r_opt = ref * bev * (1.0 + 0.18 * disp);
        break;
    case COMP_FX_GLASS_FLUTED:
        r_opt = ref * flute * 0.65 * (1.0 + 0.18 * disp);
        break;
    case COMP_FX_GLASS_ACRYLIC:
        r_opt = 4.0 * grain;
        break;
    case COMP_FX_GLASS_FROSTED:
    case COMP_FX_GLASS_HALATION:
    case COMP_FX_GLASS_AERO:
    case COMP_FX_GLASS_DICHROIC:
        r_opt = 0.0;
        break;
    }

    double total = r_blur + r_opt;
    if (!(total > 0.0))
        return 0;
    int32_t ri = (int32_t)ceil(total);
    if (ri > 256)
        ri = 256;
    return ri;
}

int32_t
comp_fx_glass_sample_reach(const CompFxGlassConfig *cfg)
{
    if (!cfg)
        return 0;
    return comp_fx_glass_sample_reach_for_style(cfg, cfg->style);
}

CompFxRect
comp_fx_glass_sample_box_for_style(CompFxRect surface_box,
                                   const CompFxGlassConfig *cfg,
                                   CompFxGlassStyle style)
{
    if (surface_box.width <= 0 || surface_box.height <= 0 || !cfg)
        return (CompFxRect){0};
    int32_t r = comp_fx_glass_sample_reach_for_style(cfg, style);
    return (CompFxRect){
        .x      = surface_box.x - r,
        .y      = surface_box.y - r,
        .width  = surface_box.width + 2 * r,
        .height = surface_box.height + 2 * r,
    };
}

CompFxRect
comp_fx_glass_sample_box(CompFxRect surface_box, const CompFxGlassConfig *cfg)
{
    if (!cfg)
        return (CompFxRect){0};
    return comp_fx_glass_sample_box_for_style(surface_box, cfg, cfg->style);
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

void
comp_fx_effective_resolution(int32_t trans_width, int32_t trans_height,
                             float scale,
                             int32_t *out_width, int32_t *out_height)
{
    double s = (scale > 0.0f) ? (double)scale : 1.0;
    int32_t w = (trans_width > 0) ? (int32_t)ceil((double)trans_width / s) : 0;
    int32_t h = (trans_height > 0) ? (int32_t)ceil((double)trans_height / s) : 0;
    if (out_width)
        *out_width = w;
    if (out_height)
        *out_height = h;
}

CompFxRect
comp_fx_clamp_rect(CompFxRect r, int32_t max_w, int32_t max_h)
{
    if (r.width <= 0 || r.height <= 0 || max_w <= 0 || max_h <= 0)
        return (CompFxRect){0};
    int32_t x0 = r.x > 0 ? r.x : 0;
    int32_t y0 = r.y > 0 ? r.y : 0;
    int32_t x1 = (r.x + r.width < max_w) ? (r.x + r.width) : max_w;
    int32_t y1 = (r.y + r.height < max_h) ? (r.y + r.height) : max_h;
    if (x1 <= x0 || y1 <= y0)
        return (CompFxRect){0};
    return (CompFxRect){
        .x      = x0,
        .y      = y0,
        .width  = x1 - x0,
        .height = y1 - y0,
    };
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

    double strength = (cfg->strength >= 0.0 && cfg->strength <= 2.0) ? cfg->strength : 1.0;
    uint32_t active = 0;
    for (size_t i = 0; i < count; i++) {
        const CompFxShadowLayer *l = &cfg->layers[i];
        if (!(l->sigma > 0.0) || !(l->opacity > 0.0) || !(strength > 0.0))
            continue;
        float ox = 0.0f, oy = 0.0f;
        comp_fx_transform_offset(l->offset_x, l->offset_y, s, transform, &ox, &oy);
        out->layers[active][0] = ox;
        out->layers[active][1] = oy;
        out->layers[active][2] = (float)(l->sigma * (double)s);
        out->layers[active][3] = clamp01f((float)(l->opacity * strength));
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

void
comp_fx_blur_pack(CompFxRect dst_box,
                  CompFxRect src_rect,
                  int32_t tex_w, int32_t tex_h,
                  double offset,
                  CompFxBlurParams *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    int32_t dw = dst_box.width  > 0 ? dst_box.width  : 1;
    int32_t dh = dst_box.height > 0 ? dst_box.height : 1;
    int32_t sw = src_rect.width  > 0 ? src_rect.width  : 1;
    int32_t sh = src_rect.height > 0 ? src_rect.height : 1;
    int32_t tw = tex_w > 0 ? tex_w : sw;
    int32_t th = tex_h > 0 ? tex_h : sh;

    out->common.dst_origin[0] = (float)dst_box.x;
    out->common.dst_origin[1] = (float)dst_box.y;
    out->common.dst_size[0]   = (float)dw;
    out->common.dst_size[1]   = (float)dh;
    out->common.opacity       = 1.0f;
    out->common.scale         = 1.0f;
    out->common.hdr_headroom  = 1.0f;
    out->common.flags         = 0;

    out->src_rect[0] = (float)src_rect.x;
    out->src_rect[1] = (float)src_rect.y;
    out->src_rect[2] = (float)sw;
    out->src_rect[3] = (float)sh;

    float min_x = (float)src_rect.x + 0.5f;
    float min_y = (float)src_rect.y + 0.5f;
    float max_x = fmaxf(min_x, (float)(src_rect.x + sw) - 0.5f);
    float max_y = fmaxf(min_y, (float)(src_rect.y + sh) - 0.5f);
    out->valid_bounds[0] = min_x;
    out->valid_bounds[1] = min_y;
    out->valid_bounds[2] = max_x;
    out->valid_bounds[3] = max_y;

    out->inv_tex_size[0] = 1.0f / (float)tw;
    out->inv_tex_size[1] = 1.0f / (float)th;
    out->offset          = (float)(offset > 0.0 ? offset : 0.0);
}

_Static_assert(sizeof(CompFxGlassParams) == 192, "CompFxGlassParams must be 192 bytes (std140)");

void
comp_fx_glass_pack_ex(const CompFxGlassConfig *cfg,
                      CompFxGlassStyle style,
                      CompFxRect full_buf_box,
                      CompFxRect pass_dst_box,
                      CompFxRect t0_src_box,
                      int32_t tex0_w, int32_t tex0_h,
                      int downscale0,
                      CompFxRect t1_src_box,
                      int32_t tex1_w, int32_t tex1_h,
                      int downscale1,
                      float opacity, float scale,
                      int transform,
                      CompFxGlassParams *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    float s   = (scale > 0.0f) ? scale : 1.0f;
    float ds0 = (downscale0 > 0) ? (float)downscale0 : 2.0f;
    float ds1 = (downscale1 > 0) ? (float)downscale1 : 2.0f;
    int32_t dw = pass_dst_box.width  > 0 ? pass_dst_box.width  : 1;
    int32_t dh = pass_dst_box.height > 0 ? pass_dst_box.height : 1;
    int32_t fw = full_buf_box.width  > 0 ? full_buf_box.width  : dw;
    int32_t fh = full_buf_box.height > 0 ? full_buf_box.height : dh;

    int32_t sw0 = t0_src_box.width  > 0 ? t0_src_box.width  : 1;
    int32_t sh0 = t0_src_box.height > 0 ? t0_src_box.height : 1;
    int32_t tw0 = tex0_w > 0 ? tex0_w : sw0;
    int32_t th0 = tex0_h > 0 ? tex0_h : sh0;

    out->common.dst_origin[0] = (float)pass_dst_box.x;
    out->common.dst_origin[1] = (float)pass_dst_box.y;
    out->common.dst_size[0]   = (float)dw;
    out->common.dst_size[1]   = (float)dh;
    out->common.opacity       = clamp01f(opacity);
    out->common.scale         = s;
    out->common.hdr_headroom  = 1.0f;
    out->common.flags         = 0;

    out->crop_rect[0] = (float)full_buf_box.x / ds0 - (float)t0_src_box.x;
    out->crop_rect[1] = (float)full_buf_box.y / ds0 - (float)t0_src_box.y;
    out->crop_rect[2] = (float)fw / ds0;
    out->crop_rect[3] = (float)fh / ds0;

    out->valid_bounds[0] = 0.5f;
    out->valid_bounds[1] = 0.5f;
    out->valid_bounds[2] = fmaxf(0.5f, (float)sw0 - 0.5f);
    out->valid_bounds[3] = fmaxf(0.5f, (float)sh0 - 0.5f);

    out->inv_tex_size[0] = 1.0f / (float)tw0;
    out->inv_tex_size[1] = 1.0f / (float)th0;

    if (tex1_w > 0 && tex1_h > 0 && t1_src_box.width > 0 && t1_src_box.height > 0) {
        out->aux_crop_rect[0]    = (float)full_buf_box.x / ds1 - (float)t1_src_box.x;
        out->aux_crop_rect[1]    = (float)full_buf_box.y / ds1 - (float)t1_src_box.y;
        out->aux_crop_rect[2]    = (float)fw / ds1;
        out->aux_crop_rect[3]    = (float)fh / ds1;
        out->aux_valid_bounds[0] = 0.5f;
        out->aux_valid_bounds[1] = 0.5f;
        out->aux_valid_bounds[2] = fmaxf(0.5f, (float)t1_src_box.width  - 0.5f);
        out->aux_valid_bounds[3] = fmaxf(0.5f, (float)t1_src_box.height - 0.5f);
        out->aux_inv_tex_size[0] = 1.0f / (float)tex1_w;
        out->aux_inv_tex_size[1] = 1.0f / (float)tex1_h;
    } else {
        memcpy(out->aux_crop_rect, out->crop_rect, sizeof(out->aux_crop_rect));
        memcpy(out->aux_valid_bounds, out->valid_bounds, sizeof(out->aux_valid_bounds));
        memcpy(out->aux_inv_tex_size, out->inv_tex_size, sizeof(out->aux_inv_tex_size));
    }

    out->surface_size[0]   = (float)fw / s;
    out->surface_size[1]   = (float)fh / s;
    out->surface_offset[0] = (float)(pass_dst_box.x - full_buf_box.x) / s;
    out->surface_offset[1] = (float)(pass_dst_box.y - full_buf_box.y) / s;

    /* Pre-rotate 2D directional vector into output-buffer orientation. */
    double dir_x = -0.4008188; /* normalize(-0.35, -0.80) */
    double dir_y = -0.9161573;
    if (style == COMP_FX_GLASS_FLUTED) {
        CompFxFluteAxis axis = cfg ? cfg->flute_axis : COMP_FX_FLUTE_VERTICAL;
        dir_x = (axis == COMP_FX_FLUTE_HORIZONTAL) ? 0.0 : 1.0;
        dir_y = (axis == COMP_FX_FLUTE_HORIZONTAL) ? 1.0 : 0.0;
    }
    comp_fx_transform_offset(dir_x, dir_y, 1.0f, transform,
                             &out->light_dir[0], &out->light_dir[1]);

    out->style = (uint32_t)style;

    if (cfg) {
        comp_fx_rgba_srgb_to_linear(cfg->tint_rgba, out->tint_lin);
        out->tint_lin[3] = clamp01f((float)cfg->tint_alpha);
        out->saturation  = (float)(cfg->saturation >= 0.0 ? cfg->saturation : 1.0);
        out->brightness  = (float)(cfg->brightness >= 0.0 ? cfg->brightness : 1.0);
        out->refraction  = (float)(cfg->refraction >= 0.0 ? cfg->refraction : 0.0);
        out->dispersion  = clamp01f((float)cfg->dispersion);
        out->bevel_px    = (float)(cfg->bevel_px >= 2.0 ? cfg->bevel_px : 2.0);
        out->specular    = (float)(cfg->specular >= 0.0 ? cfg->specular : 0.0);
        out->grain       = clamp01f((float)cfg->grain);
        out->flute_px    = (float)(cfg->flute_px >= 4.0 ? cfg->flute_px : 4.0);
    } else {
        out->saturation = 1.0f;
        out->brightness = 1.0f;
        out->refraction = 0.45f;
        out->dispersion = 0.25f;
        out->bevel_px   = 14.0f;
        out->specular   = 0.55f;
        out->grain      = 0.35f;
        out->flute_px   = 14.0f;
    }
}

void
comp_fx_glass_pack(const CompFxGlassConfig *cfg,
                   CompFxRect dst_box,
                   CompFxRect t0_src_box,
                   int32_t tex_w, int32_t tex_h,
                   int downscale,
                   float opacity, float scale,
                   CompFxGlassParams *out)
{
    CompFxGlassStyle st = cfg ? cfg->style : COMP_FX_GLASS_FROSTED;
    comp_fx_glass_pack_ex(cfg, st, dst_box, dst_box,
                          t0_src_box, tex_w, tex_h, downscale,
                          (CompFxRect){0}, 0, 0, downscale,
                          opacity, scale, 0, out);
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

bool
comp_fx_curve_from_str(const char *s, CompFxTransitionCurve *out)
{
    if (!s || !out)
        return false;
    if (ascii_ieq(s, "ease-out-expo") || ascii_ieq(s, "ease_out_expo")) {
        *out = COMP_FX_CURVE_EASE_OUT_EXPO;
        return true;
    }
    if (ascii_ieq(s, "ease-out-cubic") || ascii_ieq(s, "ease_out_cubic")) {
        *out = COMP_FX_CURVE_EASE_OUT_CUBIC;
        return true;
    }
    if (ascii_ieq(s, "ease-out-quad") || ascii_ieq(s, "ease_out_quad")) {
        *out = COMP_FX_CURVE_EASE_OUT_QUAD;
        return true;
    }
    if (ascii_ieq(s, "linear")) {
        *out = COMP_FX_CURVE_LINEAR;
        return true;
    }
    return false;
}

const char *
comp_fx_curve_to_str(CompFxTransitionCurve curve)
{
    switch (curve) {
    case COMP_FX_CURVE_EASE_OUT_EXPO:  return "ease-out-expo";
    case COMP_FX_CURVE_EASE_OUT_CUBIC: return "ease-out-cubic";
    case COMP_FX_CURVE_EASE_OUT_QUAD:  return "ease-out-quad";
    case COMP_FX_CURVE_LINEAR:         return "linear";
    }
    return "ease-out-expo";
}

bool
comp_fx_open_preset_from_str(const char *s, CompFxOpenPreset *out)
{
    if (!s || !out)
        return false;
    if (ascii_ieq(s, "default"))          { *out = COMP_FX_OPEN_DEFAULT;          return true; }
    if (ascii_ieq(s, "metro-flip"))       { *out = COMP_FX_OPEN_METRO_FLIP;       return true; }
    if (ascii_ieq(s, "expanding-circle")) { *out = COMP_FX_OPEN_EXPANDING_CIRCLE; return true; }
    if (ascii_ieq(s, "crt-on"))           { *out = COMP_FX_OPEN_CRT_ON;           return true; }
    if (ascii_ieq(s, "pixelate-in"))      { *out = COMP_FX_OPEN_PIXELATE_IN;      return true; }
    if (ascii_ieq(s, "glitch-in"))        { *out = COMP_FX_OPEN_GLITCH_IN;        return true; }
    if (ascii_ieq(s, "curtain-open"))     { *out = COMP_FX_OPEN_CURTAIN_OPEN;     return true; }
    return false;
}

const char *
comp_fx_open_preset_to_str(CompFxOpenPreset preset)
{
    switch (preset) {
    case COMP_FX_OPEN_DEFAULT:          return "default";
    case COMP_FX_OPEN_METRO_FLIP:       return "metro-flip";
    case COMP_FX_OPEN_EXPANDING_CIRCLE: return "expanding-circle";
    case COMP_FX_OPEN_CRT_ON:           return "crt-on";
    case COMP_FX_OPEN_PIXELATE_IN:      return "pixelate-in";
    case COMP_FX_OPEN_GLITCH_IN:        return "glitch-in";
    case COMP_FX_OPEN_CURTAIN_OPEN:     return "curtain-open";
    }
    return "default";
}

bool
comp_fx_close_preset_from_str(const char *s, CompFxClosePreset *out)
{
    if (!s || !out)
        return false;
    if (ascii_ieq(s, "default"))         { *out = COMP_FX_CLOSE_DEFAULT;         return true; }
    if (ascii_ieq(s, "fall-and-rotate")) { *out = COMP_FX_CLOSE_FALL_AND_ROTATE; return true; }
    if (ascii_ieq(s, "crt-off"))         { *out = COMP_FX_CLOSE_CRT_OFF;         return true; }
    if (ascii_ieq(s, "ash-dissolve"))    { *out = COMP_FX_CLOSE_ASH_DISSOLVE;    return true; }
    if (ascii_ieq(s, "vortex-implode"))  { *out = COMP_FX_CLOSE_VORTEX_IMPLODE;  return true; }
    if (ascii_ieq(s, "pixelate-out"))    { *out = COMP_FX_CLOSE_PIXELATE_OUT;    return true; }
    if (ascii_ieq(s, "slice-shatter"))   { *out = COMP_FX_CLOSE_SLICE_SHATTER;   return true; }
    return false;
}

const char *
comp_fx_close_preset_to_str(CompFxClosePreset preset)
{
    switch (preset) {
    case COMP_FX_CLOSE_DEFAULT:         return "default";
    case COMP_FX_CLOSE_FALL_AND_ROTATE: return "fall-and-rotate";
    case COMP_FX_CLOSE_CRT_OFF:         return "crt-off";
    case COMP_FX_CLOSE_ASH_DISSOLVE:    return "ash-dissolve";
    case COMP_FX_CLOSE_VORTEX_IMPLODE:  return "vortex-implode";
    case COMP_FX_CLOSE_PIXELATE_OUT:    return "pixelate-out";
    case COMP_FX_CLOSE_SLICE_SHATTER:   return "slice-shatter";
    }
    return "default";
}

bool
comp_fx_resize_preset_from_str(const char *s, CompFxResizePreset *out)
{
    if (!s || !out)
        return false;
    if (ascii_ieq(s, "default"))        { *out = COMP_FX_RESIZE_DEFAULT;        return true; }
    if (ascii_ieq(s, "stretch-next"))   { *out = COMP_FX_RESIZE_STRETCH_NEXT;   return true; }
    if (ascii_ieq(s, "pixelate-morph")) { *out = COMP_FX_RESIZE_PIXELATE_MORPH; return true; }
    if (ascii_ieq(s, "glitch-morph"))   { *out = COMP_FX_RESIZE_GLITCH_MORPH;   return true; }
    return false;
}

const char *
comp_fx_resize_preset_to_str(CompFxResizePreset preset)
{
    switch (preset) {
    case COMP_FX_RESIZE_DEFAULT:        return "default";
    case COMP_FX_RESIZE_STRETCH_NEXT:   return "stretch-next";
    case COMP_FX_RESIZE_PIXELATE_MORPH: return "pixelate-morph";
    case COMP_FX_RESIZE_GLITCH_MORPH:   return "glitch-morph";
    }
    return "default";
}

double
comp_fx_transition_eval_curve(CompFxTransitionCurve curve, double t)
{
    if (!isfinite(t) || t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;

    switch (curve) {
    case COMP_FX_CURVE_EASE_OUT_EXPO:
        return 1.0 - pow(2.0, -10.0 * t);
    case COMP_FX_CURVE_EASE_OUT_CUBIC: {
        double u = 1.0 - t;
        return 1.0 - u * u * u;
    }
    case COMP_FX_CURVE_EASE_OUT_QUAD: {
        double u = 1.0 - t;
        return 1.0 - u * u;
    }
    case COMP_FX_CURVE_LINEAR:
        return t;
    }
    return t;
}

void
comp_fx_transition_pack(CompFxRect dst_box,
                        CompFxTransitionKind kind,
                        uint32_t preset,
                        double progress,
                        double eased_progress,
                        float prev_w, float prev_h,
                        float curr_w, float curr_h,
                        float uv_scale0_x, float uv_scale0_y,
                        float uv_scale1_x, float uv_scale1_y,
                        float scale,
                        CompFxTransitionParams *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    float s = (scale > 0.0f) ? scale : 1.0f;

    out->common.dst_origin[0] = (float)dst_box.x;
    out->common.dst_origin[1] = (float)dst_box.y;
    out->common.dst_size[0]   = (float)dst_box.width;
    out->common.dst_size[1]   = (float)dst_box.height;
    out->common.opacity       = 1.0f;
    out->common.scale         = s;
    out->common.hdr_headroom  = 1.0f;
    out->common.flags         = 0;

    out->progress       = clamp01f((float)progress);
    out->eased_progress = clamp01f((float)eased_progress);
    out->preset         = preset;
    out->kind           = (uint32_t)kind;
    out->prev_size[0]   = fmaxf(prev_w, 1.0f);
    out->prev_size[1]   = fmaxf(prev_h, 1.0f);
    out->curr_size[0]   = fmaxf(curr_w, 1.0f);
    out->curr_size[1]   = fmaxf(curr_h, 1.0f);
    out->uv_scale0[0]   = (uv_scale0_x > 0.0f && uv_scale0_x <= 1.0f) ? uv_scale0_x : 1.0f;
    out->uv_scale0[1]   = (uv_scale0_y > 0.0f && uv_scale0_y <= 1.0f) ? uv_scale0_y : 1.0f;
    out->uv_scale1[0]   = (uv_scale1_x > 0.0f && uv_scale1_x <= 1.0f) ? uv_scale1_x : 1.0f;
    out->uv_scale1[1]   = (uv_scale1_y > 0.0f && uv_scale1_y <= 1.0f) ? uv_scale1_y : 1.0f;
}

static float
clamp_rangef(float v, float lo, float hi)
{
    if (!isfinite(v) || v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void
comp_fx_clamp_hdr_luminance(float edid_max_cll, float edid_max_fall,
                            float edid_min_luma, int32_t peak_override_nits,
                            double config_sdr_white_nits,
                            CompFxHdrLuminance *out)
{
    if (!out)
        return;

    float max_cll = (isfinite(edid_max_cll) && edid_max_cll > 0.0f)
                        ? clamp_rangef(edid_max_cll, 100.0f, 10000.0f)
                        : 400.0f;
    float eff_peak = (peak_override_nits >= 100 && peak_override_nits <= 10000)
                         ? (float)peak_override_nits
                         : max_cll;
    float max_fall = (isfinite(edid_max_fall) && edid_max_fall > 0.0f)
                         ? clamp_rangef(edid_max_fall, 50.0f, eff_peak)
                         : eff_peak;
    float min_luma = (isfinite(edid_min_luma) && edid_min_luma > 0.0f)
                         ? clamp_rangef(edid_min_luma, 0.0001f, 5.0f)
                         : 0.005f;

    float raw_sdr = (isfinite(config_sdr_white_nits) && config_sdr_white_nits > 0.0)
                        ? (float)config_sdr_white_nits
                        : 203.0f;
    float sdr_hi = fminf(500.0f, eff_peak);
    if (sdr_hi < 80.0f)
        sdr_hi = 80.0f;
    float sdr_white = clamp_rangef(raw_sdr, 80.0f, sdr_hi);

    out->max_cll_nits        = max_cll;
    out->max_fall_nits       = max_fall;
    out->min_luma_nits       = min_luma;
    out->effective_peak_nits = eff_peak;
    out->sdr_white_nits      = sdr_white;
}

static float
fp16_to_f32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) & 1u;
    uint32_t exp  = (uint32_t)(h >> 10) & 0x1fu;
    uint32_t mant = (uint32_t)h & 0x3ffu;
    float mag = 0.0f;
    if (exp == 0) {
        if (mant != 0)
            mag = ldexpf((float)mant, -24);
    } else if (exp == 31) {
        mag = (mant == 0) ? 65504.0f : 0.0f;
    } else {
        mag = ldexpf((float)(mant | 0x400u), (int)exp - 25);
    }
    return sign ? -mag : mag;
}

static float
linear_to_srgb_oetf(float c)
{
    float v = clamp01f(c);
    if (v <= 0.0031308f)
        return 12.92f * v;
    return clamp01f(1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
}

void
comp_fx_tonemap_fp16_to_argb8888(const uint16_t *fp16_abgr,
                                 uint8_t *out_argb8888,
                                 int width, int height)
{
    if (!fp16_abgr || !out_argb8888 || width <= 0 || height <= 0)
        return;

    size_t total = (size_t)width * (size_t)height;
    float max_val = 0.0f;
    for (size_t i = 0; i < total; i++) {
        const uint16_t *p = fp16_abgr + i * 4u;
        float a = clamp01f(fp16_to_f32(p[3]));
        if (a <= 1e-6f)
            continue;
        float r = fmaxf(0.0f, fp16_to_f32(p[0]) / a);
        float g = fmaxf(0.0f, fp16_to_f32(p[1]) / a);
        float b = fmaxf(0.0f, fp16_to_f32(p[2]) / a);
        float m = fmaxf(r, fmaxf(g, b));
        if (m > max_val)
            max_val = m;
    }

    const float ks = 0.75f;
    const float span = 1.0f - ks;
    bool compress = (max_val > 1.001f);
    float d_max = compress ? (max_val - ks) : 1.0f;
    uint32_t *dst = (uint32_t *)(void *)out_argb8888;

    for (size_t i = 0; i < total; i++) {
        const uint16_t *p = fp16_abgr + i * 4u;
        float a = clamp01f(fp16_to_f32(p[3]));
        if (a <= 1e-6f) {
            dst[i] = 0u;
            continue;
        }
        float r = fmaxf(0.0f, fp16_to_f32(p[0]) / a);
        float g = fmaxf(0.0f, fp16_to_f32(p[1]) / a);
        float b = fmaxf(0.0f, fp16_to_f32(p[2]) / a);

        if (compress) {
            float lum = fmaxf(r, fmaxf(g, b));
            if (lum > ks) {
                float d = lum - ks;
                float lum_out = ks + span * (d / (d + span)) * (1.0f + span / d_max);
                float ratio = clamp01f(lum_out) / lum;
                r *= ratio;
                g *= ratio;
                b *= ratio;
            }
        }

        float sr = linear_to_srgb_oetf(r) * a;
        float sg = linear_to_srgb_oetf(g) * a;
        float sb = linear_to_srgb_oetf(b) * a;

        uint32_t ua = (uint32_t)lroundf(clamp01f(a) * 255.0f);
        uint32_t ur = (uint32_t)lroundf(clamp01f(sr) * 255.0f);
        uint32_t ug = (uint32_t)lroundf(clamp01f(sg) * 255.0f);
        uint32_t ub = (uint32_t)lroundf(clamp01f(sb) * 255.0f);
        dst[i] = (ua << 24) | (ur << 16) | (ug << 8) | ub;
    }
}

bool
comp_fx_image_desc_requests_hdr(bool is_hdr_tf,
                                bool has_mastering_luminance,
                                float mastering_max_nits,
                                uint32_t max_cll,
                                uint32_t max_fall,
                                float sdr_white_nits)
{
    float sdr_ceil = (sdr_white_nits > 250.0f) ? sdr_white_nits : 250.0f;
    uint32_t sdr_ceil_u = (uint32_t)lroundf(sdr_ceil);

    if (max_cll > sdr_ceil_u || max_fall > sdr_ceil_u)
        return true;
    if (has_mastering_luminance && mastering_max_nits > sdr_ceil)
        return true;
    if (is_hdr_tf) {
        /* Clients like Chromium carry plain SDR pages in an ST2084_PQ container
         * with max_cll/max_fall capped at SDR white (~203 cd/m^2). Dedicated HDR
         * players (e.g. mpv) set ST2084_PQ/EXT_LINEAR with unset (0) caps. */
        bool capped_to_sdr = (max_cll > 0 || max_fall > 0 || has_mastering_luminance);
        return !capped_to_sdr;
    }
    return false;
}
