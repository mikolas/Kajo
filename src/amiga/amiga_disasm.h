#ifndef KAJO_AMIGA_DISASM_H
#define KAJO_AMIGA_DISASM_H

/* Pure GTK-free C17 parser & enricher for Amiberry `qRcmd,disasm` plain-text
 * output (m68k_disasm_2): attaches line-by-line n(r/w) cycle estimates,
 * $DFFxxx custom register names, Exec/DOS/Graphics LVOs, and relocated symbols. */

#include "amiga_decode.h"
#include "amiga_symbols.h"
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct {
  bool           is_header;
  char          *header_text;
  uint32_t       addr;
  char          *opcode_hex;
  char          *mnemonic;
  char          *operands;
  char          *symbol_name;   /* optional relocated label matching row->addr */
  char          *hw_reg_name;   /* e.g. "COLOR00 ($180)" */
  char          *lvo_name;      /* e.g. "_LVOForbid" */
  char          *comment;
  AmigaCycleInfo cycles;
  bool           has_ea_value;
  uint32_t       ea_value;
  bool           has_target_addr;
  uint32_t       target_addr;
  int            branch_taken;  /* -1 = N/A, 0 = (F), 1 = (T) */
} AmigaDisasmRow;

void amiga_disasm_row_free(AmigaDisasmRow *row);

bool amiga_disasm_parse(const char             *text,
                        const AmigaSymbolTable *syms,
                        GPtrArray             **out_rows);

G_END_DECLS

#endif /* KAJO_AMIGA_DISASM_H */
