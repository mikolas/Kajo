#include "amiga_window_private.h"

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

static void
watch_item_free(AmigaWatchItem *w)
{
  if (!w) return;
  g_free(w->expr);
  g_free(w);
}

static void
watchpoint_item_free(AmigaWatchpointItem *wp)
{
  if (!wp) return;
  g_free(wp->label);
  g_free(wp);
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

static char *
resolve_kickstart_owned(const KajoAmigaWindow *self, const AmigaHwProfile *prof)
{
  if (prof && prof->kickstart && prof->kickstart[0]) {
    if (g_file_test(prof->kickstart, G_FILE_TEST_IS_REGULAR)) return g_strdup(prof->kickstart);
    if (self->project && self->project->root_dir) {
      g_autofree char *rel = g_build_filename(self->project->root_dir, prof->kickstart, NULL);
      if (g_file_test(rel, G_FILE_TEST_IS_REGULAR)) return g_steal_pointer(&rel);
    }
  }
  bool want_aga = prof && ((prof->model && (g_strcmp0(prof->model, "A1200") == 0 || g_strcmp0(prof->model, "A4000") == 0)) ||
                           (prof->chipset && g_ascii_strcasecmp(prof->chipset, "AGA") == 0));
  if (want_aga && self->cfg_kickstart_a1200 && g_file_test(self->cfg_kickstart_a1200, G_FILE_TEST_IS_REGULAR)) {
    return g_strdup(self->cfg_kickstart_a1200);
  }
  if (self->cfg_kickstart_a500 && g_file_test(self->cfg_kickstart_a500, G_FILE_TEST_IS_REGULAR)) {
    return g_strdup(self->cfg_kickstart_a500);
  }
  return NULL;
}

void
amiga_window_debug_init(KajoAmigaWindow *self)
{
  self->source_bps = g_ptr_array_new_with_free_func((GDestroyNotify)source_bp_free);
  self->watches = g_ptr_array_new_with_free_func((GDestroyNotify)watch_item_free);
  self->watchpoints = g_ptr_array_new_with_free_func((GDestroyNotify)watchpoint_item_free);
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
  g_clear_pointer(&self->watches, g_ptr_array_unref);
  g_clear_pointer(&self->watchpoints, g_ptr_array_unref);
  g_clear_pointer(&self->last_disasm_rows, g_ptr_array_unref);
  g_clear_pointer(&self->pending_entry_exe, g_free);
  g_clear_pointer(&self->cfg_amiberry_bin, g_free);
  g_clear_pointer(&self->cfg_kickstart_a500, g_free);
  g_clear_pointer(&self->cfg_kickstart_a1200, g_free);
  g_clear_pointer(&self->cfg_gdb_host, g_free);
  g_clear_pointer(&self->cfg_c_include_dirs, g_free);
  g_clear_pointer(&self->cfg_asm_include_dirs, g_free);
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

  if (self->btn_stop) gtk_widget_set_sensitive(self->btn_stop, connected || amiga_runner_is_running(self->runner));
  if (self->btn_snapshot) gtk_widget_set_sensitive(self->btn_snapshot, stopped || running);
  if (self->btn_debug) gtk_widget_set_sensitive(self->btn_debug, has_ws || stopped);
}

void
amiga_window_debug_sync_sidebar(KajoAmigaWindow *self)
{
  if (!self || !self->debug_sidebar) return;
  amiga_debug_sidebar_set_watches(self->debug_sidebar, self->watches);
  amiga_debug_sidebar_set_breakpoints(
      self->debug_sidebar, self->source_bps, self->bp_addrs, self->watchpoints);
}

void
amiga_window_debug_reload_symbols(KajoAmigaWindow *self)
{
  g_clear_pointer(&self->symbols, amiga_symbols_free);
  self->symbols = amiga_symbols_new();
  if (!self->project) return;
  const AmigaTarget *t = amiga_window_get_selected_target(self);
  if (!t) return;

  g_autofree char *elf_rel = (t->elf_path && t->elf_path[0]) ? g_strdup(t->elf_path) : g_strdup_printf("out/%s.elf", t->name);
  g_autofree char *exe_rel = (t->exe_path && t->exe_path[0]) ? g_strdup(t->exe_path) : g_strdup_printf("out/%s.exe", t->name);
  g_autofree char *map_rel = g_strdup_printf("out/%s.map", t->name);
  g_autofree char *elf_abs = resolve_project_path(self, elf_rel);
  g_autofree char *exe_abs = resolve_project_path(self, exe_rel);
  g_autofree char *map_abs = resolve_project_path(self, map_rel);

  bool loaded_primary = false;
  if (elf_abs && g_file_test(elf_abs, G_FILE_TEST_IS_REGULAR)) {
    loaded_primary = amiga_symbols_load_elf(self->symbols, elf_abs, NULL);
  }
  if (!loaded_primary && exe_abs && g_file_test(exe_abs, G_FILE_TEST_IS_REGULAR)) {
    amiga_symbols_load_hunk(self->symbols, exe_abs, NULL);
  }
  if (map_abs && g_file_test(map_abs, G_FILE_TEST_IS_REGULAR)) {
    amiga_symbols_load_map(self->symbols, map_abs, NULL);
  }
}

/* ─── Breakpoints, Watchpoints & Exceptions ─── */

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
      bp->armed = !bp->disabled;
      if (!bp->disabled) {
        g_hash_table_insert(self->bp_addrs, GUINT_TO_POINTER(saddr), GINT_TO_POINTER(1));
        if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
          amiga_rsp_client_bp_set_async(self->rsp, saddr, NULL, NULL, NULL);
        }
        armed_cnt++;
      }
    }
  }
  amiga_window_debug_sync_sidebar(self);
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
      if (bp->line == ln && !bp->disabled && same_file_path(bp->file, path)) { keep = true; break; }
    }
    if (!keep) ide_buffer_toggle_breakpoint(buf, ln);
  }
  for (guint j = 0; j < self->source_bps->len; j++) {
    AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, j);
    if (!bp->disabled && same_file_path(bp->file, path) && !ide_buffer_has_breakpoint(buf, bp->line)) {
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
    if (ide_buffer_has_breakpoint(buf, line_1based)) ide_buffer_toggle_breakpoint(buf, line_1based);
    amiga_window_show_toast(self, "○ Breakpoint removed");
  } else {
    AmigaSourceBp *bp = g_new0(AmigaSourceBp, 1);
    bp->file = g_strdup(path);
    bp->line = line_1based;
    uint32_t saddr = 0, eaddr = 0;
    if (self->symbols && amiga_symbols_line_to_addr(self->symbols, path, line_1based, &saddr, &eaddr)) {
      saddr &= ~1u;
      bp->addr = saddr;
      bp->armed = true;
      g_hash_table_insert(self->bp_addrs, GUINT_TO_POINTER(saddr), GINT_TO_POINTER(1));
      if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
        amiga_rsp_client_bp_set_async(self->rsp, saddr, NULL, NULL, NULL);
      }
      g_autofree char *bn = g_path_get_basename(path);
      g_autofree char *msg = g_strdup_printf("● Breakpoint set at %s:%u ($%06X)", bn, line_1based, saddr);
      amiga_window_show_toast(self, msg);
    } else {
      amiga_window_show_toast(self, "● Breakpoint queued (binds on entry)");
    }
    g_ptr_array_add(self->source_bps, bp);
    if (!ide_buffer_has_breakpoint(buf, line_1based)) ide_buffer_toggle_breakpoint(buf, line_1based);
  }

  amiga_window_debug_sync_sidebar(self);
  if (self->last_disasm_rows && self->last_disasm_rows->len > 0) {
    uint32_t pc = self->has_last_regs ? self->last_regs.pc : amiga_disasm_view_get_base_addr(self->disasm_view);
    amiga_disasm_view_set_rows(self->disasm_view, self->last_disasm_rows, pc, self->bp_addrs);
  }
  gtk_widget_queue_draw(self->source_view);
}

