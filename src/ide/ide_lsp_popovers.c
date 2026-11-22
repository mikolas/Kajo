#include "ide_lsp_private.h"

#include <string.h>

/* ═══════════════════════════ Popover core & anchor tracking ═══════════════════════════ */

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

static void
set_active_anchor(IdeLspManager *lsp, GtkWidget *anchor_view)
{
  if (lsp->active_anchor != NULL) {
    g_object_remove_weak_pointer(G_OBJECT(lsp->active_anchor), (gpointer *)&lsp->active_anchor);
    lsp->active_anchor = NULL;
  }
  if (anchor_view != NULL) {
    lsp->active_anchor = anchor_view;
    g_object_add_weak_pointer(G_OBJECT(anchor_view), (gpointer *)&lsp->active_anchor);
  }
}

static void
set_ui_state_ex(IdeLspManager *lsp, IdeLspUiState state, gboolean force_notify)
{
  if (!force_notify && lsp->ui_state == state) {
    return;
  }
  lsp->ui_state = state;
  if (lsp->ui_state_cb != NULL && !lsp->shutting_down) {
    lsp->ui_state_cb(state, lsp->ui_data);
  }
}

void
ide_lsp_set_ui_state(IdeLspManager *lsp, IdeLspUiState state)
{
  set_ui_state_ex(lsp, state, FALSE);
}

static void
close_popover_widgets(IdeLspManager *lsp)
{
  if (lsp->active_popover != NULL) {
    gtk_popover_popdown(GTK_POPOVER(lsp->active_popover));
    gtk_widget_unparent(lsp->active_popover);
    lsp->active_popover = NULL;
  }
  set_active_anchor(lsp, NULL);
  lsp->active_buf = NULL;
  lsp->comp_list_box = NULL;
  lsp->comp_scroller = NULL;
  lsp->comp_doc_label = NULL;
  lsp->rename_label = NULL;
  lsp->sig_hdr_label = NULL;
  lsp->sig_code_label = NULL;
  lsp->sig_doc_label = NULL;
}

void
ide_lsp_manager_cancel_popover(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_cancel_inflight(lsp);
  if (lsp->rg_cancellable != NULL) {
    g_cancellable_cancel(lsp->rg_cancellable);
    g_clear_object(&lsp->rg_cancellable);
  }
  lsp->popover_token++;
  close_popover_widgets(lsp);
  if (lsp->rename_input != NULL) {
    g_string_free(lsp->rename_input, TRUE);
    lsp->rename_input = NULL;
  }
  lsp->comp_is_code_actions = FALSE;
  if (lsp->ref_items != NULL) {
    g_ptr_array_set_size(lsp->ref_items, 0);
  }
  if (lsp->sig_items != NULL) {
    g_ptr_array_set_size(lsp->sig_items, 0);
  }
  lsp->sig_active_idx = 0;
  lsp->sig_active_param = -1;
  if (lsp->call_items != NULL) {
    g_ptr_array_set_size(lsp->call_items, 0);
  }
  g_clear_pointer(&lsp->call_root_item, json_node_unref);
  g_clear_pointer(&lsp->call_root_uri, g_free);
  lsp->call_outgoing = FALSE;
  ide_lsp_set_ui_state(lsp, IDE_LSP_UI_CLOSED);
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

GtkWidget *
ide_lsp_new_popover_for(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  GtkWidget *pop = gtk_popover_new();
  gtk_widget_add_css_class(pop, "ide-lsp-popover");
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(pop), FALSE);
  gtk_widget_set_parent(pop, anchor_view);
  GdkRectangle rect = compute_cursor_rect(anchor_view, buf);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
  lsp->active_popover = pop;
  set_active_anchor(lsp, anchor_view);
  lsp->active_buf = buf;
  return pop;
}

/* ─── Shared popover widget builders ─── */

static GtkWidget *
popover_create_card(int min_width, int spacing)
{
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
  gtk_widget_add_css_class(card, "shell-popover-card");
  gtk_widget_set_size_request(card, min_width, -1);
  return card;
}

static GtkWidget *
popover_append_label(GtkWidget *parent, const char *text, const char *css_class)
{
  GtkWidget *lbl = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  if (css_class != NULL) {
    gtk_widget_add_css_class(lbl, css_class);
  }
  gtk_box_append(GTK_BOX(parent), lbl);
  return lbl;
}

static void
on_popover_list_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void)box;
  IdeLspManager *lsp = user_data;
  if (lsp == NULL || row == NULL || lsp->shutting_down) {
    return;
  }
  gint idx = gtk_list_box_row_get_index(row);
  if (idx >= 0) {
    lsp->selected_comp_idx = idx;
    ide_lsp_manager_confirm_selection(lsp, lsp->active_buf);
  }
}

static GtkWidget *
popover_create_list_box(IdeLspManager *lsp, gboolean activate_on_click)
{
  lsp->comp_list_box = gtk_list_box_new();
  gtk_widget_add_css_class(lsp->comp_list_box, "ide-tree-listbox");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(lsp->comp_list_box), GTK_SELECTION_SINGLE);
  if (activate_on_click) {
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(lsp->comp_list_box), TRUE);
    g_signal_connect(lsp->comp_list_box, "row-activated",
                     G_CALLBACK(on_popover_list_row_activated), lsp);
  }
  return lsp->comp_list_box;
}

