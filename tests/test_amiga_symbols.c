#include "../src/amiga/amiga_symbols.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void
push_be32(GByteArray *arr, uint32_t v)
{
  uint8_t b[4] = {
    (uint8_t)((v >> 24) & 0xFFu),
    (uint8_t)((v >> 16) & 0xFFu),
    (uint8_t)((v >> 8)  & 0xFFu),
    (uint8_t)(v & 0xFFu),
  };
  g_byte_array_append(arr, b, 4);
}

static void
test_hunk_symbol_debug_line_and_relocation(void)
{
  /* Build synthetic Amiga Hunk exe:
   * HUNK_HEADER (1 code hunk of 8 longs = 32 bytes)
   * HUNK_CODE (8 longs)
   * HUNK_SYMBOL (_start @ 0x0, _main @ 0x10)
   * HUNK_DEBUG "LINE" ("src/main.s", line 12@0x0, line 15@0x10, line 20@0x18)
   * HUNK_END */
  GByteArray *b = g_byte_array_new();
  push_be32(b, 0x000003F3u); /* HUNK_HEADER */
  push_be32(b, 0u);          /* no libs */
  push_be32(b, 1u);          /* table size */
  push_be32(b, 0u);          /* first */
  push_be32(b, 0u);          /* last */
  push_be32(b, 8u);          /* hunk 0 size = 8 longs */

  push_be32(b, 0x000003E9u); /* HUNK_CODE */
  push_be32(b, 8u);
  for (int i = 0; i < 8; i++) push_be32(b, 0x4E714E71u);

  push_be32(b, 0x000003F0u); /* HUNK_SYMBOL */
  push_be32(b, 2u);          /* "_start\0\0" = 8 bytes (2 longs) */
  g_byte_array_append(b, (const guint8 *)"_start\0\0", 8);
  push_be32(b, 0x00u);
  push_be32(b, 2u);          /* "_main\0\0\0" = 8 bytes (2 longs) */
  g_byte_array_append(b, (const guint8 *)"_main\0\0\0", 8);
  push_be32(b, 0x10u);
  push_be32(b, 0u);          /* end symbol table */

  push_be32(b, 0x000003F1u); /* HUNK_DEBUG */
  push_be32(b, 12u);         /* 12 longs */
  push_be32(b, 0u);          /* base offset */
  push_be32(b, 0x4C494E45u); /* "LINE" */
  push_be32(b, 3u);          /* "src/main.s\0\0" = 12 bytes (3 longs) */
  g_byte_array_append(b, (const guint8 *)"src/main.s\0\0", 12);
  push_be32(b, 12u); push_be32(b, 0x00u);
  push_be32(b, 15u); push_be32(b, 0x10u);
  push_be32(b, 20u); push_be32(b, 0x18u);

  push_be32(b, 0x000003F2u); /* HUNK_END */

  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-sym-XXXXXX", &err);
  g_autofree char *exe = g_build_filename(tmp, "demo.exe", NULL);
  g_assert_true(g_file_set_contents(exe, (const char *)b->data, (gssize)b->len, &err));
  g_byte_array_unref(b);

  AmigaSymbolTable *tbl = amiga_symbols_new();
  g_assert_true(amiga_symbols_load_hunk(tbl, exe, &err));
  g_assert_no_error(err);

  AmigaSegmentList sl = { .segments = g_ptr_array_new_with_free_func(g_free) };
  AmigaSegmentInfo *si = g_new0(AmigaSegmentInfo, 1);
  si->index = 0; si->address = 0x00020000u; si->size = 32;
  g_ptr_array_add(sl.segments, si);
  amiga_symbols_apply_segments(tbl, &sl);
  g_ptr_array_unref(sl.segments);

  const char *sym = NULL;
  uint32_t delta = 0;
  g_assert_true(amiga_symbols_resolve_addr(tbl, 0x00020014u, &sym, &delta));
  g_assert_cmpstr(sym, ==, "_main");
  g_assert_cmpuint(delta, ==, 4u);

  const char *file = NULL;
  uint32_t line = 0;
  g_assert_true(amiga_symbols_addr_to_line(tbl, 0x00020014u, &file, &line));
  g_assert_cmpstr(file, ==, "src/main.s");
  g_assert_cmpuint(line, ==, 15u);

  uint32_t saddr = 0, eaddr = 0;
  g_assert_true(amiga_symbols_line_to_addr(tbl, "main.s", 15, &saddr, &eaddr));
  g_assert_cmpuint(saddr, ==, 0x00020010u);
  g_assert_cmpuint(eaddr, ==, 0x00020018u);

  uint32_t main_addr = 0;
  g_assert_true(amiga_symbols_lookup_name(tbl, "_main", &main_addr));
  g_assert_cmpuint(main_addr, ==, 0x00020010u);

  amiga_symbols_free(tbl);

  /* Truncated Hunk error check */
  g_autofree char *bad = g_build_filename(tmp, "bad.exe", NULL);
  g_assert_true(g_file_set_contents(bad, "BAD!", 4, &err));
  AmigaSymbolTable *bt = amiga_symbols_new();
  g_assert_false(amiga_symbols_load_hunk(bt, bad, &err));
  g_clear_error(&err);
  amiga_symbols_free(bt);

  g_unlink(exe);
  g_unlink(bad);
  g_rmdir(tmp);
}

