#include <gtk/gtk.h>
#include "viewer_window.h"
#include "../app_init.h"

static ShellTheme *g_viewer_theme = NULL;
static char       *g_preview_mode = NULL;

static void
ensure_theme_loaded(void)
{
    if (!g_viewer_theme) {
        g_viewer_theme = kajo_app_init_theme();
    }
}

static void
on_app_activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    ensure_theme_loaded();
    KajoViewerWindow *win = kajo_viewer_window_new(app);
    if (g_preview_mode && *g_preview_mode) {
        kajo_viewer_window_open_preview_mode(win, g_preview_mode);
        g_clear_pointer(&g_preview_mode, g_free);
    }
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_open(GtkApplication *app,
            GFile         **files,
            gint            n_files,
            const gchar    *hint G_GNUC_UNUSED,
            gpointer        user_data G_GNUC_UNUSED)
{
    ensure_theme_loaded();
    if (n_files <= 0) {
        on_app_activate(app, NULL);
        return;
    }
    for (gint i = 0; i < n_files; i++) {
        KajoViewerWindow *win = kajo_viewer_window_new(app);
        kajo_viewer_window_open_file(win, files[i]);
        gtk_window_present(GTK_WINDOW(win));
    }
}

static gint
on_handle_local_options(GApplication *app G_GNUC_UNUSED,
                        GVariantDict *options,
                        gpointer      user_data G_GNUC_UNUSED)
{
    const char *pm = NULL;
    if (g_variant_dict_lookup(options, "preview-mode", "&s", &pm) && pm) {
        g_free(g_preview_mode);
        g_preview_mode = g_strdup(pm);
    }
    return -1;
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    g_clear_pointer(&g_preview_mode, g_free);
    kajo_app_shutdown_theme(g_viewer_theme);
    g_viewer_theme = NULL;
}

int
main(int argc, char *argv[])
{
    g_set_prgname("org.kajo.Viewer");
    g_set_application_name("Kajo Viewer");

    g_autoptr(GtkApplication) app = gtk_application_new(
        "org.kajo.Viewer",
        G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE
    );

    const GOptionEntry entries[] = {
        { "preview-mode", 0, 0, G_OPTION_ARG_STRING, NULL, "Open preview specimen (pdf|md|image)", "MODE" },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };
    g_application_add_main_option_entries(G_APPLICATION(app), entries);

    g_signal_connect(app, "handle-local-options", G_CALLBACK(on_handle_local_options), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_app_open), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    return g_application_run(G_APPLICATION(app), argc, argv);
}