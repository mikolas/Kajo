#include "amiga_window_private.h"
#include <string.h>

static bool
same_diag_file(const char *a, const char *b)
{
  if (!a || !b) return false;
  if (g_strcmp0(a, b) == 0) return true;
  g_autofree char *ba = g_path_get_basename(a);
  g_autofree char *bb = g_path_get_basename(b);
  return g_strcmp0(ba, bb) == 0;
}

static void
rebuild_diag_list(KajoAmigaWindow *self)
{
  if (!self->diag_list) return;
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->diag_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->diag_list), ch);
  }
  int err_cnt = 0, warn_cnt = 0;
  for (guint i = 0; self->diags && i < self->diags->len; i++) {
    AmigaBuildDiag *d = g_ptr_array_index(self->diags, i);
    if (d->severity == AMIGA_DIAG_WARNING) warn_cnt++;
    else err_cnt++;

    if (self->diag_filter == 1 && d->severity == AMIGA_DIAG_WARNING) continue;
    if (self->diag_filter == 2 && d->severity != AMIGA_DIAG_WARNING) continue;

    g_autofree char *line = g_strdup_printf("%s:%u:%u  %s",
                                            d->file ? d->file : "?", d->line, d->col,
                                            d->message ? d->message : "");
    GtkWidget *l = gtk_label_new(line);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(
        l, d->severity == AMIGA_DIAG_WARNING ? "amiga-diag-warn" : "amiga-diag-error");
    gtk_list_box_append(GTK_LIST_BOX(self->diag_list), l);
    int idx_row = -1;
    GtkWidget *last = gtk_widget_get_last_child(self->diag_list);
    if (GTK_IS_LIST_BOX_ROW(last)) {
      idx_row = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(last));
      g_object_set_data(G_OBJECT(last), "diag-idx", GUINT_TO_POINTER(i));
    }
    (void)idx_row;
  }

  if (self->btn_diag_all) {
    g_autofree char *s = g_strdup_printf("ALL (%u)", self->diags ? self->diags->len : 0u);
    gtk_button_set_label(GTK_BUTTON(self->btn_diag_all), s);
  }
  if (self->btn_diag_err) {
    g_autofree char *s = g_strdup_printf("ERRORS (%d)", err_cnt);
    gtk_button_set_label(GTK_BUTTON(self->btn_diag_err), s);
  }
  if (self->btn_diag_warn) {
    g_autofree char *s = g_strdup_printf("WARNINGS (%d)", warn_cnt);
    gtk_button_set_label(GTK_BUTTON(self->btn_diag_warn), s);
  }
  amiga_window_sync_status_bar(self);
}

static void
sync_diags_to_open_buffers(KajoAmigaWindow *self)
{
  for (guint bi = 0; self->open_bufs && bi < self->open_bufs->len; bi++) {
    IdeBuffer *buf = g_ptr_array_index(self->open_bufs, bi);
    const char *bpath = ide_buffer_get_file_path(buf);
    ide_buffer_clear_diagnostics(buf);
    for (guint di = 0; self->diags && di < self->diags->len; di++) {
      const AmigaBuildDiag *d = g_ptr_array_index(self->diags, di);
      if (!d->file || !same_diag_file(bpath, d->file)) continue;
      ide_buffer_add_diagnostic(
          buf, d->line ? d->line : 1u, d->col,
          d->severity == AMIGA_DIAG_WARNING ? IDE_DIAG_WARNING : IDE_DIAG_ERROR,
          d->message, "amiga-build");
    }
  }
  if (self->source_view) gtk_widget_queue_draw(self->source_view);
}

static void
activate_diag_by_index(KajoAmigaWindow *self, guint idx)
{
  if (!self->diags || idx >= self->diags->len) return;
  self->diag_cursor = (int)idx;
  AmigaBuildDiag *d = g_ptr_array_index(self->diags, idx);
  if (!d->file) return;
  g_autofree char *abs = (g_path_is_absolute(d->file) || !self->project || !self->project->root_dir)
                             ? g_strdup(d->file)
                             : g_build_filename(self->project->root_dir, d->file, NULL);
  amiga_window_open_file_at_line(self, abs, d->line ? d->line : 1u);
}

static void
on_diag_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "diag-idx"));
  activate_diag_by_index(self, idx);
}

