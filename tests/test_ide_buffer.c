#include "../src/ide/ide_buffer.h"
#include "../src/ide/ide_picker.h"
#include "../src/ide/ide_search.h"
#include "../src/ide/ide_tree.h"

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

static void
test_git_porcelain_fuzzy_and_search_parsers(void)
{
  /* 1. Git porcelain parser */
  const char *porcelain =
      " M src/ide/ide_window.c\n"
      "A  src/ide/ide_fsm.c\n"
      "?? src/ide/ide_markdown.c\n"
      " D old_file.c\n";
  GHashTable *git_map = ide_tree_parse_git_porcelain(porcelain);
  g_assert_cmpstr(g_hash_table_lookup(git_map, "src/ide/ide_window.c"), ==, "M");
  g_assert_cmpstr(g_hash_table_lookup(git_map, "src/ide/ide_fsm.c"), ==, "A");
  g_assert_cmpstr(g_hash_table_lookup(git_map, "src/ide/ide_markdown.c"), ==, "U");
  g_assert_cmpstr(g_hash_table_lookup(git_map, "old_file.c"), ==, "D");
  g_hash_table_destroy(git_map);

  /* 2. Subsequence fuzzy scorer */
  gint s_exact = ide_fuzzy_score("ide_win", "src/ide/ide_window.c");
  gint s_sparse = ide_fuzzy_score("ide_win", "src/index_de_winter.c");
  gint s_none = ide_fuzzy_score("zzz", "src/ide/ide_window.c");
  g_assert_cmpint(s_exact, >, s_sparse);
  g_assert_cmpint(s_sparse, >, 0);
  g_assert_cmpint(s_none, ==, 0);

  /* 3. Search & Replace engine (whole-word + regex) */
  guint matches = 0;
  g_autofree char *rep = ide_replace_text_all("foo foobar foo",
                                              "foo",
                                              "bar",
                                              TRUE,
                                              TRUE, /* whole_word */
                                              FALSE,
                                              &matches);
  g_assert_cmpuint(matches, ==, 2);
  g_assert_cmpstr(rep, ==, "bar foobar bar");

  /* 4. Compiler error regex parser */
  IdeBuildIssue *iss = ide_parse_compiler_error_line("/home/mikolas/src/Kajo",
                                                     "../src/ide/ide_window.c:141:5: error: undeclared identifier 'x'");
  g_assert_nonnull(iss);
  g_assert_cmpuint(iss->line_1based, ==, 141);
  g_assert_cmpuint(iss->col_1based, ==, 5);
  g_assert_cmpint(iss->severity, ==, IDE_DIAG_ERROR);
  g_assert_cmpstr(iss->message, ==, "undeclared identifier 'x'");
  ide_build_issue_free(iss);
}

static gboolean
iter_has_tag_named(GtkTextBuffer *tbuf, gint char_offset, const char *tag_name)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(tbuf);
  GtkTextTag *tag = gtk_text_tag_table_lookup(table, tag_name);
  if (tag == NULL) {
    return FALSE;
  }
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_offset(tbuf, &it, char_offset);
  return gtk_text_iter_has_tag(&it, tag);
}

static void
test_semantic_token_highlighter(void)
{
  IdeBuffer *buf = ide_buffer_new("sample.c");
  const char *code =
      "static gboolean\n"
      "ide_window_dispatch(KajoIdeWindow *self)\n"
      "{\n"
      "  if (self->state == IDE_ACTION_SAVE) return TRUE; /* ok */\n"
      "}\n";
  ide_buffer_set_text(buf, code);

  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  /* "static" at offset 0 -> ide-syn-kw */
  g_assert_true(iter_has_tag_named(tbuf, 1, "ide-syn-kw"));
  /* "gboolean" at offset 7 -> ide-syn-type */
  g_assert_true(iter_has_tag_named(tbuf, 8, "ide-syn-type"));
  /* "ide_window_dispatch" at offset 16 -> ide-syn-fn */
  g_assert_true(iter_has_tag_named(tbuf, 18, "ide-syn-fn"));
  /* "KajoIdeWindow" at offset 36 -> ide-syn-type */
  g_assert_true(iter_has_tag_named(tbuf, 38, "ide-syn-type"));
  /* "self" at offset 51 -> ide-syn-var */
  g_assert_true(iter_has_tag_named(tbuf, 52, "ide-syn-var"));
  /* "if" on line 4 -> ide-syn-ctrl */
  const char *if_pos = strstr(code, "if (");
  g_assert_nonnull(if_pos);
  g_assert_true(iter_has_tag_named(tbuf, (gint)(if_pos - code), "ide-syn-ctrl"));
  /* "IDE_ACTION_SAVE" -> ide-syn-mac */
  const char *mac_pos = strstr(code, "IDE_ACTION_SAVE");
  g_assert_nonnull(mac_pos);
  g_assert_true(iter_has_tag_named(tbuf, (gint)(mac_pos - code + 2), "ide-syn-mac"));
  /* Block comment -> ide-syn-cmt */
  const char *cmt_pos = strstr(code, "/* ok */");
  g_assert_nonnull(cmt_pos);
  g_assert_true(iter_has_tag_named(tbuf, (gint)(cmt_pos - code + 2), "ide-syn-cmt"));

  ide_buffer_free(buf);
}

