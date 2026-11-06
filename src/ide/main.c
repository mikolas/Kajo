#include "../config.h"
#include "../icons.h"
#include "../theme.h"
#include "ide_window.h"
#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>

static ShellTheme *g_ide_theme = NULL;

static void ensure_theme_and_schemes_loaded(void) {
  if (!g_ide_theme) {
    gtk_source_init();

    GtkSourceStyleSchemeManager *sm =
        gtk_source_style_scheme_manager_get_default();
#ifdef KAJO_SOURCE_DIR
    gtk_source_style_scheme_manager_append_search_path(
        sm, KAJO_SOURCE_DIR "/data/ide");
#endif
    g_autofree char *user_ide_dir =
        g_build_filename(g_get_user_data_dir(), "kajo", "ide", NULL);
    gtk_source_style_scheme_manager_append_search_path(sm, user_ide_dir);

    ShellConfig *config = shell_config_load(NULL);
    g_ide_theme = shell_theme_new(config ? config->css_path : NULL);
    shell_theme_apply(g_ide_theme, gdk_display_get_default());
    shell_icons_init(config ? config->icon_map_path : NULL);
    if (config) shell_config_destroy(config);
  }
}

static void on_app_activate(GtkApplication *app,
                            gpointer        user_data G_GNUC_UNUSED) {
  ensure_theme_and_schemes_loaded();
  KajoIdeWindow *win = kajo_ide_window_new(app, NULL);
  gtk_window_present(GTK_WINDOW(win));
}

static void on_app_open(GtkApplication *app,
                        GFile         **files,
                        gint            n_files,
                        const gchar    *hint G_GNUC_UNUSED,
                        gpointer        user_data G_GNUC_UNUSED) {
  ensure_theme_and_schemes_loaded();
  KajoIdeWindow *win = kajo_ide_window_new(app, NULL);
  kajo_ide_window_open_files(win, files, n_files);
  gtk_window_present(GTK_WINDOW(win));
}

static void on_app_shutdown(GtkApplication *app G_GNUC_UNUSED,
                            gpointer        user_data G_GNUC_UNUSED) {
  if (g_ide_theme) {
    shell_theme_destroy(g_ide_theme);
    g_ide_theme = NULL;
  }
  shell_icons_cleanup();
}

int main(int argc, char *argv[]) {
  g_set_prgname("org.kajo.IDE");
  g_set_application_name("Kajo IDE");

  g_autoptr(GtkApplication) app = gtk_application_new(
      "org.kajo.IDE", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);

  g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
  g_signal_connect(app, "open", G_CALLBACK(on_app_open), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

  return g_application_run(G_APPLICATION(app), argc, argv);
}
