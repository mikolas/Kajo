#include "../src/ide/ide_lsp_client.h"
#include "../src/ide/ide_lsp_servers.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* Drives the real client against tests/mock_lsp_server.py. */

typedef struct {
  GMainLoop *loop;
  gboolean   got_hover;
  gboolean   got_error;
  gboolean   got_diag;
  gboolean   got_message;
  gboolean   stopped;
  gboolean   pending_failed;
  char      *hover_text;
  char      *error_text;
  int        diag_count;
} Ctx;

static void
on_hover(IdeLspClient *c, JsonNode *result, const char *err, gpointer ud)
{
  (void)c;
  Ctx *x = ud;
  g_assert_null(err);
  g_assert_nonnull(result);
  JsonObject *contents = json_object_get_object_member(json_node_get_object(result), "contents");
  x->hover_text = g_strdup(json_object_get_string_member(contents, "value"));
  x->got_hover = TRUE;
}

static void
on_unknown(IdeLspClient *c, JsonNode *result, const char *err, gpointer ud)
{
  (void)c;
  (void)result;
  Ctx *x = ud;
  x->error_text = g_strdup(err);
  x->got_error = TRUE;
}

static void
on_notification(IdeLspClient *c, const char *method, JsonNode *params, gpointer ud)
{
  (void)c;
  Ctx *x = ud;
  if (g_strcmp0(method, "textDocument/publishDiagnostics") == 0) {
    JsonArray *d = json_object_get_array_member(json_node_get_object(params), "diagnostics");
    x->diag_count = (int)json_array_get_length(d);
    x->got_diag = TRUE;
  } else if (g_strcmp0(method, "window/showMessage") == 0) {
    x->got_message = TRUE;
  }
}

static JsonNode *
on_server_request(IdeLspClient *c, const char *method, JsonNode *params, gpointer ud)
{
  (void)c;
  (void)params;
  (void)ud;
  if (g_strcmp0(method, "workspace/configuration") == 0) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_array(b);
    json_builder_add_null_value(b);
    json_builder_add_null_value(b);
    json_builder_end_array(b);
    JsonNode *n = json_builder_get_root(b);
    g_object_unref(b);
    return n;
  }
  return NULL;
}

static void
on_state(IdeLspClient *c, IdeLspClientState s, gpointer ud)
{
  (void)c;
  Ctx *x = ud;
  if (s == IDE_LSP_CLIENT_STOPPED) {
    x->stopped = TRUE;
  }
}

static gboolean
on_timeout(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

/* Spins the default main context until `cond` holds or 10 s elapsed. */
#define WAIT_UNTIL(cond) \
  do { \
    gboolean timed_out_ = FALSE; \
    guint tid_ = g_timeout_add_seconds(10, on_timeout, &timed_out_); \
    while (!(cond) && !timed_out_) { \
      g_main_context_iteration(NULL, TRUE); \
    } \
    if (!timed_out_) { \
      g_source_remove(tid_); \
    } \
    g_assert_true(cond); \
  } while (0)

static IdeLspClient *
make_client(Ctx *ctx)
{
  g_autofree char *script = g_build_filename(KAJO_SOURCE_DIR, "tests", "mock_lsp_server.py", NULL);
  const char *argv[] = { "python3", script, NULL };
  IdeLspClient *c = ide_lsp_client_new("mock", argv, g_get_tmp_dir(), NULL);
  ide_lsp_client_set_callbacks(c, on_notification, on_server_request, on_state, ctx);
  return c;
}

static JsonNode *
did_open_params(const char *uri, const char *text)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "textDocument");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "uri");
  json_builder_add_string_value(b, uri);
  json_builder_set_member_name(b, "languageId");
  json_builder_add_string_value(b, "go");
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "text");
  json_builder_add_string_value(b, text);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *n = json_builder_get_root(b);
  g_object_unref(b);
  return n;
}

