/* Tests for the comp_motion animation coordinator (spec §3.14 M1–M6, §3.12 WS7),
 * the comp_gesture drag & flick state machine (§3.15 GS1–GS6) and the layout
 * helpers they use. Pure core, no wlroots. */
#include "../src/comp/core/comp_gesture.h"
#include "../src/comp/core/comp_layout.h"
#include "../src/comp/core/comp_motion.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#define VW 1646.0
#define VH 1029.0

static CompLayout *
new_layout(void)
{
    CompLayoutConfig cfg;
    comp_layout_config_defaults(&cfg);
    CompLayout *L = comp_layout_new(&cfg);
    g_assert_nonnull(L);
    return L;
}

static CompMotion *
new_motion(bool enabled)
{
    CompMotionConfig cfg;
    comp_motion_config_defaults(&cfg);
    cfg.enabled = enabled;
    CompMotion *M = comp_motion_new(&cfg);
    g_assert_nonnull(M);
    return M;
}

typedef struct {
    CompWindowGeometry g[32];
    size_t             n;
    double             view_x, ws_y;
} Sample;

static Sample
sample(const CompMotion *M, double t)
{
    Sample s;
    memset(&s, 0, sizeof(s));
    s.n = comp_motion_sample(M, t, s.g, 32, &s.view_x, &s.ws_y);
    return s;
}

static const CompWindowGeometry *
find(const Sample *s, CompWinId id)
{
    for (size_t i = 0; i < s->n && i < 32; i++)
        if (s->g[i].id == id)
            return &s->g[i];
    return NULL;
}

static const CompWindowGeometry *
target_of(const CompLayout *L, CompWinId id, CompWindowGeometry *buf, size_t max)
{
    size_t n = comp_layout_arrange(L, VW, VH, buf, max);
    for (size_t i = 0; i < n && i < max; i++)
        if (buf[i].id == id)
            return &buf[i];
    return NULL;
}

/* The reported bug: the rightmost column (free space to its right) did not animate
 * when resized, narrower or wider, because only neighbours and the camera moved. */
static void
test_resize_rightmost_animates(void)
{
    CompLayout *L = new_layout();
    CompMotion *M = new_motion(true);
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_adjust_column_width(L, -0.1));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    Sample s0 = sample(M, 10.0);
    g_assert_false(comp_motion_is_animating(M, 10.0));

    for (int dir = 0; dir < 2; dir++) {
        double t = 10.0 + 10.0 * dir;
        g_assert_true(comp_layout_adjust_column_width(L, dir == 0 ? 0.1 : -0.1));
        CompWindowGeometry buf[8];
        double w1 = target_of(L, 2, buf, 8)->rect.w;
        double w_before = find(&s0, 2)->rect.w;
        g_assert_cmpfloat(fabs(w1 - w_before), >, 10.0);          /* a real change */
        g_assert_true(comp_motion_sync(M, L, VW, VH, t));
        Sample mid = sample(M, t + 0.03);
        const CompWindowGeometry *g = find(&mid, 2);
        g_assert_nonnull(g);
        g_assert_true(g->resizing);
        g_assert_cmpfloat(fabs(g->rect.w - w1), >, 1.0);          /* not jumped to the target */
        g_assert_cmpfloat(fabs(g->rect.w - w_before), >, 0.1);    /* already moving */
        g_assert_true(comp_motion_is_animating(M, t + 0.03));
        Sample end = sample(M, t + 5.0);
        g_assert_cmpfloat_with_epsilon(find(&end, 2)->rect.w, w1, 0.5);
        g_assert_false(find(&end, 2)->resizing);
        g_assert_false(comp_motion_is_animating(M, t + 5.0));
        s0 = end;
    }
    comp_motion_free(M);
    comp_layout_free(L);
}

