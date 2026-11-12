/* comp_touch.c — pure-C touchscreen multi-finger & bezel gesture tracker
 * and touchpad 3-finger swipe state machine (spec §3.15).
 */
#include "comp_touch.h"

#include <math.h>
#include <string.h>

/* Default physical dimensions matching 14" 16:10 panel (300 mm × 188 mm) */
#define DEFAULT_TOUCH_WIDTH_MM  300.0
#define DEFAULT_TOUCH_HEIGHT_MM 188.0

static void
reanchor_active_slots(CompTouchTracker *t)
{
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++) {
        if (t->slots[i].active) {
            t->slots[i].start_x = t->slots[i].cur_x;
            t->slots[i].start_y = t->slots[i].cur_y;
        }
    }
}

static void
transform_delta_mm(CompTransform tf, double raw_dx_mm, double raw_dy_mm,
                   double *out_dx_mm, double *out_dy_mm)
{
    switch (tf) {
    case COMP_TRANSFORM_90:
    case COMP_TRANSFORM_FLIPPED_90:
        *out_dx_mm = -raw_dy_mm;
        *out_dy_mm =  raw_dx_mm;
        break;
    case COMP_TRANSFORM_180:
    case COMP_TRANSFORM_FLIPPED_180:
        *out_dx_mm = -raw_dx_mm;
        *out_dy_mm = -raw_dy_mm;
        break;
    case COMP_TRANSFORM_270:
    case COMP_TRANSFORM_FLIPPED_270:
        *out_dx_mm =  raw_dy_mm;
        *out_dy_mm = -raw_dx_mm;
        break;
    case COMP_TRANSFORM_FLIPPED:
        *out_dx_mm = -raw_dx_mm;
        *out_dy_mm =  raw_dy_mm;
        break;
    case COMP_TRANSFORM_NORMAL:
    default:
        *out_dx_mm = raw_dx_mm;
        *out_dy_mm = raw_dy_mm;
        break;
    }
}

static void
transform_norm_point(CompTransform tf, double raw_x, double raw_y,
                     double *out_x, double *out_y)
{
    switch (tf) {
    case COMP_TRANSFORM_90:
    case COMP_TRANSFORM_FLIPPED_90:
        *out_x = 1.0 - raw_y;
        *out_y = raw_x;
        break;
    case COMP_TRANSFORM_180:
    case COMP_TRANSFORM_FLIPPED_180:
        *out_x = 1.0 - raw_x;
        *out_y = 1.0 - raw_y;
        break;
    case COMP_TRANSFORM_270:
    case COMP_TRANSFORM_FLIPPED_270:
        *out_x = raw_y;
        *out_y = 1.0 - raw_x;
        break;
    case COMP_TRANSFORM_FLIPPED:
        *out_x = 1.0 - raw_x;
        *out_y = raw_y;
        break;
    case COMP_TRANSFORM_NORMAL:
    default:
        *out_x = raw_x;
        *out_y = raw_y;
        break;
    }
}

void
comp_touch_tracker_init(CompTouchTracker *t, double width_mm, double height_mm)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    t->width_mm  = (width_mm > 1.0)  ? width_mm  : DEFAULT_TOUCH_WIDTH_MM;
    t->height_mm = (height_mm > 1.0) ? height_mm : DEFAULT_TOUCH_HEIGHT_MM;
    t->transform = COMP_TRANSFORM_NORMAL;
}

void
comp_touch_tracker_set_geometry(CompTouchTracker *t, double width_mm, double height_mm,
                                CompTransform transform)
{
    if (!t)
        return;
    if (width_mm > 1.0)
        t->width_mm = width_mm;
    if (height_mm > 1.0)
        t->height_mm = height_mm;
    t->transform = transform;
}

int
comp_touch_tracker_active_count(const CompTouchTracker *t)
{
    if (!t)
        return 0;
    int count = 0;
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++) {
        if (t->slots[i].active)
            count++;
    }
    return count;
}

