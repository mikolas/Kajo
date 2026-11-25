/* comp_fx_core.h — pure-C effects config, colour math, coordinate transforms,
 * and std140 UBO parameter packing for kajo-comp (no wlroots, Vulkan, or GLib). */
#ifndef KAJO_COMP_FX_CORE_H
#define KAJO_COMP_FX_CORE_H

#include "comp_spring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMP_FX_SHADOW_MAX_LAYERS 4
#define COMP_FX_GLASS_MAX_PASSES  6

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
    double            strength;          /* 0..2 multiplier on layer opacities */
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

typedef enum {
    COMP_FX_GLASS_FROSTED  = 0,
    COMP_FX_GLASS_LIQUID   = 1,
    COMP_FX_GLASS_HALATION = 2,
    COMP_FX_GLASS_FLUTED   = 3,
    COMP_FX_GLASS_ACRYLIC  = 4,
    COMP_FX_GLASS_AERO     = 5,
    COMP_FX_GLASS_DICHROIC = 6,
} CompFxGlassStyle;

typedef enum {
    COMP_FX_FLUTE_VERTICAL   = 0,
    COMP_FX_FLUTE_HORIZONTAL = 1,
} CompFxFluteAxis;

bool        comp_fx_glass_style_from_str(const char *s, CompFxGlassStyle *out);
const char *comp_fx_glass_style_to_str(CompFxGlassStyle style);
bool        comp_fx_glass_style_is_dual_input(CompFxGlassStyle style);
bool        comp_fx_flute_axis_from_str(const char *s, CompFxFluteAxis *out);
const char *comp_fx_flute_axis_to_str(CompFxFluteAxis axis);

typedef struct {
    bool             enabled;
    CompFxGlassStyle style;              /* default COMP_FX_GLASS_FROSTED */
    int32_t          passes;             /* 1..COMP_FX_GLASS_MAX_PASSES (default 3) */
    double           offset;             /* sample offset per pass in px (0..16, default 2.0) */
    double           saturation;         /* 0..4 (default 1.15) */
    double           brightness;         /* linear light multiplier 0..4 (default 0.90) */
    float            tint_rgba[4];       /* config sRGB [0..1] (default #141414) */
    double           tint_alpha;         /* 0..1 (default 0.35) */
    int32_t          max_recompute_hz;   /* 0 = off, 1..1000 Hz */
    double           refraction;         /* 0..2 (default 0.45: liquid, fluted) */
    double           dispersion;         /* 0..1 (default 0.25: liquid, fluted, dichroic) */
    double           bevel_px;           /* 2..64 logical px (default 14.0: liquid, aero, dichroic) */
    double           specular;           /* 0..2 (default 0.55) */
    double           grain;              /* 0..1 (default 0.35: acrylic roughness / halation scatter) */
    double           flute_px;           /* 4..64 logical px (default 14.0: fluted rib pitch) */
    CompFxFluteAxis  flute_axis;         /* default COMP_FX_FLUTE_VERTICAL */
} CompFxGlassConfig;

typedef enum {
    COMP_FX_FILTER_NONE         = 0,
    COMP_FX_FILTER_GRAYSCALE    = 1,
    COMP_FX_FILTER_NIGHT_LIGHT  = 2,
    COMP_FX_FILTER_PROTANOPIA   = 3,
    COMP_FX_FILTER_DEUTERANOPIA = 4,
    COMP_FX_FILTER_TRITANOPIA   = 5,
} CompFxColorFilter;

bool        comp_fx_color_filter_from_str(const char *s, CompFxColorFilter *out);
const char *comp_fx_color_filter_to_str(CompFxColorFilter filter);

typedef enum {
    COMP_FX_CURVE_EASE_OUT_EXPO  = 0,
    COMP_FX_CURVE_EASE_OUT_CUBIC = 1,
    COMP_FX_CURVE_EASE_OUT_QUAD  = 2,
    COMP_FX_CURVE_LINEAR         = 3,
} CompFxTransitionCurve;

