#include "ide_lsp.h"
#include "ide_picker.h"
#include "ide_search.h"

#include <string.h>

/* ═══════════════════════════ Pure helpers ═══════════════════════════ */

IdeLspTextEdit *
ide_lsp_text_edit_new(guint line_start, guint char_start, guint line_end, guint char_end,
                      const char *new_text)
{
  IdeLspTextEdit *e = g_new0(IdeLspTextEdit, 1);
  e->line_start = line_start;
  e->char_start = char_start;
  e->line_end = line_end;
  e->char_end = char_end;
  e->new_text = g_strdup(new_text ? new_text : "");
  return e;
}

void
ide_lsp_text_edit_free(gpointer data)
{
  IdeLspTextEdit *e = data;
  if (e != NULL) {
    g_free(e->new_text);
    g_free(e);
  }
}

static gboolean
json_range_to_edit(JsonObject *range, const char *new_text, IdeLspTextEdit *out)
{
  if (range == NULL || !json_object_has_member(range, "start") ||
      !json_object_has_member(range, "end")) {
    return FALSE;
  }
  JsonObject *s = json_object_get_object_member(range, "start");
  JsonObject *e = json_object_get_object_member(range, "end");
  if (s == NULL || e == NULL) {
    return FALSE;
  }
  out->line_start = (guint)json_object_get_int_member_with_default(s, "line", 0);
  out->char_start = (guint)json_object_get_int_member_with_default(s, "character", 0);
  out->line_end = (guint)json_object_get_int_member_with_default(e, "line", 0);
  out->char_end = (guint)json_object_get_int_member_with_default(e, "character", 0);
  out->new_text = g_strdup(new_text ? new_text : "");
  return TRUE;
}

GPtrArray *
ide_lsp_parse_text_edits(JsonNode *edits_node)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);
  if (edits_node == NULL || !JSON_NODE_HOLDS_ARRAY(edits_node)) {
    return out;
  }
  JsonArray *arr = json_node_get_array(edits_node);
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonNode *n = json_array_get_element(arr, i);
    if (!JSON_NODE_HOLDS_OBJECT(n)) {
      continue;
    }
    JsonObject *o = json_node_get_object(n);
    IdeLspTextEdit tmp = { 0 };
    /* TextEdit {range,newText} or InsertReplaceEdit {insert,replace,newText} */
    JsonObject *range = json_object_has_member(o, "range")
                            ? json_object_get_object_member(o, "range")
                            : (json_object_has_member(o, "replace")
                                   ? json_object_get_object_member(o, "replace")
                                   : NULL);
    const char *text = json_object_has_member(o, "newText")
                           ? json_object_get_string_member(o, "newText")
                           : "";
    if (json_range_to_edit(range, text, &tmp)) {
      IdeLspTextEdit *e = g_memdup2(&tmp, sizeof tmp);
      g_ptr_array_add(out, e);
    }
  }
  return out;
}

/* Byte offset of LSP position (line, UTF-16 character) in `text`, or -1. */
static gssize
lsp_pos_to_byte(const char *text, gsize len, guint line, guint character)
{
  gsize i = 0;
  guint cur = 0;
  while (cur < line) {
    const char *nl = memchr(text + i, '\n', len - i);
    if (nl == NULL) {
      return -1;
    }
    i = (gsize)(nl - text) + 1;
    cur++;
  }
  guint units = 0;
  while (i < len && text[i] != '\n' && units < character) {
    gunichar ch = g_utf8_get_char_validated(text + i, (gssize)(len - i));
    gsize step = 1;
    if (ch != (gunichar)-1 && ch != (gunichar)-2) {
      step = (gsize)(g_utf8_next_char(text + i) - (text + i));
      units += (ch >= 0x10000) ? 2 : 1;
    } else {
      units++;
    }
    i += step;
  }
  return (gssize)i;
}

static gint
edit_cmp(gconstpointer a, gconstpointer b)
{
  const IdeLspTextEdit *x = *(const IdeLspTextEdit *const *)a;
  const IdeLspTextEdit *y = *(const IdeLspTextEdit *const *)b;
  if (x->line_start != y->line_start) {
    return x->line_start < y->line_start ? -1 : 1;
  }
  if (x->char_start != y->char_start) {
    return x->char_start < y->char_start ? -1 : 1;
  }
  return 0;
}

char *
ide_lsp_apply_edits_to_string(const char *text, const GPtrArray *edits)
{
  g_return_val_if_fail(text != NULL, NULL);
  gsize len = strlen(text);
  if (edits == NULL || edits->len == 0) {
    return g_strdup(text);
  }

  g_autoptr(GPtrArray) sorted = g_ptr_array_new();
  for (guint i = 0; i < edits->len; i++) {
    g_ptr_array_add(sorted, g_ptr_array_index((GPtrArray *)edits, i));
  }
  g_ptr_array_sort(sorted, edit_cmp);

  GString *out = g_string_new(NULL);
  gsize cursor = 0;
  for (guint i = 0; i < sorted->len; i++) {
    const IdeLspTextEdit *e = g_ptr_array_index(sorted, i);
    gssize s = lsp_pos_to_byte(text, len, e->line_start, e->char_start);
    gssize en = lsp_pos_to_byte(text, len, e->line_end, e->char_end);
    if (s < 0 || en < s || (gsize)s < cursor) {
      g_string_free(out, TRUE);
      return NULL;
    }
    g_string_append_len(out, text + cursor, (gssize)((gsize)s - cursor));
    g_string_append(out, e->new_text);
    cursor = (gsize)en;
  }
  g_string_append_len(out, text + cursor, (gssize)(len - cursor));
  return g_string_free(out, FALSE);
}

char *
ide_lsp_strip_snippet(const char *snippet)
{
  if (snippet == NULL) {
    return g_strdup("");
  }
  GString *out = g_string_new(NULL);
  const char *p = snippet;
  while (*p != '\0') {
    if (*p == '\\' && p[1] != '\0') {
      g_string_append_c(out, p[1]);
      p += 2;
    } else if (*p == '$') {
      p++;
      if (*p == '{') {
        /* ${n:default} / ${n|a,b|} / ${n} */
        const char *q = p + 1;
        while (g_ascii_isdigit(*q)) {
          q++;
        }
        if (*q == ':') {
          q++;
          int depth = 1;
          while (*q != '\0' && depth > 0) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) break; }
            if (depth > 0) g_string_append_c(out, *q);
            q++;
          }
          p = (*q == '}') ? q + 1 : q;
        } else {
          while (*q != '\0' && *q != '}') {
            q++;
          }
          p = (*q == '}') ? q + 1 : q;
        }
      } else {
        while (g_ascii_isdigit(*p)) {
          p++;
        }
      }
    } else {
      g_string_append_c(out, *p++);
    }
  }
  return g_string_free(out, FALSE);
}

char *
ide_lsp_hover_to_markdown(JsonNode *contents)
{
  if (contents == NULL || JSON_NODE_HOLDS_NULL(contents)) {
    return g_strdup("");
  }
  if (JSON_NODE_HOLDS_VALUE(contents)) {
    return g_strdup(json_node_get_string(contents) ? json_node_get_string(contents) : "");
  }
  if (JSON_NODE_HOLDS_OBJECT(contents)) {
    JsonObject *o = json_node_get_object(contents);
    const char *value = json_object_has_member(o, "value") ? json_object_get_string_member(o, "value") : "";
    if (json_object_has_member(o, "language")) {
      return g_strdup_printf("```%s\n%s\n```", json_object_get_string_member(o, "language"), value);
    }
    return g_strdup(value);
  }
  if (JSON_NODE_HOLDS_ARRAY(contents)) {
    GString *out = g_string_new(NULL);
    JsonArray *arr = json_node_get_array(contents);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
      g_autofree char *part = ide_lsp_hover_to_markdown(json_array_get_element(arr, i));
      if (part[0] == '\0') {
        continue;
      }
      if (out->len > 0) {
        g_string_append(out, "\n\n");
      }
      g_string_append(out, part);
    }
    return g_string_free(out, FALSE);
  }
  return g_strdup("");
}

const char *
ide_lsp_detect_server_for_file(const char *file_path, const char **out_server_id)
{
  if (out_server_id != NULL) {
    *out_server_id = "none";
  }
  const IdeLspServerSpec *spec = ide_lsp_server_spec_for_file(file_path);
  if (spec == NULL) {
    return NULL;
  }
  if (out_server_id != NULL) {
    *out_server_id = spec->server_id;
  }
  return spec->binary;
}

void
ide_completion_item_free(gpointer data)
{
  IdeCompletionItem *it = data;
  if (it == NULL) {
    return;
  }
  g_free(it->label);
  g_free(it->detail);
  g_free(it->insert_text);
  g_free(it->documentation);
  g_free(it->filter_text);
  g_free(it->sort_text);
  g_free(it->edit.new_text);
  g_clear_pointer(&it->extra_edits, g_ptr_array_unref);
  g_free(it);
}

/* ═══════════════════════════ Manager state ═══════════════════════════ */

typedef struct _DocState DocState;

typedef struct {
  IdeLspClient *client;
  guint         restarts;
} ClientEntry;

typedef enum {
  CTX_HOVER,
  CTX_COMPLETION,
  CTX_DEFINITION,
  CTX_RENAME,
  CTX_FORMAT,
  CTX_COMMAND,
} CtxKind;

typedef struct {
  IdeLspManager *mgr;
  CtxKind        kind;
  char          *uri;
  guint          token;
  GtkWidget     *anchor;     /* weak: cleared when the widget is destroyed */
  IdeBuffer     *buf;
  guint          line_1based;
  guint          col_1based;
  char          *prefix;     /* completion: text typed before the request */
  char          *new_name;
  guint          tab_size;
  IdeLspCommandCb command_cb;
  gpointer        command_data;
} RequestCtx;

