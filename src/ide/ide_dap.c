#include "ide_dap_private.h"

#include <string.h>

void
ide_dap_frame_free(gpointer p)
{
  DapFrame *f = p;
  g_free(f->name);
  g_free(f->path);
  g_free(f);
}

void
ide_dap_var_node_free(gpointer p)
{
  DapVarNode *n = p;
  if (n == NULL) {
    return;
  }
  g_free(n->name);
  g_free(n->type);
  g_free(n->value);
  g_clear_pointer(&n->children, g_ptr_array_unref);
  g_free(n);
}

void
ide_dap_watch_item_free(gpointer p)
{
  DapWatchItem *w = p;
  if (w == NULL) {
    return;
  }
  g_free(w->expression);
  g_free(w->value);
  g_free(w->type);
  g_clear_pointer(&w->children, g_ptr_array_unref);
  g_free(w);
}

void
ide_dap_breakpoint_set_free(gpointer data)
{
  IdeDapBreakpointSet *b = data;
  if (b != NULL) {
    g_free(b->path);
    g_clear_pointer(&b->lines, g_array_unref);
    g_clear_pointer(&b->conditions, g_ptr_array_unref);
    g_free(b);
  }
}

static void
dap_set_status(IdeDapView *dap, const char *text)
{
  gtk_label_set_text(GTK_LABEL(dap->status_badge), text);
}

static void
dap_append_console(IdeDapView *dap, const char *line)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(dap->console_buf, &end);
  gtk_text_buffer_insert(dap->console_buf, &end, line, -1);
  gtk_text_buffer_insert(dap->console_buf, &end, "\n", 1);
}

static void
on_dap_btn_clicked(GtkButton *btn, gpointer user_data)
{
  IdeDapView *dap = user_data;
  const char *cmd = g_object_get_data(G_OBJECT(btn), "dap-cmd");
  if (dap && dap->action_cb && cmd) {
    dap->action_cb(cmd, dap->cb_data);
  }
}

static void
notify_state(IdeDapView *dap)
{
  if (dap->state_cb != NULL) {
    dap->state_cb(dap->state == DAP_STARTING || dap->state == DAP_RUNNING || dap->state == DAP_STOPPED,
                  dap->ctx_data);
  }
}

DapToken *
ide_dap_token_new(IdeDapView *dap)
{
  DapToken *t = g_new0(DapToken, 1);
  t->dap = dap;
  t->generation = dap->generation;
  return t;
}

gboolean
ide_dap_token_valid(DapToken *t)
{
  return t->generation == t->dap->generation && t->dap->client != NULL;
}

JsonNode *
ide_dap_args_object(const char *k1, gint64 v1, const char *k2, gint64 v2)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  if (k1 != NULL) {
    json_builder_set_member_name(b, k1);
    json_builder_add_int_value(b, v1);
  }
  if (k2 != NULL) {
    json_builder_set_member_name(b, k2);
    json_builder_add_int_value(b, v2);
  }
  json_builder_end_object(b);
  return json_builder_get_root(b);
}

/* ═══════════════════════════ events ═══════════════════════════ */

static void
set_state(IdeDapView *dap, DapState st)
{
  dap->state = st;
  notify_state(dap);
}

static void
end_session(IdeDapView *dap, const char *why)
{
  if (dap->state == DAP_ENDED || dap->state == DAP_IDLE) {
    return;
  }
  ide_dap_ui_clear_stopped(dap);
  dap_set_status(dap, "TERMINATED");
  dap_append_console(dap, why);
  set_state(dap, DAP_ENDED);
  dap->generation++;
  if (dap->client != NULL) {
    ide_dap_client_stop(dap->client);
  }
}

