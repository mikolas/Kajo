#include "amiga_window_private.h"

#include "../common/spawn_detached.h"
#include "../compositor/compositor.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_TYPE(KajoAmigaWindow, kajo_amiga_window, GTK_TYPE_APPLICATION_WINDOW)

/* ─── Helpers & Toast ─── */

static gboolean
on_toast_hide(gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->toast_timer_id = 0;
  if (self->toast_bar) gtk_widget_set_visible(self->toast_bar, FALSE);
  return G_SOURCE_REMOVE;
}

void
amiga_window_show_toast(KajoAmigaWindow *self, const char *msg)
{
  if (!self || !self->toast_bar || !msg) return;
  gtk_label_set_text(GTK_LABEL(self->toast_lbl), msg);
  gtk_widget_set_visible(self->toast_bar, TRUE);
  g_clear_handle_id(&self->toast_timer_id, g_source_remove);
  self->toast_timer_id = g_timeout_add(3200, on_toast_hide, self);
}

#define show_toast amiga_window_show_toast

const AmigaTarget *
amiga_window_get_selected_target(KajoAmigaWindow *self)
{
  if (!self->project || !self->project->targets || self->project->targets->len == 0) return NULL;
  guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_target));
  if (idx >= self->project->targets->len) idx = 0;
  return g_ptr_array_index(self->project->targets, idx);
}

const AmigaHwProfile *
amiga_window_get_selected_profile(KajoAmigaWindow *self)
{
  if (!self->project || !self->project->profiles || self->project->profiles->len == 0) return NULL;
  guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_profile));
  if (idx >= self->project->profiles->len) idx = 0;
  return g_ptr_array_index(self->project->profiles, idx);
}

static void
sync_compat_badge(KajoAmigaWindow *self)
{
  const AmigaTarget *t = amiga_window_get_selected_target(self);
  const AmigaHwProfile *p = amiga_window_get_selected_profile(self);
  g_autofree char *warn = amiga_project_check_profile_compat(t, p);
  if (warn) {
    gtk_label_set_text(GTK_LABEL(self->compat_badge), warn);
    gtk_widget_set_visible(self->compat_badge, TRUE);
  } else {
    gtk_widget_set_visible(self->compat_badge, FALSE);
  }
}

void
amiga_window_sync_mode_pill(KajoAmigaWindow *self)
{
  if (!self->mode_pill) return;
  gtk_widget_remove_css_class(self->mode_pill, "insert");
  gtk_widget_remove_css_class(self->mode_pill, "select");
  gtk_widget_remove_css_class(self->mode_pill, "attach");

  if (self->attach_mode) {
    gtk_label_set_text(GTK_LABEL(self->mode_pill), "ATTACH");
    gtk_widget_add_css_class(self->mode_pill, "attach");
    return;
  }
  IdeState st = self->fsm ? ide_fsm_get_state(self->fsm) : IDE_STATE_NORMAL;
  gtk_label_set_text(GTK_LABEL(self->mode_pill), ide_state_to_string(st));
  if (st == IDE_STATE_INSERT) gtk_widget_add_css_class(self->mode_pill, "insert");
  else if (st == IDE_STATE_SELECT) gtk_widget_add_css_class(self->mode_pill, "select");
}

static void
update_cursor_and_selection_cycles(KajoAmigaWindow *self)
{
  if (!self->active_buf) {
    gtk_label_set_text(GTK_LABEL(self->lbl_cursor), "Ln 1, Col 1");
    gtk_widget_set_visible(self->lbl_sel_cycles, FALSE);
    return;
  }
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark(tb, &cur, gtk_text_buffer_get_insert(tb));
  g_autofree char *pos = g_strdup_printf("Ln %d, Col %d",
                                         gtk_text_iter_get_line(&cur) + 1,
                                         gtk_text_iter_get_line_offset(&cur) + 1);
  gtk_label_set_text(GTK_LABEL(self->lbl_cursor), pos);

  GtkTextIter s, e;
  if (self->show_sel_cycles && gtk_text_buffer_get_selection_bounds(tb, &s, &e)) {
    int l1 = gtk_text_iter_get_line(&s);
    int l2 = gtk_text_iter_get_line(&e);
    if (l2 > l1) {
      GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
      for (int ln = l1; ln <= l2; ln++) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(tb, &ls, ln);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        g_ptr_array_add(lines, gtk_text_buffer_get_text(tb, &ls, &le, FALSE));
      }
      AmigaCycleSum sum = amiga_m68k_sum_cycles(
          (const char *const *)lines->pdata, lines->len);
      g_ptr_array_unref(lines);
      if (sum.instr_count > 0) {
        g_autofree char *cs = (sum.min_cycles == sum.max_cycles)
                                  ? g_strdup_printf("SEL: %u ln · %u(%u/%u) cyc",
                                                    sum.instr_count, sum.min_cycles,
                                                    sum.reads, sum.writes)
                                  : g_strdup_printf("SEL: %u ln · %u-%u(%u/%u) cyc",
                                                    sum.instr_count, sum.min_cycles,
                                                    sum.max_cycles, sum.reads, sum.writes);
        gtk_label_set_text(GTK_LABEL(self->lbl_sel_cycles), cs);
        gtk_widget_set_visible(self->lbl_sel_cycles, TRUE);
        return;
      }
    }
  }
  gtk_widget_set_visible(self->lbl_sel_cycles, FALSE);
}

/* ─── Editor Buffer Open, Outline & Save ─── */

void
amiga_window_open_file_at_line(KajoAmigaWindow *self, const char *abs_path, guint line_1based)
{
  if (!abs_path || !g_file_test(abs_path, G_FILE_TEST_IS_REGULAR)) return;

  IdeBuffer *buf = ide_buffer_new(abs_path);
  g_autoptr(GError) err = NULL;
  if (!ide_buffer_load_from_disk(buf, &err)) {
    ide_buffer_free(buf);
    show_toast(self, err ? err->message : "Open failed");
    return;
  }
  ide_buffer_set_style_scheme(buf, self->inherited_scheme ? self->inherited_scheme : "kajo-dark");
  if (self->active_buf) ide_buffer_free(self->active_buf);
  self->active_buf = buf;

  GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(buf);
  gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view), GTK_TEXT_BUFFER(sbuf));
  kajo_ide_gutter_renderer_set_buffer(self->gutter, buf);
  self->editor_host.buffer = buf;

  amiga_window_debug_sync_buffer_breakpoints(self, buf);
  amiga_window_lsp_notify_open(self, buf);

  if (line_1based > 0) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(sbuf), &it, (int)(line_1based - 1u));
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(sbuf), &it);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view), &it, 0.2, FALSE, 0, 0.5);
  }
  gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "source");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_source), TRUE);

  g_autofree char *title = g_path_get_basename(abs_path);
  gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), title);
  amiga_window_refresh_source_gutter_and_outline(self);
  update_cursor_and_selection_cycles(self);
}

#define open_file_in_editor amiga_window_open_file_at_line

static void
on_outline_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  guint line = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "outline-line"));
  if (!self->active_buf || line == 0) return;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_line(tb, &it, (int)(line - 1u));
  gtk_text_buffer_place_cursor(tb, &it);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view), &it, 0.2, FALSE, 0, 0.5);
  gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "source");
}

void
amiga_window_refresh_source_gutter_and_outline(KajoAmigaWindow *self)
{
  if (!self->outline_list) return;
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->outline_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->outline_list), ch);
  }
  if (!self->active_buf) return;
  g_autofree char *txt = ide_buffer_get_text(self->active_buf);
  const char *p = ide_buffer_get_file_path(self->active_buf);
  bool is_c = p && (g_str_has_suffix(p, ".c") || g_str_has_suffix(p, ".h"));
  g_autoptr(GPtrArray) items = amiga_symbols_extract_outline(txt ? txt : "", is_c);

  for (guint i = 0; i < items->len; i++) {
    const AmigaOutlineItem *it = g_ptr_array_index(items, i);
    const char *tag = (it->kind == AMIGA_OUTLINE_SECTION) ? "SEC"
                    : (it->kind == AMIGA_OUTLINE_FUNCTION) ? "FN "
                    : (it->kind == AMIGA_OUTLINE_MACRO) ? "MAC"
                    : (it->kind == AMIGA_OUTLINE_EQU) ? "EQU" : "LBL";
    g_autofree char *lbl = g_strdup_printf("%s %-16s :%u", tag, it->name, it->line);
    GtkWidget *w = gtk_label_new(lbl);
    gtk_label_set_xalign(GTK_LABEL(w), 0.0f);
    gtk_list_box_append(GTK_LIST_BOX(self->outline_list), w);
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->outline_list), (int)i);
    if (row) g_object_set_data(G_OBJECT(row), "outline-line", GUINT_TO_POINTER(it->line));
  }
}

