#include "../src/ipc/socket.h"
#include "../src/shell.h"
#include "../src/launcher/launcher_surface.h"
#include <glib.h>
#include <glib/gstdio.h>

static gint g_lock_calls        = 0;
static gint g_screensaver_calls = 0;
static gint g_reload_cfg_calls  = 0;
static gint g_reload_thm_calls  = 0;
static gint g_brightness_delta  = 0;
static gint g_volume_delta      = 0;
static gint g_mute_calls        = 0;

void
shell_launcher_surface_toggle(ShellLauncherSurface *self G_GNUC_UNUSED)
{
}

void
shell_launcher_surface_show(ShellLauncherSurface *self G_GNUC_UNUSED)
{
}

void
shell_launcher_surface_hide(ShellLauncherSurface *self G_GNUC_UNUSED)
{
}

void
shell_brightness_change(ShellApp *self G_GNUC_UNUSED, gint delta_percent)
{
    g_brightness_delta += delta_percent;
}

void
shell_volume_change(ShellApp *self G_GNUC_UNUSED, gint delta_percent)
{
    g_volume_delta += delta_percent;
}

void
shell_volume_toggle_mute(ShellApp *self G_GNUC_UNUSED)
{
    g_mute_calls++;
}

void
shell_app_lock_session(ShellApp *self G_GNUC_UNUSED)
{
    g_lock_calls++;
}

void
shell_app_start_screensaver(ShellApp *self G_GNUC_UNUSED)
{
    g_screensaver_calls++;
}

void
shell_app_reload_config(ShellApp *self G_GNUC_UNUSED)
{
    g_reload_cfg_calls++;
}

void
shell_app_reload_theme(ShellApp *self G_GNUC_UNUSED)
{
    g_reload_thm_calls++;
}

static void
drain_main_context(void)
{
    for (int i = 0; i < 20; i++) {
        g_usleep(2000);
        while (g_main_context_pending(NULL)) {
            g_main_context_iteration(NULL, FALSE);
        }
    }
}

static void
test_ipc_socket_commands(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-test-ipc-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    g_setenv("XDG_RUNTIME_DIR", tmpdir, TRUE);

    /* When socket is not running, shell_ipc_send_command must return FALSE */
    g_assert_false(shell_ipc_send_command("lock"));

    ShellApp dummy_app = { 0 };
    ShellIPCSocket *ipc = shell_ipc_socket_new(&dummy_app);
    g_assert_nonnull(ipc);

    g_assert_true(shell_ipc_send_command("lock"));
    drain_main_context();
    g_assert_cmpint(g_lock_calls, ==, 1);

    g_assert_true(shell_ipc_send_command("screensaver"));
    drain_main_context();
    g_assert_cmpint(g_screensaver_calls, ==, 1);

    g_assert_true(shell_ipc_send_command("reload-config"));
    drain_main_context();
    g_assert_cmpint(g_reload_cfg_calls, ==, 1);

    g_assert_true(shell_ipc_send_command("reload-theme"));
    drain_main_context();
    g_assert_cmpint(g_reload_thm_calls, ==, 1);

    g_assert_true(shell_ipc_send_command("brightness-up"));
    g_assert_true(shell_ipc_send_command("volume-down"));
    g_assert_true(shell_ipc_send_command("volume-mute"));
    drain_main_context();
    g_assert_cmpint(g_brightness_delta, ==, 5);
    g_assert_cmpint(g_volume_delta, ==, -5);
    g_assert_cmpint(g_mute_calls, ==, 1);

    shell_ipc_socket_destroy(ipc);
    g_rmdir(tmpdir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ipc/socket-commands-dispatch", test_ipc_socket_commands);

    return g_test_run();
}
