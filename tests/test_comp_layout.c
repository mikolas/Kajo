/* Tests for src/comp/core/comp_layout.c, comp_motion.c, and comp_touch.c (pure core, no wlroots). */
#include "../src/comp/core/comp_layout.h"
#include "../src/comp/core/comp_motion.h"
#include "../src/comp/core/comp_touch.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#define VW 1646.0  /* Host A logical width at scale 1.75 */
#define VH 1029.0

static CompLayout *
new_default(void)
{
    CompLayoutConfig cfg;
    comp_layout_config_defaults(&cfg);
    CompLayout *L = comp_layout_new(&cfg);
    g_assert_nonnull(L);
    return L;
}

static void
assert_ok(const CompLayout *L)
{
    const char *why = NULL;
    if (!comp_layout_check_invariants(L, &why))
        g_error("invariant violated: %s", why);
}

static const CompWindowGeometry *
geom_of(const CompWindowGeometry *g, size_t n, CompWinId id)
{
    for (size_t i = 0; i < n; i++)
        if (g[i].id == id)
            return &g[i];
    return NULL;
}

static void
test_config_validation(void)
{
    CompLayoutConfig cfg;
    comp_layout_config_defaults(&cfg);
    g_assert_true(comp_layout_config_valid(&cfg));
    CompLayoutConfig bad = cfg;
    bad.gap = -1.0;
    g_assert_false(comp_layout_config_valid(&bad));
    bad = cfg;
    bad.default_width = 0.0;
    g_assert_false(comp_layout_config_valid(&bad));
    bad = cfg;
    bad.preset_widths[1] = 1.5;
    g_assert_false(comp_layout_config_valid(&bad));
    bad = cfg;
    bad.n_preset_widths = COMP_LAYOUT_MAX_PRESETS + 1;
    g_assert_false(comp_layout_config_valid(&bad));
    bad = cfg;
    bad.gap = NAN;
    g_assert_false(comp_layout_config_valid(&bad));
    g_assert_null(comp_layout_new(&bad));

    CompLayout *L = new_default();
    g_assert_false(comp_layout_set_config(L, &bad));
    g_assert_true(comp_layout_set_config(L, &cfg));
    comp_layout_free(L);
}

static void
test_add_remove_and_focus(void)
{
    CompLayout *L = new_default();
    assert_ok(L);
    g_assert_cmpuint(comp_layout_workspace_count(L), ==, 1);
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 0);

    g_assert_false(comp_layout_add_window(L, 0));
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_false(comp_layout_add_window(L, 1));      /* duplicate */
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_add_window(L, 3));
    assert_ok(L);
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 3);
    g_assert_cmpuint(comp_layout_window_count(L), ==, 3);
    /* One workspace with windows + the trailing empty one */
    g_assert_cmpuint(comp_layout_workspace_count(L), ==, 2);

    /* New windows open to the right of the focused column */
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_COLUMN_FIRST));
    g_assert_true(comp_layout_add_window(L, 4));
    CompWindowGeometry g[8];
    size_t n = comp_layout_arrange(L, VW, VH, g, 8);
    g_assert_cmpuint(n, ==, 4);
    g_assert_cmpfloat(geom_of(g, n, 1)->rect.x, <, geom_of(g, n, 4)->rect.x);
    g_assert_cmpfloat(geom_of(g, n, 4)->rect.x, <, geom_of(g, n, 2)->rect.x);

    /* Removing the focused window focuses a neighbour */
    g_assert_true(comp_layout_remove_window(L, 4));
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 2);
    g_assert_false(comp_layout_remove_window(L, 4));
    g_assert_true(comp_layout_remove_window(L, 1));
    g_assert_true(comp_layout_remove_window(L, 2));
    g_assert_true(comp_layout_remove_window(L, 3));
    assert_ok(L);
    g_assert_cmpuint(comp_layout_window_count(L), ==, 0);
    g_assert_cmpuint(comp_layout_workspace_count(L), ==, 1);
    comp_layout_free(L);
}

