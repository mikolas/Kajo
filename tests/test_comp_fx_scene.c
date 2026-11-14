/* test_comp_fx_scene.c — headless Vulkan scene integration tests for src/comp/fx/:
 * hit-test click-through (SH1), tiled/fullscreen disabling (SH4), damage/move
 * sync, focus spring lifecycle, renderer loss, and effects.enabled=false (V4). */
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
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

static float
fp16_to_f32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    if (exp == 0) {
        if (mant == 0)
            return 0.0f;
        float v = (float)mant / 1024.0f * (1.0f / 16384.0f);
        return sign ? -v : v;
    }
    if (exp == 31)
        return 0.0f;
    uint32_t bits = sign | ((exp + 112u) << 23) | (mant << 13);
    float out = 0.0f;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

double
comp_now_sec(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static size_t
count_enabled_children(const struct wlr_scene_tree *tree)
{
    size_t n = 0;
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
        if (node->enabled)
            n++;
    }
    return n;
}

static void
test_scene_lifecycle_and_click_through(void)
{
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "vulkan", 1);

    struct wl_display *dpy = wl_display_create();
    g_assert_nonnull(dpy);
    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    g_assert_nonnull(backend);
    struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
    if (!renderer || !wlr_renderer_is_vk(renderer)) {
        g_error("Headless Vulkan renderer required for comp-fx-scene (KAJO_REQUIRE_VULKAN)");
    }
    struct wlr_allocator *alloc = wlr_allocator_autocreate(backend, renderer);
    g_assert_nonnull(alloc);
    struct wlr_output *wo = wlr_headless_add_output(backend, 1280, 720);
    g_assert_nonnull(wo);
    g_assert_true(wlr_output_init_render(wo, alloc, renderer));
    struct wlr_output_state init_ost;
    wlr_output_state_init(&init_ost);
    wlr_output_state_set_enabled(&init_ost, true);
    wlr_output_state_set_custom_mode(&init_ost, 1280, 720, 0);
    g_assert_true(wlr_output_commit_state(wo, &init_ost));
    wlr_output_state_finish(&init_ost);
    g_assert_true(wlr_backend_start(backend));

    struct wlr_scene *scene = wlr_scene_create();
    g_assert_nonnull(scene);
    const enum wp_color_manager_v1_render_intent intents[] = {
        WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
    };
    const enum wp_color_manager_v1_transfer_function tfs[] = {
        WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
        WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
    };
    const enum wp_color_manager_v1_primaries prims[] = {
        WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
    };
    struct wlr_color_manager_v1 *cm = wlr_color_manager_v1_create(
        dpy, 1, &(struct wlr_color_manager_v1_options){
            .render_intents = intents,
            .render_intents_len = G_N_ELEMENTS(intents),
            .transfer_functions = tfs,
            .transfer_functions_len = G_N_ELEMENTS(tfs),
            .primaries = prims,
            .primaries_len = G_N_ELEMENTS(prims),
        });
    g_assert_nonnull(cm);
    wlr_scene_set_color_manager_v1(scene, cm);
    struct wlr_output_layout *layout = wlr_output_layout_create(dpy);
    struct wlr_scene_output_layout *scene_layout = wlr_scene_attach_output_layout(scene, layout);
    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(layout, wo);
    struct wlr_scene_output *so = wlr_scene_output_create(scene, wo);
    wlr_scene_output_layout_add_output(scene_layout, lo, so);

    CompConfig *cfg = comp_config_new_defaults(COMP_MOD_SUPER);
    g_assert_nonnull(cfg);

    CompServer srv = {
        .display      = dpy,
        .loop         = loop,
        .backend      = backend,
        .renderer     = renderer,
        .allocator    = alloc,
        .scene        = scene,
        .config       = cfg,
    };
    wl_list_init(&srv.outputs);

    CompOutput out = {
        .server       = &srv,
        .wlr_output   = wo,
        .scene_output = so,
    };
    wl_list_insert(&srv.outputs, &out.link);

    g_assert_true(comp_fx_init(&srv));
    bool has_kfx = comp_fx_is_active(&srv);
    comp_fx_config_apply(&srv, cfg);

    /* Lower full-screen target node (must receive hits through shadow halo). */
    const float bg_col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    struct wlr_scene_rect *lower_rect = wlr_scene_rect_create(&scene->tree, 1280, 720, bg_col);
    g_assert_nonnull(lower_rect);

    /* Floating window tree containing a window body + CompFxSurface. */
    struct wlr_scene_tree *win_tree = wlr_scene_tree_create(&scene->tree);
    g_assert_nonnull(win_tree);
    wlr_scene_node_set_position(&win_tree->node, 300, 200);
    const float win_col[4] = { 0.2f, 0.4f, 0.8f, 1.0f };
    struct wlr_scene_rect *win_rect = wlr_scene_rect_create(win_tree, 400, 300, win_col);
    g_assert_nonnull(win_rect);

    CompFxSurface *fx = comp_fx_surface_create(&srv, win_tree, COMP_FX_SURFACE_WINDOW);
    if (has_kfx)
        g_assert_nonnull(fx);

    CompFxSurfaceState st = {
        .outer_box     = { .x = 0, .y = 0, .width = 400, .height = 300 },
        .surface_box   = { .x = 0, .y = 0, .width = 400, .height = 300 },
        .shadow_below  = &win_rect->node,
        .dim_above     = &win_rect->node,
        .is_floating   = true,
        .is_focused    = true,
        .mapped        = true,
    };
    comp_fx_surface_sync(fx, &st);

    if (has_kfx) {
        /* Children in win_tree: win_rect + enabled shadow anchor (dim anchor disabled while focused). */
        g_assert_cmpuint(count_enabled_children(win_tree), ==, 2);
    }

    /* Commit a real headless Vulkan scene frame. */
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* SH1: click-through in shadow halo (x=270 is 30px left of window edge at x=300)
     * must resolve to lower_rect, never the shadow anchor node! */
    double sx = 0.0, sy = 0.0;
    struct wlr_scene_node *hit = wlr_scene_node_at(&scene->tree.node, 270.0, 320.0, &sx, &sy);
    g_assert_true(hit == &lower_rect->node);

    /* Hit inside window body hits win_rect. */
    hit = wlr_scene_node_at(&scene->tree.node, 450.0, 320.0, &sx, &sy);
    g_assert_true(hit == &win_rect->node);

    /* Unfocus with dim enabled activates dim anchor above win_rect too. */
    cfg->fx.dim.enabled = true;
    cfg->fx.dim.duration_ms = 0;
    cfg->fx.shadow.duration_ms = 0;
    comp_fx_config_apply(&srv, cfg);
    st.is_focused = false;
    comp_fx_surface_sync(fx, &st);
    if (has_kfx)
        g_assert_cmpuint(count_enabled_children(win_tree), ==, 3);
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* SH4: tiled unfocused with dim enabled keeps dim anchor (no shadow); focused tiled disables both. */
    st.is_floating = false;
    comp_fx_surface_sync(fx, &st);
    if (has_kfx)
        g_assert_cmpuint(count_enabled_children(win_tree), ==, 2);
    st.is_focused = true;
    comp_fx_surface_sync(fx, &st);
    g_assert_cmpuint(count_enabled_children(win_tree), ==, 1);

    /* Fullscreen disables both shadow and dim even when floating and unfocused. */
    st.is_floating = true;
    st.is_focused = false;
    st.is_fullscreen = true;
    comp_fx_surface_sync(fx, &st);
    g_assert_cmpuint(count_enabled_children(win_tree), ==, 1);
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* Explicit rule enables shadow on tiled window when not fullscreen (dim already active while unfocused). */
    st.is_floating = false;
    st.is_fullscreen = false;
    st.has_rule_shadow = true;
    st.rule_shadow = true;
    comp_fx_surface_sync(fx, &st);
    if (has_kfx)
        g_assert_cmpuint(count_enabled_children(win_tree), ==, 3);

    /* GB1 / GB3: Popup surface enables both shadow and glass by default;
     * idle frames do not recompute glass (DM1), and resizing reuses pooled targets (DM7). */
    struct wlr_scene_tree *pop_tree = wlr_scene_tree_create(&scene->tree);
    g_assert_nonnull(pop_tree);
    wlr_scene_node_set_position(&pop_tree->node, 200, 150);
    const float pop_col[4] = { 0.1f, 0.1f, 0.1f, 0.80f };
    struct wlr_scene_rect *pop_rect = wlr_scene_rect_create(pop_tree, 240, 160, pop_col);
    g_assert_nonnull(pop_rect);

    CompFxSurface *fx_pop = comp_fx_surface_create(&srv, pop_tree, COMP_FX_SURFACE_POPUP);
    CompFxSurfaceState pop_st = {
        .outer_box    = { .x = 0, .y = 0, .width = 240, .height = 160 },
        .surface_box  = { .x = 0, .y = 0, .width = 240, .height = 160 },
        .shadow_below = &pop_rect->node,
        .dim_above    = &pop_rect->node,
        .is_floating  = true,
        .is_focused   = true,
        .mapped       = true,
    };
    comp_fx_surface_sync(fx_pop, &pop_st);
    if (has_kfx)
        g_assert_cmpuint(count_enabled_children(pop_tree), ==, 3); /* shadow + glass + pop_rect */
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));
    if (has_kfx) {
        g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_pop), ==, 1);
        /* Idle frame with no scene damage: 0 new glass recomputes (GB3 / DM1). */
        comp_fx_output_prepare(&out, comp_now_sec());
        g_assert_true(wlr_scene_output_commit(so, NULL));
        g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_pop), ==, 1);
        /* Resize popup within bucket: recomputes without leaking targets (DM7). */
        wlr_scene_rect_set_size(pop_rect, 260, 180);
        pop_st.outer_box.width = 260;
        pop_st.outer_box.height = 180;
        pop_st.surface_box = pop_st.outer_box;
        comp_fx_surface_sync(fx_pop, &pop_st);
        comp_fx_output_prepare(&out, comp_now_sec());
        g_assert_true(wlr_scene_output_commit(so, NULL));
        g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_pop), ==, 2);
    }

    /* GB9 (DM3): Panel layer surface with xray = true caches background blur;
     * tiled window updates below the panel do NOT recompute panel blur,
     * while comp_fx_background_changed() DOES recompute panel blur. */
    srv.tree_tiled = win_tree;
    struct wlr_scene_tree *panel_tree = wlr_scene_tree_create(&scene->tree);
    g_assert_nonnull(panel_tree);
    wlr_scene_node_set_position(&panel_tree->node, 0, 0);
    struct wlr_scene_rect *panel_rect = wlr_scene_rect_create(panel_tree, 1280, 32, pop_col);
    CompFxSurface *fx_panel = comp_fx_surface_create(&srv, panel_tree, COMP_FX_SURFACE_LAYER);
    CompFxSurfaceState panel_st = {
        .outer_box     = { .x = 0, .y = 0, .width = 1280, .height = 32 },
        .surface_box   = { .x = 0, .y = 0, .width = 1280, .height = 32 },
        .shadow_below  = &panel_rect->node,
        .dim_above     = &panel_rect->node,
        .has_rule_blur = true,
        .rule_blur     = true,
        .has_rule_xray = true,
        .rule_xray     = true,
        .mapped        = true,
    };
    comp_fx_surface_sync(fx_panel, &panel_st);
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));
    if (has_kfx) {
        g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_panel), ==, 1);
        /* Move/damage tiled window right below panel (y=36 is within r_sample=16 of y=32):
         * xray panel must NOT recompute! */
        wlr_scene_node_set_position(&win_tree->node, 100, 36);
        const float win_col2[4] = { 0.9f, 0.2f, 0.2f, 1.0f };
        wlr_scene_rect_set_color(win_rect, win_col2);
        comp_fx_output_prepare(&out, comp_now_sec());
        g_assert_true(wlr_scene_output_commit(so, NULL));
        g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_panel), ==, 1);

        /* Wallpaper / background change DOES recompute xray panel glass! */
        comp_fx_background_changed(&out);
        comp_fx_output_prepare(&out, comp_now_sec());
        g_assert_true(wlr_scene_output_commit(so, NULL));
        g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_panel), ==, 2);
    }

    /* GB5: PQ HDR output image description commits cleanly with glass active,
     * and glass over a bright PQ highlight stays above SDR white (> 1.0) in the
     * linear FP16 blend image with identical tint contribution relative to SDR white. */
    struct wlr_output_image_description hdr_desc = {
        .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
        .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
    };
    const struct wlr_output_image_description *orig_img_desc = wo->image_description;
    wo->image_description = &hdr_desc;
    comp_fx_background_changed(&out);
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));
    if (has_kfx) {
        g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_panel), ==, 3);

        const struct wlr_drm_format_set *tex_fmts =
            wlr_renderer_get_texture_formats(renderer, alloc->buffer_caps);
        const struct wlr_drm_format *fmt_xrgb =
            wlr_drm_format_set_get(tex_fmts, DRM_FORMAT_XRGB8888);
        const struct wlr_drm_format *fmt_fp16 =
            wlr_drm_format_set_get(tex_fmts, DRM_FORMAT_ABGR16161616F);
        if (fmt_xrgb && fmt_fp16) {
            struct wlr_buffer *white_buf = wlr_allocator_create_buffer(alloc, 300, 300, fmt_xrgb);
            struct wlr_buffer *fp16_buf  = wlr_allocator_create_buffer(alloc, 1280, 720, fmt_fp16);
            g_assert_nonnull(white_buf);
            g_assert_nonnull(fp16_buf);

            struct wlr_render_pass *wpass = wlr_renderer_begin_buffer_pass(renderer, white_buf, NULL);
            g_assert_nonnull(wpass);
            wlr_render_pass_add_rect(wpass, &(struct wlr_render_rect_options){
                .box = { .x = 0, .y = 0, .width = 300, .height = 300 },
                .color = { 1.0f, 1.0f, 1.0f, 1.0f },
            });
            g_assert_true(wlr_render_pass_submit(wpass));

            struct wlr_scene_buffer *hdr_sbuf = wlr_scene_buffer_create(&scene->tree, white_buf);
            g_assert_nonnull(hdr_sbuf);
            wlr_scene_node_set_position(&hdr_sbuf->node, 180, 130);
            wlr_scene_node_place_below(&hdr_sbuf->node, &pop_tree->node);

            bool orig_dither = cfg->fx.dither;
            double orig_bright = cfg->fx.glass.brightness;
            cfg->fx.dither = false;
            cfg->fx.glass.brightness = 1.0;
            comp_fx_config_apply(&srv, cfg);

            /* 1. SDR (sRGB 1.0) backdrop under popup glass */
            wlr_scene_buffer_set_transfer_function(hdr_sbuf, WLR_COLOR_TRANSFER_FUNCTION_SRGB);
            wlr_scene_buffer_set_buffer(hdr_sbuf, white_buf);
            struct wlr_render_pass *cpass = comp_fx_screenshot_begin_pass(&srv, fp16_buf);
            g_assert_nonnull(cpass);
            struct wlr_box gbox = { .x = 200, .y = 150, .width = 260, .height = 180 };
            struct wlr_scene_node *child;
            wl_list_for_each(child, &pop_tree->children, link) {
                (void)comp_fx_screenshot_render_node(&srv, cpass, child, &gbox, &gbox,
                                                     0, 0, 1.0, 1280, 720);
            }
            g_assert_true(wlr_render_pass_submit(cpass));

            uint16_t sdr_px[4] = {0};
            struct wlr_texture *ftex = wlr_texture_from_buffer(renderer, fp16_buf);
            g_assert_nonnull(ftex);
            g_assert_true(wlr_texture_read_pixels(ftex, &(struct wlr_texture_read_pixels_options){
                .data   = sdr_px,
                .format = DRM_FORMAT_ABGR16161616F,
                .stride = sizeof(sdr_px),
                .src_box = { .x = 260, .y = 200, .width = 1, .height = 1 },
            }));
            wlr_texture_destroy(ftex);
            float sdr_lin = powf(fp16_to_f32(sdr_px[0]), 2.2f);
            g_assert_cmpfloat(sdr_lin, >, 0.5f);
            g_assert_cmpfloat(sdr_lin, <, 1.0f);

            /* 2. HDR (ST2084 PQ 1.0 = 10000/203 * SDR white) backdrop under popup glass */
            wlr_scene_buffer_set_transfer_function(hdr_sbuf, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
            wlr_scene_buffer_set_buffer(hdr_sbuf, white_buf);
            cpass = comp_fx_screenshot_begin_pass(&srv, fp16_buf);
            g_assert_nonnull(cpass);
            wl_list_for_each(child, &pop_tree->children, link) {
                (void)comp_fx_screenshot_render_node(&srv, cpass, child, &gbox, &gbox,
                                                     0, 0, 1.0, 1280, 720);
            }
            g_assert_true(wlr_render_pass_submit(cpass));

            uint16_t hdr_px[4] = {0};
            ftex = wlr_texture_from_buffer(renderer, fp16_buf);
            g_assert_nonnull(ftex);
            g_assert_true(wlr_texture_read_pixels(ftex, &(struct wlr_texture_read_pixels_options){
                .data   = hdr_px,
                .format = DRM_FORMAT_ABGR16161616F,
                .stride = sizeof(hdr_px),
                .src_box = { .x = 260, .y = 200, .width = 1, .height = 1 },
            }));
            wlr_texture_destroy(ftex);
            float hdr_lin = powf(fp16_to_f32(hdr_px[0]), 2.2f);
            /* Stays well above SDR white (1.0) in the blend image without clipping! */
            g_assert_cmpfloat(hdr_lin, >, 20.0f);
            /* And subtracting the attenuated HDR headroom excess leaves the exact same SDR tinted value! */
            float ta = (float)cfg->fx.glass.tint_alpha;
            float expected_excess = (1.0f - ta) * ((10000.0f / 203.0f) - 1.0f);
            g_assert_cmpfloat_with_epsilon(hdr_lin - expected_excess, sdr_lin, 0.05f);

            cfg->fx.dither = orig_dither;
            cfg->fx.glass.brightness = orig_bright;
            comp_fx_config_apply(&srv, cfg);

            wlr_scene_node_destroy(&hdr_sbuf->node);
            wlr_buffer_drop(fp16_buf);
            wlr_buffer_drop(white_buf);
        }
    }
    wo->image_description = orig_img_desc;
    comp_fx_background_changed(&out);
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* V4: [effects] enabled = false disables active FX cleanly. */
    cfg->fx.enabled = false;
    comp_fx_config_apply(&srv, cfg);
    g_assert_false(comp_fx_is_active(&srv));
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* Renderer lost clears compiled pipelines without leaking or crashing. */
    cfg->fx.enabled = true;
    comp_fx_config_apply(&srv, cfg);
    comp_fx_renderer_lost(&srv);
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* Teardown resilience: surface sync/destroy after comp_fx_finish must not UAF. */
    CompFxSurface *fx_late = comp_fx_surface_create(&srv, win_tree, COMP_FX_SURFACE_LAYER);
    if (fx_late)
        comp_fx_surface_sync(fx_late, &st);

    comp_fx_surface_destroy(fx_panel);
    comp_fx_surface_destroy(fx_pop);
    comp_fx_surface_destroy(fx);
    comp_fx_finish(&srv);
    if (fx_late) {
        st.mapped = false;
        comp_fx_surface_sync(fx_late, &st);
        comp_fx_surface_destroy(fx_late);
    }
    wl_list_remove(&out.link);

    wlr_scene_node_destroy(&scene->tree.node);
    wlr_allocator_destroy(alloc);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(dpy);
    comp_config_free(cfg);
}