typedef enum {
    COMP_FX_OPEN_DEFAULT          = 0,
    COMP_FX_OPEN_METRO_FLIP       = 1,
    COMP_FX_OPEN_EXPANDING_CIRCLE = 2,
    COMP_FX_OPEN_CRT_ON           = 3,
    COMP_FX_OPEN_PIXELATE_IN      = 4,
    COMP_FX_OPEN_GLITCH_IN        = 5,
    COMP_FX_OPEN_CURTAIN_OPEN     = 6,
} CompFxOpenPreset;

typedef enum {
    COMP_FX_CLOSE_DEFAULT         = 0,
    COMP_FX_CLOSE_FALL_AND_ROTATE = 1,
    COMP_FX_CLOSE_CRT_OFF         = 2,
    COMP_FX_CLOSE_ASH_DISSOLVE    = 3,
    COMP_FX_CLOSE_VORTEX_IMPLODE  = 4,
    COMP_FX_CLOSE_PIXELATE_OUT    = 5,
    COMP_FX_CLOSE_SLICE_SHATTER   = 6,
} CompFxClosePreset;

typedef enum {
    COMP_FX_RESIZE_DEFAULT        = 0,
    COMP_FX_RESIZE_STRETCH_NEXT   = 1,
    COMP_FX_RESIZE_PIXELATE_MORPH = 2,
    COMP_FX_RESIZE_GLITCH_MORPH   = 3,
} CompFxResizePreset;

typedef enum {
    COMP_FX_TRANS_OPEN   = 0,
    COMP_FX_TRANS_CLOSE  = 1,
    COMP_FX_TRANS_RESIZE = 2,
} CompFxTransitionKind;

typedef struct {
    bool                  enabled;
    CompFxOpenPreset      open_preset;
    int32_t               open_duration_ms;
    CompFxTransitionCurve open_curve;
    CompFxClosePreset     close_preset;
    int32_t               close_duration_ms;
    CompFxTransitionCurve close_curve;
    CompFxResizePreset    resize_preset;
    int32_t               resize_duration_ms;
} CompFxTransitionsConfig;

bool        comp_fx_curve_from_str(const char *s, CompFxTransitionCurve *out);
const char *comp_fx_curve_to_str(CompFxTransitionCurve curve);
bool        comp_fx_open_preset_from_str(const char *s, CompFxOpenPreset *out);
const char *comp_fx_open_preset_to_str(CompFxOpenPreset preset);
bool        comp_fx_close_preset_from_str(const char *s, CompFxClosePreset *out);
const char *comp_fx_close_preset_to_str(CompFxClosePreset preset);
bool        comp_fx_resize_preset_from_str(const char *s, CompFxResizePreset *out);
const char *comp_fx_resize_preset_to_str(CompFxResizePreset preset);
double      comp_fx_transition_eval_curve(CompFxTransitionCurve curve, double t);