static void
popover_attach_scroller(IdeLspManager *lsp, GtkWidget *card, int max_height)
{
  lsp->comp_scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lsp->comp_scroller), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(lsp->comp_scroller), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(lsp->comp_scroller), max_height);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lsp->comp_scroller), lsp->comp_list_box);
  gtk_box_append(GTK_BOX(card), lsp->comp_scroller);
}

static const char *
rel_path_for_display(const IdeLspManager *lsp, const char *abs_path)
{
  if (abs_path == NULL) {
    return "";
  }
  if (lsp != NULL && lsp->workspace_root != NULL && lsp->workspace_root[0] != '\0') {
    gsize rlen = strlen(lsp->workspace_root);
    if (g_str_has_prefix(abs_path, lsp->workspace_root)) {
      if (abs_path[rlen] == '/') {
        return abs_path + rlen + 1;
      }
      if (rlen > 0 && lsp->workspace_root[rlen - 1] == '/') {
        return abs_path + rlen;
      }
    }
  }
  return abs_path;
}

static guint
resolve_open_or_disk_col(IdeLspManager *mgr,
                         const char    *path,
                         guint          line_1based,
                         guint          col_1based,
                         char         **preview_out)
{
  if (path == NULL) {
    return col_1based;
  }
  guint line0 = line_1based > 0 ? line_1based - 1 : 0;
  guint utf16 = col_1based > 0 ? col_1based - 1 : 0;
  g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
  DocState *odoc = (uri != NULL) ? g_hash_table_lookup(mgr->docs_by_uri, uri) : NULL;
  if (odoc != NULL && odoc->buf != NULL) {
    GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(odoc->buf));
    if (preview_out != NULL && (gint)line0 < gtk_text_buffer_get_line_count(tbuf)) {
      g_free(*preview_out);
      *preview_out = g_strstrip(ide_lsp_buffer_line_text(tbuf, line0));
    }
    return ide_lsp_buffer_char_col_1based(tbuf, line0, utf16);
  }
  guint col1 = col_1based;
  char *line = ide_lsp_read_disk_line_and_col(path, line0, utf16, &col1);
  if (preview_out != NULL) {
    g_free(*preview_out);
    *preview_out = line;
  } else {
    g_free(line);
  }
  return col1;
}

/* ═══════════════════════════ Hover ═══════════════════════════ */

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

void
ide_lsp_show_hover_card(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf, guint line_1based,
                        guint col_1based, const char *title, const char *code, const char *doc_text,
                        gboolean is_fallback)
{
  ide_lsp_manager_cancel_popover(lsp);
  GtkWidget *pop = ide_lsp_new_popover_for(lsp, anchor_view, buf);
  GtkWidget *card = popover_create_card(380, 6);

  g_autofree char *hdr_txt = g_strdup_printf("ƒ  %s  (Ln %u, Col %u)%s",
                                             (title && title[0]) ? title : "<cursor>",
                                             line_1based, col_1based,
                                             is_fallback ? "  [RG FALLBACK — NO LSP]" : "");
  popover_append_label(card, hdr_txt, "shell-popover-title");

  if (code != NULL && code[0] != '\0') {
    GtkWidget *code_lbl = popover_append_label(card, code, "code-preview-box");
    gtk_label_set_wrap(GTK_LABEL(code_lbl), TRUE);
    gtk_label_set_selectable(GTK_LABEL(code_lbl), FALSE);
  }
  if (doc_text != NULL && doc_text[0] != '\0') {
    GtkWidget *doc_lbl = popover_append_label(card, doc_text, "row-subtitle");
    gtk_label_set_wrap(GTK_LABEL(doc_lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(doc_lbl), 64);
  }

  const IdeDiagnostic *diag = ide_buffer_get_line_diagnostic(buf, line_1based);
  if (diag != NULL) {
    g_autofree char *d_txt = g_strdup_printf("Diagnostic [%s]: %s", diag->source, diag->message);
    GtkWidget *dlbl = popover_append_label(card, d_txt, "row-subtitle");
    gtk_label_set_wrap(GTK_LABEL(dlbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dlbl), 64);
  }

  popover_append_label(card, "[Esc / q: Dismiss Hover]   [Space g d: Goto Definition]",
                       "shell-popover-badge");

  gtk_popover_set_child(GTK_POPOVER(pop), card);
  gtk_popover_popup(GTK_POPOVER(pop));
  ide_lsp_set_ui_state(lsp, IDE_LSP_UI_HOVER);
}

static void
on_hover_response(IdeLspClient *client, JsonNode *result, const char *error_message,
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

  guint line0 = 0, utf16 = 0;
  ide_lsp_buffer_cursor_position(doc->buf, &line0, &utf16);
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(doc->buf));
  guint col1 = ide_lsp_buffer_char_col_1based(tbuf, line0, utf16);
  g_autofree char *symbol = ide_lsp_extract_word_at_cursor(doc->buf, NULL);

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
  ide_lsp_show_hover_card(mgr, anchor, doc->buf, line0 + 1, col1, symbol, code, text, FALSE);
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
    ide_lsp_doc_send_change_now(doc);
    guint line0, utf16;
    ide_lsp_buffer_cursor_position(buf, &line0, &utf16);

    RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_HOVER, doc->uri);
    ctx->token = lsp->popover_token;
    ide_lsp_ctx_set_anchor(ctx, anchor_view);
    ide_lsp_send_inflight_request(lsp, doc, ctx, "textDocument/hover",
                                  ide_lsp_position_params(doc->uri, line0, utf16),
                                  on_hover_response);
    return;
  }
  ide_lsp_legacy_hover(lsp, anchor_view, buf, line_1based, col_1based);
}

