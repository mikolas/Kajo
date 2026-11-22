#include "../src/ide/ide_strip_graph.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
rm_rf_simple(const char *path)
{
  if (path == NULL) {
    return;
  }
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *d = g_dir_open(path, 0, NULL);
    if (d != NULL) {
      const char *name;
      while ((name = g_dir_read_name(d)) != NULL) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        rm_rf_simple(child);
      }
      g_dir_close(d);
    }
    rmdir(path);
  } else {
    unlink(path);
  }
}

static void
write_file_helper(const char *dir, const char *rel, const char *content)
{
  g_autofree char *full = g_build_filename(dir, rel, NULL);
  g_autofree char *parent = g_path_get_dirname(full);
  g_mkdir_with_parents(parent, 0755);
  g_assert_true(g_file_set_contents(full, content, -1, NULL));
}

/* 1. Strict workspace-only path boundary filter */
static void
test_workspace_boundary_filter(void)
{
  const char *ws = "/home/user/proj";

  /* Valid workspace paths */
  g_assert_true(ide_strip_path_in_workspace("/home/user/proj/src/main.c", ws));
  g_assert_true(ide_strip_path_in_workspace("/home/user/proj/src/builder.c", ws));
  g_assert_true(ide_strip_path_in_workspace("/home/user/proj/src/my_target.rs", ws));
  g_assert_true(ide_strip_path_in_workspace("file:///home/user/proj/src/a.c", ws));
  g_assert_true(ide_strip_path_in_workspace("/home/user/proj/a/../src/main.c", ws));
  g_assert_true(ide_strip_path_in_workspace("/home/user/proj/src/main.c", "/home/user/proj/"));

  /* Reject NULL / empty / non-file URI schemes */
  g_assert_false(ide_strip_path_in_workspace(NULL, ws));
  g_assert_false(ide_strip_path_in_workspace("", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/a.c", NULL));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/a.c", ""));
  g_assert_false(ide_strip_path_in_workspace("jdt://contents/java.base/java.lang/String.class", ws));
  g_assert_false(ide_strip_path_in_workspace("jar:file:///usr/share/java/foo.jar!/A.java", ws));
  g_assert_false(ide_strip_path_in_workspace("https://example.com/a.c", ws));

  /* Strict prefix separator boundary: /home/user/proj must NOT match /home/user/proj2/a.c */
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj2/src/a.c", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/project/src/a.c", ws));
  g_assert_false(ide_strip_path_in_workspace("/usr/include/stdio.h", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/../other/a.c", ws));

  /* Excluded build / vendor / VCS directories */
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/.git/config", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/builddir/gen.c", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/build/out.c", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/sub/build/out.c", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/node_modules/pkg/index.js", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/target/debug/main.rs", ws));
  g_assert_false(ide_strip_path_in_workspace("/home/user/proj/subprojects/foo/bar.c", ws));
}

/* 2. Peek reader (disk, override_text, trailing blank trimming) */
static void
test_read_peek(void)
{
  const char *src = "line1\nline2\n  \nline4\n   \n\n";
  g_autofree char *p1 = ide_strip_graph_read_peek(NULL, 1, 3, src);
  g_assert_cmpstr(p1, ==, "line1\nline2");

  g_autofree char *p2 = ide_strip_graph_read_peek(NULL, 4, 5, src);
  g_assert_cmpstr(p2, ==, "line4");

  g_autofree char *ws = g_dir_make_tmp("kajo-strip-peek-XXXXXX", NULL);
  write_file_helper(ws, "a.c", "int f(void)\n{\n  return 1;\n}\n\n");
  g_autofree char *path = g_build_filename(ws, "a.c", NULL);
  g_autofree char *p3 = ide_strip_graph_read_peek(path, 1, 5, NULL);
  g_assert_cmpstr(p3, ==, "int f(void)\n{\n  return 1;\n}");

  g_assert_null(ide_strip_graph_read_peek("/nonexistent/kajo_no_such_file.c", 1, 5, NULL));
  rm_rf_simple(ws);
}

