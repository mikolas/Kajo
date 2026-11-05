#include "ide_lsp.h"
#include "ide_picker.h"
#include "ide_search.h"

#include <string.h>

struct _IdeLspManager {
  char          *workspace_root;
  IdeLspJumpCb   jump_cb;
  IdeLspStatusCb status_cb;
  gpointer       cb_data;

  char          *active_server_name;
  GtkWidget     *active_popover;
  GtkWidget     *comp_list_box;
  GtkWidget     *comp_doc_label;
  GPtrArray     *comp_items;
  gint           selected_comp_idx;
  char          *comp_prefix;
};

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
  g_free(it);
}

char *
ide_jsonrpc_frame_message(const char *json_body)
{
  if (json_body == NULL) {
    json_body = "{}";
  }
  gsize len = strlen(json_body);
  return g_strdup_printf("Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n%s", len, json_body);
}

char *
ide_jsonrpc_extract_body(const char *stream_buf,
                         gsize       stream_len,
                         gsize      *out_consumed_bytes)
{
  if (out_consumed_bytes != NULL) {
    *out_consumed_bytes = 0;
  }
  if (stream_buf == NULL || stream_len == 0) {
    return NULL;
  }

  const char *hdr_end = g_strstr_len(stream_buf, (gssize)stream_len, "\r\n\r\n");
  if (hdr_end == NULL) {
    return NULL;
  }

  gsize hdr_len = (gsize)(hdr_end - stream_buf);
  g_autofree char *hdr_str = g_strndup(stream_buf, hdr_len);
  const char *cl_pos = g_strstr_len(hdr_str, (gssize)hdr_len, "Content-Length:");
  if (cl_pos == NULL) {
    cl_pos = g_strstr_len(hdr_str, (gssize)hdr_len, "content-length:");
  }
  if (cl_pos == NULL) {
    return NULL;
  }

  cl_pos += strlen("Content-Length:");
  while (*cl_pos == ' ' || *cl_pos == '\t') {
    cl_pos++;
  }
  gint64 body_len = g_ascii_strtoll(cl_pos, NULL, 10);
  if (body_len <= 0) {
    return NULL;
  }

  gsize total_needed = hdr_len + 4 + (gsize)body_len;
  if (stream_len < total_needed) {
    return NULL;
  }

  if (out_consumed_bytes != NULL) {
    *out_consumed_bytes = total_needed;
  }
  return g_strndup(hdr_end + 4, (gsize)body_len);
}

const char *
ide_lsp_detect_server_for_file(const char  *file_path,
                               const char **out_server_id)
{
  if (out_server_id != NULL) {
    *out_server_id = "none";
  }
  if (file_path == NULL || file_path[0] == '\0') {
    return NULL;
  }

  static const struct {
    const char *ext;
    const char *server_id;
    const char *binary;
  } kServers[] = {
    { ".c", "clangd", "clangd" },
    { ".h", "clangd", "clangd" },
    { ".cpp", "clangd", "clangd" },
    { ".hpp", "clangd", "clangd" },
    { ".cc", "clangd", "clangd" },
    { ".go", "gopls", "gopls" },
    { ".rs", "rust-analyzer", "rust-analyzer" },
    { ".py", "pyright", "pyright-langserver" },
    { ".sh", "bash-ls", "bash-language-server" },
    { ".json", "json-ls", "vscode-json-language-server" },
    { ".yaml", "yaml-ls", "yaml-language-server" },
    { ".yml", "yaml-ls", "yaml-language-server" },
    { ".toml", "taplo", "taplo" },
    { ".java", "jdtls", "jdtls" },
    { "meson.build", "mesonlsp", "mesonlsp" },
    { NULL, NULL, NULL }
  };

  for (gsize i = 0; kServers[i].ext != NULL; i++) {
    if (g_str_has_suffix(file_path, kServers[i].ext)) {
      if (out_server_id != NULL) {
        *out_server_id = kServers[i].server_id;
      }
      return kServers[i].binary;
    }
  }
  return NULL;
}

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

IdeLspManager *
ide_lsp_manager_new(const char    *workspace_root,
                    IdeLspJumpCb   jump_cb,
                    IdeLspStatusCb status_cb,
                    gpointer       user_data)
{
  IdeLspManager *lsp = g_new0(IdeLspManager, 1);
  lsp->workspace_root = g_strdup(workspace_root ? workspace_root : ".");
  lsp->jump_cb = jump_cb;
  lsp->status_cb = status_cb;
  lsp->cb_data = user_data;
  lsp->comp_items = g_ptr_array_new_with_free_func(ide_completion_item_free);
  return lsp;
}

void
ide_lsp_manager_free(IdeLspManager *lsp)
{
  if (lsp == NULL) {
    return;
  }
  ide_lsp_manager_cancel_popover(lsp);
  g_free(lsp->workspace_root);
  g_free(lsp->active_server_name);
  g_free(lsp->comp_prefix);
  g_clear_pointer(&lsp->comp_items, g_ptr_array_unref);
  g_free(lsp);
}

