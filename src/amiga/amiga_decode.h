#ifndef KAJO_AMIGA_DECODE_H
#define KAJO_AMIGA_DECODE_H

/* Pure GTK-free C17 decoders for Amiga 68000 / OCS-ECS-AGA:
 * - 68000 SR status register breakdown
 * - Line-by-line 68000 static cycle lookup & selection summation (v1: no loop/call unrolling)
 * - OCS/ECS/AGA $DFF000..$1FE register table, Exec/DOS/Graphics LVO lookup, BLTCON0/RGB12
 * - Copperlist disassembler & Chip RAM heuristic scanner
 * - Bitplane planar/interleaved/HAM6/EHB renderer + pixel probe
 * - Parameterized PAL/NTSC 227-color-clock DMA budget calculator */

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct {
  bool     trace;
  bool     supervisor;
  uint8_t  ipl;
  bool     extend;
  bool     negative;
  bool     zero;
  bool     overflow;
  bool     carry;
} AmigaSrFlags;

typedef struct {
  bool     has_cycles;
  uint16_t min_cycles;
  uint16_t max_cycles;
  uint8_t  reads;
  uint8_t  writes;
  bool     variable_shift; /* e.g. lsl.w d4,d0 -> 6+2n(1/0) */
  bool     is_branch;      /* e.g. bne/dbf -> 10/14 */
  char     display[24];
} AmigaCycleInfo;

typedef struct {
  uint32_t min_cycles;
  uint32_t max_cycles;
  uint32_t reads;
  uint32_t writes;
  uint32_t instr_count;
  bool     has_variable_shift;
} AmigaCycleSum;

typedef enum {
  AMIGA_REG_R = 1,
  AMIGA_REG_W = 2,
  AMIGA_REG_RW = 3,
} AmigaRegAccess;

typedef struct {
  uint16_t       offset; /* $000..$1FE */
  const char    *name;
  AmigaRegAccess access;
  const char    *summary;
} AmigaHwRegInfo;

typedef struct {
  uint16_t lf_byte;
  uint8_t  ash;
  bool     use_a;
  bool     use_b;
  bool     use_c;
  bool     use_d;
  char     equation[48];
} AmigaBltcon0Info;

typedef enum {
  AMIGA_COP_MOVE = 0,
  AMIGA_COP_WAIT,
  AMIGA_COP_SKIP,
  AMIGA_COP_END,
} AmigaCopKind;

typedef struct {
  uint32_t     addr;
  uint16_t     ir1;
  uint16_t     ir2;
  AmigaCopKind kind;
  uint8_t      vp;
  uint8_t      hp;
  bool         bfd;           /* bit 15 of IR2 for WAIT/SKIP (0 = wait blitter) */
  bool         is_line255;    /* $FFDFFFFE */
  bool         is_blitter_reg;/* MOVE to $040..$07E (requires COPCON $0002) */
  bool         is_display_reg;
  char         text[96];
  char         note[48];
} AmigaCopInsn;

typedef enum {
  AMIGA_BPL_PLANAR = 0,
  AMIGA_BPL_INTERLEAVED,
  AMIGA_BPL_LINE_2X,
} AmigaBplLayout;

typedef struct {
  uint16_t       width;
  uint16_t       height;
  uint8_t        planes;      /* 1..6 */
  bool           ham6;
  bool           ehb;
  AmigaBplLayout layout;
  uint8_t        plane_mask;  /* bit N enables plane N (0x3F = all) */
  int16_t        bpl1mod;
  int16_t        bpl2mod;
  uint32_t       plane_ptrs[6];
  uint16_t       palette_ocs[32]; /* 12-bit 0x0RGB */
} AmigaBitplaneParams;

typedef struct {
  uint16_t x;
  uint16_t y;
  uint8_t  color_index;
  uint16_t ocs_rgb;
  uint8_t  plane_bits;
  uint32_t chip_addr;
  uint8_t  bit_index; /* 7..0 (MSB first) */
} AmigaPixelProbe;

typedef struct {
  bool     ntsc;
  bool     hires;
  uint8_t  bitplanes;     /* 1..6 */
  uint8_t  blitter_ch;    /* 1..4 active channels */
  uint16_t blit_w_words;
  uint16_t blit_h_lines;
  uint16_t blits_per_frame;
  uint16_t copper_moves_per_frame;
  uint8_t  sprites_active; /* 0..8 */
  uint8_t  audio_channels; /* 0..4 */
} AmigaDmaInput;

typedef struct {
  uint16_t total_color_clocks_line; /* 227 PAL, 227 NTSC */
  uint16_t bpl_slots_line;
  uint16_t cop_slots_line;
  uint16_t free_odd_slots_visible_line;
  uint32_t avail_odd_slots_frame;
  uint32_t blitter_slots_frame;
  uint32_t copper_slots_frame;
  uint32_t used_odd_slots_frame;
  int32_t  odd_slot_margin;
  uint32_t cpu_even_cycles_frame;
  double   odd_utilization_pct;
} AmigaDmaBudget;

/* 68000 status flags & cycle estimators */
AmigaSrFlags   amiga_decode_sr(uint16_t sr);
AmigaCycleInfo amiga_m68k_line_cycles(const char *line);
AmigaCycleSum  amiga_m68k_sum_cycles(const char *const *lines, size_t n_lines);

/* Hardware registers, LVO, Blitter, and RGB12 */
const AmigaHwRegInfo *amiga_hw_reg_lookup(uint16_t offset);
const char           *amiga_lvo_lookup(const char *lib_hint, int16_t lvo_offset);
AmigaBltcon0Info      amiga_decode_bltcon0(uint16_t bltcon0);
void                  amiga_decode_ocs_rgb12(uint16_t rgb12, uint8_t *r, uint8_t *g, uint8_t *b);

/* Copperlist disassembly & Chip RAM heuristic scan */
size_t amiga_copper_disasm(const uint8_t *buf,
                           size_t         len,
                           uint32_t       base_addr,
                           size_t         max_insns,
                           AmigaCopInsn  *out_insns);
size_t amiga_copper_scan_chipram(const uint8_t *chip_ram,
                                 size_t         chip_len,
                                 uint32_t      *out_addrs,
                                 size_t         max_hits);

/* Bitplane rendering & pixel probing (out_rgba must hold width * height * 4 bytes) */
bool amiga_bitplane_render(const uint8_t             *chip_ram,
                           size_t                     chip_len,
                           const AmigaBitplaneParams *params,
                           uint8_t                   *out_rgba);
bool amiga_bitplane_probe_pixel(const uint8_t             *chip_ram,
                                size_t                     chip_len,
                                const AmigaBitplaneParams *params,
                                uint16_t                   x,
                                uint16_t                   y,
                                AmigaPixelProbe           *out_probe);

/* Parameterized PAL/NTSC DMA budget */
AmigaDmaBudget amiga_dma_calc_budget(const AmigaDmaInput *in);

G_END_DECLS

#endif /* KAJO_AMIGA_DECODE_H */
