/* test_comp_hdr.c — Stage 0 validation tests for kajo-comp HDR10:
 *   1. DisplayID 2.0 CTA-861 EDID parser (Host A Samsung eDP-1 fixture + synthetic edge cases)
 *   2. Headless Vulkan 10-bit PQ (DRM_FORMAT_XRGB2101010) readback invariants:
 *      - SDR white scaling (100 / 203 / 400 cd/m^2) without PQ supernova
 *      - HDR PQ 600 cd/m^2 invariance across sdr_white_nits overrides
 *      - Auto-peak BT.2390 tone mapping from output image_description (tonemap_peak_nits = 0)
 *      - Unclamped linear Night Light (3000K) over HDR PQ highlights
 */
#include "../src/comp/comp_server.h"
#include "../src/comp/fx/comp_fx.h"
#include "../src/comp/fx/fx_internal.h"

#include <drm_fourcc.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/kfx/scene.h>
#include <wlr/render/allocator.h>
#include <wlr/render/color.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

double
comp_now_sec(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Host A /sys/class/drm/card0-eDP-1/edid (Samsung Display Corp. 0x41AA, 256 bytes, DisplayID 2.0) */
static const uint8_t kHostAEdid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4c, 0x83, 0xaa, 0x41, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x20, 0x01, 0x04, 0xb5, 0x1e, 0x13, 0x78, 0x03, 0xcf, 0xd1, 0xae, 0x51, 0x3e, 0xb6, 0x23,
    0x0b, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xcb, 0xfe, 0x40, 0x64, 0xb0, 0x08, 0x18, 0x70, 0x20, 0x08,
    0x88, 0x00, 0x2e, 0xbd, 0x10, 0x00, 0x00, 0x1b, 0xcb, 0xfe, 0x40, 0x64, 0xb0, 0x08, 0xc8, 0x7a,
    0x20, 0x08, 0x88, 0x00, 0x2e, 0xbd, 0x10, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30,
    0x78, 0xda, 0xda, 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x01, 0x00, 0x00, 0x19, 0x96, 0xc8, 0x3a, 0x15, 0x46, 0xc8, 0x00, 0x00, 0x00, 0x01, 0x3d,
    0x70, 0x20, 0x79, 0x02, 0x00, 0x20, 0x00, 0x0c, 0xba, 0x41, 0x59, 0xaa, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x16, 0x00, 0x21, 0x00, 0x1d, 0xb8, 0x0b, 0x6c, 0x07, 0x40, 0x0b, 0x08, 0x07, 0x00,
    0xee, 0xea, 0x50, 0xec, 0xd3, 0xb6, 0x3d, 0x42, 0x0b, 0x01, 0x45, 0x54, 0x40, 0x5e, 0xd0, 0x60,
    0x18, 0x10, 0x23, 0x78, 0x26, 0x00, 0x09, 0x07, 0x06, 0x03, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00,
    0x22, 0x00, 0x14, 0xe7, 0xf3, 0x09, 0x85, 0x3f, 0x0b, 0x63, 0x00, 0x1f, 0x00, 0x07, 0x00, 0x07,
    0x07, 0x17, 0x00, 0x07, 0x00, 0x07, 0x00, 0x81, 0x00, 0x1f, 0x73, 0x1a, 0x00, 0x00, 0x03, 0x03,
    0x30, 0x78, 0x00, 0xa0, 0x74, 0x02, 0x60, 0x02, 0x78, 0x00, 0x00, 0x00, 0x00, 0x8d, 0xe3, 0x05,
    0x80, 0x00, 0xe6, 0x06, 0x05, 0x01, 0x74, 0x60, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x90,
};

static void
fix_edid_and_did2_checksums(uint8_t edid[static 256])
{
    uint8_t sum_base = 0;
    for (size_t i = 0; i < 127; i++)
        sum_base = (uint8_t)(sum_base + edid[i]);
    edid[127] = (uint8_t)(-sum_base);

    uint8_t sec_len = edid[128 + 2];
    uint8_t did_sum = 0;
    for (size_t i = 1; i < (size_t)sec_len + 5; i++)
        did_sum = (uint8_t)(did_sum + edid[128 + i]);
    edid[128 + 5 + sec_len] = (uint8_t)(-did_sum);

    uint8_t sum_ext = 0;
    for (size_t i = 0; i < 127; i++)
        sum_ext = (uint8_t)(sum_ext + edid[128 + i]);
    edid[255] = (uint8_t)(-sum_ext);
}

