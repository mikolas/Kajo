#include "ide_dap_client.h"

#include <string.h>

typedef struct {
  guint             seq;
  IdeDapResponseCb  cb;
  gpointer          user_data;
} Pending;

struct _IdeDapClient {
  GStrv           argv;
  char           *cwd;
  GSubprocess    *proc;
  GInputStream   *in;
  GOutputStream  *out;
  GCancellable   *cancellable;
  GByteArray     *rx;
  guint8          read_buf[8192];
  guint           next_seq;
  GHashTable     *pending;
  GQueue         *write_queue;
  gboolean        writing;
  gboolean        started;
  gboolean        alive;
  gboolean        exited;

  IdeDapEventCb   event_cb;
  IdeDapExitCb    exit_cb;
  gpointer        cb_data;
};

static JsonObject *
obj_member(JsonObject *o, const char *name)
{
  return (o != NULL && json_object_has_member(o, name) &&
          JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, name)))
             ? json_object_get_object_member(o, name)
             : NULL;
}

static char *
node_to_string(JsonNode *node)
{
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, node);
  return json_generator_to_data(gen, NULL);
}

IdeDapClient *
ide_dap_client_new_process(const char *const *argv, const char *cwd)
{
  g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);
  IdeDapClient *c = g_new0(IdeDapClient, 1);
  c->argv = g_strdupv((char **)argv);
  c->cwd = g_strdup(cwd);
  c->cancellable = g_cancellable_new();
  c->rx = g_byte_array_new();
  c->next_seq = 1;
  c->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  c->write_queue = g_queue_new();
  return c;
}

IdeDapClient *
ide_dap_client_new_streams(GInputStream *in, GOutputStream *out)
{
  g_return_val_if_fail(in != NULL && out != NULL, NULL);
  IdeDapClient *c = g_new0(IdeDapClient, 1);
  c->in = g_object_ref(in);
  c->out = g_object_ref(out);
  c->cancellable = g_cancellable_new();
  c->rx = g_byte_array_new();
  c->next_seq = 1;
  c->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  c->write_queue = g_queue_new();
  return c;
}

void
ide_dap_client_set_callbacks(IdeDapClient *c, IdeDapEventCb event_cb, IdeDapExitCb exit_cb,
                             gpointer user_data)
{
  g_return_if_fail(c != NULL);
  c->event_cb = event_cb;
  c->exit_cb = exit_cb;
  c->cb_data = user_data;
}

gboolean
ide_dap_client_is_alive(const IdeDapClient *c)
{
  return c != NULL && c->alive;
}

/* ─── pending requests ─── */

static void
fail_all_pending(IdeDapClient *c, const char *why)
{
  g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, c->pending);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    g_ptr_array_add(all, g_memdup2(v, sizeof(Pending)));
  }
  g_hash_table_remove_all(c->pending);
  for (guint i = 0; i < all->len; i++) {
    Pending *p = g_ptr_array_index(all, i);
    if (p->cb != NULL) {
      p->cb(c, FALSE, NULL, why, p->user_data);
    }
  }
}

static void
mark_exited(IdeDapClient *c)
{
  if (c->exited) {
    return;
  }
  c->exited = TRUE;
  c->alive = FALSE;
  fail_all_pending(c, "debug adapter exited");
  if (c->exit_cb != NULL) {
    c->exit_cb(c, c->cb_data);
  }
}

/* ─── writing ─── */

static void on_write_done(GObject *src, GAsyncResult *res, gpointer user_data);

static void
pump_writer(IdeDapClient *c)
{
  if (c->writing || c->out == NULL) {
    return;
  }
  char *framed = g_queue_pop_head(c->write_queue);
  if (framed == NULL) {
    return;
  }
  c->writing = TRUE;
  GBytes *bytes = g_bytes_new_take(framed, strlen(framed));
  g_output_stream_write_bytes_async(c->out, bytes, G_PRIORITY_DEFAULT, c->cancellable,
                                    on_write_done, c);
  g_bytes_unref(bytes);
}

