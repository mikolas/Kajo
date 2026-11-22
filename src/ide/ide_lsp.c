#include "ide_lsp_private.h"

#include <stdio.h>
#include <string.h>

/* ═══════════════════════════ Request contexts ═══════════════════════════ */

static void
client_entry_free(gpointer data)
{
  ClientEntry *ce = data;
  if (ce != NULL) {
    ide_lsp_client_free(ce->client);
    g_free(ce);
  }
}

static void
request_ctx_free(RequestCtx *ctx)
{
  if (ctx->anchor != NULL) {
    g_object_remove_weak_pointer(G_OBJECT(ctx->anchor), (gpointer *)&ctx->anchor);
  }
  g_free(ctx->uri);
  g_free(ctx->prefix);
  g_free(ctx->new_name);
  g_free(ctx);
}

RequestCtx *
ide_lsp_ctx_new(IdeLspManager *mgr, CtxKind kind, const char *uri)
{
  RequestCtx *ctx = g_new0(RequestCtx, 1);
  ctx->mgr = mgr;
  ctx->kind = kind;
  ctx->uri = g_strdup(uri);
  g_hash_table_add(mgr->live_ctx, ctx);
  return ctx;
}

void
ide_lsp_ctx_done(RequestCtx *ctx)
{
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->inflight_ctx == ctx) {
    mgr->inflight_ctx = NULL;
    mgr->inflight_client = NULL;
    mgr->inflight_id = 0;
  }
  g_hash_table_remove(mgr->live_ctx, ctx);
  request_ctx_free(ctx);
}

void
ide_lsp_ctx_set_anchor(RequestCtx *ctx, GtkWidget *anchor)
{
  ctx->anchor = anchor;
  if (anchor != NULL) {
    g_object_add_weak_pointer(G_OBJECT(anchor), (gpointer *)&ctx->anchor);
  }
}

void
ide_lsp_cancel_inflight(IdeLspManager *mgr)
{
  if (mgr->inflight_ctx == NULL) {
    return;
  }
  RequestCtx *ctx = mgr->inflight_ctx;
  if (mgr->inflight_client != NULL && mgr->inflight_id != 0) {
    ide_lsp_client_cancel(mgr->inflight_client, mgr->inflight_id);
  }
  ide_lsp_ctx_done(ctx);
}

void
ide_lsp_send_inflight_request(IdeLspManager    *mgr,
                              DocState         *doc,
                              RequestCtx       *ctx,
                              const char       *method,
                              JsonNode         *params,
                              IdeLspResponseCb  cb)
{
  mgr->inflight_ctx = ctx;
  mgr->inflight_client = doc->client;
  mgr->inflight_id = 0;
  guint id = ide_lsp_client_request(doc->client, method, params, cb, ctx);
  if (mgr->inflight_ctx == ctx) {
    mgr->inflight_id = id;
  }
}

gboolean
ide_lsp_take_popover_response(RequestCtx  *ctx,
                              gboolean     require_anchor,
                              DocState   **doc_out,
                              GtkWidget  **anchor_out)
{
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ide_lsp_ctx_done(ctx);
    return FALSE;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  GtkWidget *anchor = ctx->anchor;
  gboolean had_anchor = ctx->had_anchor;
  gboolean current = ctx->token == mgr->popover_token;
  ide_lsp_ctx_done(ctx);

  if (!current || doc == NULL) {
    return FALSE;
  }
  if (require_anchor ? (anchor == NULL) : (had_anchor && anchor == NULL)) {
    return FALSE;
  }
  if (doc_out != NULL) {
    *doc_out = doc;
  }
  if (anchor_out != NULL) {
    *anchor_out = anchor;
  }
  return TRUE;
}

/* ═══════════════════════════ Position helpers ═══════════════════════════ */

char *
ide_lsp_buffer_line_text(GtkTextBuffer *tbuf, guint line0)
{
  GtkTextIter s, e;
  gtk_text_buffer_get_iter_at_line(tbuf, &s, (gint)line0);
  e = s;
  if (!gtk_text_iter_ends_line(&e)) {
    gtk_text_iter_forward_to_line_end(&e);
  }
  return gtk_text_buffer_get_text(tbuf, &s, &e, TRUE);
}

