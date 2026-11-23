#ifndef KAJO_WEB_WINDOW_H
#define KAJO_WEB_WINDOW_H

#include <gtk/gtk.h>
#include "web_profiles.h"

G_BEGIN_DECLS

typedef struct _KajoWebWindow KajoWebWindow;

typedef struct {
    void (*on_open_profile)(const gchar *profile_id, gboolean is_private, const gchar *initial_url, gpointer ud);
    void (*on_open_picker)(gpointer ud);
} KajoWebWindowAppCallbacks;

GtkWindow     *kajo_web_window_new(GtkApplication                 *app,
                                   KajoWebProfileManager          *pm,
                                   const gchar                    *profile_id,
                                   gboolean                        is_private,
                                   gboolean                        hw_disabled_override,
                                   const gchar *const             *initial_urls,
                                   gint                            n_urls,
                                   const KajoWebWindowAppCallbacks *app_cbs,
                                   gpointer                        app_ud);

void           kajo_web_window_open_uri(GtkWindow *win, const gchar *uri, gboolean new_tab);
const gchar   *kajo_web_window_get_profile_id(GtkWindow *win);
gboolean       kajo_web_window_is_private(GtkWindow *win);

G_END_DECLS

#endif /* KAJO_WEB_WINDOW_H */
