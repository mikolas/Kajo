#include "ide_lsp_private.h"

#include <string.h>

/* ═══════════════════════════ Buffer & workspace edits ═══════════════════════════ */

void
ide_lsp_apply_edits_to_buffer(IdeLspManager *mgr, IdeBuffer *buf, const GPtrArray *edits)
{
  if (edits == NULL || edits->len == 0) {
    return;
  }
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));

  g_autoptr(GPtrArray) sorted = g_ptr_array_new();
  for (guint i = 0; i < edits->len; i++) {
    g_ptr_array_add(sorted, g_ptr_array_index((GPtrArray *)edits, i));
  }
  g_ptr_array_sort(sorted, ide_lsp_edit_cmp);

  /* Descending order keeps earlier positions valid while editing. */
  gtk_text_buffer_begin_user_action(tbuf);
  for (guint i = sorted->len; i > 0; i--) {
    const IdeLspTextEdit *e = g_ptr_array_index(sorted, i - 1);
    GtkTextIter s, en;
    ide_lsp_buffer_iter_at_pos(tbuf, e->line_start, e->char_start, &s);
    ide_lsp_buffer_iter_at_pos(tbuf, e->line_end, e->char_end, &en);
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
    ide_lsp_apply_edits_to_buffer(mgr, doc->buf, edits);
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

void
ide_lsp_apply_workspace_edit(IdeLspManager *mgr, JsonNode *edit, guint *edits_out, guint *files_out)
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

/* ═══════════════════════════ Ripgrep & buffer fallbacks ═══════════════════════════ */

static gboolean
spawn_rg_fallback(IdeLspManager       *lsp,
                  const char          *src_path,
                  const char          *symbol,
                  gboolean             for_definition,
                  GAsyncReadyCallback  cb,
                  RequestCtx          *ctx)
{
  g_autofree char *pat = ide_lsp_fallback_pattern(src_path, symbol, for_definition);
  g_autoptr(GPtrArray) av = g_ptr_array_new();
  g_ptr_array_add(av, (gpointer)"rg");
  if (for_definition) {
    g_ptr_array_add(av, (gpointer)"--vimgrep");
  }
  g_ptr_array_add(av, (gpointer)"--no-heading");
  if (!for_definition) {
    g_ptr_array_add(av, (gpointer)"--line-number");
  }
  g_ptr_array_add(av, (gpointer)"--max-count=1");
  g_ptr_array_add(av, (gpointer)"--glob=!.git");
  g_ptr_array_add(av, (gpointer)"--glob=!builddir");
  for (const char *const *g = ide_lsp_fallback_globs(src_path); *g != NULL; g++) {
    g_ptr_array_add(av, (gpointer)*g);
  }
  g_ptr_array_add(av, (gpointer)"--");
  g_ptr_array_add(av, pat);
  g_ptr_array_add(av, (gpointer)(lsp->workspace_root ? lsp->workspace_root : "."));
  g_ptr_array_add(av, NULL);

  if (lsp->rg_cancellable != NULL) {
    g_cancellable_cancel(lsp->rg_cancellable);
    g_clear_object(&lsp->rg_cancellable);
  }
  lsp->rg_cancellable = g_cancellable_new();

  g_autoptr(GError) err = NULL;
  g_autoptr(GSubprocess) proc = g_subprocess_newv(
      (const char *const *)av->pdata,
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      &err);
  if (proc == NULL) {
    return FALSE;
  }
  g_subprocess_communicate_utf8_async(proc, NULL, lsp->rg_cancellable, cb, ctx);
  return TRUE;
}

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
    ide_lsp_ctx_done(ctx);
    return;
  }
  GtkWidget *anchor = ctx->anchor;
  IdeBuffer *buf = ctx->buf;
  guint line_1based = ctx->line_1based;
  guint col_1based = ctx->col_1based;
  g_autofree char *symbol = g_strdup(ctx->prefix);
  gboolean current = (ctx->token == mgr->popover_token) &&
                     (g_hash_table_contains(mgr->docs, buf));
  ide_lsp_ctx_done(ctx);
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
  ide_lsp_show_hover_card(mgr, anchor, buf, line_1based, col_1based, symbol, decl_line, fallback_note, TRUE);
}

