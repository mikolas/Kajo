#ifndef KAJO_AMIGA_CHIPS_VIEW_H
#define KAJO_AMIGA_CHIPS_VIEW_H

#include "amiga_rsp.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_CHIPS_VIEW (amiga_chips_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaChipsView, amiga_chips_view, AMIGA, CHIPS_VIEW, GtkBox)

typedef struct _AmigaChipsCallbacks {
  void (*toggle_blit_trace)(AmigaChipsView *v, bool enabled, gpointer ud);
  void (*refresh_blit_trace)(AmigaChipsView *v, gpointer ud);
  void (*jump_mem)(AmigaChipsView *v, uint32_t addr, gpointer ud);
} AmigaChipsCallbacks;

AmigaChipsView *amiga_chips_view_new(void);

void amiga_chips_view_set_callbacks(AmigaChipsView            *self,
                                    const AmigaChipsCallbacks *cbs,
                                    gpointer                   ud);

void amiga_chips_view_set_snapshot(AmigaChipsView            *self,
                                   const AmigaCustomSnapshot *snap);

void amiga_chips_view_set_watch_events(AmigaChipsView *self,
                                       GPtrArray      *events);

G_END_DECLS

#endif /* KAJO_AMIGA_CHIPS_VIEW_H */
