#include "amiga_window_private.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
source_bp_free(AmigaSourceBp *bp)
{
  if (!bp) return;
  g_free(bp->file);
  g_free(bp);
}

static bool
same_file_path(const char *a, const char *b)
{
  if (!a || !b) return false;
  if (g_strcmp0(a, b) == 0) return true;
  g_autofree char *ba = g_path_get_basename(a);
  g_autofree char *bb = g_path_get_basename(b);
  return g_strcmp0(ba, bb) == 0;
}

static char *
resolve_project_path(const KajoAmigaWindow *self, const char *rel_or_abs)
{
  if (!rel_or_abs || !rel_or_abs[0]) return NULL;
  if (g_path_is_absolute(rel_or_abs)) return g_strdup(rel_or_abs);
  const char *root = (self->project && self->project->root_dir) ? self->project->root_dir : ".";
  return g_build_filename(root, rel_or_abs, NULL);
}

static const char *
resolve_kickstart_path(const KajoAmigaWindow *self, const AmigaHwProfile *prof)
{
  if (prof && prof->kickstart && prof->kickstart[0]) {
    if (g_file_test(prof->kickstart, G_FILE_TEST_IS_REGULAR)) {
      return prof->kickstart;
    }
    if (self->project && self->project->root_dir) {
      g_autofree char *rel = g_build_filename(self->project->root_dir, prof->kickstart, NULL);
      if (g_file_test(rel, G_FILE_TEST_IS_REGULAR)) {
        /* Static cache is unsafe for multiple instances; store temp on profile or return global config if missing */
        return self->cfg_kickstart_a500;
      }
    }
  }
  bool want_aga = prof && ((prof->model && (g_strcmp0(prof->model, "A1200") == 0 || g_strcmp0(prof->model, "A4000") == 0)) ||
                           (prof->chipset && g_ascii_strcasecmp(prof->chipset, "AGA") == 0));
  if (want_aga && self->cfg_kickstart_a1200 && g_file_test(self->cfg_kickstart_a1200, G_FILE_TEST_IS_REGULAR)) {
    return self->cfg_kickstart_a1200;
  }
  if (self->cfg_kickstart_a500 && g_file_test(self->cfg_kickstart_a500, G_FILE_TEST_IS_REGULAR)) {
    return self->cfg_kickstart_a500;
  }
  return NULL;
}

static char *
resolve_kickstart_owned(const KajoAmigaWindow *self, const AmigaHwProfile *prof)
{
  if (prof && prof->kickstart && prof->kickstart[0]) {
    if (g_file_test(prof->kickstart, G_FILE_TEST_IS_REGULAR)) {
      return g_strdup(prof->kickstart);
    }
    if (self->project && self->project->root_dir) {
      g_autofree char *rel = g_build_filename(self->project->root_dir, prof->kickstart, NULL);
      if (g_file_test(rel, G_FILE_TEST_IS_REGULAR)) {
        return g_steal_pointer(&rel);
      }
    }
  }
  const char *fb = resolve_kickstart_path(self, prof);
  return fb ? g_strdup(fb) : NULL;
}

void
amiga_window_debug_init(KajoAmigaWindow *self)
{
  self->source_bps = g_ptr_array_new_with_free_func((GDestroyNotify)source_bp_free);
  self->last_disasm_rows = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_disasm_row_free);
  self->cfg_gdb_host = g_strdup("127.0.0.1");
  self->cfg_gdb_port = 2345;
  self->cfg_capture_guest_output = true;
  self->cfg_hex_grouping = 2;
  self->cfg_enable_m68k_lsp = true;
  self->cfg_enable_clangd = true;
}

void
amiga_window_debug_dispose(KajoAmigaWindow *self)
{
  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  amiga_window_lsp_shutdown(self);
  g_clear_pointer(&self->source_bps, g_ptr_array_unref);
  g_clear_pointer(&self->last_disasm_rows, g_ptr_array_unref);
  g_clear_pointer(&self->pending_entry_exe, g_free);
  g_clear_pointer(&self->cfg_amiberry_bin, g_free);
  g_clear_pointer(&self->cfg_kickstart_a500, g_free);
  g_clear_pointer(&self->cfg_kickstart_a1200, g_free);
  g_clear_pointer(&self->cfg_gdb_host, g_free);
}

void
amiga_window_debug_sync_ui(KajoAmigaWindow *self)
{
  if (!self->rsp) return;
  AmigaRspState st = amiga_rsp_client_get_state(self->rsp);
  bool connected = (st != AMIGA_RSP_DISCONNECTED);
  bool running = (st == AMIGA_RSP_RUNNING);
  bool stopped = (st == AMIGA_RSP_STOPPED);
  bool has_ws = (self->project != NULL);

  if (self->btn_pause) gtk_widget_set_sensitive(self->btn_pause, running);
  if (self->btn_stop) gtk_widget_set_sensitive(self->btn_stop, connected || amiga_runner_is_running(self->runner));
  if (self->btn_snapshot) gtk_widget_set_sensitive(self->btn_snapshot, stopped);
  if (self->btn_debug) gtk_widget_set_sensitive(self->btn_debug, has_ws || stopped);
}

static void
load_one_symbol_file(AmigaSymbolTable *syms, const char *abs_path)
{
  if (!syms || !abs_path || !g_file_test(abs_path, G_FILE_TEST_IS_REGULAR)) return;
  if (g_str_has_suffix(abs_path, ".elf")) {
    amiga_symbols_load_elf(syms, abs_path, NULL);
  } else if (g_str_has_suffix(abs_path, ".map")) {
    amiga_symbols_load_map(syms, abs_path, NULL);
  } else {
    amiga_symbols_load_hunk(syms, abs_path, NULL);
  }
}

