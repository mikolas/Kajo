/* Unit tests for src/ide/ide_strip_layout.c (pure GTK-free Code Strip model). */

#include "../src/ide/ide_strip_layout.h"

#include <glib.h>
#include <math.h>
#include <string.h>

static IdeStripTile *
make_tile(const char *name, const char *rel_path, guint start_line, guint end_line, guint call_line)
{
  g_autofree char *abs_path = g_strdup_printf("/ws/%s", rel_path);
  return ide_strip_tile_new(NULL,
                            name,
                            abs_path,
                            rel_path,
                            start_line,
                            end_line,
                            call_line,
                            IDE_STRIP_TILE_FUNCTION);
}

static void
test_geometry_proportions_and_zen(void)
{
  IdeStripGeometry g;

  /* Depth 0: no Ancestor Spine, two 12px bridges, exact 60/25/15 split */
  ide_strip_compute_geometry(1280.0, 0, FALSE, &g);
  g_assert_cmpfloat_with_epsilon(g.spine_w, 0.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.bridge_w, 12.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.zen_rail_w, 0.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.d0_w, floor((1280.0 - 24.0) * 0.60), 1e-9);
  g_assert_cmpfloat_with_epsilon(g.d1_w, floor((1280.0 - 24.0) * 0.25), 1e-9);
  g_assert_cmpfloat_with_epsilon(g.spine_w + 2.0 * g.bridge_w + g.d0_w + g.d1_w + g.d2_w,
                                 1280.0,
                                 1e-9);

  /* Depth > 0: 28px Ancestor Spine included, zero rounding drift across odd widths */
  const double widths[] = { 1024.0, 1279.0, 1280.0, 1366.0, 1440.0, 1920.0, 2560.0, 3840.0 };
  for (gsize i = 0; i < G_N_ELEMENTS(widths); i++) {
    ide_strip_compute_geometry(widths[i], 2, FALSE, &g);
    g_assert_cmpfloat_with_epsilon(g.spine_w, 28.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(g.bridge_w, 12.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(g.zen_rail_w, 0.0, 1e-9);
    double total = g.spine_w + 2.0 * g.bridge_w + g.d0_w + g.d1_w + g.d2_w;
    g_assert_cmpfloat_with_epsilon(total, widths[i], 1e-9);
    g_assert_cmpfloat(g.d0_w, >, g.d1_w);
    g_assert_cmpfloat(g.d1_w, >, g.d2_w);
  }

  /* Zen mode: 96% D0 + 4% right rail (min 28px), spine and bridges hidden */
  ide_strip_compute_geometry(1920.0, 3, TRUE, &g);
  g_assert_cmpfloat_with_epsilon(g.spine_w, 0.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.bridge_w, 0.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.d1_w, 0.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.d2_w, 0.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.zen_rail_w, floor(1920.0 * 0.04), 1e-9);
  g_assert_cmpfloat_with_epsilon(g.d0_w + g.zen_rail_w, 1920.0, 1e-9);

  /* Narrow Zen viewport enforces minimum 28px rail and 200px D0 */
  ide_strip_compute_geometry(400.0, 1, TRUE, &g);
  g_assert_cmpfloat_with_epsilon(g.zen_rail_w, 28.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.d0_w, 372.0, 1e-9);
  ide_strip_compute_geometry(100.0, 1, TRUE, &g);
  g_assert_cmpfloat_with_epsilon(g.zen_rail_w, 28.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(g.d0_w, 200.0, 1e-9);

  /* Lens and badge helpers */
  g_assert_cmpstr(ide_strip_lens_name(IDE_STRIP_LENS_CALLS), ==, "CALLS");
  g_assert_cmpstr(ide_strip_lens_name(IDE_STRIP_LENS_TYPES), ==, "TYPES");
  g_assert_cmpstr(ide_strip_lens_name(IDE_STRIP_LENS_IMPACT), ==, "IMPACT");
  g_assert_cmpstr(ide_strip_lens_name(IDE_STRIP_LENS_DIFF), ==, "DIFF");
  g_assert_cmpstr(ide_strip_tile_kind_badge(IDE_STRIP_TILE_FUNCTION), ==, "ƒ");
  g_assert_cmpstr(ide_strip_tile_kind_badge(IDE_STRIP_TILE_TYPE), ==, "T");
  g_assert_cmpstr(ide_strip_tile_kind_badge(IDE_STRIP_TILE_FILE), ==, "F");
  g_assert_cmpstr(ide_strip_tile_kind_badge(IDE_STRIP_TILE_MACRO), ==, "M");
}

static void
test_depth_promote_and_surface(void)
{
  IdeStripModel *m = ide_strip_model_new();
  IdeStripTrail *t = ide_strip_model_get_active_trail(m);
  g_assert_nonnull(t);

  ide_strip_trail_set_root(t, make_tile("comp_output_frame", "src/comp/output.c", 10, 180, 0), TRUE);
  t->show_full_file = TRUE;

  GPtrArray *d1 = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  g_ptr_array_add(d1, make_tile("fx_glass_render", "src/comp/fx_glass.c", 210, 268, 84));
  g_ptr_array_add(d1, make_tile("fx_shadow_render", "src/comp/fx_shadow.c", 40, 110, 112));
  ide_strip_trail_set_d1_tiles(t, d1, 7, FALSE);
  t->active_d1_idx = 0;

  GPtrArray *d2 = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  g_ptr_array_add(d2, make_tile("blur_down_pass", "src/comp/fx_blur.c", 15, 72, 224));
  g_ptr_array_add(d2, make_tile("blur_up_pass", "src/comp/fx_blur.c", 80, 130, 240));
  ide_strip_trail_set_d2_tiles(t, d2);

  /* Promote D+1[0] -> D0 */
  t->d1_scroll_y = 68.0;
  ide_strip_trail_set_filter(t, "glass");
  g_assert_true(ide_strip_trail_promote_d1(t, 0, 84, 1.0));
  g_assert_cmpuint(t->ancestors->len, ==, 1);
  g_assert_cmpstr(t->root_tile->name, ==, "fx_glass_render");
  g_assert_false(t->show_full_file);
  g_assert_false(t->d1_unlinked);
  g_assert_null(t->d1_filter);
  g_assert_true(t->depth_slide_animating);
  g_assert_cmpfloat_with_epsilon(t->depth_slide_spring.from, 1.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(t->depth_slide_spring.to, 0.0, 1e-9);

  /* Previous D+2 slid left into D+1 because we promoted the active D+1 tile */
  g_assert_cmpuint(t->d1_tiles->len, ==, 2);
  g_assert_cmpstr(((IdeStripTile *)g_ptr_array_index(t->d1_tiles, 0))->name, ==, "blur_down_pass");
  g_assert_cmpint(t->active_d1_idx, ==, 0);

  /* Populate D+2 under blur_down_pass and do a direct 2x jump D+2 -> D0 */
  GPtrArray *d3 = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  g_ptr_array_add(d3, make_tile("kfx_dispatch_shader", "src/comp/fx_gl.c", 300, 360, 52));
  ide_strip_trail_set_d2_tiles(t, d3);

  g_assert_true(ide_strip_trail_promote_d2(t, 0, 224, 2.0));
  g_assert_cmpuint(t->ancestors->len, ==, 3);
  g_assert_cmpstr(((IdeStripAncestor *)g_ptr_array_index(t->ancestors, 0))->tile->name, ==, "comp_output_frame");
  g_assert_cmpstr(((IdeStripAncestor *)g_ptr_array_index(t->ancestors, 1))->tile->name, ==, "fx_glass_render");
  g_assert_cmpstr(((IdeStripAncestor *)g_ptr_array_index(t->ancestors, 2))->tile->name, ==, "blur_down_pass");
  g_assert_cmpstr(t->root_tile->name, ==, "kfx_dispatch_shader");
  g_assert_false(t->show_full_file);

  /* Surface back 1 depth: blur_down_pass */
  g_assert_true(ide_strip_trail_surface_one(t, 3.0));
  g_assert_cmpuint(t->ancestors->len, ==, 2);
  g_assert_cmpstr(t->root_tile->name, ==, "blur_down_pass");
  g_assert_true(t->depth_slide_animating);
  g_assert_cmpfloat_with_epsilon(t->depth_slide_spring.from, -1.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(t->depth_slide_spring.to, 0.0, 1e-9);

  /* Surface directly to ancestor 0: comp_output_frame (restores full_file & scroll_y) */
  g_assert_true(ide_strip_trail_surface_to(t, 0, 4.0));
  g_assert_cmpuint(t->ancestors->len, ==, 0);
  g_assert_cmpstr(t->root_tile->name, ==, "comp_output_frame");
  g_assert_true(t->show_full_file);
  g_assert_cmpfloat_with_epsilon(t->d1_scroll_y, 68.0, 1e-9);

  /* Surfacing past empty spine returns FALSE cleanly */
  g_assert_false(ide_strip_trail_surface_one(t, 5.0));
  g_assert_false(ide_strip_trail_surface_to(t, 0, 5.0));

  /* Out-of-bounds promote returns FALSE */
  g_assert_false(ide_strip_trail_promote_d1(t, 99, 10, 5.0));
  g_assert_false(ide_strip_trail_promote_d2(t, 99, 10, 5.0));

  ide_strip_model_free(m);
}

static void
test_magnetic_follow_unlink_resync_and_springs(void)
{
  IdeStripModel *m = ide_strip_model_new();
  IdeStripTrail *t = ide_strip_model_get_active_trail(m);
  ide_strip_trail_set_root(t, make_tile("dispatcher", "src/dispatcher.c", 1, 500, 0), TRUE);

  GPtrArray *d1 = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  for (guint i = 0; i < 30; i++) {
    g_autofree char *name = g_strdup_printf("step_%02u", i);
    g_ptr_array_add(d1, make_tile(name, "src/steps.c", i * 10 + 1, i * 10 + 9, (i + 1) * 10));
  }
  ide_strip_trail_set_d1_tiles(t, d1, 0, FALSE);
  g_assert_cmpint(t->active_d1_idx, ==, 0);

  /* Move cursor to line 155 (between call_line 150 [idx 14] and 160 [idx 15]).
   * Equal distance (5): tie-breaker must prefer call_line <= cursor_line (idx 14). */
  gboolean changed = ide_strip_trail_update_viewport_band(t, 120, 200, 155, 180.0, 400.0, TRUE, 10.0);
  g_assert_true(changed);
  g_assert_cmpint(t->active_d1_idx, ==, 14);
  g_assert_true(t->d1_scroll_animating);

  /* Viewport band flags set accurately for [120..200] (call_lines 120..200 => idx 11..19) */
  for (guint i = 0; i < 30; i++) {
    const IdeStripTile *tile = g_ptr_array_index(t->d1_tiles, i);
    gboolean expected = (tile->call_line >= 120 && tile->call_line <= 200);
    g_assert_true(tile->in_viewport_band == expected);
  }

  /* Expected scroll target: 14 * 34 - 180 + 16 = 312.0 */
  g_assert_cmpfloat_with_epsilon(t->d1_target_scroll_y, 312.0, 1e-6);

  /* Step springs mid-flight, retarget, then step until settled -> returns FALSE */
  g_assert_true(ide_strip_trail_step_springs(t, 10.05));
  g_assert_cmpfloat(t->d1_scroll_y, >, 0.0);
  g_assert_cmpfloat(t->d1_scroll_y, <, 312.0);

  /* Mid-flight retarget to idx 24 (call_line 250) */
  double pos_before = t->d1_scroll_y;
  g_assert_true(ide_strip_trail_update_viewport_band(t, 210, 290, 250, 120.0, 400.0, TRUE, 10.05));
  g_assert_cmpint(t->active_d1_idx, ==, 24);
  /* Position must stay continuous right at retarget instant */
  g_assert_cmpfloat_with_epsilon(comp_spring_value(&t->d1_scroll_spring, 10.05), pos_before, 1e-6);

  double now = 10.05;
  gboolean animating = TRUE;
  for (int frame = 0; frame < 300 && animating; frame++) {
    now += 1.0 / 60.0;
    animating = ide_strip_trail_step_springs(t, now);
  }
  g_assert_false(animating);
  g_assert_false(t->d1_scroll_animating);
  g_assert_cmpfloat_with_epsilon(t->d1_scroll_y, t->d1_target_scroll_y, 1e-6);

  /* Manual scroll unlinks magnetic follow and must not be fought by cursor moves */
  ide_strip_trail_manual_scroll_d1(t, 42.0);
  g_assert_true(t->d1_unlinked);
  g_assert_false(t->d1_scroll_animating);
  g_assert_cmpfloat_with_epsilon(t->d1_scroll_y, 42.0, 1e-9);

  g_assert_false(ide_strip_trail_update_viewport_band(t, 10, 80, 30, 100.0, 400.0, TRUE, now + 0.1));
  g_assert_cmpint(t->active_d1_idx, ==, 24);
  g_assert_cmpfloat_with_epsilon(t->d1_scroll_y, 42.0, 1e-9);
  /* Band flags still update even when unlinked */
  g_assert_true(((IdeStripTile *)g_ptr_array_index(t->d1_tiles, 2))->in_viewport_band);
  g_assert_false(((IdeStripTile *)g_ptr_array_index(t->d1_tiles, 24))->in_viewport_band);

  /* Resync re-links magnetic follow and selects call_line 30 (idx 2) */
  g_assert_true(ide_strip_trail_resync_d1(t, 10, 80, 30, 100.0, 400.0, now + 0.2));
  g_assert_false(t->d1_unlinked);
  g_assert_cmpint(t->active_d1_idx, ==, 2);
  g_assert_true(t->d1_scroll_animating);

  /* Settle resync spring */
  double t2 = now + 0.2;
  while (ide_strip_trail_step_springs(t, t2)) {
    t2 += 1.0 / 60.0;
    g_assert_cmpfloat(t2, <, now + 5.0);
  }
  g_assert_false(t->d1_scroll_animating);
  g_assert_cmpfloat_with_epsilon(t->d1_scroll_y, t->d1_target_scroll_y, 1e-6);

  ide_strip_model_free(m);
}

static void
test_dense_1000_tiles_magnetic_and_filter(void)
{
  IdeStripModel *m = ide_strip_model_new();
  IdeStripTrail *t = ide_strip_model_get_active_trail(m);
  ide_strip_trail_set_root(t, make_tile("mega_pipeline", "src/pipeline.c", 1, 5000, 0), TRUE);

  GPtrArray *d1 = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  guint expected_blur_matches = 0;
  for (guint i = 0; i < 1000; i++) {
    g_autofree char *name = NULL;
    g_autofree char *rel = NULL;
    if (i % 50 == 0) {
      name = g_strdup_printf("Fx_Blur_Pass_%04u", i);
      rel = g_strdup("src/comp/fx_pass.c");
      expected_blur_matches++;
    } else if (i % 75 == 0) {
      name = g_strdup_printf("shader_stage_%04u", i);
      rel = g_strdup("src/comp/blur_kernels.c");
      expected_blur_matches++;
    } else {
      name = g_strdup_printf("worker_fn_%04u", i);
      rel = g_strdup("src/core/workers.c");
    }
    g_ptr_array_add(d1, make_tile(name, rel, 10, 40, i * 4 + 10));
  }
  ide_strip_trail_set_d1_tiles(t, d1, 42, TRUE);
  g_assert_cmpuint(t->d1_tiles->len, ==, 1000);
  g_assert_cmpuint(t->ext_suppressed_count, ==, 42);
  g_assert_true(t->using_fallback);

  /* Magnetic follow deep into the 1,000-tile column (idx 750 -> call_line 3010) */
  gint64 t0 = g_get_monotonic_time();
  g_assert_true(ide_strip_trail_update_viewport_band(t, 2980, 3060, 3010, 240.0, 720.0, TRUE, 1.0));
  g_assert_cmpint(t->active_d1_idx, ==, 750);

  double expected_y = 750.0 * IDE_STRIP_D1_TILE_STRIDE - 240.0 + 16.0;
  g_assert_cmpfloat_with_epsilon(t->d1_target_scroll_y, expected_y, 1e-6);

  /* Step until settled */
  double sim = 1.0;
  while (ide_strip_trail_step_springs(t, sim)) {
    sim += 1.0 / 60.0;
    g_assert_cmpfloat(sim, <, 5.0);
  }
  g_assert_cmpfloat_with_epsilon(t->d1_scroll_y, expected_y, 1e-6);

  /* Inline case-insensitive filter ("blur") matches both name and rel_path */
  ide_strip_trail_set_filter(t, "blur");
  GArray *matched = ide_strip_trail_filtered_d1_indices(t);
  g_assert_cmpuint(matched->len, ==, expected_blur_matches);
  for (guint k = 0; k < matched->len; k++) {
    guint idx = g_array_index(matched, guint, k);
    g_assert_cmpuint(idx, <, 1000);
    g_assert_true((idx % 50 == 0) || (idx % 75 == 0));
  }
  g_array_unref(matched);

  /* Clearing filter restores all 1,000 indices */
  ide_strip_trail_set_filter(t, "");
  g_assert_null(t->d1_filter);
  GArray *all = ide_strip_trail_filtered_d1_indices(t);
  g_assert_cmpuint(all->len, ==, 1000);
  g_array_unref(all);

  /* Performance sanity check: 1,000-tile follow + filter should be well under 50 ms */
  gint64 elapsed_us = g_get_monotonic_time() - t0;
  g_assert_cmpint(elapsed_us, <, 50000);

  ide_strip_model_free(m);
}

static void
test_trails_branch_switch_close_and_pin(void)
{
  IdeStripModel *m = ide_strip_model_new();
  g_assert_false(m->strip_active);
  g_assert_cmpuint(m->trails->len, ==, 1);

  IdeStripTrail *t0 = ide_strip_model_get_active_trail(m);
  ide_strip_trail_set_root(t0, make_tile("render_frame", "src/render.c", 1, 90, 0), TRUE);
  g_assert_cmpstr(t0->title, ==, "render_frame");

  /* Switching with 1 trail returns FALSE */
  g_assert_false(ide_strip_model_switch_trail(m, 1));

  /* Pin and unpin a reference tile in D0 (including self-pin aliasing safety) */
  IdeStripTile *ref = ide_strip_tile_new(NULL, "RenderCtx", "/ws/src/render.h", "src/render.h",
                                         12, 45, 0, IDE_STRIP_TILE_TYPE);
  ide_strip_trail_pin_tile(t0, ref);
  g_assert_nonnull(t0->pinned_tile);
  g_assert_cmpstr(t0->pinned_tile->name, ==, "RenderCtx");
  g_assert_true(t0->pinned_tile != ref);
  ide_strip_trail_pin_tile(t0, t0->pinned_tile);
  g_assert_cmpstr(t0->pinned_tile->name, ==, "RenderCtx");
  ide_strip_trail_pin_tile(t0, NULL);
  g_assert_null(t0->pinned_tile);
  ide_strip_tile_free(ref);

  /* Branch active trail -> deep copy of root_tile, switches active index */
  t0->lens = IDE_STRIP_LENS_IMPACT;
  IdeStripTrail *t1 = ide_strip_model_branch_trail(m);
  g_assert_nonnull(t1);
  g_assert_cmpuint(m->trails->len, ==, 2);
  g_assert_cmpuint(m->active_trail_idx, ==, 1);
  g_assert_true(t1->root_tile != t0->root_tile);
  g_assert_cmpstr(t1->root_tile->name, ==, "render_frame");
  g_assert_cmpint(t1->lens, ==, IDE_STRIP_LENS_IMPACT);

  /* Add third trail explicitly */
  IdeStripTrail *t2 = ide_strip_model_add_trail(
    m, "Audio Path", make_tile("pa_flush", "src/audio/pa.c", 50, 95, 0));
  g_assert_nonnull(t2);
  g_assert_cmpuint(m->trails->len, ==, 3);
  g_assert_cmpuint(m->active_trail_idx, ==, 2);
  g_assert_cmpstr(t2->title, ==, "Audio Path");

  /* Switch with wrap-around forward and backward */
  g_assert_true(ide_strip_model_switch_trail(m, 1));
  g_assert_cmpuint(m->active_trail_idx, ==, 0);
  g_assert_true(ide_strip_model_switch_trail(m, -1));
  g_assert_cmpuint(m->active_trail_idx, ==, 2);

  /* Direct select */
  g_assert_true(ide_strip_model_select_trail(m, 1));
  g_assert_cmpuint(m->active_trail_idx, ==, 1);
  g_assert_false(ide_strip_model_select_trail(m, 99));

  /* Close middle trail, then second trail, then last remaining trail (resets cleanly) */
  g_assert_true(ide_strip_model_close_trail(m, 1));
  g_assert_cmpuint(m->trails->len, ==, 2);
  g_assert_cmpuint(m->active_trail_idx, ==, 1);

  g_assert_true(ide_strip_model_close_trail(m, 1));
  g_assert_cmpuint(m->trails->len, ==, 1);
  g_assert_cmpuint(m->active_trail_idx, ==, 0);

  g_assert_true(ide_strip_model_close_trail(m, 0));
  g_assert_cmpuint(m->trails->len, ==, 1);
  IdeStripTrail *reset = ide_strip_model_get_active_trail(m);
  g_assert_nonnull(reset);
  g_assert_null(reset->root_tile);
  g_assert_cmpuint(reset->ancestors->len, ==, 0);
  g_assert_cmpuint(reset->d1_tiles->len, ==, 0);
  g_assert_cmpint(reset->active_d1_idx, ==, -1);

  ide_strip_model_free(m);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide/strip/geometry", test_geometry_proportions_and_zen);
  g_test_add_func("/ide/strip/depth-promote-surface", test_depth_promote_and_surface);
  g_test_add_func("/ide/strip/magnetic-unlink-resync-springs", test_magnetic_follow_unlink_resync_and_springs);
  g_test_add_func("/ide/strip/dense-1000-tiles", test_dense_1000_tiles_magnetic_and_filter);
  g_test_add_func("/ide/strip/trails-and-pin", test_trails_branch_switch_close_and_pin);
  return g_test_run();
}