/* 3. Workspace indexing, enclosing symbol, CALLS/TYPES/IMPACT, and invalidation */
static void
test_source_order_calls_types_impact_and_invalidation(void)
{
  g_autofree char *ws = g_dir_make_tmp("kajo-strip-graph-XXXXXX", NULL);

  const char *types_h =
      "#ifndef TYPES_H\n"
      "#define TYPES_H\n"
      "\n"
      "typedef struct {\n"
      "  int width;\n"
      "  int height;\n"
      "} FrameConfig;\n"
      "\n"
      "typedef enum {\n"
      "  PASS_BLUR = 0,\n"
      "  PASS_GLOW = 1\n"
      "} PassMode;\n"
      "#endif\n";

  const char *fx_c =
      "#include \"types.h\"\n"
      "#include <stdio.h>\n"
      "\n"
      "static void\n"
      "kfx_dispatch_shader(int id)\n"
      "{\n"
      "  printf(\"dispatch %d\\n\", id);\n"
      "}\n"
      "\n"
      "void\n"
      "blur_down_pass(FrameConfig *cfg, PassMode mode)\n"
      "{\n"
      "  kfx_dispatch_shader(mode);\n"
      "  kfx_dispatch_shader(cfg->width);\n"
      "}\n"
      "\n"
      "void\n"
      "fx_glass_render(FrameConfig *cfg)\n"
      "{\n"
      "  printf(\"external noise\\n\");\n"
      "  blur_down_pass(cfg, PASS_BLUR);\n"
      "  kfx_dispatch_shader(99);\n"
      "  blur_down_pass(cfg, PASS_GLOW);\n"
      "  fx_glass_render(cfg); /* self-call must be skipped */\n"
      "}\n";

  const char *main_c =
      "#include \"types.h\"\n"
      "void fx_glass_render(FrameConfig *cfg);\n"
      "void blur_down_pass(FrameConfig *cfg, PassMode mode);\n"
      "\n"
      "int\n"
      "comp_output_frame(FrameConfig *cfg)\n"
      "{\n"
      "  fx_glass_render(cfg);\n"
      "  blur_down_pass(cfg, PASS_BLUR);\n"
      "  return 0;\n"
      "}\n";

  /* Also drop a file inside builddir/ to confirm scanner ignores excluded dirs */
  const char *ignored_c = "void blur_down_pass(void) {}\nvoid builddir_secret(void) {}\n";

  write_file_helper(ws, "src/types.h", types_h);
  write_file_helper(ws, "src/fx.c", fx_c);
  write_file_helper(ws, "src/main.c", main_c);
  write_file_helper(ws, "builddir/gen.c", ignored_c);

  IdeStripGraph *g = ide_strip_graph_new(ws);
  ide_strip_graph_ensure_indexed(g);
  g_assert_cmpuint(ide_strip_graph_file_count(g), ==, 3);
  g_assert_cmpuint(ide_strip_graph_symbol_count(g), >=, 6);

  g_autofree char *fx_path = g_build_filename(ws, "src/fx.c", NULL);
  g_autofree char *main_path = g_build_filename(ws, "src/main.c", NULL);
  g_autofree char *types_path = g_build_filename(ws, "src/types.h", NULL);

  /* Enclosing symbol: inside fx_glass_render */
  g_autofree char *enc_name = NULL;
  guint enc_s = 0, enc_e = 0;
  int enc_kind = -1;
  g_assert_true(ide_strip_graph_find_enclosing_symbol(
      g, fx_path, fx_c, 21, &enc_name, &enc_s, &enc_e, &enc_kind));
  g_assert_cmpstr(enc_name, ==, "fx_glass_render");
  g_assert_cmpint(enc_kind, ==, IDE_STRIP_SYM_FUNC);
  g_assert_cmpuint(enc_s, <=, 18);
  g_assert_cmpuint(enc_e, >=, 24);

  /* Enclosing symbol: inside FrameConfig struct */
  g_autofree char *t_name = NULL;
  guint ts = 0, te = 0;
  int tkind = -1;
  g_assert_true(ide_strip_graph_find_enclosing_symbol(
      g, types_path, types_h, 5, &t_name, &ts, &te, &tkind));
  g_assert_cmpstr(t_name, ==, "FrameConfig");
  g_assert_cmpint(tkind, ==, IDE_STRIP_SYM_TYPE);

  /* Enclosing symbol: outside any symbol -> full-file fallback */
  g_autofree char *f_name = NULL;
  guint fs = 0, fe = 0;
  int fkind = -1;
  g_assert_true(ide_strip_graph_find_enclosing_symbol(
      g, fx_path, fx_c, 1, &f_name, &fs, &fe, &fkind));
  g_assert_cmpstr(f_name, ==, "fx.c");
  g_assert_cmpint(fkind, ==, IDE_STRIP_SYM_FILE);
  g_assert_cmpuint(fs, ==, 1);

  /* CALLS lens on fx_glass_render: exact source order, deduplicated, WS ONLY */
  guint ext_sup = 0;
  g_autoptr(GPtrArray) calls = ide_strip_graph_extract_targets(
      g, fx_path, fx_c, enc_s, enc_e, IDE_STRIP_GRAPH_LENS_CALLS, &ext_sup);
  g_assert_cmpuint(ext_sup, >=, 1); /* printf suppressed */
  g_assert_cmpuint(calls->len, ==, 2);

  const IdeStripGraphEntry *c0 = g_ptr_array_index(calls, 0);
  const IdeStripGraphEntry *c1 = g_ptr_array_index(calls, 1);
  g_assert_cmpstr(c0->name, ==, "blur_down_pass");
  g_assert_cmpuint(c0->call_count, ==, 2);
  g_assert_cmpuint(c0->child_count, ==, 1); /* calls kfx_dispatch_shader */
  g_assert_nonnull(c0->peek_code);
  g_assert_nonnull(strstr(c0->peek_code, "blur_down_pass"));
  g_assert_true(c0->is_fallback);

  g_assert_cmpstr(c1->name, ==, "kfx_dispatch_shader");
  g_assert_cmpuint(c1->call_count, ==, 1);
  g_assert_cmpuint(c1->child_count, ==, 0); /* only calls external printf */
  g_assert_cmpuint(c0->call_line, <, c1->call_line);

  /* TYPES lens on blur_down_pass: FrameConfig then PassMode in source order */
  guint type_ext = 0;
  g_autoptr(GPtrArray) types = ide_strip_graph_extract_targets(
      g, fx_path, fx_c, 10, 15, IDE_STRIP_GRAPH_LENS_TYPES, &type_ext);
  g_assert_cmpuint(types->len, ==, 2);
  g_assert_cmpstr(((IdeStripGraphEntry *)g_ptr_array_index(types, 0))->name, ==, "FrameConfig");
  g_assert_cmpstr(((IdeStripGraphEntry *)g_ptr_array_index(types, 1))->name, ==, "PassMode");

  /* IMPACT lens on blur_down_pass: called by fx_glass_render (x2) and comp_output_frame (x1) */
  g_autoptr(GPtrArray) callers = ide_strip_graph_extract_targets(
      g, fx_path, fx_c, 10, 15, IDE_STRIP_GRAPH_LENS_IMPACT, NULL);
  g_assert_cmpuint(callers->len, ==, 2);
  gboolean saw_glass = FALSE, saw_comp = FALSE;
  for (guint i = 0; i < callers->len; i++) {
    const IdeStripGraphEntry *e = g_ptr_array_index(callers, i);
    if (strcmp(e->name, "fx_glass_render") == 0) {
      saw_glass = TRUE;
      g_assert_cmpuint(e->call_count, ==, 2);
    } else if (strcmp(e->name, "comp_output_frame") == 0) {
      saw_comp = TRUE;
      g_assert_cmpuint(e->call_count, ==, 1);
    }
  }
  g_assert_true(saw_glass);
  g_assert_true(saw_comp);

  /* Incremental invalidation: add a new function in main.c and verify it resolves immediately */
  const char *main_v2 =
      "#include \"types.h\"\n"
      "void newly_added_helper(void) {}\n"
      "int comp_output_frame(FrameConfig *cfg) {\n"
      "  newly_added_helper();\n"
      "  return 0;\n"
      "}\n";
  write_file_helper(ws, "src/main.c", main_v2);
  ide_strip_graph_invalidate_file(g, main_path);

  g_autoptr(GPtrArray) after_calls = ide_strip_graph_extract_targets(
      g, main_path, main_v2, 3, 6, IDE_STRIP_GRAPH_LENS_CALLS, NULL);
  g_assert_cmpuint(after_calls->len, ==, 1);
  g_assert_cmpstr(((IdeStripGraphEntry *)g_ptr_array_index(after_calls, 0))->name,
                  ==, "newly_added_helper");

  ide_strip_graph_free(g);
  rm_rf_simple(ws);
}

