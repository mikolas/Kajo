#pragma once

#include <glib.h>
#include <stdint.h>

typedef struct _ShellApp           ShellApp;
typedef struct _ShellTouchGestures ShellTouchGestures;

typedef enum {
    TOUCH_GESTURE_NONE = 0,
    TOUCH_GESTURE_3F_SWIPE_UP,
    TOUCH_GESTURE_3F_SWIPE_DOWN,
    TOUCH_GESTURE_3F_SWIPE_LEFT,
    TOUCH_GESTURE_3F_SWIPE_RIGHT,
    TOUCH_GESTURE_4F_SWIPE_VERTICAL,
    TOUCH_GESTURE_EDGE_TOP_INWARD,
    TOUCH_GESTURE_EDGE_BOTTOM_INWARD
} ShellTouchGestureType;

/* Pure state-machine tracker (exposed for headless unit testing) */
typedef struct {
    gboolean active;
    int32_t  tracking_id;
    int32_t  start_x;
    int32_t  start_y;
    int32_t  cur_x;
    int32_t  cur_y;
    gboolean has_x;
    gboolean has_y;
} ShellTouchSlot;

typedef struct {
    int32_t        min_x, max_x, res_x;
    int32_t        min_y, max_y, res_y;
    int32_t        current_slot;
    ShellTouchSlot slots[10];
    int32_t        max_fingers_in_gesture;
    gboolean       gesture_triggered;
    gboolean       edge_reveal_triggered;
    double         last_trigger_x_ratio;
} ShellTouchTracker;

void                  shell_touch_tracker_init(ShellTouchTracker *tracker,
                                               int32_t min_x, int32_t max_x, int32_t res_x,
                                               int32_t min_y, int32_t max_y, int32_t res_y);
ShellTouchGestureType shell_touch_tracker_feed_event(ShellTouchTracker *tracker,
                                                     uint16_t type,
                                                     uint16_t code,
                                                     int32_t value);

ShellTouchGestures   *shell_touch_gestures_new(ShellApp *app);
void                  shell_touch_gestures_destroy(ShellTouchGestures *self);