void
amiga_window_debug_apply_exc_mask(KajoAmigaWindow *self, uint8_t mask)
{
  if (!self) return;
  self->exc_mask = mask;
  if (self->debug_sidebar) amiga_debug_sidebar_set_exc_mask(self->debug_sidebar, mask);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    g_autofree char *cmd = g_strdup_printf("exception mask %x", (unsigned)mask);
    amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, NULL, NULL);
  }
}

void
amiga_window_debug_add_watch(KajoAmigaWindow *self, const char *expr)
{
  if (!self || !expr || !expr[0]) return;
  AmigaWatchItem *w = g_new0(AmigaWatchItem, 1);
  w->expr = g_strdup(expr);
  g_strstrip(w->expr);
  w->mode = 1; /* .l default */
  g_ptr_array_add(self->watches, w);
  amiga_window_debug_refresh_snapshot(self);
  amiga_window_debug_sync_sidebar(self);
}

void
amiga_window_debug_add_watchpoint(KajoAmigaWindow *self, uint8_t kind, uint32_t addr, uint32_t len)
{
  if (!self) return;
  AmigaWatchpointItem *wp = g_new0(AmigaWatchpointItem, 1);
  wp->kind = kind;
  wp->addr = addr;
  wp->len = len ? len : 2;
  wp->enabled = true;
  g_ptr_array_add(self->watchpoints, wp);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    amiga_rsp_client_watch_set_async(self->rsp, kind, addr, wp->len, NULL, NULL, NULL);
  }
  amiga_window_debug_sync_sidebar(self);
  amiga_window_show_toast(self, "👁 Watchpoint armed");
}

void
amiga_window_debug_add_dma_watch(KajoAmigaWindow *self, uint32_t addr, uint32_t len)
{
  if (!self) return;
  AmigaWatchpointItem *wp = g_new0(AmigaWatchpointItem, 1);
  wp->kind = 5;
  wp->addr = addr;
  wp->len = len ? len : 2;
  wp->enabled = true;
  g_ptr_array_add(self->watchpoints, wp);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    g_autofree char *cmd = g_strdup_printf("watch range %x %x 2 ff ff ffffffff 0 0 0 0", addr, wp->len);
    amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, NULL, NULL);
  }
  amiga_window_debug_sync_sidebar(self);
  amiga_window_show_toast(self, "👁 DMA range watch armed");
}

/* ─── Snapshot Completion & Async Refresh Callbacks ─── */

static void
maybe_finish_auto_snapshot(KajoAmigaWindow *self)
{
  if (!self->snapshot_auto_resume) return;
  if (self->snap_pending_ops > 0) self->snap_pending_ops--;
  if (self->snap_pending_ops == 0) {
    self->snapshot_auto_resume = false;
    g_autofree char *badge = g_strdup_printf("FROZEN SNAP @ F#%u V$%02X",
                                             self->frozen_snap_frame, self->frozen_snap_vpos);
    amiga_window_inspector_set_badge(self, badge, true);
    self->user_inspector_open = true;
    amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
    if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
      amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    }
    amiga_window_show_toast(self, "↻ Snapshot captured — guest running");
  }
}