static void
on_diag_filter_toggled(GtkToggleButton *tb, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!gtk_toggle_button_get_active(tb)) return;
  self->diag_filter = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tb), "filt"));
  rebuild_diag_list(self);
}

static void on_diag_prev_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  amiga_window_drawer_jump_diag(KAJO_AMIGA_WINDOW(ud), -1);
}
static void on_diag_next_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  amiga_window_drawer_jump_diag(KAJO_AMIGA_WINDOW(ud), +1);
}

static void
on_diag_clear_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->console_view) {
    GtkTextBuffer *cb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->console_view));
    gtk_text_buffer_set_text(cb, "", 0);
  }
}

static void
on_drawer_tab_toggled(GtkToggleButton *tb, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->drawer_stack || !gtk_toggle_button_get_active(tb)) return;
  const char *pg = g_object_get_data(G_OBJECT(tb), "drw-page");
  if (pg) gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), pg);
}

static void
on_drawer_close_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->user_drawer_open = false;
  if (self->drawer_box) gtk_widget_set_visible(self->drawer_box, FALSE);
  if (self->btn_tog_bottom) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tog_bottom), FALSE);
}

static void
on_trace_toggle_toggled(GtkToggleButton *tb, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  bool on = gtk_toggle_button_get_active(tb);
  self->cfg_enable_trace_history = on;
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    amiga_rsp_client_rcmd_async(self->rsp, on ? "history on" : "history off", NULL, NULL, NULL);
    if (on) amiga_window_debug_request_history(self);
  }
}

static void
on_trace_read_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  amiga_window_debug_request_history(KAJO_AMIGA_WINDOW(ud));
}

static void
on_trace_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  uint32_t pc = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "trace-pc"));
  if (pc != 0) amiga_window_jump_disasm(self, pc);
}

static void
on_rcmd_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  typedef struct { GWeakRef weak; char *cmd; } RcmdCtx;
  RcmdCtx *c = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *reply = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(&c->weak);
  g_weak_ref_clear(&c->weak);
  g_autofree char *cmd = c->cmd;
  g_free(c);
  if (!self) return;
  amiga_window_drawer_append_rcmd(self, cmd, reply ? reply : (err ? err->message : "(empty)"));
}

static void
send_rcmd_str(KajoAmigaWindow *self, const char *cmd)
{
  if (!cmd || !cmd[0]) return;
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_DISCONNECTED) {
    amiga_window_drawer_append_rcmd(self, cmd, "(not connected to Amiberry RSP)");
    return;
  }
  typedef struct { GWeakRef weak; char *cmd; } RcmdCtx;
  RcmdCtx *c = g_new0(RcmdCtx, 1);
  g_weak_ref_init(&c->weak, self);
  c->cmd = g_strdup(cmd);
  amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, on_rcmd_done, c);
}

static void
on_rcmd_submit(GtkWidget *w G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  const char *cmd = gtk_editable_get_text(GTK_EDITABLE(self->rcmd_entry));
  if (!cmd || !cmd[0]) return;
  send_rcmd_str(self, cmd);
  gtk_editable_set_text(GTK_EDITABLE(self->rcmd_entry), "");
}

static void
on_rcmd_chip_clicked(GtkButton *b, gpointer ud)
{
  const char *cmd = g_object_get_data(G_OBJECT(b), "rcmd");
  send_rcmd_str(KAJO_AMIGA_WINDOW(ud), cmd);
}

static void
on_mem_read_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) bytes = amiga_rsp_client_read_mem_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !bytes) return;
  gsize len = 0;
  const uint8_t *raw = g_bytes_get_data(bytes, &len);
  amiga_mem_view_set_data(self->mem_view, amiga_mem_view_get_base_addr(self->mem_view), raw, len);
}

static void
on_mem_read_cb(AmigaMemView *v G_GNUC_UNUSED, uint32_t addr, size_t len, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  amiga_rsp_client_read_mem_async(self->rsp, addr, len, NULL, on_mem_read_done, weak);
}

static void
on_mem_write_cb(AmigaMemView *v G_GNUC_UNUSED, uint32_t addr, const uint8_t *b, size_t len, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    amiga_rsp_client_write_mem_async(self->rsp, addr, b, len, NULL, NULL, NULL);
    on_mem_read_cb(self->mem_view, addr, 256, self);
  }
}

