#include "amiga_window_private.h"
#include <string.h>

static void
seed_sample_bitplanes(KajoAmigaWindow *self, const AmigaCustomSnapshot *snap)
{
  const uint32_t w_bytes = 40u, plane_sz = w_bytes * 256u;
  const size_t total = (size_t)plane_sz * 4u;
  g_autofree uint8_t *buf = g_malloc0(total);
  for (uint32_t y = 0; y < 256u; y++) {
    for (uint32_t bx = 0; bx < w_bytes; bx++) {
      uint32_t tx = bx >> 1, ty = y >> 4;
      uint8_t col = (uint8_t)(((tx + ty) & 1u) ? ((y / 16u) % 15u + 1u) : 0u);
      if (bx * 8u >= y && bx * 8u < y + 24u) col = 1u;
      for (uint32_t pl = 0; pl < 4u; pl++) {
        if ((col & (1u << pl)) != 0) buf[pl * plane_sz + y * w_bytes + bx] = 0xFFu;
      }
    }
  }
  g_clear_pointer(&self->last_chip_ram, g_bytes_unref);
  self->last_chip_ram = g_bytes_new(buf, total);
  self->last_chip_base = 0x00030000u;
  amiga_bitplane_view_set_snapshot(self->bpl_view, snap, buf, total, self->last_chip_base);
}

static void
seed_sample_copper(KajoAmigaWindow *self)
{
  static const uint8_t k_cop[] = {
    0x01, 0x00, 0x42, 0x00, 0x00, 0xE0, 0x00, 0x03,
    0x00, 0xE2, 0x00, 0x00, 0x01, 0x80, 0x00, 0x02,
    0x2C, 0x01, 0xFF, 0xFE, 0x01, 0x80, 0x00, 0x48,
    0x00, 0x58, 0x04, 0x10, 0xFF, 0xDF, 0xFF, 0xFE,
    0x10, 0x01, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFE,
  };
  amiga_copper_view_set_bytes(self->cop_view, 0x00020100u, k_cop, sizeof(k_cop), "EMU SNAPSHOT");
  static const uint32_t k_hits[2] = { 0x00020100u, 0x00020440u };
  amiga_copper_view_set_scan_hits(self->cop_view, k_hits, 2);
}