static void
test_pixman_fallback(void)
{
    /* GB7: on non-Vulkan renderer (pixman), comp_fx_init succeeds, logs one
     * diagnostic, reports comp_fx_is_active == false, and creates no anchors. */
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    struct wl_display *dpy = wl_display_create();
    g_assert_nonnull(dpy);
    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    g_assert_nonnull(backend);
    struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
    g_assert_nonnull(renderer);
    g_assert_false(wlr_renderer_is_vk(renderer));
    struct wlr_allocator *alloc = wlr_allocator_autocreate(backend, renderer);
    g_assert_nonnull(alloc);
    struct wlr_scene *scene = wlr_scene_create();
    g_assert_nonnull(scene);

    CompConfig *cfg = comp_config_new_defaults(COMP_MOD_SUPER);
    g_assert_nonnull(cfg);

    CompServer srv = {
        .display   = dpy,
        .loop      = loop,
        .backend   = backend,
        .renderer  = renderer,
        .allocator = alloc,
        .scene     = scene,
        .config    = cfg,
    };
    wl_list_init(&srv.outputs);

    g_assert_true(comp_fx_init(&srv));
    g_assert_false(comp_fx_is_active(&srv));
    comp_fx_config_apply(&srv, cfg);

    struct wlr_scene_tree *pop_tree = wlr_scene_tree_create(&scene->tree);
    const float col[4] = { 0.2f, 0.2f, 0.2f, 0.8f };
    struct wlr_scene_rect *pop_rect = wlr_scene_rect_create(pop_tree, 200, 100, col);
    CompFxSurface *fx_pop = comp_fx_surface_create(&srv, pop_tree, COMP_FX_SURFACE_POPUP);
    CompFxSurfaceState pop_st = {
        .outer_box    = { .x = 0, .y = 0, .width = 200, .height = 100 },
        .surface_box  = { .x = 0, .y = 0, .width = 200, .height = 100 },
        .shadow_below = &pop_rect->node,
        .dim_above    = &pop_rect->node,
        .is_floating  = true,
        .is_focused   = true,
        .mapped       = true,
    };
    comp_fx_surface_sync(fx_pop, &pop_st);
    g_assert_cmpuint(count_enabled_children(pop_tree), ==, 1);

    comp_fx_surface_destroy(fx_pop);
    comp_fx_finish(&srv);
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_allocator_destroy(alloc);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(dpy);
    comp_config_free(cfg);
}

