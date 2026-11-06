#include "ide_agent_ipc.h"
#include <gio/gunixsocketaddress.h>
#include <gtksourceview/gtksource.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct _IdeAgentIpc {
  IdeAgentOpenCb   open_cb;
  IdeAgentReloadCb reload_cb;
  IdeAgentStatusCb status_cb;
  gpointer         user_data;

  GSocketService  *service;
  GPtrArray       *socket_paths; /* char* */
  GPtrArray       *tx_items;     /* IdeAgentTxItem* */
  char            *active_badge;

  /* Review Drawer Widgets */
  GtkWidget       *drawer_box;
  GtkWidget       *count_badge;
  GtkWidget       *file_list;
  GtkSourceBuffer *diff_buffer;
  GtkWidget       *diff_view;
};

static void tx_item_free(gpointer data) {
  IdeAgentTxItem *item = data;
  if (!item) return;
  g_free(item->path);
  g_free(item->old_content);
  g_free(item->new_content);
  g_free(item->unified_diff);
  g_free(item);
}

static void count_diff_stats(const char *old_text,
                             const char *new_text,
                             guint      *out_added,
                             guint      *out_removed) {
  guint add = 0, rem = 0;
  gsize n_lines = 0;
  IdeDiffState *diffs = ide_compute_line_diffs(old_text ? old_text : "",
                                               new_text ? new_text : "",
                                               &n_lines);
  if (diffs) {
    for (gsize i = 0; i < n_lines; i++) {
      if (diffs[i] == IDE_DIFF_ADDED)
        add++;
      else if (diffs[i] == IDE_DIFF_MODIFIED) {
        add++;
        rem++;
      } else if (diffs[i] == IDE_DIFF_DELETED)
        rem++;
    }
    g_free(diffs);
  }
  if (out_added) *out_added = add;
  if (out_removed) *out_removed = rem;
}

static void refresh_drawer_ui(IdeAgentIpc *ipc) {
  if (!ipc || !ipc->drawer_box) return;

  g_autofree char *badge_txt = g_strdup_printf("%u FILES MODIFIED", ipc->tx_items->len);
  gtk_label_set_text(GTK_LABEL(ipc->count_badge), badge_txt);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(ipc->file_list)) != NULL)
    gtk_list_box_remove(GTK_LIST_BOX(ipc->file_list), child);

  for (guint i = 0; i < ipc->tx_items->len; i++) {
    IdeAgentTxItem *item = g_ptr_array_index(ipc->tx_items, i);
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "tx-index", GUINT_TO_POINTER(i + 1));

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(hbox, 10);
    gtk_widget_set_margin_end(hbox, 10);
    gtk_widget_set_margin_top(hbox, 6);
    gtk_widget_set_margin_bottom(hbox, 6);

    GtkWidget *m_lbl = gtk_label_new("M");
    gtk_widget_add_css_class(m_lbl, "ide-git-modified");
    gtk_widget_set_valign(m_lbl, GTK_ALIGN_CENTER);

    g_autofree char *base = g_path_get_basename(item->path);
    GtkWidget *path_lbl = gtk_label_new(base);
    gtk_widget_set_hexpand(path_lbl, TRUE);
    gtk_widget_set_halign(path_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(path_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(path_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_tooltip_text(path_lbl, item->path);

    g_autofree char *stat_str =
        g_strdup_printf("+%u -%u", item->added_lines, item->removed_lines);
    GtkWidget *stat_lbl = gtk_label_new(stat_str);
    gtk_widget_add_css_class(stat_lbl, "shell-popover-badge");
    gtk_widget_set_valign(stat_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hbox), m_lbl);
    gtk_box_append(GTK_BOX(hbox), path_lbl);
    gtk_box_append(GTK_BOX(hbox), stat_lbl);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(GTK_LIST_BOX(ipc->file_list), row);
  }

  if (ipc->tx_items->len > 0) {
    IdeAgentTxItem *first = g_ptr_array_index(ipc->tx_items, 0);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(ipc->diff_buffer),
                             first->unified_diff ? first->unified_diff : "",
                             -1);
    GtkListBoxRow *first_row =
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(ipc->file_list), 0);
    if (first_row)
      gtk_list_box_select_row(GTK_LIST_BOX(ipc->file_list), first_row);
  } else {
    gtk_text_buffer_set_text(
        GTK_TEXT_BUFFER(ipc->diff_buffer),
        "# No pending agent transactions.\n"
        "# External AI agents can stream edits via koodi-cli or $XDG_RUNTIME_DIR/kajo/ide.sock.",
        -1);
  }
}