static void
save_active_buffer(KajoAmigaWindow *self)
{
  if (!self->active_buf) return;
  g_autoptr(GError) err = NULL;
  if (ide_buffer_save(self->active_buf, &err)) {
    amiga_window_refresh_source_gutter_and_outline(self);
    amiga_window_lsp_notify_save(self, self->active_buf);
    show_toast(self, "✓ File saved");
  } else {
    show_toast(self, err ? err->message : "Save failed");
  }
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

/* ─── Option D Async Build & Clickable Diagnostics ─── */

static void
on_diag_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  int idx = gtk_list_box_row_get_index(row);
  if (idx < 0 || !self->diags || (guint)idx >= self->diags->len) return;
  AmigaBuildDiag *d = g_ptr_array_index(self->diags, (guint)idx);
  if (!d->file) return;
  g_autofree char *abs = g_path_is_absolute(d->file) || !self->project || !self->project->root_dir
                             ? g_strdup(d->file)
                             : g_build_filename(self->project->root_dir, d->file, NULL);
  open_file_in_editor(self, abs, d->line);
  if (self->active_buf) {
    ide_buffer_clear_diagnostics(self->active_buf);
    ide_buffer_add_diagnostic(
        self->active_buf, d->line, d->col,
        d->severity == AMIGA_DIAG_WARNING ? IDE_DIAG_WARNING : IDE_DIAG_ERROR,
        d->message, "amiga-build");
    gtk_widget_queue_draw(self->source_view);
  }
}

static void
on_build_finished(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  AmigaBuildResult *br = amiga_build_run_finish(res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self) {
    amiga_build_result_free(br);
    return;
  }

  self->building = false;
  bool launch_pending = self->launch_after_build;
  self->launch_after_build = false;
  gtk_widget_set_sensitive(self->btn_build, TRUE);

  GtkTextBuffer *cb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->console_view));
  if (err) {
    gtk_text_buffer_set_text(cb, err->message, -1);
    show_toast(self, err->message);
    return;
  }
  gtk_text_buffer_set_text(cb, (br && br->log_text) ? br->log_text : "", -1);

  g_ptr_array_set_size(self->diags, 0);
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->diag_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->diag_list), ch);
  }
  if (br && br->diagnostics) {
    for (guint i = 0; i < br->diagnostics->len; i++) {
      AmigaBuildDiag *d = g_ptr_array_index(br->diagnostics, i);
      AmigaBuildDiag *cp = g_new0(AmigaBuildDiag, 1);
      cp->file = g_strdup(d->file);
      cp->line = d->line;
      cp->col = d->col;
      cp->severity = d->severity;
      cp->message = g_strdup(d->message);
      g_ptr_array_add(self->diags, cp);

      g_autofree char *line = g_strdup_printf("%s:%u:%u  %s",
                                              d->file ? d->file : "?", d->line, d->col,
                                              d->message ? d->message : "");
      GtkWidget *l = gtk_label_new(line);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
      gtk_widget_add_css_class(
          l, d->severity == AMIGA_DIAG_WARNING ? "amiga-diag-warn" : "amiga-diag-error");
      gtk_list_box_append(GTK_LIST_BOX(self->diag_list), l);
    }
  }
  bool ok = (br && br->success);
  amiga_build_result_free(br);
  if (ok) {
    amiga_window_debug_reload_symbols(self);
    show_toast(self, "✓ Build succeeded");
    if (launch_pending) {
      amiga_window_debug_launch_after_build(self);
    }
  } else {
    show_toast(self, "✗ Build failed (see diagnostics)");
  }
}

void
amiga_window_trigger_build(KajoAmigaWindow *self)
{
  if (!self->project || self->building) return;
  const AmigaTarget *t = amiga_window_get_selected_target(self);
  if (!t) {
    self->launch_after_build = false;
    show_toast(self, "No target defined in project");
    return;
  }
  save_active_buffer(self);
  self->building = true;
  gtk_widget_set_sensitive(self->btn_build, FALSE);
  gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), "build");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_build), TRUE);

  g_cancellable_cancel(self->build_cancel);
  g_clear_object(&self->build_cancel);
  self->build_cancel = g_cancellable_new();

  g_autofree char *staging = g_build_filename(
      g_get_user_cache_dir(), "kajo", "amiga",
      self->project->name ? self->project->name : "default", NULL);
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  amiga_build_run_async(self->project, t, self->toolchain, staging,
                        self->build_cancel, on_build_finished, weak);
}

#define trigger_build amiga_window_trigger_build

/* ─── View Callbacks ─── */

static void on_strip_write_regs(AmigaCpuStrip *s G_GNUC_UNUSED, const AmigaCpuRegs *regs, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    self->last_regs = *regs;
    self->has_last_regs = true;
    amiga_rsp_client_write_regs_async(self->rsp, regs, NULL, NULL, NULL);
    amiga_cpu_strip_update(self->cpu_strip, regs,
                           self->has_last_exec_st ? &self->last_exec_st : NULL,
                           self->symbols, true);
  }
}

void
amiga_window_jump_mem(KajoAmigaWindow *self, uint32_t addr)
{
  if (!self) return;
  gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), "hex");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_hex), TRUE);
  amiga_mem_view_jump_to(self->mem_view, addr);
}

static void on_strip_jump(AmigaCpuStrip *s G_GNUC_UNUSED, uint32_t addr, bool to_mem, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (to_mem) {
    amiga_window_jump_mem(self, addr);
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "disasm");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_disasm), TRUE);
    amiga_disasm_view_go_to(self->disasm_view, addr, true);
  }
}

static void on_strip_cp(AmigaCpuStrip *s G_GNUC_UNUSED, bool restore, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  g_autofree char *cmd = g_strdup_printf("checkpoint %s /tmp/kajo-amiga-cp.uss", restore ? "restore" : "save");
  amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, NULL, NULL);
  show_toast(self, restore ? "⏮ Restored checkpoint" : "💾 Saved checkpoint");
}

static void on_mem_read_done(GObject *src, GAsyncResult *res, gpointer ud) {
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

static void on_mem_read_cb(AmigaMemView *v G_GNUC_UNUSED, uint32_t addr, size_t len, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
  amiga_rsp_client_read_mem_async(self->rsp, addr, len, NULL, on_mem_read_done, weak);
}

static void on_mem_write_cb(AmigaMemView *v G_GNUC_UNUSED, uint32_t addr, const uint8_t *b, size_t len, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    amiga_rsp_client_write_mem_async(self->rsp, addr, b, len, NULL, NULL, NULL);
    on_mem_read_cb(self->mem_view, addr, 256, self);
  }
}

static void on_mem_watch_cb(AmigaMemView *v G_GNUC_UNUSED, uint8_t zt, uint32_t addr, uint32_t len, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    amiga_rsp_client_watch_set_async(self->rsp, zt, addr, len, NULL, NULL, NULL);
    show_toast(self, "👁 Watchpoint requested");
  }
}

/* ─── Header & Project Wiring ─── */

static void on_tree_file_open(const char *abs_path, gpointer ud) {
  open_file_in_editor(KAJO_AMIGA_WINDOW(ud), abs_path, 1);
}

static void on_hdr_tree_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  gtk_widget_set_visible(self->sidebar_box, !gtk_widget_get_visible(self->sidebar_box));
}