/* ═══════════════════════════ Completion & selection ═══════════════════════════ */

static guint
active_list_count(const IdeLspManager *lsp)
{
  if (lsp == NULL) {
    return 0;
  }
  switch (lsp->ui_state) {
    case IDE_LSP_UI_REFERENCES:
      return lsp->ref_items != NULL ? lsp->ref_items->len : 0;
    case IDE_LSP_UI_CALL_HIERARCHY:
      return lsp->call_items != NULL ? lsp->call_items->len : 0;
    default:
      return lsp->comp_items != NULL ? lsp->comp_items->len : 0;
  }
}

static void
update_comp_selection_ui(IdeLspManager *lsp)
{
  guint count = active_list_count(lsp);
  if (count == 0) {
    return;
  }
  lsp->selected_comp_idx = CLAMP(lsp->selected_comp_idx, 0, (gint)count - 1);
  if (lsp->comp_list_box != NULL) {
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
  }
  if ((lsp->ui_state == IDE_LSP_UI_COMPLETION || lsp->ui_state == IDE_LSP_UI_CODE_ACTION ||
       lsp->ui_state == IDE_LSP_UI_CLOSED) &&
      lsp->comp_doc_label != NULL && lsp->comp_items != NULL &&
      (guint)lsp->selected_comp_idx < lsp->comp_items->len) {
    const IdeCompletionItem *it = g_ptr_array_index(lsp->comp_items, lsp->selected_comp_idx);
    gtk_label_set_text(GTK_LABEL(lsp->comp_doc_label), it->documentation ? it->documentation : "");
  }
}

void
ide_lsp_present_completion_popover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  if (lsp->comp_items->len == 0) {
    return;
  }
  GtkWidget *pop = ide_lsp_new_popover_for(lsp, anchor_view, buf);
  GtkWidget *card = popover_create_card(380, 0);
  popover_create_list_box(lsp, FALSE);

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

  popover_attach_scroller(lsp, card, 260);

  lsp->comp_doc_label = popover_append_label(card, "", "row-subtitle");
  gtk_label_set_wrap(GTK_LABEL(lsp->comp_doc_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(lsp->comp_doc_label), 56);
  gtk_widget_set_margin_top(lsp->comp_doc_label, 6);

  gtk_popover_set_child(GTK_POPOVER(pop), card);
  lsp->selected_comp_idx = 0;
  update_comp_selection_ui(lsp);
  gtk_popover_popup(GTK_POPOVER(pop));
  ide_lsp_set_ui_state(lsp, lsp->comp_is_code_actions ? IDE_LSP_UI_CODE_ACTION : IDE_LSP_UI_COMPLETION);
}

static void
on_completion_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                       gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  g_autofree char *prefix = g_strdup(ctx->prefix);
  DocState *doc = NULL;
  GtkWidget *anchor = NULL;
  if (!ide_lsp_take_popover_response(ctx, TRUE, &doc, &anchor)) {
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
      IdeCompletionItem *it = o != NULL ? ide_lsp_parse_completion_item(o, prefix) : NULL;
      if (it != NULL) {
        g_ptr_array_add(mgr->comp_items, it);
      }
    }
    g_ptr_array_sort(mgr->comp_items, ide_lsp_completion_cmp);
    if (mgr->comp_items->len > 60) {
      g_ptr_array_set_size(mgr->comp_items, 60);
    }
  }
  if (mgr->comp_items->len == 0) {
    /* Server had nothing: fall back to words from the buffer. */
    ide_lsp_collect_buffer_identifiers(mgr, doc->buf, prefix);
  }
  ide_lsp_present_completion_popover(mgr, anchor, doc->buf);
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
  g_autofree char *word = ide_lsp_extract_word_at_cursor(buf, &lsp->comp_prefix);

  DocState *doc = g_hash_table_lookup(lsp->docs, buf);
  if (doc != NULL && doc->client != NULL && doc->sent_open &&
      ide_lsp_client_get_state(doc->client) != IDE_LSP_CLIENT_STOPPED) {
    ide_lsp_doc_send_change_now(doc);
    guint line0, utf16;
    ide_lsp_buffer_cursor_position(buf, &line0, &utf16);

    RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_COMPLETION, doc->uri);
    ctx->token = lsp->popover_token;
    ctx->prefix = g_strdup(lsp->comp_prefix ? lsp->comp_prefix : "");
    ide_lsp_ctx_set_anchor(ctx, anchor_view);

    JsonNode *params = ide_lsp_position_params(doc->uri, line0, utf16);
    /* completion context: invoked explicitly by the user */
    JsonObject *po = json_node_get_object(params);
    JsonObject *cctx = json_object_new();
    json_object_set_int_member(cctx, "triggerKind", 1);
    json_object_set_object_member(po, "context", cctx);

    ide_lsp_send_inflight_request(lsp, doc, ctx, "textDocument/completion", params,
                                  on_completion_response);
    return;
  }

  ide_lsp_collect_buffer_identifiers(lsp, buf, lsp->comp_prefix);
  ide_lsp_present_completion_popover(lsp, anchor_view, buf);
}

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