static void
test_handshake_queueing_and_responses(void)
{
  Ctx ctx = { .loop = g_main_loop_new(NULL, FALSE) };
  IdeLspClient *c = make_client(&ctx);
  GError *err = NULL;
  g_assert_true(ide_lsp_client_start(c, &err));
  g_assert_no_error(err);
  g_assert_cmpint(ide_lsp_client_get_state(c), ==, IDE_LSP_CLIENT_STARTING);

  /* Issued before `initialize` completed: must be queued and flushed in order. */
  ide_lsp_client_notify(c, "textDocument/didOpen",
                        did_open_params("file:///tmp/a.go", "package a\nfunc Foo() {} // BUG here\n"));
  ide_lsp_client_request(c, "textDocument/hover",
                         ide_lsp_position_params("file:///tmp/a.go", 1, 6), on_hover, &ctx);

  WAIT_UNTIL(ctx.got_hover);
  g_assert_nonnull(strstr(ctx.hover_text, "func Foo(x int) string"));
  g_assert_cmpint(ide_lsp_client_get_state(c), ==, IDE_LSP_CLIENT_READY);

  /* capabilities */
  g_assert_true(ide_lsp_client_has_capability(c, "hoverProvider"));
  g_assert_true(ide_lsp_client_has_capability(c, "completionProvider.triggerCharacters"));
  g_assert_false(ide_lsp_client_has_capability(c, "referencesProvider"));

  /* diagnostics + server->client showMessage (server requests were auto-answered) */
  WAIT_UNTIL(ctx.got_diag && ctx.got_message);
  g_assert_cmpint(ctx.diag_count, ==, 1);

  /* error responses surface as error_message */
  ide_lsp_client_request(c, "textDocument/nonexistent", NULL, on_unknown, &ctx);
  ctx.got_error = FALSE;
  WAIT_UNTIL(ctx.got_error);
  g_assert_nonnull(strstr(ctx.error_text, "method not found"));

  ide_lsp_client_stop(c);
  g_assert_true(ide_lsp_client_wait_exit(c, 3000));
  g_assert_cmpint(ide_lsp_client_get_state(c), ==, IDE_LSP_CLIENT_STOPPED);

  /* requests after stop fail immediately instead of hanging */
  ctx.got_error = FALSE;
  g_free(ctx.error_text);
  ide_lsp_client_request(c, "textDocument/hover", NULL, on_unknown, &ctx);
  g_assert_true(ctx.got_error);

  ide_lsp_client_free(c);
  g_free(ctx.hover_text);
  g_free(ctx.error_text);
  g_main_loop_unref(ctx.loop);
}

static void
on_never(IdeLspClient *c, JsonNode *r, const char *e, gpointer ud)
{
  (void)c;
  (void)r;
  (void)e;
  (void)ud;
  g_assert_not_reached();
}

static void
on_fail_pending(IdeLspClient *c, JsonNode *r, const char *e, gpointer ud)
{
  (void)c;
  (void)r;
  Ctx *x = ud;
  x->pending_failed = (e != NULL);
}

static void
test_cancel_and_server_death(void)
{
  Ctx ctx = { .loop = g_main_loop_new(NULL, FALSE) };
  IdeLspClient *c = make_client(&ctx);
  g_assert_true(ide_lsp_client_start(c, NULL));

  /* a cancelled request never calls back */
  guint id = ide_lsp_client_request(c, "textDocument/hover",
                                    ide_lsp_position_params("file:///tmp/a.go", 0, 0), on_never, NULL);
  ide_lsp_client_cancel(c, id);

  /* a request pending when the server dies is failed, not leaked */
  ide_lsp_client_request(c, "textDocument/nonexistent", NULL, on_fail_pending, &ctx);
  ide_lsp_client_stop(c);
  g_assert_true(ctx.stopped || ide_lsp_client_get_state(c) == IDE_LSP_CLIENT_STOPPED);
  g_assert_true(ide_lsp_client_wait_exit(c, 3000));
  g_assert_true(ctx.pending_failed);

  ide_lsp_client_free(c);
  g_main_loop_unref(ctx.loop);
}

