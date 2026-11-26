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
  GtkWidget          *bar_canvas;
  GtkWidget          *toggle_ntsc;
  GtkWidget          *toggle_hires;
  GtkWidget          *sp_bpl;
  GtkWidget          *sp_spr;
  GtkWidget          *sp_aud;
  GtkWidget          *sp_blt_ch;
  GtkWidget          *sp_blt_w;
  GtkWidget          *sp_blt_h;
  GtkWidget          *sp_blits;
  GtkWidget          *sp_cop;
  GtkWidget          *meter_fill;
  GtkWidget          *lbl_budget;
};

G_DEFINE_FINAL_TYPE(AmigaDmaView, amiga_dma_view, GTK_TYPE_BOX)

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

  g_autofree char *sum = g_strdup_printf(
      "%s %s · %u BPL · Line: %u BPL + %u COP slots (%u free odd/line)\n"
      "Frame odd slots: %u used / %u avail (%.1f%%) · Margin: %+d\n"
      "Blitter: %u slots · Copper: %u slots · 68k even clocks/fr: %u",
      self->input.ntsc ? "NTSC 262" : "PAL 312",
      self->input.hires ? "HIRES" : "LORES",
      self->input.bitplanes,
      self->budget.bpl_slots_line, self->budget.cop_slots_line,
      self->budget.free_odd_slots_visible_line,
      self->budget.used_odd_slots_frame, self->budget.avail_odd_slots_frame,
      pct, self->budget.odd_slot_margin,
      self->budget.blitter_slots_frame, self->budget.copper_slots_frame,
      self->budget.cpu_even_cycles_frame);
  gtk_label_set_text(GTK_LABEL(self->lbl_budget), sum);
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
  self->input.blitter_ch = (uint8_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->sp_blt_ch));
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
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_blt_ch), self->input.blitter_ch);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_blt_w), self->input.blit_w_words);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_blt_h), self->input.blit_h_lines);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_blits), self->input.blits_per_frame);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->sp_cop), self->input.copper_moves_per_frame);
  self->updating = false;
  recalc_and_draw(self);
}

static void
on_spin_changed(GtkSpinButton *s G_GNUC_UNUSED, gpointer ud)
{
  sync_from_widgets(AMIGA_DMA_VIEW(ud));
}

static void
on_toggle_changed(GtkToggleButton *tb G_GNUC_UNUSED, gpointer ud)
{
  sync_from_widgets(AMIGA_DMA_VIEW(ud));
}