/* Two 0.5 columns fill the viewport exactly, with `gap` on both outer edges. */
static void
test_geometry_half_columns(void)
{
    CompLayout *L = new_default();
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    CompWindowGeometry g[2];
    g_assert_cmpuint(comp_layout_arrange(L, VW, VH, g, 2), ==, 2);
    const double gap = 6.0;
    g_assert_cmpfloat_with_epsilon(g[0].rect.x, gap, 1e-9);
    g_assert_cmpfloat_with_epsilon(g[0].rect.w, (VW - gap) * 0.5 - gap, 1e-9);
    g_assert_cmpfloat_with_epsilon(g[1].rect.x, g[0].rect.x + g[0].rect.w + gap, 1e-9);
    g_assert_cmpfloat_with_epsilon(g[1].rect.x + g[1].rect.w + gap, VW, 1e-9);
    g_assert_cmpfloat_with_epsilon(g[0].rect.y, gap, 1e-9);
    g_assert_cmpfloat_with_epsilon(g[0].rect.h, VH - 2.0 * gap, 1e-9);
    g_assert_true(g[1].focused);
    g_assert_false(g[0].focused);
    comp_layout_free(L);
}

static void
test_view_target_modes(void)
{
    CompLayout *L = new_default();
    /* Single 0.5 column, always_center_single_column: strip is centered */
    g_assert_true(comp_layout_add_window(L, 1));
    double strip = (VW - 6.0) * 0.5 - 6.0 + 12.0;
    g_assert_cmpfloat_with_epsilon(comp_layout_view_target(L, VW, 0.0), (strip - VW) / 2.0, 1e-9);

    /* Three 0.5 columns overflow: NEVER mode pans minimally to show the focused (last) one */
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_add_window(L, 3));
    CompWindowGeometry g[3];
    comp_layout_arrange(L, VW, VH, g, 3);
    double target = comp_layout_view_target(L, VW, 0.0);
    g_assert_cmpfloat_with_epsilon(target, g[2].rect.x + g[2].rect.w + 6.0 - VW, 1e-9);
    /* Already visible: camera stays put */
    g_assert_cmpfloat_with_epsilon(comp_layout_view_target(L, VW, target), target, 1e-9);
    /* Focus the first column: pan left to show it */
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_COLUMN_FIRST));
    g_assert_cmpfloat_with_epsilon(comp_layout_view_target(L, VW, target), 0.0, 1e-9);

    /* center-column is one-shot */
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_COLUMN_RIGHT));
    g_assert_true(comp_layout_apply(L, COMP_ACT_CENTER_COLUMN));
    double centered = g[1].rect.x + g[1].rect.w / 2.0 - VW / 2.0;
    g_assert_cmpfloat_with_epsilon(comp_layout_view_target(L, VW, 0.0), centered, 1e-9);
    g_assert_cmpfloat_with_epsilon(comp_layout_view_target(L, VW, 0.0), 0.0, 1e-9);

    /* ALWAYS mode */
    CompLayoutConfig cfg;
    comp_layout_config_defaults(&cfg);
    cfg.center_mode = COMP_CENTER_ALWAYS;
    cfg.always_center_single_column = false;
    g_assert_true(comp_layout_set_config(L, &cfg));
    g_assert_cmpfloat_with_epsilon(comp_layout_view_target(L, VW, 123.0), centered, 1e-9);
    comp_layout_free(L);
}

static double
focused_width_prop(CompLayout *L)
{
    CompWindowGeometry g[16];
    size_t n = comp_layout_arrange(L, VW, VH, g, 16);
    for (size_t i = 0; i < n; i++)
        if (g[i].focused)
            return (g[i].rect.w + 6.0) / (VW - 6.0);
    return -1.0;
}

