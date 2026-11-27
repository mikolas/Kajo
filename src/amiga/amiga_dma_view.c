#include "amiga_dma_view.h"
#include <cairo.h>
#include <math.h>
#include <string.h>

struct _AmigaDmaView {
  GtkBox              parent_instance;

  AmigaDmaInput       input;
  AmigaDmaBudget      budget;
  bool                updating;
  AmigaCustomSnapshot last_snap;
  bool                has_snap;

  GtkWidget          *prov_badge;
  GtkWidget          *dd_preset;
  GtkWidget          *bar_canvas;
  GtkWidget          *toggle_ntsc;
  GtkWidget          *toggle_hires;
  GtkWidget          *blt_ch_btns[4]; /* A, B, C, D */
  GtkWidget          *sp_bpl;
  GtkWidget          *sp_spr;
  GtkWidget          *sp_aud;
  GtkWidget          *sp_blt_w;
  GtkWidget          *sp_blt_h;
  GtkWidget          *sp_blits;
  GtkWidget          *sp_cop;
  GtkWidget          *meter_fill;
  GtkWidget          *kv_grid;
};

G_DEFINE_FINAL_TYPE(AmigaDmaView, amiga_dma_view, GTK_TYPE_BOX)

static void
kv_cell(GtkWidget *grid, int col, int row, const char *k, const char *v)
{
  GtkWidget *c = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(c, "amiga-kv-cell");
  gtk_widget_set_hexpand(c, TRUE);
  GtkWidget *lk = gtk_label_new(k);
  gtk_widget_add_css_class(lk, "amiga-muted-hint");
  gtk_label_set_xalign(GTK_LABEL(lk), 0.0f);
  gtk_widget_set_hexpand(lk, TRUE);
  GtkWidget *lv = gtk_label_new(v);
  gtk_widget_add_css_class(lv, "amiga-kv-val");
  gtk_label_set_xalign(GTK_LABEL(lv), 1.0f);
  gtk_box_append(GTK_BOX(c), lk);
  gtk_box_append(GTK_BOX(c), lv);
  gtk_grid_attach(GTK_GRID(grid), c, col, row, 1, 1);
}

static void
recalc_and_draw(AmigaDmaView *self)
{
  self->budget = amiga_dma_calc_budget(&self->input);
  double pct = self->budget.odd_utilization_pct;

  gtk_widget_remove_css_class(self->meter_fill, "amiga-dma-meter-ok");
  gtk_widget_remove_css_class(self->meter_fill, "amiga-dma-meter-warn");
  gtk_widget_remove_css_class(self->meter_fill, "amiga-dma-meter-over");
  gtk_widget_add_css_class(
      self->meter_fill,
      pct > 95.0 ? "amiga-dma-meter-over"
                 : (pct > 75.0 ? "amiga-dma-meter-warn" : "amiga-dma-meter-ok"));
  gtk_level_bar_set_value(GTK_LEVEL_BAR(self->meter_fill), CLAMP(pct / 100.0, 0.0, 1.0));

  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->kv_grid)) != NULL) {
    gtk_grid_remove(GTK_GRID(self->kv_grid), ch);
  }
  g_autofree char *s_tot = g_strdup_printf("%u / %u (%.1f%%)", self->budget.used_odd_slots_frame,
                                           self->budget.avail_odd_slots_frame, pct);
  g_autofree char *s_lin = g_strdup_printf("%u BPL + %u COP (%u free)", self->budget.bpl_slots_line,
                                           self->budget.cop_slots_line, self->budget.free_odd_slots_visible_line);
  g_autofree char *s_blt = g_strdup_printf("%u slots", self->budget.blitter_slots_frame);
  g_autofree char *s_cpu = g_strdup_printf("%u cyc", self->budget.cpu_even_cycles_frame);
  g_autofree char *s_mgn = g_strdup_printf("%+d %s", self->budget.odd_slot_margin,
                                           self->budget.odd_slot_margin < 0 ? "OVER!" : "OK");
  g_autofree char *s_cop = g_strdup_printf("%u slots", self->budget.copper_slots_frame);
  kv_cell(self->kv_grid, 0, 0, "FRAME ODD", s_tot);
  kv_cell(self->kv_grid, 1, 0, "LINE DMA", s_lin);
  kv_cell(self->kv_grid, 0, 1, "BLITTER FR", s_blt);
  kv_cell(self->kv_grid, 1, 1, "COPPER FR", s_cop);
  kv_cell(self->kv_grid, 0, 2, "68K HEADRM", s_cpu);
  kv_cell(self->kv_grid, 1, 2, "MARGIN", s_mgn);

  gtk_widget_queue_draw(self->bar_canvas);
}