struct _DocState {
  IdeLspManager          *mgr;
  IdeBuffer              *buf;
  char                   *path;
  char                   *uri;
  char                   *language_id;
  const IdeLspServerSpec *spec;
  char                   *root;
  IdeLspClient           *client;
  gint                    version;
  JsonNode               *raw_diagnostics; /* last publishDiagnostics array (for code actions) */
  guint                   change_timer;
  gboolean                sent_open;
};

struct _IdeLspManager {
  char                *workspace_root;
  IdeLspJumpCb         jump_cb;
  IdeLspStatusCb       status_cb;
  gpointer             cb_data;
  IdeLspDiagnosticsCb  diagnostics_cb;
  IdeLspMessageCb      message_cb;
  IdeLspUiStateCb      ui_state_cb;
  IdeLspUiState        ui_state;
  gpointer             ui_data;

  GHashTable          *clients;      /* "id|root" -> ClientEntry* */
  GHashTable          *docs;         /* IdeBuffer* -> DocState* */
  GHashTable          *docs_by_uri;  /* uri (borrowed from DocState) -> DocState* */
  GHashTable          *missing;      /* server_id -> TRUE */
  GHashTable          *live_ctx;     /* RequestCtx* set */
  DocState            *active_doc;
  gboolean             shutting_down;

  /* popover (hover / completion) */
  char                *active_server_name;
  GtkWidget           *active_popover;
  GtkWidget           *active_anchor;   /* source view the popover points into */
  IdeBuffer           *active_buf;      /* buffer whose cursor it tracks */
  GtkWidget           *comp_list_box;
  GtkWidget           *comp_scroller;
  GtkWidget           *comp_doc_label;
  GPtrArray           *comp_items;
  gint                 selected_comp_idx;
  char                *comp_prefix;
  guint                popover_token;
  gboolean             comp_is_code_actions; /* the list shows code actions */
  GPtrArray           *code_actions;         /* JsonNode* parallel to comp_items */
  GString             *rename_input;
  char                *rename_old;
  GtkWidget           *rename_label;

  /* the single in-flight popover request, cancelled by the next one */
  IdeLspClient        *inflight_client;
  guint                inflight_id;
  RequestCtx          *inflight_ctx;
  GCancellable        *rg_cancellable;
};

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

static RequestCtx *
ctx_new(IdeLspManager *mgr, CtxKind kind, const char *uri)
{
  RequestCtx *ctx = g_new0(RequestCtx, 1);
  ctx->mgr = mgr;
  ctx->kind = kind;
  ctx->uri = g_strdup(uri);
  g_hash_table_add(mgr->live_ctx, ctx);
  return ctx;
}

static void
ctx_done(RequestCtx *ctx)
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

static void
ctx_set_anchor(RequestCtx *ctx, GtkWidget *anchor)
{
  ctx->anchor = anchor;
  if (anchor != NULL) {
    g_object_add_weak_pointer(G_OBJECT(anchor), (gpointer *)&ctx->anchor);
  }
}

static void
cancel_inflight(IdeLspManager *mgr)
{
  if (mgr->inflight_ctx == NULL) {
    return;
  }
  RequestCtx *ctx = mgr->inflight_ctx;
  if (mgr->inflight_client != NULL && mgr->inflight_id != 0) {
    ide_lsp_client_cancel(mgr->inflight_client, mgr->inflight_id);
  }
  ctx_done(ctx);
}

/* ─── positions ─── */

static char *
buffer_line_text(GtkTextBuffer *tbuf, guint line0)
{
  GtkTextIter s, e;
  gtk_text_buffer_get_iter_at_line(tbuf, &s, (gint)line0);
  e = s;
  if (!gtk_text_iter_ends_line(&e)) {
    gtk_text_iter_forward_to_line_end(&e);
  }
  return gtk_text_buffer_get_text(tbuf, &s, &e, TRUE);
}

static void
buffer_cursor_lsp_position(IdeBuffer *buf, guint *line0, guint *utf16)
{
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(tbuf, &it, gtk_text_buffer_get_insert(tbuf));
  *line0 = (guint)gtk_text_iter_get_line(&it);
  g_autofree char *line = buffer_line_text(tbuf, *line0);
  *utf16 = ide_utf8_byte_to_utf16_col(line, (gsize)gtk_text_iter_get_line_index(&it));
}

static void
buffer_iter_at_lsp(GtkTextBuffer *tbuf, guint line0, guint utf16, GtkTextIter *out)
{
  gint last = gtk_text_buffer_get_line_count(tbuf) - 1;
  if ((gint)line0 > last) {
    gtk_text_buffer_get_end_iter(tbuf, out);
    return;
  }
  g_autofree char *line = buffer_line_text(tbuf, line0);
  gsize byte = ide_utf16_col_to_utf8_byte(line, utf16);
  byte = MIN(byte, strlen(line));
  gtk_text_buffer_get_iter_at_line_index(tbuf, out, (gint)line0, (gint)byte);
}

/* 1-based character column of an LSP (line, utf16) position in a buffer line. */
static guint
buffer_char_col_1based(GtkTextBuffer *tbuf, guint line0, guint utf16)
{
  gint last = gtk_text_buffer_get_line_count(tbuf) - 1;
  if ((gint)line0 > last) {
    return 1;
  }
  g_autofree char *line = buffer_line_text(tbuf, line0);
  gsize byte = MIN(ide_utf16_col_to_utf8_byte(line, utf16), strlen(line));
  return (guint)g_utf8_pointer_to_offset(line, line + byte) + 1;
}

/* Same conversion for a file on disk (definition targets that are not open). */
static void
file_lsp_to_line_col(const char *path, guint line0, guint utf16, guint *line1, guint *col1)
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

/* ─── status ─── */

static void
report_message(IdeLspManager *mgr, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
report_message(IdeLspManager *mgr, const char *fmt, ...)
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

/* ─── doc sync ─── */

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

static JsonNode *
text_document_params(const char *uri)
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

static void
doc_send_change_now(DocState *doc)
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
  doc_send_change_now(doc);
  return G_SOURCE_REMOVE;
}

/* ─── client lifecycle ─── */

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
      report_message(mgr, "Language server %s stopped", ide_lsp_client_get_server_id(client));
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
    ide_buffer_add_diagnostic(doc->buf, line0 + 1, buffer_char_col_1based(tbuf, line0, ch), severity, msg, src);
  }
  if (mgr->diagnostics_cb != NULL) {
    mgr->diagnostics_cb(doc->buf, mgr->ui_data);
  }
}

static void
on_client_notification(IdeLspClient *client, const char *method, JsonNode *params, gpointer user_data)
{
  (void)client;
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
      report_message(mgr, "%s: %s", ide_lsp_client_get_server_id(client),
                     json_object_get_string_member(o, "message"));
    }
  }
}

static void apply_workspace_edit(IdeLspManager *mgr, JsonNode *edit, guint *edits_out, guint *files_out);

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
      apply_workspace_edit(mgr, json_object_get_member(po, "edit"), &edits, &files);
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
  doc_send_change_now(doc);
  ide_lsp_client_notify(doc->client, "textDocument/didSave", text_document_params(doc->uri));
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
    ide_lsp_client_notify(doc->client, "textDocument/didClose", text_document_params(doc->uri));
  }
  if (lsp->active_doc == doc) {
    lsp->active_doc = NULL;
  }
  if (doc->uri != NULL) {
    g_hash_table_remove(lsp->docs_by_uri, doc->uri);
  }
  cancel_inflight(lsp);
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

/* ═══════════════════════════ Editing helpers ═══════════════════════════ */

static GdkRectangle
compute_cursor_rect(GtkWidget *anchor_view, IdeBuffer *buf)
{
  GdkRectangle rect = { 40, 40, 12, 20 };
  if (!GTK_IS_TEXT_VIEW(anchor_view) || buf == NULL) {
    return rect;
  }
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(tbuf, &iter, gtk_text_buffer_get_insert(tbuf));

  GdkRectangle strong;
  gtk_text_view_get_cursor_locations(GTK_TEXT_VIEW(anchor_view), &iter, &strong, NULL);
  gint wx = 0;
  gint wy = 0;
  gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(anchor_view),
                                        GTK_TEXT_WINDOW_WIDGET,
                                        strong.x,
                                        strong.y,
                                        &wx,
                                        &wy);
  rect.x = MAX(8, wx);
  rect.y = MAX(8, wy);
  rect.width = MAX(8, strong.width);
  rect.height = MAX(18, strong.height);
  return rect;
}

static char *
extract_word_at_cursor(IdeBuffer *buf, char **out_prefix)
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


static void
apply_edits_to_buffer(IdeLspManager *mgr, IdeBuffer *buf, const GPtrArray *edits)
{
  if (edits == NULL || edits->len == 0) {
    return;
  }
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));

  g_autoptr(GPtrArray) sorted = g_ptr_array_new();
  for (guint i = 0; i < edits->len; i++) {
    g_ptr_array_add(sorted, g_ptr_array_index((GPtrArray *)edits, i));
  }
  g_ptr_array_sort(sorted, edit_cmp);

  /* Descending order keeps earlier positions valid while editing. */
  gtk_text_buffer_begin_user_action(tbuf);
  for (guint i = sorted->len; i > 0; i--) {
    const IdeLspTextEdit *e = g_ptr_array_index(sorted, i - 1);
    GtkTextIter s, en;
    buffer_iter_at_lsp(tbuf, e->line_start, e->char_start, &s);
    buffer_iter_at_lsp(tbuf, e->line_end, e->char_end, &en);
    if (gtk_text_iter_compare(&s, &en) < 0) {
      gtk_text_buffer_delete(tbuf, &s, &en);
    }
    if (e->new_text[0] != '\0') {
      gtk_text_buffer_insert(tbuf, &s, e->new_text, -1);
    }
  }
  gtk_text_buffer_end_user_action(tbuf);
  ide_lsp_manager_notify_change(mgr, buf);
}