static void
test_edid_did2_parser(void)
{
    struct wlr_kfx_output_hdr_caps caps = {0};
    g_assert_true(wlr_kfx_edid_parse_hdr_caps(kHostAEdid, sizeof(kHostAEdid), &caps));
    g_assert_true(caps.supported);
    g_assert_cmpfloat_with_epsilon(caps.max_cll_nits, 616.884f, 0.1f);
    g_assert_cmpfloat_with_epsilon(caps.max_fall_nits, 400.0f, 0.1f);
    g_assert_cmpfloat_with_epsilon(caps.min_luma_nits, 0.000379f, 1e-5f);
    g_test_message("H2 HostA EDID: supported=%d max_cll=%.3f max_fall=%.3f min_luma=%.6f",
                   caps.supported, caps.max_cll_nits, caps.max_fall_nits, caps.min_luma_nits);

    /* 1. CTA-861-H CV==0 means unset (0.0f, NEVER 50.0f) */
    uint8_t edid_zero_cv[256];
    memcpy(edid_zero_cv, kHostAEdid, sizeof(edid_zero_cv));
    /* Offset 242 is `e6 06 05 01 74 60 02`: bytes 246..248 are max_luma, max_fall, min_luma */
    edid_zero_cv[246] = 0x00;
    edid_zero_cv[247] = 0x00;
    edid_zero_cv[248] = 0x00;
    fix_edid_and_did2_checksums(edid_zero_cv);

    struct wlr_kfx_output_hdr_caps zero_caps = {0};
    g_assert_true(wlr_kfx_edid_parse_hdr_caps(edid_zero_cv, sizeof(edid_zero_cv), &zero_caps));
    g_assert_true(zero_caps.supported);
    g_assert_cmpfloat(zero_caps.max_cll_nits, ==, 0.0f);
    g_assert_cmpfloat(zero_caps.max_fall_nits, ==, 0.0f);
    g_assert_cmpfloat(zero_caps.min_luma_nits, ==, 0.0f);

    /* 2. Bad outer EDID checksum -> rejected cleanly */
    uint8_t bad_outer[256];
    memcpy(bad_outer, kHostAEdid, sizeof(bad_outer));
    bad_outer[255] ^= 0x01;
    struct wlr_kfx_output_hdr_caps bad_caps = {0};
    g_assert_false(wlr_kfx_edid_parse_hdr_caps(bad_outer, sizeof(bad_outer), &bad_caps));
    g_assert_false(bad_caps.supported);

    /* 3. Bad inner DisplayID 2.0 checksum (while fixing outer EDID checksum) -> HDR unsupported */
    uint8_t bad_inner[256];
    memcpy(bad_inner, kHostAEdid, sizeof(bad_inner));
    bad_inner[128 + 5 + bad_inner[130]] ^= 0x01;
    uint8_t sum_ext = 0;
    for (size_t i = 0; i < 127; i++)
        sum_ext = (uint8_t)(sum_ext + bad_inner[128 + i]);
    bad_inner[255] = (uint8_t)(-sum_ext);
    struct wlr_kfx_output_hdr_caps inner_caps = {0};
    g_assert_true(wlr_kfx_edid_parse_hdr_caps(bad_inner, sizeof(bad_inner), &inner_caps));
    g_assert_false(inner_caps.supported);

    /* 4. 128-byte base EDID with extension count = 0 -> SDR only */
    uint8_t sdr_only[128];
    memcpy(sdr_only, kHostAEdid, sizeof(sdr_only));
    sdr_only[126] = 0;
    uint8_t sb = 0;
    for (size_t i = 0; i < 127; i++)
        sb = (uint8_t)(sb + sdr_only[i]);
    sdr_only[127] = (uint8_t)(-sb);
    struct wlr_kfx_output_hdr_caps sdr_caps = {0};
    g_assert_true(wlr_kfx_edid_parse_hdr_caps(sdr_only, sizeof(sdr_only), &sdr_caps));
    g_assert_false(sdr_caps.supported);
}