/* 4. DIFF lens across open IdeBuffers (dirty & agent-modified, workspace-only) */
static void
test_diff_lens_open_buffers(void)
{
  g_autofree char *ws = g_dir_make_tmp("kajo-strip-diff-XXXXXX", NULL);
  g_autofree char *ext_dir = g_dir_make_tmp("kajo-strip-ext-XXXXXX", NULL);

  const char *orig =
      "void alpha(void)\n"
      "{\n"
      "  int a = 1;\n"
      "}\n"
      "void beta(void)\n"
      "{\n"
      "  int b = 2;\n"
      "}\n";
  write_file_helper(ws, "src/mod.c", orig);
  write_file_helper(ext_dir, "outside.c", orig);

  g_autofree char *ws_file = g_build_filename(ws, "src/mod.c", NULL);
  g_autofree char *ext_file = g_build_filename(ext_dir, "outside.c", NULL);

  IdeBuffer *b_ws = ide_buffer_new(ws_file);
  g_assert_true(ide_buffer_load_from_disk(b_ws, NULL));
  ide_buffer_record_agent_snapshot(b_ws);
  ide_buffer_set_text(b_ws,
                      "void alpha(void)\n"
                      "{\n"
                      "  int a = 1;\n"
                      "}\n"
                      "void beta(void)\n"
                      "{\n"
                      "  int b = 42;\n"
                      "}\n");
  ide_buffer_recompute_diffs(b_ws);

  IdeBuffer *b_ext = ide_buffer_new(ext_file);
  g_assert_true(ide_buffer_load_from_disk(b_ext, NULL));
  ide_buffer_set_text(b_ext, "void alpha(void) { int a = 999; }\n");
  ide_buffer_recompute_diffs(b_ext);

  g_autoptr(GPtrArray) bufs = g_ptr_array_new();
  g_ptr_array_add(bufs, b_ws);
  g_ptr_array_add(bufs, b_ext);

  IdeStripGraph *g = ide_strip_graph_new(ws);
  g_autoptr(GPtrArray) diffs = ide_strip_graph_extract_diff_targets(g, bufs);
  g_assert_cmpuint(diffs->len, ==, 1);

  const IdeStripGraphEntry *d0 = g_ptr_array_index(diffs, 0);
  g_assert_cmpstr(d0->name, ==, "beta");
  g_assert_true(d0->is_agent_modified);
  g_assert_true(d0->is_dirty);
  g_assert_false(d0->is_fallback);

  ide_strip_graph_free(g);
  ide_buffer_free(b_ws);
  ide_buffer_free(b_ext);
  rm_rf_simple(ws);
  rm_rf_simple(ext_dir);
}