/* Snap mode and floating windows keep the instant size (client-driven sizes). */
static void
test_resize_snaps_when_disabled_and_floating(void)
{
    CompLayout *L = new_layout();
    CompMotion *M = new_motion(false);
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    g_assert_true(comp_layout_adjust_column_width(L, 0.2));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 1.0));
    Sample s = sample(M, 1.01);
    CompWindowGeometry buf[4];
    g_assert_cmpfloat_with_epsilon(find(&s, 1)->rect.w, target_of(L, 1, buf, 4)->rect.w, 1e-9);
    g_assert_false(find(&s, 1)->resizing);
    comp_motion_free(M);

    M = new_motion(true);
    g_assert_true(comp_layout_add_floating(L, 7, (CompRect){ 100, 100, 400, 300 }));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    g_assert_true(comp_layout_set_floating_rect(L, 7, (CompRect){ 100, 100, 600, 450 }));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 1.0));
    s = sample(M, 1.01);
    g_assert_cmpfloat_with_epsilon(find(&s, 7)->rect.w, 600.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(find(&s, 7)->rect.h, 450.0, 1e-9);
    comp_motion_free(M);
    comp_layout_free(L);
}

/* Two workspaces: window 1 on the top one, window 2 on the one below it. */
static void
two_workspaces(CompLayout *L)
{
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_apply(L, COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_DOWN));
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_UP));
    g_assert_cmpuint(comp_layout_active_workspace(L), ==, 0);
}

/* WS7: both workspaces stay visible and scroll together during a switch. */
static void
test_workspace_strip_switch(void)
{
    CompLayout *L = new_layout();
    CompMotion *M = new_motion(true);
    two_workspaces(L);
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    Sample s = sample(M, 1.0);
    g_assert_true(find(&s, 1)->visible);
    g_assert_false(find(&s, 2)->visible);           /* off screen below */
    double y1 = find(&s, 1)->rect.y;

    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_DOWN));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 1.0));
    s = sample(M, 1.05);
    const CompWindowGeometry *a = find(&s, 1), *b = find(&s, 2);
    g_assert_true(a->visible);
    g_assert_true(b->visible);
    g_assert_cmpfloat(a->rect.y, <, y1);            /* outgoing moves up */
    g_assert_cmpfloat(b->rect.y, >, a->rect.y);     /* incoming follows below it */
    double step = comp_motion_workspace_step(M);
    g_assert_cmpfloat_with_epsilon(b->rect.y - a->rect.y, step, 1e-6);   /* one strip */

    s = sample(M, 5.0);
    g_assert_false(find(&s, 1)->visible);
    g_assert_true(find(&s, 2)->visible);
    g_assert_cmpfloat_with_epsilon(find(&s, 2)->rect.y, y1, 1e-6);
    g_assert_false(comp_motion_is_animating(M, 5.0));
    comp_motion_free(M);
    comp_layout_free(L);
}

/* Each workspace keeps its own camera across a round trip. */
static void
test_per_workspace_camera(void)
{
    CompLayout *L = new_layout();
    CompMotion *M = new_motion(true);
    g_assert_true(comp_layout_add_window(L, 1));
    g_assert_true(comp_layout_add_window(L, 2));
    g_assert_true(comp_layout_add_window(L, 3));   /* scrolls workspace 0 */
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    Sample s = sample(M, 5.0);
    double view0 = s.view_x;
    g_assert_cmpfloat(view0, >, 0.0);

    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_DOWN));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 5.0));
    s = sample(M, 10.0);
    g_assert_cmpfloat_with_epsilon(s.view_x, 0.0, 1e-6);    /* empty workspace */
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_UP));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 10.0));
    s = sample(M, 10.0);
    g_assert_cmpfloat_with_epsilon(s.view_x, view0, 1e-6);  /* came back unchanged */
    comp_motion_free(M);
    comp_layout_free(L);
}

/* Removing an empty workspace above the active one shifts indices but must not animate. */
static void
test_workspace_removal_does_not_animate(void)
{
    CompLayout *L = new_layout();
    CompMotion *M = new_motion(true);
    two_workspaces(L);
    g_assert_true(comp_layout_apply(L, COMP_ACT_FOCUS_WORKSPACE_DOWN));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    Sample before = sample(M, 5.0);
    g_assert_false(comp_motion_is_animating(M, 5.0));
    uint64_t id = comp_layout_workspace_id(L, 1);

    g_assert_true(comp_layout_remove_window(L, 1));   /* workspace 0 empties and disappears */
    g_assert_cmpuint(comp_layout_active_workspace(L), ==, 0);
    g_assert_cmpuint(comp_layout_workspace_id(L, 0), ==, id);
    g_assert_true(comp_motion_sync(M, L, VW, VH, 5.0));
    Sample after = sample(M, 5.02);
    g_assert_cmpfloat_with_epsilon(find(&after, 2)->rect.y, find(&before, 2)->rect.y, 1e-6);
    g_assert_false(comp_motion_is_animating(M, 5.02));
    comp_motion_free(M);
    comp_layout_free(L);
}