void
amiga_window_debug_reload_symbols(KajoAmigaWindow *self)
{
  g_clear_pointer(&self->symbols, amiga_symbols_free);
  self->symbols = amiga_symbols_new();
  if (!self->project) return;

  const AmigaTarget *t = amiga_window_get_selected_target(self);
  if (!t) return;

  g_autofree char *elf_rel = (t->elf_path && t->elf_path[0])
                                 ? g_strdup(t->elf_path)
                                 : g_strdup_printf("out/%s.elf", t->name);
  g_autofree char *exe_rel = (t->exe_path && t->exe_path[0])
                                 ? g_strdup(t->exe_path)
                                 : g_strdup_printf("out/%s.exe", t->name);
  g_autofree char *map_rel = g_strdup_printf("out/%s.map", t->name);

  g_autofree char *elf_abs = resolve_project_path(self, elf_rel);
  g_autofree char *exe_abs = resolve_project_path(self, exe_rel);
  g_autofree char *map_abs = resolve_project_path(self, map_rel);

  bool loaded_primary = false;
  if (elf_abs && g_file_test(elf_abs, G_FILE_TEST_IS_REGULAR)) {
    loaded_primary = amiga_symbols_load_elf(self->symbols, elf_abs, NULL);
  }
  if (!loaded_primary && exe_abs && g_file_test(exe_abs, G_FILE_TEST_IS_REGULAR)) {
    loaded_primary = amiga_symbols_load_hunk(self->symbols, exe_abs, NULL);
  }
  if (map_abs && g_file_test(map_abs, G_FILE_TEST_IS_REGULAR)) {
    amiga_symbols_load_map(self->symbols, map_abs, NULL);
  }
  for (guint i = 0; t->symbol_files && i < t->symbol_files->len; i++) {
    g_autofree char *extra_abs = resolve_project_path(self, g_ptr_array_index(t->symbol_files, i));
    load_one_symbol_file(self->symbols, extra_abs);
  }
}

/* ─── Arm / Sync Breakpoints & Exceptions ─── */

static size_t
arm_all_source_breakpoints(KajoAmigaWindow *self)
{
  if (!self->source_bps || !self->symbols) return 0;
  size_t armed_cnt = 0;
  for (guint i = 0; i < self->source_bps->len; i++) {
    AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, i);
    uint32_t saddr = 0, eaddr = 0;
    if (amiga_symbols_line_to_addr(self->symbols, bp->file, bp->line, &saddr, &eaddr)) {
      saddr &= ~1u;
      bp->addr = saddr;
      bp->armed = true;
      g_hash_table_insert(self->bp_addrs, GUINT_TO_POINTER(saddr), GINT_TO_POINTER(1));
      if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
        amiga_rsp_client_bp_set_async(self->rsp, saddr, NULL, NULL, NULL);
      }
      armed_cnt++;
    }
  }
  return armed_cnt;
}

void
amiga_window_debug_sync_buffer_breakpoints(KajoAmigaWindow *self, IdeBuffer *buf)
{
  if (!self || !buf || !self->source_bps) return;
  const char *path = ide_buffer_get_file_path(buf);
  if (!path) return;

  g_autoptr(GArray) cur = ide_buffer_get_breakpoints(buf);
  for (guint i = 0; cur && i < cur->len; i++) {
    guint ln = g_array_index(cur, guint, i);
    bool keep = false;
    for (guint j = 0; j < self->source_bps->len; j++) {
      AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, j);
      if (bp->line == ln && same_file_path(bp->file, path)) { keep = true; break; }
    }
    if (!keep) ide_buffer_toggle_breakpoint(buf, ln);
  }
  for (guint j = 0; j < self->source_bps->len; j++) {
    AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, j);
    if (same_file_path(bp->file, path) && !ide_buffer_has_breakpoint(buf, bp->line)) {
      ide_buffer_toggle_breakpoint(buf, bp->line);
    }
  }
  if (self->has_last_regs && self->symbols) {
    const char *pc_file = NULL;
    uint32_t pc_line = 0;
    if (amiga_symbols_addr_to_line(self->symbols, self->last_regs.pc, &pc_file, &pc_line) &&
        same_file_path(pc_file, path)) {
      ide_buffer_set_dap_pc_line(buf, pc_line);
    } else {
      ide_buffer_set_dap_pc_line(buf, 0);
    }
  }
}

void
amiga_window_debug_toggle_source_bp(KajoAmigaWindow *self, IdeBuffer *buf, guint line_1based)
{
  if (!self || !buf || line_1based == 0) return;
  const char *path = ide_buffer_get_file_path(buf);
  if (!path) return;

  /* Ensure buffer gutter marker matches new desired state */
  guint existing_idx = G_MAXUINT;
  for (guint i = 0; self->source_bps && i < self->source_bps->len; i++) {
    AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, i);
    if (bp->line == line_1based && same_file_path(bp->file, path)) {
      existing_idx = i;
      break;
    }
  }

  if (existing_idx != G_MAXUINT) {
    AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, existing_idx);
    uint32_t addr = bp->addr;
    bool armed = bp->armed;
    g_ptr_array_remove_index(self->source_bps, existing_idx);
    if (armed && addr != 0) {
      g_hash_table_remove(self->bp_addrs, GUINT_TO_POINTER(addr));
      if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
        amiga_rsp_client_bp_remove_async(self->rsp, addr, NULL, NULL, NULL);
      }
    }
    if (ide_buffer_has_breakpoint(buf, line_1based)) {
      ide_buffer_toggle_breakpoint(buf, line_1based);
    }
    amiga_window_show_toast(self, "○ Breakpoint removed");
  } else {
    AmigaSourceBp *bp = g_new0(AmigaSourceBp, 1);
    bp->file = g_strdup(path);
    bp->line = line_1based;
    uint32_t saddr = 0, eaddr = 0;
    if (self->symbols &&
        amiga_symbols_line_to_addr(self->symbols, path, line_1based, &saddr, &eaddr)) {
      saddr &= ~1u;
      bp->addr = saddr;
      bp->armed = true;
      g_hash_table_insert(self->bp_addrs, GUINT_TO_POINTER(saddr), GINT_TO_POINTER(1));
      if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
        amiga_rsp_client_bp_set_async(self->rsp, saddr, NULL, NULL, NULL);
      }
      g_autofree char *msg = g_strdup_printf("● Breakpoint set at %s:%u ($%06X)",
                                             g_path_get_basename(path), line_1based, saddr);
      amiga_window_show_toast(self, msg);
    } else {
      amiga_window_show_toast(self, "● Breakpoint queued (binds on entry/segments)");
    }
    g_ptr_array_add(self->source_bps, bp);
    if (!ide_buffer_has_breakpoint(buf, line_1based)) {
      ide_buffer_toggle_breakpoint(buf, line_1based);
    }
  }

  if (self->last_disasm_rows && self->last_disasm_rows->len > 0) {
    uint32_t pc = self->has_last_regs ? self->last_regs.pc
                                      : amiga_disasm_view_get_base_addr(self->disasm_view);
    amiga_disasm_view_set_rows(self->disasm_view, self->last_disasm_rows, pc, self->bp_addrs);
  }
  gtk_widget_queue_draw(self->source_view);
}

