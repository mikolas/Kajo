#include <glib.h>
#include "../src/autohide.h"
#include "../src/config.h"
#include "../src/panel.h"

GtkWindow *
shell_panel_get_window(ShellPanel *panel)
{
    return panel ? panel->window : NULL;
}

GtkWidget *
shell_panel_get_container(ShellPanel *panel)
{
    return panel ? panel->container : NULL;
}

static void
test_autohide_initial_disabled(void)
{
    ShellConfig *cfg = shell_config_get_default();
    cfg->autohide_enabled = FALSE;

    ShellAutohide *ah = shell_autohide_new(NULL, cfg);
    g_assert_nonnull(ah);
    g_assert_false(ah->enabled);
    g_assert_cmpint(ah->state, ==, AUTOHIDE_VISIBLE);
    g_assert_cmpuint(ah->hide_timeout_id, ==, 0);

    shell_autohide_destroy(ah);
    shell_config_destroy(cfg);
}

static void
test_autohide_enter_leave_cycle(void)
{
    ShellConfig *cfg = shell_config_get_default();
    cfg->autohide_enabled = TRUE;
    cfg->autohide_hide_delay = 15;
    cfg->autohide_reveal_duration = 15;

    ShellAutohide *ah = shell_autohide_new(NULL, cfg);
    g_assert_nonnull(ah);
    g_assert_true(ah->enabled);
    g_assert_cmpint(ah->state, ==, AUTOHIDE_HIDDEN);

    /* Pointer enters edge strip -> reveals */
    shell_autohide_enter(ah);
    g_assert_true(ah->pointer_inside);
    g_assert_cmpint(ah->state, ==, AUTOHIDE_VISIBLE);
    g_assert_cmpuint(ah->hide_timeout_id, ==, 0);

    /* Pointer leaves panel -> schedules hide timeout */
    shell_autohide_leave(ah);
    g_assert_false(ah->pointer_inside);
    g_assert_cmpuint(ah->hide_timeout_id, !=, 0);

    /* Let hide timeout fire */
    gint64 deadline = g_get_monotonic_time() + 500 * 1000;
    while (ah->state != AUTOHIDE_HIDDEN && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, TRUE);
    }

    g_assert_cmpint(ah->state, ==, AUTOHIDE_HIDDEN);
    g_assert_cmpuint(ah->hide_timeout_id, ==, 0);

    shell_autohide_destroy(ah);
    shell_config_destroy(cfg);
}

static void
test_autohide_popover_unlock_recovers_stuck_state(void)
{
    ShellConfig *cfg = shell_config_get_default();
    cfg->autohide_enabled = TRUE;
    cfg->autohide_hide_delay = 15;

    ShellAutohide *ah = shell_autohide_new(NULL, cfg);
    g_assert_cmpint(ah->state, ==, AUTOHIDE_HIDDEN);

    /* Pointer enters panel and opens a popover */
    shell_autohide_enter(ah);
    g_assert_true(ah->pointer_inside);
    g_assert_cmpint(ah->state, ==, AUTOHIDE_VISIBLE);

    shell_autohide_lock(ah);
    g_assert_cmpint(ah->popover_count, ==, 1);

    /* Simulate Wayland popup dismissal consuming the leave event:
     * unlock is called while pointer_inside was still TRUE */
    shell_autohide_unlock(ah);
    g_assert_cmpint(ah->popover_count, ==, 0);
    g_assert_false(ah->pointer_inside);
    g_assert_cmpuint(ah->hide_timeout_id, !=, 0);

    gint64 deadline = g_get_monotonic_time() + 500 * 1000;
    while (ah->state != AUTOHIDE_HIDDEN && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, TRUE);
    }

    g_assert_cmpint(ah->state, ==, AUTOHIDE_HIDDEN);

    shell_autohide_destroy(ah);
    shell_config_destroy(cfg);
}

static void
test_autohide_reload_config_live(void)
{
    ShellConfig *cfg = shell_config_get_default();
    cfg->autohide_enabled = FALSE;
    cfg->autohide_hide_delay = 15;

    ShellAutohide *ah = shell_autohide_new(NULL, cfg);
    g_assert_cmpint(ah->state, ==, AUTOHIDE_VISIBLE);

    /* Enable autohide live via reload_config */
    cfg->autohide_enabled = TRUE;
    shell_autohide_reload_config(ah, cfg);
    g_assert_true(ah->enabled);
    g_assert_cmpuint(ah->hide_timeout_id, !=, 0);

    gint64 deadline = g_get_monotonic_time() + 500 * 1000;
    while (ah->state != AUTOHIDE_HIDDEN && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, TRUE);
    }
    g_assert_cmpint(ah->state, ==, AUTOHIDE_HIDDEN);

    /* Disable autohide live via reload_config */
    cfg->autohide_enabled = FALSE;
    shell_autohide_reload_config(ah, cfg);
    g_assert_false(ah->enabled);
    g_assert_cmpint(ah->state, ==, AUTOHIDE_VISIBLE);
    g_assert_cmpuint(ah->hide_timeout_id, ==, 0);

    shell_autohide_destroy(ah);
    shell_config_destroy(cfg);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/autohide/initial-disabled", test_autohide_initial_disabled);
    g_test_add_func("/autohide/enter-leave-cycle", test_autohide_enter_leave_cycle);
    g_test_add_func("/autohide/popover-unlock-recovery", test_autohide_popover_unlock_recovers_stuck_state);
    g_test_add_func("/autohide/reload-config-live", test_autohide_reload_config_live);

    return g_test_run();
}
