#ifndef KAJO_WEB_PICKER_H
#define KAJO_WEB_PICKER_H

#include <gtk/gtk.h>
#include "web_profiles.h"

G_BEGIN_DECLS

typedef void (*KajoWebPickerSelectCb)(const gchar *profile_id, gboolean is_private, gpointer ud);

GtkWindow *web_picker_window_new(GtkApplication        *app,
                                 KajoWebProfileManager *pm,
                                 KajoWebPickerSelectCb  on_select,
                                 gpointer               user_data);

G_END_DECLS

#endif /* KAJO_WEB_PICKER_H */