void
amiga_window_debug_apply_exc_mask(KajoAmigaWindow *self, uint8_t mask)
{
  if (!self) return;
  self->exc_mask = mask;
  self->updating_exc_ui = true;
  static const uint8_t bits[5] = { 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 7 };
  for (int i = 0; i < 5; i++) {
    if (self->exc_checks[i]) {
      gtk_check_button_set_active(GTK_CHECK_BUTTON(self->exc_checks[i]), (mask & bits[i]) != 0);
    }
  }
  self->updating_exc_ui = false;
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    g_autofree char *cmd = g_strdup_printf("exception %u", (unsigned)mask);
    amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, NULL, NULL);
  }
}

void
amiga_window_debug_on_exc_toggled(GtkCheckButton *cb G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->updating_exc_ui) return;
  uint8_t mask = 0;
  static const uint8_t bits[5] = { 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 7 };
  for (int i = 0; i < 5; i++) {
    if (self->exc_checks[i] &&
        gtk_check_button_get_active(GTK_CHECK_BUTTON(self->exc_checks[i]))) {
      mask |= bits[i];
    }
  }
  self->exc_mask = mask;
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    g_autofree char *cmd = g_strdup_printf("exception %u", (unsigned)mask);
    amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, NULL, NULL);
    g_autofree char *msg = g_strdup_printf("68000 exception trap mask set ($%02X)", mask);
    amiga_window_show_toast(self, msg);
  }
}

/* ─── Snapshot Refresh Callbacks ─── */

static void
on_custom_mem_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) bytes = amiga_rsp_client_read_mem_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !bytes || g_bytes_get_size(bytes) < 512) return;

  AmigaCustomSnapshot snap = {0};
  amiga_rsp_decode_custom_snapshot(g_bytes_get_data(bytes, NULL), &snap);
  amiga_mem_view_set_regs_and_custom(self->mem_view, NULL, &snap);
  amiga_window_inspector_apply_snapshot(self, &snap, true);
}

static void
on_exec_status_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !txt) return;

  AmigaExecStatus st = {0};
  if (amiga_rsp_parse_execution_status(txt, &st)) {
    self->last_exec_st = st;
    self->has_last_exec_st = true;
    if (self->has_last_regs) {
      amiga_cpu_strip_update(self->cpu_strip, &self->last_regs, &self->last_exec_st,
                             self->symbols, amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED);
    }
  }
}

static void
on_regs_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  AmigaCpuRegs regs = {0};
  bool ok = amiga_rsp_client_read_regs_finish(AMIGA_RSP_CLIENT(src), res, &regs, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !ok) return;

  self->last_regs = regs;
  self->has_last_regs = true;

  amiga_cpu_strip_update(self->cpu_strip, &regs,
                         self->has_last_exec_st ? &self->last_exec_st : NULL,
                         self->symbols, true);
  amiga_mem_view_set_regs_and_custom(self->mem_view, &regs, NULL);
  if (amiga_disasm_view_get_follow_pc(self->disasm_view)) {
    amiga_disasm_view_go_to(self->disasm_view, regs.pc, false);
  }

  /* Sync Source PC highlight (and follow file if SOURCE tab is active) */
  if (self->symbols) {
    const char *src_file = NULL;
    uint32_t src_line = 0;
    if (amiga_symbols_addr_to_line(self->symbols, regs.pc, &src_file, &src_line) && src_file) {
      g_autofree char *abs_src = resolve_project_path(self, src_file);
      if (self->active_buf &&
          same_file_path(ide_buffer_get_file_path(self->active_buf), abs_src ? abs_src : src_file)) {
        ide_buffer_set_dap_pc_line(self->active_buf, src_line);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_tab_source))) {
          GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
          GtkTextIter it;
          gtk_text_buffer_get_iter_at_line(tb, &it, (int)(src_line - 1u));
          gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view), &it, 0.25, FALSE, 0, 0.5);
        }
        gtk_widget_queue_draw(self->source_view);
      } else if (abs_src && g_file_test(abs_src, G_FILE_TEST_IS_REGULAR) &&
                 gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_tab_source))) {
        amiga_window_open_file_at_line(self, abs_src, src_line);
      }
    } else if (self->active_buf) {
      ide_buffer_set_dap_pc_line(self->active_buf, 0);
      gtk_widget_queue_draw(self->source_view);
    }
  }
}

static void
on_tasks_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !txt || !self->attach_list) return;

  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->attach_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->attach_list), ch);
  }
  g_autoptr(GPtrArray) tasks = NULL;
  if (amiga_rsp_parse_tasks(txt, &tasks) && tasks) {
    for (guint i = 0; i < tasks->len; i++) {
      const AmigaTaskInfo *ti = g_ptr_array_index(tasks, i);
      g_autofree char *row = g_strdup_printf("[%s] %s · SP $%08X",
                                             ti->list_name ? ti->list_name : "TASK",
                                             ti->name ? ti->name : "?",
                                             ti->saved_sp);
      GtkWidget *l = gtk_label_new(row);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
      gtk_list_box_append(GTK_LIST_BOX(self->attach_list), l);
    }
  }
}

static void
on_memmap_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !txt || !self->attach_list) return;

  g_autoptr(GPtrArray) regs = NULL;
  if (amiga_rsp_parse_memory_map(txt, &regs) && regs) {
    for (guint i = 0; i < regs->len && i < 12; i++) {
      const AmigaMemRegion *mr = g_ptr_array_index(regs, i);
      g_autofree char *row = g_strdup_printf("[MAP] $%06X (%uK) %s",
                                             mr->start, mr->length / 1024u,
                                             mr->name ? mr->name : (mr->type ? mr->type : "RAM"));
      GtkWidget *l = gtk_label_new(row);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
      gtk_list_box_append(GTK_LIST_BOX(self->attach_list), l);
    }
  }
}

void
amiga_window_debug_on_disasm_rcmd(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !txt) return;

  g_autoptr(GPtrArray) rows = NULL;
  if (amiga_disasm_parse(txt, self->symbols, &rows) && rows) {
    g_clear_pointer(&self->last_disasm_rows, g_ptr_array_unref);
    self->last_disasm_rows = g_steal_pointer(&rows);
    uint32_t pc = self->has_last_regs ? self->last_regs.pc
                                      : amiga_disasm_view_get_base_addr(self->disasm_view);
    amiga_disasm_view_set_rows(self->disasm_view, self->last_disasm_rows, pc, self->bp_addrs);
  }
}