static void on_hdr_build_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  trigger_build(KAJO_AMIGA_WINDOW(ud));
}

static void on_hdr_debug_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  amiga_window_debug_start_or_continue(KAJO_AMIGA_WINDOW(ud));
}

static void on_hdr_pause_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  amiga_window_debug_pause(KAJO_AMIGA_WINDOW(ud));
}

static void on_hdr_stop_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  amiga_window_debug_stop(KAJO_AMIGA_WINDOW(ud));
}

static void on_hdr_attach_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  kajo_amiga_window_open_attach(self, self->cfg_gdb_host, self->cfg_gdb_port);
}

static void on_hdr_snapshot_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  amiga_window_debug_refresh_snapshot(KAJO_AMIGA_WINDOW(ud));
}

static void on_hdr_float_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
  niri_toggle_window_floating();
}

static void on_hdr_close_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  gtk_window_close(GTK_WINDOW(ud));
}

static void on_hdr_settings_clicked(gpointer ud G_GNUC_UNUSED) {
  const char *argv[] = { "kajo-settings", "--page", "amiga", NULL };
  kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
}

static void on_wizard_created(const char *dir, gpointer ud) {
  kajo_amiga_window_open_project_dir(KAJO_AMIGA_WINDOW(ud), dir);
}

void
amiga_window_act_new_project(KajoAmigaWindow *self) {
  amiga_project_wizard_present(GTK_WINDOW(self), self->toolchain, on_wizard_created, self);
}

static void on_act_new_project(gpointer ud) {
  amiga_window_act_new_project(KAJO_AMIGA_WINDOW(ud));
}

static void on_open_folder_done(GObject *src, GAsyncResult *res, gpointer ud) {
  GWeakRef *weak = ud;
  g_autoptr(GFile) f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (self && f) {
    g_autofree char *p = g_file_get_path(f);
    if (p) kajo_amiga_window_open_project_dir(self, p);
  }
}

static void on_act_open_project(gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Open Amiga Project Folder");
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL, on_open_folder_done, weak);
}

static void on_act_attach(gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  kajo_amiga_window_open_attach(self, self->cfg_gdb_host, self->cfg_gdb_port);
}

static void on_proj_saved(AmigaProject *p, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (p && p->manifest_path && g_file_test(p->manifest_path, G_FILE_TEST_IS_REGULAR)) {
    g_autoptr(GError) err = NULL;
    AmigaProject *reloaded = amiga_project_load_dir(p->root_dir, &err);
    if (reloaded) {
      g_clear_pointer(&self->project, amiga_project_free);
      self->project = reloaded;
      p = reloaded;
    }
  }
  amiga_window_populate_project_dropdowns(self);
  amiga_project_setup_view_bind(self->setup_view, p, self->toolchain);
  if (p->hrm_dir && p->hrm_dir[0]) {
    g_autofree char *abs_hrm = g_path_is_absolute(p->hrm_dir)
                                   ? g_strdup(p->hrm_dir)
                                   : g_build_filename(p->root_dir, p->hrm_dir, NULL);
    amiga_hrm_view_set_hrm_dir(self->hrm_view, abs_hrm);
  }
  amiga_project_write_compile_commands(p, self->toolchain, NULL);
  amiga_window_debug_reload_symbols(self);
  amiga_window_lsp_sync_project(self);
  sync_compat_badge(self);
  show_toast(self, "✓ Project saved + compile_commands.json updated");
}

void
amiga_window_sync_header_workspace(KajoAmigaWindow *self, bool in_ws) {
  gtk_widget_set_visible(self->btn_hdr_tree, in_ws);
  gtk_widget_set_visible(self->dd_target, in_ws);
  gtk_widget_set_visible(self->dd_profile, in_ws);
  gtk_widget_set_visible(self->btn_build, in_ws);
  gtk_widget_set_visible(self->btn_debug, in_ws);
  if (self->btn_pause) gtk_widget_set_visible(self->btn_pause, in_ws);
  gtk_widget_set_visible(self->btn_stop, in_ws);
  gtk_widget_set_visible(self->btn_snapshot, in_ws);
  if (!in_ws) gtk_widget_set_visible(self->compat_badge, FALSE);
  amiga_window_debug_sync_ui(self);
}

static void on_center_tab_toggled(GtkToggleButton *tb, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->center_stack || !gtk_toggle_button_get_active(tb)) return;
  if (GTK_WIDGET(tb) == self->btn_tab_source) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "source");
  } else if (GTK_WIDGET(tb) == self->btn_tab_disasm) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "disasm");
  } else if (GTK_WIDGET(tb) == self->btn_tab_setup) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "setup");
  }
}

static void on_drawer_tab_toggled(GtkToggleButton *tb, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->drawer_stack || !gtk_toggle_button_get_active(tb)) return;
  if (GTK_WIDGET(tb) == self->btn_drw_build) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), "build");
  } else if (GTK_WIDGET(tb) == self->btn_drw_hex) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), "hex");
  } else if (GTK_WIDGET(tb) == self->btn_drw_debug) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), "debug");
  }
}

static void on_narrow_switch_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  bool show_insp = !gtk_widget_get_visible(self->inspector_box);
  gtk_widget_set_visible(self->inspector_box, show_insp);
  if (self->narrow_mode) {
    gtk_widget_set_visible(self->center_stack, !show_insp);
  }
}

static void
on_window_width_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(obj);
  int w = gtk_widget_get_width(GTK_WIDGET(self));
  if (w <= 0) return;
  bool narrow = (w < 900);
  if (narrow == self->narrow_mode) return;
  self->narrow_mode = narrow;
  gtk_widget_set_visible(self->btn_narrow_switch, narrow);
  if (narrow) {
    gtk_widget_set_visible(self->sidebar_box, FALSE);
    gtk_widget_set_visible(self->inspector_box, FALSE);
    gtk_widget_set_visible(self->center_stack, TRUE);
  } else {
    gtk_widget_set_visible(self->center_stack, TRUE);
    gtk_widget_set_visible(self->inspector_box, TRUE);
    if (w >= 1200) gtk_widget_set_visible(self->sidebar_box, TRUE);
  }
}

static void
on_sb_setup_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->btn_tab_setup) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_setup), TRUE);
  }
}

static gboolean
on_source_key_pressed(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                      guint                  keyval,
                      guint                  keycode G_GNUC_UNUSED,
                      GdkModifierType        state,
                      gpointer               ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->fsm) return FALSE;
  if (ide_fsm_get_state(self->fsm) == IDE_STATE_NORMAL &&
      keyval == GDK_KEY_K &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) == 0 &&
      self->active_buf) {
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
    GtkTextIter ws, we;
    gtk_text_buffer_get_iter_at_mark(tb, &ws, gtk_text_buffer_get_insert(tb));
    we = ws;
    while (!gtk_text_iter_starts_line(&ws)) {
      GtkTextIter prev = ws;
      gtk_text_iter_backward_char(&prev);
      gunichar c = gtk_text_iter_get_char(&prev);
      if (!g_unichar_isalnum(c) && c != '_' && c != '$' && c != '-') break;
      ws = prev;
    }
    while (!gtk_text_iter_ends_line(&we)) {
      gunichar c = gtk_text_iter_get_char(&we);
      if (!g_unichar_isalnum(c) && c != '_') break;
      gtk_text_iter_forward_char(&we);
    }
    g_autofree char *tok = gtk_text_buffer_get_text(tb, &ws, &we, FALSE);
    amiga_window_inspector_lookup_hrm(self, (tok && tok[0]) ? tok : "");
    return TRUE;
  }
  g_autofree char *key_str = ide_editor_translate_gdk_key(keyval, state);
  if (!key_str) return FALSE;
  IdeAction act = ide_fsm_handle_key(self->fsm, key_str);
  amiga_window_sync_mode_pill(self);
  const char *toast = NULL;
  IdeEditorOpOutcome out = ide_editor_ops_execute(&self->editor_host, &act, &toast);
  if (out != IDE_EDITOR_OP_UNHANDLED) {
    if (toast) show_toast(self, toast);
    update_cursor_and_selection_cycles(self);
    return TRUE;
  }
  if (act.type == IDE_ACTION_SAVE || act.type == IDE_ACTION_SAVE_ALL) { save_active_buffer(self); return TRUE; }
  if (act.type == IDE_ACTION_TASK_BUILD || act.type == IDE_ACTION_TASK_RUN) { trigger_build(self); return TRUE; }
  if (act.type == IDE_ACTION_TOGGLE_FILE_TREE) { on_hdr_tree_clicked(NULL, self); return TRUE; }
  return FALSE;
}