static void
on_custom_mem_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) bytes = amiga_rsp_client_read_mem_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self) return;

  if (bytes && g_bytes_get_size(bytes) >= 512) {
    AmigaCustomSnapshot snap = {0};
    amiga_rsp_decode_custom_snapshot(g_bytes_get_data(bytes, NULL), &snap);
    amiga_mem_view_set_regs_and_custom(self->mem_view, NULL, &snap);
    amiga_window_inspector_apply_snapshot(self, &snap, true);
  }
  maybe_finish_auto_snapshot(self);
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
  if (!self) return;

  if (txt) {
    AmigaExecStatus st = {0};
    if (amiga_rsp_parse_execution_status(txt, &st)) {
      self->last_exec_st = st;
      self->has_last_exec_st = true;
      self->frozen_snap_frame = st.frame;
      self->frozen_snap_vpos = st.vpos;
      if (self->has_last_regs) {
        amiga_debug_sidebar_update_cpu(
            self->debug_sidebar, &self->last_regs, &self->last_exec_st,
            self->symbols, amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED);
      }
    }
  }
  maybe_finish_auto_snapshot(self);
}

static void
on_stack_mem_done(GObject *src, GAsyncResult *res, gpointer ud)
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
  amiga_debug_sidebar_set_stack(self->debug_sidebar, self->last_regs.a[7], raw, len, self->symbols);
}

typedef struct { GWeakRef weak; guint watch_idx; } WatchReadCtx;

static void
on_watch_mem_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  WatchReadCtx *c = ud;
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) bytes = amiga_rsp_client_read_mem_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(&c->weak);
  g_weak_ref_clear(&c->weak);
  guint idx = c->watch_idx;
  g_free(c);
  if (!self || !bytes || !self->watches || idx >= self->watches->len) return;

  AmigaWatchItem *w = g_ptr_array_index(self->watches, idx);
  gsize len = 0;
  const uint8_t *p = g_bytes_get_data(bytes, &len);
  uint32_t nv = 0;
  if (w->mode == 0 && len >= 2) nv = ((uint32_t)p[0] << 8) | p[1];
  else if (len >= 4) nv = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  else return;
  w->changed = (w->valid && nv != w->val);
  w->prev_val = w->val;
  w->val = nv;
  w->valid = true;
  amiga_debug_sidebar_set_watches(self->debug_sidebar, self->watches);
}

static void
refresh_watches(KajoAmigaWindow *self)
{
  if (!self->watches || !self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  for (guint i = 0; i < self->watches->len; i++) {
    AmigaWatchItem *w = g_ptr_array_index(self->watches, i);
    uint32_t addr = 0;
    if (!self->symbols || !amiga_symbols_lookup_name(self->symbols, w->expr, &addr)) {
      const char *p = w->expr;
      if (p[0] == '$') p++;
      char *end = NULL;
      unsigned long v = strtoul(p, &end, 16);
      if (end && end > p) addr = (uint32_t)v;
    }
    if (addr == 0) continue;
    w->addr = addr;
    WatchReadCtx *c = g_new0(WatchReadCtx, 1);
    g_weak_ref_init(&c->weak, self);
    c->watch_idx = i;
    amiga_rsp_client_read_mem_async(self->rsp, addr, (w->mode == 0) ? 2 : 4, NULL, on_watch_mem_done, c);
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
  if (!self) return;

  if (ok) {
    self->last_regs = regs;
    self->has_last_regs = true;

    amiga_debug_sidebar_update_cpu(
        self->debug_sidebar, &regs, self->has_last_exec_st ? &self->last_exec_st : NULL,
        self->symbols, !self->snapshot_auto_resume);
    amiga_mem_view_set_regs_and_custom(self->mem_view, &regs, NULL);
    if (amiga_disasm_view_get_follow_pc(self->disasm_view)) {
      amiga_disasm_view_go_to(self->disasm_view, regs.pc, false);
    }

    GWeakRef *ws = g_new0(GWeakRef, 1);
    g_weak_ref_init(ws, self);
    amiga_rsp_client_read_mem_async(self->rsp, regs.a[7], 32, NULL, on_stack_mem_done, ws);

    if (!self->snapshot_auto_resume && self->symbols) {
      const char *src_file = NULL;
      uint32_t src_line = 0;
      if (amiga_symbols_addr_to_line(self->symbols, regs.pc, &src_file, &src_line) && src_file) {
        g_autofree char *abs_src = resolve_project_path(self, src_file);
        if (self->active_buf &&
            same_file_path(ide_buffer_get_file_path(self->active_buf), abs_src ? abs_src : src_file)) {
          ide_buffer_set_dap_pc_line(self->active_buf, src_line);
          if (!self->disasm_tab_open) {
            GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(tb, &it, (int)(src_line - 1u));
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view), &it, 0.25, FALSE, 0, 0.5);
          }
          gtk_widget_queue_draw(self->source_view);
        } else if (abs_src && g_file_test(abs_src, G_FILE_TEST_IS_REGULAR) && !self->disasm_tab_open) {
          amiga_window_open_file_at_line(self, abs_src, src_line);
        }
      } else if (self->active_buf) {
        ide_buffer_set_dap_pc_line(self->active_buf, 0);
        gtk_widget_queue_draw(self->source_view);
      }
    }
  }
  maybe_finish_auto_snapshot(self);
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
  if (!self || !txt) return;
  g_autoptr(GPtrArray) tasks = NULL;
  if (amiga_rsp_parse_tasks(txt, &tasks) && tasks) {
    amiga_debug_sidebar_set_tasks(self->debug_sidebar, tasks);
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
  if (!self || !txt) return;
  g_autoptr(GPtrArray) regs = NULL;
  if (amiga_rsp_parse_memory_map(txt, &regs) && regs) {
    amiga_debug_sidebar_set_memmap(self->debug_sidebar, regs);
  }
}

static void
on_guest_output_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self) return;
  if (txt) {
    g_autoptr(GPtrArray) lines = NULL;
    if (amiga_rsp_parse_guest_output(txt, &lines) && lines) {
      for (guint i = 0; i < lines->len; i++) {
        g_autofree char *gl = g_strdup_printf("[GUEST] %s", (const char *)g_ptr_array_index(lines, i));
        amiga_window_append_console(self, gl);
      }
    }
  }
  maybe_finish_auto_snapshot(self);
}

