#ifndef KAJO_IDE_WINDOW_H
#define KAJO_IDE_WINDOW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define KAJO_TYPE_IDE_WINDOW (kajo_ide_window_get_type())
G_DECLARE_FINAL_TYPE(KajoIdeWindow, kajo_ide_window, KAJO, IDE_WINDOW, GtkWindow)

KajoIdeWindow *kajo_ide_window_new(GtkApplication *app, const char *workspace_dir);

void           kajo_ide_window_open_path(KajoIdeWindow *self,
                                         const char    *path,
                                         guint          line_1based,
                                         guint          col_1based);

void           kajo_ide_window_open_files(KajoIdeWindow *self,
                                          GFile        **files,
                                          int            n_files);

G_END_DECLS

#endif /* KAJO_IDE_WINDOW_H */