static void
test_m3_review_regressions(void)
{
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "vulkan", 1);

    struct wl_display *dpy = wl_display_create();
    g_assert_nonnull(dpy);
    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    g_assert_nonnull(backend);
    struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
    g_assert_nonnull(renderer);
    g_assert_true(wlr_renderer_is_vk(renderer));
    struct wlr_allocator *alloc = wlr_allocator_autocreate(backend, renderer);
    g_assert_nonnull(alloc);

    struct wlr_output *wo = wlr_headless_add_output(backend, 1280, 720);
    g_assert_nonnull(wo);
    g_assert_true(wlr_output_init_render(wo, alloc, renderer));
    struct wlr_output_state ost;
    wlr_output_state_init(&ost);
    wlr_output_state_set_enabled(&ost, true);
    wlr_output_state_set_custom_mode(&ost, 1280, 720, 0);
    g_assert_true(wlr_output_commit_state(wo, &ost));
    wlr_output_state_finish(&ost);
    g_assert_true(wlr_backend_start(backend));

    struct wlr_scene *scene = wlr_scene_create();
    g_assert_nonnull(scene);
    struct wlr_output_layout *layout = wlr_output_layout_create(dpy);
    struct wlr_scene_output_layout *scene_layout = wlr_scene_attach_output_layout(scene, layout);
    struct wlr_output_layout_output *lo = wlr_output_layout_add(layout, wo, 0, 0);
    struct wlr_scene_output *so = wlr_scene_output_create(scene, wo);
    wlr_scene_output_layout_add_output(scene_layout, lo, so);

    CompConfig *cfg = comp_config_new_defaults(COMP_MOD_SUPER);
    g_assert_nonnull(cfg);
    cfg->fx.dim.enabled = true;
    cfg->fx.dim.opacity = 0.65;
    cfg->fx.dim.duration_ms = 0;
    cfg->fx.shadow.duration_ms = 0;
    cfg->fx.glass.tint_alpha = 0.0;
    cfg->fx.glass.brightness = 1.0;
    cfg->fx.glass.saturation = 1.0;

    CompServer srv = {
        .display   = dpy,
        .loop      = loop,
        .backend   = backend,
        .renderer  = renderer,
        .allocator = alloc,
        .scene     = scene,
        .config    = cfg,
    };
    wl_list_init(&srv.outputs);

    CompOutput out = {
        .server       = &srv,
        .wlr_output   = wo,
        .scene_output = so,
    };
    wl_list_insert(&srv.outputs, &out.link);

    g_assert_true(comp_fx_init(&srv));
    g_assert_true(comp_fx_is_active(&srv));
    comp_fx_config_apply(&srv, cfg);

    /* White background + floating window + popup glass over it. */
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    struct wlr_scene_rect *bg_rect = wlr_scene_rect_create(&scene->tree, 1280, 720, white);
    g_assert_nonnull(bg_rect);

    struct wlr_scene_tree *win_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&win_tree->node, 200, 150);
    struct wlr_scene_rect *win_rect = wlr_scene_rect_create(win_tree, 300, 200, white);
    CompFxSurface *fx_win = comp_fx_surface_create(&srv, win_tree, COMP_FX_SURFACE_WINDOW);
    CompFxSurfaceState win_st = {
        .outer_box    = { .x = 0, .y = 0, .width = 300, .height = 200 },
        .surface_box  = { .x = 0, .y = 0, .width = 300, .height = 200 },
        .shadow_below = &win_rect->node,
        .dim_above    = &win_rect->node,
        .is_floating  = true,
        .is_focused   = true,
        .mapped       = true,
    };
    comp_fx_surface_sync(fx_win, &win_st);

    struct wlr_scene_tree *pop_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&pop_tree->node, 160, 150);
    const float pop_col[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    struct wlr_scene_rect *pop_rect = wlr_scene_rect_create(pop_tree, 240, 160, pop_col);
    CompFxSurface *fx_pop = comp_fx_surface_create(&srv, pop_tree, COMP_FX_SURFACE_POPUP);
    CompFxSurfaceState pop_st = {
        .outer_box      = { .x = 0, .y = 0, .width = 240, .height = 160 },
        .surface_box    = { .x = 0, .y = 0, .width = 240, .height = 160 },
        .shadow_below   = &pop_rect->node,
        .dim_above      = &pop_rect->node,
        .has_rule_shadow = true,
        .rule_shadow    = false,
        .is_floating    = true,
        .is_focused     = true,
        .mapped         = true,
    };
    comp_fx_surface_sync(fx_pop, &pop_st);

    const struct wlr_drm_format_set *tex_fmts =
        wlr_renderer_get_texture_formats(renderer, alloc->buffer_caps);
    const struct wlr_drm_format *fmt_xrgb =
        wlr_drm_format_set_get(tex_fmts, DRM_FORMAT_XRGB8888);
    g_assert_nonnull(fmt_xrgb);
    struct wlr_buffer *shot_buf = wlr_allocator_create_buffer(alloc, 1280, 720, fmt_xrgb);
    g_assert_nonnull(shot_buf);

    cfg->fx.shadow.layers[0].opacity = 0.85;
    cfg->fx.shadow.layers[1].opacity = 0.85;
    comp_fx_config_apply(&srv, cfg);

    /* Finding 1: Glass backdrop includes dim and shadow WLR_KFX_INSTEAD hooks. */
    comp_fx_output_prepare(&out, 10.0);
    g_assert_true(wlr_scene_output_commit(so, NULL));

    struct wlr_box gbox = { .x = 160, .y = 150, .width = 240, .height = 160 };
    struct wlr_render_pass *spass = comp_fx_screenshot_begin_pass(&srv, shot_buf);
    g_assert_nonnull(spass);
    struct wlr_scene_node *child;
    wl_list_for_each(child, &pop_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &gbox, &gbox,
                                             0, 0, 1.0, 1280, 720);
    }
    g_assert_true(wlr_render_pass_submit(spass));

    uint8_t px_focused[4] = {0};
    uint8_t px_halo_with_shadow[4] = {0};
    struct wlr_texture *stex = wlr_texture_from_buffer(renderer, shot_buf);
    g_assert_nonnull(stex);
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_focused,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 300, .y = 230, .width = 1, .height = 1 },
    }));
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_halo_with_shadow,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 194, .y = 230, .width = 1, .height = 1 },
    }));
    wlr_texture_destroy(stex);

    /* Step 1B: Disable shadow on focused win_tree (keep focused so dim is off) -> halo becomes pure white. */
    win_st.has_rule_shadow = true;
    win_st.rule_shadow = false;
    comp_fx_surface_sync(fx_win, &win_st);
    comp_fx_output_prepare(&out, 11.0);
    g_assert_true(wlr_scene_output_commit(so, NULL));

    spass = comp_fx_screenshot_begin_pass(&srv, shot_buf);
    g_assert_nonnull(spass);
    wl_list_for_each(child, &pop_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &gbox, &gbox,
                                             0, 0, 1.0, 1280, 720);
    }
    g_assert_true(wlr_render_pass_submit(spass));

    uint8_t px_halo_no_shadow[4] = {0};
    stex = wlr_texture_from_buffer(renderer, shot_buf);
    g_assert_nonnull(stex);
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_halo_no_shadow,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 194, .y = 230, .width = 1, .height = 1 },
    }));
    wlr_texture_destroy(stex);

    /* Step 1C: Unfocus win_tree -> dim overlay becomes active on win_tree; glass over (300, 230) must darken! */
    win_st.is_focused = false;
    comp_fx_surface_sync(fx_win, &win_st);
    comp_fx_output_prepare(&out, 12.0);
    g_assert_true(wlr_scene_output_commit(so, NULL));

    spass = comp_fx_screenshot_begin_pass(&srv, shot_buf);
    g_assert_nonnull(spass);
    wl_list_for_each(child, &pop_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &gbox, &gbox,
                                             0, 0, 1.0, 1280, 720);
    }
    g_assert_true(wlr_render_pass_submit(spass));

    uint8_t px_unfocused[4] = {0};
    stex = wlr_texture_from_buffer(renderer, shot_buf);
    g_assert_nonnull(stex);
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_unfocused,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 300, .y = 230, .width = 1, .height = 1 },
    }));
    wlr_texture_destroy(stex);

    /* Dimmed backdrop is significantly darker than focused white backdrop. */
    g_assert_cmpint(px_unfocused[0], <, (int)px_focused[0] - 80);
    /* Shadow halo at x=194 (6px left of win_tree at x=200) is darker when shadow is enabled. */
    g_assert_cmpint(px_halo_with_shadow[0], <, (int)px_halo_no_shadow[0] - 8);

    /* Finding 2: Multi-monitor glass_draw does not fall back to another output's target. */
    struct wlr_output *wo2 = wlr_headless_add_output(backend, 1280, 720);
    g_assert_nonnull(wo2);
    g_assert_true(wlr_output_init_render(wo2, alloc, renderer));
    wlr_output_state_init(&ost);
    wlr_output_state_set_enabled(&ost, true);
    wlr_output_state_set_custom_mode(&ost, 1280, 720, 0);
    g_assert_true(wlr_output_commit_state(wo2, &ost));
    wlr_output_state_finish(&ost);
    struct wlr_output_layout_output *lo2 = wlr_output_layout_add(layout, wo2, 1280, 0);
    struct wlr_scene_output *so2 = wlr_scene_output_create(scene, wo2);
    wlr_scene_output_layout_add_output(scene_layout, lo2, so2);
    CompOutput out2 = {
        .server       = &srv,
        .wlr_output   = wo2,
        .scene_output = so2,
    };
    wl_list_insert(&srv.outputs, &out2.link);

    /* Clear shot_buf to green, then attempt to render fx_pop's glass node as if on out2 (origin_lx = 1280).
     * Since pop_tree is at x=160 (only on out1, no valid target on out2), glass_draw must draw nothing! */
    spass = comp_fx_screenshot_begin_pass(&srv, shot_buf);
    g_assert_nonnull(spass);
    wlr_render_pass_add_rect(spass, &(struct wlr_render_rect_options){
        .box   = { .x = 0, .y = 0, .width = 1280, .height = 720 },
        .color = { 0.0f, 1.0f, 0.0f, 1.0f },
    });
    wl_list_for_each(child, &pop_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &gbox, &gbox,
                                             1280, 0, 1.0, 1280, 720);
    }
    g_assert_true(wlr_render_pass_submit(spass));

    uint8_t px_out2[4] = {0};
    stex = wlr_texture_from_buffer(renderer, shot_buf);
    g_assert_nonnull(stex);
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_out2,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 200, .y = 200, .width = 1, .height = 1 },
    }));
    wlr_texture_destroy(stex);
    /* Still pure green (R == 0, G == 255) because glass_draw did not fall back to out1's target! */
    g_assert_cmpint(px_out2[2], ==, 0);
    g_assert_cmpint(px_out2[1], ==, 255);

    /* Multi-monitor spanning screenshot (fx5 follow-up #5 & fx6 follow-up #4):
     * Place a red backdrop (x=1080..1280) on out1 (0..1280) and a blue backdrop (x=1280..1520)
     * on out2 (1280..2560). Test both separate popups on each output AND a single straddling
     * glass+shadow popup at x=1220..1340 crossing the boundary at x=1280 in a spanning
     * screenshot (origin_lx=1000, pass_w=600 -> x=1000..1600). */
    const float red_col[4]  = { 1.0f, 0.0f, 0.0f, 1.0f };
    const float blue_col[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    struct wlr_scene_rect *red_bg = wlr_scene_rect_create(&scene->tree, 200, 320, red_col);
    wlr_scene_node_set_position(&red_bg->node, 1080, 180);
    struct wlr_scene_rect *blue_bg = wlr_scene_rect_create(&scene->tree, 240, 320, blue_col);
    wlr_scene_node_set_position(&blue_bg->node, 1280, 180);

    struct wlr_scene_tree *span_pop1_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&span_pop1_tree->node, 1100, 200);
    struct wlr_scene_rect *span_pop1_rect = wlr_scene_rect_create(span_pop1_tree, 120, 100, pop_col);
    CompFxSurface *fx_span1 = comp_fx_surface_create(&srv, span_pop1_tree, COMP_FX_SURFACE_POPUP);
    CompFxSurfaceState span1_st = {
        .outer_box       = { .x = 0, .y = 0, .width = 120, .height = 100 },
        .surface_box     = { .x = 0, .y = 0, .width = 120, .height = 100 },
        .shadow_below    = &span_pop1_rect->node,
        .dim_above       = &span_pop1_rect->node,
        .has_rule_shadow = true,
        .rule_shadow     = false,
        .is_floating     = true,
        .is_focused      = true,
        .mapped          = true,
    };
    comp_fx_surface_sync(fx_span1, &span1_st);

    struct wlr_scene_tree *span_pop2_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&span_pop2_tree->node, 1340, 200);
    struct wlr_scene_rect *span_pop2_rect = wlr_scene_rect_create(span_pop2_tree, 120, 100, pop_col);
    CompFxSurface *fx_span2 = comp_fx_surface_create(&srv, span_pop2_tree, COMP_FX_SURFACE_POPUP);
    span1_st.shadow_below = &span_pop2_rect->node;
    span1_st.dim_above    = &span_pop2_rect->node;
    comp_fx_surface_sync(fx_span2, &span1_st);

    /* Single popup straddling the output boundary x=1280 (x=1220..1340, y=340..440) with shadow enabled! */
    struct wlr_scene_tree *span_straddle_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&span_straddle_tree->node, 1220, 340);
    struct wlr_scene_rect *span_straddle_rect = wlr_scene_rect_create(span_straddle_tree, 120, 100, pop_col);
    CompFxSurface *fx_straddle = comp_fx_surface_create(&srv, span_straddle_tree, COMP_FX_SURFACE_POPUP);
    CompFxSurfaceState straddle_st = span1_st;
    straddle_st.shadow_below = &span_straddle_rect->node;
    straddle_st.dim_above    = &span_straddle_rect->node;
    straddle_st.rule_shadow  = true;
    comp_fx_surface_sync(fx_straddle, &straddle_st);

    comp_fx_output_prepare(&out, 20.0);
    comp_fx_output_prepare(&out2, 20.0);
    g_assert_true(wlr_scene_output_commit(so, NULL));
    g_assert_true(wlr_scene_output_commit(so2, NULL));

    struct wlr_buffer *span_buf = wlr_allocator_create_buffer(alloc, 600, 720, fmt_xrgb);
    g_assert_nonnull(span_buf);
    spass = comp_fx_screenshot_begin_pass(&srv, span_buf);
    g_assert_nonnull(spass);
    wlr_render_pass_add_rect(spass, &(struct wlr_render_rect_options){
        .box   = { .x = 0, .y = 0, .width = 600, .height = 720 },
        .color = { 1.0f, 1.0f, 1.0f, 1.0f },
    });
    struct wlr_box span1_dst = { .x = 1100 - 1000, .y = 200, .width = 120, .height = 100 };
    struct wlr_box span2_dst = { .x = 1340 - 1000, .y = 200, .width = 120, .height = 100 };
    wl_list_for_each(child, &span_pop1_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &span1_dst, &span1_dst,
                                             1000, 0, 1.0, 600, 720);
    }
    wl_list_for_each(child, &span_pop2_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &span2_dst, &span2_dst,
                                             1000, 0, 1.0, 600, 720);
    }
    wl_list_for_each(child, &span_straddle_tree->children, link) {
        if (child->type != WLR_SCENE_NODE_BUFFER)
            continue;
        int nx = 0, ny = 0;
        wlr_scene_node_coords(child, &nx, &ny);
        struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(child);
        struct wlr_box cbox = {
            .x      = nx - 1000,
            .y      = ny,
            .width  = sbuf->dst_width,
            .height = sbuf->dst_height,
        };
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &cbox, &cbox,
                                             1000, 0, 1.0, 600, 720);
    }
    g_assert_true(wlr_render_pass_submit(spass));

    uint8_t px_span_out1[4] = {0};
    uint8_t px_span_out2[4] = {0};
    uint8_t px_straddle_left[4] = {0};
    uint8_t px_straddle_right[4] = {0};
    uint8_t px_straddle_shadow[4] = {0};
    stex = wlr_texture_from_buffer(renderer, span_buf);
    g_assert_nonnull(stex);
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_span_out1,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 160, .y = 250, .width = 1, .height = 1 },
    }));
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_span_out2,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 400, .y = 250, .width = 1, .height = 1 },
    }));
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_straddle_left,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 240, .y = 390, .width = 1, .height = 1 },
    }));
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_straddle_right,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 310, .y = 390, .width = 1, .height = 1 },
    }));
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_straddle_shadow,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 280, .y = 444, .width = 1, .height = 1 },
    }));
    wlr_texture_destroy(stex);
    wlr_buffer_drop(span_buf);
    /* XRGB8888 little-endian: [0]=B, [1]=G, [2]=R. out1 popup is red, out2 popup is blue. */
    g_assert_cmpint(px_span_out1[2], >, 200);
    g_assert_cmpint(px_span_out1[0], <, 20);
    g_assert_cmpint(px_span_out2[0], >, 200);
    g_assert_cmpint(px_span_out2[2], <, 20);
    /* Single straddling popup: left half (on out1) is red, right half (on out2) is blue,
     * and non-glass FX_SLOT_SHADOW darkens the white background below y=440. */
    g_assert_cmpint(px_straddle_left[2], >, 200);
    g_assert_cmpint(px_straddle_left[0], <, 20);
    g_assert_cmpint(px_straddle_right[0], >, 200);
    g_assert_cmpint(px_straddle_right[2], <, 20);
    g_assert_cmpint(px_straddle_shadow[0], <, 245);
    g_assert_cmpint(px_straddle_shadow[1], <, 245);
    g_assert_cmpint(px_straddle_shadow[2], <, 245);

    comp_fx_surface_destroy(fx_straddle);
    comp_fx_surface_destroy(fx_span2);
    comp_fx_surface_destroy(fx_span1);
    wlr_scene_node_destroy(&span_straddle_tree->node);
    wlr_scene_node_destroy(&span_pop2_tree->node);
    wlr_scene_node_destroy(&span_pop1_tree->node);
    wlr_scene_node_destroy(&blue_bg->node);
    wlr_scene_node_destroy(&red_bg->node);

    wl_list_remove(&out2.link);
    wlr_scene_output_destroy(so2);
    wlr_output_destroy(wo2);

    /* Nested glass-under-glass + scene-tree stacking order (fx5 follow-up #1 & #2):
     * Create pop_upper FIRST and pop_lower SECOND, then raise pop_upper above pop_lower
     * in the scene tree via wlr_scene_node_place_above. Place a black backdrop at x=500..700
     * with a bright green patch at x=580..680, y=220..320 under pop_lower (x=500, y=150, 200x200).
     * pop_upper (x=540, y=180, 140x140) sits over pop_lower. In pop_upper's render_below pass,
     * pop_lower's glass_draw must already be valid (prepared in stacking order, not creation order)
     * and must map its downscaled dst_box accurately so pop_upper sees green at (620, 260) and
     * dark black at (555, 195). */
    const float black_col[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float green_col[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    struct wlr_scene_rect *nest_black = wlr_scene_rect_create(&scene->tree, 240, 240, black_col);
    wlr_scene_node_set_position(&nest_black->node, 480, 130);
    struct wlr_scene_rect *nest_green = wlr_scene_rect_create(&scene->tree, 80, 80, green_col);
    wlr_scene_node_set_position(&nest_green->node, 600, 240);

    struct wlr_scene_tree *pop_upper_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&pop_upper_tree->node, 540, 180);
    struct wlr_scene_rect *pop_upper_rect = wlr_scene_rect_create(pop_upper_tree, 140, 140, pop_col);
    CompFxSurface *fx_upper = comp_fx_surface_create(&srv, pop_upper_tree, COMP_FX_SURFACE_POPUP);

    struct wlr_scene_tree *pop_lower_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&pop_lower_tree->node, 500, 150);
    struct wlr_scene_rect *pop_lower_rect = wlr_scene_rect_create(pop_lower_tree, 200, 200, pop_col);
    CompFxSurface *fx_lower = comp_fx_surface_create(&srv, pop_lower_tree, COMP_FX_SURFACE_POPUP);

    /* Raise pop_upper_tree above pop_lower_tree even though fx_upper was created first! */
    wlr_scene_node_place_above(&pop_upper_tree->node, &pop_lower_tree->node);

    CompFxSurfaceState lower_st = {
        .outer_box       = { .x = 0, .y = 0, .width = 200, .height = 200 },
        .surface_box     = { .x = 0, .y = 0, .width = 200, .height = 200 },
        .shadow_below    = &pop_lower_rect->node,
        .dim_above       = &pop_lower_rect->node,
        .has_rule_shadow = true,
        .rule_shadow     = false,
        .is_floating     = true,
        .is_focused      = true,
        .mapped          = true,
    };
    CompFxSurfaceState upper_st = {
        .outer_box       = { .x = 0, .y = 0, .width = 140, .height = 140 },
        .surface_box     = { .x = 0, .y = 0, .width = 140, .height = 140 },
        .shadow_below    = &pop_upper_rect->node,
        .dim_above       = &pop_upper_rect->node,
        .has_rule_shadow = true,
        .rule_shadow     = false,
        .is_floating     = true,
        .is_focused      = true,
        .mapped          = true,
    };
    comp_fx_surface_sync(fx_lower, &lower_st);
    comp_fx_surface_sync(fx_upper, &upper_st);

    /* Zero out nest_green before rendering pop_upper? Even better: in pop_upper's backdrop pass,
     * pop_lower's glass_draw overwrites nest_green with pop_lower's blurred target. To prove
     * pop_upper sampled pop_lower's glass_draw (and not nest_green directly), give glass a 50% blue tint!
     * Then 1 glass layer over black has B ~ 188, whereas 2 nested glass layers (pop_upper over pop_lower)
     * has B ~ 225, and over the green patch at (630, 270) pop_upper has both high G (from pop_lower's
     * properly positioned crop) and 2-layer blue tint! */
    cfg->fx.glass.tint_rgba[0] = 0.0f;
    cfg->fx.glass.tint_rgba[1] = 0.0f;
    cfg->fx.glass.tint_rgba[2] = 1.0f;
    cfg->fx.glass.tint_alpha   = 0.50;
    comp_fx_config_apply(&srv, cfg);

    /* Single prepare call must prepare fx_lower BEFORE fx_upper according to scene-tree order! */
    comp_fx_output_prepare(&out, 30.0);
    g_assert_true(wlr_scene_output_commit(so, NULL));

    struct wlr_box lower_box = { .x = 500, .y = 150, .width = 200, .height = 200 };
    struct wlr_box upper_box = { .x = 540, .y = 180, .width = 140, .height = 140 };
    spass = comp_fx_screenshot_begin_pass(&srv, shot_buf);
    g_assert_nonnull(spass);
    wl_list_for_each(child, &pop_lower_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &lower_box, &lower_box,
                                             0, 0, 1.0, 1280, 720);
    }
    wl_list_for_each(child, &pop_upper_tree->children, link) {
        (void)comp_fx_screenshot_render_node(&srv, spass, child, &upper_box, &upper_box,
                                             0, 0, 1.0, 1280, 720);
    }
    g_assert_true(wlr_render_pass_submit(spass));

    uint8_t px_1layer_black[4] = {0};
    uint8_t px_2layer_black[4] = {0};
    uint8_t px_2layer_green[4] = {0};
    stex = wlr_texture_from_buffer(renderer, shot_buf);
    g_assert_nonnull(stex);
    /* (515, 165) is inside pop_lower only (1 glass layer over black). */
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_1layer_black,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 515, .y = 165, .width = 1, .height = 1 },
    }));
    /* (550, 190) is inside pop_upper over pop_lower's black top-left region (2 glass layers over black). */
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_2layer_black,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 550, .y = 190, .width = 1, .height = 1 },
    }));
    /* (630, 270) is inside pop_upper over pop_lower's green patch (2 glass layers over green). */
    g_assert_true(wlr_texture_read_pixels(stex, &(struct wlr_texture_read_pixels_options){
        .data    = px_2layer_green,
        .format  = DRM_FORMAT_XRGB8888,
        .stride  = 4,
        .src_box = { .x = 630, .y = 270, .width = 1, .height = 1 },
    }));
    wlr_texture_destroy(stex);

    /* 2-layer glass blue channel [0] is significantly brighter than 1-layer glass blue channel [0]. */
    g_assert_cmpint(px_2layer_black[0], >, (int)px_1layer_black[0] + 20);
    /* Green channel [1] at (630, 270) is bright (over green patch), while at (550, 190) it is dark. */
    g_assert_cmpint(px_2layer_green[1], >, 100);
    g_assert_cmpint(px_2layer_black[1], <, 15);

    cfg->fx.glass.tint_alpha = 0.0;
    comp_fx_surface_destroy(fx_upper);
    comp_fx_surface_destroy(fx_lower);
    wlr_scene_node_destroy(&pop_upper_tree->node);
    wlr_scene_node_destroy(&pop_lower_tree->node);
    wlr_scene_node_destroy(&nest_green->node);
    wlr_scene_node_destroy(&nest_black->node);

    /* Finding 8: Throttled glass latches dirty_pending across a frame with no new damage. */
    cfg->fx.glass.max_recompute_hz = 10; /* min_dt = 0.10 s */
    comp_fx_config_apply(&srv, cfg);
    comp_fx_output_prepare(&out, 100.0);
    g_assert_true(wlr_scene_output_commit(so, NULL));
    uint64_t r0 = comp_fx_surface_get_glass_recomputes(fx_pop);

    /* Damage bg_rect under pop_tree, then prepare at t=100.02 (throttled!). */
    const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    wlr_scene_rect_set_color(bg_rect, grey);
    g_assert_true(comp_fx_output_prepare(&out, 100.02));
    g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_pop), ==, r0);
    /* Commit clears so->pending_commit_damage. */
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* Next frame at t=100.15 with NO new client damage: dirty_pending must still trigger recompute! */
    g_assert_false(comp_fx_output_prepare(&out, 100.15));
    g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_pop), ==, r0 + 1);
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* Finding 6: Fractional scale (1.75 on 2880x1800) right/bottom edge clamp does not truncate 1 px short. */
    wlr_output_state_init(&ost);
    wlr_output_state_set_custom_mode(&ost, 2880, 1800, 0);
    wlr_output_state_set_scale(&ost, 1.75f);
    g_assert_true(wlr_output_commit_state(wo, &ost));
    wlr_output_state_finish(&ost);

    /* At 2880x1800 @ 1.75, ceil(2880/1.75) = 1646, ceil(1800/1.75) = 1029.
     * Place pop_tree touching the bottom-right corner: (1446, 869) + (200, 160) = (1646, 1029). */
    wlr_scene_node_set_position(&pop_tree->node, 1446, 869);
    comp_fx_output_prepare(&out, 200.0);
    g_assert_true(wlr_scene_output_commit(so, NULL));
    g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_pop), ==, r0 + 2);

    /* fx6 follow-up #1: Disabling an ancestor scene tree (e.g. inactive workspace) while
     * surface remains mapped immediately releases go->target back to the pool so idle eviction
     * can free it, and re-enabling the tree recomputes the target cleanly. */
    g_assert_false(wl_list_empty(&fx_pop->glass_outputs));
    wlr_scene_node_set_enabled(&pop_tree->node, false);
    comp_fx_output_prepare(&out, 201.0);
    g_assert_true(wl_list_empty(&fx_pop->glass_outputs));
    wlr_scene_node_set_enabled(&pop_tree->node, true);
    comp_fx_output_prepare(&out, 202.0);
    g_assert_false(wl_list_empty(&fx_pop->glass_outputs));
    g_assert_cmpuint(comp_fx_surface_get_glass_recomputes(fx_pop), ==, r0 + 3);

    /* fx6 follow-up #3: wlr_kfx_renderer_run_target_chain pre-flight validation (un-transitioned
     * input, empty dst_box, self-dependency, bad params_len) and multi-step write-sample-write
     * reuse of the same targets in a single chain under VK_LAYER_KHRONOS_validation. */
    struct wlr_kfx_target *t_a      = wlr_kfx_target_acquire(renderer, 63, 63);
    struct wlr_kfx_target *t_b      = wlr_kfx_target_acquire(renderer, 61, 61);
    struct wlr_kfx_target *t_uninit = wlr_kfx_target_acquire(renderer, 59, 59);
    g_assert_nonnull(t_a);
    g_assert_nonnull(t_b);
    g_assert_nonnull(t_uninit);

    CompFxBlurParams chain_bp = {0};
    comp_fx_blur_pack((CompFxRect){ 0, 0, 61, 61 }, (CompFxRect){ 0, 0, 63, 63 },
                      63, 63, 1.5, &chain_bp);

    /* Pre-flight rejects un-transitioned input t_uninit. */
    struct wlr_kfx_target_draw_step bad_step = {
        .dst  = t_b,
        .draw = {
            .shader     = srv.fx->blur_down_shader,
            .dst_box    = { 0, 0, 61, 61 },
            .inputs     = { t_uninit, NULL, NULL, NULL },
            .params     = &chain_bp,
            .params_len = sizeof(chain_bp),
            .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        },
    };
    g_assert_false(wlr_kfx_renderer_run_target_chain(renderer, &bad_step, 1));

    /* Transition t_a via a target render pass. */
    struct wlr_render_pass *tpass = wlr_kfx_target_begin_pass(t_a);
    g_assert_nonnull(tpass);
    wlr_render_pass_add_rect(tpass, &(struct wlr_render_rect_options){
        .box   = { .x = 0, .y = 0, .width = 63, .height = 63 },
        .color = { 0.0f, 1.0f, 0.0f, 1.0f },
    });
    g_assert_true(wlr_render_pass_submit(tpass));

    /* Pre-flight rejects zero-width dst_box. */
    bad_step.draw.inputs[0]     = t_a;
    bad_step.draw.dst_box.width = 0;
    g_assert_false(wlr_kfx_renderer_run_target_chain(renderer, &bad_step, 1));

    /* Pre-flight rejects self-dependency (dst == inputs[0]). */
    bad_step.draw.dst_box.width = 61;
    bad_step.dst                = t_a;
    g_assert_false(wlr_kfx_renderer_run_target_chain(renderer, &bad_step, 1));

    /* Pre-flight rejects wrong params_len. */
    bad_step.dst             = t_b;
    bad_step.draw.params_len = 4;
    g_assert_false(wlr_kfx_renderer_run_target_chain(renderer, &bad_step, 1));

    /* Multi-step write-sample-write chain: t_a -> t_b -> t_a -> t_b in one command buffer! */
    struct wlr_kfx_target_draw_step chain3[3] = {
        {
            .dst  = t_b,
            .draw = {
                .shader     = srv.fx->blur_down_shader,
                .dst_box    = { 0, 0, 61, 61 },
                .inputs     = { t_a, NULL, NULL, NULL },
                .params     = &chain_bp,
                .params_len = sizeof(chain_bp),
                .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
            },
        },
        {
            .dst  = t_a,
            .draw = {
                .shader     = srv.fx->blur_up_shader,
                .dst_box    = { 0, 0, 63, 63 },
                .inputs     = { t_b, NULL, NULL, NULL },
                .params     = &chain_bp,
                .params_len = sizeof(chain_bp),
                .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
            },
        },
        {
            .dst  = t_b,
            .draw = {
                .shader     = srv.fx->blur_down_shader,
                .dst_box    = { 0, 0, 61, 61 },
                .inputs     = { t_a, NULL, NULL, NULL },
                .params     = &chain_bp,
                .params_len = sizeof(chain_bp),
                .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
            },
        },
    };
    g_assert_true(wlr_kfx_renderer_run_target_chain(renderer, chain3, 3));

    wlr_kfx_target_release(t_uninit);
    wlr_kfx_target_release(t_b);
    wlr_kfx_target_release(t_a);

    /* Finding 3: 16x16 blue-noise dither amplitude scales with bit depth (8-bit /255 vs 10-bit /1023). */
    const struct wlr_drm_format *fmt_10 =
        wlr_drm_format_set_get(tex_fmts, DRM_FORMAT_XRGB2101010);
    if (fmt_10) {
        struct wlr_buffer *buf8  = wlr_allocator_create_buffer(alloc, 16, 16, fmt_xrgb);
        struct wlr_buffer *buf10 = wlr_allocator_create_buffer(alloc, 16, 16, fmt_10);
        g_assert_nonnull(buf8);
        g_assert_nonnull(buf10);

        cfg->fx.dither = true;
        comp_fx_config_apply(&srv, cfg);

        struct wlr_render_pass *p8 = comp_fx_screenshot_begin_pass(&srv, buf8);
        g_assert_nonnull(p8);
        wlr_render_pass_add_rect(p8, &(struct wlr_render_rect_options){
            .box   = { .x = 0, .y = 0, .width = 16, .height = 16 },
            .color = { 0.501f, 0.501f, 0.501f, 1.0f },
        });
        g_assert_true(wlr_render_pass_submit(p8));

        struct wlr_render_pass *p10 = comp_fx_screenshot_begin_pass(&srv, buf10);
        g_assert_nonnull(p10);
        wlr_render_pass_add_rect(p10, &(struct wlr_render_rect_options){
            .box   = { .x = 0, .y = 0, .width = 16, .height = 16 },
            .color = { 0.501f, 0.501f, 0.501f, 1.0f },
        });
        g_assert_true(wlr_render_pass_submit(p10));

        uint32_t px8[16 * 16] = {0};
        struct wlr_texture *t8 = wlr_texture_from_buffer(renderer, buf8);
        g_assert_nonnull(t8);
        g_assert_true(wlr_texture_read_pixels(t8, &(struct wlr_texture_read_pixels_options){
            .data    = px8,
            .format  = DRM_FORMAT_XRGB8888,
            .stride  = 16 * sizeof(uint32_t),
            .src_box = { .x = 0, .y = 0, .width = 16, .height = 16 },
        }));
        wlr_texture_destroy(t8);

        uint32_t min8 = 255, max8 = 0;
        for (int i = 0; i < 256; i++) {
            uint32_t v = px8[i] & 0xFFu;
            if (v < min8) min8 = v;
            if (v > max8) max8 = v;
        }
        /* Dithering spans 2 adjacent 8-bit quantization levels (max8 - min8 == 1). */
        g_assert_cmpuint(max8 - min8, ==, 1);

        uint32_t px10[16 * 16] = {0};
        struct wlr_texture *t10 = wlr_texture_from_buffer(renderer, buf10);
        g_assert_nonnull(t10);
        g_assert_true(wlr_texture_read_pixels(t10, &(struct wlr_texture_read_pixels_options){
            .data    = px10,
            .format  = DRM_FORMAT_XRGB2101010,
            .stride  = 16 * sizeof(uint32_t),
            .src_box = { .x = 0, .y = 0, .width = 16, .height = 16 },
        }));
        wlr_texture_destroy(t10);

        uint32_t min10 = 1023, max10 = 0;
        for (int i = 0; i < 256; i++) {
            uint32_t v = px10[i] & 0x3FFu;
            if (v < min10) min10 = v;
            if (v > max10) max10 = v;
        }
        /* 10-bit dither amplitude is 1/1023, so peak-to-peak spread is at most 1 10-bit LSB (not 4). */
        g_assert_cmpuint(max10 - min10, <=, 1);
        g_assert_cmpuint(max10 - min10, >=, 1);

        /* GB6: with dither = false, output.frag adds zero noise (uniform input -> 1 exact output level). */
        cfg->fx.dither = false;
        comp_fx_config_apply(&srv, cfg);
        struct wlr_render_pass *p8_nodither = comp_fx_screenshot_begin_pass(&srv, buf8);
        g_assert_nonnull(p8_nodither);
        wlr_render_pass_add_rect(p8_nodither, &(struct wlr_render_rect_options){
            .box   = { .x = 0, .y = 0, .width = 16, .height = 16 },
            .color = { 0.501f, 0.501f, 0.501f, 1.0f },
        });
        g_assert_true(wlr_render_pass_submit(p8_nodither));

        t8 = wlr_texture_from_buffer(renderer, buf8);
        g_assert_nonnull(t8);
        g_assert_true(wlr_texture_read_pixels(t8, &(struct wlr_texture_read_pixels_options){
            .data    = px8,
            .format  = DRM_FORMAT_XRGB8888,
            .stride  = 16 * sizeof(uint32_t),
            .src_box = { .x = 0, .y = 0, .width = 16, .height = 16 },
        }));
        wlr_texture_destroy(t8);

        uint32_t min_nd = 255, max_nd = 0;
        for (int i = 0; i < 256; i++) {
            uint32_t v = px8[i] & 0xFFu;
            if (v < min_nd) min_nd = v;
            if (v > max_nd) max_nd = v;
        }
        g_assert_cmpuint(max_nd, ==, min_nd);

        wlr_buffer_drop(buf8);
        wlr_buffer_drop(buf10);
    }

    wlr_buffer_drop(shot_buf);
    comp_fx_surface_destroy(fx_pop);
    comp_fx_surface_destroy(fx_win);
    comp_fx_finish(&srv);
    wl_list_remove(&out.link);
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_allocator_destroy(alloc);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(dpy);
    comp_config_free(cfg);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/fx/scene-click-through-and-lifecycle",
                    test_scene_lifecycle_and_click_through);
    g_test_add_func("/comp/fx/m3-review-regressions",
                    test_m3_review_regressions);
    g_test_add_func("/comp/fx/pixman-fallback",
                    test_pixman_fallback);
    return g_test_run();
}