static void
on_mem_watch_cb(AmigaMemView *v G_GNUC_UNUSED, uint8_t zt, uint32_t addr, uint32_t len, gpointer ud)
{
  amiga_window_debug_add_watchpoint(KAJO_AMIGA_WINDOW(ud), zt, addr, len);
}

static void
on_mem_dma_watch_cb(AmigaMemView *v G_GNUC_UNUSED, uint32_t addr, uint32_t len, gpointer ud)
{
  amiga_window_debug_add_dma_watch(KAJO_AMIGA_WINDOW(ud), addr, len);
}

GtkWidget *
amiga_window_drawer_build(KajoAmigaWindow *self)
{
  self->drawer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->drawer_box, "amiga-drawer");
  gtk_widget_set_size_request(self->drawer_box, -1, 158);

  GtkWidget *dtabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(dtabs, "amiga-drawer-tabs");
  gtk_widget_set_valign(dtabs, GTK_ALIGN_CENTER);

  self->btn_drw_mem = gtk_toggle_button_new_with_label("▓ MEMORY HEX");
  self->btn_drw_trace = gtk_toggle_button_new_with_label("📜 TRACE (128)");
  self->btn_drw_rcmd = gtk_toggle_button_new_with_label(">_ qRcmd CONSOLE");
  self->btn_drw_build = gtk_toggle_button_new_with_label("🔨 BUILD & DIAGNOSTICS");
  GtkToggleButton *grp = GTK_TOGGLE_BUTTON(self->btn_drw_mem);
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_drw_trace), grp);
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_drw_rcmd), grp);
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_drw_build), grp);

  struct { GtkWidget *w; const char *pg; } tabs[4] = {
    { self->btn_drw_mem, "mem" },
    { self->btn_drw_trace, "trace" },
    { self->btn_drw_rcmd, "rcmd" },
    { self->btn_drw_build, "build" },
  };
  for (int i = 0; i < 4; i++) {
    gtk_widget_add_css_class(tabs[i].w, "amiga-seg-btn");
    g_object_set_data(G_OBJECT(tabs[i].w), "drw-page", (gpointer)tabs[i].pg);
    g_signal_connect(tabs[i].w, "toggled", G_CALLBACK(on_drawer_tab_toggled), self);
    gtk_box_append(GTK_BOX(dtabs), tabs[i].w);
  }
  GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(sp, TRUE);
  GtkWidget *btn_x = gtk_button_new_with_label("✕");
  gtk_widget_add_css_class(btn_x, "amiga-mini-btn");
  g_signal_connect(btn_x, "clicked", G_CALLBACK(on_drawer_close_clicked), self);
  gtk_box_append(GTK_BOX(dtabs), sp);
  gtk_box_append(GTK_BOX(dtabs), btn_x);
  gtk_box_append(GTK_BOX(self->drawer_box), dtabs);

  self->drawer_stack = gtk_stack_new();
  gtk_widget_set_vexpand(self->drawer_stack, TRUE);

  /* Page 1: Memory Hex */
  self->mem_view = amiga_mem_view_new();
  amiga_mem_view_set_callbacks(self->mem_view, on_mem_read_cb, on_mem_write_cb, on_mem_watch_cb, self);
  amiga_mem_view_set_dma_watch_callback(self->mem_view, on_mem_dma_watch_cb, self);
  gtk_stack_add_named(GTK_STACK(self->drawer_stack), GTK_WIDGET(self->mem_view), "mem");

  /* Page 2: Trace (128) */
  GtkWidget *tr_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *tr_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(tr_bar, "amiga-disasm-toolbar");
  self->btn_trace_toggle = gtk_toggle_button_new_with_label("● TRACE ON");
  gtk_widget_add_css_class(self->btn_trace_toggle, "amiga-seg-btn");
  g_signal_connect(self->btn_trace_toggle, "toggled", G_CALLBACK(on_trace_toggle_toggled), self);
  GtkWidget *b_tread = gtk_button_new_with_label("↻ READ 32");
  gtk_widget_add_css_class(b_tread, "ide-toolbar-btn");
  g_signal_connect(b_tread, "clicked", G_CALLBACK(on_trace_read_clicked), self);
  self->lbl_trace_count = gtk_label_new("0 entries");
  gtk_widget_add_css_class(self->lbl_trace_count, "amiga-muted-hint");
  gtk_box_append(GTK_BOX(tr_bar), self->btn_trace_toggle);
  gtk_box_append(GTK_BOX(tr_bar), b_tread);
  gtk_box_append(GTK_BOX(tr_bar), self->lbl_trace_count);
  gtk_box_append(GTK_BOX(tr_box), tr_bar);
  GtkWidget *tr_scr = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(tr_scr, TRUE);
  self->trace_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->trace_list, "amiga-diag-list");
  g_signal_connect(self->trace_list, "row-activated", G_CALLBACK(on_trace_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tr_scr), self->trace_list);
  gtk_box_append(GTK_BOX(tr_box), tr_scr);
  gtk_stack_add_named(GTK_STACK(self->drawer_stack), tr_box, "trace");

  /* Page 3: qRcmd Console */
  GtkWidget *rc_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *rc_chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(rc_chips, "amiga-disasm-toolbar");
  static const char *const k_rcmds[6] = {
    "execution-status", "segments", "tasks", "disasm", "exception last", "watch events"
  };
  for (int i = 0; i < 6; i++) {
    GtkWidget *cb = gtk_button_new_with_label(k_rcmds[i]);
    gtk_widget_add_css_class(cb, "amiga-mini-btn");
    g_object_set_data(G_OBJECT(cb), "rcmd", (gpointer)k_rcmds[i]);
    g_signal_connect(cb, "clicked", G_CALLBACK(on_rcmd_chip_clicked), self);
    gtk_box_append(GTK_BOX(rc_chips), cb);
  }
  gtk_box_append(GTK_BOX(rc_box), rc_chips);
  GtkWidget *rc_scr = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(rc_scr, TRUE);
  self->rcmd_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(self->rcmd_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->rcmd_view), TRUE);
  gtk_widget_add_css_class(self->rcmd_view, "amiga-console-view");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rc_scr), self->rcmd_view);
  gtk_box_append(GTK_BOX(rc_box), rc_scr);
  GtkWidget *rc_in = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->rcmd_entry = gtk_entry_new();
  gtk_widget_add_css_class(self->rcmd_entry, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->rcmd_entry), ">_ qRcmd (e.g. execution-status, memory-map)");
  gtk_widget_set_hexpand(self->rcmd_entry, TRUE);
  g_signal_connect(self->rcmd_entry, "activate", G_CALLBACK(on_rcmd_submit), self);
  GtkWidget *b_send = gtk_button_new_with_label("SEND");
  gtk_widget_add_css_class(b_send, "ide-toolbar-btn");
  g_signal_connect(b_send, "clicked", G_CALLBACK(on_rcmd_submit), self);
  gtk_box_append(GTK_BOX(rc_in), self->rcmd_entry);
  gtk_box_append(GTK_BOX(rc_in), b_send);
  gtk_box_append(GTK_BOX(rc_box), rc_in);
  gtk_stack_add_named(GTK_STACK(self->drawer_stack), rc_box, "rcmd");

  /* Page 4: Build & Diagnostics / Guest Output */
  GtkWidget *bld_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *fbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(fbar, "amiga-disasm-toolbar");
  self->btn_diag_all = gtk_toggle_button_new_with_label("ALL (0)");
  self->btn_diag_err = gtk_toggle_button_new_with_label("ERRORS (0)");
  self->btn_diag_warn = gtk_toggle_button_new_with_label("WARNINGS (0)");
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_diag_err), GTK_TOGGLE_BUTTON(self->btn_diag_all));
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_diag_warn), GTK_TOGGLE_BUTTON(self->btn_diag_all));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_diag_all), TRUE);
  gtk_widget_add_css_class(self->btn_diag_all, "amiga-filter-pill");
  gtk_widget_add_css_class(self->btn_diag_err, "amiga-filter-pill");
  gtk_widget_add_css_class(self->btn_diag_warn, "amiga-filter-pill");
  g_object_set_data(G_OBJECT(self->btn_diag_all), "filt", GINT_TO_POINTER(0));
  g_object_set_data(G_OBJECT(self->btn_diag_err), "filt", GINT_TO_POINTER(1));
  g_object_set_data(G_OBJECT(self->btn_diag_warn), "filt", GINT_TO_POINTER(2));
  g_signal_connect(self->btn_diag_all, "toggled", G_CALLBACK(on_diag_filter_toggled), self);
  g_signal_connect(self->btn_diag_err, "toggled", G_CALLBACK(on_diag_filter_toggled), self);
  g_signal_connect(self->btn_diag_warn, "toggled", G_CALLBACK(on_diag_filter_toggled), self);

  GtkWidget *b_prev = gtk_button_new_with_label("↑ Prev");
  GtkWidget *b_next = gtk_button_new_with_label("↓ Next (F8)");
  GtkWidget *b_clr = gtk_button_new_with_label("CLEAR");
  gtk_widget_add_css_class(b_prev, "amiga-mini-btn");
  gtk_widget_add_css_class(b_next, "amiga-mini-btn");
  gtk_widget_add_css_class(b_clr, "amiga-mini-btn");
  g_signal_connect(b_prev, "clicked", G_CALLBACK(on_diag_prev_clicked), self);
  g_signal_connect(b_next, "clicked", G_CALLBACK(on_diag_next_clicked), self);
  g_signal_connect(b_clr, "clicked", G_CALLBACK(on_diag_clear_clicked), self);

  gtk_box_append(GTK_BOX(fbar), self->btn_diag_all);
  gtk_box_append(GTK_BOX(fbar), self->btn_diag_err);
  gtk_box_append(GTK_BOX(fbar), self->btn_diag_warn);
  GtkWidget *fsp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(fsp, TRUE);
  gtk_box_append(GTK_BOX(fbar), fsp);
  gtk_box_append(GTK_BOX(fbar), b_prev);
  gtk_box_append(GTK_BOX(fbar), b_next);
  gtk_box_append(GTK_BOX(fbar), b_clr);
  gtk_box_append(GTK_BOX(bld_box), fbar);

  GtkWidget *build_split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(build_split, "ide-paned");
  gtk_widget_set_vexpand(build_split, TRUE);
  GtkWidget *c_scr = gtk_scrolled_window_new();
  self->console_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(self->console_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->console_view), TRUE);
  gtk_widget_add_css_class(self->console_view, "amiga-console-view");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(c_scr), self->console_view);
  GtkWidget *d_scr = gtk_scrolled_window_new();
  self->diag_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->diag_list, "amiga-diag-list");
  g_signal_connect(self->diag_list, "row-activated", G_CALLBACK(on_diag_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(d_scr), self->diag_list);
  gtk_paned_set_start_child(GTK_PANED(build_split), c_scr);
  gtk_paned_set_end_child(GTK_PANED(build_split), d_scr);
  gtk_paned_set_position(GTK_PANED(build_split), 600);
  gtk_box_append(GTK_BOX(bld_box), build_split);
  gtk_stack_add_named(GTK_STACK(self->drawer_stack), bld_box, "build");

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_build), TRUE);
  gtk_box_append(GTK_BOX(self->drawer_box), self->drawer_stack);
  return self->drawer_box;
}

