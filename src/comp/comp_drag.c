/* comp_drag.c — 3-finger touchscreen / touchpad drag & flick (spec §3.15 GS1–GS6).
 *
 * The pure state machine lives in core/comp_gesture.c and the camera override in
 * core/comp_motion.c; this file connects them to the layout:
 *   - lock:    the camera of the locked axis is frozen and then follows the fingers
 *              (horizontal: active workspace camera; vertical: workspace strip);
 *   - release: the projected position picks the column / workspace to snap to,
 *              focus moves there (GS5) and the camera continues with the release
 *              velocity (M1); a projection onto the start is a glance (GS4).
 * `[touch].swipe_horizontal = "workspace"` maps horizontal drags onto the strip,
 * `"none"` ignores them.
 */
#include "comp_server.h"

#include "comp_log.h"

#include <math.h>
#include <stdlib.h>

static void
drag_reset(CompServer *s)
{
    comp_gesture_end(&s->drag);
    s->drag_active = false;
    s->drag_source = 0;
    s->drag_axis   = COMP_MOTION_DRAG_NONE;
}

bool
comp_drag_is_active(const CompServer *s, CompDragSource source)
{
    return s && s->drag_active && s->drag_source == (int)source;
}

bool
comp_drag_begin(CompServer *s, CompDragSource source)
{
    if (!s || !s->layout || !s->motion || s->drag_active || s->locked ||
        comp_screenshot_is_active(s) || comp_switcher_is_active(s))
        return false;
    if (s->config && !s->config->touch_enabled)
        return false;
    comp_gesture_begin(&s->drag);
    s->drag_active = true;
    s->drag_source = (int)source;
    s->drag_axis   = COMP_MOTION_DRAG_NONE;
    return true;
}

static CompTouchSwipeAction
horizontal_mode(const CompServer *s)
{
    return s->config ? s->config->touch_swipe_horizontal : COMP_SWIPE_COLUMN;
}

/* Camera range for the rubber band: every column's snapped view and the start. */
static void
horizontal_range(CompServer *s, double start, double view_w, double *lo, double *hi)
{
    *lo = start;
    *hi = start;
    size_t ncols = comp_layout_column_count(s->layout, comp_layout_active_workspace(s->layout));
    for (size_t i = 0; i < ncols; i++) {
        double v = comp_layout_column_snap_view(s->layout, i, view_w, start);
        *lo = fmin(*lo, v);
        *hi = fmax(*hi, v);
    }
}

static bool
drag_lock(CompServer *s, CompGestureAxis axis)
{
    CompRect work = {0};
    comp_server_working_area(s, &work);

    CompMotionDragAxis maxis;
    if (axis == COMP_GESTURE_AXIS_Y) {
        maxis = COMP_MOTION_DRAG_Y;
    } else {
        CompTouchSwipeAction mode = horizontal_mode(s);
        if (mode == COMP_SWIPE_NONE)
            return false;
        maxis = (mode == COMP_SWIPE_WORKSPACE) ? COMP_MOTION_DRAG_Y : COMP_MOTION_DRAG_X;
    }

    double start = 0.0;
    if (!comp_motion_drag_begin(s->motion, maxis, comp_now_sec(), &start))
        return false;

    double lo = start, hi = start, dim = 0.0;
    if (maxis == COMP_MOTION_DRAG_X) {
        horizontal_range(s, start, work.w, &lo, &hi);
        dim = work.w * COMP_GESTURE_RUBBER_DIM;
    } else {
        double step = comp_motion_workspace_step(s->motion);
        size_t nws  = comp_layout_workspace_count(s->layout);
        lo  = 0.0;
        hi  = nws ? (double)(nws - 1) * step : 0.0;
        dim = step * COMP_GESTURE_RUBBER_DIM;
    }
    comp_gesture_set_range(&s->drag, start, lo, hi, dim);
    comp_motion_drag_set(s->motion, s->drag.value);
    s->drag_axis = maxis;
    return true;
}

bool
comp_drag_motion(CompServer *s, double dx, double dy, uint32_t time_msec)
{
    if (!s || !s->drag_active)
        return false;
    if (s->locked) {
        comp_drag_end(s, time_msec, true);   /* GS4: locking cancels */
        return false;
    }
    double t = (double)time_msec / 1000.0;
    CompGestureAxis locked = comp_gesture_update(&s->drag, dx, dy, t);
    if (locked != COMP_GESTURE_AXIS_NONE) {
        if (!drag_lock(s, locked)) {
            drag_reset(s);           /* axis disabled or camera unavailable: ignore the gesture */
            return false;
        }
        comp_server_schedule_frames(s);
        return true;
    }
    if (s->drag_axis != COMP_MOTION_DRAG_NONE) {
        comp_motion_drag_set(s->motion, s->drag.value);
        comp_server_schedule_frames(s);
    }
    return false;
}

static void
release_horizontal(CompServer *s, double velocity, double projected, bool cancel, double now)
{
    CompRect work = {0};
    comp_server_working_area(s, &work);
    size_t ncols = comp_layout_column_count(s->layout, comp_layout_active_workspace(s->layout));
    size_t cur = 0;
    bool has_focus = comp_layout_focused_column(s->layout, &cur);

    double target = NAN;
    size_t pick   = cur;
    double *snaps = ncols ? calloc(ncols, sizeof(double)) : NULL;
    if (snaps) {
        for (size_t i = 0; i < ncols; i++)
            snaps[i] = comp_layout_column_snap_view(s->layout, i, work.w, s->drag.start);
        if (!cancel && has_focus)
            pick = comp_gesture_pick_snap(snaps, ncols, cur, projected);
        if (pick < ncols)
            target = snaps[pick];
        free(snaps);
    }
    comp_motion_drag_end(s->motion, velocity, target, now);
    bool changed = !cancel && has_focus && pick != cur &&
                   comp_layout_focus_column_index(s->layout, pick);
    comp_window_relayout(s);
    if (changed)
        comp_ipc_emit_windows(s);
}

static void
release_vertical(CompServer *s, double velocity, double projected, bool cancel, double now)
{
    double step   = comp_motion_workspace_step(s->motion);
    size_t nws    = comp_layout_workspace_count(s->layout);
    size_t active = comp_layout_active_workspace(s->layout);
    size_t target = active;
    if (!cancel && step > 0.0)
        target = comp_gesture_pick_workspace(s->drag.value / step, projected / step, nws);
    comp_motion_drag_end(s->motion, velocity, NAN, now);
    bool changed = target != active && comp_layout_focus_workspace_index(s->layout, target);
    comp_window_relayout(s);
    if (changed) {
        comp_ipc_emit_workspaces(s);
        comp_ipc_emit_windows(s);
    }
}

void
comp_drag_end(CompServer *s, uint32_t time_msec, bool cancel)
{
    if (!s || !s->drag_active)
        return;
    if (s->drag_axis == COMP_MOTION_DRAG_NONE) {   /* never locked: nothing moved */
        drag_reset(s);
        return;
    }
    double t         = (double)time_msec / 1000.0;
    double velocity  = cancel ? 0.0 : comp_gesture_velocity(&s->drag, t);
    double projected = cancel ? s->drag.start : comp_gesture_projected(&s->drag, t);
    double now       = comp_now_sec();
    CompMotionDragAxis axis = s->drag_axis;
    drag_reset(s);
    if (axis == COMP_MOTION_DRAG_X)
        release_horizontal(s, velocity, projected, cancel, now);
    else
        release_vertical(s, velocity, projected, cancel, now);
}
