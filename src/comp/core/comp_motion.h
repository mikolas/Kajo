/* comp_motion.h — spring animation coordinator for kajo-comp (pure C).
 *
 * Couples `CompLayout` logical target geometries with `CompSpring` instances
 * (spec §3.14 M1–M6, §3.12 WS7):
 *   - `workspace_switch`  (0.80, 523, 0.0001): the vertical workspace strip camera
 *   - `horizontal_scroll` (0.85, 423, 0.0001): one horizontal camera per workspace
 *   - `window_movement`   (0.75, 323, 0.0001): window x/y and displayed width/height
 *
 * Windows of every workspace are tracked. Workspaces are stacked vertically
 * (WS7: 10 % gap); a window is reported visible while its workspace band
 * intersects the viewport, so the outgoing and incoming workspace scroll together.
 *
 * Invariants:
 *   - M1: Retargeting preserves instantaneous position and velocity; a released
 *         drag hands its velocity to the camera spring.
 *   - M3: Stationary windows whose target did not change are never retargeted.
 *   - M5: The client is configured with its final size once (by the caller);
 *         the displayed size of tiled windows animates on `window_movement`.
 *         Floating windows (client-driven sizes, interactive resize) snap.
 *   - M6: While a drag is active the dragged camera follows `comp_motion_drag_set()`
 *         and is never retargeted by `comp_motion_sync()`.
 *   - O1: Opening the first window on an empty workspace snaps its camera.
 *   - Workspaces are followed by stable id, so removing an empty workspace above
 *     the active one never animates.
 */
#ifndef KAJO_COMP_MOTION_H
#define KAJO_COMP_MOTION_H

#include "comp_layout.h"
#include "comp_spring.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool             enabled;
    double           slowdown;          /* 1.0 = normal speed; >1 slower; <=0 snaps */
    CompSpringParams workspace_switch;
    CompSpringParams horizontal_scroll;
    CompSpringParams window_movement;
} CompMotionConfig;

typedef enum {
    COMP_MOTION_DRAG_NONE = 0,
    COMP_MOTION_DRAG_X,                 /* camera of the active workspace (strip px) */
    COMP_MOTION_DRAG_Y,                 /* workspace strip camera (px; workspace i is at i * step) */
} CompMotionDragAxis;

typedef struct CompMotion CompMotion;

void        comp_motion_config_defaults(CompMotionConfig *cfg);
bool        comp_motion_config_valid(const CompMotionConfig *cfg);

CompMotion *comp_motion_new(const CompMotionConfig *cfg);
void        comp_motion_free(CompMotion *m);
bool        comp_motion_set_config(CompMotion *m, const CompMotionConfig *cfg);

/* Synchronizes spring targets with `layout` at monotonic time `now` (seconds). */
bool        comp_motion_sync(CompMotion *m, CompLayout *layout,
                             double view_w, double view_h, double now);

/* Evaluates animated window geometries of all workspaces at time `now` (seconds).
 * Returned `rect` is viewport-relative (tiled `x` has the workspace camera
 * subtracted; `y` includes the workspace strip offset); `rect.w/h` are the
 * displayed size. `visible` is false for inactive tabs and for windows whose
 * workspace is off screen; `resizing` is true while the displayed size has not
 * reached the target. `out_view_x` is the active workspace camera, `out_ws_y`
 * the active workspace's vertical offset on screen (0 when settled). */
size_t      comp_motion_sample(const CompMotion *m, double now,
                               CompWindowGeometry *out, size_t max,
                               double *out_view_x, double *out_ws_y);

/* True while a drag is active or any camera, strip or window spring has not settled. */
bool        comp_motion_is_animating(const CompMotion *m, double now);

/* Distance between the origins of adjacent workspaces (px), valid after a sync. */
double      comp_motion_workspace_step(const CompMotion *m);

/* ── Interactive camera drag (spec §3.15 GS1–GS6, M6) ── */
/* Freezes the camera of `axis` at its displayed value and reports it in `out_start`.
 * Fails if a drag is already active or no sync has happened yet. */
bool        comp_motion_drag_begin(CompMotion *m, CompMotionDragAxis axis, double now, double *out_start);
void        comp_motion_drag_set(CompMotion *m, double value);
/* Hands the camera back to its spring at the dragged value with `velocity` (camera
 * units per second), heading to `target` (NAN: stay at the dragged value). The next
 * sync retargets from there, keeping the velocity (M1). For a horizontal drag pass
 * the snapped view (comp_layout_column_snap_view), because the view target of the
 * "never" centring mode depends on the current camera position. */
void        comp_motion_drag_end(CompMotion *m, double velocity, double target, double now);
CompMotionDragAxis comp_motion_drag_axis(const CompMotion *m);

#endif /* KAJO_COMP_MOTION_H */