static void
test_preset_widths_and_maximize(void)
{
    CompLayout *L = new_default();
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_cmpfloat_with_epsilon(focused_width_prop(L), 0.5, 1e-9);
    g_assert_true(comp_layout_apply(L, COMP_ACT_SWITCH_PRESET_COLUMN_WIDTH));
    g_assert_cmpfloat_with_epsilon(focused_width_prop(L), 0.66667, 1e-9);
    g_assert_true(comp_layout_apply(L, COMP_ACT_SWITCH_PRESET_COLUMN_WIDTH));
    g_assert_cmpfloat_with_epsilon(focused_width_prop(L), 0.33333, 1e-9);
    g_assert_true(comp_layout_apply(L, COMP_ACT_SWITCH_PRESET_COLUMN_WIDTH));
    g_assert_cmpfloat_with_epsilon(focused_width_prop(L), 0.5, 1e-9);

    g_assert_true(comp_layout_apply(L, COMP_ACT_MAXIMIZE_COLUMN));
    CompWindowGeometry g[1];
    comp_layout_arrange(L, VW, VH, g, 1);
    g_assert_cmpfloat_with_epsilon(g[0].rect.w, VW - 12.0, 1e-9);
    g_assert_true(comp_layout_apply(L, COMP_ACT_MAXIMIZE_COLUMN));
    g_assert_cmpfloat_with_epsilon(focused_width_prop(L), 0.5, 1e-9);

    g_assert_true(comp_layout_adjust_column_width(L, 0.1));
    g_assert_cmpfloat_with_epsilon(focused_width_prop(L), 0.6, 1e-9);
    g_assert_true(comp_layout_adjust_column_width(L, 10.0));
    g_assert_cmpfloat_with_epsilon(focused_width_prop(L), 1.0, 1e-9);
    g_assert_false(comp_layout_adjust_column_width(L, 0.5));
    g_assert_false(comp_layout_adjust_column_width(L, NAN));
    comp_layout_free(L);
}

static void
test_consume_expel_and_heights(void)
{
    CompLayout *L = new_default();
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_add_window(L, 3));
    g_assert_true(comp_layout_apply(L, COMP_ACT_CONSUME_INTO_COLUMN_LEFT));   /* 3 joins column of 2 */
    assert_ok(L);
    CompWindowGeometry g[3];
    size_t n = comp_layout_arrange(L, VW, VH, g, 3);
    g_assert_cmpuint(n, ==, 3);
    const CompWindowGeometry *a = geom_of(g, n, 2), *b = geom_of(g, n, 3);
    g_assert_cmpfloat_with_epsilon(a->rect.x, b->rect.x, 1e-9);
    g_assert_cmpfloat(a->rect.y, <, b->rect.y);
    g_assert_cmpfloat_with_epsilon(a->rect.h + b->rect.h + 3.0 * 6.0, VH, 1e-9);
    g_assert_true(b->focused);

    /* Preset height on the focused window, others take the rest */
    g_assert_true(comp_layout_apply(L, COMP_ACT_SWITCH_PRESET_WINDOW_HEIGHT));
    n = comp_layout_arrange(L, VW, VH, g, 3);
    a = geom_of(g, n, 2); b = geom_of(g, n, 3);
    g_assert_cmpfloat_with_epsilon(b->rect.h, (VH - 6.0) * 0.33333 - 6.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(a->rect.h + b->rect.h + 3.0 * 6.0, VH, 1e-9);
    g_assert_true(comp_layout_apply(L, COMP_ACT_RESET_WINDOW_HEIGHT));
    g_assert_false(comp_layout_apply(L, COMP_ACT_RESET_WINDOW_HEIGHT));

    /* Move inside the column, then expel back into its own column */
    g_assert_true(comp_layout_apply(L, COMP_ACT_MOVE_WINDOW_UP));
    g_assert_false(comp_layout_apply(L, COMP_ACT_MOVE_WINDOW_UP));
    g_assert_true(comp_layout_apply(L, COMP_ACT_EXPEL_FROM_COLUMN));
    g_assert_false(comp_layout_apply(L, COMP_ACT_EXPEL_FROM_COLUMN));   /* single-window column */
    assert_ok(L);
    n = comp_layout_arrange(L, VW, VH, g, 3);
    g_assert_cmpfloat(geom_of(g, n, 2)->rect.x, !=, geom_of(g, n, 3)->rect.x);
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 3);

    /* K1: consume-or-expel-window-left / -right */
    g_assert_true(comp_layout_apply(L, COMP_ACT_CONSUME_OR_EXPEL_LEFT));  /* 1-win col -> consumes left */
    assert_ok(L);
    g_assert_true(comp_layout_apply(L, COMP_ACT_CONSUME_OR_EXPEL_RIGHT)); /* 2-win col -> expels right */
    assert_ok(L);
    comp_layout_free(L);
}

