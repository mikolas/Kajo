#include <gtk/gtk.h>
#include "settings_window.h"
#include "settings_common.h"
#include "../config.h"
#include "../theme.h"
#include "../icons.h"
#include "../app_init.h"
#include "../hardware/hw_caps.h"

static char *target_page = NULL;
static ShellTheme *g_settings_theme = NULL;

static GOptionEntry entries[] = {
    { "page", 'p', 0, G_OPTION_ARG_STRING, &target_page, "Target page to open (network, bluetooth, audio, etc.)", "PAGE" },
    { NULL }
};

static void
on_app_activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    if (!g_settings_theme) {
        g_settings_theme = kajo_app_init_theme();
        settings_set_global_theme(g_settings_theme);
    }

    GtkWidget *win = desktop_settings_window_new_with_page(app, target_page);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    settings_set_global_theme(NULL);
    kajo_app_shutdown_theme(g_settings_theme);
    g_settings_theme = NULL;
    shell_hw_caps_cleanup();
    g_clear_pointer(&target_page, g_free);
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