static gboolean
on_window_key_pressed(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                      guint                  keyval,
                      guint                  keycode G_GNUC_UNUSED,
                      GdkModifierType        state,
                      gpointer               ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  bool ctrl_mod = (state & GDK_CONTROL_MASK) != 0;
  bool shift_mod = (state & GDK_SHIFT_MASK) != 0;
  bool alt_mod = (state & GDK_ALT_MASK) != 0;

  if (ctrl_mod && shift_mod && (keyval == GDK_KEY_N || keyval == GDK_KEY_n)) {
    on_act_new_project(self);
    return TRUE;
  }
  if (ctrl_mod && !shift_mod && (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) {
    on_act_open_project(self);
    return TRUE;
  }
  if (ctrl_mod && shift_mod && (keyval == GDK_KEY_P || keyval == GDK_KEY_p)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_setup), TRUE);
    return TRUE;
  }
  if (ctrl_mod && shift_mod && (keyval == GDK_KEY_A || keyval == GDK_KEY_a)) {
    on_act_attach(self);
    return TRUE;
  }
  if (ctrl_mod && (keyval == GDK_KEY_comma)) {
    on_hdr_settings_clicked(self);
    return TRUE;
  }
  if (ctrl_mod && (keyval == GDK_KEY_g || keyval == GDK_KEY_G)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_disasm), TRUE);
    amiga_disasm_view_focus_goto(self->disasm_view);
    return TRUE;
  }
  if (alt_mod && keyval == GDK_KEY_Left) {
    amiga_disasm_view_nav_back(self->disasm_view);
    return TRUE;
  }
  if (alt_mod && keyval == GDK_KEY_Right) {
    amiga_disasm_view_nav_fwd(self->disasm_view);
    return TRUE;
  }
  if (keyval == GDK_KEY_F5) {
    if (shift_mod) amiga_window_debug_stop(self);
    else amiga_window_debug_start_or_continue(self);
    return TRUE;
  }
  if (keyval == GDK_KEY_F6) {
    amiga_window_debug_pause(self);
    return TRUE;
  }
  if (keyval == GDK_KEY_F9 && self->active_buf) {
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(tb, &cur, gtk_text_buffer_get_insert(tb));
    guint ln = (guint)gtk_text_iter_get_line(&cur) + 1u;
    amiga_window_debug_toggle_source_bp(self, self->active_buf, ln);
    return TRUE;
  }
  if (keyval == GDK_KEY_F10) {
    amiga_window_debug_step_over(self);
    return TRUE;
  }
  if (keyval == GDK_KEY_F11) {
    amiga_window_debug_step_into(self);
    return TRUE;
  }
  return FALSE;
}

void
amiga_window_populate_project_dropdowns(KajoAmigaWindow *self)
{
  if (!self->project) return;
  g_autoptr(GPtrArray) tnames = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < self->project->targets->len; i++) {
    AmigaTarget *t = g_ptr_array_index(self->project->targets, i);
    g_ptr_array_add(tnames, g_strdup(t->name));
  }
  if (tnames->len == 0) g_ptr_array_add(tnames, g_strdup("default"));
  g_ptr_array_add(tnames, NULL);
  gtk_drop_down_set_model(GTK_DROP_DOWN(self->dd_target),
                          G_LIST_MODEL(gtk_string_list_new((const char *const *)tnames->pdata)));

  g_autoptr(GPtrArray) pnames = g_ptr_array_new_with_free_func(g_free);
  guint def_pidx = 0;
  for (guint i = 0; i < self->project->profiles->len; i++) {
    AmigaHwProfile *p = g_ptr_array_index(self->project->profiles, i);
    g_ptr_array_add(pnames, g_strdup(p->id));
    if (g_strcmp0(p->id, self->project->default_profile) == 0) def_pidx = i;
  }
  if (pnames->len == 0) g_ptr_array_add(pnames, g_strdup("a500_ocs"));
  g_ptr_array_add(pnames, NULL);
  gtk_drop_down_set_model(GTK_DROP_DOWN(self->dd_profile),
                          G_LIST_MODEL(gtk_string_list_new((const char *const *)pnames->pdata)));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_profile), def_pidx);
  sync_compat_badge(self);
}

static void
on_dropdown_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *ps G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  amiga_window_debug_reload_symbols(self);
  sync_compat_badge(self);
}

static void
on_gutter_bp_toggled(IdeBuffer *buf,
                     guint      line_1based,
                     gboolean   enabled G_GNUC_UNUSED,
                     gpointer   ud)
{
  amiga_window_debug_toggle_source_bp(KAJO_AMIGA_WINDOW(ud), buf, line_1based);
}

static void
on_proj_menu_action(GSimpleAction *a, GVariant *p G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  const char *nm = g_action_get_name(G_ACTION(a));
  if (g_strcmp0(nm, "new") == 0) on_act_new_project(self);
  else if (g_strcmp0(nm, "open") == 0) on_act_open_project(self);
  else if (g_strcmp0(nm, "setup") == 0) {
    amiga_window_sync_header_workspace(self, true);
    gtk_stack_set_visible_child_name(GTK_STACK(self->root_stack), "workspace");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_setup), TRUE);
  } else if (g_strcmp0(nm, "welcome") == 0) {
    amiga_window_sync_header_workspace(self, false);
    gtk_stack_set_visible_child_name(GTK_STACK(self->root_stack), "welcome");
  } else if (g_strcmp0(nm, "settings") == 0) {
    on_hdr_settings_clicked(self);
  }
}