void
amiga_window_debug_refresh_snapshot(KajoAmigaWindow *self)
{
  amiga_window_debug_sync_ui(self);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;

  GWeakRef *w1 = g_new0(GWeakRef, 1); g_weak_ref_init(w1, self);
  amiga_rsp_client_read_regs_async(self->rsp, NULL, on_regs_done, w1);

  GWeakRef *w2 = g_new0(GWeakRef, 1); g_weak_ref_init(w2, self);
  amiga_rsp_client_rcmd_async(self->rsp, "execution-status", NULL, on_exec_status_done, w2);

  GWeakRef *w3 = g_new0(GWeakRef, 1); g_weak_ref_init(w3, self);
  amiga_rsp_client_read_mem_async(self->rsp, 0xDFF000u, 0x200u, NULL, on_custom_mem_done, w3);

  if (self->attach_mode) {
    GWeakRef *w4 = g_new0(GWeakRef, 1); g_weak_ref_init(w4, self);
    amiga_rsp_client_rcmd_async(self->rsp, "tasks", NULL, on_tasks_done, w4);
    GWeakRef *w5 = g_new0(GWeakRef, 1); g_weak_ref_init(w5, self);
    amiga_rsp_client_rcmd_async(self->rsp, "memory-map", NULL, on_memmap_done, w5);
  }
}

/* ─── Segment Relocation & Entry Stop Handling ─── */

static void
on_segments_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self) return;

  if (txt) {
    AmigaSegmentList *sl = NULL;
    if (amiga_rsp_parse_segments(txt, &sl) && sl) {
      amiga_symbols_apply_segments(self->symbols, sl);
      amiga_segment_list_free(sl);
    }
  }
  size_t n_armed = arm_all_source_breakpoints(self);
  if (self->cfg_entry_break_mode == KAJO_AMIGA_RUN_TO_BP && n_armed > 0 &&
      self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_debug_sync_ui(self);
    amiga_window_show_toast(self, "▶ Entry hit — continuing to breakpoint");
    return;
  }
  amiga_window_debug_refresh_snapshot(self);
}

void
amiga_window_debug_on_rsp_stop(AmigaRspClient       *client G_GNUC_UNUSED,
                               const AmigaStopEvent *ev,
                               gpointer              ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  amiga_window_debug_sync_ui(self);

  if (self->temp_run_to_cursor_addr != 0) {
    uint32_t ta = self->temp_run_to_cursor_addr;
    self->temp_run_to_cursor_addr = 0;
    if (!g_hash_table_contains(self->bp_addrs, GUINT_TO_POINTER(ta)) &&
        self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
      amiga_rsp_client_bp_remove_async(self->rsp, ta, NULL, NULL, NULL);
    }
  }

  if (ev->kind == AMIGA_STOP_EXIT) {
    amiga_cpu_strip_reset(self->cpu_strip);
    amiga_window_show_toast(self, "Amiberry target exited");
    return;
  }

  if (ev->signal_num != 0 && ev->signal_num != 5 && self->lbl_exc_last) {
    g_autofree char *ex = g_strdup_printf(
        "Last stop: signal #%u at PC=$%08X", ev->signal_num, ev->pc);
    gtk_label_set_text(GTK_LABEL(self->lbl_exc_last), ex);
  }

  if (ev->kind == AMIGA_STOP_WINUAE_ENTRY) {
    g_autofree char *msg = ev->entry_proc
                               ? g_strdup_printf("Entry break: %s ($%08X)", ev->entry_proc, ev->pc)
                               : g_strdup_printf("Entry break ($%08X)", ev->pc);
    amiga_window_show_toast(self, msg);
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
    amiga_rsp_client_rcmd_async(self->rsp, "segments", NULL, on_segments_done, weak);
    return;
  }

  g_autofree char *msg = ev->watch_kind
                             ? g_strdup_printf("Watchpoint (%s @ $%06X)", ev->watch_kind, ev->watch_addr)
                             : g_strdup_printf("Stopped (sig=%u)", ev->signal_num);
  amiga_window_show_toast(self, msg);
  amiga_window_debug_refresh_snapshot(self);
}

/* ─── Connect With Retry & Runner Callbacks ─── */

static void try_rsp_connect_attempt(KajoAmigaWindow *self);

static void
on_rsp_connect_attempt_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  bool ok = amiga_rsp_client_connect_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self) return;

  if (!ok) {
    if (self->connect_retries_left > 0 &&
        (self->attach_mode || amiga_runner_is_running(self->runner))) {
      self->connect_retries_left--;
      return; /* retry timer will invoke next attempt */
    }
    g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
    amiga_window_debug_sync_ui(self);
    amiga_window_show_toast(self, err ? err->message : "RSP connect failed");
    return;
  }

  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  self->connect_retries_left = 0;
  amiga_window_debug_sync_ui(self);

  if (self->cfg_trace_blitter_writes) {
    amiga_rsp_client_rcmd_async(
        self->rsp, "watch range dff040 36 2 ff ff ffffffff 0 0 0 1", NULL, NULL, NULL);
  }
  if (self->cfg_enable_trace_history) {
    amiga_rsp_client_rcmd_async(self->rsp, "history on", NULL, NULL, NULL);
  }
  if (self->exc_mask != 0) {
    g_autofree char *ec = g_strdup_printf("exception %u", (unsigned)self->exc_mask);
    amiga_rsp_client_rcmd_async(self->rsp, ec, NULL, NULL, NULL);
  }

  if (self->pending_entry_exe && self->pending_entry_exe[0]) {
    g_autofree char *pb = g_strdup_printf("process-break name %s", self->pending_entry_exe);
    amiga_rsp_client_rcmd_async(self->rsp, pb, NULL, NULL, NULL);
    if (self->cfg_capture_guest_output) {
      amiga_rsp_client_rcmd_async(self->rsp, "guest-output on", NULL, NULL, NULL);
    }
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_debug_sync_ui(self);
    amiga_window_show_toast(self, "▶ Booting AmigaDOS — waiting for process entry");
    g_clear_pointer(&self->pending_entry_exe, g_free);
    return;
  }

  const AmigaTarget *t = amiga_window_get_selected_target(self);
  if (!self->attach_mode && t && t->entry_addr != 0) {
    arm_all_source_breakpoints(self);
    amiga_rsp_client_bp_set_async(self->rsp, t->entry_addr & ~1u, NULL, NULL, NULL);
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_debug_sync_ui(self);
    amiga_window_show_toast(self, "▶ Booting ADF — running to entry_addr");
    return;
  }

  amiga_window_show_toast(self, "✓ Attached to Amiberry RSP");
  amiga_window_debug_refresh_snapshot(self);
}

static void
try_rsp_connect_attempt(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_DISCONNECTED) return;
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  amiga_rsp_client_connect_async(
      self->rsp,
      (self->attach_host && self->attach_host[0]) ? self->attach_host : "127.0.0.1",
      self->attach_port ? self->attach_port : 2345,
      NULL, on_rsp_connect_attempt_done, weak);
}

