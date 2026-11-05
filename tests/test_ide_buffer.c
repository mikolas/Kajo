#include "../src/ide/ide_buffer.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void
test_utf8_utf16_lsp_coordinates(void)
{
  /* "int ö = 🚀; x"
   * "int " = 4 bytes, 4 utf16
   * "ö"    = 2 bytes (C3 B6), 1 utf16 -> byte 6, utf16 5
   * " = "  = 3 bytes, 3 utf16         -> byte 9, utf16 8
   * "🚀"   = 4 bytes (F0 9F 9A 80), 2 utf16 (surrogate pair) -> byte 13, utf16 10
   * "; x"  = 3 bytes, 3 utf16         -> byte 16, utf16 13
   */
  const char *line = "int \xC3\xB6 = \xF0\x9F\x9A\x80; x";

  g_assert_cmpuint(ide_utf8_byte_to_utf16_col(line, 0), ==, 0);
  g_assert_cmpuint(ide_utf8_byte_to_utf16_col(line, 4), ==, 4);
  g_assert_cmpuint(ide_utf8_byte_to_utf16_col(line, 6), ==, 5);
  g_assert_cmpuint(ide_utf8_byte_to_utf16_col(line, 9), ==, 8);
  g_assert_cmpuint(ide_utf8_byte_to_utf16_col(line, 13), ==, 10);
  g_assert_cmpuint(ide_utf8_byte_to_utf16_col(line, 16), ==, 13);

  g_assert_cmpuint((guint)ide_utf16_col_to_utf8_byte(line, 0), ==, 0);
  g_assert_cmpuint((guint)ide_utf16_col_to_utf8_byte(line, 4), ==, 4);
  g_assert_cmpuint((guint)ide_utf16_col_to_utf8_byte(line, 5), ==, 6);
  g_assert_cmpuint((guint)ide_utf16_col_to_utf8_byte(line, 8), ==, 9);
  g_assert_cmpuint((guint)ide_utf16_col_to_utf8_byte(line, 10), ==, 13);
  g_assert_cmpuint((guint)ide_utf16_col_to_utf8_byte(line, 13), ==, 16);
}

static void
test_nordic_structural_text_objects(void)
{
  const char *code =
      "static int\n"
      "compute_sum(int a, int b)\n"
      "{\n"
      "  const char *msg = \"hello {world}\";\n"
      "  int arr[3] = { 1, 2, 3 };\n"
      "  return (a + b);\n"
      "}\n";

  gsize start = 0;
  gsize end = 0;

  /* 1. Quote object: 'm i q' inside "hello {world}" */
  const char *quote_pos = strstr(code, "hello");
  g_assert_nonnull(quote_pos);
  gsize cursor = (gsize)(quote_pos - code);
  g_assert_true(ide_find_text_object(code, cursor, TRUE, 'q', &start, &end));
  g_autofree char *sub_q = g_strndup(code + start, end - start);
  g_assert_cmpstr(sub_q, ==, "hello {world}");

  /* 2. Array object: 'm i a' inside [3] */
  const char *arr_pos = strstr(code, "[3]");
  g_assert_nonnull(arr_pos);
  g_assert_true(ide_find_text_object(code, (gsize)(arr_pos - code) + 1, TRUE, 'a', &start, &end));
  g_autofree char *sub_a = g_strndup(code + start, end - start);
  g_assert_cmpstr(sub_a, ==, "3");

  /* 3. Parens object: 'm a p' around (a + b) */
  const char *paren_pos = strstr(code, "a + b");
  g_assert_nonnull(paren_pos);
  g_assert_true(ide_find_text_object(code, (gsize)(paren_pos - code), FALSE, 'p', &start, &end));
  g_autofree char *sub_p = g_strndup(code + start, end - start);
  g_assert_cmpstr(sub_p, ==, "(a + b)");

  /* 4. Function object: 'm a f' around entire function ( ignores "{world}" inside string! ) */
  g_assert_true(ide_find_text_object(code, (gsize)(paren_pos - code), FALSE, 'f', &start, &end));
  g_assert_cmpuint((guint)start, ==, 0);
  g_assert_cmpuint((guint)end, ==, (guint)strlen(code));
}