static void
on_write_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IdeDapClient *c = user_data;
  GError *error = NULL;
  g_output_stream_write_bytes_finish(G_OUTPUT_STREAM(src), res, &error);
  c->writing = FALSE;
  if (error != NULL) {
    g_error_free(error);
    return;
  }
  pump_writer(c);
}

static void
send_json(IdeDapClient *c, JsonNode *root)
{
  g_autofree char *body = node_to_string(root);
  g_queue_push_tail(c->write_queue, ide_jsonrpc_frame_message(body));
  pump_writer(c);
}

/* ─── reading ─── */

static void on_read_done(GObject *src, GAsyncResult *res, gpointer user_data);

static void
start_read(IdeDapClient *c)
{
  g_input_stream_read_async(c->in, c->read_buf, sizeof c->read_buf, G_PRIORITY_DEFAULT,
                            c->cancellable, on_read_done, c);
}

static void
send_response(IdeDapClient *c, JsonObject *req, gboolean success, const char *message)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "seq");
  json_builder_add_int_value(b, c->next_seq++);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "response");
  json_builder_set_member_name(b, "request_seq");
  json_builder_add_int_value(b, json_object_get_int_member_with_default(req, "seq", 0));
  json_builder_set_member_name(b, "success");
  json_builder_add_boolean_value(b, success);
  json_builder_set_member_name(b, "command");
  json_builder_add_string_value(b, json_object_get_string_member_with_default(req, "command", ""));
  if (message != NULL) {
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, message);
  }
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  send_json(c, root);
}

static void
handle_message(IdeDapClient *c, JsonNode *root)
{
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    return;
  }
  JsonObject *o = json_node_get_object(root);
  const char *type = json_object_get_string_member_with_default(o, "type", "");

  if (g_strcmp0(type, "response") == 0) {
    guint rseq = (guint)json_object_get_int_member_with_default(o, "request_seq", 0);
    Pending *p = g_hash_table_lookup(c->pending, GUINT_TO_POINTER(rseq));
    if (p == NULL) {
      return;
    }
    IdeDapResponseCb cb = p->cb;
    gpointer ud = p->user_data;
    g_hash_table_remove(c->pending, GUINT_TO_POINTER(rseq));
    if (cb != NULL) {
      gboolean ok = json_object_get_boolean_member_with_default(o, "success", FALSE);
      cb(c, ok, obj_member(o, "body"),
         ok ? NULL : json_object_get_string_member_with_default(o, "message", "request failed"), ud);
    }
  } else if (g_strcmp0(type, "event") == 0) {
    if (c->event_cb != NULL) {
      c->event_cb(c, json_object_get_string_member_with_default(o, "event", ""),
                  obj_member(o, "body"), c->cb_data);
    }
  } else if (g_strcmp0(type, "request") == 0) {
    /* Reverse requests (runInTerminal, startDebugging): not supported yet. */
    send_response(c, o, FALSE, "not supported by kajo-ide");
  }
}

static void
drain_rx(IdeDapClient *c)
{
  for (;;) {
    gsize consumed = 0;
    char *body = ide_jsonrpc_extract_body((const char *)c->rx->data, c->rx->len, &consumed);
    if (body == NULL) {
      break;
    }
    g_byte_array_remove_range(c->rx, 0, consumed);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_data(parser, body, -1, NULL)) {
      JsonNode *root = json_parser_get_root(parser);
      if (root != NULL) {
        handle_message(c, root);
      }
    }
    g_free(body);
    if (c->exited) {
      break;
    }
  }
}

static void
on_read_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IdeDapClient *c = user_data;
  GError *error = NULL;
  gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &error);
  if (n < 0) {
    gboolean cancelled = error != NULL && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    if (!cancelled) {
      mark_exited(c);
    }
    return;
  }
  if (n == 0) {
    mark_exited(c);
    return;
  }
  g_byte_array_append(c->rx, c->read_buf, (guint)n);
  drain_rx(c);
  if (!c->exited) {
    start_read(c);
  }
}