typedef struct {
    struct wlr_buffer base;
    uint32_t         *pixels;
    size_t            stride;
} CpuPixelBuffer;

static void
cpu_buf_destroy(struct wlr_buffer *buffer)
{
    CpuPixelBuffer *cb = wl_container_of(buffer, cb, base);
    wlr_buffer_finish(buffer);
    free(cb->pixels);
    free(cb);
}

static bool
cpu_buf_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags,
                              void **data, uint32_t *format, size_t *stride)
{
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
    CpuPixelBuffer *cb = wl_container_of(buffer, cb, base);
    *data   = cb->pixels;
    *format = DRM_FORMAT_XRGB2101010;
    *stride = cb->stride;
    return true;
}

static void
cpu_buf_end_data_ptr_access(struct wlr_buffer *buffer)
{
    (void)buffer;
}

static const struct wlr_buffer_impl kCpuBufImpl = {
    .destroy               = cpu_buf_destroy,
    .begin_data_ptr_access = cpu_buf_begin_data_ptr_access,
    .end_data_ptr_access   = cpu_buf_end_data_ptr_access,
};

static struct wlr_buffer *
create_solid_xrgb2101010_buffer(int width, int height, uint32_t code10)
{
    uint32_t c = code10 & 0x3FFu;
    uint32_t pixel = (0x3u << 30) | (c << 20) | (c << 10) | c;
    CpuPixelBuffer *cb = calloc(1, sizeof(*cb));
    g_assert_nonnull(cb);
    cb->stride = (size_t)width * sizeof(uint32_t);
    cb->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    g_assert_nonnull(cb->pixels);
    for (int i = 0; i < width * height; i++)
        cb->pixels[i] = pixel;
    wlr_buffer_init(&cb->base, &kCpuBufImpl, width, height);
    return &cb->base;
}

static void
read_xrgb2101010_pixel(struct wlr_renderer *renderer, struct wlr_buffer *buf,
                       int x, int y, int out_rgb[static 3])
{
    uint32_t px = 0;
    struct wlr_texture *tex = wlr_texture_from_buffer(renderer, buf);
    g_assert_nonnull(tex);
    g_assert_true(wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
        .data    = &px,
        .format  = DRM_FORMAT_XRGB2101010,
        .stride  = sizeof(px),
        .src_box = { .x = x, .y = y, .width = 1, .height = 1 },
    }));
    wlr_texture_destroy(tex);
    out_rgb[0] = (int)((px >> 20) & 0x3FFu);
    out_rgb[1] = (int)((px >> 10) & 0x3FFu);
    out_rgb[2] = (int)(px & 0x3FFu);
}

static void
render_and_sample_hdr_frame(struct wlr_renderer *renderer,
                            struct wlr_scene_output *so,
                            struct wlr_swapchain *sc10,
                            const struct wlr_output_image_description *hdr_desc,
                            struct wlr_color_transform *color_transform,
                            int px_a[static 3], int px_b[static 3], int px_c[static 3])
{
    struct wlr_scene_output_state_options s_opts = {
        .swapchain       = sc10,
        .color_transform = color_transform,
    };
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_image_description(&st, hdr_desc);
    g_assert_true(wlr_scene_output_build_state(so, &st, &s_opts));
    g_assert_nonnull(st.buffer);

    read_xrgb2101010_pixel(renderer, st.buffer, 50, 80, px_a);
    read_xrgb2101010_pixel(renderer, st.buffer, 150, 80, px_b);
    read_xrgb2101010_pixel(renderer, st.buffer, 250, 80, px_c);

    wlr_output_state_finish(&st);
}

