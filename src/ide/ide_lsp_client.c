#include "ide_lsp_client.h"
#include "ide_jsonrpc.h"

#include <string.h>
#include <unistd.h>

/* ─── Framing (shared with ide_lsp.c; defined here, declared in both headers) ─── */

typedef struct {
  guint             id;
  IdeLspResponseCb  cb;
  gpointer          user_data;
} PendingRequest;

typedef struct {
  char  *method;    /* NULL for a request, set for a notification before init */
  char  *body;      /* framed JSON ready to write */
} QueuedMessage;

struct _IdeLspClient {
  char               *server_id;
  GStrv               argv;
  char               *root_path;
  JsonNode           *init_options;

  IdeLspClientState   state;
  GSubprocess        *proc;
  GOutputStream      *stdin_stream;
  GInputStream       *stdout_stream;
  GCancellable       *cancellable;

  GByteArray         *rx;            /* accumulated stdout */
  guint8              read_buf[8192];

  guint               next_id;
  GHashTable         *pending;       /* id -> PendingRequest* */
  GQueue             *preinit_queue; /* QueuedMessage* flushed after initialized */
  JsonNode           *capabilities;  /* borrowed slice kept alive by init_result */
  JsonNode           *init_result;

  IdeLspNotificationCb  notification_cb;
  IdeLspServerRequestCb server_request_cb;
  IdeLspStateCb         state_cb;
  gpointer              cb_data;

  gboolean            exited;
  guint               kill_timeout_id;
  gboolean            writing;
  GQueue             *write_queue;   /* char* framed bodies awaiting the writer */
};

/* ─── small helpers ─── */

static void
pending_free(gpointer p)
{
  g_free(p);
}

static void
queued_message_free(gpointer data)
{
  QueuedMessage *q = data;
  if (q != NULL) {
    g_free(q->method);
    g_free(q->body);
    g_free(q);
  }
}

static char *
node_to_string(JsonNode *node)
{
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, node);
  return json_generator_to_data(gen, NULL);
}

static JsonNode *
member_or_null(JsonObject *obj, const char *name)
{
  return (obj != NULL && json_object_has_member(obj, name)) ? json_object_get_member(obj, name)
                                                            : NULL;
}

static void
fail_all_pending(IdeLspClient *client)
{
  /* Detach first: callbacks may issue new requests or free the client. */
  g_autoptr(GPtrArray) cbs = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, client->pending);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    PendingRequest *copy = g_memdup2(val, sizeof(PendingRequest));
    g_ptr_array_add(cbs, copy);
  }
  g_hash_table_remove_all(client->pending);
  for (guint i = 0; i < cbs->len; i++) {
    PendingRequest *pr = g_ptr_array_index(cbs, i);
    if (pr->cb != NULL) {
      pr->cb(client, NULL, "language server stopped", pr->user_data);
    }
  }
}

static void
set_state(IdeLspClient *client, IdeLspClientState state)
{
  if (client->state == state) {
    return;
  }
  client->state = state;
  if (state == IDE_LSP_CLIENT_STOPPED) {
    fail_all_pending(client);
  }
  if (client->state_cb != NULL) {
    client->state_cb(client, state, client->cb_data);
  }
}

/* ─── construction ─── */

IdeLspClient *
ide_lsp_client_new(const char *server_id, const char *const *argv, const char *root_path,
                   JsonNode *init_options)
{
  g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);
  IdeLspClient *client = g_new0(IdeLspClient, 1);
  client->server_id = g_strdup(server_id ? server_id : argv[0]);
  client->argv = g_strdupv((char **)argv);
  client->root_path = g_strdup(root_path ? root_path : ".");
  client->init_options = init_options;
  client->state = IDE_LSP_CLIENT_IDLE;
  client->cancellable = g_cancellable_new();
  client->rx = g_byte_array_new();
  client->next_id = 1;
  client->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, pending_free);
  client->preinit_queue = g_queue_new();
  client->write_queue = g_queue_new();
  return client;
}

void
ide_lsp_client_set_callbacks(IdeLspClient *client, IdeLspNotificationCb notification_cb,
                             IdeLspServerRequestCb server_request_cb, IdeLspStateCb state_cb,
                             gpointer user_data)
{
  g_return_if_fail(client != NULL);
  client->notification_cb = notification_cb;
  client->server_request_cb = server_request_cb;
  client->state_cb = state_cb;
  client->cb_data = user_data;
}