static void
test_fullscreen_and_motion(void)
{
    CompLayout *L = new_default();
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_apply(L, COMP_ACT_CONSUME_INTO_COLUMN_LEFT)); /* [1,2] stacked in col 0 */
    /* S2: fullscreening a window inside a multi-window stacked column expels it first */
    g_assert_true(comp_layout_toggle_fullscreen(L));
    assert_ok(L);
    CompWindowGeometry g[2];
    size_t n = comp_layout_arrange(L, VW, VH, g, 2);
    g_assert_cmpuint(n, ==, 2);
    const CompWindowGeometry *w2 = geom_of(g, n, 2);
    g_assert_true(w2->fullscreen);
    g_assert_cmpfloat_with_epsilon(w2->rect.w, VW, 1e-9);
    g_assert_cmpfloat_with_epsilon(w2->rect.h, VH, 1e-9);
    g_assert_true(comp_layout_toggle_fullscreen(L));
    n = comp_layout_arrange(L, VW, VH, g, 2);
    g_assert_false(geom_of(g, n, 2)->fullscreen);

    /* Spring coordinator (comp_motion) */
    CompMotionConfig mcfg;
    comp_motion_config_defaults(&mcfg);
    g_assert_true(comp_motion_config_valid(&mcfg));
    CompMotion *M = comp_motion_new(&mcfg);
    g_assert_nonnull(M);

    /* O1: initial sync snaps without animating */
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    g_assert_false(comp_motion_is_animating(M, 0.0));

    /* Adding a third column scrolls the camera via horizontal_scroll spring */
    g_assert_true(comp_layout_add_window(L, 3));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 1.0));
    g_assert_true(comp_motion_is_animating(M, 1.0));
    CompWindowGeometry mg[4];
    double vx = 0.0, wy = 0.0;
    g_assert_cmpuint(comp_motion_sample(M, 1.05, mg, 4, &vx, &wy), ==, 3);
    g_assert_cmpfloat(vx, >, 0.0);
    g_assert_false(comp_motion_is_animating(M, 3.0));

    comp_motion_free(M);
    comp_layout_free(L);
}

static void
test_tabbed_column(void)
{
    CompLayout *L = new_default();
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_apply(L, COMP_ACT_CONSUME_INTO_COLUMN_LEFT));
    g_assert_true(comp_layout_apply(L, COMP_ACT_TOGGLE_COLUMN_TABBED));
    CompWindowGeometry g[2];
    size_t n = comp_layout_arrange(L, VW, VH, g, 2);
    const CompWindowGeometry *a = geom_of(g, n, 1), *b = geom_of(g, n, 2);
    g_assert_false(a->visible);
    g_assert_true(b->visible);
    g_assert_cmpfloat_with_epsilon(a->rect.y, b->rect.y, 1e-9);
    g_assert_cmpfloat_with_epsilon(a->rect.h, b->rect.h, 1e-9);
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WINDOW_UP));
    n = comp_layout_arrange(L, VW, VH, g, 2);
    g_assert_true(geom_of(g, n, 1)->visible);
    g_assert_false(geom_of(g, n, 2)->visible);
    comp_layout_free(L);
}

static void
test_floating(void)
{
    CompLayout *L = new_default();
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_apply(L, COMP_ACT_TOGGLE_WINDOW_FLOATING));
    assert_ok(L);
    CompWindowGeometry g[2];
    size_t n = comp_layout_arrange(L, VW, VH, g, 2);
    const CompWindowGeometry *f = geom_of(g, n, 2);
    g_assert_true(f->floating);
    g_assert_true(f->focused);
    g_assert_cmpfloat_with_epsilon(f->rect.w, VW / 2.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(f->rect.x, VW / 4.0, 1e-9);

    g_assert_true(comp_layout_set_floating_rect(L, 2, (CompRect){ 10, 20, 300, 200 }));
    g_assert_false(comp_layout_set_floating_rect(L, 1, (CompRect){ 10, 20, 300, 200 }));
    g_assert_false(comp_layout_set_floating_rect(L, 2, (CompRect){ NAN, 0, 1, 1 }));

    g_assert_true(comp_layout_apply(L, COMP_ACT_SWITCH_FOCUS_FLOATING_TILING));
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 1);
    g_assert_true(comp_layout_apply(L, COMP_ACT_SWITCH_FOCUS_FLOATING_TILING));
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 2);

    /* Back to tiling */
    g_assert_true(comp_layout_apply(L, COMP_ACT_TOGGLE_WINDOW_FLOATING));
    n = comp_layout_arrange(L, VW, VH, g, 2);
    g_assert_false(geom_of(g, n, 2)->floating);
    assert_ok(L);

    g_assert_true(comp_layout_add_floating(L, 9, (CompRect){ 1, 2, 3, 4 }));
    g_assert_false(comp_layout_add_floating(L, 9, (CompRect){ 1, 2, 3, 4 }));
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 9);
    g_assert_true(comp_layout_remove_window(L, 9));
    g_assert_cmpuint(comp_layout_focused_window(L), !=, 9);
    assert_ok(L);
    comp_layout_free(L);
}

