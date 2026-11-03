#ifndef KAJO_FILES_WINDOW_H
#define KAJO_FILES_WINDOW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define KAJO_TYPE_FILES_WINDOW (kajo_files_window_get_type())
G_DECLARE_FINAL_TYPE(KajoFilesWindow, kajo_files_window, KAJO, FILES_WINDOW, GtkApplicationWindow)

GtkWidget *kajo_files_window_new(GtkApplication *app, const char *initial_uri_or_path);
void       kajo_files_window_navigate_uri(KajoFilesWindow *self, const char *uri_or_path);

G_END_DECLS

#endif /* KAJO_FILES_WINDOW_H */