void
ide_lsp_manager_notify_open(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  if (buf == NULL) {
    return;
  }
  const char *srv_id = NULL;
  const char *bin = ide_lsp_detect_server_for_file(ide_buffer_get_file_path(buf), &srv_id);
  g_free(lsp->active_server_name);
  if (bin != NULL) {
    g_autofree char *found = g_find_program_in_path(bin);
    lsp->active_server_name = g_strdup(srv_id);
    if (lsp->status_cb != NULL) {
      lsp->status_cb(srv_id, found != NULL, lsp->cb_data);
    }
  } else {
    lsp->active_server_name = g_strdup("native");
    if (lsp->status_cb != NULL) {
      lsp->status_cb("native", TRUE, lsp->cb_data);
    }
  }
}

void
ide_lsp_manager_notify_change(IdeLspManager *lsp, IdeBuffer *buf)
{
  (void)lsp;
  if (buf != NULL) {
    ide_buffer_recompute_diffs(buf);
  }
}

void
ide_lsp_manager_notify_save(IdeLspManager *lsp, IdeBuffer *buf)
{
  (void)lsp;
  (void)buf;
}

void
ide_lsp_manager_cancel_popover(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  if (lsp->active_popover != NULL) {
    gtk_popover_popdown(GTK_POPOVER(lsp->active_popover));
    gtk_widget_unparent(lsp->active_popover);
    lsp->active_popover = NULL;
    lsp->comp_list_box = NULL;
    lsp->comp_doc_label = NULL;
  }
}

void
ide_lsp_manager_request_hover(IdeLspManager *lsp,
                              GtkWidget     *anchor_view,
                              IdeBuffer     *buf,
                              guint          line_1based,
                              guint          col_1based)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  if (anchor_view == NULL || buf == NULL) {
    return;
  }

  g_autofree char *symbol = extract_word_at_cursor(buf, NULL);
  const IdeDiagnostic *diag = ide_buffer_get_line_diagnostic(buf, line_1based);

  /* Search workspace for symbol declaration context via ripgrep */
  g_autofree char *decl_line = NULL;
  if (symbol != NULL && symbol[0] != '\0' && lsp->workspace_root != NULL) {
    g_autofree char *pat = g_strdup_printf("\\b%s\\s*[\\(;\\{]", symbol);
    const char *argv[] = {
      "rg", "--no-heading", "--line-number", "--max-count=1",
      "--glob=*.h", "--glob=*.c", "--glob=*.go", "--",
      pat, lsp->workspace_root, NULL
    };
    g_autofree char *out = NULL;
    if (g_spawn_sync(NULL,
                     (char **)argv,
                     NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL,
                     NULL,
                     &out,
                     NULL,
                     NULL,
                     NULL) &&
        out != NULL && out[0] != '\0') {
      decl_line = g_strstrip(g_strdup(out));
    }
  }

  GtkWidget *pop = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), FALSE);
  gtk_widget_set_parent(pop, anchor_view);
  GdkRectangle rect = compute_cursor_rect(anchor_view, buf);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(card, "shell-popover-card");
  gtk_widget_set_size_request(card, 380, -1);

  g_autofree char *hdr_txt = g_strdup_printf("ƒ  %s  (Ln %u, Col %u)",
                                             (symbol && symbol[0]) ? symbol : "<cursor>",
                                             line_1based,
                                             col_1based);
  GtkWidget *hdr = gtk_label_new(hdr_txt);
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
  gtk_widget_add_css_class(hdr, "shell-popover-title");
  gtk_box_append(GTK_BOX(card), hdr);

  if (decl_line != NULL) {
    GtkWidget *code_lbl = gtk_label_new(decl_line);
    gtk_label_set_xalign(GTK_LABEL(code_lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(code_lbl), TRUE);
    gtk_widget_add_css_class(code_lbl, "code-preview-box");
    gtk_box_append(GTK_BOX(card), code_lbl);
  }

  if (diag != NULL) {
    g_autofree char *d_txt = g_strdup_printf("Diagnostic [%s]: %s", diag->source, diag->message);
    GtkWidget *dlbl = gtk_label_new(d_txt);
    gtk_label_set_xalign(GTK_LABEL(dlbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(dlbl), TRUE);
    gtk_widget_add_css_class(dlbl, "row-subtitle");
    gtk_box_append(GTK_BOX(card), dlbl);
  }

  GtkWidget *hint = gtk_label_new("[Esc / q: Dismiss Hover]   [Space g d: Goto Definition]");
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_widget_add_css_class(hint, "shell-popover-badge");
  gtk_box_append(GTK_BOX(card), hint);

  gtk_popover_set_child(GTK_POPOVER(pop), card);
  lsp->active_popover = pop;
  gtk_popover_popup(GTK_POPOVER(pop));
}

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
    if (prefix != NULL && prefix[0] != '\0' &&
        !g_str_has_prefix(ident, prefix) &&
        ide_fuzzy_score(prefix, ident) == 0) {
      continue;
    }

    g_hash_table_add(seen, g_strdup(ident));
    IdeCompletionItem *item = g_new0(IdeCompletionItem, 1);
    item->label = g_strdup(ident);
    item->insert_text = g_strdup(ident);
    item->detail = (*p == '(') ? g_strdup("function") : g_strdup("identifier");
    item->documentation = g_strdup_printf("Workspace / buffer symbol '%s'", ident);
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
  }
  const IdeCompletionItem *it = g_ptr_array_index(lsp->comp_items, lsp->selected_comp_idx);
  if (lsp->comp_doc_label != NULL && it->documentation != NULL) {
    gtk_label_set_text(GTK_LABEL(lsp->comp_doc_label), it->documentation);
  }
}

