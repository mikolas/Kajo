#include <gtk/gtk.h>
#include <glib-unix.h>
#include <signal.h>
#include <string.h>
#include "files_window.h"
#include "files_portal.h"
#include "files_chooser.h"
#include "../config.h"
#include "../theme.h"
#include "../icons.h"
#include "../app_init.h"

static ShellTheme      *g_files_theme  = NULL;
static KajoFilesPortal *g_files_portal = NULL;

static void
ensure_theme_loaded(void)
{
    if (!g_files_theme) {
        g_files_theme = kajo_app_init_theme();
    }
}

static void
on_app_activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    ensure_theme_loaded();
    GtkWidget *win = kajo_files_window_new(app, NULL);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_open(GtkApplication *app,
            GFile **files,
            gint n_files,
            const gchar *hint G_GNUC_UNUSED,
            gpointer user_data G_GNUC_UNUSED)
{
    ensure_theme_loaded();
    for (int i = 0; i < n_files; i++) {
        g_autofree char *uri = g_file_get_uri(files[i]);
        GtkWidget *win = kajo_files_window_new(app, uri);
        gtk_window_present(GTK_WINDOW(win));
    }
}

static void
on_portal_present_request(KajoPortalActiveRequest *active_req, gpointer user_data)
{
    GtkApplication *app = GTK_APPLICATION(user_data);
    ensure_theme_loaded();
    GtkWidget *chooser = kajo_files_chooser_new_for_request(app, active_req);
    gtk_window_present(GTK_WINDOW(chooser));
}

static void
on_portal_startup(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    ensure_theme_loaded();
    g_application_hold(G_APPLICATION(app));
    g_files_portal = kajo_files_portal_new(on_portal_present_request, app);
}

static gboolean
on_portal_signal_quit(gpointer user_data)
{
    g_application_quit(G_APPLICATION(user_data));
    return G_SOURCE_REMOVE;
}

static void
on_preview_activate(GtkApplication *app, gpointer user_data)
{
    const char *dir = user_data;
    ensure_theme_loaded();
    GtkWidget *win = kajo_files_chooser_new_preview(app, dir);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    g_clear_pointer(&g_files_portal, kajo_files_portal_destroy);
    kajo_app_shutdown_theme(g_files_theme);
    g_files_theme = NULL;
}

int
main(int argc, char *argv[])
{
    gboolean portal_mode = FALSE;
    gboolean preview_mode = FALSE;
    const char *preview_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--portal") == 0) {
            portal_mode = TRUE;
        } else if (g_strcmp0(argv[i], "--chooser-preview") == 0) {
            preview_mode = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                preview_dir = argv[++i];
        }
    }

    if (portal_mode) {
        kajo_app_disable_portal_client();
        g_set_prgname("org.kajo.Files.Chooser");
        g_set_application_name("Kajo File Chooser");

        g_autoptr(GtkApplication) app = gtk_application_new(
            "org.kajo.Files.Chooser",
            G_APPLICATION_IS_SERVICE
        );
        g_signal_connect(app, "startup", G_CALLBACK(on_portal_startup), NULL);
        g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);
        guint sig_int  = g_unix_signal_add(SIGINT, on_portal_signal_quit, app);
        guint sig_term = g_unix_signal_add(SIGTERM, on_portal_signal_quit, app);
        int status = g_application_run(G_APPLICATION(app), 0, NULL);
        if (sig_int > 0) g_source_remove(sig_int);
        if (sig_term > 0) g_source_remove(sig_term);
        return status;
    }

    if (preview_mode) {
        g_set_prgname("org.kajo.Files.Chooser");
        g_set_application_name("Kajo File Chooser");

        g_autoptr(GtkApplication) app = gtk_application_new(
            "org.kajo.Files.Chooser",
            G_APPLICATION_NON_UNIQUE
        );
        g_signal_connect(app, "activate", G_CALLBACK(on_preview_activate), (gpointer)preview_dir);
        g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);
        return g_application_run(G_APPLICATION(app), 0, NULL);
    }

    g_set_prgname("org.kajo.Files");
    g_set_application_name("Kajo Files");

    g_autoptr(GtkApplication) app = gtk_application_new(
        "org.kajo.Files",
        G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE
    );

    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_app_open), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    return g_application_run(G_APPLICATION(app), argc, argv);
}