static void
test_line_diffs_and_unified_diff(void)
{
  const char *old_txt = "line1\nline2\nline3\n";
  const char *new_txt = "line1\nline2_mod\nline2_added\nline3\n";

  gsize count = 0;
  g_autofree IdeDiffState *diffs = ide_compute_line_diffs(old_txt, new_txt, &count);
  g_assert_nonnull(diffs);
  g_assert_cmpuint((guint)count, >=, 4);
  g_assert_cmpint(diffs[0], ==, IDE_DIFF_NONE);

  g_autofree char *udiff = ide_format_unified_diff("src/foo.c", old_txt, new_txt);
  g_assert_nonnull(strstr(udiff, "--- a/src/foo.c"));
  g_assert_nonnull(strstr(udiff, "+++ b/src/foo.c"));
  g_assert_nonnull(strstr(udiff, "-line2"));
  g_assert_nonnull(strstr(udiff, "+line2_mod"));
}

static void
test_alternate_path_and_atomic_buffer(void)
{
  g_autofree char *alt_c = ide_resolve_alternate_path("/tmp/foo_bar.c");
  g_assert_cmpstr(alt_c, ==, "/tmp/foo_bar.h");

  g_autofree char *alt_go = ide_resolve_alternate_path("/tmp/engine.go");
  g_assert_cmpstr(alt_go, ==, "/tmp/engine_test.go");

  g_autofree char *alt_go_back = ide_resolve_alternate_path("/tmp/engine_test.go");
  g_assert_cmpstr(alt_go_back, ==, "/tmp/engine.go");

  /* Test atomic save + IdeBuffer agent snapshot & rollback */
  g_autofree char *tmp_dir = g_dir_make_tmp("kajo-ide-buf-XXXXXX", NULL);
  g_assert_nonnull(tmp_dir);
  g_autofree char *tmp_file = g_build_filename(tmp_dir, "sample.c", NULL);

  GError *err = NULL;
  g_assert_true(ide_save_file_atomic(tmp_file, "int x = 1;\n", -1, &err));
  g_assert_no_error(err);

  IdeBuffer *buf = ide_buffer_new(tmp_file);
  g_assert_true(ide_buffer_load_from_disk(buf, &err));
  g_assert_no_error(err);

  /* Breakpoints */
  g_assert_false(ide_buffer_has_breakpoint(buf, 1));
  g_assert_true(ide_buffer_toggle_breakpoint(buf, 1));
  g_assert_true(ide_buffer_has_breakpoint(buf, 1));
  g_assert_false(ide_buffer_toggle_breakpoint(buf, 1));

  /* Agent snapshot & rollback */
  ide_buffer_record_agent_snapshot(buf);
  ide_buffer_set_text(buf, "int x = 42;\nint y = 99;\n");
  g_assert_true(ide_buffer_has_agent_changes(buf));
  g_assert_cmpint(ide_buffer_get_line_diff(buf, 1), ==, IDE_DIFF_AGENT);

  g_assert_true(ide_buffer_revert_agent_changes(buf, &err));
  g_assert_no_error(err);
  g_assert_false(ide_buffer_has_agent_changes(buf));

  g_autofree char *reverted = ide_buffer_get_text(buf);
  g_assert_cmpstr(reverted, ==, "int x = 1;\n");

  ide_buffer_free(buf);
  g_unlink(tmp_file);
  g_rmdir(tmp_dir);
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ide/buffer/utf8-utf16-lsp-coordinates", test_utf8_utf16_lsp_coordinates);
  g_test_add_func("/ide/buffer/nordic-structural-text-objects", test_nordic_structural_text_objects);
  g_test_add_func("/ide/buffer/line-diffs-and-unified-diff", test_line_diffs_and_unified_diff);
  g_test_add_func("/ide/buffer/alternate-path-and-atomic-buffer", test_alternate_path_and_atomic_buffer);

  return g_test_run();
}
