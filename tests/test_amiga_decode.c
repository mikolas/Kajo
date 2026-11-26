#include "../src/amiga/amiga_decode.h"

#include <glib.h>
#include <string.h>

static void
test_sr_flags(void)
{
  /* 0x271F: S=1, IPL=7, X=1 N=1 Z=1 V=1 C=1, T=0 */
  AmigaSrFlags f = amiga_decode_sr(0x271Fu);
  g_assert_false(f.trace);
  g_assert_true(f.supervisor);
  g_assert_cmpuint(f.ipl, ==, 7);
  g_assert_true(f.extend);
  g_assert_true(f.negative);
  g_assert_true(f.zero);
  g_assert_true(f.overflow);
  g_assert_true(f.carry);

  /* 0xA000: T=1, S=1, IPL=0, CCR=0 */
  AmigaSrFlags t = amiga_decode_sr(0xA000u);
  g_assert_true(t.trace);
  g_assert_true(t.supervisor);
  g_assert_cmpuint(t.ipl, ==, 0);
  g_assert_false(t.extend);
  g_assert_false(t.negative);
  g_assert_false(t.zero);
  g_assert_false(t.overflow);
  g_assert_false(t.carry);
}

static void
test_m68k_cycle_estimator(void)
{
  AmigaCycleInfo c1 = amiga_m68k_line_cycles("  move.l d0,d1 ; comment");
  g_assert_true(c1.has_cycles);
  g_assert_cmpuint(c1.min_cycles, ==, 4);
  g_assert_cmpuint(c1.max_cycles, ==, 4);
  g_assert_cmpuint(c1.reads, ==, 1);
  g_assert_cmpuint(c1.writes, ==, 0);
  g_assert_false(c1.variable_shift);

  AmigaCycleInfo c2 = amiga_m68k_line_cycles("  move.l #$12345678,(a0)+");
  g_assert_true(c2.has_cycles);
  g_assert_cmpuint(c2.min_cycles, ==, 20);
  g_assert_cmpuint(c2.reads, ==, 3);
  g_assert_cmpuint(c2.writes, ==, 2);

  AmigaCycleInfo c3 = amiga_m68k_line_cycles("  lsl.w d4,d0");
  g_assert_true(c3.has_cycles);
  g_assert_true(c3.variable_shift);

  AmigaCycleInfo c4 = amiga_m68k_line_cycles("  bne.s .loop");
  g_assert_true(c4.has_cycles);
  g_assert_true(c4.is_branch);
  g_assert_cmpuint(c4.min_cycles, <=, c4.max_cycles);

  AmigaCycleInfo comment_only = amiga_m68k_line_cycles("  ; pure comment line");
  g_assert_false(comment_only.has_cycles);

  AmigaCycleInfo label_only = amiga_m68k_line_cycles(".loop:");
  g_assert_false(label_only.has_cycles);

  AmigaCycleInfo dir_only = amiga_m68k_line_cycles("  section code,code_c");
  g_assert_false(dir_only.has_cycles);

  const char *lines[] = {
    ".loop:",
    "  moveq #0,d0",
    "  add.w d0,d1",
    "  rts",
  };
  AmigaCycleSum sum = amiga_m68k_sum_cycles(lines, G_N_ELEMENTS(lines));
  g_assert_cmpuint(sum.instr_count, ==, 3);
  g_assert_cmpuint(sum.min_cycles, ==, 24); /* 4 + 4 + 16 */
  g_assert_cmpuint(sum.max_cycles, ==, 24);
  g_assert_false(sum.has_variable_shift);
}

