#define _GNU_SOURCE
#include "touch_gestures.h"
#include "../shell.h"
#include "../config.h"
#include "../panel.h"
#include "../autohide.h"
#include "../compositor/compositor.h"
#include "../launcher/launcher_surface.h"
#include "../osk/osk.h"

#include <glib-unix.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

#define MAX_MT_SLOTS 10
#define SWIPE_3F_THRESHOLD_MM 15.0
#define EDGE_BEZEL_RATIO      0.032
#define EDGE_SWIPE_MM         12.0

struct _ShellTouchGestures {
    ShellApp          *app;
    int                fd;
    guint              watch_id;
    ShellTouchTracker  tracker;
};

void
shell_touch_tracker_init(ShellTouchTracker *tracker,
                         int32_t min_x, int32_t max_x, int32_t res_x,
                         int32_t min_y, int32_t max_y, int32_t res_y)
{
    if (!tracker) return;
    memset(tracker, 0, sizeof(*tracker));
    tracker->min_x = min_x;
    tracker->max_x = (max_x > min_x) ? max_x : 3600;
    tracker->res_x = (res_x > 0) ? res_x : 12;
    tracker->min_y = min_y;
    tracker->max_y = (max_y > min_y) ? max_y : 2256;
    tracker->res_y = (res_y > 0) ? res_y : 12;
}

static void
reanchor_active_slots(ShellTouchTracker *tracker)
{
    for (int i = 0; i < MAX_MT_SLOTS; i++) {
        if (tracker->slots[i].active) {
            tracker->slots[i].start_x = tracker->slots[i].cur_x;
            tracker->slots[i].start_y = tracker->slots[i].cur_y;
        }
    }
}

