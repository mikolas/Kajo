#include "ide_dap.h"

#include <string.h>

struct _IdeDapView {
  IdeDapActionCb action_cb;
  gpointer       cb_data;

  GtkWidget     *root_box;
  GtkWidget     *status_badge;
  GtkWidget     *adapter_badge;
  GtkWidget     *stack_list_box;
  GtkWidget     *locals_list_box;
  GtkWidget     *console_view;
  GtkTextBuffer *console_buf;
  GtkWidget     *repl_entry;

  gboolean       running;
  guint          current_pc_line;
};

const char *
ide_dap_detect_adapter_for_file(const char *file_path)
{
  if (file_path == NULL || file_path[0] == '\0') {
    return "gdb -i=dap";
  }
  if (g_str_has_suffix(file_path, ".go")) {
    return "dlv dap";
  }
  if (g_str_has_suffix(file_path, ".rs") || g_str_has_suffix(file_path, ".cpp") ||
      g_str_has_suffix(file_path, ".c") || g_str_has_suffix(file_path, ".h")) {
    return "gdb -i=dap";
  }
  if (g_str_has_suffix(file_path, ".py")) {
    return "python3 -m debugpy.adapter";
  }
  return "gdb -i=dap";
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
dap_populate_rows(IdeDapView *dap, IdeBuffer *buf)
{
  GtkWidget *child = gtk_widget_get_first_child(dap->stack_list_box);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(dap->stack_list_box), child);
    child = next;
  }
  child = gtk_widget_get_first_child(dap->locals_list_box);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(dap->locals_list_box), child);
    child = next;
  }

  if (!dap->running || buf == NULL) {
    return;
  }

  g_autofree char *f0 = g_strdup_printf("▶ #0 %s:%u",
                                        ide_buffer_get_title(buf),
                                        dap->current_pc_line);
  GtkWidget *r0 = gtk_list_box_row_new();
  gtk_widget_add_css_class(r0, "widget-row-card");
  GtkWidget *l0 = gtk_label_new(f0);
  gtk_label_set_xalign(GTK_LABEL(l0), 0.0f);
  gtk_widget_add_css_class(l0, "row-title");
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r0), l0);
  gtk_list_box_append(GTK_LIST_BOX(dap->stack_list_box), r0);
  gtk_list_box_select_row(GTK_LIST_BOX(dap->stack_list_box), GTK_LIST_BOX_ROW(r0));

  g_autoptr(GArray) bps = ide_buffer_get_breakpoints(buf);
  g_autofree char *v0 = g_strdup_printf("active_breakpoints = %u", bps ? bps->len : 0u);
  GtkWidget *vr0 = gtk_list_box_row_new();
  gtk_widget_add_css_class(vr0, "widget-row-card");
  GtkWidget *vl0 = gtk_label_new(v0);
  gtk_label_set_xalign(GTK_LABEL(vl0), 0.0f);
  gtk_widget_add_css_class(vl0, "row-subtitle");
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(vr0), vl0);
  gtk_list_box_append(GTK_LIST_BOX(dap->locals_list_box), vr0);

  g_autofree char *v1 = g_strdup_printf("pc_line = %u", dap->current_pc_line);
  GtkWidget *vr1 = gtk_list_box_row_new();
  gtk_widget_add_css_class(vr1, "widget-row-card");
  GtkWidget *vl1 = gtk_label_new(v1);
  gtk_label_set_xalign(GTK_LABEL(vl1), 0.0f);
  gtk_widget_add_css_class(vl1, "row-subtitle");
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(vr1), vl1);
  gtk_list_box_append(GTK_LIST_BOX(dap->locals_list_box), vr1);
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
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