void
ide_lsp_legacy_hover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf, guint line_1based,
                     guint col_1based)
{
  g_autofree char *symbol = ide_lsp_extract_word_at_cursor(buf, NULL);
  if (symbol != NULL && symbol[0] != '\0' && lsp->workspace_root != NULL) {
    RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_HOVER, NULL);
    ctx->token = lsp->popover_token;
    ctx->buf = buf;
    ctx->line_1based = line_1based;
    ctx->col_1based = col_1based;
    ctx->prefix = g_strdup(symbol);
    ide_lsp_ctx_set_anchor(ctx, anchor_view);
    if (spawn_rg_fallback(lsp, ide_buffer_get_file_path(buf), symbol, FALSE, on_legacy_hover_done, ctx)) {
      return;
    }
    ide_lsp_ctx_done(ctx);
  }
  ide_lsp_show_hover_card(lsp, anchor_view, buf, line_1based, col_1based, symbol, NULL,
                          "No language server active.", TRUE);
}

void
ide_lsp_collect_buffer_identifiers(IdeLspManager *lsp, IdeBuffer *buf, const char *prefix)
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

/* ═══════════════════════════ Definition & implementation ═══════════════════════════ */

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
    ide_lsp_ctx_done(ctx);
    return;
  }
  ide_lsp_ctx_done(ctx);

  if (out != NULL && out[0] != '\0') {
    g_auto(GStrv) parts = g_strsplit(out, ":", 4);
    if (g_strv_length(parts) >= 3 && mgr->jump_cb != NULL) {
      guint ln = (guint)MAX(1, g_ascii_strtoll(parts[1], NULL, 10));
      guint cl = (guint)MAX(1, g_ascii_strtoll(parts[2], NULL, 10));
      ide_lsp_report_message(mgr, "Jumped via rg fallback (no language server)");
      mgr->jump_cb(parts[0], ln, cl, mgr->cb_data);
      return;
    }
  }
  ide_lsp_report_message(mgr, "No definition found (rg fallback — no language server)");
}

static void
legacy_definition(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_autofree char *symbol = ide_lsp_extract_word_at_cursor(buf, NULL);
  if (symbol == NULL || symbol[0] == '\0') {
    return;
  }
  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_DEFINITION, NULL);
  if (!spawn_rg_fallback(lsp, ide_buffer_get_file_path(buf), symbol, TRUE, on_legacy_definition_done, ctx)) {
    ide_lsp_ctx_done(ctx);
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
    ide_lsp_ctx_done(ctx);
    return;
  }
  ide_lsp_ctx_done(ctx);

  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Go to definition failed: %s", error_message);
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
    ide_lsp_report_message(mgr, "No definition found");
    return;
  }

  const char *t_uri = ide_lsp_json_str(loc, "targetUri");
  const char *uri = t_uri ? t_uri : ide_lsp_json_str(loc, "uri");
  JsonObject *range = json_object_has_member(loc, "targetSelectionRange")
                          ? json_object_get_object_member(loc, "targetSelectionRange")
                      : json_object_has_member(loc, "range") ? json_object_get_object_member(loc, "range")
                      : (json_object_has_member(loc, "targetRange") ? json_object_get_object_member(loc, "targetRange") : NULL);
  if (uri == NULL || range == NULL) {
    ide_lsp_report_message(mgr, "No definition found");
    return;
  }
  g_autofree char *path = g_filename_from_uri(uri, NULL, NULL);
  if (path == NULL) {
    ide_lsp_report_message(mgr, "Definition is outside the workspace (%s)", uri);
    return;
  }
  JsonObject *start = json_object_get_object_member(range, "start");
  guint line0 = (guint)json_object_get_int_member_with_default(start, "line", 0);
  guint ch = (guint)json_object_get_int_member_with_default(start, "character", 0);

  guint line1 = line0 + 1, col1 = ch + 1;
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, uri);
  if (doc != NULL) {
    GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(doc->buf));
    col1 = ide_lsp_buffer_char_col_1based(tbuf, line0, ch);
  } else {
    ide_lsp_file_to_line_col(path, line0, ch, &line1, &col1);
  }
  if (count > 1) {
    ide_lsp_report_message(mgr, "%u definitions found, jumping to the first", count);
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
  ide_lsp_doc_send_change_now(doc);
  guint line0, utf16;
  ide_lsp_buffer_cursor_position(buf, &line0, &utf16);
  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_DEFINITION, doc->uri);
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
    ide_lsp_report_message(lsp, "Go to implementation needs a running language server");
  }
}