static void on_tx_row_selected(GtkListBox    *box,
                               GtkListBoxRow *row,
                               gpointer       user_data) {
  (void)box;
  IdeAgentIpc *ipc = user_data;
  if (!row || !ipc || !ipc->diff_buffer) return;
  guint idx_plus1 = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "tx-index"));
  if (idx_plus1 == 0 || idx_plus1 > ipc->tx_items->len) return;
  IdeAgentTxItem *item = g_ptr_array_index(ipc->tx_items, idx_plus1 - 1);
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(ipc->diff_buffer),
                           item->unified_diff ? item->unified_diff : "",
                           -1);
}

static void on_accept_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  IdeAgentIpc *ipc = user_data;
  ide_agent_ipc_accept_all(ipc);
}

static void on_reject_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  IdeAgentIpc *ipc = user_data;
  ide_agent_ipc_reject_all(ipc);
}

void ide_agent_ipc_record_edit(IdeAgentIpc *ipc,
                               const char  *path,
                               const char  *old_content,
                               const char  *new_content) {
  if (!ipc || !path) return;

  /* Check if file is already in transaction list; if so, preserve initial old_content */
  for (guint i = 0; i < ipc->tx_items->len; i++) {
    IdeAgentTxItem *existing = g_ptr_array_index(ipc->tx_items, i);
    if (g_strcmp0(existing->path, path) == 0) {
      g_free(existing->new_content);
      existing->new_content = g_strdup(new_content ? new_content : "");
      g_free(existing->unified_diff);
      existing->unified_diff =
          ide_format_unified_diff(path, existing->old_content, existing->new_content);
      count_diff_stats(existing->old_content,
                       existing->new_content,
                       &existing->added_lines,
                       &existing->removed_lines);
      refresh_drawer_ui(ipc);
      if (ipc->reload_cb)
        ipc->reload_cb(path, existing->old_content, ipc->user_data);
      if (ipc->status_cb)
        ipc->status_cb(ipc->active_badge, NULL, ipc->tx_items->len, ipc->user_data);
      return;
    }
  }

  IdeAgentTxItem *item = g_new0(IdeAgentTxItem, 1);
  item->path = g_strdup(path);
  item->old_content = g_strdup(old_content ? old_content : "");
  item->new_content = g_strdup(new_content ? new_content : "");
  item->unified_diff =
      ide_format_unified_diff(path, item->old_content, item->new_content);
  count_diff_stats(item->old_content,
                   item->new_content,
                   &item->added_lines,
                   &item->removed_lines);
  g_ptr_array_add(ipc->tx_items, item);

  refresh_drawer_ui(ipc);
  if (ipc->reload_cb)
    ipc->reload_cb(path, item->old_content, ipc->user_data);
  if (ipc->status_cb)
    ipc->status_cb(ipc->active_badge, NULL, ipc->tx_items->len, ipc->user_data);
}

guint ide_agent_ipc_get_pending_count(const IdeAgentIpc *ipc) {
  return ipc ? ipc->tx_items->len : 0;
}

void ide_agent_ipc_accept_all(IdeAgentIpc *ipc) {
  if (!ipc) return;
  guint count = ipc->tx_items->len;
  for (guint i = 0; i < ipc->tx_items->len; i++) {
    IdeAgentTxItem *item = g_ptr_array_index(ipc->tx_items, i);
    if (ipc->reload_cb)
      ipc->reload_cb(item->path, NULL, ipc->user_data);
  }
  g_ptr_array_set_size(ipc->tx_items, 0);
  refresh_drawer_ui(ipc);

  if (ipc->status_cb) {
    g_autofree char *msg =
        g_strdup_printf("Accepted %u agent file transaction(s)", count);
    ipc->status_cb(ipc->active_badge, msg, 0, ipc->user_data);
  }
}