static void refresh_signature_ui(IdeLspManager *lsp);

void
ide_lsp_manager_select_next(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  if (lsp->ui_state == IDE_LSP_UI_SIGNATURE) {
    if (lsp->sig_items != NULL && lsp->sig_items->len > 1) {
      lsp->sig_active_idx = (lsp->sig_active_idx + 1) % lsp->sig_items->len;
      refresh_signature_ui(lsp);
    }
    return;
  }
  lsp->selected_comp_idx++;
  update_comp_selection_ui(lsp);
}

void
ide_lsp_manager_select_prev(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  if (lsp->ui_state == IDE_LSP_UI_SIGNATURE) {
    if (lsp->sig_items != NULL && lsp->sig_items->len > 1) {
      lsp->sig_active_idx = (lsp->sig_active_idx + lsp->sig_items->len - 1) % lsp->sig_items->len;
      refresh_signature_ui(lsp);
    }
    return;
  }
  lsp->selected_comp_idx--;
  update_comp_selection_ui(lsp);
}

gboolean
ide_lsp_manager_select_index(IdeLspManager *lsp, int idx)
{
  g_return_val_if_fail(lsp != NULL, FALSE);
  if (lsp->ui_state == IDE_LSP_UI_SIGNATURE) {
    if (lsp->sig_items == NULL || idx < 0 || (guint)idx >= lsp->sig_items->len) {
      return FALSE;
    }
    lsp->sig_active_idx = (guint)idx;
    refresh_signature_ui(lsp);
    return TRUE;
  }
  guint count = active_list_count(lsp);
  if (count == 0 || idx < 0 || (guint)idx >= count) {
    return FALSE;
  }
  lsp->selected_comp_idx = idx;
  update_comp_selection_ui(lsp);
  return TRUE;
}

static void
confirm_jump_location(IdeLspManager *lsp, const char *path, guint line, guint col)
{
  g_autofree char *jump_path = g_strdup(path);
  ide_lsp_manager_cancel_popover(lsp);
  if (jump_path != NULL && lsp->jump_cb != NULL) {
    lsp->jump_cb(jump_path, line, col, lsp->cb_data);
  }
}