void
ide_lsp_manager_request_completion(IdeLspManager *lsp,
                                   GtkWidget     *anchor_view,
                                   IdeBuffer     *buf,
                                   guint          line_1based,
                                   guint          col_1based)
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
  collect_buffer_identifiers(lsp, buf, lsp->comp_prefix);

  if (lsp->comp_items->len == 0) {
    return;
  }

  GtkWidget *pop = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), FALSE);
  gtk_widget_set_parent(pop, anchor_view);
  GdkRectangle rect = compute_cursor_rect(anchor_view, buf);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "shell-popover-card");
  gtk_widget_set_size_request(card, 340, -1);

  lsp->comp_list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(lsp->comp_list_box), GTK_SELECTION_SINGLE);

  guint max_vis = MIN(lsp->comp_items->len, 8u);
  for (guint i = 0; i < max_vis; i++) {
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
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(lbl, "row-title");
    gtk_box_append(GTK_BOX(hbox), lbl);

    GtkWidget *det = gtk_label_new(it->detail);
    gtk_widget_set_valign(det, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(det, "row-subtitle");
    gtk_box_append(GTK_BOX(hbox), det);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(GTK_LIST_BOX(lsp->comp_list_box), row);
  }
  gtk_box_append(GTK_BOX(card), lsp->comp_list_box);

  lsp->comp_doc_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(lsp->comp_doc_label), 0.0f);
  gtk_widget_set_margin_top(lsp->comp_doc_label, 6);
  gtk_widget_add_css_class(lsp->comp_doc_label, "row-subtitle");
  gtk_box_append(GTK_BOX(card), lsp->comp_doc_label);

  gtk_popover_set_child(GTK_POPOVER(pop), card);
  lsp->active_popover = pop;
  lsp->selected_comp_idx = 0;
  update_comp_selection_ui(lsp);
  gtk_popover_popup(GTK_POPOVER(pop));
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

void
ide_lsp_manager_confirm_selection(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  if (buf != NULL && lsp->selected_comp_idx >= 0 &&
      (guint)lsp->selected_comp_idx < lsp->comp_items->len) {
    const IdeCompletionItem *it = g_ptr_array_index(lsp->comp_items, lsp->selected_comp_idx);
    const char *ins = it->insert_text ? it->insert_text : it->label;
    gsize prefix_len = lsp->comp_prefix ? strlen(lsp->comp_prefix) : 0;
    if (ins != NULL && strlen(ins) >= prefix_len) {
      GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
      gtk_text_buffer_insert_at_cursor(tbuf, ins + prefix_len, -1);
    }
  }
  ide_lsp_manager_cancel_popover(lsp);
}

void
ide_lsp_manager_request_definition(IdeLspManager *lsp,
                                   IdeBuffer     *buf,
                                   guint          line_1based,
                                   guint          col_1based)
{
  (void)line_1based;
  (void)col_1based;
  g_return_if_fail(lsp != NULL);
  if (buf == NULL || lsp->jump_cb == NULL) {
    return;
  }

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
  g_autofree char *out = NULL;
  if (g_spawn_sync(NULL,
                   (char **)argv,
                   NULL,
                   G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                   NULL,
                   NULL,
                   &out,
                   NULL,
                   NULL,
                   NULL) &&
      out != NULL && out[0] != '\0') {
    g_auto(GStrv) parts = g_strsplit(out, ":", 4);
    if (g_strv_length(parts) >= 3) {
      guint ln = (guint)MAX(1, g_ascii_strtoll(parts[1], NULL, 10));
      guint cl = (guint)MAX(1, g_ascii_strtoll(parts[2], NULL, 10));
      lsp->jump_cb(parts[0], ln, cl, lsp->cb_data);
    }
  }
}

void
ide_lsp_manager_request_rename(IdeLspManager *lsp,
                               IdeBuffer     *buf,
                               guint          line_1based,
                               guint          col_1based,
                               const char    *new_name)
{
  (void)lsp;
  (void)line_1based;
  (void)col_1based;
  if (buf == NULL || new_name == NULL || new_name[0] == '\0') {
    return;
  }
  g_autofree char *old_sym = extract_word_at_cursor(buf, NULL);
  if (old_sym == NULL || old_sym[0] == '\0') {
    return;
  }
  g_autofree char *orig = ide_buffer_get_text(buf);
  guint cnt = 0;
  g_autofree char *replaced = ide_replace_text_all(orig,
                                                   old_sym,
                                                   new_name,
                                                   TRUE,
                                                   TRUE,
                                                   FALSE,
                                                   &cnt);
  if (cnt > 0) {
    ide_buffer_set_text(buf, replaced);
  }
}