static char *
tag_signature(GtkTextBuffer *tbuf)
{
  GString *sig = g_string_new(NULL);
  gint n = gtk_text_buffer_get_char_count(tbuf);
  for (gint i = 0; i < n; i++) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(tbuf, &it, i);
    GSList *tags = gtk_text_iter_get_tags(&it);
    GPtrArray *names = g_ptr_array_new();
    for (GSList *l = tags; l; l = l->next) {
      gchar *nm = NULL;
      g_object_get(l->data, "name", &nm, NULL);
      if (nm != NULL && g_str_has_prefix(nm, "ide-syn-")) {
        g_ptr_array_add(names, nm);
      } else {
        g_free(nm);
      }
    }
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
    for (guint k = 0; k < names->len; k++) {
      g_string_append(sig, g_ptr_array_index(names, k));
      g_string_append_c(sig, ',');
    }
    g_string_append_c(sig, ';');
    g_ptr_array_set_free_func(names, g_free);
    g_ptr_array_free(names, TRUE);
    g_slist_free(tags);
  }
  return g_string_free(sig, FALSE);
}

/* Incremental tree-sitter re-highlighting after a sequence of edits must leave
 * exactly the tags a from-scratch highlight produces. */
static void
test_treesitter_incremental_tags_match_fresh(void)
{
  const char *files[] = { "A.java", "m.go", "x.c" };
  const char *inits[] = {
    "class A {\n  int f() { return 1; }\n}\n",
    "package m\nfunc f() int { return 1 }\n",
    "int f(void) { return 1; }\n",
  };
  for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
    IdeBuffer *buf = ide_buffer_new(files[i]);
    ide_buffer_set_text(buf, inits[i]);
    GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));

    /* insert a comment opener mid-file, then close it, then delete a chunk */
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(tbuf, &it, 8);
    gtk_text_buffer_insert(tbuf, &it, "/* ", -1);
    gtk_text_buffer_get_iter_at_offset(tbuf, &it, 14);
    gtk_text_buffer_insert(tbuf, &it, " */", -1);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(tbuf, &a, 2);
    gtk_text_buffer_get_iter_at_offset(tbuf, &b, 5);
    gtk_text_buffer_delete(tbuf, &a, &b);

    g_autofree char *incremental = tag_signature(tbuf);

    g_autofree char *final_text = ide_buffer_get_text(buf);
    IdeBuffer *fresh = ide_buffer_new(files[i]);
    ide_buffer_set_text(fresh, final_text);
    g_autofree char *expected = tag_signature(GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(fresh)));

    g_assert_cmpstr(incremental, ==, expected);
    ide_buffer_free(fresh);
    ide_buffer_free(buf);
  }
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ide/buffer/utf8-utf16-lsp-coordinates", test_utf8_utf16_lsp_coordinates);
  g_test_add_func("/ide/buffer/nordic-structural-text-objects", test_nordic_structural_text_objects);
  g_test_add_func("/ide/buffer/line-diffs-and-unified-diff", test_line_diffs_and_unified_diff);
  g_test_add_func("/ide/buffer/alternate-path-and-atomic-buffer", test_alternate_path_and_atomic_buffer);
  g_test_add_func("/ide/buffer/git-porcelain-fuzzy-and-search", test_git_porcelain_fuzzy_and_search_parsers);
  g_test_add_func("/ide/buffer/semantic-token-highlighter", test_semantic_token_highlighter);
  g_test_add_func("/ide/buffer/treesitter-incremental-tags", test_treesitter_incremental_tags_match_fresh);

  return g_test_run();
}