static gboolean
apply_edits_for_uri(IdeLspManager *mgr, const char *uri, GPtrArray *edits)
{
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, uri);
  if (doc != NULL) {
    apply_edits_to_buffer(mgr, doc->buf, edits);
    return TRUE;
  }
  g_autofree char *path = g_filename_from_uri(uri, NULL, NULL);
  if (path == NULL) {
    return FALSE;
  }
  g_autofree char *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL)) {
    return FALSE;
  }
  g_autofree char *updated = ide_lsp_apply_edits_to_string(contents, edits);
  if (updated == NULL) {
    return FALSE;
  }
  return ide_save_file_atomic(path, updated, -1, NULL);
}

static void
apply_workspace_edit(IdeLspManager *mgr, JsonNode *edit, guint *edits_out, guint *files_out)
{
  *edits_out = 0;
  *files_out = 0;
  if (edit == NULL || !JSON_NODE_HOLDS_OBJECT(edit)) {
    return;
  }
  JsonObject *eo = json_node_get_object(edit);

  if (json_object_has_member(eo, "documentChanges")) {
    JsonArray *dc = json_object_get_array_member(eo, "documentChanges");
    for (guint i = 0; dc != NULL && i < json_array_get_length(dc); i++) {
      JsonObject *item = json_array_get_object_element(dc, i);
      if (item == NULL || !json_object_has_member(item, "textDocument")) {
        continue; /* create / rename / delete resource operations are not supported */
      }
      const char *uri = json_object_get_string_member(json_object_get_object_member(item, "textDocument"), "uri");
      g_autoptr(GPtrArray) edits = ide_lsp_parse_text_edits(json_object_get_member(item, "edits"));
      if (uri != NULL && apply_edits_for_uri(mgr, uri, edits)) {
        *edits_out += edits->len;
        (*files_out)++;
      }
    }
  } else if (json_object_has_member(eo, "changes")) {
    JsonObject *ch = json_object_get_object_member(eo, "changes");
    g_autoptr(GList) members = json_object_get_members(ch);
    for (GList *l = members; l != NULL; l = l->next) {
      g_autoptr(GPtrArray) edits = ide_lsp_parse_text_edits(json_object_get_member(ch, l->data));
      if (apply_edits_for_uri(mgr, l->data, edits)) {
        *edits_out += edits->len;
        (*files_out)++;
      }
    }
  }
}

/* ═══════════════════════════ Popovers ═══════════════════════════ */

static void
set_ui_state(IdeLspManager *lsp, IdeLspUiState state)
{
  if (lsp->ui_state == state) {
    return;
  }
  lsp->ui_state = state;
  if (lsp->ui_state_cb != NULL && !lsp->shutting_down) {
    lsp->ui_state_cb(state, lsp->ui_data);
  }
}

void
ide_lsp_manager_cancel_popover(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  cancel_inflight(lsp);
  if (lsp->rg_cancellable != NULL) {
    g_cancellable_cancel(lsp->rg_cancellable);
    g_clear_object(&lsp->rg_cancellable);
  }
  lsp->popover_token++;
  if (lsp->active_popover != NULL) {
    gtk_popover_popdown(GTK_POPOVER(lsp->active_popover));
    gtk_widget_unparent(lsp->active_popover);
    lsp->active_popover = NULL;
    lsp->active_anchor = NULL;
    lsp->active_buf = NULL;
    lsp->comp_list_box = NULL;
    lsp->comp_scroller = NULL;
    lsp->comp_doc_label = NULL;
    lsp->rename_label = NULL;
  }
  if (lsp->rename_input != NULL) {
    g_string_free(lsp->rename_input, TRUE);
    lsp->rename_input = NULL;
  }
  lsp->comp_is_code_actions = FALSE;
  set_ui_state(lsp, IDE_LSP_UI_CLOSED);
}

gboolean
ide_lsp_manager_popover_visible(IdeLspManager *lsp)
{
  return lsp != NULL && lsp->active_popover != NULL;
}

/* Keeps an open popover pinned to the cursor as the view scrolls; if the cursor
 * (and its anchor rectangle) left the visible area, dismiss it rather than let
 * it float over unrelated code. */
void
ide_lsp_manager_reposition_popover(IdeLspManager *lsp, GtkWidget *anchor_view)
{
  g_return_if_fail(lsp != NULL);
  if (lsp->active_popover == NULL || lsp->active_anchor == NULL || lsp->active_buf == NULL) {
    return;
  }
  if (anchor_view != NULL && anchor_view != lsp->active_anchor) {
    return;
  }
  GtkWidget *view = lsp->active_anchor;
  int h = gtk_widget_get_height(view);
  int w = gtk_widget_get_width(view);
  GdkRectangle rect = compute_cursor_rect(view, lsp->active_buf);
  /* compute_cursor_rect returns widget-window pixels; off-screen => dismiss. */
  if (rect.y + rect.height < 0 || rect.y > h || rect.x > w) {
    ide_lsp_manager_cancel_popover(lsp);
    return;
  }
  gtk_popover_set_pointing_to(GTK_POPOVER(lsp->active_popover), &rect);
}

static GtkWidget *
new_popover_for(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  GtkWidget *pop = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), FALSE);
  gtk_widget_set_parent(pop, anchor_view);
  GdkRectangle rect = compute_cursor_rect(anchor_view, buf);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
  lsp->active_popover = pop;
  lsp->active_anchor = anchor_view;
  lsp->active_buf = buf;
  return pop;
}

/* Markdown -> plain text for hover cards: fences, headings, emphasis, links. */
static char *
plain_markdown_line(const char *line)
{
  GString *out = g_string_new(NULL);
  const char *p = line;
  while (*p == '#') {
    p++;
  }
  if (p != line && *p == ' ') {
    p++;
  }
  while (*p != '\0') {
    if (p[0] == '*' && p[1] == '*') { p += 2; continue; }
    if (p[0] == '_' && p[1] == '_') { p += 2; continue; }
    if (*p == '`') { p++; continue; }
    if (*p == '[') {
      const char *close = strchr(p, ']');
      if (close != NULL && close[1] == '(') {
        const char *end = strchr(close, ')');
        if (end != NULL) {
          g_string_append_len(out, p + 1, close - p - 1);
          p = end + 1;
          continue;
        }
      }
    }
    g_string_append_c(out, *p++);
  }
  return g_string_free(out, FALSE);
}

static void
split_hover_markdown(const char *md, char **code_out, char **doc_out)
{
  GString *code = g_string_new(NULL);
  GString *doc = g_string_new(NULL);
  gboolean in_fence = FALSE;
  gboolean first_fence_done = FALSE;
  gboolean in_first_fence = FALSE;
  guint doc_lines = 0;

  g_auto(GStrv) lines = g_strsplit(md, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    const char *ln = lines[i];
    if (g_str_has_prefix(ln, "```")) {
      in_fence = !in_fence;
      if (in_fence && !first_fence_done) {
        in_first_fence = TRUE;
      } else if (!in_fence && in_first_fence) {
        in_first_fence = FALSE;
        first_fence_done = TRUE;
      }
      continue;
    }
    if (in_first_fence) {
      if (code->len > 0) {
        g_string_append_c(code, '\n');
      }
      g_string_append(code, ln);
      continue;
    }
    if (doc_lines >= 18) {
      continue;
    }
    g_autofree char *plain = plain_markdown_line(ln);
    if (plain[0] == '\0' && (doc->len == 0 || g_str_has_suffix(doc->str, "\n\n"))) {
      continue;
    }
    g_string_append(doc, plain);
    g_string_append_c(doc, '\n');
    doc_lines++;
  }
  g_strstrip(code->str);
  g_strstrip(doc->str);
  *code_out = g_string_free(code, FALSE);
  *doc_out = g_string_free(doc, FALSE);
}