static void
build_ui(KajoAmigaWindow *self)
{
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(self), 1240, 700);
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-settings-window");
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-amiga-window");

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* Header Bar */
  GtkWidget *handle = gtk_window_handle_new();
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "settings-header-bar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("⚙ KAJO AMIGA");
  gtk_widget_add_css_class(title, "settings-header-title");
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

  /* 📁 PROJ ▾ menu button */
  GSimpleActionGroup *pg = g_simple_action_group_new();
  const char *pacts[] = { "new", "open", "setup", "welcome", "settings" };
  for (gsize i = 0; i < G_N_ELEMENTS(pacts); i++) {
    GSimpleAction *sa = g_simple_action_new(pacts[i], NULL);
    g_signal_connect(sa, "activate", G_CALLBACK(on_proj_menu_action), self);
    g_action_map_add_action(G_ACTION_MAP(pg), G_ACTION(sa));
    g_object_unref(sa);
  }
  gtk_widget_insert_action_group(GTK_WIDGET(self), "proj", G_ACTION_GROUP(pg));
  g_object_unref(pg);

  GMenu *pmenu = g_menu_new();
  g_menu_append(pmenu, "New Project…\tCtrl+Shift+N", "proj.new");
  g_menu_append(pmenu, "Open Workspace…\tCtrl+O", "proj.open");
  g_menu_append(pmenu, "Project Setup (kajo-amiga.toml)", "proj.setup");
  g_menu_append(pmenu, "Welcome Screen", "proj.welcome");
  g_menu_append(pmenu, "Toolchain Settings…\tCtrl+,", "proj.settings");

  self->btn_proj_menu = gtk_menu_button_new();
  gtk_menu_button_set_label(GTK_MENU_BUTTON(self->btn_proj_menu), "📁 PROJ");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->btn_proj_menu), G_MENU_MODEL(pmenu));
  gtk_widget_add_css_class(self->btn_proj_menu, "ide-toolbar-btn");
  g_object_unref(pmenu);

  self->btn_hdr_tree = gtk_button_new_with_label("🌲 TREE");
  gtk_widget_add_css_class(self->btn_hdr_tree, "ide-toolbar-btn");
  g_signal_connect(self->btn_hdr_tree, "clicked", G_CALLBACK(on_hdr_tree_clicked), self);

  self->dd_target = gtk_drop_down_new_from_strings((const char *[]){"default", NULL});
  gtk_widget_add_css_class(self->dd_target, "amiga-header-dd");
  g_signal_connect(self->dd_target, "notify::selected", G_CALLBACK(on_dropdown_changed), self);

  self->dd_profile = gtk_drop_down_new_from_strings((const char *[]){"a500_ocs", NULL});
  gtk_widget_add_css_class(self->dd_profile, "amiga-header-dd");
  g_signal_connect(self->dd_profile, "notify::selected", G_CALLBACK(on_dropdown_changed), self);

  self->compat_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->compat_badge, "amiga-compat-warn");
  gtk_widget_set_visible(self->compat_badge, FALSE);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  self->btn_narrow_switch = gtk_button_new_with_label("🔧 INSPECTOR");
  gtk_widget_add_css_class(self->btn_narrow_switch, "ide-toolbar-btn");
  gtk_widget_set_visible(self->btn_narrow_switch, FALSE);
  g_signal_connect(self->btn_narrow_switch, "clicked", G_CALLBACK(on_narrow_switch_clicked), self);

  self->btn_build = gtk_button_new_with_label("🔨 BUILD");
  gtk_widget_add_css_class(self->btn_build, "ide-toolbar-btn");
  gtk_widget_add_css_class(self->btn_build, "accent-btn");
  g_signal_connect(self->btn_build, "clicked", G_CALLBACK(on_hdr_build_clicked), self);

  self->btn_debug = gtk_button_new_with_label("▶ DEBUG");
  gtk_widget_add_css_class(self->btn_debug, "ide-toolbar-btn");
  gtk_widget_add_css_class(self->btn_debug, "run-btn");
  g_signal_connect(self->btn_debug, "clicked", G_CALLBACK(on_hdr_debug_clicked), self);

  self->btn_pause = gtk_button_new_with_label("⏸ PAUSE");
  gtk_widget_add_css_class(self->btn_pause, "ide-toolbar-btn");
  gtk_widget_set_sensitive(self->btn_pause, FALSE);
  g_signal_connect(self->btn_pause, "clicked", G_CALLBACK(on_hdr_pause_clicked), self);

  self->btn_stop = gtk_button_new_with_label("⏹ STOP");
  gtk_widget_add_css_class(self->btn_stop, "ide-toolbar-btn");
  gtk_widget_add_css_class(self->btn_stop, "stop-btn");
  g_signal_connect(self->btn_stop, "clicked", G_CALLBACK(on_hdr_stop_clicked), self);

  self->btn_attach = gtk_button_new_with_label("🔌 ATTACH");
  gtk_widget_add_css_class(self->btn_attach, "ide-toolbar-btn");
  g_signal_connect(self->btn_attach, "clicked", G_CALLBACK(on_hdr_attach_clicked), self);

  self->btn_snapshot = gtk_button_new_with_label("↻ SNAP");
  gtk_widget_add_css_class(self->btn_snapshot, "ide-toolbar-btn");
  g_signal_connect(self->btn_snapshot, "clicked", G_CALLBACK(on_hdr_snapshot_clicked), self);

  GtkWidget *btn_float = gtk_button_new_with_label("🗗 FLOAT / TILE");
  gtk_widget_add_css_class(btn_float, "ide-toolbar-btn");
  g_signal_connect(btn_float, "clicked", G_CALLBACK(on_hdr_float_clicked), self);

  GtkWidget *btn_close = gtk_button_new_with_label("✕");
  gtk_widget_add_css_class(btn_close, "settings-close-btn");
  g_signal_connect(btn_close, "clicked", G_CALLBACK(on_hdr_close_clicked), self);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), self->btn_proj_menu);
  gtk_box_append(GTK_BOX(hdr), self->btn_hdr_tree);
  gtk_box_append(GTK_BOX(hdr), self->dd_target);
  gtk_box_append(GTK_BOX(hdr), self->dd_profile);
  gtk_box_append(GTK_BOX(hdr), self->compat_badge);
  gtk_box_append(GTK_BOX(hdr), spacer);
  gtk_box_append(GTK_BOX(hdr), self->btn_narrow_switch);
  gtk_box_append(GTK_BOX(hdr), self->btn_build);
  gtk_box_append(GTK_BOX(hdr), self->btn_debug);
  gtk_box_append(GTK_BOX(hdr), self->btn_pause);
  gtk_box_append(GTK_BOX(hdr), self->btn_stop);
  gtk_box_append(GTK_BOX(hdr), self->btn_attach);
  gtk_box_append(GTK_BOX(hdr), self->btn_snapshot);
  gtk_box_append(GTK_BOX(hdr), btn_float);
  gtk_box_append(GTK_BOX(hdr), btn_close);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), hdr);
  gtk_box_append(GTK_BOX(outer), handle);

  /* Root stack ("welcome" | "workspace") */
  self->root_stack = gtk_stack_new();
  gtk_widget_set_hexpand(self->root_stack, TRUE);
  gtk_widget_set_vexpand(self->root_stack, TRUE);

  self->welcome_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(self->welcome_slot),
                 amiga_welcome_view_new(self->toolchain, on_act_new_project, on_act_open_project,
                                        on_act_attach, on_hdr_settings_clicked, self));
  gtk_stack_add_named(GTK_STACK(self->root_stack), self->welcome_slot, "welcome");

  /* Workspace container */
  GtkWidget *ws_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->cpu_strip = amiga_cpu_strip_new();
  amiga_cpu_strip_set_callbacks(self->cpu_strip, on_strip_write_regs, on_strip_jump, on_strip_cp, self);
  gtk_box_append(GTK_BOX(ws_vbox), GTK_WIDGET(self->cpu_strip));

  self->main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->main_paned, "ide-paned");
  gtk_widget_set_hexpand(self->main_paned, TRUE);
  gtk_widget_set_vexpand(self->main_paned, TRUE);

  /* Left Sidebar */
  self->sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->sidebar_box, "amiga-sidebar");
  gtk_widget_set_size_request(self->sidebar_box, 204, -1);

  self->sidebar_stack = gtk_stack_new();
  gtk_widget_set_vexpand(self->sidebar_stack, TRUE);

  GtkWidget *proj_sb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sb_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(sb_hdr, "amiga-pane-header");
  GtkWidget *sb_lbl = gtk_label_new("FILES & OUTLINE");
  gtk_widget_set_hexpand(sb_lbl, TRUE);
  gtk_label_set_xalign(GTK_LABEL(sb_lbl), 0.0f);
  GtkWidget *b_sb_setup = gtk_button_new_with_label("⚙");
  gtk_widget_add_css_class(b_sb_setup, "ide-toolbar-btn");
  g_signal_connect(b_sb_setup, "clicked", G_CALLBACK(on_sb_setup_clicked), self);
  gtk_box_append(GTK_BOX(sb_hdr), sb_lbl);
  gtk_box_append(GTK_BOX(sb_hdr), b_sb_setup);
  gtk_box_append(GTK_BOX(proj_sb), sb_hdr);

  self->tree_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(self->tree_slot, TRUE);
  self->file_tree = ide_tree_new(".", on_tree_file_open, self);
  gtk_box_append(GTK_BOX(self->tree_slot), ide_tree_get_widget(self->file_tree));

  GtkWidget *out_hdr = gtk_label_new("68k / C OUTLINE");
  gtk_widget_add_css_class(out_hdr, "amiga-pane-header");
  gtk_label_set_xalign(GTK_LABEL(out_hdr), 0.0f);
  GtkWidget *out_scroll = gtk_scrolled_window_new();
  gtk_widget_set_size_request(out_scroll, -1, 180);
  self->outline_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->outline_list, "amiga-diag-list");
  g_signal_connect(self->outline_list, "row-activated", G_CALLBACK(on_outline_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(out_scroll), self->outline_list);

  gtk_box_append(GTK_BOX(proj_sb), self->tree_slot);
  gtk_box_append(GTK_BOX(proj_sb), out_hdr);
  gtk_box_append(GTK_BOX(proj_sb), out_scroll);
  gtk_stack_add_named(GTK_STACK(self->sidebar_stack), proj_sb, "project");

  GtkWidget *att_sb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *att_hdr = gtk_label_new("LIVE TASKS & MEMORY MAP");
  gtk_widget_add_css_class(att_hdr, "amiga-pane-header");
  gtk_label_set_xalign(GTK_LABEL(att_hdr), 0.0f);
  self->attach_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->attach_list, "amiga-diag-list");
  GtkWidget *att_scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(att_scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(att_scroll), self->attach_list);
  gtk_box_append(GTK_BOX(att_sb), att_hdr);
  gtk_box_append(GTK_BOX(att_sb), att_scroll);
  gtk_stack_add_named(GTK_STACK(self->sidebar_stack), att_sb, "attach");

  gtk_box_append(GTK_BOX(self->sidebar_box), self->sidebar_stack);
  gtk_paned_set_start_child(GTK_PANED(self->main_paned), self->sidebar_box);
  gtk_paned_set_resize_start_child(GTK_PANED(self->main_paned), FALSE);

  /* Right half: vert_paned (center_paned / bottom_drawer) */
  self->vert_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(self->vert_paned, "ide-paned");
  gtk_widget_set_hexpand(self->vert_paned, TRUE);
  gtk_widget_set_vexpand(self->vert_paned, TRUE);

  self->center_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->center_paned, "ide-paned");

  /* Center Stack + Tab Strip */
  GtkWidget *center_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(center_col, TRUE);
  gtk_widget_set_vexpand(center_col, TRUE);

  GtkWidget *ctabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(ctabs, "amiga-drawer-tabs");
  self->btn_tab_source = gtk_toggle_button_new_with_label("📝 SOURCE");
  self->btn_tab_disasm = gtk_toggle_button_new_with_label("⚙ DISASM");
  self->btn_tab_setup = gtk_toggle_button_new_with_label("🛠 kajo-amiga.toml");
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_tab_disasm), GTK_TOGGLE_BUTTON(self->btn_tab_source));
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_tab_setup), GTK_TOGGLE_BUTTON(self->btn_tab_source));
  gtk_widget_add_css_class(self->btn_tab_source, "amiga-seg-btn");
  gtk_widget_add_css_class(self->btn_tab_disasm, "amiga-seg-btn");
  gtk_widget_add_css_class(self->btn_tab_setup, "amiga-seg-btn");
  g_signal_connect(self->btn_tab_source, "toggled", G_CALLBACK(on_center_tab_toggled), self);
  g_signal_connect(self->btn_tab_disasm, "toggled", G_CALLBACK(on_center_tab_toggled), self);
  g_signal_connect(self->btn_tab_setup, "toggled", G_CALLBACK(on_center_tab_toggled), self);
  gtk_box_append(GTK_BOX(ctabs), self->btn_tab_source);
  gtk_box_append(GTK_BOX(ctabs), self->btn_tab_disasm);
  gtk_box_append(GTK_BOX(ctabs), self->btn_tab_setup);
  gtk_box_append(GTK_BOX(center_col), ctabs);

  self->center_stack = gtk_stack_new();
  gtk_stack_set_vhomogeneous(GTK_STACK(self->center_stack), FALSE);
  gtk_stack_set_hhomogeneous(GTK_STACK(self->center_stack), FALSE);
  gtk_widget_set_hexpand(self->center_stack, TRUE);
  gtk_widget_set_vexpand(self->center_stack, TRUE);

  /* Source view + Column 7 Cycle Gutter + Hover */
  GtkWidget *src_scroll = gtk_scrolled_window_new();
  self->source_view = gtk_source_view_new();
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->source_view), 2);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(self->source_view), TRUE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->source_view), TRUE);
  gtk_widget_set_has_tooltip(self->source_view, TRUE);
  g_signal_connect(self->source_view, "query-tooltip",
                   G_CALLBACK(amiga_window_source_query_tooltip), self);

  GtkSourceGutter *lg = gtk_source_view_get_gutter(
      GTK_SOURCE_VIEW(self->source_view), GTK_TEXT_WINDOW_LEFT);
  self->gutter = KAJO_IDE_GUTTER_RENDERER(kajo_ide_gutter_renderer_new(NULL));
  kajo_ide_gutter_renderer_set_breakpoint_callback(
      self->gutter, on_gutter_bp_toggled, self);
  kajo_ide_gutter_renderer_set_annotation_callback(
      self->gutter, amiga_window_gutter_cycle_annotate, 76, self);
  gtk_source_gutter_insert(lg, GTK_SOURCE_GUTTER_RENDERER(self->gutter), 0);

  GtkEventController *sk = gtk_event_controller_key_new();
  g_signal_connect(sk, "key-pressed", G_CALLBACK(on_source_key_pressed), self);
  gtk_widget_add_controller(self->source_view, sk);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(src_scroll), self->source_view);
  gtk_stack_add_named(GTK_STACK(self->center_stack), src_scroll, "source");

  /* Disasm view */
  self->disasm_view = amiga_disasm_view_new();
  AmigaDisasmCallbacks dcbs = {0};
  amiga_window_debug_fill_disasm_callbacks(&dcbs);
  amiga_disasm_view_set_callbacks(self->disasm_view, &dcbs, self);
  gtk_stack_add_named(GTK_STACK(self->center_stack), GTK_WIDGET(self->disasm_view), "disasm");

  /* Project Setup view */
  self->setup_view = amiga_project_setup_view_new(on_proj_saved, self);
  gtk_stack_add_named(GTK_STACK(self->center_stack), GTK_WIDGET(self->setup_view), "setup");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_source), TRUE);

  gtk_box_append(GTK_BOX(center_col), self->center_stack);
  gtk_paned_set_start_child(GTK_PANED(self->center_paned), center_col);
  gtk_paned_set_resize_start_child(GTK_PANED(self->center_paned), TRUE);

  /* Right 5-Tab Hardware Inspector (M4) */
  gtk_paned_set_end_child(GTK_PANED(self->center_paned), amiga_window_inspector_build(self));
  gtk_paned_set_resize_end_child(GTK_PANED(self->center_paned), FALSE);
  gtk_paned_set_start_child(GTK_PANED(self->vert_paned), self->center_paned);
  gtk_paned_set_resize_start_child(GTK_PANED(self->vert_paned), TRUE);

  /* Bottom Drawer */
  GtkWidget *drawer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(drawer_box, "amiga-drawer");
  gtk_widget_set_size_request(drawer_box, -1, 148);

  GtkWidget *dtabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(dtabs, "amiga-drawer-tabs");
  self->btn_drw_build = gtk_toggle_button_new_with_label("🔨 BUILD & DIAGNOSTICS");
  self->btn_drw_hex = gtk_toggle_button_new_with_label("▓ MEMORY HEX");
  self->btn_drw_debug = gtk_toggle_button_new_with_label("🐞 EXCEPTIONS & CHECKPOINTS");
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_drw_hex), GTK_TOGGLE_BUTTON(self->btn_drw_build));
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_drw_debug), GTK_TOGGLE_BUTTON(self->btn_drw_build));
  gtk_widget_add_css_class(self->btn_drw_build, "amiga-seg-btn");
  gtk_widget_add_css_class(self->btn_drw_hex, "amiga-seg-btn");
  gtk_widget_add_css_class(self->btn_drw_debug, "amiga-seg-btn");
  g_signal_connect(self->btn_drw_build, "toggled", G_CALLBACK(on_drawer_tab_toggled), self);
  g_signal_connect(self->btn_drw_hex, "toggled", G_CALLBACK(on_drawer_tab_toggled), self);
  g_signal_connect(self->btn_drw_debug, "toggled", G_CALLBACK(on_drawer_tab_toggled), self);
  gtk_box_append(GTK_BOX(dtabs), self->btn_drw_build);
  gtk_box_append(GTK_BOX(dtabs), self->btn_drw_hex);
  gtk_box_append(GTK_BOX(dtabs), self->btn_drw_debug);
  gtk_box_append(GTK_BOX(drawer_box), dtabs);

  self->drawer_stack = gtk_stack_new();
  gtk_widget_set_vexpand(self->drawer_stack, TRUE);

  GtkWidget *build_split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(build_split, "ide-paned");
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
  gtk_paned_set_position(GTK_PANED(build_split), 640);
  gtk_stack_add_named(GTK_STACK(self->drawer_stack), build_split, "build");

  self->mem_view = amiga_mem_view_new();
  amiga_mem_view_set_callbacks(self->mem_view, on_mem_read_cb, on_mem_write_cb, on_mem_watch_cb, self);
  gtk_stack_add_named(GTK_STACK(self->drawer_stack), GTK_WIDGET(self->mem_view), "hex");

  GtkWidget *dbg_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(dbg_box, 10);
  gtk_widget_set_margin_start(dbg_box, 10);
  GtkWidget *exc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  static const char *const enames[5] = {
    "Bus (#2)", "Addr (#3)", "Illegal (#4)", "Div0 (#5)", "Privilege (#8)"
  };
  for (int i = 0; i < 5; i++) {
    self->exc_checks[i] = gtk_check_button_new_with_label(enames[i]);
    g_signal_connect(self->exc_checks[i], "toggled",
                     G_CALLBACK(amiga_window_debug_on_exc_toggled), self);
    gtk_box_append(GTK_BOX(exc_row), self->exc_checks[i]);
  }
  self->lbl_exc_last = gtk_label_new("No captured 68000 exception.");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_exc_last), 0.0f);
  gtk_box_append(GTK_BOX(dbg_box), exc_row);
  gtk_box_append(GTK_BOX(dbg_box), self->lbl_exc_last);
  gtk_stack_add_named(GTK_STACK(self->drawer_stack), dbg_box, "debug");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_drw_build), TRUE);

  gtk_box_append(GTK_BOX(drawer_box), self->drawer_stack);
  gtk_paned_set_end_child(GTK_PANED(self->vert_paned), drawer_box);
  gtk_paned_set_resize_end_child(GTK_PANED(self->vert_paned), FALSE);

  gtk_paned_set_end_child(GTK_PANED(self->main_paned), self->vert_paned);
  gtk_paned_set_resize_end_child(GTK_PANED(self->main_paned), TRUE);

  gtk_box_append(GTK_BOX(ws_vbox), self->main_paned);
  gtk_stack_add_named(GTK_STACK(self->root_stack), ws_vbox, "workspace");
  gtk_box_append(GTK_BOX(outer), self->root_stack);

  /* Toast Banner + Status Bar */
  self->toast_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->toast_bar, "amiga-toast-banner");
  gtk_widget_set_visible(self->toast_bar, FALSE);
  self->toast_lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->toast_lbl), 0.0f);
  gtk_widget_set_hexpand(self->toast_lbl, TRUE);
  gtk_box_append(GTK_BOX(self->toast_bar), self->toast_lbl);
  gtk_box_append(GTK_BOX(outer), self->toast_bar);

  GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(sbar, "amiga-status-bar");
  gtk_widget_set_valign(sbar, GTK_ALIGN_CENTER);

  self->mode_pill = gtk_label_new("NORMAL");
  gtk_widget_add_css_class(self->mode_pill, "amiga-mode-pill");
  self->lbl_ws_state = gtk_label_new("Welcome");
  gtk_widget_set_hexpand(self->lbl_ws_state, TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_ws_state), 0.0f);
  self->lbl_sel_cycles = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_sel_cycles, "amiga-disasm-cycle-col");
  gtk_widget_set_visible(self->lbl_sel_cycles, FALSE);
  self->lbl_cursor = gtk_label_new("Ln 1, Col 1");

  gtk_box_append(GTK_BOX(sbar), self->mode_pill);
  gtk_box_append(GTK_BOX(sbar), self->lbl_ws_state);
  gtk_box_append(GTK_BOX(sbar), self->lbl_sel_cycles);
  gtk_box_append(GTK_BOX(sbar), self->lbl_cursor);
  gtk_box_append(GTK_BOX(outer), sbar);

  gtk_window_set_child(GTK_WINDOW(self), outer);

  GtkEventController *wk = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(wk, GTK_PHASE_CAPTURE);
  g_signal_connect(wk, "key-pressed", G_CALLBACK(on_window_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), wk);

  g_signal_connect(self, "notify::default-width", G_CALLBACK(on_window_width_changed), NULL);
  g_signal_connect(self->source_view, "notify::width-request", G_CALLBACK(on_window_width_changed), NULL);
  amiga_window_sync_header_workspace(self, false);
}