static void
sync_from_widgets(AmigaDmaView *self)
{
  if (self->updating) return;
  self->input.ntsc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->toggle_ntsc));
  self->input.hires = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->toggle_hires));
  self->input.bitplanes = (uint8_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_bpl));
  self->input.sprites_active = (uint8_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_spr));
  self->input.audio_channels = (uint8_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_aud));
  uint8_t bch = 0;
  for (int i = 0; i < 4; i++) {
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->blt_ch_btns[i]))) bch++;
  }
  self->input.blitter_ch = MAX(1u, bch);
  self->input.blit_w_words = (uint16_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_blt_w));
  self->input.blit_h_lines = (uint16_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_blt_h));
  self->input.blits_per_frame = (uint16_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_blits));
  self->input.copper_moves_per_frame = (uint16_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_cop));
  recalc_and_draw(self);
}

static void
push_to_widgets(AmigaDmaView *self)
{
  self->updating = true;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toggle_ntsc), self->input.ntsc);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toggle_hires), self->input.hires);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_bpl), self->input.bitplanes);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_spr), self->input.sprites_active);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_aud), self->input.audio_channels);
  for (int i = 0; i < 4; i++) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->blt_ch_btns[i]), i < (int)self->input.blitter_ch);
  }
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_blt_w), self->input.blit_w_words);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_blt_h), self->input.blit_h_lines);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_blits), self->input.blits_per_frame);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_cop), self->input.copper_moves_per_frame);
  self->updating = false;
  recalc_and_draw(self);
}

static void on_spin_changed(GtkSpinButton *s G_GNUC_UNUSED, gpointer ud) { sync_from_widgets(AMIGA_DMA_VIEW(ud)); }
static void on_toggle_changed(GtkToggleButton *tb G_GNUC_UNUSED, gpointer ud) { sync_from_widgets(AMIGA_DMA_VIEW(ud)); }

static void
on_preset_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *ps G_GNUC_UNUSED, gpointer ud)
{
  AmigaDmaView *self = AMIGA_DMA_VIEW(ud);
  if (self->updating) return;
  guint id = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_preset));
  if (id == 3 && self->has_snap) {
    amiga_dma_view_set_snapshot(self, &self->last_snap);
    return;
  }
  self->input.ntsc = false;
  self->input.hires = (id == 2);
  self->input.bitplanes = (id == 1) ? 5u : 4u;
  self->input.sprites_active = 8u;
  self->input.audio_channels = 4u;
  self->input.blitter_ch = 3u;
  self->input.blit_w_words = 4u;
  self->input.blit_h_lines = 32u;
  self->input.blits_per_frame = (id == 0) ? 16u : 20u;
  self->input.copper_moves_per_frame = 48u;
  gtk_label_set_text(GTK_LABEL(self->prov_badge), "PARAM");
  push_to_widgets(self);
}