static gboolean
on_connect_retry_tick(gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  if (!self) return G_SOURCE_REMOVE;
  if (!self->rsp ||
      amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_DISCONNECTED ||
      self->connect_retries_left <= 0 ||
      (!self->attach_mode && !amiga_runner_is_running(self->runner))) {
    if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_CONNECTING) {
      return G_SOURCE_CONTINUE;
    }
    self->connect_retry_timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  try_rsp_connect_attempt(self);
  return G_SOURCE_CONTINUE;
}

static void
retry_weak_free(gpointer p)
{
  GWeakRef *w = p;
  g_weak_ref_clear(w);
  g_free(w);
}

void
amiga_window_debug_connect_with_retry(KajoAmigaWindow *self,
                                      const char      *host,
                                      uint16_t         port,
                                      bool             attach_mode,
                                      const char      *entry_exe)
{
  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  self->attach_mode = attach_mode;
  g_free(self->attach_host);
  self->attach_host = g_strdup((host && host[0]) ? host : "127.0.0.1");
  self->attach_port = port ? port : 2345;
  g_free(self->pending_entry_exe);
  self->pending_entry_exe = entry_exe ? g_strdup(entry_exe) : NULL;

  gtk_stack_set_visible_child_name(GTK_STACK(self->sidebar_stack),
                                   attach_mode ? "attach" : "project");
  gtk_stack_set_visible_child_name(GTK_STACK(self->root_stack), "workspace");
  amiga_window_sync_header_workspace(self, true);
  if (attach_mode) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_disasm), TRUE);
  }
  amiga_window_sync_mode_pill(self);

  g_autofree char *lbl = g_strdup_printf("%s %s:%u",
                                         attach_mode ? "Attach" : "Debug",
                                         self->attach_host, self->attach_port);
  gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), lbl);

  self->connect_retries_left = attach_mode ? 0 : 25; /* up to 5s while Amiberry initializes */
  try_rsp_connect_attempt(self);
  if (!attach_mode) {
    GWeakRef *weak = g_new0(GWeakRef, 1);
    g_weak_ref_init(weak, self);
    self->connect_retry_timer_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 200, on_connect_retry_tick, weak, retry_weak_free);
  }
}

static void
on_runner_log(AmigaRunner *r G_GNUC_UNUSED, const char *line, bool is_err G_GNUC_UNUSED, gpointer ud)
{
  amiga_window_append_console(KAJO_AMIGA_WINDOW(ud), line);
}

static void
on_runner_exit(AmigaRunner *r G_GNUC_UNUSED, int status, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  amiga_cpu_strip_reset(self->cpu_strip);
  amiga_window_debug_sync_ui(self);
  g_autofree char *msg = g_strdup_printf("Amiberry exited (status %d)", status);
  amiga_window_show_toast(self, msg);
}

void
amiga_window_debug_launch_after_build(KajoAmigaWindow *self)
{
  const AmigaTarget *t = amiga_window_get_selected_target(self);
  const AmigaHwProfile *p = amiga_window_get_selected_profile(self);
  const char *amiberry = (self->cfg_amiberry_bin && g_file_test(self->cfg_amiberry_bin, G_FILE_TEST_IS_EXECUTABLE))
                             ? self->cfg_amiberry_bin
                             : (self->toolchain ? self->toolchain->amiberry : NULL);
  if (!amiberry || !amiberry[0]) amiberry = g_find_program_in_path(
      (self->cfg_amiberry_bin && self->cfg_amiberry_bin[0]) ? self->cfg_amiberry_bin : "amiberry");

  if (!self->project || !t || !amiberry) {
    amiga_window_show_toast(self, "Amiberry binary or project target missing");
    return;
  }

  const char *pname = self->project->name ? self->project->name : "default";
  g_autofree char *dh0 = g_build_filename(g_get_user_cache_dir(), "kajo", "amiga", pname, "dh0", NULL);
  g_autofree char *adf_abs = (t->boot_adf && t->boot_adf[0])
                                 ? resolve_project_path(self, t->boot_adf)
                                 : g_strdup_printf("%s/out/%s.adf", self->project->root_dir, t->name);
  g_autofree char *kick = resolve_kickstart_owned(self, p);
  uint16_t port = self->cfg_gdb_port ? self->cfg_gdb_port : 2345;
  const char *host = (self->cfg_gdb_host && self->cfg_gdb_host[0]) ? self->cfg_gdb_host : "127.0.0.1";

  AmigaTargetKind launch_kind = (t->boot_adf && t->boot_adf[0]) ? AMIGA_TARGET_ADF_RAW : t->kind;
  AmigaRunnerLaunchSpec spec = {
    .amiberry_bin = amiberry,
    .profile = p,
    .kickstart_override = kick,
    .target_kind = launch_kind,
    .target_name = t->name,
    .dh0_dir = dh0,
    .adf_path = adf_abs,
    .gdb_port = port,
    .uaeboard_guest_output = self->project->trap86_guest_output || self->cfg_capture_guest_output,
  };

  amiga_runner_set_callbacks(self->runner, on_runner_log, on_runner_exit, self);
  g_autoptr(GError) err = NULL;
  if (!amiga_runner_start(self->runner, &spec, self->project->root_dir, &err)) {
    amiga_window_show_toast(self, err ? err->message : "Launch failed");
    return;
  }

  g_autofree char *entry_exe = (launch_kind != AMIGA_TARGET_ADF_RAW)
                                   ? g_strdup_printf("%s.exe", t->name)
                                   : NULL;
  amiga_window_debug_connect_with_retry(self, host, port, false, entry_exe);
}

void
amiga_window_debug_start_or_continue(KajoAmigaWindow *self)
{
  if (!self->rsp) return;
  AmigaRspState st = amiga_rsp_client_get_state(self->rsp);
  if (st == AMIGA_RSP_STOPPED) {
    if (self->active_buf) ide_buffer_set_dap_pc_line(self->active_buf, 0);
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_debug_sync_ui(self);
    amiga_window_show_toast(self, "▶ Running guest");
    return;
  }
  if (st == AMIGA_RSP_RUNNING || st == AMIGA_RSP_CONNECTING) {
    amiga_window_show_toast(self, "Guest is already running (click ⏸ PAUSE or F6)");
    return;
  }
  if (!self->project) {
    amiga_window_show_toast(self, "Open a project or use 🔌 ATTACH");
    return;
  }
  self->launch_after_build = true;
  amiga_window_trigger_build(self);
}