static void
show_hover_card(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf, guint line_1based,
                guint col_1based, const char *title, const char *code, const char *doc_text,
                gboolean is_fallback)
{
  ide_lsp_manager_cancel_popover(lsp);
  GtkWidget *pop = new_popover_for(lsp, anchor_view, buf);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(card, "shell-popover-card");
  gtk_widget_set_size_request(card, 380, -1);

  g_autofree char *hdr_txt = g_strdup_printf("ƒ  %s  (Ln %u, Col %u)%s",
                                             (title && title[0]) ? title : "<cursor>",
                                             line_1based, col_1based,
                                             is_fallback ? "  [RG FALLBACK — NO LSP]" : "");
  GtkWidget *hdr = gtk_label_new(hdr_txt);
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
  gtk_widget_add_css_class(hdr, "shell-popover-title");
  gtk_box_append(GTK_BOX(card), hdr);

  if (code != NULL && code[0] != '\0') {
    GtkWidget *code_lbl = gtk_label_new(code);
    gtk_label_set_xalign(GTK_LABEL(code_lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(code_lbl), TRUE);
    gtk_label_set_selectable(GTK_LABEL(code_lbl), FALSE);
    gtk_widget_add_css_class(code_lbl, "code-preview-box");
    gtk_box_append(GTK_BOX(card), code_lbl);
  }
  if (doc_text != NULL && doc_text[0] != '\0') {
    GtkWidget *doc_lbl = gtk_label_new(doc_text);
    gtk_label_set_xalign(GTK_LABEL(doc_lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(doc_lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(doc_lbl), 64);
    gtk_widget_add_css_class(doc_lbl, "row-subtitle");
    gtk_box_append(GTK_BOX(card), doc_lbl);
  }

  const IdeDiagnostic *diag = ide_buffer_get_line_diagnostic(buf, line_1based);
  if (diag != NULL) {
    g_autofree char *d_txt = g_strdup_printf("Diagnostic [%s]: %s", diag->source, diag->message);
    GtkWidget *dlbl = gtk_label_new(d_txt);
    gtk_label_set_xalign(GTK_LABEL(dlbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(dlbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dlbl), 64);
    gtk_widget_add_css_class(dlbl, "row-subtitle");
    gtk_box_append(GTK_BOX(card), dlbl);
  }

  GtkWidget *hint = gtk_label_new("[Esc / q: Dismiss Hover]   [Space g d: Goto Definition]");
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_widget_add_css_class(hint, "shell-popover-badge");
  gtk_box_append(GTK_BOX(card), hint);

  gtk_popover_set_child(GTK_POPOVER(pop), card);
  gtk_popover_popup(GTK_POPOVER(pop));
  set_ui_state(lsp, IDE_LSP_UI_HOVER);
}

/* ─── Hover ─── */

static void
on_legacy_hover_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GSubprocess *proc = G_SUBPROCESS(source);
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  g_autofree char *out = NULL;
  g_autoptr(GError) err = NULL;
  g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, &err);

  if (mgr->shutting_down || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    ctx_done(ctx);
    return;
  }
  GtkWidget *anchor = ctx->anchor;
  IdeBuffer *buf = ctx->buf;
  guint line_1based = ctx->line_1based;
  guint col_1based = ctx->col_1based;
  g_autofree char *symbol = g_strdup(ctx->prefix);
  gboolean current = (ctx->token == mgr->popover_token) &&
                     (g_hash_table_contains(mgr->docs, buf));
  ctx_done(ctx);
  if (!current || anchor == NULL || buf == NULL) {
    return;
  }
  g_autofree char *decl_line = NULL;
  if (out != NULL && out[0] != '\0') {
    decl_line = g_strstrip(g_strdup(out));
  }
  const char *fallback_note = (decl_line != NULL && decl_line[0] != '\0')
                                  ? NULL
                                  : "No language server active (rg fallback found no declaration).";
  show_hover_card(mgr, anchor, buf, line_1based, col_1based, symbol, decl_line, fallback_note, TRUE);
}

static void
legacy_hover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf, guint line_1based,
             guint col_1based)
{
  g_autofree char *symbol = extract_word_at_cursor(buf, NULL);
  if (symbol != NULL && symbol[0] != '\0' && lsp->workspace_root != NULL) {
    g_autofree char *pat = g_strdup_printf("\\b%s\\s*[\\(;\\{]", symbol);
    const char *argv[] = {
      "rg", "--no-heading", "--line-number", "--max-count=1",
      "--glob=!.git", "--glob=!builddir", "--",
      pat, lsp->workspace_root, NULL
    };
    if (lsp->rg_cancellable != NULL) {
      g_cancellable_cancel(lsp->rg_cancellable);
      g_clear_object(&lsp->rg_cancellable);
    }
    lsp->rg_cancellable = g_cancellable_new();
    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) proc = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    if (proc != NULL) {
      RequestCtx *ctx = ctx_new(lsp, CTX_HOVER, NULL);
      ctx->token = lsp->popover_token;
      ctx->buf = buf;
      ctx->line_1based = line_1based;
      ctx->col_1based = col_1based;
      ctx->prefix = g_strdup(symbol);
      ctx_set_anchor(ctx, anchor_view);
      g_subprocess_communicate_utf8_async(proc, NULL, lsp->rg_cancellable, on_legacy_hover_done, ctx);
      return;
    }
  }
  show_hover_card(lsp, anchor_view, buf, line_1based, col_1based, symbol, NULL,
                  "No language server active.", TRUE);
}

static void
on_hover_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                  gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ctx_done(ctx);
    return;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  GtkWidget *anchor = ctx->anchor;
  gboolean current = ctx->token == mgr->popover_token;
  ctx_done(ctx);
  if (!current || doc == NULL || anchor == NULL) {
    return;
  }

  guint line0 = 0, utf16 = 0;
  buffer_cursor_lsp_position(doc->buf, &line0, &utf16);
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(doc->buf));
  guint col1 = buffer_char_col_1based(tbuf, line0, utf16);
  g_autofree char *symbol = extract_word_at_cursor(doc->buf, NULL);

  g_autofree char *code = NULL;
  g_autofree char *text = NULL;
  if (error_message == NULL && result != NULL && JSON_NODE_HOLDS_OBJECT(result)) {
    JsonObject *ro = json_node_get_object(result);
    g_autofree char *md = ide_lsp_hover_to_markdown(json_object_get_member(ro, "contents"));
    split_hover_markdown(md, &code, &text);
  }
  if ((code == NULL || code[0] == '\0') && (text == NULL || text[0] == '\0')) {
    g_free(text);
    text = g_strdup(error_message != NULL ? error_message : "No information available at the cursor.");
  }
  show_hover_card(mgr, anchor, doc->buf, line0 + 1, col1, symbol, code, text, FALSE);
}

void
ide_lsp_manager_request_hover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf,
                              guint line_1based, guint col_1based)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  if (anchor_view == NULL || buf == NULL) {
    return;
  }

  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc != NULL && doc->client != NULL && doc->sent_open &&
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_STOPPED) {
    doc_send_change_now(doc);
    guint line0, utf16;
    buffer_cursor_lsp_position(buf, &line0, &utf16);

    RequestCtx *ctx = ctx_new(lsp, CTX_HOVER, doc->uri);
    ctx->token = lsp->popover_token;
    ctx_set_anchor(ctx, anchor_view);
    lsp->inflight_ctx = ctx;
    lsp->inflight_client = doc->client;
    lsp->inflight_id = 0;
    guint id = ide_lsp_client_request(doc->client, "textDocument/hover",
                                      ide_lsp_position_params(doc->uri, line0, utf16),
                                      on_hover_response, ctx);
    if (lsp->inflight_ctx == ctx) {
      lsp->inflight_id = id;
    }
    return;
  }
  legacy_hover(lsp, anchor_view, buf, line_1based, col_1based);
}

/* ─── Completion ─── */

static void
collect_buffer_identifiers(IdeLspManager *lsp, IdeBuffer *buf, const char *prefix)
{
  g_ptr_array_set_size(lsp->comp_items, 0);
  g_autofree char *text = ide_buffer_get_text(buf);
  if (text == NULL) {
    return;
  }

  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (prefix != NULL && prefix[0] != '\0') {
    g_hash_table_add(seen, g_strdup(prefix));
  }

  const char *p = text;
  while (*p != '\0' && lsp->comp_items->len < 25) {
    while (*p != '\0' && !g_ascii_isalpha(*p) && *p != '_') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    const char *start = p;
    while (*p != '\0' && (g_ascii_isalnum(*p) || *p == '_')) {
      p++;
    }
    gsize len = (gsize)(p - start);
    if (len < 3 || len > 64) {
      continue;
    }
    g_autofree char *ident = g_strndup(start, len);
    if (g_hash_table_contains(seen, ident)) {
      continue;
    }
    if (prefix != NULL && prefix[0] != '\0' && !g_str_has_prefix(ident, prefix) &&
        ide_fuzzy_score(prefix, ident) == 0) {
      continue;
    }

    g_hash_table_add(seen, g_strdup(ident));
    IdeCompletionItem *item = g_new0(IdeCompletionItem, 1);
    item->label = g_strdup(ident);
    item->insert_text = g_strdup(ident);
    item->detail = (*p == '(') ? g_strdup("function (buffer fallback)") : g_strdup("identifier (buffer fallback)");
    item->documentation = g_strdup_printf("Buffer symbol fallback (no LSP): '%s'", ident);
    g_strlcpy(item->kind_badge, (*p == '(') ? "ƒ" : "v", sizeof(item->kind_badge));
    g_ptr_array_add(lsp->comp_items, item);
  }
}

static void
update_comp_selection_ui(IdeLspManager *lsp)
{
  if (lsp->comp_list_box == NULL || lsp->comp_items->len == 0) {
    return;
  }
  lsp->selected_comp_idx = CLAMP(lsp->selected_comp_idx, 0, (gint)lsp->comp_items->len - 1);
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lsp->comp_list_box),
                                                     lsp->selected_comp_idx);
  if (row != NULL) {
    gtk_list_box_select_row(GTK_LIST_BOX(lsp->comp_list_box), row);

    if (lsp->comp_scroller != NULL) {
      GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(lsp->comp_scroller));
      graphene_rect_t b;
      if (adj != NULL && gtk_widget_compute_bounds(GTK_WIDGET(row), lsp->comp_list_box, &b)) {
        double top = b.origin.y;
        double bottom = b.origin.y + b.size.height;
        double value = gtk_adjustment_get_value(adj);
        double page = gtk_adjustment_get_page_size(adj);
        if (top < value) {
          gtk_adjustment_set_value(adj, top);
        } else if (bottom > value + page) {
          gtk_adjustment_set_value(adj, bottom - page);
        }
      }
    }
  }
  const IdeCompletionItem *it = g_ptr_array_index(lsp->comp_items, lsp->selected_comp_idx);
  if (lsp->comp_doc_label != NULL) {
    gtk_label_set_text(GTK_LABEL(lsp->comp_doc_label), it->documentation ? it->documentation : "");
  }
}