void
ide_lsp_buffer_cursor_position(IdeBuffer *buf, guint *line0, guint *utf16)
{
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(tbuf, &it, gtk_text_buffer_get_insert(tbuf));
  *line0 = (guint)gtk_text_iter_get_line(&it);
  g_autofree char *line = ide_lsp_buffer_line_text(tbuf, *line0);
  *utf16 = ide_utf8_byte_to_utf16_col(line, (gsize)gtk_text_iter_get_line_index(&it));
}

void
ide_lsp_buffer_iter_at_pos(GtkTextBuffer *tbuf, guint line0, guint utf16, GtkTextIter *out)
{
  gint last = gtk_text_buffer_get_line_count(tbuf) - 1;
  if ((gint)line0 > last) {
    gtk_text_buffer_get_end_iter(tbuf, out);
    return;
  }
  g_autofree char *line = ide_lsp_buffer_line_text(tbuf, line0);
  gsize byte = ide_utf16_col_to_utf8_byte(line, utf16);
  byte = MIN(byte, strlen(line));
  gtk_text_buffer_get_iter_at_line_index(tbuf, out, (gint)line0, (gint)byte);
}

/* 1-based character column of an LSP (line, utf16) position in a buffer line. */
guint
ide_lsp_buffer_char_col_1based(GtkTextBuffer *tbuf, guint line0, guint utf16)
{
  gint last = gtk_text_buffer_get_line_count(tbuf) - 1;
  if ((gint)line0 > last) {
    return 1;
  }
  g_autofree char *line = ide_lsp_buffer_line_text(tbuf, line0);
  gsize byte = MIN(ide_utf16_col_to_utf8_byte(line, utf16), strlen(line));
  return (guint)g_utf8_pointer_to_offset(line, line + byte) + 1;
}

/* Same conversion for a file on disk (definition targets that are not open). */
void
ide_lsp_file_to_line_col(const char *path, guint line0, guint utf16, guint *line1, guint *col1)
{
  *line1 = line0 + 1;
  *col1 = utf16 + 1;
  g_autofree char *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL)) {
    return;
  }
  g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
  if (line0 < g_strv_length(lines)) {
    gsize byte = MIN(ide_utf16_col_to_utf8_byte(lines[line0], utf16), strlen(lines[line0]));
    *col1 = (guint)g_utf8_pointer_to_offset(lines[line0], lines[line0] + byte) + 1;
  }
}

/* Reads only up to `line0` from disk; returns trimmed preview line (or NULL)
 * and resolves UTF-16 `utf16` into 1-based char column `*col1_out`. */
char *
ide_lsp_read_disk_line_and_col(const char *path, guint line0, guint utf16, guint *col1_out)
{
  if (col1_out != NULL) {
    *col1_out = utf16 + 1;
  }
  if (path == NULL) {
    return NULL;
  }
  FILE *fp = fopen(path, "r");
  if (fp == NULL) {
    return NULL;
  }
  GString *cur = g_string_new(NULL);
  guint cur_line = 0;
  gboolean found = FALSE;
  char chunk[512];
  while (fgets(chunk, sizeof(chunk), fp) != NULL) {
    gsize n = strlen(chunk);
    gboolean has_nl = (n > 0 && chunk[n - 1] == '\n');
    if (cur_line == line0) {
      g_string_append_len(cur, chunk, (gssize)n);
      if (has_nl) {
        found = TRUE;
        break;
      }
    } else if (has_nl) {
      cur_line++;
    }
  }
  if (!found && cur_line == line0 && cur->len > 0) {
    found = TRUE;
  }
  fclose(fp);
  if (!found) {
    g_string_free(cur, TRUE);
    return NULL;
  }
  while (cur->len > 0 && (cur->str[cur->len - 1] == '\n' || cur->str[cur->len - 1] == '\r')) {
    g_string_truncate(cur, cur->len - 1);
  }
  if (col1_out != NULL) {
    gsize byte = MIN(ide_utf16_col_to_utf8_byte(cur->str, utf16), cur->len);
    *col1_out = (guint)g_utf8_pointer_to_offset(cur->str, cur->str + byte) + 1;
  }
  g_strstrip(cur->str);
  return g_string_free(cur, FALSE);
}