static void
test_hw_reg_and_lvo_and_blitter_rgb(void)
{
  const AmigaHwRegInfo *intena = amiga_hw_reg_lookup(0x09Au);
  g_assert_nonnull(intena);
  g_assert_cmpstr(intena->name, ==, "INTENA");
  g_assert_cmpint(intena->access, ==, AMIGA_REG_W);

  const AmigaHwRegInfo *color00 = amiga_hw_reg_lookup(0x180u);
  g_assert_nonnull(color00);
  g_assert_cmpstr(color00->name, ==, "COLOR00");

  g_assert_null(amiga_hw_reg_lookup(0x3FFu));

  const char *forbid = amiga_lvo_lookup("exec", -132);
  g_assert_nonnull(forbid);
  g_assert_cmpstr(forbid, ==, "_LVOForbid");

  const char *dos_open = amiga_lvo_lookup("dos", -30);
  g_assert_nonnull(dos_open);
  g_assert_cmpstr(dos_open, ==, "_LVOOpen");

  g_assert_null(amiga_lvo_lookup("exec", -9999));

  AmigaBltcon0Info blt = amiga_decode_bltcon0(0x0FCAu);
  g_assert_cmpuint(blt.ash, ==, 0);
  g_assert_cmpuint(blt.lf_byte, ==, 0xCAu);
  g_assert_true(blt.use_a);
  g_assert_true(blt.use_b);
  g_assert_true(blt.use_c);
  g_assert_true(blt.use_d);

  uint8_t r = 0, g = 0, b = 0;
  amiga_decode_ocs_rgb12(0x0F80u, &r, &g, &b);
  g_assert_cmpuint(r, ==, 0xFFu);
  g_assert_cmpuint(g, ==, 0x88u);
  g_assert_cmpuint(b, ==, 0x00u);
}

static void
test_copper_disasm_and_scan(void)
{
  /* Assemble a valid Copperlist at index 16 inside 256-byte Chip RAM buffer:
   * +0: MOVE $0180,$0F00 (COLOR00 = red)
   * +4: WAIT V=$2C, H=$07, BFD=1 ($2C07, $FFFE)
   * +8: MOVE $0058,$0000 (BLTSIZE - blitter register)
   * +12: WAIT line255+ ($FFDF, $FFFE)
   * +16: END ($FFFF, $FFFE) */
  uint8_t chip[256];
  memset(chip, 0, sizeof(chip));
  const uint16_t words[] = {
    0x0180u, 0x0F00u,
    0x2C07u, 0xFFFEu,
    0x0058u, 0x0000u,
    0xFFDFu, 0xFFFEu,
    0xFFFFu, 0xFFFEu,
  };
  for (size_t i = 0; i < G_N_ELEMENTS(words); i++) {
    chip[16 + i * 2]     = (uint8_t)(words[i] >> 8);
    chip[16 + i * 2 + 1] = (uint8_t)(words[i] & 0xFFu);
  }

  AmigaCopInsn insns[8];
  size_t n = amiga_copper_disasm(chip + 16, sizeof(words) * 2, 0x00010010u, 8, insns);
  g_assert_cmpuint(n, ==, 5);
  g_assert_cmpint(insns[0].kind, ==, AMIGA_COP_MOVE);
  g_assert_true(insns[0].is_display_reg);
  g_assert_false(insns[0].is_blitter_reg);

  g_assert_cmpint(insns[1].kind, ==, AMIGA_COP_WAIT);
  g_assert_cmpuint(insns[1].vp, ==, 0x2Cu);
  g_assert_true(insns[1].bfd);

  g_assert_cmpint(insns[2].kind, ==, AMIGA_COP_MOVE);
  g_assert_true(insns[2].is_blitter_reg);

  g_assert_true(insns[3].is_line255);
  g_assert_cmpint(insns[4].kind, ==, AMIGA_COP_END);

  uint32_t hits[4] = {0};
  size_t nhits = amiga_copper_scan_chipram(chip, sizeof(chip), hits, 4);
  g_assert_cmpuint(nhits, >=, 1);
}