static void
on_exc_last_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (self && txt && txt[0]) {
    amiga_debug_sidebar_set_exc_last(self->debug_sidebar, g_strstrip(txt));
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

  if (self->cfg_capture_guest_output) {
    GWeakRef *wg = g_new0(GWeakRef, 1); g_weak_ref_init(wg, self);
    amiga_rsp_client_rcmd_async(self->rsp, "guest-output read", NULL, on_guest_output_done, wg);
  } else {
    maybe_finish_auto_snapshot(self);
  }

  refresh_watches(self);
  if (self->attach_mode) {
    GWeakRef *w4 = g_new0(GWeakRef, 1); g_weak_ref_init(w4, self);
    amiga_rsp_client_rcmd_async(self->rsp, "tasks", NULL, on_tasks_done, w4);
    GWeakRef *w5 = g_new0(GWeakRef, 1); g_weak_ref_init(w5, self);
    amiga_rsp_client_rcmd_async(self->rsp, "memory-map", NULL, on_memmap_done, w5);
  }
}

void
amiga_window_debug_snapshot_now(KajoAmigaWindow *self)
{
  if (!self || !self->rsp) return;
  AmigaRspState st = amiga_rsp_client_get_state(self->rsp);
  if (st == AMIGA_RSP_STOPPED) {
    amiga_window_debug_refresh_snapshot(self);
  } else if (st == AMIGA_RSP_RUNNING) {
    self->inspector_frozen_snap = true;
    self->snapshot_auto_resume = true;
    self->snap_pending_ops = 4; /* regs, exec-status, custom ram, guest-output */
    amiga_rsp_client_interrupt_async(self->rsp, NULL, NULL, NULL);
    amiga_window_show_toast(self, "↻ Capturing one-shot hardware snapshot…");
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
      amiga_debug_sidebar_set_segments(self->debug_sidebar, sl);
      if (sl->segments && sl->segments->len > 0) {
        const AmigaSegmentInfo *s0 = g_ptr_array_index(sl->segments, 0);
        self->first_hunk_addr = s0->address;
        amiga_window_sync_status_bar(self);
      }
      amiga_segment_list_free(sl);
    }
  }
  size_t n_armed = arm_all_source_breakpoints(self);
  if ((self->launch_without_entry_break ||
       (self->cfg_entry_break_mode == KAJO_AMIGA_RUN_TO_BP && n_armed > 0)) &&
      self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    self->launch_without_entry_break = false;
    amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_show_toast(self, "▶ Guest running");
    return;
  }
  self->launch_without_entry_break = false;
  amiga_window_apply_session_state(self, AMIGA_SESSION_PAUSED);
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
  if (self->pending_step_out_addr != 0) {
    uint32_t sa = self->pending_step_out_addr;
    self->pending_step_out_addr = 0;
    if (!g_hash_table_contains(self->bp_addrs, GUINT_TO_POINTER(sa)) &&
        self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
      amiga_rsp_client_bp_remove_async(self->rsp, sa, NULL, NULL, NULL);
    }
  }

  if (ev->kind == AMIGA_STOP_EXIT) {
    self->snapshot_auto_resume = false;
    self->inspector_frozen_snap = false;
    self->first_hunk_addr = 0;
    amiga_debug_sidebar_reset(self->debug_sidebar);
    amiga_window_apply_session_state(self, self->project ? AMIGA_SESSION_EDIT : AMIGA_SESSION_WELCOME);
    amiga_window_show_toast(self, "Amiberry target exited");
    return;
  }

  if (self->snapshot_auto_resume) {
    amiga_window_debug_refresh_snapshot(self);
    return;
  }

  self->inspector_frozen_snap = false;
  if (ev->signal_num != 0 && ev->signal_num != 5) {
    g_autofree char *ex = g_strdup_printf("Last stop: signal #%u at PC=$%08X", ev->signal_num, ev->pc);
    amiga_debug_sidebar_set_exc_last(self->debug_sidebar, ex);
    GWeakRef *we = g_new0(GWeakRef, 1); g_weak_ref_init(we, self);
    amiga_rsp_client_rcmd_async(self->rsp, "exception last", NULL, on_exc_last_done, we);
  } else {
    amiga_debug_sidebar_set_exc_last(self->debug_sidebar, NULL);
  }

  if (ev->kind == AMIGA_STOP_WINUAE_ENTRY) {
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
    amiga_rsp_client_rcmd_async(self->rsp, "segments", NULL, on_segments_done, weak);
    return;
  }

  amiga_window_apply_session_state(self, self->attach_mode ? AMIGA_SESSION_ATTACH : AMIGA_SESSION_PAUSED);
  g_autofree char *msg = ev->watch_kind
                             ? g_strdup_printf("Watchpoint (%s @ $%06X)", ev->watch_kind, ev->watch_addr)
                             : g_strdup_printf("Paused (sig=%u)", ev->signal_num);
  amiga_window_show_toast(self, msg);
  amiga_window_debug_refresh_snapshot(self);
  if (self->cfg_enable_trace_history) amiga_window_debug_request_history(self);
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
      return;
    }
    g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
    amiga_window_apply_session_state(self, self->project ? AMIGA_SESSION_EDIT : AMIGA_SESSION_WELCOME);
    amiga_window_show_toast(self, err ? err->message : "RSP connect failed");
    return;
  }

  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  self->connect_retries_left = 0;

  if (self->cfg_trace_blitter_writes) {
    amiga_rsp_client_rcmd_async(
        self->rsp, "watch range dff040 36 2 ff ff ffffffff 0 0 0 1", NULL, NULL, NULL);
  }
  if (self->cfg_enable_trace_history) {
    amiga_rsp_client_rcmd_async(self->rsp, "history on", NULL, NULL, NULL);
  }
  if (self->exc_mask != 0) {
    g_autofree char *ec = g_strdup_printf("exception mask %x", (unsigned)self->exc_mask);
    amiga_rsp_client_rcmd_async(self->rsp, ec, NULL, NULL, NULL);
  }

  if (self->pending_entry_exe && self->pending_entry_exe[0]) {
    g_autofree char *pb = g_strdup_printf("process-break name %s", self->pending_entry_exe);
    amiga_rsp_client_rcmd_async(self->rsp, pb, NULL, NULL, NULL);
    if (self->cfg_capture_guest_output) {
      amiga_rsp_client_rcmd_async(self->rsp, "guest-output on", NULL, NULL, NULL);
    }
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
    amiga_window_show_toast(self, "▶ Booting AmigaDOS…");
    g_clear_pointer(&self->pending_entry_exe, g_free);
    return;
  }

  if (self->attach_mode) {
    amiga_window_apply_session_state(self, AMIGA_SESSION_ATTACH);
    amiga_window_show_toast(self, "✓ Attached to Amiberry RSP");
    amiga_window_debug_refresh_snapshot(self);
  } else {
    self->launch_without_entry_break = false;
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
    amiga_window_show_toast(self, "▶ Running guest");
  }
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
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_DISCONNECTED ||
      self->connect_retries_left <= 0 ||
      (!self->attach_mode && !amiga_runner_is_running(self->runner))) {
    if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_CONNECTING) return G_SOURCE_CONTINUE;
    self->connect_retry_timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  try_rsp_connect_attempt(self);
  return G_SOURCE_CONTINUE;
}

