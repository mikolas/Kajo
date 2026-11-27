#ifndef KAJO_AMIGA_DISASM_VIEW_H
#define KAJO_AMIGA_DISASM_VIEW_H

#include "amiga_disasm.h"
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_DISASM_VIEW (amiga_disasm_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaDisasmView, amiga_disasm_view, AMIGA, DISASM_VIEW, GtkBox)

typedef struct {
  void (*fetch_disasm)(AmigaDisasmView *view, uint32_t addr, int count, gpointer user_data);
  void (*toggle_bp)(AmigaDisasmView *view, uint32_t addr, gpointer user_data);
  void (*run_to_cursor)(AmigaDisasmView *view, uint32_t addr, gpointer user_data);
  void (*set_pc)(AmigaDisasmView *view, uint32_t addr, gpointer user_data);
  void (*patch_word)(AmigaDisasmView *view, uint32_t addr, uint16_t word_val, gpointer user_data);
  void (*toggle_history)(AmigaDisasmView *view, bool enabled, gpointer user_data);
  void (*selection_cycles)(AmigaDisasmView *view, const char *summary, gpointer user_data);
  uint32_t (*resolve_expr)(AmigaDisasmView *view, const char *expr, bool *ok, gpointer user_data);
} AmigaDisasmCallbacks;

AmigaDisasmView *amiga_disasm_view_new(void);

void amiga_disasm_view_set_callbacks(AmigaDisasmView            *self,
                                     const AmigaDisasmCallbacks *cbs,
                                     gpointer                    user_data);

/* Takes ownership? No, copies row presentation state cleanly. bp_set maps GUINT_TO_POINTER(addr)->1. */
void amiga_disasm_view_set_rows(AmigaDisasmView *self,
                                GPtrArray       *rows,
                                uint32_t         current_pc,
                                GHashTable      *bp_set);

void amiga_disasm_view_go_to(AmigaDisasmView *self, uint32_t addr, bool push_history);
void amiga_disasm_view_focus_goto(AmigaDisasmView *self);
bool amiga_disasm_view_nav_back(AmigaDisasmView *self);
bool amiga_disasm_view_nav_fwd(AmigaDisasmView *self);
bool amiga_disasm_view_get_follow_pc(AmigaDisasmView *self);
uint32_t amiga_disasm_view_get_base_addr(AmigaDisasmView *self);

G_END_DECLS

#endif /* KAJO_AMIGA_DISASM_VIEW_H */
