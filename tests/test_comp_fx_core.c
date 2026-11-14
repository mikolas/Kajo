/* test_comp_fx_core.c — unit tests for pure-C effects math, std140 packing,
 * transforms, and config defaults (spec §15 CB6). */
#include "../src/comp/core/comp_fx_core.h"
#include "../src/comp/core/comp_config.h"

#include <glib.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Verify std140 UBO layout & offsets match src/comp/fx/shaders/ shaders exactly. */
_Static_assert(sizeof(CompFxCommonParams) == 32, "CompFxCommonParams must be 32 bytes (std140)");
_Static_assert(sizeof(CompFxShadowParams) == 128, "CompFxShadowParams must be 128 bytes (std140)");
_Static_assert(sizeof(CompFxDimParams) == 64, "CompFxDimParams must be 64 bytes (std140)");
_Static_assert(offsetof(CompFxCommonParams, dst_origin) == 0, "dst_origin offset");
_Static_assert(offsetof(CompFxCommonParams, dst_size) == 8, "dst_size offset");
_Static_assert(offsetof(CompFxCommonParams, opacity) == 16, "opacity offset");
_Static_assert(offsetof(CompFxCommonParams, scale) == 20, "scale offset");
_Static_assert(offsetof(CompFxCommonParams, hdr_headroom) == 24, "hdr_headroom offset");
_Static_assert(offsetof(CompFxCommonParams, flags) == 28, "flags offset");
_Static_assert(offsetof(CompFxShadowParams, color_lin) == 32, "shadow color_lin offset");
_Static_assert(offsetof(CompFxShadowParams, outer_rect) == 48, "shadow outer_rect offset");
_Static_assert(offsetof(CompFxShadowParams, layers) == 64, "shadow layers offset");
_Static_assert(offsetof(CompFxDimParams, color_lin) == 32, "dim color_lin offset");
_Static_assert(offsetof(CompFxDimParams, outer_rect) == 48, "dim outer_rect offset");

