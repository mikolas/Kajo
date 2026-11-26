#ifndef KAJO_AMIGA_CPU_STRIP_H
#define KAJO_AMIGA_CPU_STRIP_H

#include "amiga_decode.h"
#include "amiga_rsp.h"
#include "amiga_symbols.h"
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_CPU_STRIP (amiga_cpu_strip_get_type())
G_DECLARE_FINAL_TYPE(AmigaCpuStrip, amiga_cpu_strip, AMIGA, CPU_STRIP, GtkBox)

typedef void (*AmigaCpuWriteRegsCb)(AmigaCpuStrip      *strip,
                                    const AmigaCpuRegs *regs,
                                    gpointer            user_data);

typedef void (*AmigaCpuJumpAddrCb)(AmigaCpuStrip *strip,
                                   uint32_t       addr,
                                   bool           to_mem_hex,
                                   gpointer       user_data);

typedef void (*AmigaCpuCheckpointCb)(AmigaCpuStrip *strip,
                                     bool           restore,
                                     gpointer       user_data);

AmigaCpuStrip *amiga_cpu_strip_new(void);

void amiga_cpu_strip_set_callbacks(AmigaCpuStrip       *self,
                                   AmigaCpuWriteRegsCb  write_cb,
                                   AmigaCpuJumpAddrCb   jump_cb,
                                   AmigaCpuCheckpointCb cp_cb,
                                   gpointer             user_data);

void amiga_cpu_strip_update(AmigaCpuStrip          *self,
                            const AmigaCpuRegs     *regs,
                            const AmigaExecStatus  *exec_st,
                            const AmigaSymbolTable *syms,
                            bool                    paused);

void amiga_cpu_strip_reset(AmigaCpuStrip *self);

G_END_DECLS

#endif /* KAJO_AMIGA_CPU_STRIP_H */