static void
kajo_amiga_window_dispose(GObject *obj)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(obj);
  if (self->cfg_sub_id) {
    kajo_config_store_unsubscribe(NULL, self->cfg_sub_id);
    self->cfg_sub_id = 0;
  }
  g_clear_pointer(&self->inherited_scheme, g_free);
  g_clear_handle_id(&self->toast_timer_id, g_source_remove);
  if (self->build_cancel) {
    g_cancellable_cancel(self->build_cancel);
    g_clear_object(&self->build_cancel);
  }
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_DISCONNECTED) {
    amiga_rsp_client_set_stop_callback(self->rsp, NULL, NULL);
  }
  g_clear_object(&self->rsp);
  if (self->runner) {
    amiga_runner_set_callbacks(self->runner, NULL, NULL, NULL);
    if (amiga_runner_owns_process(self->runner)) amiga_runner_stop(self->runner);
    g_clear_object(&self->runner);
  }
  g_clear_pointer(&self->last_chip_ram, g_bytes_unref);
  g_clear_pointer(&self->active_buf, ide_buffer_free);
  g_clear_pointer(&self->file_tree, ide_tree_free);
  g_clear_pointer(&self->fsm, ide_fsm_free);
  g_clear_pointer(&self->yank_reg, g_free);
  g_clear_pointer(&self->symbols, amiga_symbols_free);
  g_clear_pointer(&self->project, amiga_project_free);
  g_clear_pointer(&self->toolchain, amiga_toolchain_status_free);
  g_clear_pointer(&self->diags, g_ptr_array_unref);
  g_clear_pointer(&self->bp_addrs, g_hash_table_unref);
  g_free(self->attach_host);
  self->attach_host = NULL;
  amiga_window_debug_dispose(self);
  G_OBJECT_CLASS(kajo_amiga_window_parent_class)->dispose(obj);
}