void
amiga_window_drawer_sync_tabs(KajoAmigaWindow *self, AmigaSessionState st)
{
  if (!self->btn_drw_build) return;
  bool is_edit = (st == AMIGA_SESSION_EDIT || st == AMIGA_SESSION_BUILD_ERR || st == AMIGA_SESSION_SETUP);
  bool is_run = (st == AMIGA_SESSION_RUNNING);
  bool is_paused = (st == AMIGA_SESSION_PAUSED);
  bool is_attach = (st == AMIGA_SESSION_ATTACH);

  gtk_widget_set_visible(self->btn_drw_mem, is_paused || is_attach);
  gtk_widget_set_visible(self->btn_drw_trace, is_paused || is_attach);
  gtk_widget_set_visible(self->btn_drw_rcmd, is_paused || is_attach);
  gtk_widget_set_visible(self->btn_drw_build, is_edit || is_run || is_paused);

  gtk_button_set_label(GTK_BUTTON(self->btn_drw_build),
                       (is_run || is_paused) ? "🔨 BUILD & GUEST OUTPUT" : "🔨 BUILD & DIAGNOSTICS");

  const char *cur = gtk_stack_get_visible_child_name(GTK_STACK(self->drawer_stack));
  if ((is_edit || is_run) && g_strcmp0(cur, "build") != 0) {
    amiga_window_drawer_select(self, "build");
  } else if (is_attach && g_strcmp0(cur, "build") == 0) {
    amiga_window_drawer_select(self, "mem");
  }
}

