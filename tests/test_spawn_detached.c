#define _POSIX_C_SOURCE 200809L

#include "../src/common/spawn_detached.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static gboolean
wait_for_file_nonempty(const gchar *path, int timeout_ms)
{
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        g_autofree gchar *contents = NULL;
        gsize len = 0;
        if (g_file_get_contents(path, &contents, &len, NULL) && len > 0)
            return TRUE;
        g_usleep(10000);
    }
    return FALSE;
}

static void
test_invalid_and_missing_commands(void)
{
    g_autoptr(GError) err = NULL;
    g_assert_false(kajo_spawn_detached_command_line(NULL, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(kajo_spawn_detached_command_line("", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(kajo_spawn_detached_command_line("echo \"unterminated", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(kajo_spawn_detached_command_line("kajo-definitely-no-such-binary-9f8e7d", &err));
    g_assert_nonnull(err);
    g_assert_cmpint(err->code, ==, G_SPAWN_ERROR_NOENT);
    g_clear_error(&err);

    const gchar *bad_dir_argv[] = { "/tmp", NULL };
    g_assert_false(kajo_spawn_detached_argv(NULL, bad_dir_argv, NULL, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
}

static void
test_setsid_sigpipe_and_cwd(void)
{
    g_setenv("KAJO_SPAWN_NO_SYSTEMD", "1", TRUE);

    /* Deliberately ignore SIGPIPE in parent to confirm child resets SIG_DFL. */
    void (*prev_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);

    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-spawn-test-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    g_autofree gchar *out_file = g_build_filename(tmpdir, "info.txt", NULL);
    g_autofree gchar *tmp_file = g_build_filename(tmpdir, "info.txt.tmp", NULL);

    g_autofree gchar *script = g_strdup_printf(
        "pid=$$; "
        "read _ _ _ _ pgid sid _ < /proc/$$/stat; "
        "sigign=$(awk '/^SigIgn:/{print $2}' /proc/$$/status); "
        "printf '%%s %%s %%s %%s %%s\\n' \"$pid\" \"$pgid\" \"$sid\" \"$sigign\" \"$(pwd)\" > '%s' && "
        "mv '%s' '%s'",
        tmp_file, tmp_file, out_file);

    const gchar *argv[] = { "/bin/sh", "-c", script, NULL };
    GPid child_pid = 0;
    g_autoptr(GError) err = NULL;
    gboolean ok = kajo_spawn_detached_argv(tmpdir, argv, &child_pid, &err);
    signal(SIGPIPE, prev_sigpipe);

    g_assert_no_error(err);
    g_assert_true(ok);
    g_assert_cmpint((int)child_pid, >, 0);

    int status = 0;
    pid_t waited = waitpid(child_pid, &status, 0);
    g_assert_cmpint((int)waited, ==, (int)child_pid);
    g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    g_spawn_close_pid(child_pid);

    g_assert_true(wait_for_file_nonempty(out_file, 2000));
    g_autofree gchar *contents = NULL;
    g_assert_true(g_file_get_contents(out_file, &contents, NULL, NULL));
    g_strstrip(contents);

    long pid_val = 0, pgid_val = 0, sid_val = 0;
    unsigned long long sigign_val = 0;
    char cwd_buf[512] = { 0 };
    int matched = sscanf(contents, "%ld %ld %ld %llx %511[^\n]",
                         &pid_val, &pgid_val, &sid_val, &sigign_val, cwd_buf);
    g_assert_cmpint(matched, ==, 5);

    pid_t my_pgid = getpgid(0);
    pid_t my_sid = getsid(0);
    g_assert_cmpint((int)pid_val, ==, (int)child_pid);
    g_assert_cmpint((int)pgid_val, ==, (int)child_pid);
    g_assert_cmpint((int)sid_val, ==, (int)child_pid);
    g_assert_cmpint((int)pgid_val, !=, (int)my_pgid);
    g_assert_cmpint((int)sid_val, !=, (int)my_sid);
    g_assert_cmpstr(cwd_buf, ==, tmpdir);

    /* Bit (SIGPIPE - 1) in SigIgn must be 0 (SIG_DFL restored). */
    unsigned long long sigpipe_bit = 1ULL << (SIGPIPE - 1);
    g_assert_cmpuint(sigign_val & sigpipe_bit, ==, 0);

    g_unlink(out_file);
    g_rmdir(tmpdir);
}

static void
test_detached_child_survives_parent_pgid_sigterm(void)
{
    g_setenv("KAJO_SPAWN_NO_SYSTEMD", "1", TRUE);

    int pipefd[2];
    g_assert_cmpint(pipe(pipefd), ==, 0);

    pid_t helper = fork();
    g_assert_cmpint((int)helper, >=, 0);

    if (helper == 0) {
        close(pipefd[0]);
        /* Give helper its own process group simulating kajo's PGID. */
        (void)setpgid(0, 0);
        const gchar *argv[] = { "/bin/sleep", "30", NULL };
        GPid grandchild = 0;
        if (kajo_spawn_detached_argv(NULL, argv, &grandchild, NULL)) {
            pid_t gp = (pid_t)grandchild;
            ssize_t wr = write(pipefd[1], &gp, sizeof(gp));
            (void)wr;
        }
        close(pipefd[1]);
        /* Wait to be killed by SIGTERM to our process group. */
        pause();
        _exit(0);
    }

    close(pipefd[1]);
    pid_t grandchild = 0;
    ssize_t n = read(pipefd[0], &grandchild, sizeof(grandchild));
    close(pipefd[0]);
    g_assert_cmpint((int)n, ==, (int)sizeof(grandchild));
    g_assert_cmpint((int)grandchild, >, 0);

    /* Wait briefly for grandchild to finish setsid() + exec(). */
    g_usleep(50000);

    /* Terminate the entire helper process group (exact failure mode that
     * previously killed kitty when kajo's PGID was signalled). */
    g_assert_cmpint(kill(-helper, SIGTERM), ==, 0);

    int hstatus = 0;
    g_assert_cmpint((int)waitpid(helper, &hstatus, 0), ==, (int)helper);
    g_assert_true(WIFSIGNALED(hstatus) && WTERMSIG(hstatus) == SIGTERM);

    /* Grandchild must still be alive in its own session/PGID. */
    g_usleep(50000);
    g_assert_cmpint(kill(grandchild, 0), ==, 0);
    g_assert_cmpint((int)getpgid(grandchild), ==, (int)grandchild);
    g_assert_cmpint((int)getpgid(grandchild), !=, (int)helper);

    /* Clean up the test sleeper. */
    kill(grandchild, SIGKILL);
}

static void
test_systemd_scope_when_available(void)
{
    g_unsetenv("KAJO_SPAWN_NO_SYSTEMD");

    const gchar *runtime_dir = g_get_user_runtime_dir();
    g_autofree gchar *priv_sock = runtime_dir
        ? g_build_filename(runtime_dir, "systemd", "private", NULL)
        : NULL;
    g_autofree gchar *sd_run = g_find_program_in_path("systemd-run");
    if (!g_file_test("/run/systemd/system", G_FILE_TEST_IS_DIR) ||
        !priv_sock || !g_file_test(priv_sock, G_FILE_TEST_EXISTS) ||
        !sd_run) {
        g_test_skip("user systemd instance not reachable in this environment");
        return;
    }

    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-spawn-sd-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    g_autofree gchar *out_file = g_build_filename(tmpdir, "cgroup.txt", NULL);
    g_autofree gchar *tmp_file = g_build_filename(tmpdir, "cgroup.txt.tmp", NULL);
    g_autofree gchar *cmd = g_strdup_printf(
        "/bin/sh -c \"cat /proc/self/cgroup > '%s' && mv '%s' '%s'\"",
        tmp_file, tmp_file, out_file);

    g_autoptr(GError) err = NULL;
    g_assert_true(kajo_spawn_detached_command_line(cmd, &err));
    g_assert_no_error(err);

    g_assert_true(wait_for_file_nonempty(out_file, 3000));
    g_autofree gchar *cg = NULL;
    g_assert_true(g_file_get_contents(out_file, &cg, NULL, NULL));
    g_assert_nonnull(strstr(cg, "/app.slice/"));

    g_unlink(out_file);
    g_rmdir(tmpdir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/common/spawn-detached/invalid-and-missing", test_invalid_and_missing_commands);
    g_test_add_func("/common/spawn-detached/setsid-sigpipe-cwd", test_setsid_sigpipe_and_cwd);
    g_test_add_func("/common/spawn-detached/survives-parent-pgid-sigterm", test_detached_child_survives_parent_pgid_sigterm);
    g_test_add_func("/common/spawn-detached/systemd-scope-when-available", test_systemd_scope_when_available);
    return g_test_run();
}