/* ═══════════════════════════ Rename & prompt ═══════════════════════════ */

static void
legacy_rename(IdeLspManager *lsp, IdeBuffer *buf, const char *new_name)
{
  g_autofree char *old_sym = ide_lsp_extract_word_at_cursor(buf, NULL);
  if (old_sym == NULL || old_sym[0] == '\0') {
    return;
  }
  g_autofree char *orig = ide_buffer_get_text(buf);
  guint cnt = 0;
  g_autofree char *replaced = ide_replace_text_all(orig, old_sym, new_name, TRUE, TRUE, FALSE, &cnt);
  if (cnt > 0) {
    ide_buffer_set_text(buf, replaced);
    ide_lsp_report_message(lsp, "Renamed '%s' to '%s': %u buffer edit%s (buffer fallback — no LSP)",
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
    ide_lsp_ctx_done(ctx);
    return;
  }
  g_autofree char *new_name = g_strdup(ctx->new_name);
  ide_lsp_ctx_done(ctx);

  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Rename failed: %s", error_message);
    return;
  }
  guint edits = 0, files = 0;
  ide_lsp_apply_workspace_edit(mgr, result, &edits, &files);
  if (edits == 0) {
    ide_lsp_report_message(mgr, "Rename: nothing to change");
  } else {
    ide_lsp_report_message(mgr, "Renamed to '%s': %u edit%s in %u file%s", new_name, edits,
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
    ide_lsp_doc_send_change_now(doc);
    guint line0, utf16;
    ide_lsp_buffer_cursor_position(buf, &line0, &utf16);
    RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_RENAME, doc->uri);
    ctx->new_name = g_strdup(new_name);
    JsonNode *params = ide_lsp_position_params(doc->uri, line0, utf16);
    json_object_set_string_member(json_node_get_object(params), "newName", new_name);
    ide_lsp_client_request(doc->client, "textDocument/rename", params, on_rename_response, ctx);
    return;
  }
  legacy_rename(lsp, buf, new_name);
}

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
  g_autofree char *word = ide_lsp_extract_word_at_cursor(buf, NULL);
  if (word == NULL || word[0] == '\0') {
    ide_lsp_report_message(lsp, "Place the cursor on a symbol to rename it");
    return;
  }
  g_free(lsp->rename_old);
  lsp->rename_old = g_steal_pointer(&word);
  lsp->rename_input = g_string_new(lsp->rename_old);

  GtkWidget *pop = ide_lsp_new_popover_for(lsp, anchor_view, buf);
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
  ide_lsp_set_ui_state(lsp, IDE_LSP_UI_RENAME);
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

/* ═══════════════════════════ Formatting ═══════════════════════════ */

static void
on_format_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                   gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  if (mgr->shutting_down) {
    ide_lsp_ctx_done(ctx);
    return;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  ide_lsp_ctx_done(ctx);
  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Format failed: %s", error_message);
    return;
  }
  if (doc == NULL) {
    return;
  }
  g_autoptr(GPtrArray) edits = ide_lsp_parse_text_edits(result);
  if (edits->len == 0) {
    ide_lsp_report_message(mgr, "Already formatted");
    return;
  }
  ide_lsp_apply_edits_to_buffer(mgr, doc->buf, edits);
  ide_lsp_report_message(mgr, "Formatted with %s", doc->spec ? doc->spec->server_id : "language server");
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
    ide_lsp_report_message(lsp, "No language server with formatting support for this file");
    return;
  }
  ide_lsp_doc_send_change_now(doc);

  guint effective_tab = (tab_size > 0) ? tab_size : 4;
  gboolean effective_spaces = (tab_size > 0)
                                  ? insert_spaces
                                  : (g_strcmp0(doc->language_id, "go") != 0);

  g_autoptr(JsonNode) base = ide_lsp_text_document_params(doc->uri);
  JsonNode *params = json_node_copy(base);
  JsonObject *po = json_node_get_object(params);
  JsonObject *opts = json_object_new();
  json_object_set_int_member(opts, "tabSize", (gint64)effective_tab);
  json_object_set_boolean_member(opts, "insertSpaces", effective_spaces);
  json_object_set_object_member(po, "options", opts);

  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_FORMAT, doc->uri);
  ide_lsp_client_request(doc->client, "textDocument/formatting", params, on_format_response, ctx);
}