void
amiga_window_debug_pause(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_RUNNING) return;
  amiga_rsp_client_interrupt_async(self->rsp, NULL, NULL, NULL);
  amiga_window_show_toast(self, "⏸ Interrupt requested (0x03)");
}

void
amiga_window_debug_stop(KajoAmigaWindow *self)
{
  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  self->launch_after_build = false;
  g_clear_pointer(&self->pending_entry_exe, g_free);
  self->temp_run_to_cursor_addr = 0;

  if (self->rsp && amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_DISCONNECTED) {
    amiga_rsp_client_detach_async(self->rsp, NULL, NULL, NULL);
  }
  if (amiga_runner_owns_process(self->runner)) {
    amiga_runner_stop(self->runner);
  }
  if (self->active_buf) {
    ide_buffer_set_dap_pc_line(self->active_buf, 0);
    gtk_widget_queue_draw(self->source_view);
  }
  self->has_last_regs = false;
  self->has_last_exec_st = false;
  amiga_cpu_strip_reset(self->cpu_strip);
  amiga_window_debug_sync_ui(self);
  amiga_window_show_toast(self, self->attach_mode ? "🔌 Detached safely" : "⏹ Stopped");
}

void
amiga_window_debug_step_over(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  if (self->active_buf && self->has_last_regs && self->symbols) {
    const char *path = ide_buffer_get_file_path(self->active_buf);
    if (path && (g_str_has_suffix(path, ".c") || g_str_has_suffix(path, ".C"))) {
      const char *sf = NULL;
      uint32_t sl = 0, start = 0, end = 0;
      if (amiga_symbols_addr_to_line(self->symbols, self->last_regs.pc, &sf, &sl) &&
          amiga_symbols_line_to_addr(self->symbols, sf, sl, &start, &end) &&
          end > start && self->last_regs.pc >= start && self->last_regs.pc < end) {
        amiga_rsp_client_step_range_async(self->rsp, start, end, NULL, NULL, NULL);
        amiga_window_debug_sync_ui(self);
        return;
      }
    }
  }
  amiga_rsp_client_step_over_async(self->rsp, NULL, NULL, NULL);
  amiga_window_debug_sync_ui(self);
}

void
amiga_window_debug_step_into(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  amiga_rsp_client_step_into_async(self->rsp, NULL, NULL, NULL);
  amiga_window_debug_sync_ui(self);
}

/* ─── Wired Disasm Toolbar Actions ─── */

static void
on_disasm_fetch_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, int count, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  g_autofree char *cmd = g_strdup_printf("disasm %x %d", addr, count);
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
  amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, amiga_window_debug_on_disasm_rcmd, weak);
}

static void
on_disasm_toggle_bp_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (addr & 1u) return;
  gpointer k = GUINT_TO_POINTER(addr);
  if (g_hash_table_contains(self->bp_addrs, k)) {
    g_hash_table_remove(self->bp_addrs, k);
    if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
      amiga_rsp_client_bp_remove_async(self->rsp, addr, NULL, NULL, NULL);
    }
  } else {
    g_hash_table_insert(self->bp_addrs, k, GINT_TO_POINTER(1));
    if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
      amiga_rsp_client_bp_set_async(self->rsp, addr, NULL, NULL, NULL);
    }
  }
  if (self->last_disasm_rows && self->last_disasm_rows->len > 0) {
    uint32_t pc = self->has_last_regs ? self->last_regs.pc
                                      : amiga_disasm_view_get_base_addr(self->disasm_view);
    amiga_disasm_view_set_rows(self->disasm_view, self->last_disasm_rows, pc, self->bp_addrs);
  }
}

static void
on_disasm_run_to_cursor_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED || (addr & 1u)) return;
  self->temp_run_to_cursor_addr = addr;
  if (!g_hash_table_contains(self->bp_addrs, GUINT_TO_POINTER(addr))) {
    amiga_rsp_client_bp_set_async(self->rsp, addr, NULL, NULL, NULL);
  }
  amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
  amiga_window_debug_sync_ui(self);
  g_autofree char *msg = g_strdup_printf("⏩ Running to $%08X", addr);
  amiga_window_show_toast(self, msg);
}

static void
on_disasm_set_pc_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED ||
      !self->has_last_regs || (addr & 1u)) {
    return;
  }
  AmigaCpuRegs next = self->last_regs;
  next.pc = addr;
  self->last_regs = next;
  amiga_rsp_client_write_regs_async(self->rsp, &next, NULL, NULL, NULL);
  amiga_cpu_strip_update(self->cpu_strip, &next,
                         self->has_last_exec_st ? &self->last_exec_st : NULL,
                         self->symbols, true);
  amiga_disasm_view_go_to(self->disasm_view, addr, false);
  g_autofree char *msg = g_strdup_printf("🎯 PC set to $%08X", addr);
  amiga_window_show_toast(self, msg);
}

static void
on_disasm_patch_word_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, uint16_t word_val, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED || (addr & 1u)) return;
  uint8_t be[2] = { (uint8_t)(word_val >> 8), (uint8_t)(word_val & 0xFFu) };
  amiga_rsp_client_write_mem_async(self->rsp, addr, be, 2, NULL, NULL, NULL);
  on_disasm_fetch_cb(self->disasm_view, amiga_disasm_view_get_base_addr(self->disasm_view), 64, self);
  g_autofree char *msg = g_strdup_printf("✏ Patched $%04X at $%08X", word_val, addr);
  amiga_window_show_toast(self, msg);
}

static void
on_history_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !txt) return;
  g_autoptr(GPtrArray) entries = NULL;
  if (amiga_rsp_parse_history(txt, &entries) && entries) {
    g_autofree char *hdr = g_strdup_printf("─── CPU History (%u entries) ───", entries->len);
    amiga_window_append_console(self, hdr);
    for (guint i = 0; i < entries->len && i < 16; i++) {
      const AmigaHistoryEntry *he = g_ptr_array_index(entries, i);
      g_autofree char *line = g_strdup_printf("  F%u V%u H%u  PC=$%08X  D0=$%08X A0=$%08X",
                                              he->frame, he->vpos, he->hpos,
                                              he->pc, he->regs[0], he->regs[8]);
      amiga_window_append_console(self, line);
    }
  }
}

