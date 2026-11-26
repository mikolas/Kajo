#include "amiga_window_private.h"

typedef struct {
  GWeakRef weak;
  uint32_t addr;
  enum {
    INSP_REQ_BPL = 0,
    INSP_REQ_COP,
    INSP_REQ_SCAN,
  } kind;
} InspMemCtx;

static void
insp_mem_ctx_free(InspMemCtx *c)
{
  g_weak_ref_clear(&c->weak);
  g_free(c);
}

static void
on_insp_mem_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  InspMemCtx *ctx = ud;
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) bytes = amiga_rsp_client_read_mem_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(&ctx->weak);
  uint32_t addr = ctx->addr;
  int kind = ctx->kind;
  insp_mem_ctx_free(ctx);

  if (!self || !bytes) return;
  gsize len = 0;
  const uint8_t *raw = g_bytes_get_data(bytes, &len);

  if (kind == INSP_REQ_BPL) {
    g_clear_pointer(&self->last_chip_ram, g_bytes_unref);
    self->last_chip_ram = g_bytes_ref(bytes);
    self->last_chip_base = addr;
    amiga_bitplane_view_set_chip_ram(self->bpl_view, addr, raw, len);
  } else if (kind == INSP_REQ_COP) {
    amiga_copper_view_set_bytes(self->cop_view, addr, raw, len, "EMU SNAPSHOT");
  } else if (kind == INSP_REQ_SCAN) {
    uint32_t hits[16];
    size_t nh = amiga_copper_scan_chipram(raw, len, hits, G_N_ELEMENTS(hits));
    for (size_t i = 0; i < nh; i++) hits[i] += addr;
    amiga_copper_view_set_scan_hits(self->cop_view, hits, nh);
    amiga_window_show_toast(self, nh > 0 ? "🔎 Copper scan found lists" : "No Copperlist signatures found");
  }
}

static void
start_insp_mem_read(KajoAmigaWindow *self, uint32_t addr, size_t len, int kind)
{
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  InspMemCtx *ctx = g_new0(InspMemCtx, 1);
  g_weak_ref_init(&ctx->weak, self);
  ctx->addr = addr;
  ctx->kind = kind;
  amiga_rsp_client_read_mem_async(self->rsp, addr, len, NULL, on_insp_mem_done, ctx);
}

static void
on_watch_events_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *txt = amiga_rsp_client_rcmd_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !txt) return;
  g_autoptr(GPtrArray) events = NULL;
  if (amiga_rsp_parse_watch_events(txt, &events)) {
    amiga_chips_view_set_watch_events(self->chips_view, events);
  }
}

/* ─── Sub-View Callbacks ─── */

static void
on_bpl_request_mem(AmigaBitplaneView *v G_GNUC_UNUSED, uint32_t addr, size_t len, gpointer ud)
{
  start_insp_mem_read(KAJO_AMIGA_WINDOW(ud), addr, len, INSP_REQ_BPL);
}

static uint32_t
on_bpl_resolve_sym(AmigaBitplaneView *v G_GNUC_UNUSED, const char *sym, bool *ok, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  uint32_t val = 0;
  if (self->symbols && amiga_symbols_lookup_name(self->symbols, sym, &val)) {
    if (ok) *ok = true;
    return val;
  }
  if (ok) *ok = false;
  return 0;
}

static void
on_cop_request_mem(AmigaCopperView *v G_GNUC_UNUSED, uint32_t addr, size_t len, gpointer ud)
{
  start_insp_mem_read(KAJO_AMIGA_WINDOW(ud), addr, len, INSP_REQ_COP);
}

static void
on_cop_scan(AmigaCopperView *v G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    start_insp_mem_read(self, 0x000000u, 65536u, INSP_REQ_SCAN);
    return;
  }
  if (self->last_chip_ram) {
    gsize len = 0;
    const uint8_t *raw = g_bytes_get_data(self->last_chip_ram, &len);
    uint32_t hits[16];
    size_t nh = amiga_copper_scan_chipram(raw, len, hits, G_N_ELEMENTS(hits));
    for (size_t i = 0; i < nh; i++) hits[i] += self->last_chip_base;
    amiga_copper_view_set_scan_hits(self->cop_view, hits, nh);
  }
}

