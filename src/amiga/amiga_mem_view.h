#ifndef KAJO_AMIGA_MEM_VIEW_H
#define KAJO_AMIGA_MEM_VIEW_H

#include "amiga_rsp.h"
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_MEM_VIEW (amiga_mem_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaMemView, amiga_mem_view, AMIGA, MEM_VIEW, GtkBox)

typedef void (*AmigaMemReadCb)(AmigaMemView *view, uint32_t addr, size_t len, gpointer user_data);
typedef void (*AmigaMemWriteCb)(AmigaMemView  *view,
                                uint32_t       addr,
                                const uint8_t *bytes,
                                size_t         len,
                                gpointer       user_data);
typedef void (*AmigaMemWatchCb)(AmigaMemView *view,
                                uint8_t       z_type,
                                uint32_t      addr,
                                uint32_t      len,
                                gpointer      user_data);
typedef void (*AmigaMemDmaWatchCb)(AmigaMemView *view,
                                   uint32_t      addr,
                                   uint32_t      len,
                                   gpointer      user_data);

AmigaMemView *amiga_mem_view_new(void);

void amiga_mem_view_set_callbacks(AmigaMemView    *self,
                                  AmigaMemReadCb   read_cb,
                                  AmigaMemWriteCb  write_cb,
                                  AmigaMemWatchCb  watch_cb,
                                  gpointer         user_data);
void amiga_mem_view_set_dma_watch_callback(AmigaMemView       *self,
                                           AmigaMemDmaWatchCb  dma_cb,
                                           gpointer            user_data);

void amiga_mem_view_set_regs_and_custom(AmigaMemView              *self,
                                        const AmigaCpuRegs        *regs,
                                        const AmigaCustomSnapshot *custom);

void amiga_mem_view_set_data(AmigaMemView  *self,
                             uint32_t       base_addr,
                             const uint8_t *bytes,
                             size_t         len);

void     amiga_mem_view_jump_to(AmigaMemView *self, uint32_t addr);
void     amiga_mem_view_set_grouping(AmigaMemView *self, int group_bytes);
uint32_t amiga_mem_view_get_base_addr(AmigaMemView *self);

G_END_DECLS

#endif /* KAJO_AMIGA_MEM_VIEW_H */
