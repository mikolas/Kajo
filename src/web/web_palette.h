#ifndef KAJO_WEB_PALETTE_H
#define KAJO_WEB_PALETTE_H

#include <gtk/gtk.h>
#include "web_model.h"
#include "web_storage.h"

G_BEGIN_DECLS

typedef struct {
    void (*on_select_tab)(guint64 tab_id, gpointer ud);
    void (*on_open_uri)(const gchar *uri, gpointer ud);
    void (*on_run_command)(const gchar *cmd_id, gpointer ud);
} KajoWebPaletteCallbacks;

GtkWidget *web_palette_new(const KajoWebPaletteCallbacks *cbs, gpointer user_data);
void       web_palette_open(GtkWidget            *palette,
                            const KajoWebModel   *model,
                            KajoWebStorage       *storage);
void       web_palette_close(GtkWidget *palette);

G_END_DECLS

#endif /* KAJO_WEB_PALETTE_H */