void ide_agent_ipc_reject_all(IdeAgentIpc *ipc) {
  if (!ipc) return;
  guint count = ipc->tx_items->len;
  for (guint i = 0; i < ipc->tx_items->len; i++) {
    IdeAgentTxItem *item = g_ptr_array_index(ipc->tx_items, i);
    if (item->path && item->old_content) {
      ide_save_file_atomic(item->path,
                           item->old_content,
                           strlen(item->old_content),
                           NULL);
      if (ipc->reload_cb)
        ipc->reload_cb(item->path, NULL, ipc->user_data);
    }
  }
  g_ptr_array_set_size(ipc->tx_items, 0);
  refresh_drawer_ui(ipc);

  if (ipc->status_cb) {
    g_autofree char *msg =
        g_strdup_printf("Reverted %u agent file transaction(s)", count);
    ipc->status_cb(ipc->active_badge, msg, 0, ipc->user_data);
  }
}

char *ide_agent_ipc_dispatch_json(IdeAgentIpc *ipc, const char *json_request) {
  if (!ipc || !json_request || !*json_request)
    return g_strdup("{\"ok\":false,\"error\":\"Empty request\"}\n");

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) err = NULL;
  if (!json_parser_load_from_data(parser, json_request, -1, &err)) {
    return g_strdup_printf("{\"ok\":false,\"error\":\"Invalid JSON: %s\"}\n",
                           err ? err->message : "parse error");
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root))
    return g_strdup("{\"ok\":false,\"error\":\"Root must be JSON object\"}\n");

  JsonObject *obj = json_node_get_object(root);
  const char *type = json_object_get_string_member_with_default(obj, "type", "");
  if (!*type)
    type = json_object_get_string_member_with_default(obj, "cmd", "");

  if (g_strcmp0(type, "open") == 0) {
    const char *path = json_object_get_string_member_with_default(obj, "path", "");
    guint line = (guint)json_object_get_int_member_with_default(obj, "line", 1);
    guint col = (guint)json_object_get_int_member_with_default(obj, "col", 1);
    if (!*path)
      return g_strdup("{\"ok\":false,\"error\":\"Missing path\"}\n");
    if (ipc->open_cb)
      ipc->open_cb(path, line ? line : 1, col ? col : 1, ipc->user_data);
    return g_strdup("{\"ok\":true,\"type\":\"open\"}\n");
  }

  if (g_strcmp0(type, "diff") == 0) {
    const char *path = json_object_get_string_member_with_default(obj, "path", "");
    const char *old_c =
        json_object_get_string_member_with_default(obj, "old_content", NULL);
    const char *new_c =
        json_object_get_string_member_with_default(obj, "new_content", NULL);
    if (!*path)
      return g_strdup("{\"ok\":false,\"error\":\"Missing path\"}\n");

    g_autofree char *disk_old = NULL;
    if (!old_c) {
      g_file_get_contents(path, &disk_old, NULL, NULL);
      old_c = disk_old ? disk_old : "";
    }
    g_autofree char *disk_new = NULL;
    if (!new_c) {
      g_file_get_contents(path, &disk_new, NULL, NULL);
      new_c = disk_new ? disk_new : "";
    }

    ide_agent_ipc_record_edit(ipc, path, old_c, new_c);
    return g_strdup_printf("{\"ok\":true,\"type\":\"diff\",\"pending_files\":%u}\n",
                           ipc->tx_items->len);
  }

  if (g_strcmp0(type, "reload") == 0) {
    const char *path = json_object_get_string_member_with_default(obj, "path", "");
    if (ipc->reload_cb)
      ipc->reload_cb(path, NULL, ipc->user_data);
    return g_strdup("{\"ok\":true,\"type\":\"reload\"}\n");
  }

  if (g_strcmp0(type, "notify") == 0) {
    const char *msg = json_object_get_string_member_with_default(obj, "message", "");
    if (ipc->status_cb)
      ipc->status_cb(ipc->active_badge, msg, ipc->tx_items->len, ipc->user_data);
    return g_strdup("{\"ok\":true,\"type\":\"notify\"}\n");
  }

  if (g_strcmp0(type, "status-badge") == 0) {
    const char *text = json_object_get_string_member_with_default(obj, "text", "");
    gboolean active = json_object_get_boolean_member_with_default(obj, "active", TRUE);
    g_free(ipc->active_badge);
    ipc->active_badge = (active && *text) ? g_strdup(text) : NULL;
    if (ipc->status_cb)
      ipc->status_cb(ipc->active_badge, NULL, ipc->tx_items->len, ipc->user_data);
    return g_strdup("{\"ok\":true,\"type\":\"status-badge\"}\n");
  }

  if (g_strcmp0(type, "action") == 0) {
    const char *act = json_object_get_string_member_with_default(obj, "action", "");
    if (g_strcmp0(act, "agent_accept_all") == 0 || g_strcmp0(act, "accept_all") == 0) {
      ide_agent_ipc_accept_all(ipc);
      return g_strdup("{\"ok\":true,\"action\":\"agent_accept_all\"}\n");
    }
    if (g_strcmp0(act, "agent_reject_all") == 0 || g_strcmp0(act, "reject_all") == 0) {
      ide_agent_ipc_reject_all(ipc);
      return g_strdup("{\"ok\":true,\"action\":\"agent_reject_all\"}\n");
    }
    return g_strdup_printf("{\"ok\":false,\"error\":\"Unknown action: %s\"}\n", act);
  }

  return g_strdup_printf("{\"ok\":false,\"error\":\"Unknown command type: %s\"}\n",
                         type);
}