IdeDapView *
ide_dap_view_new(IdeDapActionCb action_cb, gpointer user_data)
{
  IdeDapView *dap = g_new0(IdeDapView, 1);
  dap->action_cb = action_cb;
  dap->cb_data = user_data;

  dap->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(dap->root_box, -1, 210);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "settings-header-bar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("🐞 DAP DEBUGGER INSPECTOR (Space d v) — [F5: Continue | F10: Over | F11: Into | F12: Out]");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(title, "settings-header-title");
  gtk_box_append(GTK_BOX(hdr), title);

  dap->status_badge = gtk_label_new("IDLE");
  gtk_widget_set_valign(dap->status_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(dap->status_badge, "shell-popover-badge");
  gtk_box_append(GTK_BOX(hdr), dap->status_badge);

  dap->adapter_badge = gtk_label_new("gdb -i=dap");
  gtk_widget_set_valign(dap->adapter_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(dap->adapter_badge, "shell-popover-badge");
  gtk_box_append(GTK_BOX(hdr), dap->adapter_badge);
  gtk_box_append(GTK_BOX(dap->root_box), hdr);

  /* 3-Column Horizontal Split: Call Stack | Locals & Scopes | Debug Console & REPL */
  GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_vexpand(cols, TRUE);
  gtk_widget_set_margin_start(cols, 8);
  gtk_widget_set_margin_end(cols, 8);
  gtk_widget_set_margin_top(cols, 6);
  gtk_widget_set_margin_bottom(cols, 6);

  /* Col 1: Call Stack */
  GtkWidget *c1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(c1, TRUE);
  GtkWidget *c1_lbl = gtk_label_new("CALL STACK");
  gtk_label_set_xalign(GTK_LABEL(c1_lbl), 0.0f);
  gtk_widget_add_css_class(c1_lbl, "sidebar-category-header");
  gtk_box_append(GTK_BOX(c1), c1_lbl);
  GtkWidget *s1 = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(s1, TRUE);
  dap->stack_list_box = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s1), dap->stack_list_box);
  gtk_box_append(GTK_BOX(c1), s1);
  gtk_box_append(GTK_BOX(cols), c1);

  /* Col 2: Locals & Scopes */
  GtkWidget *c2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(c2, TRUE);
  GtkWidget *c2_lbl = gtk_label_new("LOCALS & SCOPES");
  gtk_label_set_xalign(GTK_LABEL(c2_lbl), 0.0f);
  gtk_widget_add_css_class(c2_lbl, "sidebar-category-header");
  gtk_box_append(GTK_BOX(c2), c2_lbl);
  GtkWidget *s2 = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(s2, TRUE);
  dap->locals_list_box = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s2), dap->locals_list_box);
  gtk_box_append(GTK_BOX(c2), s2);
  gtk_box_append(GTK_BOX(cols), c2);

  /* Col 3: Debug Output & REPL */
  GtkWidget *c3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(c3, TRUE);
  GtkWidget *c3_lbl = gtk_label_new("DEBUG CONSOLE & REPL");
  gtk_label_set_xalign(GTK_LABEL(c3_lbl), 0.0f);
  gtk_widget_add_css_class(c3_lbl, "sidebar-category-header");
  gtk_box_append(GTK_BOX(c3), c3_lbl);

  GtkWidget *s3 = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(s3, TRUE);
  dap->console_buf = gtk_text_buffer_new(NULL);
  dap->console_view = gtk_text_view_new_with_buffer(dap->console_buf);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(dap->console_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(dap->console_view), TRUE);
  gtk_widget_add_css_class(dap->console_view, "code-preview-box");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s3), dap->console_view);
  gtk_box_append(GTK_BOX(c3), s3);

  dap->repl_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(dap->repl_entry), "Evaluate expression (p expr)...");
  gtk_widget_add_css_class(dap->repl_entry, "shell-launcher-input");
  g_signal_connect(dap->repl_entry, "activate", G_CALLBACK(on_repl_activate), dap);
  gtk_box_append(GTK_BOX(c3), dap->repl_entry);

  gtk_box_append(GTK_BOX(cols), c3);
  gtk_box_append(GTK_BOX(dap->root_box), cols);

  return dap;
}

void
ide_dap_view_free(IdeDapView *dap)
{
  if (dap == NULL) {
    return;
  }
  g_clear_object(&dap->console_buf);
  g_free(dap);
}