static void
present_completion_popover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  if (lsp->comp_items->len == 0) {
    return;
  }
  GtkWidget *pop = new_popover_for(lsp, anchor_view, buf);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "shell-popover-card");
  gtk_widget_set_size_request(card, 380, -1);

  lsp->comp_list_box = gtk_list_box_new();
  gtk_widget_add_css_class(lsp->comp_list_box, "ide-tree-listbox");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(lsp->comp_list_box), GTK_SELECTION_SINGLE);

  guint shown = MIN(lsp->comp_items->len, 60u);
  for (guint i = 0; i < shown; i++) {
    const IdeCompletionItem *it = g_ptr_array_index(lsp->comp_items, i);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "widget-row-card");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *badge = gtk_label_new(it->kind_badge);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    gtk_box_append(GTK_BOX(hbox), badge);

    GtkWidget *lbl = gtk_label_new(it->label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(lbl, "row-title");
    gtk_box_append(GTK_BOX(hbox), lbl);

    if (it->detail != NULL) {
      GtkWidget *det = gtk_label_new(it->detail);
      gtk_label_set_ellipsize(GTK_LABEL(det), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars(GTK_LABEL(det), 28);
      gtk_widget_set_valign(det, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(det, "row-subtitle");
      gtk_box_append(GTK_BOX(hbox), det);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(GTK_LIST_BOX(lsp->comp_list_box), row);
  }

  lsp->comp_scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lsp->comp_scroller), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(lsp->comp_scroller), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(lsp->comp_scroller), 260);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lsp->comp_scroller), lsp->comp_list_box);
  gtk_box_append(GTK_BOX(card), lsp->comp_scroller);

  lsp->comp_doc_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(lsp->comp_doc_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(lsp->comp_doc_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(lsp->comp_doc_label), 56);
  gtk_widget_set_margin_top(lsp->comp_doc_label, 6);
  gtk_widget_add_css_class(lsp->comp_doc_label, "row-subtitle");
  gtk_box_append(GTK_BOX(card), lsp->comp_doc_label);

  gtk_popover_set_child(GTK_POPOVER(pop), card);
  lsp->selected_comp_idx = 0;
  update_comp_selection_ui(lsp);
  gtk_popover_popup(GTK_POPOVER(pop));
  set_ui_state(lsp, lsp->comp_is_code_actions ? IDE_LSP_UI_CODE_ACTION : IDE_LSP_UI_COMPLETION);
}

static const char *
completion_badge(gint kind)
{
  switch (kind) {
    case 2: case 3: case 4: return "ƒ";      /* method, function, constructor */
    case 5: case 6: case 10: return "v";     /* field, variable, property */
    case 7: case 8: case 22: case 13: return "T"; /* class, interface, struct, enum */
    case 9: return "m";                      /* module */
    case 14: return "k";                     /* keyword */
    case 21: case 20: return "c";            /* constant, enum member */
    default: return "•";
  }
}

static const char *
json_str(JsonObject *o, const char *name)
{
  return (o != NULL && json_object_has_member(o, name) &&
          JSON_NODE_HOLDS_VALUE(json_object_get_member(o, name)))
             ? json_object_get_string_member(o, name)
             : NULL;
}

static IdeCompletionItem *
parse_completion_item(JsonObject *o, const char *prefix)
{
  const char *label = json_str(o, "label");
  if (label == NULL) {
    return NULL;
  }
  IdeCompletionItem *it = g_new0(IdeCompletionItem, 1);
  it->label = g_strdup(label);
  const char *ft = json_str(o, "filterText");
  it->filter_text = g_strdup((ft != NULL && ft[0] != '\0') ? ft : label);
  it->sort_text = g_strdup(json_str(o, "sortText") ? json_str(o, "sortText") : label);
  it->detail = g_strdup(json_str(o, "detail"));
  if (it->detail == NULL && json_object_has_member(o, "labelDetails")) {
    JsonObject *ld = json_object_get_object_member(o, "labelDetails");
    it->detail = g_strdup(json_str(ld, "detail") ? json_str(ld, "detail") : json_str(ld, "description"));
  }
  g_strlcpy(it->kind_badge, completion_badge((gint)json_object_get_int_member_with_default(o, "kind", 0)),
            sizeof(it->kind_badge));

  if (json_object_has_member(o, "documentation")) {
    JsonNode *dn = json_object_get_member(o, "documentation");
    if (JSON_NODE_HOLDS_VALUE(dn)) {
      it->documentation = g_strdup(json_node_get_string(dn));
    } else if (JSON_NODE_HOLDS_OBJECT(dn)) {
      it->documentation = g_strdup(json_str(json_node_get_object(dn), "value"));
    }
  }

  gboolean snippet = json_object_get_int_member_with_default(o, "insertTextFormat", 1) == 2;
  const char *new_text = json_str(o, "insertText") ? json_str(o, "insertText") : label;

  if (json_object_has_member(o, "textEdit") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "textEdit"))) {
    JsonObject *te = json_object_get_object_member(o, "textEdit");
    JsonObject *range = json_object_has_member(te, "range") ? json_object_get_object_member(te, "range")
                        : (json_object_has_member(te, "replace") ? json_object_get_object_member(te, "replace") : NULL);
    const char *nt = json_str(te, "newText") ? json_str(te, "newText") : new_text;
    g_autofree char *stripped = snippet ? ide_lsp_strip_snippet(nt) : g_strdup(nt);
    IdeLspTextEdit tmp = { 0 };
    if (json_range_to_edit(range, stripped, &tmp)) {
      it->edit = tmp;
      it->has_edit = TRUE;
      new_text = it->edit.new_text;
    }
  }
  if (it->has_edit) {
    it->insert_text = g_strdup(it->edit.new_text);
  } else {
    it->insert_text = snippet ? ide_lsp_strip_snippet(new_text) : g_strdup(new_text);
  }

  if (json_object_has_member(o, "additionalTextEdits")) {
    it->extra_edits = ide_lsp_parse_text_edits(json_object_get_member(o, "additionalTextEdits"));
    if (it->extra_edits->len == 0) {
      g_clear_pointer(&it->extra_edits, g_ptr_array_unref);
    }
  }

  if (prefix == NULL || prefix[0] == '\0') {
    it->score = 1;
  } else if (g_str_has_prefix(it->filter_text, prefix)) {
    it->score = 10000 - (gint)strlen(it->filter_text);
  } else {
    it->score = ide_fuzzy_score(prefix, it->filter_text);
  }
  if (it->score <= 0) {
    ide_completion_item_free(it);
    return NULL;
  }
  return it;
}

static gint
completion_cmp(gconstpointer a, gconstpointer b)
{
  const IdeCompletionItem *x = *(const IdeCompletionItem *const *)a;
  const IdeCompletionItem *y = *(const IdeCompletionItem *const *)b;
  if (x->score != y->score) {
    return x->score > y->score ? -1 : 1;
  }
  gint c = g_strcmp0(x->sort_text, y->sort_text);
  return c != 0 ? c : g_strcmp0(x->label, y->label);
}

static void
on_completion_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                       gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ctx_done(ctx);
    return;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  GtkWidget *anchor = ctx->anchor;
  gboolean current = ctx->token == mgr->popover_token;
  g_autofree char *prefix = g_strdup(ctx->prefix);
  ctx_done(ctx);
  if (!current || doc == NULL || anchor == NULL) {
    return;
  }

  g_ptr_array_set_size(mgr->comp_items, 0);
  JsonNode *items_node = NULL;
  if (error_message == NULL && result != NULL) {
    if (JSON_NODE_HOLDS_ARRAY(result)) {
      items_node = result;
    } else if (JSON_NODE_HOLDS_OBJECT(result) && json_object_has_member(json_node_get_object(result), "items")) {
      items_node = json_object_get_member(json_node_get_object(result), "items");
    }
  }
  if (items_node != NULL && JSON_NODE_HOLDS_ARRAY(items_node)) {
    JsonArray *arr = json_node_get_array(items_node);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
      JsonObject *o = json_array_get_object_element(arr, i);
      IdeCompletionItem *it = o != NULL ? parse_completion_item(o, prefix) : NULL;
      if (it != NULL) {
        g_ptr_array_add(mgr->comp_items, it);
      }
    }
    g_ptr_array_sort(mgr->comp_items, completion_cmp);
    if (mgr->comp_items->len > 60) {
      g_ptr_array_set_size(mgr->comp_items, 60);
    }
  }
  if (mgr->comp_items->len == 0) {
    /* Server had nothing: fall back to words from the buffer. */
    collect_buffer_identifiers(mgr, doc->buf, prefix);
  }
  present_completion_popover(mgr, anchor, doc->buf);
}

void
ide_lsp_manager_request_completion(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf,
                                   guint line_1based, guint col_1based)
{
  (void)line_1based;
  (void)col_1based;
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  if (anchor_view == NULL || buf == NULL) {
    return;
  }

  g_free(lsp->comp_prefix);
  lsp->comp_prefix = NULL;
  g_autofree char *word = extract_word_at_cursor(buf, &lsp->comp_prefix);

  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc != NULL && doc->client != NULL && doc->sent_open &&
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_STOPPED) {
    doc_send_change_now(doc);
    guint line0, utf16;
    buffer_cursor_lsp_position(buf, &line0, &utf16);

    RequestCtx *ctx = ctx_new(lsp, CTX_COMPLETION, doc->uri);
    ctx->token = lsp->popover_token;
    ctx->prefix = g_strdup(lsp->comp_prefix ? lsp->comp_prefix : "");
    ctx_set_anchor(ctx, anchor_view);
    lsp->inflight_ctx = ctx;
    lsp->inflight_client = doc->client;
    lsp->inflight_id = 0;

    JsonNode *params = ide_lsp_position_params(doc->uri, line0, utf16);
    /* completion context: invoked explicitly by the user */
    JsonObject *po = json_node_get_object(params);
    JsonObject *cctx = json_object_new();
    json_object_set_int_member(cctx, "triggerKind", 1);
    json_object_set_object_member(po, "context", cctx);

    guint id = ide_lsp_client_request(doc->client, "textDocument/completion", params,
                                      on_completion_response, ctx);
    if (lsp->inflight_ctx == ctx) {
      lsp->inflight_id = id;
    }
    return;
  }

  collect_buffer_identifiers(lsp, buf, lsp->comp_prefix);
  present_completion_popover(lsp, anchor_view, buf);
}

