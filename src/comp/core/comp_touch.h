/* comp_touch.h — pure-C touchscreen multi-finger & bezel gesture tracker
 * and touchpad 3-finger gesture tracking (spec §3.15).
 *
 * 3-finger swipes on either device are interactive drags with a flick
 * (comp_gesture.h, driven by comp_drag.c); this module tracks the fingers,
 * reports their centroid and detects the 1-finger bezel swipes.
 *
 * Zero wlroots, Vulkan, or GLib dependencies.
 */
#ifndef KAJO_COMP_TOUCH_H
#define KAJO_COMP_TOUCH_H

#include "comp_config.h"
#include "comp_layout.h"

#include <stdbool.h>
#include <stdint.h>

#define COMP_TOUCH_MAX_SLOTS        10
#define COMP_EDGE_BEZEL_RATIO       0.032
#define COMP_EDGE_SWIPE_MM          12.0

typedef enum {
    COMP_TOUCH_EV_NONE = 0,
    COMP_TOUCH_EV_BEZEL_TOP,
    COMP_TOUCH_EV_BEZEL_BOTTOM,
} CompTouchGestureEvent;

typedef struct {
    bool    active;
    int32_t touch_id;
    double  start_x; /* normalized 0.0..1.0 in raw device space */
    double  start_y;
    double  cur_x;
    double  cur_y;
} CompTouchSlot;

typedef struct {
    CompTouchSlot slots[COMP_TOUCH_MAX_SLOTS];
    int           max_fingers_in_gesture;
    bool          gesture_triggered; /* latched until all fingers lift: no client touch traffic */
    double        width_mm;
    double        height_mm;
    CompTransform transform;
    double        last_x_ratio; /* rotated [0.0..1.0] X coordinate of bezel swipe start */
} CompTouchTracker;

typedef struct {
    bool     active;
    uint32_t fingers;
} CompTouchpadSwipe;

void                  comp_touch_tracker_init(CompTouchTracker *t, double width_mm, double height_mm);
void                  comp_touch_tracker_set_geometry(CompTouchTracker *t, double width_mm, double height_mm,
                                                      CompTransform transform);
void                  comp_touch_tracker_down(CompTouchTracker *t, int32_t touch_id, double x_norm, double y_norm);
CompTouchGestureEvent comp_touch_tracker_motion(CompTouchTracker *t, int32_t touch_id, double x_norm, double y_norm);
void                  comp_touch_tracker_up(CompTouchTracker *t, int32_t touch_id);
void                  comp_touch_tracker_cancel(CompTouchTracker *t);
int                   comp_touch_tracker_active_count(const CompTouchTracker *t);
/* Mean position of the active fingers, normalized 0..1 in the rotated output frame. */
bool                  comp_touch_tracker_centroid(const CompTouchTracker *t, double *out_x, double *out_y);

void                  comp_touchpad_swipe_begin(CompTouchpadSwipe *s, uint32_t fingers);
void                  comp_touchpad_swipe_end(CompTouchpadSwipe *s);

#endif /* KAJO_COMP_TOUCH_H */