void
ide_lsp_manager_confirm_selection(IdeLspManager *lsp, IdeBuffer *buf)
{
  g_return_if_fail(lsp != NULL);
  if (lsp->ui_state == IDE_LSP_UI_REFERENCES) {
    if (lsp->ref_items != NULL && lsp->selected_comp_idx >= 0 &&
        (guint)lsp->selected_comp_idx < lsp->ref_items->len) {
      const IdeLspLocation *loc = g_ptr_array_index(lsp->ref_items, lsp->selected_comp_idx);
      confirm_jump_location(lsp, loc->path, loc->line_1based, loc->col_1based);
    } else {
      ide_lsp_manager_cancel_popover(lsp);
    }
    return;
  }
  if (lsp->ui_state == IDE_LSP_UI_CALL_HIERARCHY) {
    if (lsp->call_items != NULL && lsp->selected_comp_idx >= 0 &&
        (guint)lsp->selected_comp_idx < lsp->call_items->len) {
      const IdeLspCallEntry *ce = g_ptr_array_index(lsp->call_items, lsp->selected_comp_idx);
      confirm_jump_location(lsp, ce->path, ce->line_1based, ce->col_1based);
    } else {
      ide_lsp_manager_cancel_popover(lsp);
    }
    return;
  }
  if (lsp->ui_state == IDE_LSP_UI_SIGNATURE) {
    ide_lsp_manager_cancel_popover(lsp);
    return;
  }
  if (lsp->comp_is_code_actions) {
    gint idx = lsp->selected_comp_idx;
    ide_lsp_manager_cancel_popover(lsp);
    if (buf != NULL && idx >= 0) {
      ide_lsp_run_code_action(lsp, buf, (guint)idx);
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
      ide_lsp_buffer_cursor_position(buf, &line0, &utf16);
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
    ide_lsp_apply_edits_to_buffer(lsp, buf, edits);
    return;
  }
  ide_lsp_manager_cancel_popover(lsp);
}

/* ═══════════════════════════ References ═══════════════════════════ */

static void
present_references_popover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  if (lsp->ref_items == NULL || lsp->ref_items->len == 0) {
    return;
  }
  close_popover_widgets(lsp);
  lsp->selected_comp_idx = 0;
  lsp->active_buf = buf;

  if (anchor_view != NULL) {
    GtkWidget *pop = ide_lsp_new_popover_for(lsp, anchor_view, buf);
    GtkWidget *card = popover_create_card(460, 6);

    g_autofree char *hdr_txt = g_strdup_printf("REFERENCES (%u)", lsp->ref_items->len);
    popover_append_label(card, hdr_txt, "shell-popover-title");
    popover_create_list_box(lsp, TRUE);

    guint shown = MIN(lsp->ref_items->len, 100u);
    for (guint i = 0; i < shown; i++) {
      const IdeLspLocation *loc = g_ptr_array_index(lsp->ref_items, i);
      GtkWidget *row = gtk_list_box_row_new();
      gtk_widget_add_css_class(row, "widget-row-card");
      gtk_widget_add_css_class(row, "ide-picker-row");

      GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

      char num_buf[8];
      if (i < 9) {
        g_snprintf(num_buf, sizeof(num_buf), "%u", i + 1);
      } else {
        g_strlcpy(num_buf, "•", sizeof(num_buf));
      }
      GtkWidget *badge = gtk_label_new(num_buf);
      gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(badge, "shell-popover-badge");
      gtk_widget_add_css_class(badge, "ide-symbol-badge");
      gtk_box_append(GTK_BOX(hbox), badge);

      GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_hexpand(vbox, TRUE);
      gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

      g_autofree char *loc_txt = g_strdup_printf("%s:%u:%u",
                                                 rel_path_for_display(lsp, loc->path),
                                                 loc->line_1based,
                                                 loc->col_1based);
      GtkWidget *loc_lbl = gtk_label_new(loc_txt);
      gtk_label_set_xalign(GTK_LABEL(loc_lbl), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(loc_lbl), PANGO_ELLIPSIZE_START);
      gtk_widget_set_valign(loc_lbl, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(loc_lbl, "row-title");
      gtk_box_append(GTK_BOX(vbox), loc_lbl);

      if (loc->preview != NULL && loc->preview[0] != '\0') {
        GtkWidget *prev_lbl = gtk_label_new(loc->preview);
        gtk_label_set_xalign(GTK_LABEL(prev_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(prev_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(prev_lbl), 58);
        gtk_widget_set_valign(prev_lbl, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(prev_lbl, "row-subtitle");
        gtk_box_append(GTK_BOX(vbox), prev_lbl);
      }

      gtk_box_append(GTK_BOX(hbox), vbox);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
      gtk_list_box_append(GTK_LIST_BOX(lsp->comp_list_box), row);
    }

    popover_attach_scroller(lsp, card, 280);
    popover_append_label(card, "[j/k: Select]   [Enter / 1-9: Jump]   [Esc: Close]",
                         "shell-popover-badge");

    gtk_popover_set_child(GTK_POPOVER(pop), card);
    gtk_popover_popup(GTK_POPOVER(pop));
  }

  ide_lsp_set_ui_state(lsp, IDE_LSP_UI_REFERENCES);
  update_comp_selection_ui(lsp);
}

static void
on_references_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                       gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  DocState *doc = NULL;
  GtkWidget *anchor = NULL;
  if (!ide_lsp_take_popover_response(ctx, FALSE, &doc, &anchor)) {
    return;
  }
  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Find references failed: %s", error_message);
    return;
  }

  g_autoptr(GPtrArray) locs = ide_lsp_parse_locations(result);
  if (locs->len == 0) {
    ide_lsp_report_message(mgr, "No references found");
    return;
  }
  for (guint i = 0; i < locs->len; i++) {
    IdeLspLocation *loc = g_ptr_array_index(locs, i);
    loc->col_1based = resolve_open_or_disk_col(mgr, loc->path, loc->line_1based,
                                               loc->col_1based, &loc->preview);
  }

  g_clear_pointer(&mgr->ref_items, g_ptr_array_unref);
  mgr->ref_items = g_steal_pointer(&locs);
  present_references_popover(mgr, anchor, doc->buf);
}

void
ide_lsp_manager_request_references(IdeLspManager *lsp,
                                   GtkWidget     *anchor_view,
                                   IdeBuffer     *buf,
                                   guint          line_1based,
                                   guint          col_1based)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  if (buf == NULL) {
    return;
  }

  DocState *doc = ide_lsp_require_doc(lsp, buf, "referencesProvider", "Find references", "references");
  if (doc == NULL) {
    return;
  }

  ide_lsp_doc_send_change_now(doc);
  guint line0 = 0, utf16 = 0;
  ide_lsp_resolve_request_position(buf, line_1based, col_1based, &line0, &utf16);

  JsonNode *params = ide_lsp_position_params(doc->uri, line0, utf16);
  JsonObject *po = json_node_get_object(params);
  JsonObject *rctx = json_object_new();
  json_object_set_boolean_member(rctx, "includeDeclaration", TRUE);
  json_object_set_object_member(po, "context", rctx);

  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_REFERENCES, doc->uri);
  ctx->token = lsp->popover_token;
  ctx->had_anchor = (anchor_view != NULL);
  ide_lsp_ctx_set_anchor(ctx, anchor_view);
  ide_lsp_send_inflight_request(lsp, doc, ctx, "textDocument/references", params,
                                on_references_response);
}

/* ═══════════════════════════ Signature Help ═══════════════════════════ */