static void
test_vulkan_10bit_pq_readback(void)
{
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "vulkan", 1);

    struct wl_display *dpy = wl_display_create();
    g_assert_nonnull(dpy);
    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    g_assert_nonnull(backend);
    struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
    if (!renderer || !wlr_renderer_is_vk(renderer) ||
            !renderer->features.input_color_transform ||
            !renderer->features.output_color_transform) {
        g_test_skip("Headless Vulkan color-transform renderer unavailable");
        if (renderer) wlr_renderer_destroy(renderer);
        wlr_backend_destroy(backend);
        wl_display_destroy(dpy);
        return;
    }
    struct wlr_allocator *alloc = wlr_allocator_autocreate(backend, renderer);
    g_assert_nonnull(alloc);

    const struct wlr_drm_format_set *tex_fmts =
        wlr_renderer_get_texture_formats(renderer, alloc->buffer_caps);
    const struct wlr_drm_format *fmt_10 =
        wlr_drm_format_set_get(tex_fmts, DRM_FORMAT_XRGB2101010);
    if (!fmt_10) {
        g_test_skip("Vulkan XRGB2101010 format unavailable");
        wlr_allocator_destroy(alloc);
        wlr_renderer_destroy(renderer);
        wlr_backend_destroy(backend);
        wl_display_destroy(dpy);
        return;
    }

    struct wlr_swapchain *sc10 = wlr_swapchain_create(alloc, 320, 160, fmt_10);
    g_assert_nonnull(sc10);

    struct wlr_scene *scene = wlr_scene_create();
    g_assert_nonnull(scene);

    struct wlr_output *wo = wlr_headless_add_output(backend, 320, 160);
    g_assert_nonnull(wo);
    g_assert_true(wlr_output_init_render(wo, alloc, renderer));

    struct wlr_kfx_output_hdr_caps non_drm_caps = { .supported = true };
    g_assert_false(wlr_kfx_output_get_hdr_caps(wo, &non_drm_caps));
    g_assert_false(non_drm_caps.supported);

    struct wlr_output_state init_st;
    wlr_output_state_init(&init_st);
    wlr_output_state_set_enabled(&init_st, true);
    wlr_output_state_set_custom_mode(&init_st, 320, 160, 60000);
    g_assert_true(wlr_output_commit_state(wo, &init_st));
    wlr_output_state_finish(&init_st);
    g_assert_true(wlr_backend_start(backend));

    struct wlr_scene_output *so = wlr_scene_output_create(scene, wo);
    g_assert_nonnull(so);

    CompConfig *cfg = comp_config_new_defaults(0);
    cfg->fx.dither  = false;

    CompServer srv = {0};
    wl_list_init(&srv.outputs);
    wl_list_init(&srv.windows);
    wl_list_init(&srv.layer_surfaces);
    srv.display   = dpy;
    srv.backend   = backend;
    srv.renderer  = renderer;
    srv.allocator = alloc;
    srv.scene     = scene;
    srv.config    = cfg;

    CompOutput out = {0};
    out.server       = &srv;
    out.wlr_output   = wo;
    out.scene_output = so;
    wl_list_insert(&srv.outputs, &out.link);

    g_assert_true(comp_fx_init(&srv));
    comp_fx_config_apply(&srv, cfg);

    /* Patch A (x=0..100): SDR white (1.0) */
    const float c_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    struct wlr_scene_rect *patch_a = wlr_scene_rect_create(&scene->tree, 100, 160, c_white);
    wlr_scene_node_set_position(&patch_a->node, 0, 0);

    /* Patch B (x=100..200): 600 cd/m^2 PQ BT.2020 HDR surface */
    uint32_t in_600 = (uint32_t)lroundf(comp_fx_linear_to_pq(600.0f / 10000.0f) * 1023.0f);
    struct wlr_buffer *buf_b = create_solid_xrgb2101010_buffer(100, 160, in_600);
    struct wlr_scene_buffer *patch_b = wlr_scene_buffer_create(&scene->tree, buf_b);
    wlr_scene_node_set_position(&patch_b->node, 100, 0);
    wlr_scene_buffer_set_transfer_function(patch_b, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
    wlr_scene_buffer_set_primaries(patch_b, WLR_COLOR_NAMED_PRIMARIES_BT2020);
    wlr_buffer_drop(buf_b);

    /* Patch C (x=200..300): 4000 cd/m^2 PQ BT.2020 HDR surface */
    uint32_t in_4000 = (uint32_t)lroundf(comp_fx_linear_to_pq(4000.0f / 10000.0f) * 1023.0f);
    struct wlr_buffer *buf_c = create_solid_xrgb2101010_buffer(100, 160, in_4000);
    struct wlr_scene_buffer *patch_c = wlr_scene_buffer_create(&scene->tree, buf_c);
    wlr_scene_node_set_position(&patch_c->node, 200, 0);
    wlr_scene_buffer_set_transfer_function(patch_c, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
    wlr_scene_buffer_set_primaries(patch_c, WLR_COLOR_NAMED_PRIMARIES_BT2020);
    wlr_buffer_drop(buf_c);

    const struct wlr_output_image_description hdr_desc = {
        .primaries         = WLR_COLOR_NAMED_PRIMARIES_BT2020,
        .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
        .mastering_luminance = { .min = 0.000379, .max = 616.884 },
        .max_cll           = 617,
        .max_fall          = 400,
    };
    const struct wlr_output_image_description *orig_img_desc = wo->image_description;
    wo->image_description = &hdr_desc;

    const int exp_pq_100  = (int)lroundf(comp_fx_linear_to_pq(100.0f / 10000.0f) * 1023.0f);
    const int exp_pq_203  = (int)lroundf(comp_fx_linear_to_pq(203.0f / 10000.0f) * 1023.0f);
    const int exp_pq_400  = (int)lroundf(comp_fx_linear_to_pq(400.0f / 10000.0f) * 1023.0f);
    const int exp_pq_600  = (int)lroundf(comp_fx_linear_to_pq(600.0f / 10000.0f) * 1023.0f);
    const int exp_pq_peak = (int)lroundf(comp_fx_linear_to_pq(616.884f / 10000.0f) * 1023.0f);

    int px_a[3] = {0}, px_b[3] = {0}, px_c[3] = {0};

    /* 1. tonemap=false, sdr_white_nits=100, 203 (default 0), and 400:
     * Patch A scales with sdr_white_nits; Patch B stays invariant at ~717/1023 (no supernova!). */
    const double sdr_levels[] = { 100.0, 203.0, 400.0 };
    const int    exp_a_list[] = { exp_pq_100, exp_pq_203, exp_pq_400 };
    for (size_t i = 0; i < G_N_ELEMENTS(sdr_levels); i++) {
        cfg->fx.tonemap           = false;
        cfg->fx.tonemap_peak_nits = 0.0;
        cfg->fx.sdr_white_nits    = (i == 1) ? 0.0 : sdr_levels[i];
        comp_fx_config_apply(&srv, cfg);
        comp_fx_output_prepare(&out, (double)(i + 1));

        render_and_sample_hdr_frame(renderer, so, sc10, &hdr_desc, NULL, px_a, px_b, px_c);
        g_test_message("H3 sdr_white=%.0f no-tonemap: SDR_A=(%d,%d,%d) exp=%d | PQ600_B=(%d,%d,%d) exp=%d | PQ4000_C=(%d,%d,%d)",
                       sdr_levels[i], px_a[0], px_a[1], px_a[2], exp_a_list[i],
                       px_b[0], px_b[1], px_b[2], exp_pq_600,
                       px_c[0], px_c[1], px_c[2]);

        for (int ch = 0; ch < 3; ch++) {
            g_assert_cmpint(abs(px_a[ch] - exp_a_list[i]), <=, 2);
            g_assert_cmpint(abs(px_b[ch] - exp_pq_600), <=, 2);
        }
    }

    /* 2. Auto-peak BT.2390 tone mapping (tonemap=true, tonemap_peak_nits=0 -> uses 616.884 from hdr_desc) */
    cfg->fx.sdr_white_nits    = 0.0;
    cfg->fx.tonemap           = true;
    cfg->fx.tonemap_peak_nits = 0.0;
    comp_fx_config_apply(&srv, cfg);
    comp_fx_output_prepare(&out, 4.0);

    render_and_sample_hdr_frame(renderer, so, sc10, &hdr_desc, NULL, px_a, px_b, px_c);
    g_test_message("H3 auto-tonemap (peak 616.9): SDR_A=(%d,%d,%d) | PQ600_B=(%d,%d,%d) | PQ4000_C=(%d,%d,%d) max_pq=%d",
                   px_a[0], px_a[1], px_a[2], px_b[0], px_b[1], px_b[2],
                   px_c[0], px_c[1], px_c[2], exp_pq_peak);

    float headroom = wlr_kfx_output_get_hdr_headroom(wo);
    g_assert_cmpfloat_with_epsilon(headroom, 616.884f / 203.0f, 0.02f);
    for (int ch = 0; ch < 3; ch++) {
        g_assert_cmpint(abs(px_a[ch] - exp_pq_203), <=, 2);
        g_assert_cmpint(px_c[ch], <=, exp_pq_peak + 1);
        g_assert_cmpint(px_c[ch], >, 695);
    }

    /* 3. Unclamped Night Light (3000K) over HDR PQ highlights */
    cfg->fx.tonemap                 = false;
    cfg->fx.color_filter            = COMP_FX_FILTER_NIGHT_LIGHT;
    cfg->fx.night_light_temperature = 3000;
    comp_fx_config_apply(&srv, cfg);
    comp_fx_output_prepare(&out, 5.0);

    struct wlr_color_transform *nl_ct = comp_fx_output_get_color_transform(&out);
    g_assert_nonnull(nl_ct);
    render_and_sample_hdr_frame(renderer, so, sc10, &hdr_desc, nl_ct, px_a, px_b, px_c);
    g_test_message("H3 night-light 3000K: SDR_A=(%d,%d,%d) | PQ600_B=(%d,%d,%d)",
                   px_a[0], px_a[1], px_a[2], px_b[0], px_b[1], px_b[2]);
    g_assert_cmpint(px_b[0], >, px_b[1]);
    g_assert_cmpint(px_b[1], >, px_b[2]);
    g_assert_cmpint(px_b[0], >, 680);

    wo->image_description = orig_img_desc;
    wlr_swapchain_destroy(sc10);

    /* 4. Linear FP16 HDR screenshot capture -> comp_fx_tonemap_fp16_to_argb8888 */
    cfg->fx.color_filter   = COMP_FX_FILTER_NONE;
    cfg->fx.sdr_white_nits = 0.0;
    comp_fx_config_apply(&srv, cfg);
    wo->image_description = orig_img_desc;
    const struct wlr_drm_format_set *rfmts =
        (renderer->WLR_PRIVATE.impl && renderer->WLR_PRIVATE.impl->get_render_formats)
            ? renderer->WLR_PRIVATE.impl->get_render_formats(renderer)
            : NULL;
    const struct wlr_drm_format *fp16_fmt = rfmts ? wlr_drm_format_set_get(rfmts, DRM_FORMAT_ABGR16161616F) : NULL;
    if (fp16_fmt) {
        struct wlr_buffer *fp16_buf = wlr_allocator_create_buffer(alloc, 32, 32, fp16_fmt);
        if (fp16_buf) {
            struct wlr_render_pass *hpass = comp_fx_screenshot_begin_hdr_pass(&srv, fp16_buf);
            g_assert_nonnull(hpass);
            wlr_render_pass_add_rect(hpass, &(struct wlr_render_rect_options){
                .box = { 0, 0, 32, 32 },
                .color = { 0.0f, 0.0f, 0.0f, 1.0f },
                .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
            });
            wlr_render_pass_add_rect(hpass, &(struct wlr_render_rect_options){
                .box = { 0, 0, 10, 32 },
                .color = { 1.0f, 1.0f, 1.0f, 1.0f },
                .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
            });
            struct wlr_buffer *shot_b = create_solid_xrgb2101010_buffer(32, 32, in_600);
            struct wlr_buffer *shot_c = create_solid_xrgb2101010_buffer(32, 32, in_4000);
            struct wlr_texture *tex_b = wlr_texture_from_buffer(renderer, shot_b);
            struct wlr_texture *tex_c = wlr_texture_from_buffer(renderer, shot_c);
            wlr_buffer_drop(shot_b);
            wlr_buffer_drop(shot_c);
            g_assert_nonnull(tex_b);
            g_assert_nonnull(tex_c);
            struct wlr_color_primaries bt2020_prim;
            wlr_color_primaries_from_named(&bt2020_prim, WLR_COLOR_NAMED_PRIMARIES_BT2020);
            float pq_mult = 10000.0f / 203.0f;
            float op = 1.0f;
            wlr_render_pass_add_texture(hpass, &(struct wlr_render_texture_options){
                .texture              = tex_b,
                .dst_box              = { 10, 0, 11, 32 },
                .alpha                = &op,
                .blend_mode           = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
                .transfer_function    = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
                .primaries            = &bt2020_prim,
                .luminance_multiplier = &pq_mult,
            });
            wlr_render_pass_add_texture(hpass, &(struct wlr_render_texture_options){
                .texture              = tex_c,
                .dst_box              = { 21, 0, 11, 32 },
                .alpha                = &op,
                .blend_mode           = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
                .transfer_function    = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
                .primaries            = &bt2020_prim,
                .luminance_multiplier = &pq_mult,
            });
            g_assert_true(wlr_render_pass_submit(hpass));
            wlr_texture_destroy(tex_b);
            wlr_texture_destroy(tex_c);

            uint16_t fp16_raw[32 * 32 * 4];
            uint32_t argb_out[32 * 32];
            struct wlr_texture *htex = wlr_texture_from_buffer(renderer, fp16_buf);
            g_assert_nonnull(htex);
            bool ro_fp16 = wlr_texture_read_pixels(htex, &(struct wlr_texture_read_pixels_options){
                .data   = fp16_raw,
                .format = DRM_FORMAT_ABGR16161616F,
                .stride = 32 * 8,
            });
            g_assert_true(ro_fp16);
            comp_fx_tonemap_fp16_to_argb8888(fp16_raw, (uint8_t *)argb_out, 32, 32);
            int sdr_r  = (int)((argb_out[16 * 32 + 5] >> 16) & 0xffu);
            int pq6_r  = (int)((argb_out[16 * 32 + 15] >> 16) & 0xffu);
            int pq4k_r = (int)((argb_out[16 * 32 + 26] >> 16) & 0xffu);
            g_test_message("Stage1 GPU FP16 screenshot tonemap: SDR=%d PQ600=%d PQ4000=%d",
                           sdr_r, pq6_r, pq4k_r);
            g_assert_cmpint(sdr_r, >=, 235);
            g_assert_cmpint(pq6_r, >, sdr_r);
            g_assert_cmpint(pq4k_r, >, pq6_r);
            g_assert_cmpint(pq4k_r, ==, 255);
            wlr_texture_destroy(htex);
            wlr_buffer_drop(fp16_buf);
        }
    }

    comp_fx_finish(&srv);
    wl_list_remove(&out.link);
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_allocator_destroy(alloc);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(dpy);
    comp_config_free(cfg);
}

static void
test_hdr_clamp_invariants(void)
{
    CompFxHdrLuminance lum = {0};

    comp_fx_clamp_hdr_luminance(0.0f, 0.0f, 0.0f, 0, 0.0, &lum);
    g_assert_cmpfloat_with_epsilon(lum.max_cll_nits, 400.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.max_fall_nits, 400.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.min_luma_nits, 0.005f, 1e-6f);
    g_assert_cmpfloat_with_epsilon(lum.effective_peak_nits, 400.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.sdr_white_nits, 203.0f, 1e-3f);

    comp_fx_clamp_hdr_luminance(616.884f, 400.0f, 0.000379f, 0, 203.0, &lum);
    g_assert_cmpfloat_with_epsilon(lum.effective_peak_nits, 616.884f, 1e-2f);
    g_assert_cmpfloat_with_epsilon(lum.max_fall_nits, 400.0f, 1e-2f);
    g_assert_cmpfloat_with_epsilon(lum.min_luma_nits, 0.000379f, 1e-6f);
    g_assert_cmpfloat_with_epsilon(lum.sdr_white_nits, 203.0f, 1e-2f);

    comp_fx_clamp_hdr_luminance(616.884f, 900.0f, 0.00001f, 300, 450.0, &lum);
    g_assert_cmpfloat_with_epsilon(lum.effective_peak_nits, 300.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.max_fall_nits, 300.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.min_luma_nits, 0.0001f, 1e-6f);
    g_assert_cmpfloat_with_epsilon(lum.sdr_white_nits, 300.0f, 1e-3f);

    comp_fx_clamp_hdr_luminance(50000.0f, 20.0f, 9.0f, 0, 800.0, &lum);
    g_assert_cmpfloat_with_epsilon(lum.effective_peak_nits, 10000.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.max_fall_nits, 50.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.min_luma_nits, 5.0f, 1e-3f);
    g_assert_cmpfloat_with_epsilon(lum.sdr_white_nits, 500.0f, 1e-3f);
}

static uint16_t
f32_to_fp16(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xffu) - 127 + 15;
    uint32_t mant = (x >> 13) & 0x3ffu;
    if ((x & 0x7fffffffu) == 0)
        return (uint16_t)sign;
    if (exp <= 0)
        return (uint16_t)sign;
    if (exp >= 31)
        return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | mant);
}