static void
test_workspaces(void)
{
    CompLayout *L = new_default();
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_cmpuint(comp_layout_workspace_count(L), ==, 2);

    /* Move a column down into the trailing empty workspace: a new empty one appears */
    g_assert_true(comp_layout_apply(L, COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_DOWN));
    assert_ok(L);
    g_assert_cmpuint(comp_layout_workspace_count(L), ==, 3);
    g_assert_cmpuint(comp_layout_active_workspace(L), ==, 1);
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 2);

    /* Going down to the empty one and back up: the empty one stays the only trailing one */
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_DOWN));
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 0);
    g_assert_false(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_DOWN));
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_UP));
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_UP));
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 1);
    assert_ok(L);

    /* Swap workspaces */
    g_assert_true(comp_layout_apply(L, COMP_ACT_MOVE_WORKSPACE_DOWN));
    g_assert_cmpuint(comp_layout_active_workspace(L), ==, 1);
    g_assert_false(comp_layout_apply(L, COMP_ACT_MOVE_WORKSPACE_DOWN));  /* would pass the trailing empty one */
    assert_ok(L);

    /* focus_window switches workspace */
    g_assert_true(comp_layout_focus_window(L, 2));
    g_assert_cmpuint(comp_layout_active_workspace(L), ==, 0);

    /* Emptying a middle workspace and leaving it removes it */
    g_assert_true(comp_layout_remove_window(L, 2));
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 0);
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_DOWN));
    assert_ok(L);
    g_assert_cmpuint(comp_layout_workspace_count(L), ==, 2);
    g_assert_cmpuint(comp_layout_focused_window(L), ==, 1);
    comp_layout_free(L);
}

/* The action name table must be complete and round-trip. Keeps binds.toml docs honest. */
static void
test_action_names(void)
{
    for (int i = 0; i < COMP_ACT__COUNT; i++) {
        const char *name = comp_action_name((CompAction)i);
        g_assert_nonnull(name);
        CompAction back;
        g_assert_true(comp_action_from_name(name, &back));
        g_assert_cmpint((int)back, ==, i);
        for (int j = 0; j < i; j++)
            g_assert_cmpstr(name, !=, comp_action_name((CompAction)j));
    }
    g_assert_null(comp_action_name(COMP_ACT__COUNT));
    g_assert_false(comp_action_from_name("no-such-action", NULL));
    g_assert_false(comp_action_from_name(NULL, NULL));
}