static void retry_weak_free(gpointer p) { GWeakRef *w = p; g_weak_ref_clear(w); g_free(w); }

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

  amiga_window_apply_session_state(self, attach_mode ? AMIGA_SESSION_ATTACH : AMIGA_SESSION_RUNNING);
  g_autofree char *lbl = g_strdup_printf("%s %s:%u",
                                         attach_mode ? "Attach" : "Debug",
                                         self->attach_host, self->attach_port);
  gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), lbl);

  self->connect_retries_left = attach_mode ? 0 : 25;
  try_rsp_connect_attempt(self);
  if (!attach_mode) {
    GWeakRef *weak = g_new0(GWeakRef, 1);
    g_weak_ref_init(weak, self);
    self->connect_retry_timer_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 200, on_connect_retry_tick, weak, retry_weak_free);
  }
}

static void on_runner_log(AmigaRunner *r G_GNUC_UNUSED, const char *line, bool e G_GNUC_UNUSED, gpointer ud) {
  amiga_window_append_console(KAJO_AMIGA_WINDOW(ud), line);
}
static void on_runner_exit(AmigaRunner *r G_GNUC_UNUSED, int status, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  self->snapshot_auto_resume = false;
  self->inspector_frozen_snap = false;
  self->first_hunk_addr = 0;
  amiga_debug_sidebar_reset(self->debug_sidebar);
  amiga_window_apply_session_state(self, self->project ? AMIGA_SESSION_EDIT : AMIGA_SESSION_WELCOME);
  g_autofree char *msg = g_strdup_printf("Amiberry exited (status %d)", status);
  amiga_window_show_toast(self, msg);
}

void
amiga_window_debug_launch_after_build(KajoAmigaWindow *self)
{
  const AmigaTarget *t = amiga_window_get_selected_target(self);
  const AmigaHwProfile *p = amiga_window_get_selected_profile(self);
  const char *amiberry = (self->cfg_amiberry_bin && g_file_test(self->cfg_amiberry_bin, G_FILE_TEST_IS_EXECUTABLE))
                             ? self->cfg_amiberry_bin : (self->toolchain ? self->toolchain->amiberry : NULL);
  if (!amiberry || !amiberry[0]) amiberry = g_find_program_in_path(
      (self->cfg_amiberry_bin && self->cfg_amiberry_bin[0]) ? self->cfg_amiberry_bin : "amiberry");

  if (!self->project || !t || !amiberry) {
    self->launch_without_entry_break = false;
    amiga_window_show_toast(self, "Amiberry binary or project target missing");
    return;
  }

  if (t->kind == AMIGA_TARGET_FLAT_BIN) {
    self->launch_without_entry_break = false;
    amiga_window_show_toast(self, "✓ Built flat binary (use Attach :2345 or an EXE target to boot Amiberry)");
    return;
  }

  const char *pname = self->project->name ? self->project->name : "default";
  g_autofree char *dh0 = g_build_filename(g_get_user_cache_dir(), "kajo", "amiga", pname, "dh0", NULL);
  g_autofree char *adf_abs = g_strdup_printf("%s/out/%s.adf", self->project->root_dir, t->name);
  g_autofree char *kick = resolve_kickstart_owned(self, p);
  uint16_t port = self->cfg_gdb_port ? self->cfg_gdb_port : 2345;
  const char *host = (self->cfg_gdb_host && self->cfg_gdb_host[0]) ? self->cfg_gdb_host : "127.0.0.1";

  AmigaRunnerLaunchSpec spec = {
    .amiberry_bin = amiberry,
    .profile = p,
    .kickstart_override = kick,
    .target_kind = t->kind,
    .target_name = t->name,
    .dh0_dir = dh0,
    .adf_path = adf_abs,
    .gdb_port = port,
    .uaeboard_guest_output = self->project->trap86_guest_output || self->cfg_capture_guest_output,
  };

  amiga_runner_set_callbacks(self->runner, on_runner_log, on_runner_exit, self);
  g_autoptr(GError) err = NULL;
  if (!amiga_runner_start(self->runner, &spec, self->project->root_dir, &err)) {
    self->launch_without_entry_break = false;
    amiga_window_show_toast(self, err ? err->message : "Launch failed");
    return;
  }

  g_autofree char *entry_exe = !self->launch_without_entry_break
                                   ? g_strdup_printf("%s.exe", t->name) : NULL;
  amiga_window_debug_connect_with_retry(self, host, port, false, entry_exe);
}