/* 5. 1,000-call stress test (exact order, WS-only external suppression, fast perf) */
static void
test_thousand_call_stress(void)
{
  g_autofree char *ws = g_dir_make_tmp("kajo-strip-1000-XXXXXX", NULL);
  GString *src = g_string_sized_new(65536);

  for (guint i = 0; i < 1000; i++) {
    g_string_append_printf(src, "static void fn_%04u(void) {}\n", i);
  }
  g_string_append(src, "void mega_dispatcher(void) {\n");
  for (guint i = 0; i < 1000; i++) {
    if (i % 10 == 0) {
      g_string_append_printf(src, "  external_noise_%u();\n", i);
    }
    g_string_append_printf(src, "  fn_%04u();\n", i);
  }
  g_string_append(src, "}\n");

  write_file_helper(ws, "src/mega.c", src->str);
  g_autofree char *mega_path = g_build_filename(ws, "src/mega.c", NULL);

  gint64 t0 = g_get_monotonic_time();
  IdeStripGraph *g = ide_strip_graph_new(ws);
  ide_strip_graph_ensure_indexed(g);

  guint ext_sup = 0;
  g_autoptr(GPtrArray) targets = ide_strip_graph_extract_targets(
      g, mega_path, src->str, 1001, 2102, IDE_STRIP_GRAPH_LENS_CALLS, &ext_sup);
  gint64 elapsed_ms = (g_get_monotonic_time() - t0) / 1000;

  g_assert_cmpuint(targets->len, ==, 1000);
  g_assert_cmpuint(ext_sup, ==, 100);
  for (guint i = 0; i < 1000; i++) {
    const IdeStripGraphEntry *e = g_ptr_array_index(targets, i);
    g_autofree char *expected = g_strdup_printf("fn_%04u", i);
    g_assert_cmpstr(e->name, ==, expected);
    if (i > 0) {
      const IdeStripGraphEntry *prev = g_ptr_array_index(targets, i - 1);
      g_assert_cmpuint(prev->call_line, <, e->call_line);
    }
  }
  /* Must finish comfortably well under 2 seconds even on slow CI */
  g_assert_cmpint(elapsed_ms, <, 2000);
  g_test_message("1,000-call index + extract completed in %" G_GINT64_FORMAT " ms", elapsed_ms);

  ide_strip_graph_free(g);
  g_string_free(src, TRUE);
  rm_rf_simple(ws);
}

