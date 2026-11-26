#ifndef KAJO_AMIGA_COPPER_VIEW_H
#define KAJO_AMIGA_COPPER_VIEW_H

#include "amiga_decode.h"
#include "amiga_rsp.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_COPPER_VIEW (amiga_copper_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaCopperView, amiga_copper_view, AMIGA, COPPER_VIEW, GtkBox)

typedef struct _AmigaCopperCallbacks {
  void (*request_mem)(AmigaCopperView *v, uint32_t addr, size_t len, gpointer ud);
  void (*scan_chip_ram)(AmigaCopperView *v, gpointer ud);
  void (*toggle_watch_cop1lc)(AmigaCopperView *v, bool enabled, gpointer ud);
  void (*jump_mem)(AmigaCopperView *v, uint32_t addr, gpointer ud);
} AmigaCopperCallbacks;

AmigaCopperView *amiga_copper_view_new(void);

void amiga_copper_view_set_callbacks(AmigaCopperView            *self,
                                     const AmigaCopperCallbacks *cbs,
                                     gpointer                    ud);

void amiga_copper_view_set_snapshot(AmigaCopperView           *self,
                                    const AmigaCustomSnapshot *snap);

void amiga_copper_view_set_bytes(AmigaCopperView *self,
                                 uint32_t         base_addr,
                                 const uint8_t   *data,
                                 size_t           len,
                                 const char      *provenance_label);

void amiga_copper_view_set_scan_hits(AmigaCopperView *self,
                                     const uint32_t  *addrs,
                                     size_t           count);

G_END_DECLS

#endif /* KAJO_AMIGA_COPPER_VIEW_H */