void
ide_lsp_resolve_request_position(IdeBuffer *buf, guint line_1based, guint col_1based,
                                 guint *line0, guint *utf16)
{
  if (line_1based == 0 || col_1based == 0) {
    ide_lsp_buffer_cursor_position(buf, line0, utf16);
    return;
  }
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  gint max_line = MAX(0, gtk_text_buffer_get_line_count(tbuf) - 1);
  *line0 = (guint)MIN((gint)(line_1based - 1), max_line);
  g_autofree char *line = ide_lsp_buffer_line_text(tbuf, *line0);
  glong chars = g_utf8_strlen(line, -1);
  glong off = MIN((glong)(col_1based - 1), chars);
  const char *p = g_utf8_offset_to_pointer(line, off);
  gsize byte = MIN((gsize)(p - line), strlen(line));
  *utf16 = ide_utf8_byte_to_utf16_col(line, byte);
}

char *
ide_lsp_extract_word_at_cursor(IdeBuffer *buf, char **out_prefix)
{
  if (out_prefix != NULL) {
    *out_prefix = g_strdup("");
  }
  if (buf == NULL) {
    return g_strdup("");
  }
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark(tbuf, &cur, gtk_text_buffer_get_insert(tbuf));

  GtkTextIter start = cur;
  while (gtk_text_iter_backward_char(&start)) {
    gunichar ch = gtk_text_iter_get_char(&start);
    if (!g_unichar_isalnum(ch) && ch != '_') {
      gtk_text_iter_forward_char(&start);
      break;
    }
  }

  if (out_prefix != NULL) {
    g_free(*out_prefix);
    *out_prefix = gtk_text_buffer_get_text(tbuf, &start, &cur, FALSE);
  }

  GtkTextIter end = cur;
  while (!gtk_text_iter_is_end(&end)) {
    gunichar ch = gtk_text_iter_get_char(&end);
    if (!g_unichar_isalnum(ch) && ch != '_') {
      break;
    }
    gtk_text_iter_forward_char(&end);
  }

  return gtk_text_buffer_get_text(tbuf, &start, &end, FALSE);
}

/* ═══════════════════════════ Status & doc sync ═══════════════════════════ */

void
ide_lsp_report_message(IdeLspManager *mgr, const char *fmt, ...)
{
  if (mgr->message_cb == NULL || mgr->shutting_down) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  g_autofree char *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  mgr->message_cb(msg, mgr->ui_data);
}

static void
update_status(IdeLspManager *mgr)
{
  if (mgr->status_cb == NULL || mgr->shutting_down) {
    return;
  }
  DocState *doc = mgr->active_doc;
  if (doc == NULL || doc->spec == NULL) {
    mgr->status_cb("native", TRUE, mgr->cb_data);
    return;
  }
  if (g_hash_table_contains(mgr->missing, doc->spec->server_id)) {
    g_autofree char *lbl = g_strdup_printf("%s (not installed)", doc->spec->server_id);
    mgr->status_cb(lbl, FALSE, mgr->cb_data);
    return;
  }
  gboolean ready = doc->client != NULL &&
                   ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_READY;
  mgr->status_cb(doc->spec->server_id, ready, mgr->cb_data);
}

static void
doc_free(gpointer data)
{
  DocState *doc = data;
  if (doc->change_timer != 0) {
    g_source_remove(doc->change_timer);
  }
  g_clear_pointer(&doc->raw_diagnostics, json_node_unref);
  g_free(doc->path);
  g_free(doc->uri);
  g_free(doc->language_id);
  g_free(doc->root);
  g_free(doc);
}

JsonNode *
ide_lsp_text_document_params(const char *uri)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "textDocument");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "uri");
  json_builder_add_string_value(b, uri);
  json_builder_end_object(b);
  json_builder_end_object(b);
  return json_builder_get_root(b);
}

static void
doc_send_open(DocState *doc)
{
  if (doc->client == NULL || doc->sent_open) {
    return;
  }
  g_autofree char *text = ide_buffer_get_text(doc->buf);
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "textDocument");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "uri");
  json_builder_add_string_value(b, doc->uri);
  json_builder_set_member_name(b, "languageId");
  json_builder_add_string_value(b, doc->language_id);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, doc->version);
  json_builder_set_member_name(b, "text");
  json_builder_add_string_value(b, text ? text : "");
  json_builder_end_object(b);
  json_builder_end_object(b);
  ide_lsp_client_notify(doc->client, "textDocument/didOpen", json_builder_get_root(b));
  doc->sent_open = TRUE;
}