void
comp_touch_tracker_down(CompTouchTracker *t, int32_t touch_id, double x_norm, double y_norm)
{
    if (!t || !isfinite(x_norm) || !isfinite(y_norm))
        return;

    int slot_idx = -1;
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++) {
        if (t->slots[i].active && t->slots[i].touch_id == touch_id) {
            slot_idx = i;
            break;
        }
        if (!t->slots[i].active && slot_idx < 0)
            slot_idx = i;
    }
    if (slot_idx < 0)
        return;

    if (x_norm < 0.0) x_norm = 0.0;
    if (x_norm > 1.0) x_norm = 1.0;
    if (y_norm < 0.0) y_norm = 0.0;
    if (y_norm > 1.0) y_norm = 1.0;

    CompTouchSlot *s = &t->slots[slot_idx];
    s->active   = true;
    s->touch_id = touch_id;
    s->start_x  = x_norm;
    s->start_y  = y_norm;
    s->cur_x    = x_norm;
    s->cur_y    = y_norm;

    int active = comp_touch_tracker_active_count(t);
    /* Spec §3.15.1: whenever the active finger count increases (1 -> 2 -> 3),
     * re-anchor all active slot origins to their current positions. */
    if (active > t->max_fingers_in_gesture) {
        t->max_fingers_in_gesture = active;
        reanchor_active_slots(t);
    }
}

CompTouchGestureEvent
comp_touch_tracker_motion(CompTouchTracker *t, int32_t touch_id, double x_norm, double y_norm)
{
    if (!t || !isfinite(x_norm) || !isfinite(y_norm))
        return COMP_TOUCH_EV_NONE;

    if (x_norm < 0.0) x_norm = 0.0;
    if (x_norm > 1.0) x_norm = 1.0;
    if (y_norm < 0.0) y_norm = 0.0;
    if (y_norm > 1.0) y_norm = 1.0;

    bool found = false;
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++) {
        if (t->slots[i].active && t->slots[i].touch_id == touch_id) {
            t->slots[i].cur_x = x_norm;
            t->slots[i].cur_y = y_norm;
            found = true;
            break;
        }
    }
    if (!found || t->gesture_triggered)
        return COMP_TOUCH_EV_NONE;

    int active = 0;
    int first_idx = -1;
    double sum_raw_dx_mm = 0.0;
    double sum_raw_dy_mm = 0.0;

    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++) {
        const CompTouchSlot *s = &t->slots[i];
        if (s->active) {
            if (first_idx < 0)
                first_idx = i;
            active++;
            sum_raw_dx_mm += (s->cur_x - s->start_x) * t->width_mm;
            sum_raw_dy_mm += (s->cur_y - s->start_y) * t->height_mm;
        }
    }
    if (active == 0)
        return COMP_TOUCH_EV_NONE;

    double raw_dx_mm = sum_raw_dx_mm / (double)active;
    double raw_dy_mm = sum_raw_dy_mm / (double)active;
    double avg_dx_mm = 0.0, avg_dy_mm = 0.0;
    transform_delta_mm(t->transform, raw_dx_mm, raw_dy_mm, &avg_dx_mm, &avg_dy_mm);

    double abs_dx = fabs(avg_dx_mm);
    double abs_dy = fabs(avg_dy_mm);

    /* Spec §3.15.1: 3-Finger Global Swipes (SWIPE_3F_THRESHOLD_MM = 15.0) */
    if (active == 3 && t->max_fingers_in_gesture == 3) {
        if (abs_dy >= COMP_SWIPE_3F_THRESHOLD_MM && abs_dy > 1.25 * abs_dx) {
            t->gesture_triggered = true;
            return (avg_dy_mm < 0.0) ? COMP_TOUCH_EV_3F_SWIPE_UP : COMP_TOUCH_EV_3F_SWIPE_DOWN;
        }
        if (abs_dx >= COMP_SWIPE_3F_THRESHOLD_MM && abs_dx > 1.25 * abs_dy) {
            t->gesture_triggered = true;
            return (avg_dx_mm < 0.0) ? COMP_TOUCH_EV_3F_SWIPE_LEFT : COMP_TOUCH_EV_3F_SWIPE_RIGHT;
        }
        return COMP_TOUCH_EV_NONE;
    }

    /* Spec §3.15.1: 1-Finger Top/Bottom Bezel Swipe (EDGE_BEZEL_RATIO = 0.032, EDGE_SWIPE_MM = 12.0) */
    if (active == 1 && t->max_fingers_in_gesture == 1 && first_idx >= 0) {
        const CompTouchSlot *s = &t->slots[first_idx];
        double sx_norm = 0.0, sy_norm = 0.0;
        transform_norm_point(t->transform, s->start_x, s->start_y, &sx_norm, &sy_norm);

        if (abs_dy >= COMP_EDGE_SWIPE_MM && abs_dy > 1.3 * abs_dx) {
            if (sy_norm <= COMP_EDGE_BEZEL_RATIO && avg_dy_mm > 0.0) {
                t->gesture_triggered = true;
                t->last_x_ratio = sx_norm;
                return COMP_TOUCH_EV_BEZEL_TOP;
            }
            if (sy_norm >= (1.0 - COMP_EDGE_BEZEL_RATIO) && avg_dy_mm < 0.0) {
                t->gesture_triggered = true;
                t->last_x_ratio = sx_norm;
                return COMP_TOUCH_EV_BEZEL_BOTTOM;
            }
        }
    }

    return COMP_TOUCH_EV_NONE;
}