/* Random operation sequences; invariants and the id set are checked after every step. */
static void
test_monkey(void)
{
    GRand *rng = g_rand_new_with_seed(0x6b616a6f);
    for (int round = 0; round < 40; round++) {
        CompLayout *L = new_default();
        GHashTable *model = g_hash_table_new(g_int64_hash, g_int64_equal);
        gint64 *ids = g_new0(gint64, 4096);
        size_t next_id = 1;
        for (int step = 0; step < 1500; step++) {
            guint32 op = g_rand_int_range(rng, 0, 100);
            if (op < 22 && next_id < 4096) {
                ids[next_id] = (gint64)next_id;
                gboolean floating = g_rand_int_range(rng, 0, 6) == 0;
                gboolean ok = floating
                    ? comp_layout_add_floating(L, next_id, (CompRect){ 0, 0, 0, 0 })
                    : comp_layout_add_window(L, next_id);
                g_assert_true(ok);
                g_hash_table_add(model, &ids[next_id]);
                next_id++;
            } else if (op < 36 && g_hash_table_size(model) > 0) {
                gint64 pick = ids[g_rand_int_range(rng, 1, (gint32)next_id)];
                gboolean in = g_hash_table_contains(model, &pick);
                g_assert_cmpint(comp_layout_remove_window(L, (CompWinId)pick), ==, in);
                g_hash_table_remove(model, &pick);
            } else if (op < 40 && next_id > 1) {
                gint64 pick = ids[g_rand_int_range(rng, 1, (gint32)next_id)];
                g_assert_cmpint(comp_layout_focus_window(L, (CompWinId)pick), ==,
                                g_hash_table_contains(model, &pick));
            } else if (op < 44) {
                (void)comp_layout_adjust_column_width(L, g_rand_double_range(rng, -0.5, 0.5));
            } else if (op < 47) {
                (void)comp_layout_adjust_window_height(L, g_rand_double_range(rng, -0.5, 0.5));
            } else {
                (void)comp_layout_apply(L, (CompAction)g_rand_int_range(rng, 0, COMP_ACT__COUNT));
            }
            assert_ok(L);
            g_assert_cmpuint(comp_layout_window_count(L), ==, g_hash_table_size(model));
            CompWinId f = comp_layout_focused_window(L);
            if (f) {
                gint64 fi = (gint64)f;
                g_assert_true(g_hash_table_contains(model, &fi));
            }
            CompWindowGeometry g[64];
            size_t n = comp_layout_arrange(L, VW, VH, g, 64);
            for (size_t i = 0; i < n && i < 64; i++) {
                g_assert_true(isfinite(g[i].rect.x) && isfinite(g[i].rect.y));
                g_assert_cmpfloat(g[i].rect.w, >=, 1.0);
                g_assert_cmpfloat(g[i].rect.h, >=, 1.0);
            }
            g_assert_true(isfinite(comp_layout_view_target(L, VW, 0.0)));
        }
        g_hash_table_destroy(model);
        g_free(ids);
        comp_layout_free(L);
    }
    g_rand_free(rng);
}