typedef struct {
    bool                    enabled;
    bool                    dither;
    CompFxColorFilter       color_filter;            /* default COMP_FX_FILTER_NONE */
    double                  color_filter_strength;   /* 0..1 (default 1.0) */
    int32_t                 night_light_temperature; /* 1500..6500 K (default 4000 K) */
    double                  sdr_white_nits;          /* 0 = default (203 cd/m^2), or 80..1000 */
    bool                    tonemap;                 /* default true */
    double                  tonemap_peak_nits;       /* 0 = default/off, or 100..10000 cd/m^2 */
    CompFxShadowConfig      shadow;
    CompFxDimConfig         dim;
    CompFxGlassConfig       glass;
    CompFxTransitionsConfig transitions;
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

/* Kawase blur pass fragment shader UBO (std140, 80 bytes <= 256).
 * Matches src/comp/fx/shaders/blur_down.frag and blur_up.frag Params. */
typedef struct {
    CompFxCommonParams common;          /* offset 0 */
    float              src_rect[4];     /* offset 32: valid (x, y, w, h) in source target px */
    float              valid_bounds[4]; /* offset 48: clamp (min_x, min_y, max_x, max_y) in source px */
    float              inv_tex_size[2]; /* offset 64: (1.0 / tex_w, 1.0 / tex_h) */
    float              offset;          /* offset 72: sample offset in source texels */
    float              _pad0;           /* offset 76: std140 16-byte struct alignment */
} CompFxBlurParams;

/* Glass composite fragment shader UBO (std140, 192 bytes <= 256).
 * Matches src/comp/fx/shaders/glass.frag and glass_dual.frag Params. */
typedef struct {
    CompFxCommonParams common;              /*   0..32 */
    float              crop_rect[4];        /*  32..48:  (x, y, w, h) in u_tex0 px */
    float              valid_bounds[4];     /*  48..64:  clamp (min_x, min_y, max_x, max_y) in u_tex0 px */
    float              tint_lin[4];         /*  64..80:  linear RGB + tint_alpha in .a */
    float              inv_tex_size[2];     /*  80..88:  (1.0 / tex0_w, 1.0 / tex0_h) */
    float              saturation;          /*  88..92 */
    float              brightness;          /*  92..96 */
    float              aux_crop_rect[4];    /*  96..112: (x, y, w, h) in u_tex1 px (dual-input) */
    float              aux_valid_bounds[4]; /* 112..128: clamp bounds in u_tex1 px */
    float              aux_inv_tex_size[2]; /* 128..136: (1.0 / tex1_w, 1.0 / tex1_h) */
    float              surface_size[2];     /* 136..144: full surface logical size (W, H) */
    float              light_dir[2];        /* 144..152: pre-rotated key light / flute axis */
    float              surface_offset[2];   /* 152..160: clipped pass_dst - full_glass_buf (logical px) */
    uint32_t           style;               /* 160..164: CompFxGlassStyle (0..6) */
    float              refraction;          /* 164..168: 0..2 */
    float              dispersion;          /* 168..172: 0..1 */
    float              bevel_px;            /* 172..176: 2..64 logical px */
    float              specular;            /* 176..180: 0..2 */
    float              grain;               /* 180..184: 0..1 (acrylic roughness / halation scatter) */
    float              flute_px;            /* 184..188: 4..64 logical px */
    float              _pad0;               /* 188..192: std140 16-byte struct alignment */
} CompFxGlassParams;

/* Window transition fragment shader UBO (std140, 80 bytes <= 256).
 * Matches src/comp/fx/shaders/transition_single.frag and transition_resize.frag Params. */
typedef struct {
    CompFxCommonParams common;         /* offset 0 */
    float              progress;       /* offset 32: linear progress 0..1 */
    float              eased_progress; /* offset 36: eased progress 0..1 */
    uint32_t           preset;         /* offset 40: preset enum id */
    uint32_t           kind;           /* offset 44: 0=open, 1=close, 2=resize */
    float              prev_size[2];   /* offset 48: previous logical size */
    float              curr_size[2];   /* offset 56: current logical size */
    float              uv_scale0[2];   /* offset 64: valid/bucket UV scale for u_tex0 */
    float              uv_scale1[2];   /* offset 72: valid/bucket UV scale for u_tex1 */
} CompFxTransitionParams;

void comp_fx_config_defaults(CompFxConfig *cfg);

/* IEC 61966-2-1 sRGB <-> linear conversion matching wlroots output/rect encoding (spec §5 H2). */
float comp_fx_srgb_to_linear(float v);
float comp_fx_linear_to_srgb(float v);
float comp_fx_linear_to_gamma22(float v);
void  comp_fx_rgba_srgb_to_linear(const float srgb[4], float out_lin[4]);

/* ST.2084 PQ inverse EOTF and ITU-R BT.2390-11 §5.4 EETF Hermite roll-off (spec §6 X6). */
float comp_fx_linear_to_pq(float v);
float comp_fx_bt2390_pq(float e1, float max_pq);

/* Computes the 3x3 row-major linear-light colour filter matrix for `cfg` (spec §6 X6, M4).
 * Always writes a valid 3x3 matrix to `out_matrix`; returns true when a non-identity
 * filter is active (`cfg->enabled`, `color_filter != NONE`, `color_filter_strength > 0`). */
bool  comp_fx_color_filter_matrix(const CompFxConfig *cfg, float out_matrix[9]);
void  comp_fx_color_filter_apply_linear(const float matrix[9],
                                        const float in_rgb[3],
                                        float out_rgb[3]);

/* Shadow paint reach (max over active layers of ceil(max(0, 3*sigma +/- offset))). */
CompFxMargins comp_fx_shadow_reach(const CompFxShadowConfig *cfg);
CompFxRect    comp_fx_shadow_anchor_box(CompFxRect outer_box, const CompFxShadowConfig *cfg);

/* Glass sample reach: ceil(2^passes * offset) + optical displacement in logical px. */
int32_t    comp_fx_glass_sample_reach(const CompFxGlassConfig *cfg);
int32_t    comp_fx_glass_sample_reach_for_style(const CompFxGlassConfig *cfg,
                                                CompFxGlassStyle style);
CompFxRect comp_fx_glass_sample_box(CompFxRect surface_box, const CompFxGlassConfig *cfg);
CompFxRect comp_fx_glass_sample_box_for_style(CompFxRect surface_box,
                                              const CompFxGlassConfig *cfg,
                                              CompFxGlassStyle style);

/* Closed-form 2D Gaussian box coverage at point (x, y) for box [x0, x1] x [y0, y1]
 * with blur standard deviation `sigma` (spec §11.2). */
double comp_fx_shadow_coverage(double x, double y,
                               double x0, double y0, double x1, double y1,
                               double sigma);

/* Computes effective logical output dimensions ceil(trans_width / scale),
 * ceil(trans_height / scale) matching wlr_output_effective_resolution(). */
void comp_fx_effective_resolution(int32_t trans_width, int32_t trans_height,
                                  float scale,
                                  int32_t *out_width, int32_t *out_height);

/* Clamps `r` to [0, max_w) x [0, max_h). Returns {0} when non-intersecting. */
CompFxRect comp_fx_clamp_rect(CompFxRect r, int32_t max_w, int32_t max_h);

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

void comp_fx_blur_pack(CompFxRect dst_box,
                       CompFxRect src_rect,
                       int32_t tex_w, int32_t tex_h,
                       double offset,
                       CompFxBlurParams *out);

void comp_fx_glass_pack(const CompFxGlassConfig *cfg,
                        CompFxRect dst_box,
                        CompFxRect t0_src_box,
                        int32_t tex_w, int32_t tex_h,
                        int downscale,
                        float opacity, float scale,
                        CompFxGlassParams *out);

void comp_fx_glass_pack_ex(const CompFxGlassConfig *cfg,
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
                           CompFxGlassParams *out);

void comp_fx_transition_pack(CompFxRect dst_box,
                             CompFxTransitionKind kind,
                             uint32_t preset,
                             double progress,
                             double eased_progress,
                             float prev_w, float prev_h,
                             float curr_w, float curr_h,
                             float uv_scale0_x, float uv_scale0_y,
                             float uv_scale1_x, float uv_scale1_y,
                             float scale,
                             CompFxTransitionParams *out);

/* Builds a critically damped CompSpringParams for focus opacity transitions.
 * Returns false when animation should snap immediately (duration_ms <= 0 or slowdown <= 0). */
bool comp_fx_focus_spring_params(int32_t duration_ms, double slowdown, CompSpringParams *out);

typedef struct {
    float max_cll_nits;
    float max_fall_nits;
    float min_luma_nits;
    float effective_peak_nits;
    float sdr_white_nits;
} CompFxHdrLuminance;

/* Physical luminance clamping invariants (spec_comp_hdr.md §2.4.3). */
void comp_fx_clamp_hdr_luminance(float edid_max_cll, float edid_max_fall,
                                 float edid_min_luma, int32_t peak_override_nits,
                                 double config_sdr_white_nits,
                                 CompFxHdrLuminance *out);

/* Tone-maps an offscreen premultiplied linear RGBA16F (DRM_FORMAT_ABGR16161616F)
 * buffer to premultiplied sRGB ARGB8888 for faithful SDR PNG/clipboard screenshots. */
void comp_fx_tonemap_fp16_to_argb8888(const uint16_t *fp16_abgr,
                                      uint8_t *out_argb8888,
                                      int width, int height);

#endif /* KAJO_COMP_FX_CORE_H */