static void
on_dap_event(IdeDapClient *c, const char *event, JsonObject *body, gpointer user_data)
{
  IdeDapView *dap = user_data;
  if (c != dap->client) {
    return;
  }
  if (g_strcmp0(event, "stopped") == 0) {
    dap->thread_id = body ? json_object_get_int_member_with_default(body, "threadId", 1) : 1;
    const char *reason = body ? json_object_get_string_member_with_default(body, "reason", "pause") : "pause";
    g_autofree char *st = g_strdup_printf("PAUSED (%s)", reason);
    dap_set_status(dap, st);
    set_state(dap, DAP_STOPPED);
    ide_dap_ui_request_stack(dap);
  } else if (g_strcmp0(event, "continued") == 0) {
    dap_set_status(dap, "RUNNING");
    set_state(dap, DAP_RUNNING);
    ide_dap_ui_clear_stopped(dap);
  } else if (g_strcmp0(event, "output") == 0 && body != NULL) {
    if (!dap->seen_initialized) {
      return; /* gdb prints its startup banner before the handshake */
    }
    const char *out = json_object_get_string_member_with_default(body, "output", "");
    g_autofree char *trimmed = g_strchomp(g_strdup(out));
    if (trimmed[0] != '\0') {
      dap_append_console(dap, trimmed);
    }
  } else if (g_strcmp0(event, "exited") == 0) {
    gint64 code = body ? json_object_get_int_member_with_default(body, "exitCode", 0) : 0;
    g_autofree char *m = g_strdup_printf("[dap] Program exited with code %" G_GINT64_FORMAT, code);
    dap_append_console(dap, m);
  } else if (g_strcmp0(event, "terminated") == 0) {
    end_session(dap, "[dap] Debug session terminated.");
  }
}

static void
on_dap_adapter_exit(IdeDapClient *c, gpointer user_data)
{
  IdeDapView *dap = user_data;
  if (c == dap->client) {
    end_session(dap, "[dap] Debug adapter exited.");
  }
}

/* ═══════════════════════════ breakpoints ═══════════════════════════ */

static void
on_set_breakpoints(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  (void)body;
  DapToken *t = user_data;
  if (ide_dap_token_valid(t) && !ok) {
    g_autofree char *m = g_strdup_printf("[dap] setBreakpoints failed: %s", msg ? msg : "?");
    dap_append_console(t->dap, m);
  }
  g_free(t);
}

static gboolean
send_breakpoints(IdeDapView *dap)
{
  if (dap->breakpoints_fn == NULL || dap->client == NULL) {
    return FALSE;
  }
  g_autoptr(GPtrArray) sets = dap->breakpoints_fn(dap->ctx_data);
  gboolean any = FALSE;
  for (guint i = 0; sets != NULL && i < sets->len; i++) {
    const IdeDapBreakpointSet *bs = g_ptr_array_index(sets, i);
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "source");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "path");
    json_builder_add_string_value(b, bs->path);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "breakpoints");
    json_builder_begin_array(b);
    for (guint k = 0; bs->lines != NULL && k < bs->lines->len; k++) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "line");
      json_builder_add_int_value(b, g_array_index(bs->lines, guint, k));
      if (bs->conditions != NULL && k < bs->conditions->len) {
        const char *cond = g_ptr_array_index(bs->conditions, k);
        if (cond != NULL && cond[0] != '\0') {
          json_builder_set_member_name(b, "condition");
          json_builder_add_string_value(b, cond);
        }
      }
      json_builder_end_object(b);
      any = TRUE;
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    ide_dap_client_request(dap->client, "setBreakpoints", json_builder_get_root(b), on_set_breakpoints,
                           ide_dap_token_new(dap));
  }
  return any;
}

void
ide_dap_view_breakpoints_changed(IdeDapView *dap)
{
  g_return_if_fail(dap != NULL);
  if (dap->state == DAP_RUNNING || dap->state == DAP_STOPPED) {
    send_breakpoints(dap);
  }
}

/* ═══════════════════════════ session start ═══════════════════════════ */

static void
on_config_done(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  (void)body;
  DapToken *t = user_data;
  if (ide_dap_token_valid(t)) {
    if (!ok) {
      g_autofree char *m = g_strdup_printf("[dap] configurationDone failed: %s", msg ? msg : "?");
      dap_append_console(t->dap, m);
    } else if (t->dap->state == DAP_STARTING) {
      dap_set_status(t->dap, "RUNNING");
      set_state(t->dap, DAP_RUNNING);
    }
  }
  g_free(t);
}

static void
on_launch_response(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  (void)body;
  DapToken *t = user_data;
  if (ide_dap_token_valid(t) && !ok) {
    g_autofree char *m = g_strdup_printf("[dap] launch failed: %s", msg ? msg : "?");
    end_session(t->dap, m);
  }
  g_free(t);
}