static void
kajo_amiga_window_class_init(KajoAmigaWindowClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = kajo_amiga_window_dispose;
}

static void
apply_global_config(KajoAmigaWindow *self)
{
  g_autoptr(KajoAmigaCfgRef) acfg = kajo_config_store_acquire_amiga(NULL);
  if (!acfg) return;

  g_free(self->cfg_amiberry_bin);
  self->cfg_amiberry_bin = g_strdup(acfg->amiberry_bin);
  g_free(self->cfg_kickstart_a500);
  self->cfg_kickstart_a500 = g_strdup(acfg->kickstart_a500);
  g_free(self->cfg_kickstart_a1200);
  self->cfg_kickstart_a1200 = g_strdup(acfg->kickstart_a1200);
  g_free(self->cfg_gdb_host);
  self->cfg_gdb_host = g_strdup((acfg->gdb_host && acfg->gdb_host[0]) ? acfg->gdb_host : "127.0.0.1");
  self->cfg_gdb_port = acfg->gdb_port ? (uint16_t)acfg->gdb_port : 2345;
  self->cfg_entry_break_mode = acfg->entry_break_mode;
  self->cfg_capture_guest_output = acfg->capture_guest_output;
  self->cfg_enable_trace_history = acfg->enable_trace_history;
  self->cfg_default_exc_mask = (uint8_t)acfg->default_exception_mask;
  self->cfg_trace_blitter_writes = acfg->trace_blitter_writes;
  self->cfg_hex_grouping = (acfg->default_hex_grouping == KAJO_AMIGA_HEX_8B) ? 1
                         : (acfg->default_hex_grouping == KAJO_AMIGA_HEX_32B) ? 4 : 2;
  self->cfg_bitplane_grid_16px = acfg->bitplane_grid_16px;
  self->cfg_enable_m68k_lsp = acfg->enable_m68k_lsp;
  self->cfg_enable_clangd = acfg->enable_clangd;
  self->show_cycle_gutter = acfg->show_cycle_gutter;
  self->show_sel_cycles = acfg->show_selection_cycle_sum;
  self->enable_hover_decoders = acfg->enable_hover_decoders;

  if (self->mem_view) amiga_mem_view_set_grouping(self->mem_view, self->cfg_hex_grouping);
  if (self->bpl_view) amiga_bitplane_view_set_grid(self->bpl_view, self->cfg_bitplane_grid_16px);
  amiga_window_debug_apply_exc_mask(self, self->cfg_default_exc_mask);

  g_auto(GStrv) extra = (acfg->extra_bin_paths && acfg->extra_bin_paths[0])
                            ? g_strsplit(acfg->extra_bin_paths, ":", -1)
                            : NULL;
  AmigaToolchainStatus *ntc = amiga_project_probe_toolchain(
      extra ? (const char *const *)extra : NULL);
  g_clear_pointer(&self->toolchain, amiga_toolchain_status_free);
  self->toolchain = ntc;
  if (self->toolchain && self->cfg_amiberry_bin && self->cfg_amiberry_bin[0]) {
    g_autofree char *resolved = g_find_program_in_path(self->cfg_amiberry_bin);
    if (resolved) {
      g_free(self->toolchain->amiberry);
      self->toolchain->amiberry = g_steal_pointer(&resolved);
    } else if (g_file_test(self->cfg_amiberry_bin, G_FILE_TEST_IS_EXECUTABLE)) {
      g_free(self->toolchain->amiberry);
      self->toolchain->amiberry = g_strdup(self->cfg_amiberry_bin);
    }
  }

  if (self->hrm_view && (!self->project || !self->project->hrm_dir || !self->project->hrm_dir[0])) {
    amiga_hrm_view_set_hrm_dir(self->hrm_view, acfg->hrm_dir);
  }
  if (!self->narrow_mode && self->inspector_box) {
    gtk_widget_set_visible(self->inspector_box, acfg->inspector_visible);
  }

  const char *scheme = "kajo-dark";
  int tab_w = 2;
  if (acfg->inherit_ide_editor_prefs) {
    g_autoptr(KajoIdeCfgRef) icfg = kajo_config_store_acquire_ide(NULL);
    if (icfg) {
      if (icfg->color_scheme && icfg->color_scheme[0]) scheme = icfg->color_scheme;
      if (icfg->tab_width > 0) tab_w = icfg->tab_width;
    }
  }
  g_free(self->inherited_scheme);
  self->inherited_scheme = g_strdup(scheme);
  self->inherited_tab_width = tab_w;
  if (self->source_view) {
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->source_view), (guint)tab_w);
    if (self->active_buf) ide_buffer_set_style_scheme(self->active_buf, scheme);
    gtk_widget_queue_draw(self->source_view);
  }
  amiga_window_lsp_sync_project(self);
  update_cursor_and_selection_cycles(self);
}