ShellTouchGestureType
shell_touch_tracker_feed_event(ShellTouchTracker *tracker,
                               uint16_t type,
                               uint16_t code,
                               int32_t value)
{
    if (!tracker) return TOUCH_GESTURE_NONE;

    if (type == EV_ABS) {
        if (code == ABS_MT_SLOT) {
            tracker->current_slot = CLAMP(value, 0, MAX_MT_SLOTS - 1);
        } else {
            ShellTouchSlot *s = &tracker->slots[tracker->current_slot];
            if (code == ABS_MT_TRACKING_ID) {
                if (value >= 0) {
                    s->active = TRUE;
                    s->tracking_id = value;
                    s->has_x = FALSE;
                    s->has_y = FALSE;
                } else {
                    s->active = FALSE;
                    s->has_x = FALSE;
                    s->has_y = FALSE;
                }
            } else if (code == ABS_MT_POSITION_X) {
                s->cur_x = value;
                if (!s->has_x) {
                    s->start_x = value;
                    s->has_x = TRUE;
                }
            } else if (code == ABS_MT_POSITION_Y) {
                s->cur_y = value;
                if (!s->has_y) {
                    s->start_y = value;
                    s->has_y = TRUE;
                }
            }
        }
        return TOUCH_GESTURE_NONE;
    }

    if (type != EV_SYN || code != SYN_REPORT) {
        return TOUCH_GESTURE_NONE;
    }

    /* Count active fingers that have valid initial coordinates */
    int active_count = 0;
    double sum_dx_mm = 0.0;
    double sum_dy_mm = 0.0;
    int first_active_idx = -1;

    for (int i = 0; i < MAX_MT_SLOTS; i++) {
        ShellTouchSlot *s = &tracker->slots[i];
        if (s->active && s->has_x && s->has_y) {
            if (first_active_idx < 0) first_active_idx = i;
            active_count++;
            sum_dx_mm += (double)(s->cur_x - s->start_x) / (double)tracker->res_x;
            sum_dy_mm += (double)(s->cur_y - s->start_y) / (double)tracker->res_y;
        }
    }

    if (active_count == 0) {
        tracker->max_fingers_in_gesture = 0;
        tracker->gesture_triggered = FALSE;
        tracker->edge_reveal_triggered = FALSE;
        return TOUCH_GESTURE_NONE;
    }

    /* When an additional finger touches down (e.g. 1 -> 2 -> 3 fingers),
     * re-anchor all active slot origins so multi-finger displacement is clean. */
    if (active_count > tracker->max_fingers_in_gesture) {
        tracker->max_fingers_in_gesture = active_count;
        reanchor_active_slots(tracker);
        return TOUCH_GESTURE_NONE;
    }

    if (tracker->gesture_triggered) {
        return TOUCH_GESTURE_NONE;
    }

    double avg_dx_mm = sum_dx_mm / (double)active_count;
    double avg_dy_mm = sum_dy_mm / (double)active_count;
    double abs_dx = fabs(avg_dx_mm);
    double abs_dy = fabs(avg_dy_mm);

    /* 3-Finger and 4-Finger Global Touchscreen Swipes (Anywhere on Screen) */
    if (active_count >= 3 && active_count == tracker->max_fingers_in_gesture) {
        if (active_count >= 4 && abs_dy >= SWIPE_3F_THRESHOLD_MM && abs_dy > 1.25 * abs_dx) {
            tracker->gesture_triggered = TRUE;
            return TOUCH_GESTURE_4F_SWIPE_VERTICAL;
        }

        if (active_count == 3) {
            if (abs_dy >= SWIPE_3F_THRESHOLD_MM && abs_dy > 1.25 * abs_dx) {
                tracker->gesture_triggered = TRUE;
                return (avg_dy_mm < 0.0) ? TOUCH_GESTURE_3F_SWIPE_UP : TOUCH_GESTURE_3F_SWIPE_DOWN;
            }
            if (abs_dx >= SWIPE_3F_THRESHOLD_MM && abs_dx > 1.25 * abs_dy) {
                tracker->gesture_triggered = TRUE;
                return (avg_dx_mm < 0.0) ? TOUCH_GESTURE_3F_SWIPE_LEFT : TOUCH_GESTURE_3F_SWIPE_RIGHT;
            }
        }
        return TOUCH_GESTURE_NONE;
    }

    /* 1-Finger Top/Bottom Bezel Edge Swipe (Reveals Autohide Panel) */
    if (active_count == 1 && tracker->max_fingers_in_gesture == 1 && first_active_idx >= 0) {
        ShellTouchSlot *s = &tracker->slots[first_active_idx];
        double width_span  = (double)(tracker->max_x - tracker->min_x);
        double height_span = (double)(tracker->max_y - tracker->min_y);
        if (width_span <= 0.0 || height_span <= 0.0) return TOUCH_GESTURE_NONE;

        double start_y_norm = (double)(s->start_y - tracker->min_y) / height_span;
        double start_x_norm = (double)(s->start_x - tracker->min_x) / width_span;

        if (!tracker->edge_reveal_triggered && abs_dy > 1.3 * abs_dx && abs_dy >= EDGE_SWIPE_MM) {
            if (start_y_norm <= EDGE_BEZEL_RATIO && avg_dy_mm > 0.0) {
                tracker->edge_reveal_triggered = TRUE;
                tracker->last_trigger_x_ratio = start_x_norm;
                return TOUCH_GESTURE_EDGE_TOP_INWARD;
            }
            if (start_y_norm >= (1.0 - EDGE_BEZEL_RATIO) && avg_dy_mm < 0.0) {
                tracker->edge_reveal_triggered = TRUE;
                tracker->last_trigger_x_ratio = start_x_norm;
                return TOUCH_GESTURE_EDGE_BOTTOM_INWARD;
            }
        }
    }

    return TOUCH_GESTURE_NONE;
}