static void
test_missing_binary_fails_cleanly(void)
{
  Ctx ctx = { .loop = g_main_loop_new(NULL, FALSE) };
  const char *argv[] = { "/nonexistent/language-server", NULL };
  IdeLspClient *c = ide_lsp_client_new("nope", argv, g_get_tmp_dir(), NULL);
  GError *err = NULL;
  g_assert_false(ide_lsp_client_start(c, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);
  g_assert_cmpint(ide_lsp_client_get_state(c), ==, IDE_LSP_CLIENT_STOPPED);
  ide_lsp_client_free(c);
  g_main_loop_unref(ctx.loop);
}

/* A temp directory, optionally marked as an m68k project (.m68krc.json). */
static char *
m68k_tmp_project(gboolean marked)
{
  char *dir = g_dir_make_tmp("kajo-m68k-XXXXXX", NULL);
  g_assert_nonnull(dir);
  if (marked) {
    g_autofree char *m = g_build_filename(dir, ".m68krc.json", NULL);
    g_assert_true(g_file_set_contents(m, "{}", -1, NULL));
  }
  return dir;
}

static void
m68k_tmp_cleanup(const char *dir)
{
  g_autofree char *m = g_build_filename(dir, ".m68krc.json", NULL);
  g_unlink(m);
  g_rmdir(dir);
}

static void
test_m68k_is_opt_in_per_project(void)
{
  g_autofree char *plain = m68k_tmp_project(FALSE);
  g_autofree char *plain_s = g_build_filename(plain, "boot.S", NULL);
  g_unsetenv("KAJO_M68K_ASM");
  /* Without the marker, .S/.asm/.i are not claimed (x86/ARM/NASM/SWIG projects). */
  g_assert_null(ide_lsp_server_spec_for_file(plain_s));
  g_assert_cmpstr(ide_lsp_language_id_for_file(plain_s), ==, "plaintext");
  g_autofree char *plain_pat = ide_lsp_fallback_pattern(plain_s, "start", TRUE);
  g_assert_null(strstr(plain_pat, "equ"));
  g_assert_null(ide_lsp_fallback_globs(plain_s)[0]);

  /* The marker in an ancestor opts a whole tree in. */
  g_autofree char *proj = m68k_tmp_project(TRUE);
  g_autofree char *nested = g_build_filename(proj, "src", "deep", "x.i", NULL);
  g_assert_cmpstr(ide_lsp_language_id_for_file(nested), ==, "m68k");
  g_assert_nonnull(ide_lsp_server_spec_for_file(nested));

  /* Explicit override for unmarked projects. */
  g_setenv("KAJO_M68K_ASM", "1", TRUE);
  g_assert_cmpstr(ide_lsp_language_id_for_file(plain_s), ==, "m68k");
  g_unsetenv("KAJO_M68K_ASM");
  m68k_tmp_cleanup(plain);
  m68k_tmp_cleanup(proj);
}

static gboolean
rg_pattern_matches(const char *pattern, const char *line)
{
  g_autoptr(GRegex) rx = g_regex_new(pattern, G_REGEX_MULTILINE, 0, NULL);
  g_assert_nonnull(rx);
  return g_regex_match(rx, line, 0, NULL);
}

static void
test_fallback_patterns_are_language_scoped(void)
{
  /* m68k: labels, assignments and directives match... */
  g_autofree char *proj = m68k_tmp_project(TRUE);
  g_autofree char *asm_path = g_build_filename(proj, "main.asm", NULL);
  g_autofree char *m = ide_lsp_fallback_pattern(asm_path, "start", TRUE);
  g_assert_true(rg_pattern_matches(m, "start:\n"));
  g_assert_true(rg_pattern_matches(m, "  start::\n"));
  g_assert_true(rg_pattern_matches(m, "start equ 5\n"));
  g_assert_true(rg_pattern_matches(m, "start = 5\n"));
  /* ...but a bare use of the symbol does not */
  g_assert_false(rg_pattern_matches(m, "  jsr start\n"));
  const char *const *mg = ide_lsp_fallback_globs(asm_path);
  g_assert_nonnull(mg[0]);
  g_assert_cmpstr(mg[0], ==, "--glob=*.s");

  /* Other languages keep their strict patterns: `:` and `=` after a name are not declarations. */
  g_autofree char *d_h = ide_lsp_fallback_pattern("a/x.c", "count", FALSE);
  g_assert_true(rg_pattern_matches(d_h, "int count(void);\n"));
  g_assert_false(rg_pattern_matches(d_h, "count: 3\n"));
  g_assert_false(rg_pattern_matches(d_h, "x = count = 4\n"));
  g_autofree char *d_d = ide_lsp_fallback_pattern("a/x.go", "count", TRUE);
  g_assert_true(rg_pattern_matches(d_d, "func count(x int) {\n"));
  g_assert_false(rg_pattern_matches(d_d, "case count:\n"));
  g_assert_null(ide_lsp_fallback_globs("a/x.go")[0]);
  m68k_tmp_cleanup(proj);

  /* The symbol is escaped, never interpreted as regex syntax. */
  g_autofree char *esc = ide_lsp_fallback_pattern("a/x.c", "a.b", FALSE);
  g_assert_false(rg_pattern_matches(esc, "axb(\n"));
  g_assert_true(rg_pattern_matches(esc, "a.b(\n"));
}

static void
test_real_m68k_lsp_server(void)
{
  g_autofree char *proj = m68k_tmp_project(TRUE);
  g_autofree char *main_asm = g_build_filename(proj, "disasm", "main.asm", NULL);
  g_autofree char *set_s = g_build_filename(proj, "src", "SetSystem.s", NULL);
  g_autofree char *eq_i = g_build_filename(proj, "src", "equates.i", NULL);
  const IdeLspServerSpec *spec = ide_lsp_server_spec_for_file(main_asm);
  g_assert_nonnull(spec);
  g_assert_cmpstr(spec->server_id, ==, "m68k-lsp");
  g_assert_cmpstr(ide_lsp_language_id_for_file(main_asm), ==, "m68k");
  g_assert_cmpstr(ide_lsp_language_id_for_file(set_s), ==, "m68k");
  g_assert_cmpstr(ide_lsp_language_id_for_file(eq_i), ==, "m68k");

  g_autofree char *bin = g_find_program_in_path(spec->binary);
  if (bin == NULL) {
    m68k_tmp_cleanup(proj);
    g_test_skip("EXTERNAL TOOL MISSING: m68k-lsp-server not in PATH, real-server check not run");
    return;
  }

  Ctx ctx = { .loop = g_main_loop_new(NULL, FALSE) };
  g_auto(GStrv) argv = ide_lsp_build_argv(spec, g_get_tmp_dir());
  IdeLspClient *c = ide_lsp_client_new(spec->server_id, (const char *const *)argv, g_get_tmp_dir(), NULL);
  ide_lsp_client_set_callbacks(c, on_notification, on_server_request, on_state, &ctx);

  GError *err = NULL;
  g_assert_true(ide_lsp_client_start(c, &err));
  g_assert_no_error(err);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "textDocument");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "uri");
  json_builder_add_string_value(b, "file:///tmp/test_m68k.s");
  json_builder_set_member_name(b, "languageId");
  json_builder_add_string_value(b, "m68k");
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "text");
  json_builder_add_string_value(b, "main:\n  move.w #$1234, d0\n  rts\n");
  json_builder_end_object(b);
  json_builder_end_object(b);
  ide_lsp_client_notify(c, "textDocument/didOpen", json_builder_get_root(b));
  g_object_unref(b);

  ide_lsp_client_request(c, "textDocument/hover",
                         ide_lsp_position_params("file:///tmp/test_m68k.s", 1, 3), on_hover, &ctx);
  WAIT_UNTIL(ctx.got_hover);
  g_assert_nonnull(ctx.hover_text);
  g_autofree char *hover_lower = g_ascii_strdown(ctx.hover_text, -1);
  g_assert_nonnull(strstr(hover_lower, "move"));
  g_assert_cmpint(ide_lsp_client_get_state(c), ==, IDE_LSP_CLIENT_READY);

  ide_lsp_client_stop(c);
  g_assert_true(ide_lsp_client_wait_exit(c, 3000));
  ide_lsp_client_free(c);
  g_free(ctx.hover_text);
  g_main_loop_unref(ctx.loop);
  m68k_tmp_cleanup(proj);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide/lsp-client/handshake-queueing-responses", test_handshake_queueing_and_responses);
  g_test_add_func("/ide/lsp-client/cancel-and-server-death", test_cancel_and_server_death);
  g_test_add_func("/ide/lsp-client/missing-binary", test_missing_binary_fails_cleanly);
  g_test_add_func("/ide/lsp-client/m68k-opt-in", test_m68k_is_opt_in_per_project);
  g_test_add_func("/ide/lsp-client/fallback-patterns", test_fallback_patterns_are_language_scoped);
  g_test_add_func("/ide/lsp-client/real-m68k-lsp", test_real_m68k_lsp_server);
  return g_test_run();
}
