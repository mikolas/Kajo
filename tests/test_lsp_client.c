#include "../src/ide/ide_lsp_client.h"

#include <glib.h>
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

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide/lsp-client/handshake-queueing-responses", test_handshake_queueing_and_responses);
  g_test_add_func("/ide/lsp-client/cancel-and-server-death", test_cancel_and_server_death);
  g_test_add_func("/ide/lsp-client/missing-binary", test_missing_binary_fails_cleanly);
  return g_test_run();
}
