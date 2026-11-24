#ifndef KAJO_WEB_OMNIBAR_AC_H
#define KAJO_WEB_OMNIBAR_AC_H

#include <gtk/gtk.h>
#include "web_model.h"
#include "web_storage.h"

G_BEGIN_DECLS

typedef struct _KajoWebOmnibarAc KajoWebOmnibarAc;

typedef void (*KajoWebOmnibarAcSelectCb)(const gchar *uri, gpointer user_data);

KajoWebOmnibarAc *web_omnibar_ac_new(GtkEntry                *entry,
                                     KajoWebStorage          *storage,
                                     const KajoWebModel      *model,
                                     KajoWebOmnibarAcSelectCb on_select,
                                     gpointer                 user_data);
void              web_omnibar_ac_free(KajoWebOmnibarAc *ac);

void              web_omnibar_ac_schedule(KajoWebOmnibarAc *ac, const gchar *query);
void              web_omnibar_ac_dismiss(KajoWebOmnibarAc *ac);
gboolean          web_omnibar_ac_handle_key(KajoWebOmnibarAc *ac, guint keyval);
gboolean          web_omnibar_ac_is_visible(const KajoWebOmnibarAc *ac);

G_END_DECLS

#endif /* KAJO_WEB_OMNIBAR_AC_H */
