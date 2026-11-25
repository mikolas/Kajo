#ifndef KAJO_VIEWER_WINDOW_H
#define KAJO_VIEWER_WINDOW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define KAJO_TYPE_VIEWER_WINDOW (kajo_viewer_window_get_type())
G_DECLARE_FINAL_TYPE(KajoViewerWindow, kajo_viewer_window, KAJO, VIEWER_WINDOW, GtkApplicationWindow)

KajoViewerWindow *kajo_viewer_window_new(GtkApplication *app);
void              kajo_viewer_window_open_file(KajoViewerWindow *self, GFile *file);
void              kajo_viewer_window_open_preview_mode(KajoViewerWindow *self, const char *mode);

G_END_DECLS

#endif /* KAJO_VIEWER_WINDOW_H */