void
comp_touch_tracker_up(CompTouchTracker *t, int32_t touch_id)
{
    if (!t)
        return;
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++) {
        if (t->slots[i].active && t->slots[i].touch_id == touch_id) {
            t->slots[i].active = false;
            break;
        }
    }
    if (comp_touch_tracker_active_count(t) == 0) {
        t->max_fingers_in_gesture = 0;
        t->gesture_triggered      = false;
    }
}

void
comp_touch_tracker_cancel(CompTouchTracker *t)
{
    if (!t)
        return;
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++)
        t->slots[i].active = false;
    t->max_fingers_in_gesture = 0;
    t->gesture_triggered      = false;
}

/* ───────────────────────── Touchpad 3-Finger Swipes (§3.15.2) ───────────────────────── */

void
comp_touchpad_swipe_begin(CompTouchpadSwipe *s, uint32_t fingers)
{
    if (!s)
        return;
    s->active    = true;
    s->triggered = false;
    s->fingers   = fingers;
    s->dx_accum  = 0.0;
    s->dy_accum  = 0.0;
}

bool
comp_touchpad_swipe_update(CompTouchpadSwipe *s, uint32_t fingers,
                           double dx, double dy, bool natural_scroll,
                           CompAction *out_action)
{
    if (!s || !s->active || s->triggered || fingers != 3 || s->fingers != 3 ||
        !out_action || !isfinite(dx) || !isfinite(dy))
        return false;

    s->dx_accum += dx;
    s->dy_accum += dy;

    double abs_dx = fabs(s->dx_accum);
    double abs_dy = fabs(s->dy_accum);

    if (abs_dx >= COMP_TOUCHPAD_SWIPE_THRESH && abs_dx > 1.25 * abs_dy) {
        s->triggered = true;
        bool go_right = natural_scroll ? (s->dx_accum < 0.0) : (s->dx_accum > 0.0);
        *out_action = go_right ? COMP_ACT_FOCUS_COLUMN_RIGHT : COMP_ACT_FOCUS_COLUMN_LEFT;
        return true;
    }
    if (abs_dy >= COMP_TOUCHPAD_SWIPE_THRESH && abs_dy > 1.25 * abs_dx) {
        s->triggered = true;
        bool go_down = natural_scroll ? (s->dy_accum < 0.0) : (s->dy_accum > 0.0);
        *out_action = go_down ? COMP_ACT_FOCUS_WORKSPACE_DOWN : COMP_ACT_FOCUS_WORKSPACE_UP;
        return true;
    }
    return false;
}

void
comp_touchpad_swipe_end(CompTouchpadSwipe *s)
{
    if (!s)
        return;
    s->active    = false;
    s->triggered = false;
    s->fingers   = 0;
    s->dx_accum  = 0.0;
    s->dy_accum  = 0.0;
}
