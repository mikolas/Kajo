/* comp_gesture.h — interactive 3-finger drag & flick state machine (pure C, spec §3.15 GS1–GS6).
 *
 * Shared by the touchscreen and the touchpad. The caller feeds finger deltas in
 * logical px (content direction: +x = fingers moved right, +y = fingers moved down)
 * with an event timestamp in seconds. The gesture locks to the dominant axis after
 * COMP_GESTURE_LOCK_PX; from then on the camera value follows the fingers 1:1
 * (camera = start − finger travel), rubber-banded past [lo, hi]. On release the
 * position is projected forward by the release velocity, and the caller snaps to the
 * target nearest that projection (comp_gesture_pick_workspace,
 * comp_layout_drag_release_target).
 *
 * Zero wlroots, Vulkan or GLib dependencies.
 */
#ifndef KAJO_COMP_GESTURE_H
#define KAJO_COMP_GESTURE_H

#include <stdbool.h>
#include <stddef.h>

#define COMP_GESTURE_LOCK_PX      16.0   /* GS1: axis lock distance */
#define COMP_GESTURE_LOCK_RATIO   1.25   /* GS1: dominant axis must exceed the other by this */
#define COMP_GESTURE_TAU_S        0.333  /* GS3: release projection time */
#define COMP_GESTURE_RUBBER_K     0.55   /* GS2: rubber band stiffness */
#define COMP_GESTURE_RUBBER_DIM   0.30   /* GS2: max overshoot as a fraction of the axis size */
#define COMP_GESTURE_VEL_WINDOW_S 0.15   /* velocity is measured over the last 150 ms */
#define COMP_GESTURE_SAMPLES      32

typedef enum {
    COMP_GESTURE_AXIS_NONE = 0,
    COMP_GESTURE_AXIS_X,
    COMP_GESTURE_AXIS_Y,
} CompGestureAxis;

typedef struct {
    double t;   /* seconds */
    double d;   /* finger travel along the locked axis since the lock, px */
} CompGestureSample;

typedef struct {
    bool              active;      /* between begin and end/cancel */
    CompGestureAxis   axis;        /* NONE until locked */
    double            dx, dy;      /* finger travel since begin, px */
    double            lock_at;     /* travel on the locked axis at the moment of the lock */
    double            start;       /* camera value when the lock happened */
    double            lo, hi;      /* camera range before rubber banding */
    double            dim;         /* rubber band size (px) */
    double            value;       /* current camera value */
    CompGestureSample samples[COMP_GESTURE_SAMPLES];
    size_t            nsamples;
} CompGesture;

void            comp_gesture_begin(CompGesture *g);
/* Adds a finger delta. Returns the axis if this call locked it, NONE otherwise
 * (including every call after the lock). Non-finite input is ignored. */
CompGestureAxis comp_gesture_update(CompGesture *g, double ddx, double ddy, double t);
/* Must be called once after the lock, before the next update: the camera value at
 * the lock and its allowed range. lo > hi is treated as [hi, lo]. */
void            comp_gesture_set_range(CompGesture *g, double start, double lo, double hi, double dim);
/* Camera velocity (units per second; −finger velocity) over the last samples, as of
 * time `t`. A finger held still before lifting therefore reports ~0. */
double          comp_gesture_velocity(const CompGesture *g, double t);
/* Release-point projection: value + velocity × COMP_GESTURE_TAU_S. */
double          comp_gesture_projected(const CompGesture *g, double t);
void            comp_gesture_end(CompGesture *g);

/* Soft limit: identity inside [lo, hi]; outside approaches lo − dim / hi + dim. */
double          comp_gesture_rubber_band(double v, double lo, double hi, double dim);
/* GS3: workspace (in workspace units) nearest the projection, at most one away from
 * the workspace nearest the release point, clamped to [0, n). */
size_t          comp_gesture_pick_workspace(double released, double projected, size_t n);

#endif /* KAJO_COMP_GESTURE_H */
