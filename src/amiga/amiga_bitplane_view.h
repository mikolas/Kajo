#ifndef KAJO_AMIGA_BITPLANE_VIEW_H
#define KAJO_AMIGA_BITPLANE_VIEW_H

#include "amiga_decode.h"
#include "amiga_rsp.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_BITPLANE_VIEW (amiga_bitplane_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaBitplaneView, amiga_bitplane_view, AMIGA, BITPLANE_VIEW, GtkBox)

typedef struct _AmigaBitplaneCallbacks {
  void     (*request_mem)(AmigaBitplaneView *v, uint32_t addr, size_t len, gpointer ud);
  uint32_t (*resolve_symbol)(AmigaBitplaneView *v, const char *sym, bool *ok, gpointer ud);
} AmigaBitplaneCallbacks;

AmigaBitplaneView *amiga_bitplane_view_new(void);

void amiga_bitplane_view_set_callbacks(AmigaBitplaneView            *self,
                                       const AmigaBitplaneCallbacks *cbs,
                                       gpointer                      ud);

void amiga_bitplane_view_set_snapshot(AmigaBitplaneView         *self,
                                      const AmigaCustomSnapshot *snap,
                                      const uint8_t             *chip_ram,
                                      size_t                     chip_len,
                                      uint32_t                   chip_base);

void amiga_bitplane_view_set_chip_ram(AmigaBitplaneView *self,
                                      uint32_t           chip_base,
                                      const uint8_t     *chip_ram,
                                      size_t             chip_len);

void amiga_bitplane_view_set_symbol_hint(AmigaBitplaneView *self, const char *sym_name);

G_END_DECLS

#endif /* KAJO_AMIGA_BITPLANE_VIEW_H */