void
ide_lsp_doc_send_change_now(DocState *doc)
{
  if (doc->change_timer != 0) {
    g_source_remove(doc->change_timer);
    doc->change_timer = 0;
  }
  if (doc->client == NULL || !doc->sent_open) {
    return;
  }
  g_autofree char *text = ide_buffer_get_text(doc->buf);
  doc->version++;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "textDocument");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "uri");
  json_builder_add_string_value(b, doc->uri);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, doc->version);
  json_builder_end_object(b);
  json_builder_set_member_name(b, "contentChanges");
  json_builder_begin_array(b);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "text");
  json_builder_add_string_value(b, text ? text : "");
  json_builder_end_object(b);
  json_builder_end_array(b);
  json_builder_end_object(b);
  ide_lsp_client_notify(doc->client, "textDocument/didChange", json_builder_get_root(b));
}

static gboolean
on_change_timer(gpointer data)
{
  DocState *doc = data;
  doc->change_timer = 0;
  ide_lsp_doc_send_change_now(doc);
  return G_SOURCE_REMOVE;
}

DocState *
ide_lsp_require_doc(IdeLspManager *lsp,
                    IdeBuffer     *buf,
                    const char    *capability,
                    const char    *feature_name,
                    const char    *cap_label)
{
  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc == NULL || doc->client == NULL || !doc->sent_open ||
      ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_STOPPED) {
    ide_lsp_report_message(lsp, "%s needs a running language server", feature_name);
    return NULL;
  }
  if (capability != NULL &&
      ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_READY &&
      !ide_lsp_client_has_capability(doc->client, capability)) {
    ide_lsp_report_message(lsp, "Language server %s does not support %s",
                           ide_lsp_client_get_server_id(doc->client), cap_label);
    return NULL;
  }
  return doc;
}

/* ═══════════════════════════ Client lifecycle & diagnostics ═══════════════════════════ */

static gchar *
client_key(const char *server_id, const char *root)
{
  return g_strdup_printf("%s|%s", server_id, root);
}

static void
on_client_state(IdeLspClient *client, IdeLspClientState state, gpointer user_data)
{
  IdeLspManager *mgr = user_data;
  if (mgr->shutting_down) {
    return;
  }
  if (state == IDE_LSP_CLIENT_STOPPED) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, mgr->docs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      DocState *doc = v;
      if (doc->client == client) {
        doc->sent_open = FALSE;
      }
    }
    if (mgr->active_doc != NULL && mgr->active_doc->client == client) {
      ide_lsp_report_message(mgr, "Language server %s stopped", ide_lsp_client_get_server_id(client));
    }
  }
  update_status(mgr);
}

static void
publish_diagnostics(IdeLspManager *mgr, JsonNode *params)
{
  if (params == NULL || !JSON_NODE_HOLDS_OBJECT(params)) {
    return;
  }
  JsonObject *po = json_node_get_object(params);
  const char *uri = json_object_has_member(po, "uri") ? json_object_get_string_member(po, "uri") : NULL;
  DocState *doc = uri != NULL ? g_hash_table_lookup(mgr->docs_by_uri, uri) : NULL;
  if (doc == NULL) {
    return;
  }

  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(doc->buf));
  ide_buffer_clear_diagnostics(doc->buf);
  g_clear_pointer(&doc->raw_diagnostics, json_node_unref);
  if (json_object_has_member(po, "diagnostics")) {
    doc->raw_diagnostics = json_node_copy(json_object_get_member(po, "diagnostics"));
  }

  JsonArray *arr = json_object_has_member(po, "diagnostics")
                       ? json_object_get_array_member(po, "diagnostics")
                       : NULL;
  for (guint i = 0; arr != NULL && i < json_array_get_length(arr); i++) {
    JsonObject *d = json_array_get_object_element(arr, i);
    if (d == NULL || !json_object_has_member(d, "range")) {
      continue;
    }
    JsonObject *start = json_object_get_object_member(json_object_get_object_member(d, "range"), "start");
    guint line0 = (guint)json_object_get_int_member_with_default(start, "line", 0);
    guint ch = (guint)json_object_get_int_member_with_default(start, "character", 0);
    gint sev = (gint)json_object_get_int_member_with_default(d, "severity", 1);

    IdeDiagSeverity severity = IDE_DIAG_ERROR;
    switch (sev) {
      case 2: severity = IDE_DIAG_WARNING; break;
      case 3: severity = IDE_DIAG_INFO; break;
      case 4: severity = IDE_DIAG_HINT; break;
      default: break;
    }
    const char *msg = json_object_has_member(d, "message") ? json_object_get_string_member(d, "message") : "";
    const char *src = json_object_has_member(d, "source") ? json_object_get_string_member(d, "source")
                                                           : (doc->spec ? doc->spec->server_id : "lsp");
    ide_buffer_add_diagnostic(doc->buf, line0 + 1, ide_lsp_buffer_char_col_1based(tbuf, line0, ch), severity, msg, src);
  }
  if (mgr->diagnostics_cb != NULL) {
    mgr->diagnostics_cb(doc->buf, mgr->ui_data);
  }
}

