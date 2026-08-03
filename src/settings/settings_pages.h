#ifndef DESKTOP_SETTINGS_PAGES_H
#define DESKTOP_SETTINGS_PAGES_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *build_page_panel(void);
GtkWidget *build_page_widgets(void);
GtkWidget *build_page_niri(void);
GtkWidget *build_page_displays(void);
GtkWidget *build_page_network(void);
GtkWidget *build_page_bluetooth(void);
GtkWidget *build_page_audio(void);
GtkWidget *build_page_notifications(void);
GtkWidget *build_page_theme(void);
GtkWidget *build_page_about(void);

G_END_DECLS

#endif /* DESKTOP_SETTINGS_PAGES_H */