static void
set_fp16_px(uint16_t *dst, float r, float g, float b, float a)
{
    dst[0] = f32_to_fp16(r);
    dst[1] = f32_to_fp16(g);
    dst[2] = f32_to_fp16(b);
    dst[3] = f32_to_fp16(a);
}

static void
test_screenshot_fp16_tonemap(void)
{
    /* Pure SDR pixels must round-trip without highlight compression */
    uint16_t sdr_in[4 * 4];
    uint32_t sdr_out[4] = {0};
    set_fp16_px(sdr_in + 0,  0.0f,  0.0f,  0.0f,  1.0f);
    set_fp16_px(sdr_in + 4,  0.25f, 0.25f, 0.25f, 1.0f);
    set_fp16_px(sdr_in + 8,  0.50f, 0.50f, 0.50f, 1.0f);
    set_fp16_px(sdr_in + 12, 1.0f,  1.0f,  1.0f,  1.0f);
    comp_fx_tonemap_fp16_to_argb8888(sdr_in, (uint8_t *)sdr_out, 4, 1);
    g_assert_cmpint((int)((sdr_out[0] >> 16) & 0xffu), ==, 0);
    g_assert_cmpint((int)((sdr_out[1] >> 16) & 0xffu), ==, 137);
    g_assert_cmpint((int)((sdr_out[2] >> 16) & 0xffu), ==, 188);
    g_assert_cmpint((int)((sdr_out[3] >> 16) & 0xffu), ==, 255);

    /* Mixed SDR + HDR: midtones intact, highlights ordered, hue preserved */
    uint16_t hdr_in[5 * 4];
    uint32_t hdr_out[5] = {0};
    set_fp16_px(hdr_in + 0,  0.25f, 0.25f, 0.25f, 1.0f);
    set_fp16_px(hdr_in + 4,  1.00f, 1.00f, 1.00f, 1.0f);
    set_fp16_px(hdr_in + 8,  600.0f / 203.0f, 600.0f / 203.0f, 600.0f / 203.0f, 1.0f);
    set_fp16_px(hdr_in + 12, 4000.0f / 203.0f, 4000.0f / 203.0f, 4000.0f / 203.0f, 1.0f);
    set_fp16_px(hdr_in + 16, 2.9557f, 1.4778f, 0.2956f, 1.0f);
    comp_fx_tonemap_fp16_to_argb8888(hdr_in, (uint8_t *)hdr_out, 5, 1);

    int mid  = (int)((hdr_out[0] >> 16) & 0xffu);
    int sdr  = (int)((hdr_out[1] >> 16) & 0xffu);
    int pq6  = (int)((hdr_out[2] >> 16) & 0xffu);
    int pq4k = (int)((hdr_out[3] >> 16) & 0xffu);
    int hue_r = (int)((hdr_out[4] >> 16) & 0xffu);
    int hue_g = (int)((hdr_out[4] >> 8) & 0xffu);
    int hue_b = (int)(hdr_out[4] & 0xffu);
    g_test_message("Stage1 CPU FP16 tonemap: mid=%d sdr=%d pq600=%d pq4000=%d hue=(%d,%d,%d)",
                   mid, sdr, pq6, pq4k, hue_r, hue_g, hue_b);
    g_assert_cmpint(mid, ==, 137);
    g_assert_cmpint(sdr, >=, 235);
    g_assert_cmpint(pq6, >, sdr);
    g_assert_cmpint(pq4k, >, pq6);
    g_assert_cmpint(pq4k, ==, 255);
    g_assert_cmpint(hue_r, >, hue_g);
    g_assert_cmpint(hue_g, >, hue_b);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/hdr/edid-did2-parser", test_edid_did2_parser);
    g_test_add_func("/comp/hdr/clamp-invariants", test_hdr_clamp_invariants);
    g_test_add_func("/comp/hdr/screenshot-fp16-tonemap", test_screenshot_fp16_tonemap);
    g_test_add_func("/comp/hdr/vulkan-10bit-pq-readback", test_vulkan_10bit_pq_readback);
    return g_test_run();
}
