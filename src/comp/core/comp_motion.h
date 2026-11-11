/* comp_motion.h — spring animation coordinator for kajo-comp (pure C).
 *
 * Couples `CompLayout` logical target geometries with `CompSpring` instances
 * (spec §3.14 M1–M5):
 *   - `workspace_switch`  (0.80, 523, 0.0001): vertical workspace transitions (WS7)
 *   - `horizontal_scroll` (0.85, 423, 0.0001): per-workspace camera `view_x`
 *   - `window_movement`   (0.75, 323, 0.0001): window x/y within the strip or floating layer
 *
 * Invariants:
 *   - M1: Retargeting preserves instantaneous position and velocity.
 *   - M3: Stationary windows whose target position did not change are never retargeted.
 *   - M5: Window width/height changes take effect immediately while x/y animate.
 *   - O1: Opening the first window on an empty workspace snaps `view_x` without scrolling.
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

typedef struct CompMotion CompMotion;

void        comp_motion_config_defaults(CompMotionConfig *cfg);
bool        comp_motion_config_valid(const CompMotionConfig *cfg);

CompMotion *comp_motion_new(const CompMotionConfig *cfg);
void        comp_motion_free(CompMotion *m);
bool        comp_motion_set_config(CompMotion *m, const CompMotionConfig *cfg);

/* Synchronizes spring targets with `layout` at monotonic time `now` (seconds). */
bool        comp_motion_sync(CompMotion *m, CompLayout *layout,
                             double view_w, double view_h, double now);

/* Evaluates animated window geometries at monotonic time `now` (seconds).
 * Returned `rect.x` and `rect.y` are viewport-relative screen coordinates
 * (tiled `x` already has animated `view_x` subtracted; `y` includes `ws_y`). */
size_t      comp_motion_sample(const CompMotion *m, double now,
                               CompWindowGeometry *out, size_t max,
                               double *out_view_x, double *out_ws_y);

/* True while any camera, workspace, or window spring has not settled at `now`. */
bool        comp_motion_is_animating(const CompMotion *m, double now);

#endif /* KAJO_COMP_MOTION_H */