GtkWidget *
ide_dap_view_get_widget(IdeDapView *dap)
{
  g_return_val_if_fail(dap != NULL, NULL);
  return dap->root_box;
}

void
ide_dap_view_start_session(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  const char *adapter = ide_dap_detect_adapter_for_file(
      active_buf ? ide_buffer_get_file_path(active_buf) : NULL);
  gtk_label_set_text(GTK_LABEL(dap->adapter_badge), adapter);
  gtk_label_set_text(GTK_LABEL(dap->status_badge), "PAUSED AT BREAKPOINT");

  dap->running = TRUE;
  dap->current_pc_line = 1;
  if (active_buf != NULL) {
    g_autoptr(GArray) bps = ide_buffer_get_breakpoints(active_buf);
    if (bps != NULL && bps->len > 0) {
      dap->current_pc_line = g_array_index(bps, guint, 0);
    }
    ide_buffer_set_dap_pc_line(active_buf, dap->current_pc_line);
  }
  dap_append_console(dap, "[dap] Debug session started; stopped at initial breakpoint.");
  dap_populate_rows(dap, active_buf);
}

void
ide_dap_view_continue(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  if (!dap->running) {
    ide_dap_view_start_session(dap, active_buf);
    return;
  }
  if (active_buf != NULL) {
    g_autoptr(GArray) bps = ide_buffer_get_breakpoints(active_buf);
    if (bps != NULL && bps->len > 0) {
      for (guint i = 0; i < bps->len; i++) {
        guint ln = g_array_index(bps, guint, i);
        if (ln > dap->current_pc_line) {
          dap->current_pc_line = ln;
          break;
        }
      }
      ide_buffer_set_dap_pc_line(active_buf, dap->current_pc_line);
    }
  }
  dap_append_console(dap, "[dap] Continued to next breakpoint.");
  dap_populate_rows(dap, active_buf);
}

void
ide_dap_view_step_over(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  if (!dap->running) {
    return;
  }
  dap->current_pc_line++;
  if (active_buf != NULL) {
    ide_buffer_set_dap_pc_line(active_buf, dap->current_pc_line);
  }
  dap_append_console(dap, "[dap] Step Over (F10)");
  dap_populate_rows(dap, active_buf);
}

void
ide_dap_view_step_into(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  if (!dap->running) {
    return;
  }
  dap->current_pc_line++;
  if (active_buf != NULL) {
    ide_buffer_set_dap_pc_line(active_buf, dap->current_pc_line);
  }
  dap_append_console(dap, "[dap] Step Into (F11)");
  dap_populate_rows(dap, active_buf);
}

void
ide_dap_view_step_out(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  if (!dap->running) {
    return;
  }
  if (dap->current_pc_line > 1) {
    dap->current_pc_line--;
  }
  if (active_buf != NULL) {
    ide_buffer_set_dap_pc_line(active_buf, dap->current_pc_line);
  }
  dap_append_console(dap, "[dap] Step Out (F12)");
  dap_populate_rows(dap, active_buf);
}

void
ide_dap_view_pause(IdeDapView *dap)
{
  g_return_if_fail(dap != NULL);
  gtk_label_set_text(GTK_LABEL(dap->status_badge), "PAUSED");
  dap_append_console(dap, "[dap] Execution paused.");
}

void
ide_dap_view_stop(IdeDapView *dap, IdeBuffer *active_buf)
{
  g_return_if_fail(dap != NULL);
  dap->running = FALSE;
  dap->current_pc_line = 0;
  if (active_buf != NULL) {
    ide_buffer_set_dap_pc_line(active_buf, 0);
  }
  gtk_label_set_text(GTK_LABEL(dap->status_badge), "TERMINATED");
  dap_append_console(dap, "[dap] Debug session terminated.");
  dap_populate_rows(dap, active_buf);
}

gboolean
ide_dap_view_is_running(const IdeDapView *dap)
{
  g_return_val_if_fail(dap != NULL, FALSE);
  return dap->running;
}
