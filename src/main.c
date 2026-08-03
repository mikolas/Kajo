#include "shell.h"
#include "icons.h"

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
    shell_icons_cleanup();

    if (g_shell_app) {
        shell_app_destroy(g_shell_app);
        g_shell_app = NULL;
    }
}

static gboolean
on_signal_quit(gpointer user_data)
{
    GApplication *app = G_APPLICATION(user_data);
    g_application_quit(app);
    return G_SOURCE_REMOVE;
}

#include "ipc/socket.h"

int
main(int argc, char *argv[])
{
#ifdef PR_SET_DUMPABLE
    /* Allow portal settings and /proc/self/root inspection when running with setcap capabilities */
    prctl(PR_SET_DUMPABLE, 1);
#endif
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (g_strcmp0(argv[i], "--toggle-launcher") == 0 ||
                g_strcmp0(argv[i], "-t") == 0 ||
                g_strcmp0(argv[i], "launcher-toggle") == 0) {
                if (shell_ipc_send_command("launcher-toggle")) {
                    return 0;
                } else {
                    g_printerr("kajo is not running.\n");
                    return 1;
                }
            } else if (g_strcmp0(argv[i], "--brightness-up") == 0 ||
                       g_strcmp0(argv[i], "brightness-up") == 0) {
                if (shell_ipc_send_command("brightness-up")) {
                    return 0;
                } else {
                    g_printerr("kajo is not running.\n");
                    return 1;
                }
            } else if (g_strcmp0(argv[i], "--brightness-down") == 0 ||
                       g_strcmp0(argv[i], "brightness-down") == 0) {
                if (shell_ipc_send_command("brightness-down")) {
                    return 0;
                } else {
                    g_printerr("kajo is not running.\n");
                    return 1;
                }
            } else if (g_strcmp0(argv[i], "--volume-up") == 0 ||
                       g_strcmp0(argv[i], "volume-up") == 0) {
                if (shell_ipc_send_command("volume-up")) {
                    return 0;
                } else {
                    g_printerr("kajo is not running.\n");
                    return 1;
                }
            } else if (g_strcmp0(argv[i], "--volume-down") == 0 ||
                       g_strcmp0(argv[i], "volume-down") == 0) {
                if (shell_ipc_send_command("volume-down")) {
                    return 0;
                } else {
                    g_printerr("kajo is not running.\n");
                    return 1;
                }
            } else if (g_strcmp0(argv[i], "--volume-mute") == 0 ||
                       g_strcmp0(argv[i], "volume-mute") == 0) {
                if (shell_ipc_send_command("volume-mute")) {
                    return 0;
                } else {
                    g_printerr("kajo is not running.\n");
                    return 1;
                }
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

    return status;
}
