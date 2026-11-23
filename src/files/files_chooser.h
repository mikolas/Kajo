#ifndef KAJO_FILES_CHOOSER_H
#define KAJO_FILES_CHOOSER_H

#include <gtk/gtk.h>
#include "files_portal.h"

G_BEGIN_DECLS

#define KAJO_TYPE_FILES_CHOOSER (kajo_files_chooser_get_type())
G_DECLARE_FINAL_TYPE(KajoFilesChooser, kajo_files_chooser, KAJO, FILES_CHOOSER, GtkWindow)

/* Create & present a native Aero+Metro FileChooser dialog backed by a portal request. */
GtkWidget *kajo_files_chooser_new_for_request(GtkApplication          *app,
                                              KajoPortalActiveRequest *active_req);

/* Create a standalone preview chooser window (for headless ui-shot visual verification). */
GtkWidget *kajo_files_chooser_new_preview(GtkApplication *app, const char *initial_dir);

G_END_DECLS

#endif /* KAJO_FILES_CHOOSER_H */