static void
on_disasm_toggle_history_cb(AmigaDisasmView *v G_GNUC_UNUSED, bool enabled, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->cfg_enable_trace_history = enabled;
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) {
    amiga_window_show_toast(self, "Pause Amiberry to toggle trace history");
    return;
  }
  amiga_rsp_client_rcmd_async(self->rsp, enabled ? "history on" : "history off", NULL, NULL, NULL);
  if (enabled) {
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
    amiga_rsp_client_rcmd_async(self->rsp, "history", NULL, on_history_done, weak);
    amiga_window_show_toast(self, "📜 Trace history enabled");
  } else {
    amiga_window_show_toast(self, "Trace history disabled");
  }
}

static void
on_disasm_sel_cycles_cb(AmigaDisasmView *v G_GNUC_UNUSED, const char *txt, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (txt && self->show_sel_cycles) {
    gtk_label_set_text(GTK_LABEL(self->lbl_sel_cycles), txt);
    gtk_widget_set_visible(self->lbl_sel_cycles, TRUE);
  } else {
    gtk_widget_set_visible(self->lbl_sel_cycles, FALSE);
  }
}

static uint32_t
on_disasm_resolve_cb(AmigaDisasmView *v G_GNUC_UNUSED, const char *expr, bool *ok, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  uint32_t val = 0;
  if (self->symbols && amiga_symbols_lookup_name(self->symbols, expr, &val)) {
    if (ok) *ok = true;
    return val;
  }
  if (ok) *ok = false;
  return 0;
}

void
amiga_window_debug_fill_disasm_callbacks(AmigaDisasmCallbacks *cbs)
{
  if (!cbs) return;
  cbs->fetch_disasm = on_disasm_fetch_cb;
  cbs->toggle_bp = on_disasm_toggle_bp_cb;
  cbs->run_to_cursor = on_disasm_run_to_cursor_cb;
  cbs->set_pc = on_disasm_set_pc_cb;
  cbs->patch_word = on_disasm_patch_word_cb;
  cbs->toggle_history = on_disasm_toggle_history_cb;
  cbs->selection_cycles = on_disasm_sel_cycles_cb;
  cbs->resolve_expr = on_disasm_resolve_cb;
}

/* ─── Lightweight Async LSP Clients (clangd & m68k-lsp-server) ─── */

static void
apply_publish_diagnostics(KajoAmigaWindow *self, JsonNode *params)
{
  if (!self->active_buf || !params || !JSON_NODE_HOLDS_OBJECT(params)) return;
  JsonObject *o = json_node_get_object(params);
  if (!json_object_has_member(o, "uri") || !json_object_has_member(o, "diagnostics")) return;
  const char *uri = json_object_get_string_member(o, "uri");
  const char *apath = ide_buffer_get_file_path(self->active_buf);
  if (!uri || !apath) return;
  g_autofree char *active_uri = g_filename_to_uri(apath, NULL, NULL);
  if (g_strcmp0(uri, active_uri) != 0) return;

  ide_buffer_clear_diagnostics(self->active_buf);
  JsonArray *arr = json_object_get_array_member(o, "diagnostics");
  guint len = arr ? json_array_get_length(arr) : 0;
  for (guint i = 0; i < len; i++) {
    JsonObject *d = json_array_get_object_element(arr, i);
    if (!d || !json_object_has_member(d, "range")) continue;
    JsonObject *rng = json_object_get_object_member(d, "range");
    JsonObject *st = rng ? json_object_get_object_member(rng, "start") : NULL;
    guint line1 = st ? (guint)json_object_get_int_member(st, "line") + 1u : 1u;
    guint col1 = st ? (guint)json_object_get_int_member(st, "character") + 1u : 1u;
    gint64 sev_num = json_object_has_member(d, "severity")
                         ? json_object_get_int_member(d, "severity") : 1;
    IdeDiagSeverity sev = (sev_num == 1) ? IDE_DIAG_ERROR
                        : (sev_num == 2) ? IDE_DIAG_WARNING
                        : (sev_num == 3) ? IDE_DIAG_INFO : IDE_DIAG_HINT;
    const char *msg = json_object_has_member(d, "message")
                          ? json_object_get_string_member(d, "message") : "";
    const char *src = json_object_has_member(d, "source")
                          ? json_object_get_string_member(d, "source") : "lsp";
    ide_buffer_add_diagnostic(self->active_buf, line1, col1, sev, msg, src);
  }
  gtk_widget_queue_draw(self->source_view);
}

static void
on_lsp_notification(IdeLspClient *client G_GNUC_UNUSED,
                    const char   *method,
                    JsonNode     *params,
                    gpointer      ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (g_strcmp0(method, "textDocument/publishDiagnostics") == 0) {
    apply_publish_diagnostics(self, params);
  }
}

void
amiga_window_lsp_shutdown(KajoAmigaWindow *self)
{
  if (!self) return;
  if (self->lsp_clangd) {
    ide_lsp_client_stop(self->lsp_clangd);
    ide_lsp_client_free(self->lsp_clangd);
    self->lsp_clangd = NULL;
  }
  if (self->lsp_m68k) {
    ide_lsp_client_stop(self->lsp_m68k);
    ide_lsp_client_free(self->lsp_m68k);
    self->lsp_m68k = NULL;
  }
  g_clear_pointer(&self->lsp_root_clangd, g_free);
  g_clear_pointer(&self->lsp_root_m68k, g_free);
  g_clear_pointer(&self->lsp_open_uri_c, g_free);
  g_clear_pointer(&self->lsp_open_uri_m68k, g_free);
}

