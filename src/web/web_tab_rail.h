#ifndef KAJO_WEB_TAB_RAIL_H
#define KAJO_WEB_TAB_RAIL_H

#include <gtk/gtk.h>
#include "web_model.h"
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebTabRail KajoWebTabRail;

typedef struct {
    void (*on_select_tab)(guint64 tab_id, gpointer ud);
    void (*on_close_tab)(guint64 tab_id, gpointer ud);
    void (*on_toggle_mute)(guint64 tab_id, gpointer ud);
    void (*on_toggle_pin)(guint64 tab_id, gpointer ud);
    void (*on_new_tab)(gpointer ud);
    void (*on_undo_close)(gpointer ud);
    void (*on_reorder_tab)(guint64 tab_id, guint target_index, gpointer ud);
} KajoWebTabRailCallbacks;

KajoWebTabRail *web_tab_rail_new(const KajoWebTabRailCallbacks *cbs, gpointer user_data);
void            web_tab_rail_free(KajoWebTabRail *rail);

/* Returns the trigger strip (4px rgba(0,0,0,0.01) overlay child) and the rail drawer widget */
GtkWidget      *web_tab_rail_get_trigger(KajoWebTabRail *rail);
GtkWidget      *web_tab_rail_get_widget(KajoWebTabRail *rail);
GtkWidget      *web_tab_rail_get_compact_spacer(KajoWebTabRail *rail);

void            web_tab_rail_apply_config(KajoWebTabRail *rail, const KajoWebCfg *cfg);
void            web_tab_rail_rebuild(KajoWebTabRail *rail, const KajoWebModel *model);
void            web_tab_rail_toggle_reveal(KajoWebTabRail *rail);
void            web_tab_rail_escape(KajoWebTabRail *rail);
void            web_tab_rail_focus_lost(KajoWebTabRail *rail);
gboolean        web_tab_rail_is_expanded(const KajoWebTabRail *rail);

G_END_DECLS

#endif /* KAJO_WEB_TAB_RAIL_H */