IdeLspClientState ide_lsp_client_get_state(const IdeLspClient *c) { return c ? c->state : IDE_LSP_CLIENT_STOPPED; }
const char *ide_lsp_client_get_server_id(const IdeLspClient *c) { return c ? c->server_id : NULL; }
const char *ide_lsp_client_get_root_path(const IdeLspClient *c) { return c ? c->root_path : NULL; }

/* ─── writing ─── */

static void on_write_done(GObject *src, GAsyncResult *res, gpointer user_data);

static void
pump_writer(IdeLspClient *client)
{
  if (client->writing || client->stdin_stream == NULL) {
    return;
  }
  char *body = g_queue_pop_head(client->write_queue);
  if (body == NULL) {
    return;
  }
  client->writing = TRUE;
  GBytes *bytes = g_bytes_new_take(body, strlen(body));
  g_output_stream_write_bytes_async(client->stdin_stream, bytes, G_PRIORITY_DEFAULT,
                                    client->cancellable, on_write_done, client);
  g_bytes_unref(bytes);
}

static void
on_write_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IdeLspClient *client = user_data;
  GError *error = NULL;
  g_output_stream_write_bytes_finish(G_OUTPUT_STREAM(src), res, &error);
  if (error != NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("ide-lsp[%s]: write failed: %s", client->server_id, error->message);
    }
    g_error_free(error);
    client->writing = FALSE;
    return;
  }
  client->writing = FALSE;
  pump_writer(client);
}

static void
send_framed(IdeLspClient *client, const char *body)
{
  g_queue_push_tail(client->write_queue, ide_jsonrpc_frame_message(body));
  pump_writer(client);
}

static char *
build_message(const char *method, JsonNode *params, const int *id)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  if (id != NULL) {
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, *id);
  }
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, method);
  if (params != NULL) {
    json_builder_set_member_name(b, "params");
    json_builder_add_value(b, json_node_copy(params));
  }
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  return node_to_string(root);
}

/* ─── reading ─── */

static void on_read_done(GObject *src, GAsyncResult *res, gpointer user_data);

static void
start_read(IdeLspClient *client)
{
  if (client->stdout_stream == NULL) {
    return;
  }
  g_input_stream_read_async(client->stdout_stream, client->read_buf, sizeof client->read_buf,
                            G_PRIORITY_DEFAULT, client->cancellable, on_read_done, client);
}

static void handle_message(IdeLspClient *client, JsonNode *root);

static void
drain_rx(IdeLspClient *client)
{
  for (;;) {
    gsize consumed = 0;
    char *body = ide_jsonrpc_extract_body((const char *)client->rx->data, client->rx->len, &consumed);
    if (body == NULL) {
      break;
    }
    g_byte_array_remove_range(client->rx, 0, consumed);

    g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_data(parser, body, -1, NULL)) {
      JsonNode *root = json_parser_get_root(parser);
      if (root != NULL) {
        handle_message(client, root);
      }
    }
    g_free(body);
  }
}

static void
on_read_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IdeLspClient *client = user_data;
  GError *error = NULL;
  gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &error);
  if (n < 0) {
    if (error != NULL && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("ide-lsp[%s]: read failed: %s", client->server_id, error->message);
    }
    g_clear_error(&error);
    return;
  }
  if (n == 0) { /* EOF: server closed stdout */
    set_state(client, IDE_LSP_CLIENT_STOPPED);
    return;
  }
  g_byte_array_append(client->rx, client->read_buf, (guint)n);
  drain_rx(client);
  start_read(client);
}

/* ─── dispatch ─── */

static void flush_preinit_queue(IdeLspClient *client);

static void
handle_response(IdeLspClient *client, JsonObject *obj, gint64 id)
{
  PendingRequest *pr = g_hash_table_lookup(client->pending, GUINT_TO_POINTER((guint)id));
  if (pr == NULL) {
    return; /* cancelled or unknown */
  }
  IdeLspResponseCb cb = pr->cb;
  gpointer ud = pr->user_data;
  g_hash_table_remove(client->pending, GUINT_TO_POINTER((guint)id));
  if (cb == NULL) {
    return;
  }

  JsonNode *err = member_or_null(obj, "error");
  if (err != NULL && JSON_NODE_HOLDS_OBJECT(err)) {
    JsonObject *eo = json_node_get_object(err);
    const char *msg = json_object_has_member(eo, "message")
                          ? json_object_get_string_member(eo, "message")
                          : "server error";
    cb(client, NULL, msg, ud);
  } else {
    cb(client, member_or_null(obj, "result"), NULL, ud);
  }
}