static void
seed_sample_debug_state(KajoAmigaWindow *self, AmigaCustomSnapshot *out_snap)
{
  AmigaCpuRegs r = {
    .d = { 0x00002C00u, 0x00000F00u, 0x00000004u, 0, 0xDEAD0001u, 0, 0, 0 },
    .a = { 0x00020100u, 0x00DFF000u, 0, 0, 0, 0, 0x00F80000u, 0x0007FF00u },
    .sr = 0x2004u, .pc = 0x00020014u,
  };
  AmigaExecStatus es = {
    .execution = "stopped", .debugger_stopped = true,
    .frame = 128, .vpos = 44, .hpos = 96, .cycles = 4412800u,
  };
  self->last_regs = r; self->has_last_regs = true;
  self->last_exec_st = es; self->has_last_exec_st = true;
  self->first_hunk_addr = 0x00020000u;
  amiga_debug_sidebar_update_cpu(self->debug_sidebar, &r, &es, NULL, true);

  static const uint8_t k_stk[16] = {
    0x00, 0x02, 0x01, 0x4C, 0x00, 0x02, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x04, 0x00, 0xDF, 0xF0, 0x00
  };
  amiga_debug_sidebar_set_stack(self->debug_sidebar, r.a[7], k_stk, sizeof(k_stk), NULL);

  g_ptr_array_set_size(self->watches, 0);
  AmigaWatchItem *w1 = g_new0(AmigaWatchItem, 1);
  w1->expr = g_strdup("copper_list"); w1->mode = 2; w1->addr = 0x020100u; w1->val = 0x01004200u; w1->valid = true;
  AmigaWatchItem *w2 = g_new0(AmigaWatchItem, 1);
  w2->expr = g_strdup("$DFF004"); w2->mode = 0; w2->addr = 0xDFF004u; w2->val = 0x2C00u; w2->valid = true; w2->changed = true;
  g_ptr_array_add(self->watches, w1); g_ptr_array_add(self->watches, w2);

  g_hash_table_insert(self->bp_addrs, GUINT_TO_POINTER(0x00020014u), GINT_TO_POINTER(1));
  AmigaSourceBp *sbp = g_new0(AmigaSourceBp, 1);
  sbp->file = g_strdup("src/main.s"); sbp->line = 6; sbp->addr = 0x00020014u; sbp->armed = true;
  g_ptr_array_set_size(self->source_bps, 0);
  g_ptr_array_add(self->source_bps, sbp);
  amiga_window_debug_sync_sidebar(self);

  AmigaSegmentList sl = {0};
  sl.segments = g_ptr_array_new();
  AmigaSegmentInfo seg0 = { .index = 0, .address = 0x00020000u, .size = 4096 };
  AmigaSegmentInfo seg1 = { .index = 1, .address = 0x00030000u, .size = 40960 };
  g_ptr_array_add(sl.segments, &seg0); g_ptr_array_add(sl.segments, &seg1);
  amiga_debug_sidebar_set_segments(self->debug_sidebar, &sl);
  g_ptr_array_unref(sl.segments);

  uint8_t custom[512];
  memset(custom, 0, sizeof(custom));
  custom[0x002] = 0x03; custom[0x003] = 0xF0;
  custom[0x004] = 0x20; custom[0x005] = 0x00;
  custom[0x040] = 0x09; custom[0x041] = 0xF0;
  custom[0x058] = 0x04; custom[0x059] = 0x10;
  custom[0x080] = 0x00; custom[0x081] = 0x02; custom[0x082] = 0x01; custom[0x083] = 0x00;
  custom[0x08E] = 0x2C; custom[0x08F] = 0x81; custom[0x090] = 0x2C; custom[0x091] = 0xC1;
  custom[0x092] = 0x00; custom[0x093] = 0x38; custom[0x094] = 0x00; custom[0x095] = 0xD0;
  custom[0x100] = 0x42; custom[0x101] = 0x00;
  custom[0x0E0] = 0x00; custom[0x0E1] = 0x03;
  custom[0x180] = 0x00; custom[0x181] = 0x02; custom[0x182] = 0x0F; custom[0x183] = 0xFF;
  for (int c = 2; c < 16; c++) {
    uint16_t rgb = (uint16_t)(((c * 2) & 0xF) << 8 | (((16 - c) & 0xF) << 4) | 0xA);
    custom[0x180 + c * 2] = (uint8_t)(rgb >> 8);
    custom[0x180 + c * 2 + 1] = (uint8_t)(rgb & 0xFF);
  }
  amiga_rsp_decode_custom_snapshot(custom, out_snap);
  seed_sample_bitplanes(self, out_snap);
  seed_sample_copper(self);
  amiga_window_inspector_apply_snapshot(self, out_snap, false);
  amiga_mem_view_set_data(self->mem_view, 0x00DFF000u, custom, 128);
}