static void
refresh_signature_ui(IdeLspManager *lsp)
{
  if (lsp->sig_items == NULL || lsp->sig_items->len == 0) {
    return;
  }
  if (lsp->sig_active_idx >= lsp->sig_items->len) {
    lsp->sig_active_idx = lsp->sig_items->len - 1;
  }
  const IdeLspSignatureInfo *sig = g_ptr_array_index(lsp->sig_items, lsp->sig_active_idx);
  gint active_param = (sig->active_param >= 0) ? sig->active_param : lsp->sig_active_param;

  if (lsp->sig_hdr_label != NULL) {
    if (lsp->sig_items->len > 1) {
      g_autofree char *hdr = g_strdup_printf("SIGNATURE HELP (%u / %u)",
                                             lsp->sig_active_idx + 1, lsp->sig_items->len);
      gtk_label_set_text(GTK_LABEL(lsp->sig_hdr_label), hdr);
    } else {
      gtk_label_set_text(GTK_LABEL(lsp->sig_hdr_label), "SIGNATURE HELP");
    }
  }
  if (lsp->sig_code_label != NULL) {
    g_autofree char *markup = ide_lsp_format_signature_pango(sig, active_param);
    gtk_label_set_markup(GTK_LABEL(lsp->sig_code_label), markup);
  }
  if (lsp->sig_doc_label != NULL) {
    if (sig->documentation != NULL && sig->documentation[0] != '\0') {
      gtk_label_set_text(GTK_LABEL(lsp->sig_doc_label), sig->documentation);
      gtk_widget_set_visible(lsp->sig_doc_label, TRUE);
    } else {
      gtk_label_set_text(GTK_LABEL(lsp->sig_doc_label), "");
      gtk_widget_set_visible(lsp->sig_doc_label, FALSE);
    }
  }
}

static void
present_signature_popover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  if (lsp->sig_items == NULL || lsp->sig_items->len == 0) {
    return;
  }
  close_popover_widgets(lsp);
  lsp->active_buf = buf;

  if (anchor_view != NULL) {
    GtkWidget *pop = ide_lsp_new_popover_for(lsp, anchor_view, buf);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_TOP);
    GtkWidget *card = popover_create_card(420, 6);

    lsp->sig_hdr_label = popover_append_label(card, "SIGNATURE HELP", "shell-popover-title");

    lsp->sig_code_label = popover_append_label(card, "", "code-preview-box");
    gtk_label_set_wrap(GTK_LABEL(lsp->sig_code_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lsp->sig_code_label), 64);

    lsp->sig_doc_label = popover_append_label(card, "", "row-subtitle");
    gtk_label_set_wrap(GTK_LABEL(lsp->sig_doc_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lsp->sig_doc_label), 64);

    const char *hint_txt = (lsp->sig_items->len > 1)
                               ? "[j/k: Cycle Overload]   [Esc / Enter: Dismiss]"
                               : "[Esc / Enter: Dismiss]";
    popover_append_label(card, hint_txt, "shell-popover-badge");

    gtk_popover_set_child(GTK_POPOVER(pop), card);
    refresh_signature_ui(lsp);
    gtk_popover_popup(GTK_POPOVER(pop));
  }

  ide_lsp_set_ui_state(lsp, IDE_LSP_UI_SIGNATURE);
}

static void
on_signature_help_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                           gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  DocState *doc = NULL;
  GtkWidget *anchor = NULL;
  if (!ide_lsp_take_popover_response(ctx, FALSE, &doc, &anchor)) {
    return;
  }
  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Signature help failed: %s", error_message);
    return;
  }

  guint active_sig = 0;
  gint active_param = -1;
  g_autoptr(GPtrArray) sigs = ide_lsp_parse_signature_help(result, &active_sig, &active_param);
  if (sigs->len == 0) {
    ide_lsp_report_message(mgr, "No signature help available here");
    return;
  }

  g_clear_pointer(&mgr->sig_items, g_ptr_array_unref);
  mgr->sig_items = g_steal_pointer(&sigs);
  mgr->sig_active_idx = active_sig;
  mgr->sig_active_param = active_param;
  present_signature_popover(mgr, anchor, doc->buf);
}

void
ide_lsp_manager_request_signature_help(IdeLspManager *lsp,
                                       GtkWidget     *anchor_view,
                                       IdeBuffer     *buf,
                                       guint          line_1based,
                                       guint          col_1based)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  if (buf == NULL) {
    return;
  }

  DocState *doc = ide_lsp_require_doc(lsp, buf, "signatureHelpProvider",
                                      "Signature help", "signature help");
  if (doc == NULL) {
    return;
  }

  ide_lsp_doc_send_change_now(doc);
  guint line0 = 0, utf16 = 0;
  ide_lsp_resolve_request_position(buf, line_1based, col_1based, &line0, &utf16);

  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_SIGNATURE, doc->uri);
  ctx->token = lsp->popover_token;
  ctx->had_anchor = (anchor_view != NULL);
  ide_lsp_ctx_set_anchor(ctx, anchor_view);
  ide_lsp_send_inflight_request(lsp, doc, ctx, "textDocument/signatureHelp",
                                ide_lsp_position_params(doc->uri, line0, utf16),
                                on_signature_help_response);
}

/* ═══════════════════════════ Call Hierarchy ═══════════════════════════ */

static void
on_call_hierarchy_toggle_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  IdeLspManager *lsp = user_data;
  if (lsp != NULL && !lsp->shutting_down) {
    ide_lsp_manager_toggle_call_hierarchy_dir(lsp);
  }
}