static void
handle_server_request(IdeLspClient *client, JsonObject *obj, gint64 id)
{
  const char *method = json_object_get_string_member(obj, "method");
  JsonNode *params = member_or_null(obj, "params");
  JsonNode *result = NULL;
  if (client->server_request_cb != NULL) {
    result = client->server_request_cb(client, method, params, client->cb_data);
  }

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, id);
  json_builder_set_member_name(b, "result");
  if (result != NULL) {
    json_builder_add_value(b, result); /* ownership transferred */
  } else {
    json_builder_add_null_value(b);
  }
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  g_autofree char *body = node_to_string(root);
  send_framed(client, body);
}

static void
handle_message(IdeLspClient *client, JsonNode *root)
{
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    return;
  }
  JsonObject *obj = json_node_get_object(root);
  gboolean has_id = json_object_has_member(obj, "id");
  gboolean has_method = json_object_has_member(obj, "method");

  if (has_method && has_id) {
    JsonNode *idn = json_object_get_member(obj, "id");
    gint64 id = JSON_NODE_HOLDS_VALUE(idn) ? json_node_get_int(idn) : 0;
    handle_server_request(client, obj, id);
  } else if (has_method) {
    if (client->notification_cb != NULL) {
      client->notification_cb(client, json_object_get_string_member(obj, "method"),
                              member_or_null(obj, "params"), client->cb_data);
    }
  } else if (has_id) {
    JsonNode *idn = json_object_get_member(obj, "id");
    if (JSON_NODE_HOLDS_VALUE(idn)) {
      handle_response(client, obj, json_node_get_int(idn));
    }
  }
}

/* ─── initialize handshake ─── */

static JsonNode *
build_client_capabilities(void)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "textDocument");
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "synchronization");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "didSave");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);

  json_builder_set_member_name(b, "hover");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "contentFormat");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "plaintext");
  json_builder_add_string_value(b, "markdown");
  json_builder_end_array(b);
  json_builder_end_object(b);

  json_builder_set_member_name(b, "completion");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "completionItem");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "snippetSupport");
  json_builder_add_boolean_value(b, FALSE);
  json_builder_end_object(b);
  json_builder_end_object(b);

  json_builder_set_member_name(b, "definition");
  json_builder_begin_object(b);
  json_builder_end_object(b);
  json_builder_set_member_name(b, "rename");
  json_builder_begin_object(b);
  json_builder_end_object(b);
  json_builder_set_member_name(b, "publishDiagnostics");
  json_builder_begin_object(b);
  json_builder_end_object(b);

  json_builder_end_object(b); /* textDocument */

  json_builder_set_member_name(b, "workspace");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "applyEdit");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_set_member_name(b, "workspaceFolders");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);

  json_builder_end_object(b);
  return json_builder_get_root(b);
}

static void
on_initialize_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                       gpointer user_data)
{
  (void)user_data;
  if (error_message != NULL || result == NULL || !JSON_NODE_HOLDS_OBJECT(result)) {
    g_debug("ide-lsp[%s]: initialize failed: %s", client->server_id,
            error_message ? error_message : "no result");
    ide_lsp_client_stop(client);
    return;
  }
  client->init_result = json_node_copy(result);
  JsonObject *ro = json_node_get_object(client->init_result);
  client->capabilities = member_or_null(ro, "capabilities");

  /* initialized notification */
  g_autoptr(JsonNode) empty = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(empty, json_object_new());
  g_autofree char *body = build_message("initialized", empty, NULL);
  send_framed(client, body);

  set_state(client, IDE_LSP_CLIENT_READY);
  flush_preinit_queue(client);
}

static void
send_initialize(IdeLspClient *client)
{
  int id = (int)client->next_id++;

  g_autofree char *root_uri = g_filename_to_uri(client->root_path, NULL, NULL);

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "processId");
  json_builder_add_int_value(b, getpid());
  json_builder_set_member_name(b, "rootUri");
  if (root_uri != NULL) {
    json_builder_add_string_value(b, root_uri);
  } else {
    json_builder_add_null_value(b);
  }
  json_builder_set_member_name(b, "rootPath");
  json_builder_add_string_value(b, client->root_path);

  if (root_uri != NULL) {
    json_builder_set_member_name(b, "workspaceFolders");
    json_builder_begin_array(b);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "uri");
    json_builder_add_string_value(b, root_uri);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "kajo");
    json_builder_end_object(b);
    json_builder_end_array(b);
  }

  json_builder_set_member_name(b, "capabilities");
  g_autoptr(JsonNode) caps = build_client_capabilities();
  json_builder_add_value(b, json_node_ref(caps));

  if (client->init_options != NULL) {
    json_builder_set_member_name(b, "initializationOptions");
    json_builder_add_value(b, json_node_ref(client->init_options));
  }
  json_builder_set_member_name(b, "clientInfo");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, "kajo-ide");
  json_builder_end_object(b);
  json_builder_end_object(b);

  g_autoptr(JsonNode) params = json_builder_get_root(b);

  PendingRequest *pr = g_new0(PendingRequest, 1);
  pr->id = (guint)id;
  pr->cb = on_initialize_response;
  g_hash_table_insert(client->pending, GUINT_TO_POINTER(pr->id), pr);

  g_autofree char *body = build_message("initialize", params, &id);
  send_framed(client, body);
}

