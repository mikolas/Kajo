#ifndef KAJO_AMIGA_SYMBOLS_H
#define KAJO_AMIGA_SYMBOLS_H

/* Pure GTK-free C17 debug symbol, DWARF3/Hunk line-table, runtime segment
 * relocation, and ASM/C outline extractor. */

#include "amiga_rsp.h"
#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct {
  char     *name;
  uint32_t  hunk_index;
  uint32_t  offset;
  uint32_t  size;
} AmigaSymbol;

typedef struct {
  char     *file;
  uint32_t  line;
  uint32_t  hunk_index;
  uint32_t  offset;
  uint32_t  end_offset; /* exclusive upper bound for vCont;r statement stepping */
} AmigaLineEntry;

typedef enum {
  AMIGA_OUTLINE_SECTION = 0,
  AMIGA_OUTLINE_FUNCTION,
  AMIGA_OUTLINE_LABEL,
  AMIGA_OUTLINE_EQU,
  AMIGA_OUTLINE_MACRO,
} AmigaOutlineKind;

typedef struct {
  AmigaOutlineKind  kind;
  char             *name;
  guint             line; /* 1-based */
} AmigaOutlineItem;

typedef struct _AmigaSymbolTable AmigaSymbolTable;

void amiga_outline_item_free(AmigaOutlineItem *it);

AmigaSymbolTable *amiga_symbols_new(void);
void              amiga_symbols_free(AmigaSymbolTable *tbl);

void amiga_symbols_add_symbol(AmigaSymbolTable *tbl,
                              const char       *name,
                              uint32_t          hunk_index,
                              uint32_t          offset,
                              uint32_t          size);
void amiga_symbols_add_line(AmigaSymbolTable *tbl,
                            const char       *file,
                            uint32_t          line,
                            uint32_t          hunk_index,
                            uint32_t          offset);
/* Recomputes end_offset for each line entry from adjacent statement starts. */
void amiga_symbols_finalize_line_ranges(AmigaSymbolTable *tbl);

bool amiga_symbols_load_elf(AmigaSymbolTable *tbl,
                            const char       *elf_path,
                            GError          **error);
bool amiga_symbols_load_hunk(AmigaSymbolTable *tbl,
                             const char       *exe_path,
                             GError          **error);
bool amiga_symbols_load_map(AmigaSymbolTable *tbl,
                            const char       *map_path,
                            GError          **error);

void amiga_symbols_apply_segments(AmigaSymbolTable       *tbl,
                                  const AmigaSegmentList *segs);

bool amiga_symbols_resolve_addr(const AmigaSymbolTable  *tbl,
                                uint32_t                 phys_addr,
                                const char             **out_name,
                                uint32_t                *out_delta);

bool amiga_symbols_addr_to_line(const AmigaSymbolTable  *tbl,
                                uint32_t                 phys_addr,
                                const char             **out_file,
                                uint32_t                *out_line);

bool amiga_symbols_line_to_addr(const AmigaSymbolTable *tbl,
                                const char             *file,
                                uint32_t                line,
                                uint32_t               *out_start_addr,
                                uint32_t               *out_end_addr);

bool amiga_symbols_lookup_name(const AmigaSymbolTable *tbl,
                               const char             *name,
                               uint32_t               *out_phys_addr);

const GPtrArray *amiga_symbols_get_symbols(const AmigaSymbolTable *tbl);
const GPtrArray *amiga_symbols_get_lines(const AmigaSymbolTable *tbl);

/* Extracts outline entries (ASM labels/EQU/SECTION/MACRO or C functions). */
GPtrArray *amiga_symbols_extract_outline(const char *source_text, bool is_c);

G_END_DECLS

#endif /* KAJO_AMIGA_SYMBOLS_H */
