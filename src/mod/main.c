#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include "mod_window.h"
#include "../theme.h"
#include "../app_init.h"

static ShellTheme *g_mod_theme     = NULL;
static int         g_opt_col_width = 0;
static GPtrArray  *g_initial_paths = NULL;

static void
ensure_theme_loaded(void)
{
    if (!g_mod_theme) {
        g_mod_theme = kajo_app_init_theme();
    }
}

static void
on_app_activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    ensure_theme_loaded();
    int w = (g_opt_col_width >= 480) ? g_opt_col_width : 814;
    KajoModWindow *win = kajo_mod_window_new(app, w, 940);
    if (g_opt_col_width >= 480) {
        kajo_mod_window_set_simulated_column_width(win, g_opt_col_width);
    }
    if (g_initial_paths && g_initial_paths->len > 0) {
        kajo_mod_window_open_paths(win,
                                   (const char * const *)g_initial_paths->pdata,
                                   (int)g_initial_paths->len);
    }
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    kajo_app_shutdown_theme(g_mod_theme);
    g_mod_theme = NULL;
}

int
main(int argc, char *argv[])
{
    int filtered_argc = 1;
    char **filtered_argv = g_new0(char *, argc + 1);
    filtered_argv[0] = argv[0];
    g_initial_paths = g_ptr_array_new_with_free_func(g_free);

    for (int i = 1; i < argc; i++) {
        if ((g_strcmp0(argv[i], "--column-width") == 0 ||
             g_strcmp0(argv[i], "--col-width") == 0) && i + 1 < argc) {
            g_opt_col_width = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            g_ptr_array_add(g_initial_paths, g_strdup(argv[i]));
        } else {
            filtered_argv[filtered_argc++] = argv[i];
        }
    }

    g_set_prgname("org.kajo.Mod");
    g_set_application_name("Kajo Mod");

    g_autoptr(GtkApplication) app = gtk_application_new(
        "org.kajo.Mod",
        G_APPLICATION_NON_UNIQUE
    );

    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    int status = g_application_run(G_APPLICATION(app), filtered_argc, filtered_argv);
    g_ptr_array_unref(g_initial_paths);
    g_initial_paths = NULL;
    g_free(filtered_argv);
    return status;
}