void
amiga_window_drawer_select(KajoAmigaWindow *self, const char *page)
{
  if (!self || !self->drawer_stack || !page) return;
  if (g_strcmp0(page, "mem") == 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_mem), TRUE);
  else if (g_strcmp0(page, "trace") == 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_trace), TRUE);
  else if (g_strcmp0(page, "rcmd") == 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_rcmd), TRUE);
  else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_build), TRUE);
  gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), page);
}

void
amiga_window_append_console(KajoAmigaWindow *self, const char *line)
{
  if (!self || !self->console_view || !line) return;
  GtkTextBuffer *cb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->console_view));
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(cb, &end);
  gtk_text_buffer_insert(cb, &end, line, -1);
  if (!g_str_has_suffix(line, "\n")) gtk_text_buffer_insert(cb, &end, "\n", 1);
}

void
amiga_window_drawer_set_diags(KajoAmigaWindow *self, GPtrArray *diags)
{
  g_ptr_array_set_size(self->diags, 0);
  self->diag_cursor = -1;
  if (diags) {
    for (guint i = 0; i < diags->len; i++) {
      AmigaBuildDiag *d = g_ptr_array_index(diags, i);
      AmigaBuildDiag *cp = g_new0(AmigaBuildDiag, 1);
      cp->file = g_strdup(d->file);
      cp->line = d->line;
      cp->col = d->col;
      cp->severity = d->severity;
      cp->message = g_strdup(d->message);
      g_ptr_array_add(self->diags, cp);
    }
  }
  rebuild_diag_list(self);
  sync_diags_to_open_buffers(self);
}