/* The adapter signals readiness for configuration with `initialized`. */
static void
on_initialized_event(IdeDapView *dap)
{
  send_breakpoints(dap);
  {
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "filters");
    json_builder_begin_array(b);
    json_builder_end_array(b);
    json_builder_end_object(b);
    ide_dap_client_request(dap->client, "setExceptionBreakpoints", json_builder_get_root(b), NULL, NULL);
  }
  ide_dap_client_request(dap->client, "configurationDone", NULL, on_config_done, ide_dap_token_new(dap));
}

typedef struct {
  IdeDapView *dap;
  guint       generation;
  JsonNode   *launch_args;
  char       *request;
} StartCtx;

static void
on_initialize_response(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  (void)body;
  StartCtx *s = user_data;
  IdeDapView *dap = s->dap;
  if (s->generation == dap->generation && dap->client != NULL) {
    if (!ok) {
      g_autofree char *m = g_strdup_printf("[dap] initialize failed: %s", msg ? msg : "?");
      end_session(dap, m);
    } else {
      ide_dap_client_request(dap->client, s->request, json_node_ref(s->launch_args), on_launch_response,
                             ide_dap_token_new(dap));
    }
  }
  json_node_unref(s->launch_args);
  g_free(s->request);
  g_free(s);
}

/* Wraps the generic event handler so `initialized` can drive configuration. */
static void
on_dap_event_dispatch(IdeDapClient *c, const char *event, JsonObject *body, gpointer user_data)
{
  IdeDapView *dap = user_data;
  if (c != dap->client) {
    return;
  }
  if (g_strcmp0(event, "initialized") == 0) {
    dap->seen_initialized = TRUE;
    on_initialized_event(dap);
    return;
  }
  on_dap_event(c, event, body, user_data);
}

static void
teardown_client(IdeDapView *dap)
{
  dap->generation++;
  if (dap->java_cancel != NULL) {
    g_cancellable_cancel(dap->java_cancel);
    g_clear_object(&dap->java_cancel);
  }
  if (dap->client != NULL) {
    IdeDapClient *old = dap->client;
    dap->client = NULL;
    ide_dap_client_stop(old);
    ide_dap_client_free(old);
  }
}

/* initialize -> (response) launch; the `initialized` event then drives the
 * breakpoint / configurationDone exchange (see on_dap_event_dispatch). */
static void
begin_handshake(IdeDapView *dap, const char *adapter_id, JsonNode *launch_arguments, const char *request)
{
  StartCtx *s = g_new0(StartCtx, 1);
  s->dap = dap;
  s->generation = dap->generation;
  s->launch_args = launch_arguments ? json_node_ref(launch_arguments) : json_node_new(JSON_NODE_OBJECT);
  s->request = g_strdup(request ? request : "launch");

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "clientID");
  json_builder_add_string_value(b, "kajo-ide");
  json_builder_set_member_name(b, "clientName");
  json_builder_add_string_value(b, "Kajo IDE");
  json_builder_set_member_name(b, "adapterID");
  json_builder_add_string_value(b, adapter_id);
  json_builder_set_member_name(b, "linesStartAt1");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_set_member_name(b, "columnsStartAt1");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_set_member_name(b, "pathFormat");
  json_builder_add_string_value(b, "path");
  json_builder_set_member_name(b, "supportsRunInTerminalRequest");
  json_builder_add_boolean_value(b, FALSE);
  json_builder_end_object(b);
  ide_dap_client_request(dap->client, "initialize", json_builder_get_root(b), on_initialize_response, s);
}

typedef struct {
  IdeDapView *dap;
  guint       generation;
  char       *file;
} JavaStart;

typedef struct {
  IdeDapView *dap;
  char       *file;
} JavaHost;

static void
java_host_free(gpointer p)
{
  JavaHost *h = p;
  if (h != NULL) {
    g_free(h->file);
    g_free(h);
  }
}

static gboolean
java_host_exec(const char *command, JsonNode *arguments, IdeJavaExecCb cb, gpointer cb_data, gpointer host)
{
  JavaHost *h = host;
  if (h->dap->java_exec == NULL) {
    json_node_unref(arguments);
    return FALSE;
  }
  return h->dap->java_exec(h->file, command, arguments, cb, cb_data, h->dap->java_exec_data);
}