static void
on_cop_watch_toggle(AmigaCopperView *v G_GNUC_UNUSED, bool enabled, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) {
    amiga_window_show_toast(self, "Pause Amiberry to arm $DFF080 watch");
    return;
  }
  if (enabled) {
    amiga_rsp_client_rcmd_async(
        self->rsp, "watch range dff080 8 2 ff ff ffffffff 0 0 0 1", NULL, NULL, NULL);
    amiga_window_show_toast(self, "👁 Watching $DFF080 COP1LC writes (log_only=1)");
  } else {
    amiga_rsp_client_rcmd_async(self->rsp, "watch clear-events", NULL, NULL, NULL);
  }
}

static void
on_insp_jump_mem(gpointer view G_GNUC_UNUSED, uint32_t addr, gpointer ud)
{
  amiga_window_jump_mem(KAJO_AMIGA_WINDOW(ud), addr);
}

static void
on_chips_trace_toggle(AmigaChipsView *v G_GNUC_UNUSED, bool enabled, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) {
    amiga_window_show_toast(self, "Pause Amiberry to arm non-breaking blit log");
    return;
  }
  if (enabled) {
    amiga_rsp_client_rcmd_async(
        self->rsp, "watch range dff040 36 2 ff ff ffffffff 0 0 0 1", NULL, NULL, NULL);
    amiga_window_show_toast(self, "👁 Blit log armed ($DFF040..076, log_only=1)");
  } else {
    amiga_rsp_client_rcmd_async(self->rsp, "watch clear-events", NULL, NULL, NULL);
  }
}

static void
on_chips_refresh_trace(AmigaChipsView *v G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->rsp || amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_STOPPED) return;
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  amiga_rsp_client_rcmd_async(self->rsp, "watch events", NULL, on_watch_events_done, weak);
}

static void
on_insp_tab_toggled(GtkToggleButton *tb, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->insp_stack || !gtk_toggle_button_get_active(tb)) return;
  static const char *const k_pages[5] = { "bpl", "cop", "chips", "dma", "hrm" };
  for (int i = 0; i < 5; i++) {
    if (GTK_WIDGET(tb) == self->insp_tab_btns[i]) {
      gtk_stack_set_visible_child_name(GTK_STACK(self->insp_stack), k_pages[i]);
      break;
    }
  }
}

GtkWidget *
amiga_window_inspector_build(KajoAmigaWindow *self)
{
  self->inspector_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->inspector_box, "amiga-inspector");
  gtk_widget_set_size_request(self->inspector_box, 356, -1);

  GtkWidget *ihdr = gtk_label_new("HARDWARE INSPECTOR ($DFF000)");
  gtk_widget_add_css_class(ihdr, "amiga-pane-header");
  gtk_label_set_xalign(GTK_LABEL(ihdr), 0.0f);
  gtk_box_append(GTK_BOX(self->inspector_box), ihdr);

  GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(tabs, "amiga-insp-tabs");
  static const char *const k_labels[5] = { "BPL", "COP", "CHIPS", "DMA", "HRM" };
  for (int i = 0; i < 5; i++) {
    self->insp_tab_btns[i] = gtk_toggle_button_new_with_label(k_labels[i]);
    gtk_widget_add_css_class(self->insp_tab_btns[i], "amiga-insp-tab");
    gtk_widget_set_hexpand(self->insp_tab_btns[i], TRUE);
    if (i > 0) {
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->insp_tab_btns[i]),
                                  GTK_TOGGLE_BUTTON(self->insp_tab_btns[0]));
    }
    g_signal_connect(self->insp_tab_btns[i], "toggled", G_CALLBACK(on_insp_tab_toggled), self);
    gtk_box_append(GTK_BOX(tabs), self->insp_tab_btns[i]);
  }
  gtk_box_append(GTK_BOX(self->inspector_box), tabs);

  self->insp_stack = gtk_stack_new();
  gtk_stack_set_vhomogeneous(GTK_STACK(self->insp_stack), FALSE);
  gtk_stack_set_hhomogeneous(GTK_STACK(self->insp_stack), FALSE);
  gtk_widget_set_vexpand(self->insp_stack, TRUE);
  gtk_widget_set_hexpand(self->insp_stack, TRUE);

  self->bpl_view = amiga_bitplane_view_new();
  AmigaBitplaneCallbacks bc = {
    .request_mem = on_bpl_request_mem,
    .resolve_symbol = on_bpl_resolve_sym,
  };
  amiga_bitplane_view_set_callbacks(self->bpl_view, &bc, self);
  gtk_stack_add_named(GTK_STACK(self->insp_stack), GTK_WIDGET(self->bpl_view), "bpl");

  self->cop_view = amiga_copper_view_new();
  AmigaCopperCallbacks cc = {
    .request_mem = on_cop_request_mem,
    .scan_chip_ram = on_cop_scan,
    .toggle_watch_cop1lc = on_cop_watch_toggle,
    .jump_mem = (void (*)(AmigaCopperView *, uint32_t, gpointer))on_insp_jump_mem,
  };
  amiga_copper_view_set_callbacks(self->cop_view, &cc, self);
  gtk_stack_add_named(GTK_STACK(self->insp_stack), GTK_WIDGET(self->cop_view), "cop");

  self->chips_view = amiga_chips_view_new();
  AmigaChipsCallbacks chc = {
    .toggle_blit_trace = on_chips_trace_toggle,
    .refresh_blit_trace = on_chips_refresh_trace,
    .jump_mem = (void (*)(AmigaChipsView *, uint32_t, gpointer))on_insp_jump_mem,
  };
  amiga_chips_view_set_callbacks(self->chips_view, &chc, self);
  gtk_stack_add_named(GTK_STACK(self->insp_stack), GTK_WIDGET(self->chips_view), "chips");

  self->dma_view = amiga_dma_view_new();
  gtk_stack_add_named(GTK_STACK(self->insp_stack), GTK_WIDGET(self->dma_view), "dma");

  self->hrm_view = amiga_hrm_view_new();
  gtk_stack_add_named(GTK_STACK(self->insp_stack), GTK_WIDGET(self->hrm_view), "hrm");

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->insp_tab_btns[0]), TRUE);
  gtk_box_append(GTK_BOX(self->inspector_box), self->insp_stack);
  return self->inspector_box;
}