static void
test_defaults_and_srgb(void)
{
    CompFxConfig cfg;
    comp_fx_config_defaults(&cfg);

    g_assert_true(cfg.enabled);
    g_assert_false(cfg.dither);
    g_assert_true(cfg.shadow.enabled);
    g_assert_cmpuint(cfg.shadow.n_layers, ==, 2);
    g_assert_cmpfloat_with_epsilon(cfg.shadow.unfocused_scale, 0.6, 1e-9);
    g_assert_cmpint(cfg.shadow.duration_ms, ==, 150);
    g_assert_false(cfg.dim.enabled);
    g_assert_cmpfloat_with_epsilon(cfg.dim.opacity, 0.15, 1e-9);

    g_assert_cmpfloat_with_epsilon(comp_fx_srgb_to_linear(-0.5f), 0.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(comp_fx_srgb_to_linear(0.0f), 0.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(comp_fx_srgb_to_linear(0.04045f), 0.04045f / 12.92f, 1e-6);
    g_assert_cmpfloat_with_epsilon(comp_fx_srgb_to_linear(0.5f), 0.214041f, 1e-4);
    g_assert_cmpfloat_with_epsilon(comp_fx_srgb_to_linear(1.5f), 1.0f, 1e-6);

    float srgb[4] = { 0.5f, 0.25f, 1.0f, 0.8f };
    float lin[4] = {0};
    comp_fx_rgba_srgb_to_linear(srgb, lin);
    g_assert_cmpfloat_with_epsilon(lin[0], comp_fx_srgb_to_linear(0.5f), 1e-6);
    g_assert_cmpfloat_with_epsilon(lin[1], comp_fx_srgb_to_linear(0.25f), 1e-6);
    g_assert_cmpfloat_with_epsilon(lin[2], 1.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(lin[3], 0.8f, 1e-6);
}

static void
test_reach_and_anchor_box(void)
{
    CompFxShadowConfig sh = {0};
    sh.enabled = true;
    sh.n_layers = 2;
    sh.layers[0] = (CompFxShadowLayer){ .sigma = 6.0, .offset_x = 0.0, .offset_y = 2.0, .opacity = 0.30 };
    sh.layers[1] = (CompFxShadowLayer){ .sigma = 24.0, .offset_x = 0.0, .offset_y = 8.0, .opacity = 0.18 };

    /* 3 * 24 = 72 px; top = 72 - 8 = 64, bottom = 72 + 8 = 80, left = 72, right = 72. */
    CompFxMargins m = comp_fx_shadow_reach(&sh);
    g_assert_cmpint(m.left, ==, 72);
    g_assert_cmpint(m.top, ==, 64);
    g_assert_cmpint(m.right, ==, 72);
    g_assert_cmpint(m.bottom, ==, 80);

    CompFxRect outer = { .x = 100, .y = 200, .width = 400, .height = 300 };
    CompFxRect anchor = comp_fx_shadow_anchor_box(outer, &sh);
    g_assert_cmpint(anchor.x, ==, 100 - 72);
    g_assert_cmpint(anchor.y, ==, 200 - 64);
    g_assert_cmpint(anchor.width, ==, 400 + 72 + 72);
    g_assert_cmpint(anchor.height, ==, 300 + 64 + 80);

    /* Offset larger than 3*sigma clamps opposite side to >= 0, and caps at 256 px. */
    CompFxShadowConfig sh_extreme = {0};
    sh_extreme.enabled = true;
    sh_extreme.n_layers = 1;
    sh_extreme.layers[0] = (CompFxShadowLayer){ .sigma = 1.0, .offset_x = 200.0, .offset_y = -200.0, .opacity = 0.5 };
    CompFxMargins m2 = comp_fx_shadow_reach(&sh_extreme);
    g_assert_cmpint(m2.left, ==, 0);
    g_assert_cmpint(m2.right, ==, 203);
    g_assert_cmpint(m2.top, ==, 203);
    g_assert_cmpint(m2.bottom, ==, 0);

    /* 3*120 = 360; right = 360 + 50 = 410, left = 360 - 50 = 310. */
    sh_extreme.layers[0] = (CompFxShadowLayer){ .sigma = 120.0, .offset_x = 50.0, .offset_y = 0.0, .opacity = 0.5 };
    CompFxMargins m3 = comp_fx_shadow_reach(&sh_extreme);
    g_assert_cmpint(m3.right, ==, 410);
    g_assert_cmpint(m3.left, ==, 310);
}

static void
test_coverage_gaussian_box(void)
{
    double x0 = 100.0, y0 = 100.0, x1 = 500.0, y1 = 400.0;
    double sigma = 10.0;

    /* Deep interior approaches 1.0 */
    double center = comp_fx_shadow_coverage(300.0, 250.0, x0, y0, x1, y1, sigma);
    g_assert_cmpfloat_with_epsilon(center, 1.0, 1e-6);

    /* Center of straight top edge is 0.5 in Y and ~1.0 in X -> ~0.5 */
    double edge = comp_fx_shadow_coverage(300.0, y0, x0, y0, x1, y1, sigma);
    g_assert_cmpfloat_with_epsilon(edge, 0.5, 1e-4);

    /* Corner is 0.5 * 0.5 = 0.25 */
    double corner = comp_fx_shadow_coverage(x0, y0, x0, y0, x1, y1, sigma);
    g_assert_cmpfloat_with_epsilon(corner, 0.25, 1e-4);

    /* 1*sigma outside straight edge: 0.5 * erfc(1/sqrt(2)) ≈ 0.158655 */
    double one_sigma = comp_fx_shadow_coverage(300.0, y0 - sigma, x0, y0, x1, y1, sigma);
    g_assert_cmpfloat_with_epsilon(one_sigma, 0.158655, 1e-4);

    /* Monotonic decrease away from edge */
    double two_sigma = comp_fx_shadow_coverage(300.0, y0 - 2.0 * sigma, x0, y0, x1, y1, sigma);
    g_assert_cmpfloat(two_sigma, <, one_sigma);
    g_assert_cmpfloat(two_sigma, >, 0.0);
}

static void
test_transforms_and_packing(void)
{
    /* Test transform at fractional scale 1.75 and 90-degree rotation (trans_w=1920, trans_h=1080 buffer). */
    CompFxRect out_log = { .x = 0, .y = 0, .width = 1097, .height = 617 };
    CompFxRect box_log = { .x = 100, .y = 80, .width = 200, .height = 120 };

    CompFxRect b_norm = comp_fx_transform_box(box_log, out_log, 1.75f, COMP_TRANSFORM_NORMAL, 1920, 1080);
    /* round(100*1.75)=175, round(300*1.75)-175=525-175=350; round(80*1.75)=140, round(200*1.75)-140=350-140=210 */
    g_assert_cmpint(b_norm.x, ==, 175);
    g_assert_cmpint(b_norm.y, ==, 140);
    g_assert_cmpint(b_norm.width, ==, 350);
    g_assert_cmpint(b_norm.height, ==, 210);

    CompFxRect b_90 = comp_fx_transform_box(box_log, out_log, 1.0f, COMP_TRANSFORM_90, 1080, 1097);
    g_assert_cmpint(b_90.width, ==, 120);
    g_assert_cmpint(b_90.height, ==, 200);

    float ox = 0.0f, oy = 0.0f;
    comp_fx_transform_offset(4.0, 10.0, 2.0f, COMP_TRANSFORM_90, &ox, &oy);
    g_assert_cmpfloat(hypot((double)ox, (double)oy), >, 20.0);

    /* Pack shadow UBO & check std140 fields */
    CompFxConfig cfg;
    comp_fx_config_defaults(&cfg);
    cfg.shadow.color_rgba[0] = 0.5f;
    cfg.shadow.color_rgba[1] = 0.0f;
    cfg.shadow.color_rgba[2] = 0.25f;
    cfg.shadow.color_rgba[3] = 1.0f;

    CompFxRect dst_box = { .x = 40, .y = 50, .width = 500, .height = 400 };
    CompFxRect outer_buf = { .x = 120, .y = 110, .width = 350, .height = 210 };
    CompFxShadowParams sp;
    memset(&sp, 0xAA, sizeof(sp));
    comp_fx_shadow_pack(&cfg.shadow, dst_box, outer_buf, 0.75f, 1.75f, COMP_TRANSFORM_NORMAL, &sp);

    g_assert_cmpfloat_with_epsilon(sp.common.dst_origin[0], 40.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.common.dst_origin[1], 50.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.common.dst_size[0], 500.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.common.dst_size[1], 400.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.common.opacity, 0.75f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.common.scale, 1.75f, 1e-6);
    g_assert_cmpuint(sp.common.flags, ==, 2u);
    g_assert_cmpfloat_with_epsilon(sp.color_lin[0], comp_fx_srgb_to_linear(0.5f), 1e-5);
    g_assert_cmpfloat_with_epsilon(sp.outer_rect[0], 120.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.outer_rect[2], 470.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.layers[0][2], 6.0f * 1.75f, 1e-5);
    g_assert_cmpfloat_with_epsilon(sp.layers[0][3], 0.30f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.layers[1][1], 8.0f * 1.75f, 1e-5);
    /* Unused layers zeroed */
    g_assert_cmpfloat_with_epsilon(sp.layers[2][2], 0.0f, 1e-6);
    g_assert_cmpfloat_with_epsilon(sp.layers[3][3], 0.0f, 1e-6);

    /* Pack dim UBO */
    cfg.dim.enabled = true;
    cfg.dim.color_rgba[0] = 0.1f;
    CompFxDimParams dp;
    memset(&dp, 0xAA, sizeof(dp));
    comp_fx_dim_pack(&cfg.dim, outer_buf, outer_buf, 0.25f, 2.0f, &dp);
    g_assert_cmpfloat_with_epsilon(dp.common.opacity, 0.25f, 1e-6);
    g_assert_cmpfloat_with_epsilon(dp.common.scale, 2.0f, 1e-6);
    g_assert_cmpuint(dp.common.flags, ==, 0u);
    g_assert_cmpfloat_with_epsilon(dp.color_lin[0], comp_fx_srgb_to_linear(0.1f), 1e-5);
    g_assert_cmpfloat_with_epsilon(dp.outer_rect[3], 320.0f, 1e-6);

    /* Focus spring params */
    CompSpringParams spr;
    g_assert_false(comp_fx_focus_spring_params(0, 1.0, &spr));
    g_assert_false(comp_fx_focus_spring_params(150, 0.0, &spr));
    g_assert_true(comp_fx_focus_spring_params(150, 1.0, &spr));
    g_assert_cmpfloat(spr.stiffness, >, 0.0);
    g_assert_cmpfloat_with_epsilon(spr.damping_ratio, 1.0, 1e-9);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/fx-core/defaults-and-srgb", test_defaults_and_srgb);
    g_test_add_func("/comp/fx-core/reach-and-anchor-box", test_reach_and_anchor_box);
    g_test_add_func("/comp/fx-core/coverage-gaussian-box", test_coverage_gaussian_box);
    g_test_add_func("/comp/fx-core/transforms-and-packing", test_transforms_and_packing);
    return g_test_run();
}