static void
present_call_hierarchy_popover(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf)
{
  close_popover_widgets(lsp);
  lsp->selected_comp_idx = 0;
  lsp->active_buf = buf;

  guint count = lsp->call_items != NULL ? lsp->call_items->len : 0;
  if (anchor_view != NULL) {
    GtkWidget *pop = ide_lsp_new_popover_for(lsp, anchor_view, buf);
    GtkWidget *card = popover_create_card(480, 6);

    GtkWidget *hdr_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hdr_row, GTK_ALIGN_CENTER);

    g_autofree char *hdr_txt = g_strdup_printf("CALL HIERARCHY — %s (%u)",
                                               lsp->call_outgoing ? "OUTGOING" : "INCOMING",
                                               count);
    GtkWidget *hdr = gtk_label_new(hdr_txt);
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
    gtk_widget_set_hexpand(hdr, TRUE);
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(hdr, "shell-popover-title");
    gtk_box_append(GTK_BOX(hdr_row), hdr);

    GtkWidget *toggle_btn = gtk_button_new_with_label(
        lsp->call_outgoing ? "⇄ SWITCH TO INCOMING" : "⇄ SWITCH TO OUTGOING");
    gtk_widget_set_valign(toggle_btn, GTK_ALIGN_CENTER);
    g_signal_connect(toggle_btn, "clicked",
                     G_CALLBACK(on_call_hierarchy_toggle_clicked), lsp);
    gtk_box_append(GTK_BOX(hdr_row), toggle_btn);
    gtk_box_append(GTK_BOX(card), hdr_row);

    if (count == 0) {
      popover_append_label(card, lsp->call_outgoing ? "No outgoing calls found."
                                                    : "No incoming calls found.",
                           "row-subtitle");
    } else {
      popover_create_list_box(lsp, TRUE);

      guint shown = MIN(count, 100u);
      for (guint i = 0; i < shown; i++) {
        const IdeLspCallEntry *ce = g_ptr_array_index(lsp->call_items, i);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_widget_add_css_class(row, "widget-row-card");
        gtk_widget_add_css_class(row, "ide-picker-row");

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

        GtkWidget *badge = gtk_label_new(ce->kind_badge);
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(badge, "shell-popover-badge");
        gtk_widget_add_css_class(badge, "ide-symbol-badge");
        gtk_box_append(GTK_BOX(hbox), badge);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(vbox, TRUE);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(top, GTK_ALIGN_CENTER);

        GtkWidget *name_lbl = gtk_label_new(ce->name);
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(name_lbl, TRUE);
        gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(name_lbl, "row-title");
        gtk_box_append(GTK_BOX(top), name_lbl);

        if (ce->detail != NULL && ce->detail[0] != '\0') {
          GtkWidget *det_lbl = gtk_label_new(ce->detail);
          gtk_label_set_ellipsize(GTK_LABEL(det_lbl), PANGO_ELLIPSIZE_END);
          gtk_label_set_max_width_chars(GTK_LABEL(det_lbl), 28);
          gtk_widget_set_valign(det_lbl, GTK_ALIGN_CENTER);
          gtk_widget_add_css_class(det_lbl, "row-subtitle");
          gtk_box_append(GTK_BOX(top), det_lbl);
        }
        gtk_box_append(GTK_BOX(vbox), top);

        g_autofree char *loc_txt = g_strdup_printf("%s:%u",
                                                   rel_path_for_display(lsp, ce->path),
                                                   ce->line_1based);
        GtkWidget *loc_lbl = gtk_label_new(loc_txt);
        gtk_label_set_xalign(GTK_LABEL(loc_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(loc_lbl), PANGO_ELLIPSIZE_START);
        gtk_widget_set_valign(loc_lbl, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(loc_lbl, "row-subtitle");
        gtk_box_append(GTK_BOX(vbox), loc_lbl);

        gtk_box_append(GTK_BOX(hbox), vbox);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
        gtk_list_box_append(GTK_LIST_BOX(lsp->comp_list_box), row);
      }

      popover_attach_scroller(lsp, card, 260);
    }

    popover_append_label(card, "[j/k: Select]   [Enter / 1-9: Jump]   [Esc: Close]",
                         "shell-popover-badge");

    gtk_popover_set_child(GTK_POPOVER(pop), card);
    gtk_popover_popup(GTK_POPOVER(pop));
  }

  set_ui_state_ex(lsp, IDE_LSP_UI_CALL_HIERARCHY, TRUE);
  update_comp_selection_ui(lsp);
}

static void
on_call_hierarchy_list_response(IdeLspClient *client, JsonNode *result, const char *error_message,
                                gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  gboolean outgoing = ctx->outgoing;
  DocState *doc = NULL;
  GtkWidget *anchor = NULL;
  if (!ide_lsp_take_popover_response(ctx, FALSE, &doc, &anchor)) {
    return;
  }
  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Call hierarchy failed: %s", error_message);
    return;
  }

  g_autoptr(GPtrArray) entries = ide_lsp_parse_call_hierarchy(result, outgoing);
  for (guint i = 0; i < entries->len; i++) {
    IdeLspCallEntry *ce = g_ptr_array_index(entries, i);
    ce->col_1based = resolve_open_or_disk_col(mgr, ce->path, ce->line_1based, ce->col_1based, NULL);
  }
  g_clear_pointer(&mgr->call_items, g_ptr_array_unref);
  mgr->call_items = g_steal_pointer(&entries);
  mgr->call_outgoing = outgoing;
  present_call_hierarchy_popover(mgr, anchor, doc->buf);
}