static void
on_process_exited(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IdeDapClient *c = user_data;
  GError *error = NULL;
  g_subprocess_wait_finish(G_SUBPROCESS(src), res, &error);
  if (error != NULL) {
    g_error_free(error);
    return;
  }
  mark_exited(c);
}

/* ─── lifecycle ─── */

gboolean
ide_dap_client_start(IdeDapClient *c, GError **error)
{
  g_return_val_if_fail(c != NULL, FALSE);
  if (c->started) {
    return TRUE;
  }
  if (c->proc == NULL && c->in == NULL) {
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    if (c->cwd != NULL) {
      g_subprocess_launcher_set_cwd(launcher, c->cwd);
    }
    c->proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)c->argv, error);
    if (c->proc == NULL) {
      return FALSE;
    }
    c->in = g_object_ref(g_subprocess_get_stdout_pipe(c->proc));
    c->out = g_object_ref(g_subprocess_get_stdin_pipe(c->proc));
    g_subprocess_wait_async(c->proc, c->cancellable, on_process_exited, c);
  }
  c->started = TRUE;
  c->alive = TRUE;
  start_read(c);
  return TRUE;
}

void
ide_dap_client_stop(IdeDapClient *c)
{
  g_return_if_fail(c != NULL);
  if (c->proc != NULL) {
    g_subprocess_force_exit(c->proc);
  }
  if (c->out != NULL && c->proc == NULL) {
    g_output_stream_close(c->out, NULL, NULL);
  }
  if (c->alive) {
    c->alive = FALSE;
  }
}

gboolean
ide_dap_client_wait_exit(IdeDapClient *c, guint timeout_ms)
{
  g_return_val_if_fail(c != NULL, TRUE);
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (!c->exited && g_get_monotonic_time() < deadline) {
    if (!g_main_context_iteration(NULL, FALSE)) {
      g_usleep(5000);
    }
  }
  return c->exited;
}

void
ide_dap_client_free(IdeDapClient *c)
{
  if (c == NULL) {
    return;
  }
  g_cancellable_cancel(c->cancellable);
  if (c->proc != NULL) {
    g_subprocess_force_exit(c->proc);
    g_object_unref(c->proc);
  }
  g_clear_object(&c->in);
  g_clear_object(&c->out);
  g_clear_object(&c->cancellable);
  g_clear_pointer(&c->rx, g_byte_array_unref);
  g_clear_pointer(&c->pending, g_hash_table_destroy);
  if (c->write_queue != NULL) {
    g_queue_free_full(c->write_queue, g_free);
  }
  g_strfreev(c->argv);
  g_free(c->cwd);
  g_free(c);
}

guint
ide_dap_client_request(IdeDapClient *c, const char *command, JsonNode *arguments,
                       IdeDapResponseCb cb, gpointer user_data)
{
  g_return_val_if_fail(c != NULL && command != NULL, 0);
  g_autoptr(JsonNode) args = arguments;
  if (c->exited || !c->started) {
    if (cb != NULL) {
      cb(c, FALSE, NULL, "debug adapter not running", user_data);
    }
    return 0;
  }

  guint seq = c->next_seq++;
  Pending *p = g_new0(Pending, 1);
  p->seq = seq;
  p->cb = cb;
  p->user_data = user_data;
  g_hash_table_insert(c->pending, GUINT_TO_POINTER(seq), p);

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "seq");
  json_builder_add_int_value(b, seq);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "request");
  json_builder_set_member_name(b, "command");
  json_builder_add_string_value(b, command);
  if (args != NULL) {
    json_builder_set_member_name(b, "arguments");
    json_builder_add_value(b, json_node_copy(args));
  }
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  send_json(c, root);
  return seq;
}