/* 6. Async LSP query against mock_lsp_server.py (IMPACT + CALLS + safe cancellation) */
typedef struct {
  gboolean   done;
  guint      count;
  guint      ext_suppressed;
  gboolean   is_fallback;
  char      *first_name;
  guint      first_call_line;
} AsyncProbe;

static void
on_strip_async_done(GPtrArray *entries, guint ext_suppressed, gboolean is_fallback, gpointer ud)
{
  AsyncProbe *p = ud;
  p->done = TRUE;
  p->count = entries ? entries->len : 0;
  p->ext_suppressed = ext_suppressed;
  p->is_fallback = is_fallback;
  g_clear_pointer(&p->first_name, g_free);
  p->first_call_line = 0;
  if (entries != NULL && entries->len > 0) {
    const IdeStripGraphEntry *e0 = g_ptr_array_index(entries, 0);
    p->first_name = g_strdup(e0->name);
    p->first_call_line = e0->call_line;
  }
}

static gboolean
timeout_cb(gpointer d)
{
  *(gboolean *)d = TRUE;
  return G_SOURCE_REMOVE;
}

#define SPIN_UNTIL(cond) \
  do { \
    gboolean to_ = FALSE; \
    guint id_ = g_timeout_add_seconds(10, timeout_cb, &to_); \
    while (!(cond) && !to_) g_main_context_iteration(NULL, TRUE); \
    if (!to_) g_source_remove(id_); \
    g_assert_true(cond); \
  } while (0)

static void dummy_jump(const char *p, guint l, guint c, gpointer ud) { (void)p; (void)l; (void)c; (void)ud; }
static void dummy_status(const char *n, gboolean a, gpointer ud) { (void)n; (void)a; (void)ud; }