/* ─── start / stop ─── */

static void
on_process_exited(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IdeLspClient *client = user_data;
  GError *error = NULL;
  g_subprocess_wait_finish(G_SUBPROCESS(src), res, &error);
  if (error != NULL) {
    g_error_free(error); /* cancelled during free */
    return;
  }
  client->exited = TRUE;
  set_state(client, IDE_LSP_CLIENT_STOPPED);
}

gboolean
ide_lsp_client_start(IdeLspClient *client, GError **error)
{
  g_return_val_if_fail(client != NULL, FALSE);
  if (client->state != IDE_LSP_CLIENT_IDLE) {
    return TRUE;
  }

  g_autoptr(GSubprocessLauncher) launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  if (client->root_path != NULL) {
    g_subprocess_launcher_set_cwd(launcher, client->root_path);
  }
  client->proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)client->argv, error);
  if (client->proc == NULL) {
    set_state(client, IDE_LSP_CLIENT_STOPPED);
    return FALSE;
  }

  client->stdin_stream = g_subprocess_get_stdin_pipe(client->proc);
  client->stdout_stream = g_subprocess_get_stdout_pipe(client->proc);

  set_state(client, IDE_LSP_CLIENT_STARTING);
  g_subprocess_wait_async(client->proc, client->cancellable, on_process_exited, client);
  start_read(client);
  send_initialize(client);
  return TRUE;
}

static gboolean
on_kill_grace_elapsed(gpointer data)
{
  IdeLspClient *client = data;
  client->kill_timeout_id = 0;
  if (client->proc != NULL) {
    g_subprocess_force_exit(client->proc);
  }
  return G_SOURCE_REMOVE;
}

void
ide_lsp_client_stop(IdeLspClient *client)
{
  g_return_if_fail(client != NULL);
  if (client->state == IDE_LSP_CLIENT_STOPPED) {
    return;
  }

  gboolean was_ready = (client->state == IDE_LSP_CLIENT_READY);
  if (was_ready && client->stdin_stream != NULL) {
    int id = (int)client->next_id++;
    g_autofree char *sbody = build_message("shutdown", NULL, &id);
    send_framed(client, sbody);
    g_autofree char *ebody = build_message("exit", NULL, NULL);
    send_framed(client, ebody);
  }
  set_state(client, IDE_LSP_CLIENT_STOPPED);

  if (client->proc != NULL && !was_ready) {
    g_subprocess_force_exit(client->proc); /* never initialized: nothing to flush */
  } else if (client->proc != NULL && client->kill_timeout_id == 0) {
    client->kill_timeout_id = g_timeout_add(1500, on_kill_grace_elapsed, client);
  }
}

gboolean
ide_lsp_client_wait_exit(IdeLspClient *client, guint timeout_ms)
{
  g_return_val_if_fail(client != NULL, TRUE);
  if (client->proc == NULL) {
    return TRUE;
  }
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (!client->exited && g_get_monotonic_time() < deadline) {
    if (!g_main_context_iteration(NULL, FALSE)) {
      g_usleep(5000);
    }
  }
  return client->exited;
}

void
ide_lsp_client_free(IdeLspClient *client)
{
  if (client == NULL) {
    return;
  }
  g_cancellable_cancel(client->cancellable);
  if (client->kill_timeout_id != 0) {
    g_source_remove(client->kill_timeout_id);
  }
  if (client->proc != NULL) {
    g_subprocess_force_exit(client->proc);
    g_object_unref(client->proc);
  }
  g_clear_object(&client->cancellable);
  g_clear_pointer(&client->rx, g_byte_array_unref);
  g_clear_pointer(&client->pending, g_hash_table_destroy);
  if (client->preinit_queue != NULL) {
    g_queue_free_full(client->preinit_queue, queued_message_free);
  }
  if (client->write_queue != NULL) {
    g_queue_free_full(client->write_queue, g_free);
  }
  g_clear_pointer(&client->init_result, json_node_unref);
  if (client->init_options != NULL) {
    json_node_unref(client->init_options);
  }
  g_strfreev(client->argv);
  g_free(client->server_id);
  g_free(client->root_path);
  g_free(client);
}

