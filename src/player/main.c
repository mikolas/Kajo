#include <gtk/gtk.h>
#include "player_window.h"
#include "../config.h"
#include "../theme.h"
#include "../icons.h"
#include "../app_init.h"

static ShellTheme *g_player_theme = NULL;

static void
ensure_theme_loaded(void)
{
    if (!g_player_theme) {
        g_player_theme = kajo_app_init_theme();
    }
}

static void
on_app_activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    ensure_theme_loaded();
    KajoPlayerWindow *win = kajo_player_window_new(app);
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
    KajoPlayerWindow *win = kajo_player_window_new(app);
    kajo_player_window_open_files(win, files, n_files);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    kajo_app_shutdown_theme(g_player_theme);
    g_player_theme = NULL;
}

int
main(int argc, char *argv[])
{
    g_set_prgname("org.kajo.Player");
    g_set_application_name("Kajo Player");

    g_autoptr(GtkApplication) app = gtk_application_new(
        "org.kajo.Player",
        G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE
    );

    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_app_open), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    return g_application_run(G_APPLICATION(app), argc, argv);
}
