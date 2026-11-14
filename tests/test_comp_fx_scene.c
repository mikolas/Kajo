/* test_comp_fx_scene.c — headless Vulkan scene integration tests for src/comp/fx/:
 * hit-test click-through (SH1), tiled/fullscreen disabling (SH4), damage/move
 * sync, focus spring lifecycle, renderer loss, and effects.enabled=false (V4). */
#include "../src/comp/comp_server.h"
#include "../src/comp/fx/comp_fx.h"

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

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/fx/scene-click-through-and-lifecycle",
                    test_scene_lifecycle_and_click_through);
    g_test_add_func("/comp/fx/pixman-fallback",
                    test_pixman_fallback);
    return g_test_run();
}