static void
send_call_hierarchy_list_request(IdeLspManager *mgr, DocState *doc, GtkWidget *anchor,
                                 gboolean had_anchor, gboolean outgoing)
{
  if (mgr->call_root_item == NULL || doc == NULL || doc->client == NULL) {
    return;
  }
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "item");
  json_builder_add_value(b, json_node_copy(mgr->call_root_item));
  json_builder_end_object(b);

  RequestCtx *ctx = ide_lsp_ctx_new(mgr, CTX_CALL_LIST, doc->uri);
  ctx->token = mgr->popover_token;
  ctx->had_anchor = had_anchor;
  ctx->outgoing = outgoing;
  ide_lsp_ctx_set_anchor(ctx, anchor);
  const char *method = outgoing ? "callHierarchy/outgoingCalls" : "callHierarchy/incomingCalls";
  ide_lsp_send_inflight_request(mgr, doc, ctx, method, json_builder_get_root(b),
                                on_call_hierarchy_list_response);
}

static void
on_call_hierarchy_prepare_response(IdeLspClient *client, JsonNode *result,
                                   const char *error_message, gpointer user_data)
{
  (void)client;
  RequestCtx *ctx = user_data;
  IdeLspManager *mgr = ctx->mgr;
  gboolean had_anchor = ctx->had_anchor;
  gboolean outgoing = ctx->outgoing;
  DocState *doc = NULL;
  GtkWidget *anchor = NULL;
  if (!ide_lsp_take_popover_response(ctx, FALSE, &doc, &anchor)) {
    return;
  }
  if (error_message != NULL) {
    ide_lsp_report_message(mgr, "Call hierarchy failed: %s", error_message);
    return;
  }

  JsonNode *first = NULL;
  if (result != NULL && JSON_NODE_HOLDS_ARRAY(result)) {
    JsonArray *arr = json_node_get_array(result);
    if (json_array_get_length(arr) > 0) {
      JsonNode *n0 = json_array_get_element(arr, 0);
      if (n0 != NULL && JSON_NODE_HOLDS_OBJECT(n0)) {
        first = n0;
      }
    }
  } else if (result != NULL && JSON_NODE_HOLDS_OBJECT(result)) {
    first = result;
  }
  if (first == NULL) {
    ide_lsp_report_message(mgr, "No call hierarchy symbol at cursor");
    return;
  }

  g_clear_pointer(&mgr->call_root_item, json_node_unref);
  mgr->call_root_item = json_node_copy(first);
  g_free(mgr->call_root_uri);
  mgr->call_root_uri = g_strdup(doc->uri);
  mgr->call_outgoing = outgoing;

  send_call_hierarchy_list_request(mgr, doc, anchor, had_anchor, outgoing);
}

void
ide_lsp_manager_request_call_hierarchy(IdeLspManager *lsp,
                                       GtkWidget     *anchor_view,
                                       IdeBuffer     *buf,
                                       gboolean       outgoing)
{
  g_return_if_fail(lsp != NULL);
  ide_lsp_manager_cancel_popover(lsp);
  if (buf == NULL) {
    return;
  }

  DocState *doc = ide_lsp_require_doc(lsp, buf, "callHierarchyProvider",
                                      "Call hierarchy", "call hierarchy");
  if (doc == NULL) {
    return;
  }

  ide_lsp_doc_send_change_now(doc);
  guint line0 = 0, utf16 = 0;
  ide_lsp_buffer_cursor_position(buf, &line0, &utf16);

  RequestCtx *ctx = ide_lsp_ctx_new(lsp, CTX_CALL_PREPARE, doc->uri);
  ctx->token = lsp->popover_token;
  ctx->had_anchor = (anchor_view != NULL);
  ctx->outgoing = outgoing;
  ide_lsp_ctx_set_anchor(ctx, anchor_view);
  ide_lsp_send_inflight_request(lsp, doc, ctx, "textDocument/prepareCallHierarchy",
                                ide_lsp_position_params(doc->uri, line0, utf16),
                                on_call_hierarchy_prepare_response);
}

void
ide_lsp_manager_toggle_call_hierarchy_dir(IdeLspManager *lsp)
{
  g_return_if_fail(lsp != NULL);
  if (lsp->call_root_item == NULL || lsp->call_root_uri == NULL) {
    return;
  }
  DocState *doc = g_hash_table_lookup(lsp->docs_by_uri, lsp->call_root_uri);
  if (doc == NULL || doc->client == NULL ||
      ide_lsp_client_get_state(doc->client) == IDE_LSP_CLIENT_STOPPED) {
    return;
  }

  GtkWidget *anchor = lsp->active_anchor;
  gboolean had_anchor = (anchor != NULL);
  gboolean next_outgoing = !lsp->call_outgoing;

  ide_lsp_cancel_inflight(lsp);
  lsp->popover_token++;
  send_call_hierarchy_list_request(lsp, doc, anchor, had_anchor, next_outgoing);
}