/* ─── requests / notifications ─── */

static void
flush_preinit_queue(IdeLspClient *client)
{
  QueuedMessage *q;
  while ((q = g_queue_pop_head(client->preinit_queue)) != NULL) {
    send_framed(client, q->body);
    queued_message_free(q);
  }
}

void
ide_lsp_client_notify(IdeLspClient *client, const char *method, JsonNode *params)
{
  g_return_if_fail(client != NULL && method != NULL);
  g_autoptr(JsonNode) owned = params;
  if (client->state == IDE_LSP_CLIENT_STOPPED) {
    return;
  }
  g_autofree char *body = build_message(method, owned, NULL);
  if (client->state == IDE_LSP_CLIENT_READY) {
    send_framed(client, body);
  } else {
    QueuedMessage *q = g_new0(QueuedMessage, 1);
    q->method = g_strdup(method);
    q->body = g_steal_pointer(&body);
    g_queue_push_tail(client->preinit_queue, q);
  }
}

guint
ide_lsp_client_request(IdeLspClient *client, const char *method, JsonNode *params,
                       IdeLspResponseCb cb, gpointer user_data)
{
  g_return_val_if_fail(client != NULL && method != NULL, 0);
  g_autoptr(JsonNode) owned = params;
  if (client->state == IDE_LSP_CLIENT_STOPPED) {
    if (cb != NULL) {
      cb(client, NULL, "server stopped", user_data);
    }
    return 0;
  }

  int id = (int)client->next_id++;
  PendingRequest *pr = g_new0(PendingRequest, 1);
  pr->id = (guint)id;
  pr->cb = cb;
  pr->user_data = user_data;
  g_hash_table_insert(client->pending, GUINT_TO_POINTER(pr->id), pr);

  g_autofree char *body = build_message(method, owned, &id);
  if (client->state == IDE_LSP_CLIENT_READY) {
    send_framed(client, body);
  } else {
    QueuedMessage *q = g_new0(QueuedMessage, 1);
    q->body = g_steal_pointer(&body);
    g_queue_push_tail(client->preinit_queue, q);
  }
  return (guint)id;
}

void
ide_lsp_client_cancel(IdeLspClient *client, guint request_id)
{
  g_return_if_fail(client != NULL);
  if (!g_hash_table_remove(client->pending, GUINT_TO_POINTER(request_id))) {
    return;
  }
  if (client->state != IDE_LSP_CLIENT_READY) {
    return;
  }
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, request_id);
  json_builder_end_object(b);
  g_autoptr(JsonNode) params = json_builder_get_root(b);
  g_autofree char *body = build_message("$/cancelRequest", params, NULL);
  send_framed(client, body);
}

/* ─── capability lookup ─── */

JsonNode *
ide_lsp_client_get_capability(const IdeLspClient *client, const char *path)
{
  if (client == NULL || client->capabilities == NULL || path == NULL ||
      !JSON_NODE_HOLDS_OBJECT(client->capabilities)) {
    return NULL;
  }
  g_auto(GStrv) parts = g_strsplit(path, ".", -1);
  JsonNode *node = client->capabilities;
  for (guint i = 0; parts[i] != NULL; i++) {
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
      return NULL;
    }
    JsonObject *obj = json_node_get_object(node);
    if (!json_object_has_member(obj, parts[i])) {
      return NULL;
    }
    node = json_object_get_member(obj, parts[i]);
  }
  return node;
}

gboolean
ide_lsp_client_has_capability(const IdeLspClient *client, const char *path)
{
  JsonNode *node = ide_lsp_client_get_capability(client, path);
  if (node == NULL || JSON_NODE_HOLDS_NULL(node)) {
    return FALSE;
  }
  if (JSON_NODE_HOLDS_VALUE(node) &&
      json_node_get_value_type(node) == G_TYPE_BOOLEAN) {
    return json_node_get_boolean(node);
  }
  return TRUE;
}

JsonNode *
ide_lsp_position_params(const char *uri, guint line0, guint character_utf16)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "textDocument");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "uri");
  json_builder_add_string_value(b, uri);
  json_builder_end_object(b);
  json_builder_set_member_name(b, "position");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "line");
  json_builder_add_int_value(b, line0);
  json_builder_set_member_name(b, "character");
  json_builder_add_int_value(b, character_utf16);
  json_builder_end_object(b);
  json_builder_end_object(b);
  return json_builder_get_root(b);
}
