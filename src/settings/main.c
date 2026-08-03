#include <gtk/gtk.h>
#include "settings_window.h"
#include "../config.h"
#include "../theme.h"
#include "../icons.h"

static char *target_page = NULL;
static ShellTheme *g_settings_theme = NULL;

static GOptionEntry entries[] = {
    { "page", 'p', 0, G_OPTION_ARG_STRING, &target_page, "Target page to open (network, bluetooth, audio, etc.)", "PAGE" },
    { NULL }
};

static void
on_app_activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    /* Force GTK4 Dark Theme preference */
    GtkSettings *gtk_settings = gtk_settings_get_default();
    if (gtk_settings) {
        g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
    }

    if (!g_settings_theme) {
        ShellConfig *config = shell_config_load(NULL);
        g_settings_theme = shell_theme_new(config ? config->css_path : NULL);
        shell_theme_apply(g_settings_theme, gdk_display_get_default());
        shell_icons_init(config ? config->icon_map_path : NULL);
        if (config) shell_config_destroy(config);
    }

    GtkWidget *win = desktop_settings_window_new_with_page(app, target_page);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    if (g_settings_theme) {
        shell_theme_destroy(g_settings_theme);
        g_settings_theme = NULL;
    }
    shell_icons_cleanup();
}

int
main(int argc, char *argv[])
{
    g_set_prgname("org.kajo.Settings");
    g_set_application_name("Kajo Settings");

    g_autoptr(GOptionContext) context = g_option_context_new("- Kajo Settings Application");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_parse(context, &argc, &argv, NULL);

    g_autoptr(GtkApplication) app = gtk_application_new("org.kajo.Settings", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