static void
dispatch_touch_gesture(ShellTouchGestures *self, ShellTouchGestureType gesture)
{
    if (!self || !self->app || gesture == TOUCH_GESTURE_NONE) return;

    ShellConfig *cfg = shell_app_get_config(self->app);
    if (!cfg || !cfg->touch_gestures_enabled) return;

    switch (gesture) {
        case TOUCH_GESTURE_3F_SWIPE_UP:
            /* Natural scroll: 3-finger swipe UP moves to next workspace down */
            niri_send_action("{\"Action\":{\"FocusWorkspaceDown\":{}}}");
            break;
        case TOUCH_GESTURE_3F_SWIPE_DOWN:
            /* Natural scroll: 3-finger swipe DOWN moves to previous workspace up */
            niri_send_action("{\"Action\":{\"FocusWorkspaceUp\":{}}}");
            break;
        case TOUCH_GESTURE_3F_SWIPE_LEFT:
            /* Natural scroll: 3-finger swipe LEFT moves to next window column right */
            niri_send_action("{\"Action\":{\"FocusColumnRight\":{}}}");
            break;
        case TOUCH_GESTURE_3F_SWIPE_RIGHT:
            /* Natural scroll: 3-finger swipe RIGHT moves to previous window column left */
            niri_send_action("{\"Action\":{\"FocusColumnLeft\":{}}}");
            break;
        case TOUCH_GESTURE_4F_SWIPE_VERTICAL:
            niri_send_action("{\"Action\":{\"ToggleOverview\":{}}}");
            break;
        case TOUCH_GESTURE_EDGE_TOP_INWARD:
        case TOUCH_GESTURE_EDGE_BOTTOM_INWARD: {
            gboolean is_bottom_panel = (cfg->panel_position == PANEL_POSITION_BOTTOM);
            gboolean is_panel_edge = (is_bottom_panel && gesture == TOUCH_GESTURE_EDGE_BOTTOM_INWARD) ||
                                     (!is_bottom_panel && gesture == TOUCH_GESTURE_EDGE_TOP_INWARD);
            if (is_panel_edge && self->app->panel && self->app->panel->autohide) {
                shell_autohide_touch_reveal(self->app->panel->autohide, cfg->touch_dwell_ms);
            }
            break;
        }
        default:
            break;
    }
}

static gboolean
on_evdev_readable(gint fd, GIOCondition condition, gpointer user_data)
{
    ShellTouchGestures *self = user_data;
    if (!self) return G_SOURCE_REMOVE;

    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        self->watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    struct input_event ev[32];
    ssize_t n = read(fd, ev, sizeof(ev));
    if (n <= 0) {
        return G_SOURCE_CONTINUE;
    }

    size_t count = (size_t)n / sizeof(struct input_event);
    for (size_t i = 0; i < count; i++) {
        ShellTouchGestureType g = shell_touch_tracker_feed_event(
            &self->tracker, ev[i].type, ev[i].code, ev[i].value
        );
        if (g != TOUCH_GESTURE_NONE) {
            dispatch_touch_gesture(self, g);
        }
    }

    return G_SOURCE_CONTINUE;
}

static int
open_direct_touchscreen_evdev(ShellTouchTracker *out_tracker)
{
    for (int i = 0; i < 32; i++) {
        char dev_path[64];
        g_snprintf(dev_path, sizeof(dev_path), "/dev/input/event%d", i);
        int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long props[1] = {0};
        if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0 ||
            !(props[0] & (1UL << INPUT_PROP_DIRECT))) {
            close(fd);
            continue;
        }

        struct input_absinfo abs_slot = {0};
        struct input_absinfo abs_x = {0};
        struct input_absinfo abs_y = {0};

        if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &abs_slot) == 0 && abs_slot.maximum >= 2 &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > abs_x.minimum &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > abs_y.minimum) {
            char name[128] = "Touchscreen";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            g_message("TouchGestures: bound to multi-touch screen '%s' (%s, X=%d..%d res=%d, Y=%d..%d res=%d)",
                      name, dev_path,
                      abs_x.minimum, abs_x.maximum, abs_x.resolution,
                      abs_y.minimum, abs_y.maximum, abs_y.resolution);

            shell_touch_tracker_init(out_tracker,
                                     abs_x.minimum, abs_x.maximum, abs_x.resolution,
                                     abs_y.minimum, abs_y.maximum, abs_y.resolution);
            return fd;
        }

        close(fd);
    }
    return -1;
}

ShellTouchGestures *
shell_touch_gestures_new(ShellApp *app)
{
    ShellTouchGestures *self = g_new0(ShellTouchGestures, 1);
    self->app = app;
    self->fd = open_direct_touchscreen_evdev(&self->tracker);
    if (self->fd >= 0) {
        self->watch_id = g_unix_fd_add(
            self->fd,
            G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
            on_evdev_readable,
            self
        );
    }
    return self;
}

void
shell_touch_gestures_destroy(ShellTouchGestures *self)
{
    if (!self) return;
    if (self->watch_id > 0) {
        g_source_remove(self->watch_id);
        self->watch_id = 0;
    }
    if (self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
    }
    g_free(self);
}