void
kajo_amiga_window_set_preview_mode(KajoAmigaWindow *self, const char *mode)
{
  g_return_if_fail(KAJO_IS_AMIGA_WINDOW(self));
  if (!mode || g_strcmp0(mode, "welcome") == 0) {
    amiga_window_apply_session_state(self, AMIGA_SESSION_WELCOME);
    return;
  }

  if (!self->project) {
    self->project = amiga_project_new();
    self->project->name = g_strdup("CopperBars");
    self->project->root_dir = g_strdup("/tmp/copper-demo-preview");
    AmigaTarget *t = amiga_target_new("copper_bars");
    t->kind = AMIGA_TARGET_EXE_ADF;
    g_ptr_array_add(t->sources, g_strdup("src/main.s"));
    t->copper_symbol = g_strdup("copper_list");
    t->bitplane_symbol = g_strdup("bpl0");
    g_ptr_array_add(self->project->targets, t);
    amiga_window_populate_project_dropdowns(self);
    amiga_project_setup_view_bind(self->setup_view, self->project, self->toolchain);
  }

  const char *sample_s =
      "  section code,code_c\nstart:\n"
      "  move.w #$0020,$dff09a  ; disable INTENA\n"
      "  lea    copper_list(pc),a0\n"
      "  move.l a0,$dff080      ; COP1LC\n"
      "main_loop:\n"
      "  move.w $dff004,d0      ; wait VPOSR\n"
      "  bne.s  main_loop\n  rts\n";
  g_ptr_array_set_size(self->open_bufs, 0);
  IdeBuffer *b1 = ide_buffer_new("src/main.s");
  ide_buffer_set_text(b1, sample_s);
  ide_buffer_set_style_scheme(b1, "kajo-dark");
  g_ptr_array_add(self->open_bufs, b1);
  self->active_buf = b1;
  gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view),
                           GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(b1)));
  kajo_ide_gutter_renderer_set_buffer(self->gutter, b1);
  self->editor_host.buffer = b1;
  amiga_window_rebuild_center_tabs(self);
  amiga_window_refresh_source_gutter_and_outline(self);

  AmigaCustomSnapshot snap = {0};
  seed_sample_debug_state(self, &snap);

  g_autoptr(GPtrArray) drows = NULL;
  amiga_disasm_parse(
      "Segment 00 ($00020000)\n"
      "00020014 33fc 0f00 00df f180  move.w #$0f00,$00dff180\n"
      "0002001c 4eae ff7c            jsr -$0084(a6) == $00f81234\n"
      "00020020 66f2                 bne.b #$f2 == $00020014 (T)\n", NULL, &drows);
  amiga_disasm_view_set_rows(self->disasm_view, drows, 0x00020014u, self->bp_addrs);

  if (g_strcmp0(mode, "edit") == 0) {
    amiga_window_apply_session_state(self, AMIGA_SESSION_EDIT);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · src/main.s");
  } else if (g_strcmp0(mode, "build_err") == 0) {
    g_autoptr(GPtrArray) ds = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_build_diag_free);
    AmigaBuildDiag *d = g_new0(AmigaBuildDiag, 1);
    d->file = g_strdup("src/main.s"); d->line = 4; d->col = 10;
    d->severity = AMIGA_DIAG_ERROR; d->message = g_strdup("undefined symbol 'cop_list_v2'");
    g_ptr_array_add(ds, d);
    amiga_window_drawer_set_diags(self, ds);
    amiga_window_append_console(self, "vasmm68k_mot src/main.s - error 3009: undefined symbol 'cop_list_v2'");
    amiga_window_apply_session_state(self, AMIGA_SESSION_BUILD_ERR);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · Build failed (1 error)");
  } else if (g_strcmp0(mode, "setup") == 0) {
    amiga_window_apply_session_state(self, AMIGA_SESSION_SETUP);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · kajo-amiga.toml");
  } else if (g_strcmp0(mode, "running") == 0 || g_strcmp0(mode, "running_snap") == 0) {
    self->inspector_frozen_snap = (g_strcmp0(mode, "running_snap") == 0);
    self->user_inspector_open = self->inspector_frozen_snap;
    amiga_window_append_console(self, "[GUEST] CopperBars v1.0 started · Trap 86 OK");
    amiga_window_apply_session_state(self, AMIGA_SESSION_RUNNING);
    if (self->inspector_frozen_snap) amiga_window_inspector_set_badge(self, "FROZEN SNAP @ F#128 V$2C", true);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state),
                       self->inspector_frozen_snap ? "Running · Frozen Snap @ F#128 V$2C"
                                                   : "Running on Amiberry · CopperBars");
  } else if (g_strcmp0(mode, "attach") == 0) {
    self->attach_mode = true;
    g_autoptr(GPtrArray) tks = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_task_info_free);
    AmigaTaskInfo *ti = g_new0(AmigaTaskInfo, 1);
    ti->list_name = g_strdup("CUR"); ti->name = g_strdup("demo.exe"); ti->saved_sp = 0x0007FF00u;
    g_ptr_array_add(tks, ti);
    amiga_debug_sidebar_set_tasks(self->debug_sidebar, tks);
    g_autoptr(GPtrArray) mrs = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_mem_region_free);
    AmigaMemRegion *mr = g_new0(AmigaMemRegion, 1);
    mr->start = 0x00000000u; mr->length = 524288u; mr->name = g_strdup("Chip RAM");
    g_ptr_array_add(mrs, mr);
    amiga_debug_sidebar_set_memmap(self->debug_sidebar, mrs);
    amiga_window_apply_session_state(self, AMIGA_SESSION_ATTACH);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "Attach 127.0.0.1:2345");
  } else if (g_strcmp0(mode, "wizard") == 0) {
    amiga_window_apply_session_state(self, AMIGA_SESSION_EDIT);
    amiga_window_act_new_project(self);
  } else {
    amiga_window_apply_session_state(self, AMIGA_SESSION_PAUSED);
    if (g_strcmp0(mode, "copper") == 0) amiga_window_inspector_select_tab(self, 1);
    else if (g_strcmp0(mode, "chips") == 0) amiga_window_inspector_select_tab(self, 2);
    else if (g_strcmp0(mode, "dma") == 0) amiga_window_inspector_select_tab(self, 3);
    else if (g_strcmp0(mode, "hrm") == 0) amiga_window_inspector_lookup_hrm(self, "$DFF180");
    else amiga_window_inspector_select_tab(self, 0);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "Paused @ $00020014 · CopperBars");
  }
}