#ifdef HAVE_ELFUTILS
static void
test_elf_dwarf_loader(void)
{
  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-elf-XXXXXX", &err);
  g_autofree char *src = g_build_filename(tmp, "sample.c", NULL);
  g_autofree char *elf = g_build_filename(tmp, "sample.elf", NULL);
  g_assert_true(g_file_set_contents(
      src, "int helper(int x) { return x + 1; }\nint main(void) { return helper(2); }\n",
      -1, &err));

  g_autofree char *cmd = g_strdup_printf("cc -g -O0 %s -o %s", src, elf);
  int status = 0;
  g_assert_true(g_spawn_command_line_sync(cmd, NULL, NULL, &status, &err));
  g_assert_cmpint(status, ==, 0);

  AmigaSymbolTable *tbl = amiga_symbols_new();
  g_assert_true(amiga_symbols_load_elf(tbl, elf, &err));
  g_assert_no_error(err);

  uint32_t addr = 0;
  g_assert_true(amiga_symbols_lookup_name(tbl, "main", &addr));
  g_assert_cmpuint(addr, >, 0);
  g_assert_cmpuint(amiga_symbols_get_lines(tbl)->len, >=, 2);

  amiga_symbols_free(tbl);
  g_unlink(src);
  g_unlink(elf);
  g_rmdir(tmp);
}
#endif

static void
test_map_and_outlines(void)
{
  const char *asm_src =
      "  section code,code_c\n"
      "COLOR00 equ $dff180\n"
      "WAITBLIT macro\n"
      "  endm\n"
      "init_copper:\n"
      "  rts\n";
  g_autoptr(GPtrArray) a_out = amiga_symbols_extract_outline(asm_src, false);
  g_assert_cmpuint(a_out->len, ==, 4);

  const char *c_src =
      "static int helper(int x) {\n"
      "  if (x > 0) { return x; }\n"
      "  return 0;\n"
      "}\n"
      "void effect_loop(void) {\n"
      "}\n";
  g_autoptr(GPtrArray) c_out = amiga_symbols_extract_outline(c_src, true);
  g_assert_cmpuint(c_out->len, ==, 2);
  AmigaOutlineItem *f0 = g_ptr_array_index(c_out, 0);
  g_assert_cmpstr(f0->name, ==, "helper");

  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-map-XXXXXX", &err);
  g_autofree char *mp = g_build_filename(tmp, "demo.map", NULL);
  g_assert_true(g_file_set_contents(
      mp, " .text 0x00000000 0x40 obj/main.o\n  0x00000018   copper_init\n", -1, &err));
  AmigaSymbolTable *tbl = amiga_symbols_new();
  g_assert_true(amiga_symbols_load_map(tbl, mp, &err));
  uint32_t addr = 0;
  g_assert_true(amiga_symbols_lookup_name(tbl, "copper_init", &addr));
  g_assert_cmpuint(addr, ==, 0x18u);
  amiga_symbols_free(tbl);
  g_unlink(mp);
  g_rmdir(tmp);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/symbols/hunk-debug-line-and-relocation", test_hunk_symbol_debug_line_and_relocation);
#ifdef HAVE_ELFUTILS
  g_test_add_func("/amiga/symbols/elf-dwarf-loader", test_elf_dwarf_loader);
#endif
  g_test_add_func("/amiga/symbols/map-and-outlines", test_map_and_outlines);
  return g_test_run();
}