static void
on_client_notification(IdeLspClient *client, const char *method, JsonNode *params, gpointer user_data)
{
  IdeLspManager *mgr = user_data;
  if (mgr->shutting_down) {
    return;
  }
  if (g_strcmp0(method, "textDocument/publishDiagnostics") == 0) {
    publish_diagnostics(mgr, params);
  } else if (g_strcmp0(method, "window/showMessage") == 0 && params != NULL &&
             JSON_NODE_HOLDS_OBJECT(params)) {
    JsonObject *o = json_node_get_object(params);
    if (json_object_get_int_member_with_default(o, "type", 3) <= 2 && json_object_has_member(o, "message")) {
      ide_lsp_report_message(mgr, "%s: %s", ide_lsp_client_get_server_id(client),
                             json_object_get_string_member(o, "message"));
    }
  }
}

static JsonNode *
on_server_request(IdeLspClient *client, const char *method, JsonNode *params, gpointer user_data)
{
  IdeLspManager *mgr = user_data;

  if (g_strcmp0(method, "workspace/configuration") == 0) {
    /* One (null) configuration object per requested item. */
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_array(b);
    if (params != NULL && JSON_NODE_HOLDS_OBJECT(params)) {
      JsonArray *items = json_object_get_array_member(json_node_get_object(params), "items");
      for (guint i = 0; items != NULL && i < json_array_get_length(items); i++) {
        json_builder_add_null_value(b);
      }
    }
    json_builder_end_array(b);
    return json_builder_get_root(b);
  }

  if (g_strcmp0(method, "workspace/applyEdit") == 0 && params != NULL && JSON_NODE_HOLDS_OBJECT(params)) {
    JsonObject *po = json_node_get_object(params);
    guint edits = 0, files = 0;
    if (json_object_has_member(po, "edit")) {
      ide_lsp_apply_workspace_edit(mgr, json_object_get_member(po, "edit"), &edits, &files);
    }
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "applied");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);
    return json_builder_get_root(b);
  }

  if (g_strcmp0(method, "workspace/workspaceFolders") == 0) {
    g_autofree char *uri = g_filename_to_uri(ide_lsp_client_get_root_path(client), NULL, NULL);
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_array(b);
    if (uri != NULL) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "uri");
      json_builder_add_string_value(b, uri);
      json_builder_set_member_name(b, "name");
      json_builder_add_string_value(b, "kajo");
      json_builder_end_object(b);
    }
    json_builder_end_array(b);
    return json_builder_get_root(b);
  }

  return NULL; /* registerCapability, workDoneProgress/create, ... -> null */
}

/* Returns a running (or starting) client for the spec/root, creating or
 * restarting it as needed. NULL when the server is not installed or keeps dying. */
static IdeLspClient *
get_client(IdeLspManager *mgr, const IdeLspServerSpec *spec, const char *root)
{
  g_autofree char *key = client_key(spec->server_id, root);
  ClientEntry *ce = g_hash_table_lookup(mgr->clients, key);
  if (ce != NULL && ide_lsp_client_get_state(ce->client) != IDE_LSP_CLIENT_STOPPED) {
    return ce->client;
  }
  if (ce != NULL && ce->restarts >= 3) {
    return NULL;
  }
  if (g_hash_table_contains(mgr->missing, spec->server_id)) {
    return NULL;
  }

  g_auto(GStrv) argv = ide_lsp_build_argv(spec, root);
  if (argv == NULL || argv[0] == NULL) {
    return NULL;
  }
  g_autofree char *found = g_find_program_in_path(argv[0]);
  if (found == NULL) {
    g_hash_table_add(mgr->missing, g_strdup(spec->server_id));
    return NULL;
  }

  IdeLspClient *client = ide_lsp_client_new(spec->server_id, (const char *const *)argv, root,
                                            ide_lsp_server_init_options(spec));
  ide_lsp_client_set_callbacks(client, on_client_notification, on_server_request, on_client_state, mgr);

  guint restarts = ce != NULL ? ce->restarts + 1 : 0;
  ClientEntry *fresh = g_new0(ClientEntry, 1);
  fresh->client = client;
  fresh->restarts = restarts;
  IdeLspClient *old = ce != NULL ? ce->client : NULL;

  /* Re-point documents of the dead client at the replacement. */
  if (old != NULL) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, mgr->docs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      DocState *doc = v;
      if (doc->client == old) {
        doc->client = client;
        doc->sent_open = FALSE;
      }
    }
  }
  g_hash_table_insert(mgr->clients, g_strdup(key), fresh); /* frees the old entry */

  GError *error = NULL;
  if (!ide_lsp_client_start(client, &error)) {
    g_debug("ide-lsp: cannot start %s: %s", spec->server_id, error ? error->message : "?");
    g_clear_error(&error);
    g_hash_table_add(mgr->missing, g_strdup(spec->server_id));
    return NULL;
  }
  return client;
}