void
ide_lsp_manager_request_format(IdeLspManager *lsp, IdeBuffer *buf)
{
  ide_lsp_manager_request_format_with_options(lsp, buf, 0, TRUE);
}

/* ═══════════════════════════ Code actions ═══════════════════════════ */

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
    ide_lsp_apply_workspace_edit(lsp, json_object_get_member(ao, "edit"), &edits, &files);
    const char *title = ide_lsp_json_str(ao, "title");
    ide_lsp_report_message(lsp, "%s: %u edit%s in %u file%s", title ? title : "Code action",
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
    ide_lsp_ctx_done(ctx);
    return;
  }
  DocState *doc = g_hash_table_lookup(mgr->docs_by_uri, ctx->uri);
  ide_lsp_ctx_done(ctx);
  if (error_message != NULL || result == NULL) {
    ide_lsp_report_message(mgr, "Code action failed: %s", error_message ? error_message : "no result");
    return;
  }
  apply_code_action_node(mgr, doc, result);
}

void
ide_lsp_run_code_action(IdeLspManager *lsp, IdeBuffer *buf, guint index)
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
    RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_FORMAT, doc->uri);
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
  DocState *doc = NULL;
  GtkWidget *anchor = NULL;
  if (!ide_lsp_take_popover_response(ctx, TRUE, &doc, &anchor)) {
    return;
  }
  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Code actions failed: %s", error_message);
    return;
  }

  g_ptr_array_set_size(mgr->comp_items, 0);
  g_ptr_array_set_size(mgr->code_actions, 0);
  if (result != NULL && JSON_NODE_HOLDS_ARRAY(result)) {
    JsonArray *arr = json_node_get_array(result);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
      JsonObject *o = json_array_get_object_element(arr, i);
      if (o == NULL || ide_lsp_json_str(o, "title") == NULL) {
        continue;
      }
      if (json_object_get_boolean_member_with_default(o, "disabled", FALSE)) {
        continue;
      }
      IdeCompletionItem *it = g_new0(IdeCompletionItem, 1);
      it->label = g_strdup(ide_lsp_json_str(o, "title"));
      it->detail = g_strdup(ide_lsp_json_str(o, "kind"));
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
    ide_lsp_report_message(mgr, "No code actions available here");
    return;
  }
  mgr->comp_is_code_actions = TRUE;
  ide_lsp_present_completion_popover(mgr, anchor, doc->buf);
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
    ide_lsp_report_message(lsp, "Code actions need a running language server that supports them");
    return;
  }
  ide_lsp_doc_send_change_now(doc);

  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter a, b;
  if (!gtk_text_buffer_get_selection_bounds(tbuf, &a, &b)) {
    gtk_text_buffer_get_iter_at_mark(tbuf, &a, gtk_text_buffer_get_insert(tbuf));
    b = a;
  }
  guint l0 = (guint)gtk_text_iter_get_line(&a);
  g_autofree char *line_a = ide_lsp_buffer_line_text(tbuf, l0);
  guint c0 = ide_utf8_byte_to_utf16_col(line_a, (gsize)gtk_text_iter_get_line_index(&a));
  guint l1 = (guint)gtk_text_iter_get_line(&b);
  g_autofree char *line_b = ide_lsp_buffer_line_text(tbuf, l1);
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

  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_COMPLETION, doc->uri);
  ctx->token = lsp->popover_token;
  ide_lsp_ctx_set_anchor(ctx, anchor_view);
  ide_lsp_send_inflight_request(lsp, doc, ctx, "textDocument/codeAction",
                                json_builder_get_root(bld), on_code_actions_response);
}