void
ide_lsp_manager_select_next(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  lsp->selected_comp_idx++;
  update_comp_selection_ui(lsp);
}

void
ide_lsp_manager_select_prev(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  lsp->selected_comp_idx--;
  update_comp_selection_ui(lsp);
}

gboolean
ide_lsp_manager_select_index(IdeLspManager *lsp, int idx)
{
  g_return_val_if_fail(lsp != NULL, FALSE);
  if (lsp->comp_items == NULL || idx < 0 || (guint)idx >= lsp->comp_items->len) {
    return FALSE;
  }
  lsp->selected_comp_idx = idx;
  update_comp_selection_ui(lsp);
  return TRUE;
}

static void run_code_action(IdeLspManager *lsp, IdeBuffer *buf, guint index);

void
ide_lsp_manager_confirm_selection(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  if (lsp->comp_is_code_actions) {
    gint idx = lsp->selected_comp_idx;
    ide_lsp_manager_cancel_popover(lsp);
    if (buf != NULL && idx >= 0) {
      run_code_action(lsp, buf, (guint)idx);
    }
    return;
  }
  if (buf != NULL && lsp->selected_comp_idx >= 0 &&
      (guint)lsp->selected_comp_idx < lsp->comp_items->len) {
    const IdeCompletionItem *it = g_ptr_array_index(lsp->comp_items, lsp->selected_comp_idx);
    g_autoptr(GPtrArray) edits = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);

    if (it->has_edit) {
      g_ptr_array_add(edits, ide_lsp_text_edit_new(it->edit.line_start, it->edit.char_start,
                                                   it->edit.line_end, it->edit.char_end,
                                                   it->edit.new_text));
    } else {
      /* Replace the typed prefix with the candidate. */
      guint line0, utf16;
      buffer_cursor_lsp_position(buf, &line0, &utf16);
      guint prefix_units = 0;
      for (const char *p = lsp->comp_prefix ? lsp->comp_prefix : ""; *p; p = g_utf8_next_char(p)) {
        prefix_units += (g_utf8_get_char(p) >= 0x10000) ? 2 : 1;
      }
      prefix_units = MIN(prefix_units, utf16);
      g_ptr_array_add(edits, ide_lsp_text_edit_new(line0, utf16 - prefix_units, line0, utf16,
                                                   it->insert_text ? it->insert_text : it->label));
    }
    if (it->extra_edits != NULL) {
      for (guint i = 0; i < it->extra_edits->len; i++) {
        const IdeLspTextEdit *x = g_ptr_array_index(it->extra_edits, i);
        g_ptr_array_add(edits, ide_lsp_text_edit_new(x->line_start, x->char_start, x->line_end,
                                                     x->char_end, x->new_text));
      }
    }
    ide_lsp_manager_cancel_popover(lsp); /* drops lsp->comp_items' widgets, not the items */
    apply_edits_to_buffer(lsp, buf, edits);
    return;
  }
  ide_lsp_manager_cancel_popover(lsp);
}

/* ─── Definition ─── */

static void
on_legacy_definition_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GSubprocess *proc = G_SUBPROCESS(source);
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  g_autofree char *out = NULL;
  g_autoptr(GError) err = NULL;
  g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, &err);

  if (mgr->shutting_down || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    ctx_done(ctx);
    return;
  }
  ctx_done(ctx);

  if (out != NULL && out[0] != '\0') {
    g_auto(GStrv) parts = g_strsplit(out, ":", 4);
    if (g_strv_length(parts) >= 3 && mgr->jump_cb != NULL) {
      guint ln = (guint)MAX(1, g_ascii_strtoll(parts[1], NULL, 10));
      guint cl = (guint)MAX(1, g_ascii_strtoll(parts[2], NULL, 10));
      report_message(mgr, "Jumped via rg fallback (no language server)");
      mgr->jump_cb(parts[0], ln, cl, mgr->cb_data);
      return;
    }
  }
  report_message(mgr, "No definition found (rg fallback — no language server)");
}

static void
legacy_definition(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_autofree char *symbol = extract_word_at_cursor(buf, NULL);
  if (symbol == NULL || symbol[0] == '\0') {
    return;
  }

  g_autofree char *pat = g_strdup_printf("(?:^|\\s)(?:func\\s+|def\\s+|struct\\s+|#define\\s+)?%s\\s*[\\(\\{=]",
                                         symbol);
  const char *argv[] = {
    "rg", "--vimgrep", "--no-heading", "--max-count=1",
    "--glob=!.git", "--glob=!builddir", "--", pat,
    lsp->workspace_root ? lsp->workspace_root : ".", NULL
  };
  if (lsp->rg_cancellable != NULL) {
    g_cancellable_cancel(lsp->rg_cancellable);
    g_clear_object(&lsp->rg_cancellable);
  }
  lsp->rg_cancellable = g_cancellable_new();
  g_autoptr(GError) err = NULL;
  g_autoptr(GSubprocess) proc = g_subprocess_newv(
      argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
  if (proc != NULL) {
    RequestCtx *ctx = ctx_new(lsp, CTX_DEFINITION, NULL);
    g_subprocess_communicate_utf8_async(proc, NULL, lsp->rg_cancellable, on_legacy_definition_done, ctx);
  }
}

static void
on_definition_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                       gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ctx_done(ctx);
    return;
  }
  ctx_done(ctx);

  if (error_message != NULL) {
    report_message(mgr, "Go to definition failed: %s", error_message);
    return;
  }

  JsonObject *loc = NULL;
  guint count = 0;
  if (result != NULL && JSON_NODE_HOLDS_ARRAY(result)) {
    JsonArray *arr = json_node_get_array(result);
    count = json_array_get_length(arr);
    if (count > 0) {
      loc = json_array_get_object_element(arr, 0);
    }
  } else if (result != NULL && JSON_NODE_HOLDS_OBJECT(result)) {
    loc = json_node_get_object(result);
    count = 1;
  }
  if (loc == NULL) {
    report_message(mgr, "No definition found");
    return;
  }

  const char *uri = json_str(loc, "targetUri") ? json_str(loc, "targetUri") : json_str(loc, "uri");
  JsonObject *range = json_object_has_member(loc, "targetSelectionRange")
                          ? json_object_get_object_member(loc, "targetSelectionRange")
                      : json_object_has_member(loc, "range") ? json_object_get_object_member(loc, "range")
                      : (json_object_has_member(loc, "targetRange") ? json_object_get_object_member(loc, "targetRange") : NULL);
  if (uri == NULL || range == NULL) {
    report_message(mgr, "No definition found");
    return;
  }
  g_autofree char *path = g_filename_from_uri(uri, NULL, NULL);
  if (path == NULL) {
    report_message(mgr, "Definition is outside the workspace (%s)", uri);
    return;
  }
  JsonObject *start = json_object_get_object_member(range, "start");
  guint line0 = (guint)json_object_get_int_member_with_default(start, "line", 0);
  guint ch = (guint)json_object_get_int_member_with_default(start, "character", 0);

  guint line1 = line0 + 1, col1 = ch + 1;
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, uri);
  if (doc != NULL) {
    GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(doc->buf));
    col1 = buffer_char_col_1based(tbuf, line0, ch);
  } else {
    file_lsp_to_line_col(path, line0, ch, &line1, &col1);
  }
  if (count > 1) {
    report_message(mgr, "%u definitions found, jumping to the first", count);
  }
  if (mgr->jump_cb != NULL) {
    mgr->jump_cb(path, line1, col1, mgr->cb_data);
  }
}

static gboolean
request_location(IdeLspManager *lsp, IdeBuffer *buf, const char *method)
{
  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc == NULL || doc->client == NULL || !doc->sent_open ||
      ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_STOPPED) {
    return FALSE;
  }
  doc_send_change_now(doc);
  guint line0, utf16;
  buffer_cursor_lsp_position(buf, &line0, &utf16);
  RequestCtx *ctx = ctx_new(lsp, CTX_DEFINITION, doc->uri);
  ide_lsp_client_request(doc->client, method, ide_lsp_position_params(doc->uri, line0, utf16),
                         on_definition_response, ctx);
  return TRUE;
}

void
ide_lsp_manager_request_definition(IdeLspManager *lsp, IdeBuffer *buf, guint line_1based,
                                   guint col_1based)
{
  (void)line_1based;
  (void)col_1based;
  g_return_if_fail(lsp != NULL);
  if (buf == NULL || lsp->jump_cb == NULL) {
    return;
  }
  if (!request_location(lsp, buf, "textDocument/definition")) {
    legacy_definition(lsp, buf);
  }
}

void
ide_lsp_manager_request_implementation(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  if (buf == NULL || lsp->jump_cb == NULL) {
    return;
  }
  if (!request_location(lsp, buf, "textDocument/implementation")) {
    report_message(lsp, "Go to implementation needs a running language server");
  }
}

