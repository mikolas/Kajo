#include "../src/lock/idle_manager.h"
#include "../src/lock/screensaver.h"
#include "../src/lock/session_lock.h"
#include "../src/shell.h"
#include "../src/config.h"
#include <glib.h>

/* Lightweight test stubs for symbols referenced by idle_manager.c */
ShellConfig *
shell_app_get_config(ShellApp *self)
{
    return self ? self->config : NULL;
}

gboolean
shell_session_lock_is_locked(ShellSessionLock *self G_GNUC_UNUSED)
{
    return FALSE;
}

void
shell_session_lock_activate(ShellSessionLock *self G_GNUC_UNUSED)
{
}

void
shell_screensaver_start(ShellScreensaver *self G_GNUC_UNUSED)
{
}

void
shell_screensaver_stop(ShellScreensaver *self G_GNUC_UNUSED)
{
}

gboolean
shell_screensaver_is_active(ShellScreensaver *self G_GNUC_UNUSED)
{
    return FALSE;
}

void
shell_screensaver_set_dpms(gboolean power_on G_GNUC_UNUSED)
{
}

typedef struct {
    gint     call_count;
    gboolean last_value;
} ListenerState;

static void
on_inhibit_changed_cb(gboolean inhibited, gpointer user_data)
{
    ListenerState *st = user_data;
    st->call_count++;
    st->last_value = inhibited;
}

static void
test_idle_manager_inhibit_and_listeners(void)
{
    ShellIdleManager *mgr = shell_idle_manager_new(NULL);
    g_assert_nonnull(mgr);
    g_assert_false(shell_idle_manager_get_inhibited(mgr));

    ListenerState widget_listener = { 0, FALSE };
    ListenerState cc_listener     = { 0, FALSE };

    shell_idle_manager_add_listener(mgr, on_inhibit_changed_cb, &widget_listener);
    shell_idle_manager_add_listener(mgr, on_inhibit_changed_cb, &cc_listener);

    /* Enable Keep Awake -> both listeners must fire once with TRUE */
    shell_idle_manager_set_inhibited(mgr, TRUE);
    g_assert_true(shell_idle_manager_get_inhibited(mgr));
    g_assert_cmpint(widget_listener.call_count, ==, 1);
    g_assert_true(widget_listener.last_value);
    g_assert_cmpint(cc_listener.call_count, ==, 1);
    g_assert_true(cc_listener.last_value);

    /* Idempotent call -> listeners must not fire again if state is unchanged */
    shell_idle_manager_set_inhibited(mgr, TRUE);
    g_assert_cmpint(widget_listener.call_count, ==, 1);
    g_assert_cmpint(cc_listener.call_count, ==, 1);

    /* Remove widget_listener and disable Keep Awake -> only cc_listener fires */
    shell_idle_manager_remove_listener(mgr, on_inhibit_changed_cb, &widget_listener);
    shell_idle_manager_set_inhibited(mgr, FALSE);

    g_assert_false(shell_idle_manager_get_inhibited(mgr));
    g_assert_cmpint(widget_listener.call_count, ==, 1);
    g_assert_true(widget_listener.last_value);
    g_assert_cmpint(cc_listener.call_count, ==, 2);
    g_assert_false(cc_listener.last_value);

    shell_idle_manager_remove_listener(mgr, on_inhibit_changed_cb, &cc_listener);
    shell_idle_manager_destroy(mgr);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/idle-manager/inhibit-and-listeners", test_idle_manager_inhibit_and_listeners);

    return g_test_run();
}
