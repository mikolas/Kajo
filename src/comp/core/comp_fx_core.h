/* comp_fx_core.h — pure-C effects config, colour math, coordinate transforms,
 * and std140 UBO parameter packing for kajo-comp (no wlroots, Vulkan, or GLib). */
#ifndef KAJO_COMP_FX_CORE_H
#define KAJO_COMP_FX_CORE_H

#include "comp_spring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMP_FX_SHADOW_MAX_LAYERS 4

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} CompFxRect;

typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} CompFxMargins;

typedef struct {
    double sigma;     /* blur sigma in logical px (> 0..256) */
    double offset_x;  /* logical px (-256..256) */
    double offset_y;  /* logical px (-256..256) */
    double opacity;   /* 0..1 */
} CompFxShadowLayer;

typedef struct {
    bool              enabled;
    float             color_rgba[4];     /* config sRGB [0..1] */
    double            unfocused_scale;   /* 0..2 */
    int32_t           duration_ms;       /* focus animation duration (0..5000 ms) */
    CompFxShadowLayer layers[COMP_FX_SHADOW_MAX_LAYERS];
    size_t            n_layers;
} CompFxShadowConfig;

typedef struct {
    bool    enabled;
    float   color_rgba[4];               /* config sRGB [0..1] */
    double  opacity;                     /* 0..1 */
    int32_t duration_ms;                 /* 0..5000 ms */
} CompFxDimConfig;

typedef struct {
    bool               enabled;
    bool               dither;
    CompFxShadowConfig shadow;
    CompFxDimConfig    dim;
} CompFxConfig;

/* Common 32-byte header shared by all Kajo effect shaders (std140, 16-byte aligned).
 * Matches src/comp/fx/shaders/fx_common.glsl FX_COMMON_HEADER. */
typedef struct {
    float    dst_origin[2];  /* 0..8:   draw dst_box top-left in output-buffer px */
    float    dst_size[2];    /* 8..16:  draw dst_box size in output-buffer px */
    float    opacity;        /* 16..20: overall effect strength/opacity [0..1] */
    float    scale;          /* 20..24: output scale factor */
    float    hdr_headroom;   /* 24..28: output max / SDR white (1.0 on SDR) */
    uint32_t flags;          /* 28..32: effect-specific flags / layer count */
} CompFxCommonParams;

/* Shadow fragment shader UBO (std140, 128 bytes <= 256).
 * Matches src/comp/fx/shaders/shadow.frag Params. */
typedef struct {
    CompFxCommonParams common;                             /* offset 0 */
    float              color_lin[4];                       /* offset 32: linear RGB + A */
    float              outer_rect[4];                      /* offset 48: x0, y0, x1, y1 in buffer px */
    float              layers[COMP_FX_SHADOW_MAX_LAYERS][4]; /* offset 64: (off_x_px, off_y_px, sigma_px, opacity) */
} CompFxShadowParams;

/* Dim fragment shader UBO (std140, 64 bytes <= 256).
 * Matches src/comp/fx/shaders/dim.frag Params. */
typedef struct {
    CompFxCommonParams common;        /* offset 0 */
    float              color_lin[4];  /* offset 32: linear RGB + A */
    float              outer_rect[4]; /* offset 48: x0, y0, x1, y1 in buffer px */
} CompFxDimParams;

void comp_fx_config_defaults(CompFxConfig *cfg);

/* IEC 61966-2-1 sRGB <-> linear conversion matching wlroots output/rect encoding (spec §5 H2). */
float comp_fx_srgb_to_linear(float v);
void  comp_fx_rgba_srgb_to_linear(const float srgb[4], float out_lin[4]);

/* Shadow paint reach (max over active layers of ceil(max(0, 3*sigma +/- offset))). */
CompFxMargins comp_fx_shadow_reach(const CompFxShadowConfig *cfg);
CompFxRect    comp_fx_shadow_anchor_box(CompFxRect outer_box, const CompFxShadowConfig *cfg);

/* Closed-form 2D Gaussian box coverage at point (x, y) for box [x0, x1] x [y0, y1]
 * with blur standard deviation `sigma` (spec §11.2). */
double comp_fx_shadow_coverage(double x, double y,
                               double x0, double y0, double x1, double y1,
                               double sigma);

/* Logical-to-output-buffer coordinate transform matching wlr_scene.c's
 * scale_box() + wlr_box_transform(invert(transform), trans_w, trans_h).
 * `transform` uses wl_output_transform / CompTransform enum values 0..7. */
CompFxRect comp_fx_transform_box(CompFxRect box_logical, CompFxRect output_logical,
                                 float scale, int transform,
                                 int32_t trans_width, int32_t trans_height);

void comp_fx_transform_offset(double off_x, double off_y, float scale, int transform,
                              float *out_x, float *out_y);

/* Packs std140 UBOs for shadow and dim fragment shaders. */
void comp_fx_shadow_pack(const CompFxShadowConfig *cfg,
                         CompFxRect anchor_dst_box,
                         CompFxRect outer_buf_box,
                         float opacity, float scale, int transform,
                         CompFxShadowParams *out);

void comp_fx_dim_pack(const CompFxDimConfig *cfg,
                      CompFxRect dst_box,
                      CompFxRect outer_buf_box,
                      float opacity, float scale,
                      CompFxDimParams *out);

/* Builds a critically damped CompSpringParams for focus opacity transitions.
 * Returns false when animation should snap immediately (duration_ms <= 0 or slowdown <= 0). */
bool comp_fx_focus_spring_params(int32_t duration_ms, double slowdown, CompSpringParams *out);

#endif /* KAJO_COMP_FX_CORE_H */
