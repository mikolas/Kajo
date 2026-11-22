#define _POSIX_C_SOURCE 200809L

#include "spawn_detached.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

static void
detached_child_setup(gpointer user_data G_GNUC_UNUSED)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    (void)setsid();
}

static gboolean
target_executable_exists(const gchar *prog)
{
    if (!prog || prog[0] == '\0')
        return FALSE;

    if (strchr(prog, '/') != NULL) {
        return g_file_test(prog, G_FILE_TEST_IS_EXECUTABLE) &&
               !g_file_test(prog, G_FILE_TEST_IS_DIR);
    }

    g_autofree gchar *found = g_find_program_in_path(prog);
    return found != NULL;
}

static gchar *
find_usable_systemd_run(void)
{
    const gchar *disable = g_getenv("KAJO_SPAWN_NO_SYSTEMD");
    if (disable && disable[0] != '\0' && g_strcmp0(disable, "0") != 0)
        return NULL;

    if (!g_file_test("/run/systemd/system", G_FILE_TEST_IS_DIR))
        return NULL;

    const gchar *runtime_dir = g_get_user_runtime_dir();
    if (!runtime_dir || runtime_dir[0] == '\0')
        return NULL;

    g_autofree gchar *priv_sock = g_build_filename(runtime_dir, "systemd", "private", NULL);
    if (!g_file_test(priv_sock, G_FILE_TEST_EXISTS))
        return NULL;

    return g_find_program_in_path("systemd-run");
}

gboolean
kajo_spawn_detached_argv(const gchar         *working_directory,
                         const gchar *const  *argv,
                         GPid                *child_pid_out,
                         GError             **error)
{
    if (!argv || !argv[0] || argv[0][0] == '\0') {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_INVAL,
                            "Empty command argv");
        return FALSE;
    }

    if (!target_executable_exists(argv[0])) {
        g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT,
                    "Executable not found: %s", argv[0]);
        return FALSE;
    }

    g_autofree gchar *systemd_run = find_usable_systemd_run();
    g_autoptr(GPtrArray) full_argv = g_ptr_array_new();

    if (systemd_run != NULL) {
        g_ptr_array_add(full_argv, systemd_run);
        g_ptr_array_add(full_argv, (gpointer)"--user");
        g_ptr_array_add(full_argv, (gpointer)"--scope");
        g_ptr_array_add(full_argv, (gpointer)"--quiet");
        g_ptr_array_add(full_argv, (gpointer)"--collect");
        g_ptr_array_add(full_argv, (gpointer)"--slice=app.slice");
        g_ptr_array_add(full_argv, (gpointer)"--");
    }

    for (gsize i = 0; argv[i] != NULL; i++) {
        g_ptr_array_add(full_argv, (gpointer)argv[i]);
    }
    g_ptr_array_add(full_argv, NULL);

    GSpawnFlags flags = G_SPAWN_SEARCH_PATH;
    if (child_pid_out != NULL)
        flags |= G_SPAWN_DO_NOT_REAP_CHILD;

    return g_spawn_async(working_directory,
                         (gchar **)full_argv->pdata,
                         NULL,
                         flags,
                         detached_child_setup,
                         NULL,
                         child_pid_out,
                         error);
}

gboolean
kajo_spawn_detached_command_line(const gchar  *command_line,
                                 GError      **error)
{
    if (!command_line || command_line[0] == '\0') {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_INVAL,
                            "Empty command line");
        return FALSE;
    }

    gint argc = 0;
    g_auto(GStrv) argv = NULL;
    if (!g_shell_parse_argv(command_line, &argc, &argv, error))
        return FALSE;

    if (argc <= 0 || !argv || !argv[0]) {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_INVAL,
                            "Command line parsed to zero arguments");
        return FALSE;
    }

    return kajo_spawn_detached_argv(NULL, (const gchar *const *)argv, NULL, error);
}