static void
test_bitplane_render_and_probe(void)
{
  /* 16x2 planar image, 2 bitplanes:
   * row 0: plane0 = 0xFF00, plane1 = 0x0FF0
   *   x=0..3:  p0=1,p1=0 -> idx 1 (0xF00 red)
   *   x=4..7:  p0=1,p1=1 -> idx 3 (0xFFF white)
   *   x=8..11: p0=0,p1=1 -> idx 2 (0x0F0 green)
   *   x=12..15:p0=0,p1=0 -> idx 0 (0x000 black) */
  uint8_t chip[256];
  memset(chip, 0, sizeof(chip));
  chip[0] = 0xFFu; chip[1] = 0x00u; /* plane 0 row 0 */
  chip[16] = 0x0Fu; chip[17] = 0xF0u; /* plane 1 row 0 */

  AmigaBitplaneParams p = {
    .width = 16,
    .height = 2,
    .planes = 2,
    .layout = AMIGA_BPL_PLANAR,
    .plane_mask = 0x3Fu,
    .plane_ptrs = {0, 16, 0, 0, 0, 0},
    .palette_ocs = {0x000u, 0xF00u, 0x0F0u, 0xFFFu},
  };
  uint8_t rgba[16 * 2 * 4];
  g_assert_true(amiga_bitplane_render(chip, sizeof(chip), &p, rgba));

  /* Pixel (0, 0) -> idx 1 -> red (255, 0, 0, 255) */
  g_assert_cmpuint(rgba[0], ==, 0xFFu);
  g_assert_cmpuint(rgba[1], ==, 0x00u);
  g_assert_cmpuint(rgba[2], ==, 0x00u);
  g_assert_cmpuint(rgba[3], ==, 0xFFu);

  /* Pixel (5, 0) -> idx 3 -> white */
  g_assert_cmpuint(rgba[5 * 4 + 0], ==, 0xFFu);
  g_assert_cmpuint(rgba[5 * 4 + 1], ==, 0xFFu);
  g_assert_cmpuint(rgba[5 * 4 + 2], ==, 0xFFu);

  /* Pixel (9, 0) -> idx 2 -> green */
  g_assert_cmpuint(rgba[9 * 4 + 1], ==, 0xFFu);

  AmigaPixelProbe probe = {0};
  g_assert_true(amiga_bitplane_probe_pixel(chip, sizeof(chip), &p, 9, 0, &probe));
  g_assert_cmpuint(probe.color_index, ==, 2);
  g_assert_cmpuint(probe.ocs_rgb, ==, 0x0F0u);

  /* Out-of-bounds probe fails cleanly */
  g_assert_false(amiga_bitplane_probe_pixel(chip, sizeof(chip), &p, 16, 0, &probe));

  /* EHB & HAM6 sanity */
  AmigaBitplaneParams ehb = p;
  ehb.planes = 6;
  ehb.ehb = true;
  g_assert_true(amiga_bitplane_render(chip, sizeof(chip), &ehb, rgba));

  AmigaBitplaneParams ham = p;
  ham.planes = 6;
  ham.ham6 = true;
  g_assert_true(amiga_bitplane_render(chip, sizeof(chip), &ham, rgba));
}

static void
test_dma_budget(void)
{
  AmigaDmaInput lores = {
    .ntsc = false,
    .hires = false,
    .bitplanes = 4,
    .blitter_ch = 3,
    .blit_w_words = 10,
    .blit_h_lines = 64,
    .blits_per_frame = 2,
    .copper_moves_per_frame = 20,
    .sprites_active = 2,
    .audio_channels = 2,
  };
  AmigaDmaBudget b_lo = amiga_dma_calc_budget(&lores);
  g_assert_cmpuint(b_lo.total_color_clocks_line, ==, 227);
  g_assert_true(b_lo.odd_slot_margin > 0);
  g_assert_true(b_lo.odd_utilization_pct > 0.0 && b_lo.odd_utilization_pct < 100.0);

  AmigaDmaInput hires6 = lores;
  hires6.hires = true;
  hires6.bitplanes = 4;
  AmigaDmaBudget b_hi = amiga_dma_calc_budget(&hires6);
  g_assert_cmpuint(b_hi.bpl_slots_line, >, b_lo.bpl_slots_line);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/decode/sr-flags", test_sr_flags);
  g_test_add_func("/amiga/decode/m68k-cycle-estimator", test_m68k_cycle_estimator);
  g_test_add_func("/amiga/decode/hw-reg-lvo-blitter-rgb", test_hw_reg_and_lvo_and_blitter_rgb);
  g_test_add_func("/amiga/decode/copper-disasm-and-scan", test_copper_disasm_and_scan);
  g_test_add_func("/amiga/decode/bitplane-render-and-probe", test_bitplane_render_and_probe);
  g_test_add_func("/amiga/decode/dma-budget", test_dma_budget);
  return g_test_run();
}