void
amiga_window_inspector_select_tab(KajoAmigaWindow *self, int idx)
{
  if (!self || idx < 0 || idx >= 5 || !self->insp_tab_btns[idx]) return;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->insp_tab_btns[idx]), TRUE);
}

void
amiga_window_inspector_apply_snapshot(KajoAmigaWindow           *self,
                                      const AmigaCustomSnapshot *snap,
                                      bool                       fetch_live_ram)
{
  if (!self || !snap) return;
  self->last_custom = *snap;
  self->has_custom = true;

  amiga_chips_view_set_snapshot(self->chips_view, snap);
  amiga_dma_view_set_snapshot(self->dma_view, snap);
  amiga_copper_view_set_snapshot(self->cop_view, snap);

  gsize chip_len = 0;
  const uint8_t *chip = self->last_chip_ram ? g_bytes_get_data(self->last_chip_ram, &chip_len) : NULL;
  amiga_bitplane_view_set_snapshot(self->bpl_view, snap, chip, chip_len, self->last_chip_base);

  if (fetch_live_ram && self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_STOPPED) {
    if (snap->bplpt[0] > 0) start_insp_mem_read(self, snap->bplpt[0], 40960u, INSP_REQ_BPL);
    if (snap->cop1lc > 0) start_insp_mem_read(self, snap->cop1lc, 1024u, INSP_REQ_COP);
  }
}

void
amiga_window_inspector_lookup_hrm(KajoAmigaWindow *self, const char *query)
{
  if (!self || !self->hrm_view) return;
  if (self->project && self->project->hrm_dir) {
    amiga_hrm_view_set_hrm_dir(self->hrm_view, self->project->hrm_dir);
  }
  amiga_hrm_view_lookup(self->hrm_view, query);
  amiga_window_inspector_select_tab(self, 4);
}