static void
on_java_ready(GIOStream *conn, JsonNode *launch_args, const char *error, gpointer user_data)
{
  JavaStart *js = user_data;
  IdeDapView *dap = js->dap;
  gboolean current = js->generation == dap->generation && dap->state == DAP_STARTING;
  g_free(js->file);
  g_free(js);

  if (error != NULL || !current) {
    if (conn != NULL) {
      g_object_unref(conn);
    }
    if (launch_args != NULL) {
      json_node_unref(launch_args);
    }
    if (current) {
      g_autofree char *m = g_strdup_printf("[dap] Cannot start Java debugging: %s", error);
      end_session(dap, m);
      dap_set_status(dap, "NOT STARTED");
    }
    return;
  }

  dap->client = ide_dap_client_new_streams(g_io_stream_get_input_stream(conn), g_io_stream_get_output_stream(conn));
  g_object_unref(conn); /* the client's streams keep the connection alive */
  ide_dap_client_set_callbacks(dap->client, on_dap_event_dispatch, on_dap_adapter_exit, dap);
  GError *err = NULL;
  if (!ide_dap_client_start(dap->client, &err)) {
    g_autofree char *m = g_strdup_printf("[dap] Cannot start Java debugging: %s", err ? err->message : "?");
    g_clear_error(&err);
    ide_dap_client_free(dap->client);
    dap->client = NULL;
    json_node_unref(launch_args);
    end_session(dap, m);
    return;
  }
  dap->seen_initialized = TRUE; /* no banner on a socket adapter */
  begin_handshake(dap, "java", launch_args, "launch");
  json_node_unref(launch_args);
}

static void
start_java_session(IdeDapView *dap, const char *active_file)
{
  dap->generation++;
  dap->seen_initialized = FALSE;
  set_state(dap, DAP_STARTING);
  dap_set_status(dap, "STARTING (jdtls)");

  JavaStart *js = g_new0(JavaStart, 1);
  js->dap = dap;
  js->generation = dap->generation;
  js->file = g_strdup(active_file);
  JavaHost *host = g_new0(JavaHost, 1); /* lives as long as the process: tiny, freed with the view */
  host->dap = dap;
  host->file = g_strdup(active_file);
  java_host_free(dap->java_host_cache);
  dap->java_host_cache = host;
  if (dap->java_cancel != NULL) {
    g_cancellable_cancel(dap->java_cancel);
    g_clear_object(&dap->java_cancel);
  }
  dap->java_cancel = g_cancellable_new();
  ide_java_debug_prepare_cancellable(active_file, dap->workspace_root, dap->java_cancel,
                                     java_host_exec, host, on_java_ready, js);
}

void
ide_dap_view_set_java_exec(IdeDapView *dap, IdeDapJavaExecFn fn, gpointer user_data)
{
  g_return_if_fail(dap != NULL);
  dap->java_exec = fn;
  dap->java_exec_data = user_data;
}

void
ide_dap_view_set_context(IdeDapView *dap, const char *workspace_root, IdeDapBreakpointsFn breakpoints_fn,
                         IdeDapFrameCb frame_cb, IdeDapStateCb state_cb, gpointer user_data)
{
  g_return_if_fail(dap != NULL);
  char *dup = g_strdup(workspace_root);
  g_free(dap->workspace_root);
  dap->workspace_root = dup;
  dap->breakpoints_fn = breakpoints_fn;
  dap->frame_cb = frame_cb;
  dap->state_cb = state_cb;
  dap->ctx_data = user_data;
}