static void
on_preset_clicked(GtkButton *b, gpointer ud)
{
  AmigaDmaView *self = AMIGA_DMA_VIEW(ud);
  int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "preset-id"));
  if (id == 4) {
    if (self->has_snap) amiga_dma_view_set_snapshot(self, &self->last_snap);
    return;
  }
  self->input.ntsc = false;
  self->input.hires = (id == 3);
  self->input.bitplanes = (id == 1) ? 5u : ((id == 2) ? 6u : 4u);
  self->input.sprites_active = (id == 2) ? 4u : 8u;
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

  double bar_x = 8.0, bar_y = 22.0;
  double bar_w = MAX(40.0, (double)w - 16.0);
  double bar_h = 26.0;
  double total_cc = 227.0;

  /* 113 odd DMA slots in CC coordinates; each DMA slot occupies ~2 color clocks inside the 227 line */
  double ref_cc = 4.0 * 2.0;
  double dsk_cc = 3.0 * 2.0;
  double aud_cc = (double)self->input.audio_channels * 2.0;
  double spr_cc = (double)self->input.sprites_active * 2.0 * 2.0;
  double bpl_cc = (double)self->budget.bpl_slots_line * (self->input.hires ? 1.0 : 1.5);
  if (bpl_cc + ref_cc + dsk_cc + aud_cc + spr_cc > total_cc) {
    bpl_cc = MAX(0.0, total_cc - (ref_cc + dsk_cc + aud_cc + spr_cc));
  }
  double cop_cc = MIN((double)self->budget.cop_slots_line * 2.0,
                      MAX(0.0, total_cc - (ref_cc + dsk_cc + aud_cc + spr_cc + bpl_cc)));
  double blt_cc = MIN((double)(self->budget.blitter_slots_frame / 312u) * 2.0,
                      MAX(0.0, total_cc - (ref_cc + dsk_cc + aud_cc + spr_cc + bpl_cc + cop_cc)));

  struct { double cc; double r, g, b; const char *name; } segs[7] = {
    { ref_cc, 0.35, 0.38, 0.42, "REF" },
    { dsk_cc, 0.48, 0.42, 0.65, "DSK" },
    { aud_cc, 0.25, 0.72, 0.50, "AUD" },
    { spr_cc, 0.85, 0.35, 0.72, "SPR" },
    { bpl_cc, 0.00, 0.48, 0.80, "BPL" },
    { cop_cc, 0.00, 0.85, 0.94, "COP" },
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

  cairo_set_source_rgb(cr, 0.24, 0.26, 0.30);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, bar_x + 0.5, bar_y + 0.5, bar_w - 1.0, bar_h - 1.0);
  cairo_stroke(cr);

  /* Ruler labels & legend */
  cairo_select_font_face(cr, "Fira Code", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 9.5);
  cairo_set_source_rgb(cr, 0.68, 0.72, 0.78);
  cairo_move_to(cr, bar_x, 14.0);
  cairo_show_text(cr, "CC 0");
  cairo_move_to(cr, bar_x + bar_w * 0.5 - 12.0, 14.0);
  cairo_show_text(cr, "113");
  cairo_move_to(cr, bar_x + bar_w - 36.0, 14.0);
  cairo_show_text(cr, "227 CC");

  double lx = bar_x;
  for (int i = 0; i < 7; i++) {
    cairo_set_source_rgb(cr, segs[i].r, segs[i].g, segs[i].b);
    cairo_rectangle(cr, lx, bar_y + bar_h + 12.0, 8.0, 8.0);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.75, 0.78, 0.82);
    cairo_move_to(cr, lx + 11.0, bar_y + bar_h + 20.0);
    cairo_show_text(cr, segs[i].name);
    lx += 42.0;
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
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(sp), 0);
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

  self->input.bitplanes = 4;
  self->input.blitter_ch = 3;
  self->input.blit_w_words = 4;
  self->input.blit_h_lines = 32;
  self->input.blits_per_frame = 16;
  self->input.copper_moves_per_frame = 48;
  self->input.sprites_active = 8;
  self->input.audio_channels = 4;

  GtkWidget *scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scr, TRUE);
  gtk_widget_set_hexpand(scr, TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);

  /* Header + presets */
  GtkWidget *r0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  self->prov_badge = gtk_label_new("PARAM");
  gtk_widget_add_css_class(self->prov_badge, "amiga-badge-snapshot");
  gtk_box_append(GTK_BOX(r0), self->prov_badge);
  static const char *const pnames[5] = { "4BPL", "5BPL", "6HAM", "HIRES", "↻" };
  for (int i = 0; i < 5; i++) {
    GtkWidget *b = gtk_button_new_with_label(pnames[i]);
    gtk_widget_add_css_class(b, "amiga-insp-tab");
    g_object_set_data(G_OBJECT(b), "preset-id", GINT_TO_POINTER(i));
    g_signal_connect(b, "clicked", G_CALLBACK(on_preset_clicked), self);
    gtk_box_append(GTK_BOX(r0), b);
  }
  gtk_box_append(GTK_BOX(box), r0);

  /* Scanline Bar */
  self->bar_canvas = gtk_drawing_area_new();
  gtk_widget_set_size_request(self->bar_canvas, -1, 76);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->bar_canvas), on_bar_draw, self, NULL);
  gtk_box_append(GTK_BOX(box), self->bar_canvas);

  /* Switches + 2-column Parameter Grid */
  GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  self->toggle_ntsc = gtk_toggle_button_new_with_label("NTSC (262 lines)");
  gtk_widget_add_css_class(self->toggle_ntsc, "amiga-insp-tab");
  g_signal_connect(self->toggle_ntsc, "toggled", G_CALLBACK(on_toggle_changed), self);
  self->toggle_hires = gtk_toggle_button_new_with_label("HIRES (640px)");
  gtk_widget_add_css_class(self->toggle_hires, "amiga-insp-tab");
  g_signal_connect(self->toggle_hires, "toggled", G_CALLBACK(on_toggle_changed), self);
  gtk_box_append(GTK_BOX(trow), self->toggle_ntsc);
  gtk_box_append(GTK_BOX(trow), self->toggle_hires);
  gtk_box_append(GTK_BOX(box), trow);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Bitplanes", 0, 6, &self->sp_bpl), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Sprites", 0, 8, &self->sp_spr), 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Audio Ch", 0, 4, &self->sp_aud), 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Blit Ch", 1, 4, &self->sp_blt_ch), 1, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Blit W(w)", 1, 64, &self->sp_blt_w), 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Blit H(l)", 1, 512, &self->sp_blt_h), 1, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Blits/Fr", 0, 500, &self->sp_blits), 0, 3, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), make_spin_pair(self, "Cop Mv", 0, 1000, &self->sp_cop), 1, 3, 1, 1);
  gtk_box_append(GTK_BOX(box), grid);

  /* Utilization Meter & Budget Summary */
  GtkWidget *bcard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_add_css_class(bcard, "amiga-section-card");
  self->meter_fill = gtk_level_bar_new();
  gtk_level_bar_set_min_value(GTK_LEVEL_BAR(self->meter_fill), 0.0);
  gtk_level_bar_set_max_value(GTK_LEVEL_BAR(self->meter_fill), 1.0);
  self->lbl_budget = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_budget), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_budget), TRUE);
  gtk_box_append(GTK_BOX(bcard), self->meter_fill);
  gtk_box_append(GTK_BOX(bcard), self->lbl_budget);
  gtk_box_append(GTK_BOX(box), bcard);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), box);
  gtk_box_append(GTK_BOX(self), scr);

  push_to_widgets(self);
}

AmigaDmaView *
amiga_dma_view_new(void)
{
  return g_object_new(AMIGA_TYPE_DMA_VIEW, NULL);
}

void
amiga_dma_view_set_snapshot(AmigaDmaView              *self,
                            const AmigaCustomSnapshot *snap)
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
  for (int i = 0; i < 4; i++) {
    if ((snap->dmaconr & (1u << i)) != 0) aud++;
  }
  self->input.audio_channels = aud;
  self->input.sprites_active = ((snap->dmaconr & 0x0020u) != 0) ? 8u : 0u;

  gtk_label_set_text(GTK_LABEL(self->prov_badge), "SNAP-SYNCED");
  push_to_widgets(self);
}