static void
on_bar_draw(GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int w, int h, gpointer ud)
{
  AmigaDmaView *self = AMIGA_DMA_VIEW(ud);
  cairo_set_source_rgb(cr, 0.06, 0.06, 0.07);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  double bar_x = 8.0, bar_y = 20.0;
  double bar_w = MAX(40.0, (double)w - 16.0), bar_h = 22.0, total_cc = 227.0;
  double ref_cc = 8.0, dsk_cc = 6.0;
  double aud_cc = (double)self->input.audio_channels * 2.0;
  double spr_cc = (double)self->input.sprites_active * 4.0;
  double bpl_cc = MIN(total_cc - (ref_cc + dsk_cc + aud_cc + spr_cc),
                      (double)self->budget.bpl_slots_line * (self->input.hires ? 1.0 : 1.5));
  if (bpl_cc < 0.0) bpl_cc = 0.0;
  double cop_cc = MIN((double)self->budget.cop_slots_line * 2.0,
                      MAX(0.0, total_cc - (ref_cc + dsk_cc + aud_cc + spr_cc + bpl_cc)));
  double blt_cc = MIN((double)(self->budget.blitter_slots_frame / 312u) * 2.0,
                      MAX(0.0, total_cc - (ref_cc + dsk_cc + aud_cc + spr_cc + bpl_cc + cop_cc)));

  struct { double cc; double r, g, b; const char *name; } segs[7] = {
    { ref_cc, 0.35, 0.38, 0.42, "REF" }, { dsk_cc, 0.48, 0.42, 0.65, "DSK" },
    { aud_cc, 0.25, 0.72, 0.50, "AUD" }, { spr_cc, 0.85, 0.35, 0.72, "SPR" },
    { bpl_cc, 0.00, 0.48, 0.80, "BPL" }, { cop_cc, 0.00, 0.85, 0.94, "COP" },
    { blt_cc, 1.00, 0.55, 0.00, "BLT" },
  };
  cairo_set_source_rgb(cr, 0.14, 0.15, 0.17);
  cairo_rectangle(cr, bar_x, bar_y, bar_w, bar_h);
  cairo_fill(cr);
  double cur_x = bar_x;
  for (int i = 0; i < 7; i++) {
    double sw = (segs[i].cc / total_cc) * bar_w;
    if (sw < 0.5) continue;
    cairo_set_source_rgb(cr, segs[i].r, segs[i].g, segs[i].b);
    cairo_rectangle(cr, cur_x, bar_y, sw, bar_h);
    cairo_fill(cr);
    cur_x += sw;
  }
  cairo_select_font_face(cr, "Fira Code", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 9.5);
  cairo_set_source_rgb(cr, 0.68, 0.72, 0.78);
  cairo_move_to(cr, bar_x, 13.0); cairo_show_text(cr, "CC 0");
  cairo_move_to(cr, bar_x + bar_w * 0.5 - 12.0, 13.0); cairo_show_text(cr, "113");
  cairo_move_to(cr, bar_x + bar_w - 36.0, 13.0); cairo_show_text(cr, "227 CC");
  double lx = bar_x;
  for (int i = 0; i < 7; i++) {
    cairo_set_source_rgb(cr, segs[i].r, segs[i].g, segs[i].b);
    cairo_rectangle(cr, lx, bar_y + bar_h + 10.0, 8.0, 8.0);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.75, 0.78, 0.82);
    cairo_move_to(cr, lx + 11.0, bar_y + bar_h + 18.0);
    cairo_show_text(cr, segs[i].name);
    lx += 44.0;
  }
}

static GtkWidget *
make_spin_pair(AmigaDmaView *self, const char *label, int min_v, int max_v, GtkWidget **out_sp)
{
  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
  GtkWidget *l = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  gtk_widget_set_hexpand(l, TRUE);
  GtkWidget *sp = gtk_spin_button_new_with_range(min_v, max_v, 1);
  gtk_widget_add_css_class(sp, "amiga-spin");
  gtk_editable_set_width_chars(GTK_EDITABLE(sp), 3);
  g_signal_connect(sp, "value-changed", G_CALLBACK(on_spin_changed), self);
  *out_sp = sp;
  gtk_box_append(GTK_BOX(hb), l);
  gtk_box_append(GTK_BOX(hb), sp);
  return hb;
}

static void
amiga_dma_view_class_init(AmigaDmaViewClass *klass G_GNUC_UNUSED)
{
}