static void
test_touch_gestures_and_touchpad_swipes(void)
{
    CompTouchTracker t;
    comp_touch_tracker_init(&t, 300.0, 200.0);

    /* 1. Top bezel swipe down (>= 12mm in 200mm height -> dy >= 0.06) */
    comp_touch_tracker_down(&t, 1, 0.45, 0.01);
    g_assert_cmpint(comp_touch_tracker_active_count(&t), ==, 1);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 1, 0.45, 0.04), ==, COMP_TOUCH_EV_NONE);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 1, 0.45, 0.09), ==, COMP_TOUCH_EV_BEZEL_TOP);
    g_assert_cmpfloat_with_epsilon(t.last_x_ratio, 0.45, 1e-6);
    /* Second motion in same gesture must not re-fire */
    g_assert_cmpint(comp_touch_tracker_motion(&t, 1, 0.45, 0.15), ==, COMP_TOUCH_EV_NONE);
    comp_touch_tracker_up(&t, 1);
    g_assert_cmpint(comp_touch_tracker_active_count(&t), ==, 0);

    /* 2. Bottom bezel swipe up */
    comp_touch_tracker_down(&t, 2, 0.70, 0.99);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 2, 0.70, 0.90), ==, COMP_TOUCH_EV_BEZEL_BOTTOM);
    g_assert_cmpfloat_with_epsilon(t.last_x_ratio, 0.70, 1e-6);
    comp_touch_tracker_up(&t, 2);

    /* 3. Mid-screen 1-finger swipe and diagonal drift must NOT trigger bezel swipe */
    comp_touch_tracker_down(&t, 3, 0.50, 0.50);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 3, 0.50, 0.30), ==, COMP_TOUCH_EV_NONE);
    comp_touch_tracker_up(&t, 3);

    comp_touch_tracker_down(&t, 4, 0.50, 0.01);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 4, 0.60, 0.09), ==, COMP_TOUCH_EV_NONE);
    comp_touch_tracker_up(&t, 4);

    /* 4. 90-degree rotated output transform: physical left edge (raw_x = 0.01) becomes logical top bezel */
    comp_touch_tracker_set_geometry(&t, 300.0, 200.0, COMP_TRANSFORM_90);
    comp_touch_tracker_down(&t, 5, 0.01, 0.40);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 5, 0.08, 0.40), ==, COMP_TOUCH_EV_BEZEL_TOP);
    g_assert_cmpfloat_with_epsilon(t.last_x_ratio, 0.60, 1e-6);
    comp_touch_tracker_up(&t, 5);
    comp_touch_tracker_set_geometry(&t, 300.0, 200.0, COMP_TRANSFORM_NORMAL);

    /* 5. 3-finger global swipes (up, down, left, right) + progressive 1->2->3 re-anchoring */
    comp_touch_tracker_down(&t, 10, 0.3, 0.5);
    comp_touch_tracker_motion(&t, 10, 0.3, 0.6); /* moved before 2nd/3rd finger land -> re-anchored */
    comp_touch_tracker_down(&t, 11, 0.5, 0.6);
    comp_touch_tracker_down(&t, 12, 0.7, 0.6);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 10, 0.3, 0.50), ==, COMP_TOUCH_EV_NONE);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 11, 0.5, 0.50), ==, COMP_TOUCH_EV_NONE);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 12, 0.7, 0.50), ==, COMP_TOUCH_EV_3F_SWIPE_UP);
    comp_touch_tracker_cancel(&t);
    g_assert_cmpint(comp_touch_tracker_active_count(&t), ==, 0);

    comp_touch_tracker_down(&t, 1, 0.3, 0.5);
    comp_touch_tracker_down(&t, 2, 0.5, 0.5);
    comp_touch_tracker_down(&t, 3, 0.7, 0.5);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 1, 0.36, 0.5), ==, COMP_TOUCH_EV_NONE);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 2, 0.56, 0.5), ==, COMP_TOUCH_EV_NONE);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 3, 0.76, 0.5), ==, COMP_TOUCH_EV_3F_SWIPE_RIGHT);
    comp_touch_tracker_cancel(&t);

    /* 6. NaN / Infinity / slot overflow safety */
    comp_touch_tracker_down(&t, 99, NAN, 0.5);
    comp_touch_tracker_down(&t, 99, 0.5, INFINITY);
    g_assert_cmpint(comp_touch_tracker_active_count(&t), ==, 0);
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS + 4; i++)
        comp_touch_tracker_down(&t, i + 100, 0.5, 0.5);
    g_assert_cmpint(comp_touch_tracker_active_count(&t), ==, COMP_TOUCH_MAX_SLOTS);
    comp_touch_tracker_cancel(&t);

    /* 7. Touchpad 3-finger swipe state machine */
    CompTouchpadSwipe tp;
    CompAction act = COMP_ACT__COUNT;
    comp_touchpad_swipe_begin(&tp, 3);
    g_assert_false(comp_touchpad_swipe_update(&tp, 3, NAN, 10.0, true, &act));
    g_assert_false(comp_touchpad_swipe_update(&tp, 3, -20.0, 0.0, true, &act));
    g_assert_true(comp_touchpad_swipe_update(&tp, 3, -25.0, 0.0, true, &act));
    g_assert_cmpint(act, ==, COMP_ACT_FOCUS_COLUMN_RIGHT);
    /* Already triggered in this gesture */
    g_assert_false(comp_touchpad_swipe_update(&tp, 3, -50.0, 0.0, true, &act));
    comp_touchpad_swipe_end(&tp);

    comp_touchpad_swipe_begin(&tp, 3);
    g_assert_true(comp_touchpad_swipe_update(&tp, 3, 0.0, -45.0, true, &act));
    g_assert_cmpint(act, ==, COMP_ACT_FOCUS_WORKSPACE_DOWN);
    comp_touchpad_swipe_end(&tp);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/layout/config-validation", test_config_validation);
    g_test_add_func("/comp/layout/add-remove-focus", test_add_remove_and_focus);
    g_test_add_func("/comp/layout/geometry-half-columns", test_geometry_half_columns);
    g_test_add_func("/comp/layout/view-target-modes", test_view_target_modes);
    g_test_add_func("/comp/layout/preset-widths-maximize", test_preset_widths_and_maximize);
    g_test_add_func("/comp/layout/consume-expel-heights", test_consume_expel_and_heights);
    g_test_add_func("/comp/layout/fullscreen-and-motion", test_fullscreen_and_motion);
    g_test_add_func("/comp/layout/tabbed-column", test_tabbed_column);
    g_test_add_func("/comp/layout/floating", test_floating);
    g_test_add_func("/comp/layout/workspaces", test_workspaces);
    g_test_add_func("/comp/layout/action-names", test_action_names);
    g_test_add_func("/comp/layout/touch-and-touchpad", test_touch_gestures_and_touchpad_swipes);
    g_test_add_func("/comp/layout/monkey", test_monkey);
    return g_test_run();
}
