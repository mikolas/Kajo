#ifndef DESKTOP_SETTINGS_WINDOW_H
#define DESKTOP_SETTINGS_WINDOW_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

GtkWidget *desktop_settings_window_new(GtkApplication *app);
GtkWidget *desktop_settings_window_new_with_page(GtkApplication *app, const char *initial_page);
void       desktop_settings_window_set_simulated_column_width(GtkWidget *window, int col_width);

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_SETTINGS_WINDOW_H */
