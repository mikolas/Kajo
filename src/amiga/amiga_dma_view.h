#ifndef KAJO_AMIGA_DMA_VIEW_H
#define KAJO_AMIGA_DMA_VIEW_H

#include "amiga_decode.h"
#include "amiga_rsp.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_DMA_VIEW (amiga_dma_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaDmaView, amiga_dma_view, AMIGA, DMA_VIEW, GtkBox)

AmigaDmaView *amiga_dma_view_new(void);
void          amiga_dma_view_set_snapshot(AmigaDmaView              *self,
                                          const AmigaCustomSnapshot *snap);

G_END_DECLS

#endif /* KAJO_AMIGA_DMA_VIEW_H */