void
amiga_window_lsp_sync_project(KajoAmigaWindow *self)
{
  if (!self) return;
  const char *root = (self->project && self->project->root_dir) ? self->project->root_dir : NULL;
  if (!root) {
    amiga_window_lsp_shutdown(self);
    return;
  }

  if (!self->cfg_enable_clangd || !self->toolchain || !self->toolchain->clangd) {
    if (self->lsp_clangd) {
      ide_lsp_client_stop(self->lsp_clangd);
      ide_lsp_client_free(self->lsp_clangd);
      self->lsp_clangd = NULL;
      g_clear_pointer(&self->lsp_root_clangd, g_free);
      g_clear_pointer(&self->lsp_open_uri_c, g_free);
    }
  } else if (!self->lsp_clangd || g_strcmp0(self->lsp_root_clangd, root) != 0) {
    if (self->lsp_clangd) {
      ide_lsp_client_stop(self->lsp_clangd);
      ide_lsp_client_free(self->lsp_clangd);
      self->lsp_clangd = NULL;
    }
    g_autofree char *cc_flag = g_strdup_printf("--compile-commands-dir=%s/.kajo-amiga", root);
    const char *argv[] = { self->toolchain->clangd, cc_flag, "--background-index=false", NULL };
    self->lsp_clangd = ide_lsp_client_new("clangd", argv, root, NULL);
    ide_lsp_client_set_callbacks(self->lsp_clangd, on_lsp_notification, NULL, NULL, self);
    if (ide_lsp_client_start(self->lsp_clangd, NULL)) {
      g_free(self->lsp_root_clangd);
      self->lsp_root_clangd = g_strdup(root);
      g_clear_pointer(&self->lsp_open_uri_c, g_free);
    } else {
      ide_lsp_client_free(self->lsp_clangd);
      self->lsp_clangd = NULL;
    }
  }

  if (!self->cfg_enable_m68k_lsp || !self->toolchain || !self->toolchain->m68k_lsp_server) {
    if (self->lsp_m68k) {
      ide_lsp_client_stop(self->lsp_m68k);
      ide_lsp_client_free(self->lsp_m68k);
      self->lsp_m68k = NULL;
      g_clear_pointer(&self->lsp_root_m68k, g_free);
      g_clear_pointer(&self->lsp_open_uri_m68k, g_free);
    }
  } else if (!self->lsp_m68k || g_strcmp0(self->lsp_root_m68k, root) != 0) {
    if (self->lsp_m68k) {
      ide_lsp_client_stop(self->lsp_m68k);
      ide_lsp_client_free(self->lsp_m68k);
      self->lsp_m68k = NULL;
    }
    const char *argv[] = { self->toolchain->m68k_lsp_server, "--stdio", NULL };
    self->lsp_m68k = ide_lsp_client_new("m68k-lsp-server", argv, root, NULL);
    ide_lsp_client_set_callbacks(self->lsp_m68k, on_lsp_notification, NULL, NULL, self);
    if (ide_lsp_client_start(self->lsp_m68k, NULL)) {
      g_free(self->lsp_root_m68k);
      self->lsp_root_m68k = g_strdup(root);
      g_clear_pointer(&self->lsp_open_uri_m68k, g_free);
    } else {
      ide_lsp_client_free(self->lsp_m68k);
      self->lsp_m68k = NULL;
    }
  }
}

static IdeLspClient *
pick_lsp_for_path(KajoAmigaWindow *self, const char *path, const char **out_lang, char ***out_slot)
{
  if (!path) return NULL;
  if (g_str_has_suffix(path, ".c") || g_str_has_suffix(path, ".C") ||
      g_str_has_suffix(path, ".h") || g_str_has_suffix(path, ".H")) {
    if (out_lang) *out_lang = "c";
    if (out_slot) *out_slot = &self->lsp_open_uri_c;
    return self->lsp_clangd;
  }
  if (g_str_has_suffix(path, ".asm") || g_str_has_suffix(path, ".s") ||
      g_str_has_suffix(path, ".S") || g_str_has_suffix(path, ".i")) {
    if (out_lang) *out_lang = "m68k";
    if (out_slot) *out_slot = &self->lsp_open_uri_m68k;
    return self->lsp_m68k;
  }
  return NULL;
}

void
amiga_window_lsp_notify_open(KajoAmigaWindow *self, IdeBuffer *buf)
{
  if (!self || !buf) return;
  const char *path = ide_buffer_get_file_path(buf);
  const char *lang = NULL;
  char **slot = NULL;
  IdeLspClient *cli = pick_lsp_for_path(self, path, &lang, &slot);
  if (!cli) return;

  g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
  if (!uri || g_strcmp0(*slot, uri) == 0) return;
  if (*slot && (*slot)[0]) {
    g_autoptr(JsonBuilder) cb = json_builder_new();
    json_builder_begin_object(cb);
    json_builder_set_member_name(cb, "textDocument");
    json_builder_begin_object(cb);
    json_builder_set_member_name(cb, "uri");
    json_builder_add_string_value(cb, *slot);
    json_builder_end_object(cb);
    json_builder_end_object(cb);
    ide_lsp_client_notify(cli, "textDocument/didClose", json_builder_get_root(cb));
  }
  g_free(*slot);
  *slot = g_strdup(uri);

  g_autofree char *text = ide_buffer_get_text(buf);
  self->lsp_doc_ver++;
  g_autoptr(JsonBuilder) ob = json_builder_new();
  json_builder_begin_object(ob);
  json_builder_set_member_name(ob, "textDocument");
  json_builder_begin_object(ob);
  json_builder_set_member_name(ob, "uri"); json_builder_add_string_value(ob, uri);
  json_builder_set_member_name(ob, "languageId"); json_builder_add_string_value(ob, lang);
  json_builder_set_member_name(ob, "version"); json_builder_add_int_value(ob, self->lsp_doc_ver);
  json_builder_set_member_name(ob, "text"); json_builder_add_string_value(ob, text ? text : "");
  json_builder_end_object(ob);
  json_builder_end_object(ob);
  ide_lsp_client_notify(cli, "textDocument/didOpen", json_builder_get_root(ob));
}

void
amiga_window_lsp_notify_save(KajoAmigaWindow *self, IdeBuffer *buf)
{
  if (!self || !buf) return;
  const char *path = ide_buffer_get_file_path(buf);
  IdeLspClient *cli = pick_lsp_for_path(self, path, NULL, NULL);
  if (!cli) return;

  g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
  if (!uri) return;
  g_autofree char *text = ide_buffer_get_text(buf);
  self->lsp_doc_ver++;

  g_autoptr(JsonBuilder) chb = json_builder_new();
  json_builder_begin_object(chb);
  json_builder_set_member_name(chb, "textDocument");
  json_builder_begin_object(chb);
  json_builder_set_member_name(chb, "uri"); json_builder_add_string_value(chb, uri);
  json_builder_set_member_name(chb, "version"); json_builder_add_int_value(chb, self->lsp_doc_ver);
  json_builder_end_object(chb);
  json_builder_set_member_name(chb, "contentChanges");
  json_builder_begin_array(chb);
  json_builder_begin_object(chb);
  json_builder_set_member_name(chb, "text"); json_builder_add_string_value(chb, text ? text : "");
  json_builder_end_object(chb);
  json_builder_end_array(chb);
  json_builder_end_object(chb);
  ide_lsp_client_notify(cli, "textDocument/didChange", json_builder_get_root(chb));

  g_autoptr(JsonBuilder) sb = json_builder_new();
  json_builder_begin_object(sb);
  json_builder_set_member_name(sb, "textDocument");
  json_builder_begin_object(sb);
  json_builder_set_member_name(sb, "uri"); json_builder_add_string_value(sb, uri);
  json_builder_end_object(sb);
  json_builder_end_object(sb);
  ide_lsp_client_notify(cli, "textDocument/didSave", json_builder_get_root(sb));
}
