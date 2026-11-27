#ifndef KAJO_AMIGA_DEBUG_SIDEBAR_H
#define KAJO_AMIGA_DEBUG_SIDEBAR_H

#include "amiga_decode.h"
#include "amiga_rsp.h"
#include "amiga_symbols.h"
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct {
  char     *file;
  guint     line;
  uint32_t  addr;
  bool      armed;
  bool      disabled;
} AmigaSourceBp;

typedef struct {
  char     *expr;
  uint8_t   mode; /* 0=.w, 1=.l, 2=ptr */
  uint32_t  addr;
  uint32_t  val;
  uint32_t  prev_val;
  bool      valid;
  bool      changed;
} AmigaWatchItem;

typedef struct {
  uint8_t   kind; /* 2=Z2 write, 3=Z3 read, 4=Z4 access, 5=dma */
  uint32_t  addr;
  uint32_t  len;
  bool      enabled;
  char     *label;
} AmigaWatchpointItem;

#define AMIGA_TYPE_DEBUG_SIDEBAR (amiga_debug_sidebar_get_type())
G_DECLARE_FINAL_TYPE(AmigaDebugSidebar, amiga_debug_sidebar, AMIGA, DEBUG_SIDEBAR, GtkBox)

typedef struct {
  void (*write_regs)(AmigaDebugSidebar *sb, const AmigaCpuRegs *regs, gpointer ud);
  void (*jump_addr)(AmigaDebugSidebar *sb, uint32_t addr, bool to_mem_hex, gpointer ud);
  void (*checkpoint)(AmigaDebugSidebar *sb, bool restore, gpointer ud);
  void (*exc_mask_changed)(AmigaDebugSidebar *sb, uint8_t mask, gpointer ud);
  void (*add_watch)(AmigaDebugSidebar *sb, const char *expr, gpointer ud);
  void (*remove_watch)(AmigaDebugSidebar *sb, guint idx, gpointer ud);
  void (*cycle_watch_mode)(AmigaDebugSidebar *sb, guint idx, gpointer ud);
  void (*toggle_source_bp)(AmigaDebugSidebar *sb, guint idx, bool enabled, gpointer ud);
  void (*remove_source_bp)(AmigaDebugSidebar *sb, guint idx, gpointer ud);
  void (*toggle_addr_bp)(AmigaDebugSidebar *sb, uint32_t addr, gpointer ud);
  void (*toggle_watchpoint)(AmigaDebugSidebar *sb, guint idx, bool enabled, gpointer ud);
  void (*remove_watchpoint)(AmigaDebugSidebar *sb, guint idx, gpointer ud);
} AmigaDebugSidebarCallbacks;

AmigaDebugSidebar *amiga_debug_sidebar_new(void);

void amiga_debug_sidebar_set_callbacks(AmigaDebugSidebar                *self,
                                       const AmigaDebugSidebarCallbacks *cbs,
                                       gpointer                          user_data);

void amiga_debug_sidebar_set_attach_mode(AmigaDebugSidebar *self, bool attach_mode);

void amiga_debug_sidebar_update_cpu(AmigaDebugSidebar      *self,
                                    const AmigaCpuRegs     *regs,
                                    const AmigaExecStatus  *exec_st,
                                    const AmigaSymbolTable *syms,
                                    bool                    paused);

void amiga_debug_sidebar_set_stack(AmigaDebugSidebar      *self,
                                   uint32_t                sp,
                                   const uint8_t          *bytes,
                                   size_t                  len,
                                   const AmigaSymbolTable *syms);

void amiga_debug_sidebar_set_exc_mask(AmigaDebugSidebar *self, uint8_t mask);
void amiga_debug_sidebar_set_exc_last(AmigaDebugSidebar *self, const char *text);

void amiga_debug_sidebar_set_watches(AmigaDebugSidebar *self, GPtrArray *watches);

void amiga_debug_sidebar_set_breakpoints(AmigaDebugSidebar *self,
                                         GPtrArray         *source_bps,
                                         GHashTable        *bp_addrs,
                                         GPtrArray         *watchpoints);

void amiga_debug_sidebar_set_segments(AmigaDebugSidebar      *self,
                                        const AmigaSegmentList *sl);

void amiga_debug_sidebar_set_tasks(AmigaDebugSidebar *self, GPtrArray *tasks);
void amiga_debug_sidebar_set_memmap(AmigaDebugSidebar *self, GPtrArray *regions);

void amiga_debug_sidebar_reset(AmigaDebugSidebar *self);

G_END_DECLS

#endif /* KAJO_AMIGA_DEBUG_SIDEBAR_H */
