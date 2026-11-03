#include "../src/hardware/touch_gestures.h"
#include "../src/shell.h"
#include "../src/config.h"
#include "../src/autohide.h"
#include "../src/compositor/compositor.h"
#include <linux/input-event-codes.h>
#include <glib.h>

ShellConfig *
shell_app_get_config(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

void
niri_send_action(const char *json_action G_GNUC_UNUSED)
{
}

void
shell_autohide_touch_reveal(ShellAutohide *self G_GNUC_UNUSED, gint dwell_ms G_GNUC_UNUSED)
{
}

static void
put_finger_down(ShellTouchTracker *t, int slot, int id, int x, int y)
{
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_SLOT, slot);
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_TRACKING_ID, id);
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_POSITION_X, x);
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_POSITION_Y, y);
}

static void
move_finger(ShellTouchTracker *t, int slot, int x, int y)
{
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_SLOT, slot);
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_POSITION_X, x);
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_POSITION_Y, y);
}

static void
lift_finger(ShellTouchTracker *t, int slot)
{
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_SLOT, slot);
    shell_touch_tracker_feed_event(t, EV_ABS, ABS_MT_TRACKING_ID, -1);
}

static void
test_3f_vertical_and_horizontal_swipes(void)
{
    ShellTouchTracker t;
    shell_touch_tracker_init(&t, 0, 3600, 12, 0, 2256, 12);

    /* 3-finger swipe UP (dy = -240 units = -20mm) -> TOUCH_GESTURE_3F_SWIPE_UP */
    put_finger_down(&t, 0, 10, 1000, 1200);
    put_finger_down(&t, 1, 11, 1200, 1200);
    put_finger_down(&t, 2, 12, 1400, 1200);
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_NONE);

    move_finger(&t, 0, 1000, 960);
    move_finger(&t, 1, 1200, 960);
    move_finger(&t, 2, 1400, 960);
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_3F_SWIPE_UP);

    /* Lift all fingers */
    lift_finger(&t, 0);
    lift_finger(&t, 1);
    lift_finger(&t, 2);
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_NONE);

    /* 3-finger swipe LEFT (dx = -240 units = -20mm) -> TOUCH_GESTURE_3F_SWIPE_LEFT */
    put_finger_down(&t, 0, 20, 1800, 1000);
    put_finger_down(&t, 1, 21, 1900, 1000);
    put_finger_down(&t, 2, 22, 2000, 1000);
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_NONE);

    move_finger(&t, 0, 1560, 1000);
    move_finger(&t, 1, 1660, 1000);
    move_finger(&t, 2, 1760, 1000);
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_3F_SWIPE_LEFT);

    lift_finger(&t, 0);
    lift_finger(&t, 1);
    lift_finger(&t, 2);
    shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0);
}

static void
test_4f_overview_and_1f_edge_swipe(void)
{
    ShellTouchTracker t;
    shell_touch_tracker_init(&t, 0, 3600, 12, 0, 2256, 12);

    /* 4-finger swipe UP -> TOUCH_GESTURE_4F_SWIPE_VERTICAL */
    for (int i = 0; i < 4; i++) {
        put_finger_down(&t, i, 30 + i, 1000 + i * 100, 1200);
    }
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_NONE);

    for (int i = 0; i < 4; i++) {
        move_finger(&t, i, 1000 + i * 100, 960);
    }
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_4F_SWIPE_VERTICAL);

    for (int i = 0; i < 4; i++) lift_finger(&t, i);
    shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0);

    /* 1-finger bottom bezel inward swipe -> TOUCH_GESTURE_EDGE_BOTTOM_INWARD */
    put_finger_down(&t, 0, 50, 1800, 2240);
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_NONE);
    move_finger(&t, 0, 1800, 2060);
    g_assert_cmpint(shell_touch_tracker_feed_event(&t, EV_SYN, SYN_REPORT, 0), ==, TOUCH_GESTURE_EDGE_BOTTOM_INWARD);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/touch-gestures/3f-vertical-and-horizontal", test_3f_vertical_and_horizontal_swipes);
    g_test_add_func("/touch-gestures/4f-overview-and-1f-edge", test_4f_overview_and_1f_edge_swipe);

    return g_test_run();
}