static void
test_lsp_async_queries_and_cancellation(void)
{
  g_autofree char *script = g_build_filename(KAJO_SOURCE_DIR, "tests", "mock_lsp_server.py", NULL);
  g_autofree char *cmd = g_strdup_printf("python3 %s", script);
  g_setenv("KAJO_LSP_MOCK_CMD", cmd, TRUE);

  g_autofree char *ws = g_dir_make_tmp("kajo-strip-lsp-XXXXXX", NULL);
  g_autofree char *path = g_build_filename(ws, "demo.kajo-mock", NULL);
  const char *src = "alpha := foo(1)\nfoo(2)\ncallee_fn(3)\n";
  g_assert_true(g_file_set_contents(path, src, -1, NULL));

  IdeLspManager *lsp = ide_lsp_manager_new(ws, dummy_jump, dummy_status, NULL);
  IdeBuffer *buf = ide_buffer_new(path);
  g_assert_true(ide_buffer_load_from_disk(buf, NULL));
  ide_lsp_manager_notify_open(lsp, buf);
  SPIN_UNTIL(ide_lsp_manager_is_ready(lsp, buf));

  IdeStripGraph *g = ide_strip_graph_new(ws);

  /* 1. IMPACT via LSP textDocument/references */
  AsyncProbe p_imp = { 0 };
  g_assert_true(ide_strip_graph_query_lsp_async(
      g, lsp, buf, 1, 10, IDE_STRIP_GRAPH_LENS_IMPACT, on_strip_async_done, &p_imp));
  SPIN_UNTIL(p_imp.done);
  g_assert_false(p_imp.is_fallback);
  g_assert_cmpuint(p_imp.count, >=, 1);
  g_clear_pointer(&p_imp.first_name, g_free);

  /* 2. CALLS via LSP prepareCallHierarchy -> callHierarchy/outgoingCalls */
  AsyncProbe p_calls = { 0 };
  g_assert_true(ide_strip_graph_query_lsp_async(
      g, lsp, buf, 1, 10, IDE_STRIP_GRAPH_LENS_CALLS, on_strip_async_done, &p_calls));
  SPIN_UNTIL(p_calls.done);
  g_assert_false(p_calls.is_fallback);
  g_assert_cmpuint(p_calls.count, ==, 1);
  g_assert_cmpstr(p_calls.first_name, ==, "callee_fn");
  g_assert_cmpuint(p_calls.first_call_line, ==, 2);
  g_clear_pointer(&p_calls.first_name, g_free);

  /* 3. TYPES / DIFF return FALSE for async LSP */
  g_assert_false(ide_strip_graph_query_lsp_async(
      g, lsp, buf, 1, 10, IDE_STRIP_GRAPH_LENS_TYPES, on_strip_async_done, &p_calls));

  /* 4. Freeing IdeStripGraph while a query is in flight must safely cancel without UAF */
  AsyncProbe p_cancel = { 0 };
  g_assert_true(ide_strip_graph_query_lsp_async(
      g, lsp, buf, 1, 10, IDE_STRIP_GRAPH_LENS_CALLS, on_strip_async_done, &p_cancel));
  ide_strip_graph_free(g);
  /* Pump main loop briefly so in-flight mock reply arrives into cancelled guard */
  for (int i = 0; i < 20; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(5000);
  }
  g_assert_false(p_cancel.done);

  ide_buffer_free(buf);
  ide_lsp_manager_free(lsp);
  rm_rf_simple(ws);
  g_unsetenv("KAJO_LSP_MOCK_CMD");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide/strip-graph/workspace-boundary", test_workspace_boundary_filter);
  g_test_add_func("/ide/strip-graph/read-peek", test_read_peek);
  g_test_add_func("/ide/strip-graph/source-order-calls-types-impact-invalidation",
                  test_source_order_calls_types_impact_and_invalidation);
  g_test_add_func("/ide/strip-graph/diff-open-buffers", test_diff_lens_open_buffers);
  g_test_add_func("/ide/strip-graph/thousand-call-stress", test_thousand_call_stress);
  g_test_add_func("/ide/strip-graph/lsp-async-and-cancellation",
                  test_lsp_async_queries_and_cancellation);
  return g_test_run();
}