void
ide_dap_view_start_session(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  if (dap->state == DAP_STARTING || dap->state == DAP_RUNNING || dap->state == DAP_STOPPED) {
    return;
  }
  teardown_client(dap);

  const char *active = active_buf ? ide_buffer_get_file_path(active_buf) : NULL;
  gboolean has_bp = FALSE;
  if (dap->breakpoints_fn != NULL) {
    g_autoptr(GPtrArray) sets = dap->breakpoints_fn(dap->ctx_data);
    for (guint i = 0; sets != NULL && i < sets->len; i++) {
      const IdeDapBreakpointSet *bs = g_ptr_array_index(sets, i);
      has_bp |= (bs->lines != NULL && bs->lines->len > 0);
    }
  }

  IdeDapLaunch *launch = ide_dap_launch_resolve(dap->workspace_root, active, has_bp);
  gtk_label_set_text(GTK_LABEL(dap->adapter_badge),
                     launch->adapter_label ? launch->adapter_label : ide_dap_detect_adapter_for_file(active));
  if (launch->error != NULL || launch->adapter_argv == NULL) {
    g_autofree char *m = g_strdup_printf("[dap] Cannot start: %s", launch->error ? launch->error : "no adapter");
    dap_append_console(dap, m);
    dap_set_status(dap, "NOT STARTED");
    ide_dap_launch_free(launch);
    return;
  }

  g_autofree char *startm = g_strdup_printf("[dap] %s → %s", launch->adapter_label, launch->program ? launch->program : "?");
  dap_append_console(dap, startm);

  if (launch->java) {
    start_java_session(dap, active);
    ide_dap_launch_free(launch);
    return;
  }

  dap->client = ide_dap_client_new_process((const char *const *)launch->adapter_argv, launch->cwd);
  ide_dap_client_set_callbacks(dap->client, on_dap_event_dispatch, on_dap_adapter_exit, dap);
  GError *error = NULL;
  if (!ide_dap_client_start(dap->client, &error)) {
    g_autofree char *m = g_strdup_printf("[dap] Cannot start adapter: %s", error ? error->message : "?");
    dap_append_console(dap, m);
    g_clear_error(&error);
    ide_dap_client_free(dap->client);
    dap->client = NULL;
    dap_set_status(dap, "NOT STARTED");
    ide_dap_launch_free(launch);
    return;
  }

  dap->generation++;
  dap->seen_initialized = FALSE;
  set_state(dap, DAP_STARTING);
  dap_set_status(dap, "STARTING");

  begin_handshake(dap, launch->adapter_label, launch->arguments, launch->request);
  ide_dap_launch_free(launch);
}

/* ═══════════════════════════ controls ═══════════════════════════ */

static void
on_control_response(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  (void)body;
  DapToken *t = user_data;
  if (ide_dap_token_valid(t) && !ok) {
    g_autofree char *m = g_strdup_printf("[dap] request failed: %s", msg ? msg : "?");
    dap_append_console(t->dap, m);
  }
  g_free(t);
}

static void
send_thread_command(IdeDapView *dap, const char *command, const char *label)
{
  if (dap->state != DAP_STOPPED || dap->client == NULL) {
    return;
  }
  dap_append_console(dap, label);
  ide_dap_client_request(dap->client, command, ide_dap_args_object("threadId", dap->thread_id, NULL, 0),
                         on_control_response, ide_dap_token_new(dap));
}

void
ide_dap_view_continue(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  if (dap->state == DAP_IDLE || dap->state == DAP_ENDED) {
    ide_dap_view_start_session(dap, active_buf);
    return;
  }
  send_thread_command(dap, "continue", "[dap] Continue (F5)");
}

void
ide_dap_view_step_over(IdeDapView *dap, IdeBuffer *active_buf)
{
  (void)active_buf;
  g_return_if_fail(dap != NULL);
  send_thread_command(dap, "next", "[dap] Step Over (F10)");
}

void
ide_dap_view_step_into(IdeDapView *dap, IdeBuffer *active_buf)
{
  (void)active_buf;
  g_return_if_fail(dap != NULL);
  send_thread_command(dap, "stepIn", "[dap] Step Into (F11)");
}

void
ide_dap_view_step_out(IdeDapView *dap, IdeBuffer *active_buf)
{
  (void)active_buf;
  g_return_if_fail(dap != NULL);
  send_thread_command(dap, "stepOut", "[dap] Step Out (F12)");
}

void
ide_dap_view_pause(IdeDapView *dap)
{
  g_return_if_fail(dap != NULL);
  if (dap->state == DAP_RUNNING && dap->client != NULL) {
    ide_dap_client_request(dap->client, "pause",
                           ide_dap_args_object("threadId", dap->thread_id ? dap->thread_id : 1, NULL, 0),
                           on_control_response, ide_dap_token_new(dap));
    dap_append_console(dap, "[dap] Pause requested.");
  }
}

void
ide_dap_view_stop(IdeDapView *dap, IdeBuffer *active_buf)
{
  (void)active_buf;
  g_return_if_fail(dap != NULL);
  if (dap->state == DAP_IDLE || dap->state == DAP_ENDED) {
    return;
  }
  if (dap->client != NULL && ide_dap_client_is_alive(dap->client)) {
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "terminateDebuggee");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);
    ide_dap_client_request(dap->client, "disconnect", json_builder_get_root(b), NULL, NULL);
  }
  end_session(dap, "[dap] Debug session stopped.");
}