/* M6 + M1: a dragged camera follows the fingers, sync never retargets it, and the
 * release continues from the dragged value with the release velocity. */
static void
test_drag_override_and_release(void)
{
    CompLayout *L = new_layout();
    CompMotion *M = new_motion(true);
    for (CompWinId id = 1; id <= 4; id++)
        g_assert_true(comp_layout_add_window(L, id));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    double start = 0.0;
    g_assert_false(comp_motion_drag_begin(M, COMP_MOTION_DRAG_NONE, 5.0, &start));
    g_assert_true(comp_motion_drag_begin(M, COMP_MOTION_DRAG_X, 5.0, &start));
    g_assert_false(comp_motion_drag_begin(M, COMP_MOTION_DRAG_Y, 5.0, &start));   /* one at a time */
    g_assert_cmpint(comp_motion_drag_axis(M), ==, COMP_MOTION_DRAG_X);
    g_assert_true(comp_motion_is_animating(M, 5.0));
    comp_motion_drag_set(M, start - 300.0);
    g_assert_true(comp_motion_sync(M, L, VW, VH, 5.1));                           /* must not retarget */
    Sample s = sample(M, 5.2);
    g_assert_cmpfloat_with_epsilon(s.view_x, start - 300.0, 1e-9);

    double target = comp_layout_column_snap_view(L, 0, VW, start);
    comp_motion_drag_end(M, -2000.0, target, 5.2);
    g_assert_cmpint(comp_motion_drag_axis(M), ==, COMP_MOTION_DRAG_NONE);
    s = sample(M, 5.2);
    g_assert_cmpfloat_with_epsilon(s.view_x, start - 300.0, 1e-6);               /* no jump at release */
    Sample s2 = sample(M, 5.21);
    g_assert_cmpfloat(s2.view_x, <, s.view_x);                                   /* velocity carried */
    s = sample(M, 10.0);
    g_assert_cmpfloat_with_epsilon(s.view_x, target, 1e-3);

    /* Vertical: strip drag */
    g_assert_true(comp_motion_drag_begin(M, COMP_MOTION_DRAG_Y, 10.0, &start));
    g_assert_cmpfloat_with_epsilon(start, 0.0, 1e-9);
    comp_motion_drag_set(M, 0.4 * comp_motion_workspace_step(M));
    s = sample(M, 10.1);
    g_assert_cmpfloat_with_epsilon(s.ws_y, -0.4 * comp_motion_workspace_step(M), 1e-9);
    comp_motion_drag_end(M, 0.0, NAN, 10.1);
    g_assert_true(comp_motion_sync(M, L, VW, VH, 10.1));                          /* glance: back to 0 */
    s = sample(M, 15.0);
    g_assert_cmpfloat_with_epsilon(s.ws_y, 0.0, 1e-3);
    comp_motion_free(M);
    comp_layout_free(L);
}

/* M3: windows whose targets do not change are never retargeted. */
static void
test_stationary_windows_stay_still(void)
{
    CompLayout *L = new_layout();
    CompMotion *M = new_motion(true);
    two_workspaces(L);
    g_assert_true(comp_motion_sync(M, L, VW, VH, 0.0));
    g_assert_true(comp_motion_sync(M, L, VW, VH, 1.0));
    g_assert_false(comp_motion_is_animating(M, 1.0));
    comp_motion_free(M);
    comp_layout_free(L);
}

