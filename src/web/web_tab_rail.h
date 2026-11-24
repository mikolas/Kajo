#ifndef KAJO_WEB_TAB_RAIL_H
#define KAJO_WEB_TAB_RAIL_H

#include <gtk/gtk.h>
#include "web_model.h"
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebTabRail KajoWebTabRail;

typedef struct {
    void        (*on_select_tab)(guint64 tab_id, gpointer ud);
    void        (*on_close_tab)(guint64 tab_id, gpointer ud);
    void        (*on_toggle_mute)(guint64 tab_id, gpointer ud);
    void        (*on_toggle_pin)(guint64 tab_id, gpointer ud);
    void        (*on_reload_tab)(guint64 tab_id, gpointer ud);
    void        (*on_duplicate_tab)(guint64 tab_id, gpointer ud);
    void        (*on_copy_tab_url)(guint64 tab_id, gpointer ud);
    void        (*on_suspend_tab)(guint64 tab_id, gpointer ud);
    void        (*on_close_other_tabs)(guint64 keep_tab_id, gpointer ud);
    void        (*on_close_tabs_below)(guint64 pivot_tab_id, gpointer ud);
    void        (*on_toggle_group)(const gchar *group_name, gpointer ud);
    void        (*on_new_tab)(gpointer ud);
    void        (*on_undo_close)(gpointer ud);
    void        (*on_reorder_tab)(guint64 tab_id, guint target_index, gpointer ud);
    GdkTexture *(*get_favicon)(guint64 tab_id, gpointer ud);
} KajoWebTabRailCallbacks;

KajoWebTabRail *web_tab_rail_new(const KajoWebTabRailCallbacks *cbs, gpointer user_data);
void            web_tab_rail_free(KajoWebTabRail *rail);

/* Returns the trigger strip (rgba(0,0,0,0.01) overlay child), rail drawer, and 48px compact spacer */
GtkWidget      *web_tab_rail_get_trigger(KajoWebTabRail *rail);
GtkWidget      *web_tab_rail_get_widget(KajoWebTabRail *rail);
GtkWidget      *web_tab_rail_get_compact_spacer(KajoWebTabRail *rail);

void            web_tab_rail_apply_config(KajoWebTabRail *rail, const KajoWebCfg *cfg);
void            web_tab_rail_rebuild(KajoWebTabRail *rail, const KajoWebModel *model);
void            web_tab_rail_update_tab(KajoWebTabRail *rail, const KajoWebModel *model, guint64 tab_id);
void            web_tab_rail_toggle_reveal(KajoWebTabRail *rail);
gboolean        web_tab_rail_escape(KajoWebTabRail *rail);
void            web_tab_rail_focus_lost(KajoWebTabRail *rail);
gboolean        web_tab_rail_is_expanded(const KajoWebTabRail *rail);

G_END_DECLS

#endif /* KAJO_WEB_TAB_RAIL_H */
