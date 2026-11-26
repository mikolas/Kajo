#include "amiga_window_private.h"
#include <string.h>

static void
seed_sample_bitplanes(KajoAmigaWindow *self, const AmigaCustomSnapshot *snap)
{
  /* 320x256 4-bitplane deterministic Amiga checkerboard + copper raster demo (40,960 bytes) */
  const uint32_t w_bytes = 40u;
  const uint32_t plane_sz = w_bytes * 256u;
  const size_t total = (size_t)plane_sz * 4u;
  g_autofree uint8_t *buf = g_malloc0(total);

  for (uint32_t y = 0; y < 256u; y++) {
    for (uint32_t bx = 0; bx < w_bytes; bx++) {
      uint32_t tx = bx >> 1;
      uint32_t ty = y >> 4;
      uint8_t col = (uint8_t)(((tx + ty) & 1u) ? ((y / 16u) % 15u + 1u) : 0u);
      /* Bright diagonal highlight bar */
      if (bx * 8u >= y && bx * 8u < y + 24u) col = 1u;
      for (uint32_t pl = 0; pl < 4u; pl++) {
        if ((col & (1u << pl)) != 0) {
          buf[pl * plane_sz + y * w_bytes + bx] = 0xFFu;
        }
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
    0x01, 0x00, 0x42, 0x00, /* MOVE BPLCON0, #$4200 */
    0x00, 0xE0, 0x00, 0x03, /* MOVE BPL1PTH, #$0003 */
    0x00, 0xE2, 0x00, 0x00, /* MOVE BPL1PTL, #$0000 */
    0x01, 0x80, 0x00, 0x02, /* MOVE COLOR00, #$0002 */
    0x2C, 0x01, 0xFF, 0xFE, /* WAIT V=$2C, H=$00 */
    0x01, 0x80, 0x00, 0x48, /* MOVE COLOR00, #$0048 */
    0x00, 0x58, 0x04, 0x10, /* MOVE BLTSIZE, #$0410 (requires COPCON!) */
    0xFF, 0xDF, 0xFF, 0xFE, /* WAIT $FFDF, $FFFE (line 255) */
    0x10, 0x01, 0xFF, 0xFE, /* WAIT V=$10, H=$00 */
    0xFF, 0xFF, 0xFF, 0xFE, /* END */
  };
  amiga_copper_view_set_bytes(self->cop_view, 0x00020100u, k_cop, sizeof(k_cop), "EMU SNAPSHOT");
  static const uint32_t k_hits[2] = { 0x00020100u, 0x00020440u };
  amiga_copper_view_set_scan_hits(self->cop_view, k_hits, 2);
}

static void
seed_sample_watch_events(KajoAmigaWindow *self)
{
  g_autoptr(GPtrArray) evs = g_ptr_array_new_with_free_func(g_free);
  AmigaWatchEvent *e1 = g_new0(AmigaWatchEvent, 1);
  e1->frame = 128; e1->vpos = 44; e1->addr = 0xDFF058u; e1->new_val = 0x0410u; e1->pc = 0x020084u;
  AmigaWatchEvent *e2 = g_new0(AmigaWatchEvent, 1);
  e2->frame = 128; e2->vpos = 48; e2->addr = 0xDFF040u; e2->new_val = 0x09F0u; e2->pc = 0x02006Cu;
  g_ptr_array_add(evs, e1);
  g_ptr_array_add(evs, e2);
  amiga_chips_view_set_watch_events(self->chips_view, evs);
}

void
kajo_amiga_window_set_preview_mode(KajoAmigaWindow *self, const char *mode)
{
  g_return_if_fail(KAJO_IS_AMIGA_WINDOW(self));
  if (!mode || g_strcmp0(mode, "welcome") == 0) {
    amiga_window_sync_header_workspace(self, false);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "Welcome");
    gtk_stack_set_visible_child_name(GTK_STACK(self->root_stack), "welcome");
    return;
  }

  /* Deterministic read-only sample state for visual QA without touching user disk files */
  if (!self->project) {
    self->project = amiga_project_new();
    self->project->name = g_strdup("CopperBars");
    self->project->root_dir = g_strdup("/tmp/copper-demo-preview");
    AmigaTarget *t = amiga_target_new("copper_bars");
    t->kind = AMIGA_TARGET_ADF_DOS;
    g_ptr_array_add(t->sources, g_strdup("src/main.s"));
    t->copper_symbol = g_strdup("copper_list");
    t->bitplane_symbol = g_strdup("bpl0");
    g_ptr_array_add(self->project->targets, t);
    amiga_window_populate_project_dropdowns(self);
    amiga_project_setup_view_bind(self->setup_view, self->project, self->toolchain);
  }

  amiga_window_sync_header_workspace(self, true);
  gtk_stack_set_visible_child_name(GTK_STACK(self->root_stack), "workspace");

  const char *sample_s =
      "  section code,code_c\n"
      "start:\n"
      "  move.w #$0020,$dff09a  ; disable INTENA\n"
      "  lea    copper_list(pc),a0\n"
      "  move.l a0,$dff080      ; COP1LC\n"
      "main_loop:\n"
      "  move.w $dff004,d0      ; wait VPOSR\n"
      "  bne.s  main_loop\n"
      "  rts\n";
  if (self->active_buf) ide_buffer_free(self->active_buf);
  self->active_buf = ide_buffer_new("src/main.s");
  ide_buffer_set_text(self->active_buf, sample_s);
  ide_buffer_set_style_scheme(self->active_buf, "kajo-dark");
  gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view),
                           GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf)));
  kajo_ide_gutter_renderer_set_buffer(self->gutter, self->active_buf);
  self->editor_host.buffer = self->active_buf;
  amiga_window_refresh_source_gutter_and_outline(self);

  AmigaCpuRegs r = {
    .d = { 0x00002C00u, 0x00000F00u, 0x00000004u, 0, 0xDEAD0001u, 0, 0, 0 },
    .a = { 0x00020100u, 0x00DFF000u, 0, 0, 0, 0, 0x00F80000u, 0x0007FF00u },
    .sr = 0x2004u,
    .pc = 0x00020014u,
  };
  AmigaExecStatus es = {
    .execution = "stopped", .debugger_stopped = true,
    .frame = 128, .vpos = 44, .hpos = 96, .cycles = 4412800u,
  };
  amiga_cpu_strip_update(self->cpu_strip, &r, &es, NULL, true);

  uint8_t custom[512];
  memset(custom, 0, sizeof(custom));
  custom[0x002] = 0x03; custom[0x003] = 0xF0;
  custom[0x004] = 0x20; custom[0x005] = 0x00;
  custom[0x040] = 0x09; custom[0x041] = 0xF0; /* BLTCON0: D=A, A|D */
  custom[0x044] = 0xFF; custom[0x045] = 0xFF;
  custom[0x046] = 0xFF; custom[0x047] = 0xFF;
  custom[0x058] = 0x04; custom[0x059] = 0x10; /* BLTSIZE */
  custom[0x080] = 0x00; custom[0x081] = 0x02; custom[0x082] = 0x01; custom[0x083] = 0x00;
  custom[0x08E] = 0x2C; custom[0x08F] = 0x81;
  custom[0x090] = 0x2C; custom[0x091] = 0xC1;
  custom[0x092] = 0x00; custom[0x093] = 0x38;
  custom[0x094] = 0x00; custom[0x095] = 0xD0;
  custom[0x100] = 0x42; custom[0x101] = 0x00;
  custom[0x0E0] = 0x00; custom[0x0E1] = 0x03; /* BPL1PT $00030000 */
  custom[0x180] = 0x00; custom[0x181] = 0x02;
  custom[0x182] = 0x0F; custom[0x183] = 0xFF;
  custom[0x184] = 0x00; custom[0x185] = 0x8E;
  custom[0x186] = 0x0F; custom[0x187] = 0x80;
  custom[0x188] = 0x02; custom[0x189] = 0xD8;
  custom[0x18A] = 0x0B; custom[0x18B] = 0x4E;
  for (int c = 6; c < 16; c++) {
    uint16_t rgb = (uint16_t)(((c * 2) & 0xF) << 8 | (((16 - c) & 0xF) << 4) | 0xA);
    custom[0x180 + c * 2] = (uint8_t)(rgb >> 8);
    custom[0x180 + c * 2 + 1] = (uint8_t)(rgb & 0xFF);
  }

  AmigaCustomSnapshot snap = {0};
  amiga_rsp_decode_custom_snapshot(custom, &snap);
  seed_sample_bitplanes(self, &snap);
  seed_sample_copper(self);
  seed_sample_watch_events(self);
  amiga_window_inspector_apply_snapshot(self, &snap, false);
  amiga_mem_view_set_data(self->mem_view, 0x00DFF000u, custom, 128);

  if (g_strcmp0(mode, "setup") == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · kajo-amiga.toml");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_setup), TRUE);
  } else if (g_strcmp0(mode, "attach") == 0) {
    self->attach_mode = true;
    amiga_window_sync_mode_pill(self);
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "Attach 127.0.0.1:2345");
    gtk_stack_set_visible_child_name(GTK_STACK(self->sidebar_stack), "attach");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_disasm), TRUE);
    GtkWidget *old;
    while ((old = gtk_widget_get_first_child(self->attach_list)) != NULL) {
      gtk_list_box_remove(GTK_LIST_BOX(self->attach_list), old);
    }
    static const char *const preview_rows[3] = {
      "[CUR] demo.exe · SP $0007FF00",
      "RAM   Chip · $00000000..$00080000",
      "SEG0  .text · $00020000 (4096 B)"
    };
    for (int i = 0; i < 3; i++) {
      GtkWidget *l = gtk_label_new(preview_rows[i]);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
      gtk_list_box_append(GTK_LIST_BOX(self->attach_list), l);
    }
    g_autoptr(GPtrArray) drows = NULL;
    amiga_disasm_parse(
        "Segment 00 ($00020000)\n"
        "00020014 33fc 0f00 00df f180  move.w #$0f00,$00dff180\n"
        "0002001c 4eae ff7c            jsr -$0084(a6) == $00f81234\n"
        "00020020 66f2                 bne.b #$f2 == $00020014 (T)\n",
        NULL, &drows);
    amiga_disasm_view_set_rows(self->disasm_view, drows, 0x00020014u, self->bp_addrs);
  } else if (g_strcmp0(mode, "wizard") == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · New Project Wizard");
    amiga_window_act_new_project(self);
  } else if (g_strcmp0(mode, "bitplanes") == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · Bitplanes");
    amiga_window_inspector_select_tab(self, 0);
  } else if (g_strcmp0(mode, "copper") == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · Copperlist");
    amiga_window_inspector_select_tab(self, 1);
  } else if (g_strcmp0(mode, "chips") == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · Chips & Blitter");
    amiga_window_inspector_select_tab(self, 2);
  } else if (g_strcmp0(mode, "dma") == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · DMA Budget");
    amiga_window_inspector_select_tab(self, 3);
  } else if (g_strcmp0(mode, "hrm") == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · HRM Reference");
    amiga_window_inspector_lookup_hrm(self, "$DFF180");
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), "CopperBars · src/main.s");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tab_source), TRUE);
  }
}