static void
test_layout_ids_and_snap_views(void)
{
    CompLayout *L = new_layout();
    uint64_t first = comp_layout_workspace_id(L, 0);
    g_assert_cmpuint(first, !=, 0);
    two_workspaces(L);
    uint64_t a = comp_layout_workspace_id(L, 0), b = comp_layout_workspace_id(L, 1);
    g_assert_cmpuint(a, ==, first);
    g_assert_cmpuint(a, !=, b);
    g_assert_cmpuint(comp_layout_workspace_id(L, 99), ==, 0);
    g_assert_true(comp_layout_apply(L, COMP_ACT_MOVE_WORKSPACE_DOWN));         /* ids follow the content */
    g_assert_cmpuint(comp_layout_workspace_id(L, 1), ==, a);
    g_assert_cmpuint(comp_layout_workspace_id(L, 0), ==, b);

    CompLayout *L2 = new_layout();
    for (CompWinId id = 1; id <= 4; id++)
        g_assert_true(comp_layout_add_window(L2, id));
    size_t col = 99;
    g_assert_true(comp_layout_focused_column(L2, &col));
    g_assert_cmpuint(col, ==, 3);
    g_assert_cmpuint(comp_layout_column_count(L2, 0), ==, 4);
    /* Snap views have no side effects: a pending center-column request survives */
    g_assert_true(comp_layout_apply(L2, COMP_ACT_CENTER_COLUMN));
    double v0 = comp_layout_column_snap_view(L2, 0, VW, 2000.0);
    g_assert_cmpfloat_with_epsilon(v0, 0.0, 1e-9);                            /* first column at the left */
    double centered = comp_layout_view_target(L2, VW, 0.0);
    CompWindowGeometry g[8];
    size_t n = comp_layout_arrange(L2, VW, VH, g, 8);
    g_assert_cmpuint(n, ==, 4);
    g_assert_cmpfloat_with_epsilon(centered, g[3].rect.x + g[3].rect.w / 2.0 - VW / 2.0, 1e-9);
    g_assert_true(comp_layout_focus_column_index(L2, 1));
    g_assert_false(comp_layout_focus_column_index(L2, 1));
    g_assert_false(comp_layout_focus_column_index(L2, 9));
    g_assert_cmpuint(comp_layout_focused_window(L2), ==, 2);
    /* Arrange of another workspace: never focused */
    g_assert_cmpuint(comp_layout_arrange_workspace(L2, 1, VW, VH, g, 8), ==, 0);
    comp_layout_free(L2);
    comp_layout_free(L);
}

static void
test_gesture_lock_and_follow(void)
{
    CompGesture g;
    comp_gesture_begin(&g);
    g_assert_cmpint(comp_gesture_update(&g, 10.0, 2.0, 0.00), ==, COMP_GESTURE_AXIS_NONE);
    g_assert_cmpint(comp_gesture_update(&g, NAN, 0.0, 0.01), ==, COMP_GESTURE_AXIS_NONE);
    /* 18 px horizontal vs 15 px vertical: not dominant enough (ratio 1.25) */
    g_assert_cmpint(comp_gesture_update(&g, 8.0, 13.0, 0.02), ==, COMP_GESTURE_AXIS_NONE);
    g_assert_cmpint(g.axis, ==, COMP_GESTURE_AXIS_NONE);
    comp_gesture_end(&g);

    comp_gesture_begin(&g);
    g_assert_cmpint(comp_gesture_update(&g, 0.0, -20.0, 0.00), ==, COMP_GESTURE_AXIS_Y);
    comp_gesture_set_range(&g, 0.0, 0.0, 2000.0, 300.0);
    g_assert_cmpint(comp_gesture_update(&g, 0.0, -100.0, 0.05), ==, COMP_GESTURE_AXIS_NONE);
    g_assert_cmpfloat_with_epsilon(g.value, 100.0, 1e-9);           /* 1:1 from the lock point */
    /* Past the start (fingers down on the first workspace): rubber band */
    g_assert_cmpint(comp_gesture_update(&g, 0.0, 500.0, 0.10), ==, COMP_GESTURE_AXIS_NONE);
    g_assert_cmpfloat(g.value, <, 0.0);
    g_assert_cmpfloat(g.value, >, -300.0);
    comp_gesture_end(&g);
    g_assert_false(g.active);
}

static void
test_gesture_velocity_and_projection(void)
{
    CompGesture g;
    comp_gesture_begin(&g);
    (void)comp_gesture_update(&g, -20.0, 0.0, 1.000);
    comp_gesture_set_range(&g, 500.0, -10000.0, 10000.0, 500.0);
    for (int i = 1; i <= 10; i++)
        (void)comp_gesture_update(&g, -10.0, 0.0, 1.000 + 0.01 * i);   /* 1000 px/s left */
    double v = comp_gesture_velocity(&g, 1.10);
    g_assert_cmpfloat_with_epsilon(v, 1000.0, 1.0);                    /* camera moves right */
    g_assert_cmpfloat_with_epsilon(comp_gesture_projected(&g, 1.10), g.value + 1000.0 * COMP_GESTURE_TAU_S, 1.0);
    /* Held still for longer than the window before lifting: no velocity */
    g_assert_cmpfloat_with_epsilon(comp_gesture_velocity(&g, 1.50), 0.0, 1e-9);
    comp_gesture_end(&g);
    g_assert_cmpfloat_with_epsilon(comp_gesture_velocity(&g, 1.0), 0.0, 1e-9);
}