static gboolean on_incoming_connection(GSocketService    *service,
                                       GSocketConnection *connection,
                                       GObject           *source_object,
                                       gpointer           user_data) {
  (void)service;
  (void)source_object;
  IdeAgentIpc *ipc = user_data;

  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

  g_autoptr(GDataInputStream) data_in = g_data_input_stream_new(in);
  gsize len = 0;
  g_autofree char *line =
      g_data_input_stream_read_line(data_in, &len, NULL, NULL);
  if (line) {
    g_autofree char *resp = ide_agent_ipc_dispatch_json(ipc, line);
    if (resp) {
      g_output_stream_write_all(out, resp, strlen(resp), NULL, NULL, NULL);
      g_output_stream_flush(out, NULL, NULL);
    }
  }
  return TRUE;
}

gboolean ide_agent_ipc_start_on_path(IdeAgentIpc *ipc,
                                     const char  *socket_path,
                                     GError     **error) {
  if (!ipc || !socket_path || !*socket_path) return FALSE;

  g_autofree char *dir = g_path_get_dirname(socket_path);
  g_mkdir_with_parents(dir, 0700);
  unlink(socket_path);

  if (!ipc->service) {
    ipc->service = g_socket_service_new();
    g_signal_connect(ipc->service,
                     "incoming",
                     G_CALLBACK(on_incoming_connection),
                     ipc);
  }

  g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(socket_path);
  if (!g_socket_listener_add_address(G_SOCKET_LISTENER(ipc->service),
                                     addr,
                                     G_SOCKET_TYPE_STREAM,
                                     G_SOCKET_PROTOCOL_DEFAULT,
                                     NULL,
                                     NULL,
                                     error)) {
    return FALSE;
  }

  chmod(socket_path, 0600);
  g_ptr_array_add(ipc->socket_paths, g_strdup(socket_path));
  g_socket_service_start(ipc->service);
  return TRUE;
}

gboolean ide_agent_ipc_start_default(IdeAgentIpc *ipc) {
  if (!ipc) return FALSE;
  const char *runtime_dir = g_get_user_runtime_dir();
  if (!runtime_dir || !*runtime_dir) return FALSE;

  g_autofree char *kajo_sock =
      g_build_filename(runtime_dir, "kajo", "ide.sock", NULL);
  g_autofree char *koodi_sock =
      g_build_filename(runtime_dir, "koodi", "koodi-default.sock", NULL);

  gboolean ok1 = ide_agent_ipc_start_on_path(ipc, kajo_sock, NULL);
  gboolean ok2 = ide_agent_ipc_start_on_path(ipc, koodi_sock, NULL);
  return ok1 || ok2;
}