void
amiga_window_debug_run_free(KajoAmigaWindow *self)
{
  if (!self->project) {
    amiga_window_show_toast(self, "Open a project first");
    return;
  }
  self->launch_without_entry_break = true;
  self->launch_after_build = true;
  amiga_window_trigger_build(self);
}

void
amiga_window_debug_start_or_continue(KajoAmigaWindow *self)
{
  if (!self->rsp) return;
  AmigaRspState st = amiga_rsp_client_get_state(self->rsp);
  if (st == AMIGA_RSP_STOPPED) {
    if (self->active_buf) ide_buffer_set_dap_pc_line(self->active_buf, 0);
    self->inspector_frozen_snap = false;
    amiga_debug_sidebar_set_exc_last(self->debug_sidebar, NULL);
    amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
    amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
    amiga_window_show_toast(self, "▶ Running guest");
    return;
  }
  if (st == AMIGA_RSP_RUNNING || st == AMIGA_RSP_CONNECTING) {
    amiga_window_show_toast(self, "Guest is running (click ⏸ or F6)");
    return;
  }
  if (!self->project) {
    amiga_window_show_toast(self, "Open a project or use 🔌 ATTACH");
    return;
  }
  self->launch_without_entry_break = false;
  self->launch_after_build = true;
  amiga_window_trigger_build(self);
}

void
amiga_window_debug_pause(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_RUNNING) return;
  self->snapshot_auto_resume = false;
  amiga_rsp_client_interrupt_async(self->rsp, NULL, NULL, NULL);
  amiga_window_show_toast(self, "⏸ Interrupt requested (0x03)");
}

void
amiga_window_debug_stop(KajoAmigaWindow *self)
{
  bool was_attach = self->attach_mode;
  g_clear_handle_id(&self->connect_retry_timer_id, g_source_remove);
  self->launch_after_build = false;
  self->launch_without_entry_break = false;
  self->snapshot_auto_resume = false;
  self->inspector_frozen_snap = false;
  g_clear_pointer(&self->pending_entry_exe, g_free);
  self->temp_run_to_cursor_addr = 0;
  self->pending_step_out_addr = 0;
  self->first_hunk_addr = 0;

  if (self->rsp && amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_DISCONNECTED) {
    amiga_rsp_client_detach_async(self->rsp, NULL, NULL, NULL);
  }
  if (amiga_runner_owns_process(self->runner)) amiga_runner_stop(self->runner);
  if (self->active_buf) {
    ide_buffer_set_dap_pc_line(self->active_buf, 0);
    gtk_widget_queue_draw(self->source_view);
  }
  self->has_last_regs = false;
  self->has_last_exec_st = false;
  self->attach_mode = false;
  amiga_debug_sidebar_reset(self->debug_sidebar);
  amiga_window_apply_session_state(self, self->project ? AMIGA_SESSION_EDIT : AMIGA_SESSION_WELCOME);
  amiga_window_show_toast(self, was_attach ? "🔌 Detached safely" : "⏹ Stopped");
}

void
amiga_window_debug_restart(KajoAmigaWindow *self)
{
  if (self->attach_mode || !self->project) return;
  amiga_window_debug_stop(self);
  amiga_window_debug_start_or_continue(self);
}

void
amiga_window_debug_step_over(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  if (!self->step_insn_mode && self->active_buf && self->has_last_regs && self->symbols) {
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

static void
on_step_out_ret_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) bytes = amiga_rsp_client_read_mem_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !bytes || g_bytes_get_size(bytes) < 4) return;
  const uint8_t *p = g_bytes_get_data(bytes, NULL);
  uint32_t ret = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | (uint32_t)p[3]) & 0x00FFFFFEu;
  if (ret == 0 || !self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) {
    amiga_window_show_toast(self, "Cannot step out (invalid return address on A7)");
    return;
  }
  self->pending_step_out_addr = ret;
  if (!g_hash_table_contains(self->bp_addrs, GUINT_TO_POINTER(ret))) {
    amiga_rsp_client_bp_set_async(self->rsp, ret, NULL, NULL, NULL);
  }
  amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
  amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
  g_autofree char *msg = g_strdup_printf("⤴ Stepping out to $%08X", ret);
  amiga_window_show_toast(self, msg);
}

void
amiga_window_debug_step_out(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED || !self->has_last_regs) return;
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  amiga_rsp_client_read_mem_async(self->rsp, self->last_regs.a[7], 4, NULL, on_step_out_ret_done, weak);
}