/* ─── Rename ─── */

static void
legacy_rename(IdeLspManager *lsp, IdeBuffer *buf, const char *new_name)
{
  g_autofree char *old_sym = extract_word_at_cursor(buf, NULL);
  if (old_sym == NULL || old_sym[0] == '\0') {
    return;
  }
  g_autofree char *orig = ide_buffer_get_text(buf);
  guint cnt = 0;
  g_autofree char *replaced = ide_replace_text_all(orig, old_sym, new_name, TRUE, TRUE, FALSE, &cnt);
  if (cnt > 0) {
    ide_buffer_set_text(buf, replaced);
    report_message(lsp, "Renamed '%s' to '%s': %u buffer edit%s (buffer fallback — no LSP)",
                   old_sym, new_name, cnt, cnt == 1 ? "" : "s");
  }
}

static void
on_rename_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                   gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ctx_done(ctx);
    return;
  }
  g_autofree char *new_name = g_strdup(ctx->new_name);
  ctx_done(ctx);

  if (error_message != NULL) {
    report_message(mgr, "Rename failed: %s", error_message);
    return;
  }
  guint edits = 0, files = 0;
  apply_workspace_edit(mgr, result, &edits, &files);
  if (edits == 0) {
    report_message(mgr, "Rename: nothing to change");
  } else {
    report_message(mgr, "Renamed to '%s': %u edit%s in %u file%s", new_name, edits,
                   edits == 1 ? "" : "s", files, files == 1 ? "" : "s");
  }
}

void
ide_lsp_manager_request_rename(IdeLspManager *lsp, IdeBuffer *buf, guint line_1based,
                               guint col_1based, const char *new_name)
{
  (void)line_1based;
  (void)col_1based;
  g_return_if_fail(lsp != NULL);
  if (buf == NULL || new_name == NULL || new_name[0] == '\0') {
    return;
  }

  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc != NULL && doc->client != NULL && doc->sent_open &&
      ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_READY &&
      ide_lsp_client_has_capability(doc->client, "renameProvider")) {
    doc_send_change_now(doc);
    guint line0, utf16;
    buffer_cursor_lsp_position(buf, &line0, &utf16);
    RequestCtx *ctx = ctx_new(lsp, CTX_RENAME, doc->uri);
    ctx->new_name = g_strdup(new_name);
    JsonNode *params = ide_lsp_position_params(doc->uri, line0, utf16);
    json_object_set_string_member(json_node_get_object(params), "newName", new_name);
    ide_lsp_client_request(doc->client, "textDocument/rename", params, on_rename_response, ctx);
    return;
  }
  legacy_rename(lsp, buf, new_name);
}

/* ─── Formatting ─── */

static void
on_format_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                   gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ctx_done(ctx);
    return;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  ctx_done(ctx);
  if (error_message != NULL) {
    report_message(mgr, "Format failed: %s", error_message);
    return;
  }
  if (doc == NULL) {
    return;
  }
  g_autoptr(GPtrArray) edits = ide_lsp_parse_text_edits(result);
  if (edits->len == 0) {
    report_message(mgr, "Already formatted");
    return;
  }
  apply_edits_to_buffer(mgr, doc->buf, edits);
  report_message(mgr, "Formatted with %s", doc->spec ? doc->spec->server_id : "language server");
}

void
ide_lsp_manager_request_format_with_options(IdeLspManager *lsp,
                                            IdeBuffer     *buf,
                                            guint          tab_size,
                                            gboolean       insert_spaces)
{
  g_return_if_fail(lsp != NULL);
  if (buf == NULL) {
    return;
  }
  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc == NULL || doc->client == NULL ||
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_READY ||
      !ide_lsp_client_has_capability(doc->client, "documentFormattingProvider")) {
    report_message(lsp, "No language server with formatting support for this file");
    return;
  }
  doc_send_change_now(doc);

  guint effective_tab = (tab_size > 0) ? tab_size : 4;
  gboolean effective_spaces = (tab_size > 0)
                                  ? insert_spaces
                                  : (g_strcmp0(doc->language_id, "go") != 0);

  g_autoptr(JsonNode) base = text_document_params(doc->uri);
  JsonNode *params = json_node_copy(base);
  JsonObject *po = json_node_get_object(params);
  JsonObject *opts = json_object_new();
  json_object_set_int_member(opts, "tabSize", (gint64)effective_tab);
  json_object_set_boolean_member(opts, "insertSpaces", effective_spaces);
  json_object_set_object_member(po, "options", opts);

  RequestCtx *ctx = ctx_new(lsp, CTX_FORMAT, doc->uri);
  ide_lsp_client_request(doc->client, "textDocument/formatting", params, on_format_response, ctx);
}

void
ide_lsp_manager_request_format(IdeLspManager *lsp, IdeBuffer *buf)
{
  ide_lsp_manager_request_format_with_options(lsp, buf, 0, TRUE);
}

/* ─── Code actions ─── */

static void
execute_command(IdeLspManager *lsp, DocState *doc, JsonObject *cmd)
{
  if (doc == NULL || doc->client == NULL || !json_object_has_member(cmd, "command")) {
    return;
  }
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "command");
  json_builder_add_string_value(b, json_object_get_string_member(cmd, "command"));
  if (json_object_has_member(cmd, "arguments")) {
    json_builder_set_member_name(b, "arguments");
    json_builder_add_value(b, json_node_copy(json_object_get_member(cmd, "arguments")));
  }
  json_builder_end_object(b);
  (void)lsp;
  ide_lsp_client_request(doc->client, "workspace/executeCommand", json_builder_get_root(b), NULL, NULL);
}

static void
apply_code_action_node(IdeLspManager *lsp, DocState *doc, JsonNode *action)
{
  if (!JSON_NODE_HOLDS_OBJECT(action)) {
    return;
  }
  JsonObject *ao = json_node_get_object(action);
  if (json_object_has_member(ao, "edit")) {
    guint edits = 0, files = 0;
    apply_workspace_edit(lsp, json_object_get_member(ao, "edit"), &edits, &files);
    report_message(lsp, "%s: %u edit%s in %u file%s", json_str(ao, "title") ? json_str(ao, "title") : "Code action",
                   edits, edits == 1 ? "" : "s", files, files == 1 ? "" : "s");
  }
  if (json_object_has_member(ao, "command")) {
    JsonNode *c = json_object_get_member(ao, "command");
    if (JSON_NODE_HOLDS_OBJECT(c)) {
      execute_command(lsp, doc, json_node_get_object(c));          /* CodeAction.command */
    } else if (JSON_NODE_HOLDS_VALUE(c)) {
      execute_command(lsp, doc, ao);                               /* bare Command */
    }
  }
}

static void
on_code_action_resolved(IdeLspClient *client, JsonNode *result, const char *error_message,
                        gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ctx_done(ctx);
    return;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  ctx_done(ctx);
  if (error_message != NULL || result == NULL) {
    report_message(mgr, "Code action failed: %s", error_message ? error_message : "no result");
    return;
  }
  apply_code_action_node(mgr, doc, result);
}

static void
run_code_action(IdeLspManager *lsp, IdeBuffer *buf, guint index)
{
  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc == NULL || index >= lsp->code_actions->len) {
    return;
  }
  JsonNode *action = g_ptr_array_index(lsp->code_actions, index);
  JsonObject *ao = JSON_NODE_HOLDS_OBJECT(action) ? json_node_get_object(action) : NULL;
  if (ao == NULL) {
    return;
  }
  /* Lazily-filled actions carry only `data`: ask the server to resolve them. */
  if (!json_object_has_member(ao, "edit") && !json_object_has_member(ao, "command") &&
      doc->client != NULL && ide_lsp_client_has_capability(doc->client, "codeActionProvider.resolveProvider")) {
    RequestCtx *ctx = ctx_new(lsp, CTX_FORMAT, doc->uri);
    ide_lsp_client_request(doc->client, "codeAction/resolve", json_node_copy(action),
                           on_code_action_resolved, ctx);
    return;
  }
  apply_code_action_node(lsp, doc, action);
}

static void
on_code_actions_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                         gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ctx_done(ctx);
    return;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  GtkWidget *anchor = ctx->anchor;
  gboolean current = ctx->token == mgr->popover_token;
  ctx_done(ctx);
  if (!current || doc == NULL || anchor == NULL) {
    return;
  }
  if (error_message != NULL) {
    report_message(mgr, "Code actions failed: %s", error_message);
    return;
  }

  g_ptr_array_set_size(mgr->comp_items, 0);
  g_ptr_array_set_size(mgr->code_actions, 0);
  if (result != NULL && JSON_NODE_HOLDS_ARRAY(result)) {
    JsonArray *arr = json_node_get_array(result);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
      JsonObject *o = json_array_get_object_element(arr, i);
      if (o == NULL || json_str(o, "title") == NULL) {
        continue;
      }
      if (json_object_get_boolean_member_with_default(o, "disabled", FALSE)) {
        continue;
      }
      IdeCompletionItem *it = g_new0(IdeCompletionItem, 1);
      it->label = g_strdup(json_str(o, "title"));
      it->detail = g_strdup(json_str(o, "kind"));
      it->insert_text = g_strdup(it->label);
      if (mgr->comp_items->len < 9) {
        g_snprintf(it->kind_badge, sizeof(it->kind_badge), "%u", mgr->comp_items->len + 1);
      } else {
        g_strlcpy(it->kind_badge, json_object_get_boolean_member_with_default(o, "isPreferred", FALSE) ? "★" : "⚡",
                  sizeof(it->kind_badge));
      }
      g_ptr_array_add(mgr->comp_items, it);
      g_ptr_array_add(mgr->code_actions, json_node_copy(json_array_get_element(arr, i)));
    }
  }
  if (mgr->comp_items->len == 0) {
    report_message(mgr, "No code actions available here");
    return;
  }
  mgr->comp_is_code_actions = TRUE;
  present_completion_popover(mgr, anchor, doc->buf);
}