static void
doc_destroy_notify(IdeBuffer *buf, gpointer user_data)
{
  ide_lsp_manager_notify_close(user_data, buf);
}

/* ═══════════════════════════ Public: lifecycle ═══════════════════════════ */

IdeLspManager *
ide_lsp_manager_new(const char *workspace_root, IdeLspJumpCb jump_cb, IdeLspStatusCb status_cb,
                    gpointer user_data)
{
  IdeLspManager *lsp = g_new0(IdeLspManager, 1);
  lsp->workspace_root = g_strdup(workspace_root ? workspace_root : ".");
  lsp->jump_cb = jump_cb;
  lsp->status_cb = status_cb;
  lsp->cb_data = user_data;
  lsp->comp_items = g_ptr_array_new_with_free_func(ide_completion_item_free);
  lsp->code_actions = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_unref);
  lsp->ref_items = g_ptr_array_new_with_free_func(ide_lsp_location_free);
  lsp->sig_items = g_ptr_array_new_with_free_func(ide_lsp_signature_info_free);
  lsp->sig_active_param = -1;
  lsp->call_items = g_ptr_array_new_with_free_func(ide_lsp_call_entry_free);
  lsp->clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, client_entry_free);
  lsp->docs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, doc_free);
  lsp->docs_by_uri = g_hash_table_new(g_str_hash, g_str_equal);
  lsp->missing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  lsp->live_ctx = g_hash_table_new(g_direct_hash, g_direct_equal);
  return lsp;
}

void
ide_lsp_manager_set_workspace_root(IdeLspManager *lsp, const char *workspace_root)
{
  g_return_if_fail(lsp != NULL);
  char *dup = g_strdup(workspace_root ? workspace_root : ".");
  if (lsp->rg_cancellable != NULL) {
    g_cancellable_cancel(lsp->rg_cancellable);
    g_clear_object(&lsp->rg_cancellable);
  }
  g_free(lsp->workspace_root);
  lsp->workspace_root = dup;
}

void
ide_lsp_manager_set_callbacks(IdeLspManager *lsp, IdeLspDiagnosticsCb diagnostics_cb,
                              IdeLspMessageCb message_cb, IdeLspUiStateCb ui_state_cb,
                              gpointer user_data)
{
  g_return_if_fail(lsp != NULL);
  lsp->diagnostics_cb = diagnostics_cb;
  lsp->message_cb = message_cb;
  lsp->ui_state_cb = ui_state_cb;
  lsp->ui_data = user_data;
}

