#ifndef SHELL_THEME_H
#define SHELL_THEME_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellTheme ShellTheme;

struct _ShellTheme {
    GtkCssProvider *provider;
    GFileMonitor   *monitor;
    gchar          *css_path;

    GtkCssProvider *override_provider;
    GFileMonitor   *override_monitor;
    gchar          *override_css_path;
};

ShellTheme *shell_theme_new(const gchar *css_path);
void        shell_theme_apply(ShellTheme *theme, GdkDisplay *display);
void        shell_theme_destroy(ShellTheme *theme);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_THEME_H */