void
amiga_window_debug_run_to_cursor_source(KajoAmigaWindow *self)
{
  if (!self->active_buf || !self->symbols ||
      !self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) {
    return;
  }
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark(tb, &cur, gtk_text_buffer_get_insert(tb));
  guint ln = (guint)gtk_text_iter_get_line(&cur) + 1u;
  uint32_t saddr = 0, eaddr = 0;
  if (!amiga_symbols_line_to_addr(self->symbols, ide_buffer_get_file_path(self->active_buf), ln, &saddr, &eaddr)) {
    amiga_window_show_toast(self, "Current line has no debug address");
    return;
  }
  saddr &= ~1u;
  self->temp_run_to_cursor_addr = saddr;
  if (!g_hash_table_contains(self->bp_addrs, GUINT_TO_POINTER(saddr))) {
    amiga_rsp_client_bp_set_async(self->rsp, saddr, NULL, NULL, NULL);
  }
  amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
  amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
  g_autofree char *msg = g_strdup_printf("⏩ Running to $%08X", saddr);
  amiga_window_show_toast(self, msg);
}

/* ─── Trace History, Disasm & Debug Sidebar Callbacks ─── */

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
    amiga_window_drawer_set_trace(self, entries);
  }
}

void
amiga_window_debug_request_history(KajoAmigaWindow *self)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  amiga_rsp_client_rcmd_async(self->rsp, "history read 32", NULL, on_history_done, weak);
}

static void on_disasm_fetch_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, int count, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  g_autofree char *cmd = g_strdup_printf("disasm %x %d", addr, count);
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
  amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, amiga_window_debug_on_disasm_rcmd, weak);
}

static void toggle_raw_addr_bp(KajoAmigaWindow *self, uint32_t addr) {
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
  amiga_window_debug_sync_sidebar(self);
  if (self->last_disasm_rows && self->last_disasm_rows->len > 0) {
    uint32_t pc = self->has_last_regs ? self->last_regs.pc : amiga_disasm_view_get_base_addr(self->disasm_view);
    amiga_disasm_view_set_rows(self->disasm_view, self->last_disasm_rows, pc, self->bp_addrs);
  }
}