void
ide_lsp_manager_free(IdeLspManager *lsp)
{
  if (lsp == NULL) {
    return;
  }
  lsp->shutting_down = TRUE;
  ide_lsp_manager_cancel_popover(lsp);

  /* Detach buffers so their free doesn't call back into a dead manager. */
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, lsp->docs);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    DocState *doc = v;
    ide_buffer_set_destroy_notify(doc->buf, NULL, NULL);
  }

  /* Ask servers to exit cleanly (jdtls spawns a JVM that a SIGKILL would
   * orphan); give them a short grace period before they are force-killed. */
  g_hash_table_iter_init(&it, lsp->clients);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    ide_lsp_client_stop(((ClientEntry *)v)->client);
  }
  g_hash_table_iter_init(&it, lsp->clients);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    ide_lsp_client_wait_exit(((ClientEntry *)v)->client, 1500);
  }

  GHashTableIter cit;
  g_hash_table_iter_init(&cit, lsp->live_ctx);
  GPtrArray *ctxs = g_ptr_array_new();
  while (g_hash_table_iter_next(&cit, &k, NULL)) {
    g_ptr_array_add(ctxs, k);
  }
  for (guint i = 0; i < ctxs->len; i++) {
    request_ctx_free(g_ptr_array_index(ctxs, i));
  }
  g_ptr_array_free(ctxs, TRUE);
  g_hash_table_destroy(lsp->live_ctx);

  g_hash_table_destroy(lsp->clients);
  g_hash_table_destroy(lsp->docs_by_uri);
  g_hash_table_destroy(lsp->docs);
  g_hash_table_destroy(lsp->missing);
  if (lsp->rg_cancellable != NULL) {
    g_cancellable_cancel(lsp->rg_cancellable);
    g_clear_object(&lsp->rg_cancellable);
  }
  g_free(lsp->workspace_root);
  g_free(lsp->active_server_name);
  g_free(lsp->comp_prefix);
  g_clear_pointer(&lsp->comp_items, g_ptr_array_unref);
  g_clear_pointer(&lsp->code_actions, g_ptr_array_unref);
  g_clear_pointer(&lsp->ref_items, g_ptr_array_unref);
  g_clear_pointer(&lsp->sig_items, g_ptr_array_unref);
  g_clear_pointer(&lsp->call_items, g_ptr_array_unref);
  g_clear_pointer(&lsp->call_root_item, json_node_unref);
  g_clear_pointer(&lsp->call_root_uri, g_free);
  if (lsp->rename_input != NULL) {
    g_string_free(lsp->rename_input, TRUE);
  }
  g_free(lsp->rename_old);
  g_free(lsp);
}

void
ide_lsp_manager_notify_open(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  if (buf == NULL) {
    return;
  }

  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc != NULL) {
    lsp->active_doc = doc;
    /* A dead client may be replaced when the user comes back to the tab. */
    if (doc->client != NULL && ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_STOPPED &&
        doc->spec != NULL) {
      IdeLspClient *c = get_client(lsp, doc->spec, doc->root);
      if (c != NULL) {
        doc->client = c;
        doc_send_open(doc);
      }
    }
    update_status(lsp);
    return;
  }

  const char *path = ide_buffer_get_file_path(buf);
  const IdeLspServerSpec *spec = ide_lsp_server_spec_for_file(path);

  doc = g_new0(DocState, 1);
  doc->mgr = lsp;
  doc->buf = buf;
  doc->spec = spec;
  doc->version = 1;
  g_hash_table_insert(lsp->docs, buf, doc);
  ide_buffer_set_destroy_notify(buf, doc_destroy_notify, lsp);
  lsp->active_doc = doc;

  if (path != NULL && spec != NULL) {
    doc->path = g_strdup(path);
    doc->uri = g_filename_to_uri(path, NULL, NULL);
    doc->language_id = g_strdup(ide_lsp_language_id_for_file(path));
    doc->root = ide_lsp_find_root(spec, path, lsp->workspace_root);
    if (doc->uri != NULL) {
      g_hash_table_insert(lsp->docs_by_uri, doc->uri, doc);
      doc->client = get_client(lsp, spec, doc->root);
      doc_send_open(doc);
    }
  }
  update_status(lsp);
}

void
ide_lsp_manager_notify_change(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  if (buf == NULL) {
    return;
  }
  ide_buffer_recompute_diffs(buf);

  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc == NULL || doc->client == NULL || !doc->sent_open) {
    return;
  }
  if (doc->change_timer != 0) {
    g_source_remove(doc->change_timer);
  }
  doc->change_timer = g_timeout_add(120, on_change_timer, doc);
}

void
ide_lsp_manager_notify_save(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  DocState *doc = buf != NULL ? g_hash_table_lookup(lsp->docs, buf) : NULL;
  if (doc == NULL || doc->client == NULL || !doc->sent_open) {
    return;
  }
  ide_lsp_doc_send_change_now(doc);
  ide_lsp_client_notify(doc->client, "textDocument/didSave", ide_lsp_text_document_params(doc->uri));
}