void
amiga_window_drawer_jump_diag(KajoAmigaWindow *self, int delta)
{
  if (!self || !self->diags || self->diags->len == 0) {
    amiga_window_show_toast(self, "No build diagnostics");
    return;
  }
  int n = (int)self->diags->len;
  int next = (self->diag_cursor < 0) ? (delta >= 0 ? 0 : n - 1)
                                     : ((self->diag_cursor + delta + n) % n);
  activate_diag_by_index(self, (guint)next);
}

void
amiga_window_drawer_set_trace(KajoAmigaWindow *self, GPtrArray *entries)
{
  if (!self || !self->trace_list) return;
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->trace_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->trace_list), ch);
  }
  guint cnt = entries ? entries->len : 0u;
  g_autofree char *cs = g_strdup_printf("%u entries", cnt);
  gtk_label_set_text(GTK_LABEL(self->lbl_trace_count), cs);
  if (!entries || cnt == 0) return;

  for (guint i = 0; i < cnt; i++) {
    const AmigaHistoryEntry *he = g_ptr_array_index(entries, i);
    const char *sname = NULL;
    uint32_t sdelta = 0;
    g_autofree char *sym = NULL;
    if (self->symbols && amiga_symbols_resolve_addr(self->symbols, he->pc, &sname, &sdelta)) {
      sym = sdelta == 0 ? g_strdup(sname) : g_strdup_printf("%s+$%x", sname, sdelta);
    } else {
      sym = g_strdup("");
    }
    g_autofree char *row = g_strdup_printf(
        "#%02u F%u V%u H%u  $%08X %-16s  D0=%08X A0=%08X",
        i, he->frame, he->vpos, he->hpos, he->pc, sym, he->regs[0], he->regs[8]);
    GtkWidget *l = gtk_label_new(row);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_list_box_append(GTK_LIST_BOX(self->trace_list), l);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->trace_list), (int)i);
    if (r) g_object_set_data(G_OBJECT(r), "trace-pc", GUINT_TO_POINTER(he->pc));
  }
}

void
amiga_window_drawer_append_rcmd(KajoAmigaWindow *self, const char *cmd, const char *reply)
{
  if (!self || !self->rcmd_view) return;
  GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->rcmd_view));
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(tb, &end);
  g_autofree char *hdr = g_strdup_printf("> %s\n", cmd ? cmd : "");
  gtk_text_buffer_insert(tb, &end, hdr, -1);
  if (reply && reply[0]) {
    gtk_text_buffer_insert(tb, &end, reply, -1);
    if (!g_str_has_suffix(reply, "\n")) gtk_text_buffer_insert(tb, &end, "\n", 1);
  }
}