gboolean
ide_dap_view_is_running(const IdeDapView *dap)
{
  g_return_val_if_fail(dap != NULL, FALSE);
  return dap->state == DAP_STARTING || dap->state == DAP_RUNNING || dap->state == DAP_STOPPED;
}

/* ═══════════════════════════ REPL ═══════════════════════════ */

static void
on_evaluate(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  DapToken *t = user_data;
  if (ide_dap_token_valid(t)) {
    if (ok && body != NULL) {
      dap_append_console(t->dap, json_object_get_string_member_with_default(body, "result", ""));
    } else {
      g_autofree char *m = g_strdup_printf("error: %s", msg ? msg : "evaluation failed");
      dap_append_console(t->dap, m);
    }
  }
  g_free(t);
}

static void
on_repl_activate(GtkEntry *entry, gpointer user_data)
{
  IdeDapView *dap = user_data;
  const char *expr = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (expr == NULL || expr[0] == '\0') {
    return;
  }
  g_autofree char *cmd_line = g_strdup_printf("> %s", expr);
  dap_append_console(dap, cmd_line);
  if (dap->state == DAP_STOPPED && dap->client != NULL) {
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "expression");
    json_builder_add_string_value(b, expr);
    json_builder_set_member_name(b, "context");
    json_builder_add_string_value(b, "repl");
    json_builder_set_member_name(b, "frameId");
    json_builder_add_int_value(b, dap->frame_id);
    json_builder_end_object(b);
    ide_dap_client_request(dap->client, "evaluate", json_builder_get_root(b), on_evaluate,
                           ide_dap_token_new(dap));
  } else {
    dap_append_console(dap, "(program must be paused to evaluate)");
  }
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

/* ═══════════════════════════ view lifecycle ═══════════════════════════ */

static GtkWidget *
build_header_bar(IdeDapView *dap)
{
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "ide-drawer-header");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  dap->adapter_badge = gtk_label_new("gdb -i=dap");
  gtk_widget_set_valign(dap->adapter_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(dap->adapter_badge, "shell-popover-badge");
  gtk_box_append(GTK_BOX(hdr), dap->adapter_badge);

  dap->status_badge = gtk_label_new("IDLE");
  gtk_widget_set_valign(dap->status_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(dap->status_badge, "shell-popover-badge");
  gtk_box_append(GTK_BOX(hdr), dap->status_badge);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(hdr), spacer);

  struct {
    const char *label;
    const char *cmd;
    const char *css;
  } btns[] = {
    { "▶ CONTINUE (F5)",  "continue",  "action-btn-primary" },
    { "⏸ PAUSE",          "pause",     "ide-toolbar-btn" },
    { "↷ STEP OVER (F10)", "step-over", "ide-toolbar-btn" },
    { "↓ STEP INTO (F11)", "step-into", "ide-toolbar-btn" },
    { "↑ STEP OUT (F12)",  "step-out",  "ide-toolbar-btn" },
    { "■ STOP (Shift+F5)", "stop",      "ide-btn-reject" }
  };

  for (size_t i = 0; i < G_N_ELEMENTS(btns); i++) {
    GtkWidget *b = gtk_button_new_with_label(btns[i].label);
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b, "ide-toolbar-btn");
    gtk_widget_add_css_class(b, btns[i].css);
    g_object_set_data(G_OBJECT(b), "dap-cmd", (gpointer)btns[i].cmd);
    g_signal_connect(b, "clicked", G_CALLBACK(on_dap_btn_clicked), dap);
    gtk_box_append(GTK_BOX(hdr), b);
  }
  return hdr;
}

static GtkWidget *
build_console_column(IdeDapView *dap)
{
  GtkWidget *c3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(c3, TRUE);
  GtkWidget *c3_lbl = gtk_label_new("GDB / LLDB CONSOLE");
  gtk_label_set_xalign(GTK_LABEL(c3_lbl), 0.0f);
  gtk_widget_add_css_class(c3_lbl, "ide-tree-header-title");
  gtk_box_append(GTK_BOX(c3), c3_lbl);

  GtkWidget *s3 = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(s3, TRUE);
  dap->console_buf = gtk_text_buffer_new(NULL);
  dap->console_view = gtk_text_view_new_with_buffer(dap->console_buf);
  gtk_widget_set_focusable(dap->console_view, FALSE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(dap->console_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(dap->console_view), TRUE);
  gtk_widget_add_css_class(dap->console_view, "ide-source-view");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s3), dap->console_view);
  gtk_box_append(GTK_BOX(c3), s3);

  dap->repl_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(dap->repl_entry), "Evaluate expression (p expr)...");
  gtk_widget_add_css_class(dap->repl_entry, "shell-launcher-input");
  g_signal_connect(dap->repl_entry, "activate", G_CALLBACK(on_repl_activate), dap);
  gtk_box_append(GTK_BOX(c3), dap->repl_entry);

  return c3;
}

