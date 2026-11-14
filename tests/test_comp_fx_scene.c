/* test_comp_fx_scene.c — headless Vulkan scene integration tests for src/comp/fx/:
 * hit-test click-through (SH1), tiled/fullscreen disabling (SH4), damage/move
 * sync, focus spring lifecycle, renderer loss, and effects.enabled=false (V4). */
#include "../src/comp/comp_server.h"
#include "../src/comp/fx/comp_fx.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_color_management_v1.h>
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
#if defined(WLR_HAS_KFX) && WLR_HAS_KFX
    g_assert_true(comp_fx_is_active(&srv));
#endif
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
#if defined(WLR_HAS_KFX) && WLR_HAS_KFX
    g_assert_nonnull(fx);
#endif

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

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX
    /* Children in win_tree: win_rect + enabled shadow anchor (dim anchor disabled while focused). */
    g_assert_cmpuint(count_enabled_children(win_tree), ==, 2);
#endif

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
    comp_fx_config_apply(&srv, cfg);
    st.is_focused = false;
    comp_fx_surface_sync(fx, &st);
#if defined(WLR_HAS_KFX) && WLR_HAS_KFX
    g_assert_cmpuint(count_enabled_children(win_tree), ==, 3);
#endif
    comp_fx_output_prepare(&out, comp_now_sec());
    g_assert_true(wlr_scene_output_commit(so, NULL));

    /* SH4: tiled unfocused with dim enabled keeps dim anchor (no shadow); focused tiled disables both. */
    st.is_floating = false;
    comp_fx_surface_sync(fx, &st);
#if defined(WLR_HAS_KFX) && WLR_HAS_KFX
    g_assert_cmpuint(count_enabled_children(win_tree), ==, 2);
#endif
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
#if defined(WLR_HAS_KFX) && WLR_HAS_KFX
    g_assert_cmpuint(count_enabled_children(win_tree), ==, 3);
#endif

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

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/fx/scene-click-through-and-lifecycle",
                    test_scene_lifecycle_and_click_through);
    return g_test_run();
}