static void
amiga_dma_view_init(AmigaDmaView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  self->input.bitplanes = 4; self->input.blitter_ch = 3;
  self->input.blit_w_words = 4; self->input.blit_h_lines = 32;
  self->input.blits_per_frame = 16; self->input.copper_moves_per_frame = 48;
  self->input.sprites_active = 8; self->input.audio_channels = 4;

  GtkWidget *scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scr, TRUE); gtk_widget_set_hexpand(scr, TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 6); gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);

  GtkWidget *r0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->prov_badge = gtk_label_new("PARAM");
  gtk_widget_add_css_class(self->prov_badge, "amiga-badge-snapshot");
  self->dd_preset = gtk_drop_down_new_from_strings((const char *[]){
      "PAL 320×256 · 4 BPL", "PAL 320×256 · 5 BPL", "PAL 640×256 · HIRES", "Sync Live Snapshot", NULL
  });
  gtk_widget_add_css_class(self->dd_preset, "amiga-header-dd");
  gtk_widget_set_hexpand(self->dd_preset, TRUE);
  g_signal_connect(self->dd_preset, "notify::selected", G_CALLBACK(on_preset_changed), self);
  gtk_box_append(GTK_BOX(r0), self->prov_badge);
  gtk_box_append(GTK_BOX(r0), self->dd_preset);
  gtk_box_append(GTK_BOX(box), r0);

  self->bar_canvas = gtk_drawing_area_new();
  gtk_widget_set_size_request(self->bar_canvas, -1, 68);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->bar_canvas), on_bar_draw, self, NULL);
  gtk_box_append(GTK_BOX(box), self->bar_canvas);

  GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->toggle_ntsc = gtk_toggle_button_new_with_label("NTSC");
  self->toggle_hires = gtk_toggle_button_new_with_label("HIRES");
  gtk_widget_add_css_class(self->toggle_ntsc, "amiga-insp-tab");
  gtk_widget_add_css_class(self->toggle_hires, "amiga-insp-tab");
  g_signal_connect(self->toggle_ntsc, "toggled", G_CALLBACK(on_toggle_changed), self);
  g_signal_connect(self->toggle_hires, "toggled", G_CALLBACK(on_toggle_changed), self);
  gtk_box_append(GTK_BOX(trow), self->toggle_ntsc);
  gtk_box_append(GTK_BOX(trow), self->toggle_hires);
  static const char *const ch_names[4] = { "A", "B", "C", "D" };
  for (int i = 0; i < 4; i++) {
    self->blt_ch_btns[i] = gtk_toggle_button_new_with_label(ch_names[i]);
    gtk_widget_add_css_class(self->blt_ch_btns[i], "amiga-insp-tab");
    g_signal_connect(self->blt_ch_btns[i], "toggled", G_CALLBACK(on_toggle_changed), self);
    gtk_box_append(GTK_BOX(trow), self->blt_ch_btns[i]);
  }
  gtk_box_append(GTK_BOX(box), trow);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Bitplanes", 0, 6, &self->sp_bpl), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Sprites", 0, 8, &self->sp_spr), 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Audio Ch", 0, 4, &self->sp_aud), 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Cop Mv", 0, 1000, &self->sp_cop), 1, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Blit W(w)", 1, 64, &self->sp_blt_w), 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Blit H(l)", 1, 512, &self->sp_blt_h), 1, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Blits/Fr", 0, 500, &self->sp_blits), 0, 3, 2, 1);
  gtk_box_append(GTK_BOX(box), grid);

  GtkWidget *bcard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_add_css_class(bcard, "amiga-section-card");
  self->meter_fill = gtk_level_bar_new();
  gtk_level_bar_set_min_value(GTK_LEVEL_BAR(self->meter_fill), 0.0);
  gtk_level_bar_set_max_value(GTK_LEVEL_BAR(self->meter_fill), 1.0);
  self->kv_grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(self->kv_grid), 4);
  gtk_grid_set_row_spacing(GTK_GRID(self->kv_grid), 3);
  gtk_grid_set_column_homogeneous(GTK_GRID(self->kv_grid), TRUE);
  gtk_box_append(GTK_BOX(bcard), self->meter_fill);
  gtk_box_append(GTK_BOX(bcard), self->kv_grid);
  gtk_box_append(GTK_BOX(box), bcard);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), box);
  gtk_box_append(GTK_BOX(self), scr);
  push_to_widgets(self);
}

AmigaDmaView *amiga_dma_view_new(void) { return g_object_new(AMIGA_TYPE_DMA_VIEW, NULL); }

void
amiga_dma_view_set_snapshot(AmigaDmaView *self, const AmigaCustomSnapshot *snap)
{
  g_return_if_fail(AMIGA_IS_DMA_VIEW(self));
  if (!snap) return;
  self->last_snap = *snap;
  self->has_snap = true;
  uint16_t c0 = snap->bplcon[0];
  uint8_t bpu = (uint8_t)((c0 >> 12) & 0x7u);
  if (bpu > 6) bpu = 6;
  self->input.ntsc = snap->ntsc;
  self->input.hires = (c0 & 0x8000u) != 0;
  self->input.bitplanes = bpu;
  uint8_t aud = 0;
  for (int i = 0; i < 4; i++) if ((snap->dmaconr & (1u << i)) != 0) aud++;
  self->input.audio_channels = aud;
  self->input.sprites_active = ((snap->dmaconr & 0x0020u) != 0) ? 8u : 0u;
  gtk_label_set_text(GTK_LABEL(self->prov_badge), "SNAP-SYNCED");
  push_to_widgets(self);
}