IdeDapView *
ide_dap_view_new(IdeDapActionCb action_cb, gpointer user_data)
{
  IdeDapView *dap = g_new0(IdeDapView, 1);
  dap->frames = g_ptr_array_new_with_free_func(ide_dap_frame_free);
  dap->var_roots = g_ptr_array_new_with_free_func(ide_dap_var_node_free);
  dap->watches = g_ptr_array_new_with_free_func(ide_dap_watch_item_free);
  dap->action_cb = action_cb;
  dap->cb_data = user_data;

  dap->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(dap->root_box, -1, 185);
  gtk_box_append(GTK_BOX(dap->root_box), build_header_bar(dap));

  /* 3-Column Horizontal Split: Call Stack | Locals & Watches | Debug Console & REPL */
  GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_vexpand(cols, TRUE);
  gtk_widget_set_margin_start(cols, 8);
  gtk_widget_set_margin_end(cols, 8);
  gtk_widget_set_margin_top(cols, 6);
  gtk_widget_set_margin_bottom(cols, 6);

  ide_dap_ui_build_panels(dap, cols);
  gtk_box_append(GTK_BOX(cols), build_console_column(dap));
  gtk_box_append(GTK_BOX(dap->root_box), cols);

  return dap;
}

void
ide_dap_view_free(IdeDapView *dap)
{
  if (dap == NULL) {
    return;
  }
  teardown_client(dap);
  java_host_free(dap->java_host_cache);
  g_clear_pointer(&dap->frames, g_ptr_array_unref);
  g_clear_pointer(&dap->var_roots, g_ptr_array_unref);
  g_clear_pointer(&dap->watches, g_ptr_array_unref);
  g_clear_object(&dap->console_buf);
  g_free(dap->workspace_root);
  g_free(dap);
}

GtkWidget *
ide_dap_view_get_widget(IdeDapView *dap)
{
  g_return_val_if_fail(dap != NULL, NULL);
  return dap->root_box;
}

GtkWidget *
ide_dap_view_get_repl_entry(IdeDapView *dap)
{
  g_return_val_if_fail(dap != NULL, NULL);
  return dap->repl_entry;
}

GtkWidget *
ide_dap_view_get_watch_entry(IdeDapView *dap)
{
  g_return_val_if_fail(dap != NULL, NULL);
  return dap->watch_entry;
}

void
ide_dap_view_add_watch(IdeDapView *dap, const char *expression)
{
  g_return_if_fail(dap != NULL);
  if (expression == NULL) {
    return;
  }
  g_autofree char *expr = g_strstrip(g_strdup(expression));
  if (expr[0] == '\0') {
    return;
  }
  DapWatchItem *w = g_new0(DapWatchItem, 1);
  w->id = ++dap->next_watch_id;
  w->expression = g_steal_pointer(&expr);
  w->value = g_strdup(dap->state == DAP_STOPPED ? "(evaluating...)" : "(paused to evaluate)");
  g_ptr_array_add(dap->watches, w);
  ide_dap_ui_rebuild_watch_list(dap);
  if (dap->state == DAP_STOPPED && dap->client != NULL) {
    ide_dap_ui_evaluate_watch(dap, w);
  }
}

void
ide_dap_view_remove_watch(IdeDapView *dap, guint index)
{
  g_return_if_fail(dap != NULL);
  if (index >= dap->watches->len) {
    return;
  }
  g_ptr_array_remove_index(dap->watches, index);
  ide_dap_ui_rebuild_watch_list(dap);
}

void
ide_dap_view_clear_watches(IdeDapView *dap)
{
  g_return_if_fail(dap != NULL);
  g_ptr_array_set_size(dap->watches, 0);
  ide_dap_ui_rebuild_watch_list(dap);
}
