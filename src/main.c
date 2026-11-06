#include "shell.h"
#include "icons.h"
#include "hardware/hw_caps.h"

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <signal.h>
#include <sys/prctl.h>

static ShellApp *g_shell_app = NULL;
static guint sigint_id = 0;
static guint sigterm_id = 0;

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    g_shell_app = shell_app_new(app);
    shell_app_activate(g_shell_app);
}

static void
on_shutdown(GtkApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;

    if (sigint_id > 0) { g_source_remove(sigint_id); sigint_id = 0; }
    if (sigterm_id > 0) { g_source_remove(sigterm_id); sigterm_id = 0; }

    if (g_shell_app) {
        shell_app_destroy(g_shell_app);
        g_shell_app = NULL;
    }

    shell_icons_cleanup();
    shell_hw_caps_cleanup();
}

static gboolean
on_signal_quit(gpointer user_data)
{
    GApplication *app = G_APPLICATION(user_data);
    g_application_quit(app);
    return G_SOURCE_REMOVE;
}

#include "ipc/socket.h"

typedef struct {
    const char *command;    /* IPC command sent to the shell (also accepted as a bare word) */
    const char *long_flag;
    const char *short_flag; /* may be NULL */
} CliCommand;

static const CliCommand kCliCommands[] = {
    { "launcher-toggle",  "--toggle-launcher",  "-t" },
    { "brightness-up",    "--brightness-up",    NULL },
    { "brightness-down",  "--brightness-down",  NULL },
    { "volume-up",        "--volume-up",        NULL },
    { "volume-down",      "--volume-down",      NULL },
    { "volume-mute",      "--volume-mute",      NULL },
    { "lock",             "--lock",             NULL },
    { "screensaver",      "--screensaver",      NULL },
    { "reload-config",    "--reload-config",    NULL },
    { "reload-theme",     "--reload-theme",     NULL },
    { "toggle-osk",       "--toggle-osk",       NULL },
    { "show-osk",         "--show-osk",         NULL },
    { "hide-osk",         "--hide-osk",         NULL },
};


int
main(int argc, char *argv[])
{
#ifdef PR_SET_DUMPABLE
    /* Allow portal settings and /proc/self/root inspection when running with setcap capabilities */
    prctl(PR_SET_DUMPABLE, 1);
#endif
    /* CLI client mode: `kajo --lock`, `kajo volume-up`, ... forward one command
     * to the running shell over its socket and exit. */
    for (int i = 1; i < argc; i++) {
        for (gsize c = 0; c < G_N_ELEMENTS(kCliCommands); c++) {
            const CliCommand *cmd = &kCliCommands[c];
            if (g_strcmp0(argv[i], cmd->long_flag) == 0 ||
                g_strcmp0(argv[i], cmd->command) == 0 ||
                (cmd->short_flag && g_strcmp0(argv[i], cmd->short_flag) == 0)) {
                if (shell_ipc_send_command(cmd->command))
                    return 0;
                g_printerr("kajo is not running.\n");
                return 1;
            }
        }
    }

    g_autoptr(GtkApplication) app = gtk_application_new("org.kajo.shell", G_APPLICATION_DEFAULT_FLAGS);
    int status;

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);

    /* Handle SIGINT and SIGTERM for clean shutdown */
    sigint_id = g_unix_signal_add(SIGINT, on_signal_quit, app);
    sigterm_id = g_unix_signal_add(SIGTERM, on_signal_quit, app);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    if (sigint_id > 0) { g_source_remove(sigint_id); sigint_id = 0; }
    if (sigterm_id > 0) { g_source_remove(sigterm_id); sigterm_id = 0; }
    shell_icons_cleanup();
    shell_hw_caps_cleanup();

    return status;
}
