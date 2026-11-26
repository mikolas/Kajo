#include "../src/amiga/amiga_disasm.h"

#include <glib.h>

static void
test_disasm_parse_and_enrichment(void)
{
  const char *raw =
      "Segment 00 ($00020000)\n"
      "00020000 33fc 0f00 00df f180  move.w #$0f00,$00dff180\n"
      "00020008 4eae ff7c            jsr -$0084(a6) == $00f81234\n"
      "0002000c 66f2                 bne.b #$f2 == $00020000 (T)\n"
      "0002000e c300                 abcd d0,d1\n";

  AmigaSymbolTable *syms = amiga_symbols_new();
  amiga_symbols_add_symbol(syms, "start_loop", 0, 0x0u, 0);
  AmigaSegmentList sl = { .segments = g_ptr_array_new_with_free_func(g_free) };
  AmigaSegmentInfo *si = g_new0(AmigaSegmentInfo, 1);
  si->index = 0; si->address = 0x00020000u; si->size = 0x100u;
  g_ptr_array_add(sl.segments, si);
  amiga_symbols_apply_segments(syms, &sl);
  g_ptr_array_unref(sl.segments);

  g_autoptr(GPtrArray) rows = NULL;
  g_assert_true(amiga_disasm_parse(raw, syms, &rows));
  g_assert_cmpuint(rows->len, ==, 5);

  AmigaDisasmRow *r0 = g_ptr_array_index(rows, 0);
  g_assert_true(r0->is_header);

  AmigaDisasmRow *r1 = g_ptr_array_index(rows, 1);
  g_assert_false(r1->is_header);
  g_assert_cmpuint(r1->addr, ==, 0x00020000u);
  g_assert_cmpstr(r1->symbol_name, ==, "start_loop");
  g_assert_cmpstr(r1->mnemonic, ==, "move.w");
  g_assert_nonnull(r1->hw_reg_name);
  g_assert_nonnull(strstr(r1->hw_reg_name, "COLOR00"));
  g_assert_true(r1->cycles.has_cycles);

  AmigaDisasmRow *r2 = g_ptr_array_index(rows, 2);
  g_assert_cmpstr(r2->lvo_name, ==, "_LVOForbid");
  g_assert_true(r2->has_ea_value);
  g_assert_cmpuint(r2->ea_value, ==, 0x00F81234u);

  AmigaDisasmRow *r3 = g_ptr_array_index(rows, 3);
  g_assert_cmpint(r3->branch_taken, ==, 1);
  g_assert_true(r3->has_target_addr);
  g_assert_cmpuint(r3->target_addr, ==, 0x00020000u);

  AmigaDisasmRow *r4 = g_ptr_array_index(rows, 4);
  g_assert_cmpstr(r4->mnemonic, ==, "abcd");

  amiga_symbols_free(syms);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/disasm/parse-and-enrichment", test_disasm_parse_and_enrichment);
  return g_test_run();
}