GtkWidget *ide_agent_ipc_get_drawer_widget(IdeAgentIpc *ipc) {
  if (!ipc) return NULL;
  if (ipc->drawer_box) return ipc->drawer_box;

  ipc->drawer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(ipc->drawer_box, "ide-drawer-panel");
  gtk_widget_set_size_request(ipc->drawer_box, -1, 230);

  /* Header Bar */
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "ide-drawer-header");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("🤖 AGENT TRANSACTION REVIEW (Space d s)");
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

  ipc->count_badge = gtk_label_new("0 FILES MODIFIED");
  gtk_widget_add_css_class(ipc->count_badge, "shell-popover-badge");
  gtk_widget_set_valign(ipc->count_badge, GTK_ALIGN_CENTER);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  GtkWidget *accept_btn = gtk_button_new_with_label("✓ ACCEPT ALL (Space d a)");
  gtk_widget_add_css_class(accept_btn, "ide-btn-accept");
  gtk_widget_set_valign(accept_btn, GTK_ALIGN_CENTER);
  g_signal_connect(accept_btn, "clicked", G_CALLBACK(on_accept_clicked), ipc);

  GtkWidget *reject_btn = gtk_button_new_with_label("✕ REVERT ALL (Space d r)");
  gtk_widget_add_css_class(reject_btn, "ide-btn-reject");
  gtk_widget_set_valign(reject_btn, GTK_ALIGN_CENTER);
  g_signal_connect(reject_btn, "clicked", G_CALLBACK(on_reject_clicked), ipc);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), ipc->count_badge);
  gtk_box_append(GTK_BOX(hdr), spacer);
  gtk_box_append(GTK_BOX(hdr), accept_btn);
  gtk_box_append(GTK_BOX(hdr), reject_btn);
  gtk_box_append(GTK_BOX(ipc->drawer_box), hdr);

  /* 2-Column Split: Left = Modified files list, Right = Unified Diff View */
  GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand(split, TRUE);

  GtkWidget *left_scroll = gtk_scrolled_window_new();
  gtk_widget_set_size_request(left_scroll, 260, -1);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_scroll),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);

  ipc->file_list = gtk_list_box_new();
  gtk_widget_add_css_class(ipc->file_list, "ide-tree-sidebar");
  g_signal_connect(ipc->file_list,
                   "row-selected",
                   G_CALLBACK(on_tx_row_selected),
                   ipc);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(left_scroll), ipc->file_list);

  GtkWidget *right_scroll = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(right_scroll, TRUE);
  gtk_widget_set_vexpand(right_scroll, TRUE);

  ipc->diff_buffer = gtk_source_buffer_new(NULL);
  GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
  GtkSourceLanguage *diff_lang = gtk_source_language_manager_get_language(lm, "diff");
  if (diff_lang)
    gtk_source_buffer_set_language(ipc->diff_buffer, diff_lang);

  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  GtkSourceStyleScheme *scheme =
      gtk_source_style_scheme_manager_get_scheme(sm, "kajo-metro-dark");
  if (scheme)
    gtk_source_buffer_set_style_scheme(ipc->diff_buffer, scheme);

  ipc->diff_view = gtk_source_view_new_with_buffer(ipc->diff_buffer);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(ipc->diff_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(ipc->diff_view), TRUE);
  gtk_widget_add_css_class(ipc->diff_view, "ide-source-view");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(right_scroll), ipc->diff_view);

  gtk_box_append(GTK_BOX(split), left_scroll);
  gtk_box_append(GTK_BOX(split), right_scroll);
  gtk_box_append(GTK_BOX(ipc->drawer_box), split);

  refresh_drawer_ui(ipc);
  return ipc->drawer_box;
}

IdeAgentIpc *ide_agent_ipc_new(IdeAgentOpenCb   open_cb,
                               IdeAgentReloadCb reload_cb,
                               IdeAgentStatusCb status_cb,
                               gpointer         user_data) {
  IdeAgentIpc *ipc = g_new0(IdeAgentIpc, 1);
  ipc->open_cb = open_cb;
  ipc->reload_cb = reload_cb;
  ipc->status_cb = status_cb;
  ipc->user_data = user_data;
  ipc->socket_paths = g_ptr_array_new_with_free_func(g_free);
  ipc->tx_items = g_ptr_array_new_with_free_func(tx_item_free);
  return ipc;
}

void ide_agent_ipc_free(IdeAgentIpc *ipc) {
  if (!ipc) return;
  if (ipc->service) {
    g_socket_service_stop(ipc->service);
    g_socket_listener_close(G_SOCKET_LISTENER(ipc->service));
    g_clear_object(&ipc->service);
  }
  if (ipc->socket_paths) {
    for (guint i = 0; i < ipc->socket_paths->len; i++) {
      const char *p = g_ptr_array_index(ipc->socket_paths, i);
      if (p) unlink(p);
    }
    g_ptr_array_free(ipc->socket_paths, TRUE);
  }
  if (ipc->tx_items) g_ptr_array_free(ipc->tx_items, TRUE);
  g_free(ipc->active_badge);
  g_clear_object(&ipc->diff_buffer);
  g_free(ipc);
}