static void
test_gesture_snap_picks(void)
{
    /* Workspaces (units): glance, slow drag past half, flick back, flick capped, clamp */
    g_assert_cmpuint(comp_gesture_pick_workspace(0.36, 0.36, 3), ==, 0);
    g_assert_cmpuint(comp_gesture_pick_workspace(0.56, 0.56, 3), ==, 1);
    g_assert_cmpuint(comp_gesture_pick_workspace(0.87, 0.40, 3), ==, 0);
    g_assert_cmpuint(comp_gesture_pick_workspace(0.10, 0.70, 3), ==, 1);   /* short flick switches */
    g_assert_cmpuint(comp_gesture_pick_workspace(0.10, 2.90, 3), ==, 1);   /* at most one */
    g_assert_cmpuint(comp_gesture_pick_workspace(0.00, -0.8, 3), ==, 0);
    g_assert_cmpuint(comp_gesture_pick_workspace(1.00, NAN, 3), ==, 1);
    g_assert_cmpuint(comp_gesture_pick_workspace(1.00, 1.0, 0), ==, 0);

    /* Columns: nearest snap; ties keep the current one (glance) */
    const double snaps[4] = { 0.0, 0.0, 600.0, 1200.0 };
    g_assert_cmpuint(comp_gesture_pick_snap(snaps, 4, 1, 10.0), ==, 1);
    g_assert_cmpuint(comp_gesture_pick_snap(snaps, 4, 1, 700.0), ==, 2);
    g_assert_cmpuint(comp_gesture_pick_snap(snaps, 4, 3, 1100.0), ==, 3);
    g_assert_cmpuint(comp_gesture_pick_snap(snaps, 4, 9, NAN), ==, 3);
    g_assert_cmpuint(comp_gesture_pick_snap(NULL, 0, 0, 1.0), ==, 0);

    /* Rubber band: identity inside, bounded outside, swapped bounds accepted */
    g_assert_cmpfloat_with_epsilon(comp_gesture_rubber_band(5.0, 0.0, 10.0, 100.0), 5.0, 1e-12);
    double far = comp_gesture_rubber_band(1e9, 0.0, 10.0, 100.0);
    g_assert_cmpfloat(far, >, 10.0);
    g_assert_cmpfloat(far, <, 110.0);
    g_assert_cmpfloat_with_epsilon(comp_gesture_rubber_band(5.0, 10.0, 0.0, 100.0), 5.0, 1e-12);
    g_assert_cmpfloat_with_epsilon(comp_gesture_rubber_band(-5.0, 0.0, 10.0, 0.0), 0.0, 1e-12);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/motion/resize-rightmost-animates", test_resize_rightmost_animates);
    g_test_add_func("/comp/motion/resize-snaps-disabled-floating", test_resize_snaps_when_disabled_and_floating);
    g_test_add_func("/comp/motion/workspace-strip-switch", test_workspace_strip_switch);
    g_test_add_func("/comp/motion/per-workspace-camera", test_per_workspace_camera);
    g_test_add_func("/comp/motion/workspace-removal-no-animation", test_workspace_removal_does_not_animate);
    g_test_add_func("/comp/motion/drag-override-and-release", test_drag_override_and_release);
    g_test_add_func("/comp/motion/stationary-windows", test_stationary_windows_stay_still);
    g_test_add_func("/comp/layout/workspace-ids-and-snap-views", test_layout_ids_and_snap_views);
    g_test_add_func("/comp/gesture/lock-and-follow", test_gesture_lock_and_follow);
    g_test_add_func("/comp/gesture/velocity-and-projection", test_gesture_velocity_and_projection);
    g_test_add_func("/comp/gesture/snap-picks", test_gesture_snap_picks);
    return g_test_run();
}