static void on_disasm_toggle_bp_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, gpointer ud) {
  toggle_raw_addr_bp(KAJO_AMIGA_WINDOW(ud), addr);
}
static void on_disasm_run_to_cursor_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED || (addr & 1u)) return;
  self->temp_run_to_cursor_addr = addr;
  if (!g_hash_table_contains(self->bp_addrs, GUINT_TO_POINTER(addr))) {
    amiga_rsp_client_bp_set_async(self->rsp, addr, NULL, NULL, NULL);
  }
  amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
  amiga_rsp_client_continue_async(self->rsp, NULL, NULL, NULL);
}
static void on_disasm_set_pc_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED || !self->has_last_regs || (addr & 1u)) return;
  AmigaCpuRegs next = self->last_regs;
  next.pc = addr;
  self->last_regs = next;
  amiga_rsp_client_write_regs_async(self->rsp, &next, NULL, NULL, NULL);
  amiga_debug_sidebar_update_cpu(
      self->debug_sidebar, &next, self->has_last_exec_st ? &self->last_exec_st : NULL, self->symbols, true);
  amiga_disasm_view_go_to(self->disasm_view, addr, false);
}
static void on_disasm_patch_word_cb(AmigaDisasmView *v G_GNUC_UNUSED, uint32_t addr, uint16_t word_val, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED || (addr & 1u)) return;
  uint8_t be[2] = { (uint8_t)(word_val >> 8), (uint8_t)(word_val & 0xFFu) };
  amiga_rsp_client_write_mem_async(self->rsp, addr, be, 2, NULL, NULL, NULL);
  on_disasm_fetch_cb(self->disasm_view, amiga_disasm_view_get_base_addr(self->disasm_view), 64, self);
}
static void on_disasm_toggle_history_cb(AmigaDisasmView *v G_GNUC_UNUSED, bool enabled, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->cfg_enable_trace_history = enabled;
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  amiga_rsp_client_rcmd_async(self->rsp, enabled ? "history on" : "history off", NULL, NULL, NULL);
  if (enabled) {
    self->user_drawer_open = true;
    gtk_widget_set_visible(self->drawer_box, TRUE);
    amiga_window_drawer_select(self, "trace");
    amiga_window_debug_request_history(self);
  }
}
static void on_disasm_sel_cycles_cb(AmigaDisasmView *v G_GNUC_UNUSED, const char *txt, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (txt && self->show_sel_cycles) {
    gtk_button_set_label(GTK_BUTTON(self->btn_sel_cycles), txt);
    gtk_widget_set_visible(self->btn_sel_cycles, TRUE);
  } else {
    gtk_widget_set_visible(self->btn_sel_cycles, FALSE);
  }
}
static uint32_t on_disasm_resolve_cb(AmigaDisasmView *v G_GNUC_UNUSED, const char *expr, bool *ok, gpointer ud) {
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

static void on_sb_write_regs(AmigaDebugSidebar *s G_GNUC_UNUSED, const AmigaCpuRegs *regs, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    self->last_regs = *regs;
    self->has_last_regs = true;
    amiga_rsp_client_write_regs_async(self->rsp, regs, NULL, NULL, NULL);
    amiga_debug_sidebar_update_cpu(
        self->debug_sidebar, regs, self->has_last_exec_st ? &self->last_exec_st : NULL, self->symbols, true);
  }
}
static void on_sb_jump(AmigaDebugSidebar *s G_GNUC_UNUSED, uint32_t addr, bool to_mem, gpointer ud) {
  if (to_mem) amiga_window_jump_mem(KAJO_AMIGA_WINDOW(ud), addr);
  else amiga_window_jump_disasm(KAJO_AMIGA_WINDOW(ud), addr);
}
static void on_sb_cp(AmigaDebugSidebar *s G_GNUC_UNUSED, bool restore, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  g_autofree char *cmd = g_strdup_printf("checkpoint %s /tmp/kajo-amiga-cp.uss", restore ? "restore" : "save");
  amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, NULL, NULL);
  amiga_window_show_toast(self, restore ? "⏮ Restored checkpoint" : "💾 Saved checkpoint");
}
static void on_sb_exc(AmigaDebugSidebar *s G_GNUC_UNUSED, uint8_t mask, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->exc_mask = mask;
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    g_autofree char *cmd = g_strdup_printf("exception mask %x", (unsigned)mask);
    amiga_rsp_client_rcmd_async(self->rsp, cmd, NULL, NULL, NULL);
  }
}
static void on_sb_add_watch(AmigaDebugSidebar *s G_GNUC_UNUSED, const char *expr, gpointer ud) {
  amiga_window_debug_add_watch(KAJO_AMIGA_WINDOW(ud), expr);
}
static void on_sb_rem_watch(AmigaDebugSidebar *s G_GNUC_UNUSED, guint idx, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->watches && idx < self->watches->len) g_ptr_array_remove_index(self->watches, idx);
  amiga_window_debug_sync_sidebar(self);
}
static void on_sb_cyc_watch(AmigaDebugSidebar *s G_GNUC_UNUSED, guint idx, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->watches || idx >= self->watches->len) return;
  AmigaWatchItem *w = g_ptr_array_index(self->watches, idx);
  w->mode = (uint8_t)((w->mode + 1u) % 3u);
  refresh_watches(self);
  amiga_window_debug_sync_sidebar(self);
}
static void on_sb_tog_src_bp(AmigaDebugSidebar *s G_GNUC_UNUSED, guint idx, bool enabled, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->source_bps || idx >= self->source_bps->len) return;
  AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, idx);
  bp->disabled = !enabled;
  if (bp->addr != 0) {
    if (enabled) {
      g_hash_table_insert(self->bp_addrs, GUINT_TO_POINTER(bp->addr), GINT_TO_POINTER(1));
      if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
        amiga_rsp_client_bp_set_async(self->rsp, bp->addr, NULL, NULL, NULL);
      }
    } else {
      g_hash_table_remove(self->bp_addrs, GUINT_TO_POINTER(bp->addr));
      if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
        amiga_rsp_client_bp_remove_async(self->rsp, bp->addr, NULL, NULL, NULL);
      }
    }
  }
  if (self->active_buf) amiga_window_debug_sync_buffer_breakpoints(self, self->active_buf);
}
static void on_sb_rem_src_bp(AmigaDebugSidebar *s G_GNUC_UNUSED, guint idx, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->source_bps || idx >= self->source_bps->len) return;
  AmigaSourceBp *bp = g_ptr_array_index(self->source_bps, idx);
  if (bp->addr) {
    g_hash_table_remove(self->bp_addrs, GUINT_TO_POINTER(bp->addr));
    if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
      amiga_rsp_client_bp_remove_async(self->rsp, bp->addr, NULL, NULL, NULL);
    }
  }
  g_ptr_array_remove_index(self->source_bps, idx);
  if (self->active_buf) amiga_window_debug_sync_buffer_breakpoints(self, self->active_buf);
  amiga_window_debug_sync_sidebar(self);
}
static void on_sb_tog_addr_bp(AmigaDebugSidebar *s G_GNUC_UNUSED, uint32_t addr, gpointer ud) {
  toggle_raw_addr_bp(KAJO_AMIGA_WINDOW(ud), addr);
}
static void on_sb_tog_wp(AmigaDebugSidebar *s G_GNUC_UNUSED, guint idx, bool enabled, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->watchpoints || idx >= self->watchpoints->len) return;
  AmigaWatchpointItem *wp = g_ptr_array_index(self->watchpoints, idx);
  wp->enabled = enabled;
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED && wp->kind < 5) {
    if (enabled) amiga_rsp_client_watch_set_async(self->rsp, wp->kind, wp->addr, wp->len, NULL, NULL, NULL);
    else amiga_rsp_client_watch_remove_async(self->rsp, wp->kind, wp->addr, wp->len, NULL, NULL, NULL);
  }
}
static void on_sb_rem_wp(AmigaDebugSidebar *s G_GNUC_UNUSED, guint idx, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->watchpoints || idx >= self->watchpoints->len) return;
  AmigaWatchpointItem *wp = g_ptr_array_index(self->watchpoints, idx);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED && wp->kind < 5) {
    amiga_rsp_client_watch_remove_async(self->rsp, wp->kind, wp->addr, wp->len, NULL, NULL, NULL);
  }
  g_ptr_array_remove_index(self->watchpoints, idx);
  amiga_window_debug_sync_sidebar(self);
}

void
amiga_window_debug_fill_sidebar_callbacks(AmigaDebugSidebarCallbacks *cbs)
{
  if (!cbs) return;
  cbs->write_regs = on_sb_write_regs;
  cbs->jump_addr = on_sb_jump;
  cbs->checkpoint = on_sb_cp;
  cbs->exc_mask_changed = on_sb_exc;
  cbs->add_watch = on_sb_add_watch;
  cbs->remove_watch = on_sb_rem_watch;
  cbs->cycle_watch_mode = on_sb_cyc_watch;
  cbs->toggle_source_bp = on_sb_tog_src_bp;
  cbs->remove_source_bp = on_sb_rem_src_bp;
  cbs->toggle_addr_bp = on_sb_tog_addr_bp;
  cbs->toggle_watchpoint = on_sb_tog_wp;
  cbs->remove_watchpoint = on_sb_rem_wp;
}