void
ide_lsp_manager_notify_close(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  DocState *doc = buf != NULL ? g_hash_table_lookup(lsp->docs, buf) : NULL;
  if (doc == NULL) {
    return;
  }
  if (doc->client != NULL && doc->sent_open &&
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_STOPPED) {
    ide_lsp_client_notify(doc->client, "textDocument/didClose", ide_lsp_text_document_params(doc->uri));
  }
  if (lsp->active_doc == doc) {
    lsp->active_doc = NULL;
  }
  if (doc->uri != NULL) {
    g_hash_table_remove(lsp->docs_by_uri, doc->uri);
  }
  if (lsp->active_buf == buf) {
    ide_lsp_manager_cancel_popover(lsp);
  }
  ide_lsp_cancel_inflight(lsp);
  g_hash_table_remove(lsp->docs, buf); /* frees doc */
  if (!lsp->shutting_down) {
    update_status(lsp);
  }
}

gboolean
ide_lsp_manager_has_server(IdeLspManager *lsp, IdeBuffer *buf)
{
  DocState *doc = (lsp && buf) ? g_hash_table_lookup(lsp->docs, buf) : NULL;
  return doc != NULL && doc->client != NULL &&
         ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_STOPPED;
}

gboolean
ide_lsp_manager_is_ready(IdeLspManager *lsp, IdeBuffer *buf)
{
  DocState *doc = (lsp && buf) ? g_hash_table_lookup(lsp->docs, buf) : NULL;
  return doc != NULL && doc->client != NULL &&
         ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_READY;
}

/* ═══════════════════════════ workspace/executeCommand & raw bridge ═══════════════════════════ */

static void
on_command_response(IdeLspClient *client, JsonNode *result, const char *error_message, gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspCommandCb cb = ctx->command_cb;
  gpointer ud = ctx->command_data;
  gboolean quiet = ctx->mgr->shutting_down;
  ide_lsp_ctx_done(ctx);
  if (cb != NULL && !quiet) {
    cb(error_message == NULL ? result : NULL, error_message, ud);
  }
}

gboolean
ide_lsp_manager_execute_command(IdeLspManager *lsp, const char *file_path, const char *command,
                                JsonNode *arguments, IdeLspCommandCb cb, gpointer user_data)
{
  g_return_val_if_fail(lsp != NULL && command != NULL, FALSE);
  g_autoptr(JsonNode) owned = arguments;

  DocState *doc = NULL;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, lsp->docs);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    DocState *d = v;
    if (d->path != NULL && g_strcmp0(d->path, file_path) == 0) {
      doc = d;
      break;
    }
  }
  if (doc == NULL || doc->client == NULL ||
      ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_STOPPED) {
    return FALSE;
  }

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "command");
  json_builder_add_string_value(b, command);
  json_builder_set_member_name(b, "arguments");
  if (owned != NULL) {
    json_builder_add_value(b, json_node_copy(owned));
  } else {
    json_builder_begin_array(b);
    json_builder_end_array(b);
  }
  json_builder_end_object(b);

  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_COMMAND, doc->uri);
  ctx->command_cb = cb;
  ctx->command_data = user_data;
  ide_lsp_client_request(doc->client, "workspace/executeCommand", json_builder_get_root(b),
                         on_command_response, ctx);
  return TRUE;
}

static void
on_raw_response(IdeLspClient *client, JsonNode *result, const char *error_message, gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspRawCb cb = ctx->raw_cb;
  gpointer ud = ctx->raw_data;
  gboolean quiet = ctx->mgr->shutting_down;
  ide_lsp_ctx_done(ctx);
  if (cb != NULL && !quiet) {
    cb(error_message == NULL ? result : NULL, error_message, ud);
  }
}

gboolean
ide_lsp_manager_request_raw(IdeLspManager *lsp,
                            IdeBuffer     *buf,
                            const char    *method,
                            JsonNode      *params,
                            IdeLspRawCb    cb,
                            gpointer       user_data)
{
  g_autoptr(JsonNode) owned = params;
  g_return_val_if_fail(lsp != NULL && method != NULL, FALSE);
  if (buf == NULL) {
    return FALSE;
  }

  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc == NULL || doc->client == NULL || !doc->sent_open ||
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_READY) {
    return FALSE;
  }

  ide_lsp_doc_send_change_now(doc);

  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_RAW, doc->uri);
  ctx->raw_cb = cb;
  ctx->raw_data = user_data;
  ide_lsp_client_request(doc->client, method, g_steal_pointer(&owned),
                         on_raw_response, ctx);
  return TRUE;
}