static void
on_global_cfg_changed(KajoConfigStore *store G_GNUC_UNUSED,
                      KajoConfigDomain changed G_GNUC_UNUSED,
                      gpointer         ud)
{
  GWeakRef *weak = ud;
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  if (self) apply_global_config(self);
}

static void
cfg_weak_free(gpointer data)
{
  GWeakRef *weak = data;
  g_weak_ref_clear(weak);
  g_free(weak);
}

static void
kajo_amiga_window_init(KajoAmigaWindow *self)
{
  amiga_window_debug_init(self);
  self->toolchain = amiga_project_probe_toolchain(NULL);
  self->symbols = amiga_symbols_new();
  self->runner = amiga_runner_new();
  self->rsp = amiga_rsp_client_new();
  amiga_rsp_client_set_stop_callback(self->rsp, amiga_window_debug_on_rsp_stop, self);
  self->bp_addrs = g_hash_table_new(g_direct_hash, g_direct_equal);
  self->diags = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_build_diag_free);
  self->fsm = ide_fsm_new();
  self->show_cycle_gutter = true;
  self->show_sel_cycles = true;
  self->enable_hover_decoders = true;
  self->inherited_tab_width = 2;

  build_ui(self);

  self->editor_host.source_view = GTK_SOURCE_VIEW(self->source_view);
  self->editor_host.fsm = self->fsm;
  self->editor_host.yank_register = &self->yank_reg;

  apply_global_config(self);
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  self->cfg_sub_id = kajo_config_store_subscribe(
      NULL, (KajoConfigDomain)(KAJO_CONFIG_DOMAIN_AMIGA | KAJO_CONFIG_DOMAIN_IDE),
      on_global_cfg_changed, weak, cfg_weak_free);
}

KajoAmigaWindow *
kajo_amiga_window_new(GtkApplication *app)
{
  return g_object_new(KAJO_TYPE_AMIGA_WINDOW, "application", app, NULL);
}

void
kajo_amiga_window_open_project_dir(KajoAmigaWindow *self, const char *dir_path)
{
  g_return_if_fail(KAJO_IS_AMIGA_WINDOW(self));
  g_autoptr(GError) err = NULL;
  AmigaProject *proj = amiga_project_load_dir(dir_path, &err);
  if (!proj) {
    show_toast(self, err ? err->message : "Cannot load Amiga project");
    return;
  }
  g_clear_pointer(&self->project, amiga_project_free);
  self->project = proj;
  self->attach_mode = false;

  amiga_project_write_compile_commands(proj, self->toolchain, NULL);
  ide_tree_set_workspace_root(self->file_tree, proj->root_dir);
  amiga_window_populate_project_dropdowns(self);
  amiga_project_setup_view_bind(self->setup_view, proj, self->toolchain);
  if (proj->hrm_dir && proj->hrm_dir[0]) {
    g_autofree char *abs_hrm = g_path_is_absolute(proj->hrm_dir)
                                   ? g_strdup(proj->hrm_dir)
                                   : g_build_filename(proj->root_dir, proj->hrm_dir, NULL);
    amiga_hrm_view_set_hrm_dir(self->hrm_view, abs_hrm);
  }
  amiga_window_debug_reload_symbols(self);
  amiga_window_lsp_sync_project(self);

  gtk_stack_set_visible_child_name(GTK_STACK(self->sidebar_stack), "project");
  gtk_stack_set_visible_child_name(GTK_STACK(self->root_stack), "workspace");
  amiga_window_sync_header_workspace(self, true);
  amiga_window_sync_mode_pill(self);
  gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), proj->name ? proj->name : dir_path);

  if (proj->targets->len > 0) {
    AmigaTarget *t = g_ptr_array_index(proj->targets, 0);
    if (t->bitplane_symbol) {
      amiga_bitplane_view_set_symbol_hint(self->bpl_view, t->bitplane_symbol);
    }
    if (t->sources->len > 0) {
      const char *rel = g_ptr_array_index(t->sources, 0);
      g_autofree char *abs = g_build_filename(proj->root_dir, rel, NULL);
      open_file_in_editor(self, abs, 1);
    }
  }
}

void
kajo_amiga_window_open_attach(KajoAmigaWindow *self, const char *host, uint16_t port)
{
  g_return_if_fail(KAJO_IS_AMIGA_WINDOW(self));
  const char *h = (host && host[0]) ? host
                : (self->cfg_gdb_host && self->cfg_gdb_host[0]) ? self->cfg_gdb_host : "127.0.0.1";
  uint16_t p = port ? port : (self->cfg_gdb_port ? self->cfg_gdb_port : 2345);
  amiga_window_debug_connect_with_retry(self, h, p, true, NULL);
}