void
ide_lsp_manager_request_code_actions(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  DocState *doc = buf != NULL ? g_hash_table_lookup(lsp->docs, buf) : NULL;
  if (doc == NULL || doc->client == NULL ||
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_READY ||
      !ide_lsp_client_has_capability(doc->client, "codeActionProvider")) {
    report_message(lsp, "Code actions need a running language server that supports them");
    return;
  }
  doc_send_change_now(doc);

  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter a, b;
  if (!gtk_text_buffer_get_selection_bounds(tbuf, &a, &b)) {
    gtk_text_buffer_get_iter_at_mark(tbuf, &a, gtk_text_buffer_get_insert(tbuf));
    b = a;
  }
  guint l0 = (guint)gtk_text_iter_get_line(&a);
  g_autofree char *line_a = buffer_line_text(tbuf, l0);
  guint c0 = ide_utf8_byte_to_utf16_col(line_a, (gsize)gtk_text_iter_get_line_index(&a));
  guint l1 = (guint)gtk_text_iter_get_line(&b);
  g_autofree char *line_b = buffer_line_text(tbuf, l1);
  guint c1 = ide_utf8_byte_to_utf16_col(line_b, (gsize)gtk_text_iter_get_line_index(&b));

  g_autoptr(JsonBuilder) bld = json_builder_new();
  json_builder_begin_object(bld);
  json_builder_set_member_name(bld, "textDocument");
  json_builder_begin_object(bld);
  json_builder_set_member_name(bld, "uri");
  json_builder_add_string_value(bld, doc->uri);
  json_builder_end_object(bld);
  json_builder_set_member_name(bld, "range");
  json_builder_begin_object(bld);
  json_builder_set_member_name(bld, "start");
  json_builder_begin_object(bld);
  json_builder_set_member_name(bld, "line");
  json_builder_add_int_value(bld, l0);
  json_builder_set_member_name(bld, "character");
  json_builder_add_int_value(bld, c0);
  json_builder_end_object(bld);
  json_builder_set_member_name(bld, "end");
  json_builder_begin_object(bld);
  json_builder_set_member_name(bld, "line");
  json_builder_add_int_value(bld, l1);
  json_builder_set_member_name(bld, "character");
  json_builder_add_int_value(bld, c1);
  json_builder_end_object(bld);
  json_builder_end_object(bld);
  json_builder_set_member_name(bld, "context");
  json_builder_begin_object(bld);
  json_builder_set_member_name(bld, "diagnostics");
  json_builder_begin_array(bld);
  /* Diagnostics touching the requested lines enable quick fixes. */
  if (doc->raw_diagnostics != NULL && JSON_NODE_HOLDS_ARRAY(doc->raw_diagnostics)) {
    JsonArray *darr = json_node_get_array(doc->raw_diagnostics);
    for (guint i = 0; i < json_array_get_length(darr); i++) {
      JsonObject *d = json_array_get_object_element(darr, i);
      if (d == NULL || !json_object_has_member(d, "range")) {
        continue;
      }
      JsonObject *r = json_object_get_object_member(d, "range");
      gint64 ds = json_object_get_int_member_with_default(json_object_get_object_member(r, "start"), "line", 0);
      gint64 de = json_object_get_int_member_with_default(json_object_get_object_member(r, "end"), "line", 0);
      if ((gint64)l1 >= ds && (gint64)l0 <= de) {
        json_builder_add_value(bld, json_node_copy(json_array_get_element(darr, i)));
      }
    }
  }
  json_builder_end_array(bld);
  json_builder_end_object(bld);
  json_builder_end_object(bld);

  RequestCtx *ctx = ctx_new(lsp, CTX_COMPLETION, doc->uri);
  ctx->token = lsp->popover_token;
  ctx_set_anchor(ctx, anchor_view);
  lsp->inflight_ctx = ctx;
  lsp->inflight_client = doc->client;
  lsp->inflight_id = 0;
  guint id = ide_lsp_client_request(doc->client, "textDocument/codeAction", json_builder_get_root(bld),
                                    on_code_actions_response, ctx);
  if (lsp->inflight_ctx == ctx) {
    lsp->inflight_id = id;
  }
}

/* ─── Rename prompt ─── */

static void
refresh_rename_label(IdeLspManager *lsp)
{
  if (lsp->rename_label == NULL) {
    return;
  }
  g_autofree char *txt = g_strdup_printf("%s  →  %s▏", lsp->rename_old ? lsp->rename_old : "",
                                         lsp->rename_input ? lsp->rename_input->str : "");
  gtk_label_set_text(GTK_LABEL(lsp->rename_label), txt);
}

void
ide_lsp_manager_rename_begin(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  if (anchor_view == NULL || buf == NULL) {
    return;
  }
  g_autofree char *word = extract_word_at_cursor(buf, NULL);
  if (word == NULL || word[0] == '\0') {
    report_message(lsp, "Place the cursor on a symbol to rename it");
    return;
  }
  g_free(lsp->rename_old);
  lsp->rename_old = g_steal_pointer(&word);
  lsp->rename_input = g_string_new(lsp->rename_old);

  GtkWidget *pop = new_popover_for(lsp, anchor_view, buf);
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(card, "shell-popover-card");
  gtk_widget_set_size_request(card, 360, -1);

  GtkWidget *hdr = gtk_label_new("RENAME SYMBOL");
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
  gtk_widget_add_css_class(hdr, "shell-popover-title");
  gtk_box_append(GTK_BOX(card), hdr);

  lsp->rename_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(lsp->rename_label), 0.0f);
  gtk_widget_add_css_class(lsp->rename_label, "code-preview-box");
  gtk_box_append(GTK_BOX(card), lsp->rename_label);

  GtkWidget *hint = gtk_label_new("[Enter: Rename]   [Esc: Cancel]");
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_widget_add_css_class(hint, "shell-popover-badge");
  gtk_box_append(GTK_BOX(card), hint);

  gtk_popover_set_child(GTK_POPOVER(pop), card);
  refresh_rename_label(lsp);
  gtk_popover_popup(GTK_POPOVER(pop));
  set_ui_state(lsp, IDE_LSP_UI_RENAME);
}

gboolean
ide_lsp_manager_rename_active(IdeLspManager *lsp)
{
  return lsp != NULL && lsp->rename_input != NULL && lsp->ui_state == IDE_LSP_UI_RENAME;
}

void
ide_lsp_manager_rename_input(IdeLspManager *lsp, const char *text)
{
  if (!ide_lsp_manager_rename_active(lsp) || text == NULL) {
    return;
  }
  g_string_append(lsp->rename_input, text);
  refresh_rename_label(lsp);
}

void
ide_lsp_manager_rename_backspace(IdeLspManager *lsp)
{
  if (!ide_lsp_manager_rename_active(lsp) || lsp->rename_input->len == 0) {
    return;
  }
  const char *s = lsp->rename_input->str;
  const char *last = g_utf8_prev_char(s + lsp->rename_input->len);
  g_string_truncate(lsp->rename_input, (gsize)(last - s));
  refresh_rename_label(lsp);
}

void
ide_lsp_manager_rename_confirm(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  g_autofree char *name = lsp->rename_input != NULL ? g_strdup(lsp->rename_input->str) : NULL;
  g_autofree char *old = g_strdup(lsp->rename_old);
  ide_lsp_manager_cancel_popover(lsp);
  if (name == NULL || name[0] == '\0' || g_strcmp0(name, old) == 0 || buf == NULL) {
    return;
  }
  ide_lsp_manager_request_rename(lsp, buf, 0, 0, name);
}

/* ─── Auto-trigger ─── */

gboolean
ide_lsp_manager_completion_active(IdeLspManager *lsp)
{
  return lsp != NULL && lsp->ui_state == IDE_LSP_UI_COMPLETION;
}

gboolean
ide_lsp_manager_maybe_trigger_completion(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf,
                                         const char *typed)
{
  g_return_val_if_fail(lsp != NULL, FALSE);
  if (buf == NULL || typed == NULL || typed[0] == '\0') {
    return FALSE;
  }
  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc == NULL || doc->client == NULL ||
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_READY) {
    return FALSE;
  }
  JsonNode *trig = ide_lsp_client_get_capability(doc->client, "completionProvider.triggerCharacters");
  if (trig == NULL || !JSON_NODE_HOLDS_ARRAY(trig)) {
    return FALSE;
  }
  JsonArray *arr = json_node_get_array(trig);
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    if (g_strcmp0(json_array_get_string_element(arr, i), typed) == 0) {
      ide_lsp_manager_request_completion(lsp, anchor_view, buf, 0, 0);
      return TRUE;
    }
  }
  return FALSE;
}

/* ─── workspace/executeCommand ─── */

static void
on_command_response(IdeLspClient *client, JsonNode *result, const char *error_message, gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspCommandCb cb = ctx->command_cb;
  gpointer ud = ctx->command_data;
  gboolean quiet = ctx->mgr->shutting_down;
  ctx_done(ctx);
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

  RequestCtx *ctx = ctx_new(lsp, CTX_COMMAND, doc->uri);
  ctx->command_cb = cb;
  ctx->command_data = user_data;
  ide_lsp_client_request(doc->client, "workspace/executeCommand", json_builder_get_root(b),
                         on_command_response, ctx);
  return TRUE;
}