gboolean
amiga_window_gutter_cycle_annotate(IdeBuffer *buf,
                                   guint      line_1based,
                                   char      *out_text,
                                   gsize      out_cap,
                                   double    *out_r,
                                   double    *out_g,
                                   double    *out_b,
                                   gpointer   ud G_GNUC_UNUSED)
{
  if (!buf || line_1based == 0) return FALSE;
  const char *path = ide_buffer_get_file_path(buf);
  if (path && (g_str_has_suffix(path, ".c") || g_str_has_suffix(path, ".h"))) {
    return FALSE;
  }
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter s, e;
  gtk_text_buffer_get_iter_at_line(tb, &s, (int)(line_1based - 1u));
  e = s;
  if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
  g_autofree char *line = gtk_text_buffer_get_text(tb, &s, &e, FALSE);
  AmigaCycleInfo ci = amiga_m68k_line_cycles(line);
  if (!ci.has_cycles) return FALSE;

  g_strlcpy(out_text, ci.display, out_cap);
  if (ci.is_branch) {
    *out_r = 0.0; *out_g = 0.85; *out_b = 0.94;
  } else if (ci.max_cycles >= 16) {
    *out_r = 1.0; *out_g = 0.55; *out_b = 0.0;
  } else {
    *out_r = 0.65; *out_g = 0.78; *out_b = 0.65;
  }
  return TRUE;
}

gboolean
amiga_window_source_query_tooltip(GtkWidget  *widget,
                                  gint        x,
                                  gint        y,
                                  gboolean    keyboard_mode G_GNUC_UNUSED,
                                  GtkTooltip *tooltip,
                                  gpointer    ud G_GNUC_UNUSED)
{
  GtkTextView *tv = GTK_TEXT_VIEW(widget);
  gint bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_TEXT, x, y, &bx, &by);
  GtkTextIter iter;
  if (!gtk_text_view_get_iter_at_location(tv, &iter, bx, by)) return FALSE;

  GtkTextIter ws = iter, we = iter;
  while (!gtk_text_iter_starts_line(&ws)) {
    GtkTextIter prev = ws;
    gtk_text_iter_backward_char(&prev);
    gunichar c = gtk_text_iter_get_char(&prev);
    if (!g_unichar_isalnum(c) && c != '_' && c != '$' && c != '#' && c != '-') break;
    ws = prev;
  }
  while (!gtk_text_iter_ends_line(&we)) {
    gunichar c = gtk_text_iter_get_char(&we);
    if (!g_unichar_isalnum(c) && c != '_') break;
    gtk_text_iter_forward_char(&we);
  }
  g_autofree char *tok = gtk_text_buffer_get_text(gtk_text_view_get_buffer(tv), &ws, &we, FALSE);
  if (!tok || !tok[0]) return FALSE;

  /* 1. $DFFxxx or literal/RGB12 check */
  const char *hex_p = strchr(tok, '$');
  if (hex_p) {
    unsigned long v = strtoul(hex_p + 1, NULL, 16);
    if (v >= 0xDFF000ul && v <= 0xDFF1FEul) {
      const AmigaHwRegInfo *ri = amiga_hw_reg_lookup((uint16_t)(v & 0x1FEu));
      if (ri) {
        const char *acc = (ri->access == AMIGA_REG_R) ? "RO"
                        : (ri->access == AMIGA_REG_W) ? "WO" : "RW";
        g_autofree char *tip = g_strdup_printf("$DFF%03X  %s [%s]\n%s",
                                               ri->offset, ri->name, acc, ri->summary);
        gtk_tooltip_set_text(tooltip, tip);
        return TRUE;
      }
    } else if (tok[0] == '#' && v <= 0xFFFFul) {
      uint8_t r = 0, g = 0, b = 0;
      amiga_decode_ocs_rgb12((uint16_t)(v & 0x0FFFu), &r, &g, &b);
      AmigaBltcon0Info blt = amiga_decode_bltcon0((uint16_t)v);
      g_autofree char *tip = g_strdup_printf(
          "#$%04lX = %lu dec\nOCS RGB12 #%02X%02X%02X · BLTCON0 ash=%u LF=$%02X (%s)",
          v, v, r, g, b, blt.ash, blt.lf_byte, blt.equation);
      gtk_tooltip_set_text(tooltip, tip);
      return TRUE;
    }
  }

  /* 2. _LVO* check */
  if ((g_str_has_prefix(tok, "_LVO") || g_str_has_prefix(tok, "-")) && tok[0] == '-') {
    long lvo = strtol(tok, NULL, 10);
    const char *nm = amiga_lvo_lookup(NULL, (int16_t)lvo);
    if (nm) {
      g_autofree char *tip = g_strdup_printf("LVO %ld → %s", lvo, nm);
      gtk_tooltip_set_text(tooltip, tip);
      return TRUE;
    }
  }
  return FALSE;
}
