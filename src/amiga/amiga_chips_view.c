#include "amiga_chips_view.h"
#include "amiga_decode.h"
#include <string.h>

struct _AmigaChipsView {
  GtkBox              parent_instance;

  AmigaChipsCallbacks cbs;
  gpointer            user_data;

  GtkWidget          *dma_pills[7];
  GtkWidget          *lbl_live;
  GtkWidget          *lbl_display;
  GtkWidget          *lbl_blitter;
  GtkWidget          *btn_trace_blits;
  GtkWidget          *watch_list;
};

G_DEFINE_FINAL_TYPE(AmigaChipsView, amiga_chips_view, GTK_TYPE_BOX)

static GtkWidget *
make_card(GtkWidget *parent_box, const char *badge_text, const char *badge_css, const char *title)
{
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_add_css_class(card, "amiga-section-card");

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);
  GtkWidget *badge = gtk_label_new(badge_text);
  gtk_widget_add_css_class(badge, badge_css);
  GtkWidget *tl = gtk_label_new(title);
  gtk_widget_add_css_class(tl, "amiga-disasm-header");
  gtk_label_set_xalign(GTK_LABEL(tl), 0.0f);
  gtk_box_append(GTK_BOX(hdr), badge);
  gtk_box_append(GTK_BOX(hdr), tl);
  gtk_box_append(GTK_BOX(card), hdr);

  gtk_box_append(GTK_BOX(parent_box), card);
  return card;
}

static void
on_trace_blits_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaChipsView *self = AMIGA_CHIPS_VIEW(ud);
  if (self->cbs.toggle_blit_trace) {
    self->cbs.toggle_blit_trace(self, gtk_toggle_button_get_active(tb), self->user_data);
  }
}

static void
on_read_blits_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaChipsView *self = AMIGA_CHIPS_VIEW(ud);
  if (self->cbs.refresh_blit_trace) {
    self->cbs.refresh_blit_trace(self, self->user_data);
  }
}

static void
amiga_chips_view_class_init(AmigaChipsViewClass *klass G_GNUC_UNUSED)
{
}

static void
amiga_chips_view_init(AmigaChipsView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);

  GtkWidget *scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scr, TRUE);
  gtk_widget_set_hexpand(scr, TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);

  /* Card 1: LIVE Readable */
  GtkWidget *c_live = make_card(box, "LIVE", "amiga-badge-live", "READABLE CUSTOM REGS");
  GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  static const char *const pnames[7] = { "MST", "BPL", "COP", "BLT", "SPR", "DSK", "AUD" };
  for (int i = 0; i < 7; i++) {
    self->dma_pills[i] = gtk_label_new(pnames[i]);
    gtk_widget_add_css_class(self->dma_pills[i], "amiga-chip-pill");
    gtk_box_append(GTK_BOX(prow), self->dma_pills[i]);
  }
  gtk_box_append(GTK_BOX(c_live), prow);
  self->lbl_live = gtk_label_new("Pause or click ↻ SNAP to read DMACONR / VPOSR / INT*");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_live), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_live), TRUE);
  gtk_box_append(GTK_BOX(c_live), self->lbl_live);

  /* Card 2: EMU SNAPSHOT Display & Copper */
  GtkWidget *c_disp = make_card(box, "EMU SNAPSHOT", "amiga-badge-snapshot", "DISPLAY & COPPER (m dff000,200)");
  self->lbl_display = gtk_label_new("BPLCON0..2 / DIW / DDF / BPLPT / COP1LC pending.");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_display), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_display), TRUE);
  gtk_box_append(GTK_BOX(c_disp), self->lbl_display);

  /* Card 3: EMU SNAPSHOT Blitter */
  GtkWidget *c_blt = make_card(box, "EMU SNAPSHOT", "amiga-badge-snapshot", "BLITTER STATE");
  self->lbl_blitter = gtk_label_new("BLTCON0 / BLTSIZE / BLTxPT / BLTxMOD pending.");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_blitter), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_blitter), TRUE);
  gtk_box_append(GTK_BOX(c_blt), self->lbl_blitter);

  /* Card 4: WATCHED Blit Log */
  GtkWidget *c_wat = make_card(box, "WATCHED", "amiga-badge-watched", "BLIT LOG (log_only=1)");
  GtkWidget *wbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->btn_trace_blits = gtk_toggle_button_new_with_label("👁 TRACE BLITS ($DFF040..076)");
  gtk_widget_add_css_class(self->btn_trace_blits, "amiga-insp-tab");
  g_signal_connect(self->btn_trace_blits, "toggled", G_CALLBACK(on_trace_blits_toggled), self);
  GtkWidget *btn_r = gtk_button_new_with_label("↻ READ");
  gtk_widget_add_css_class(btn_r, "ide-toolbar-btn");
  g_signal_connect(btn_r, "clicked", G_CALLBACK(on_read_blits_clicked), self);
  gtk_box_append(GTK_BOX(wbar), self->btn_trace_blits);
  gtk_box_append(GTK_BOX(wbar), btn_r);
  gtk_box_append(GTK_BOX(c_wat), wbar);

  self->watch_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->watch_list, "amiga-diag-list");
  gtk_box_append(GTK_BOX(c_wat), self->watch_list);

  /* Card 5: WO - UNAVAILABLE */
  GtkWidget *c_wo = make_card(box, "WO — UNAVAILABLE", "amiga-badge-placeholder", "OMITTED BY AMIBERRY SHADOW");
  GtkWidget *wo_txt = gtk_label_new(
      "AUD0..3 ($DFF0A0..0DF) return zero and AGA 24-bit low nibbles / COLOR32..255 are not present in Amiberry's 512-byte save_custom() block.");
  gtk_label_set_xalign(GTK_LABEL(wo_txt), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(wo_txt), TRUE);
  gtk_box_append(GTK_BOX(c_wo), wo_txt);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), box);
  gtk_box_append(GTK_BOX(self), scr);
}

AmigaChipsView *
amiga_chips_view_new(void)
{
  return g_object_new(AMIGA_TYPE_CHIPS_VIEW, NULL);
}

void
amiga_chips_view_set_callbacks(AmigaChipsView            *self,
                               const AmigaChipsCallbacks *cbs,
                               gpointer                   ud)
{
  g_return_if_fail(AMIGA_IS_CHIPS_VIEW(self));
  if (cbs) self->cbs = *cbs;
  else memset(&self->cbs, 0, sizeof(self->cbs));
  self->user_data = ud;
}

void
amiga_chips_view_set_snapshot(AmigaChipsView            *self,
                              const AmigaCustomSnapshot *snap)
{
  g_return_if_fail(AMIGA_IS_CHIPS_VIEW(self));
  if (!snap) return;

  static const uint16_t k_dma_bits[7] = {
    0x0200u, /* DMAEN / MST */
    0x0100u, /* BPLEN */
    0x0080u, /* COPEN */
    0x0040u, /* BLTEN */
    0x0020u, /* SPREN */
    0x0010u, /* DSKEN */
    0x000Fu, /* AUD0..3 */
  };
  bool master = (snap->dmaconr & 0x0200u) != 0;
  for (int i = 0; i < 7; i++) {
    bool on = (i == 0) ? master : (master && (snap->dmaconr & k_dma_bits[i]) != 0);
    if (on) gtk_widget_add_css_class(self->dma_pills[i], "on");
    else gtk_widget_remove_css_class(self->dma_pills[i], "on");
  }

  g_autofree char *s_live = g_strdup_printf(
      "DMACONR: $%04X   VPOSR: $%04X (%s)   VHPOSR: $%04X\n"
      "INTENAR: $%04X   INTREQR: $%04X   ADKCONR: $%04X   CLXDAT: $%04X",
      snap->dmaconr, snap->vposr, snap->ntsc ? "NTSC" : "PAL", snap->vhposr,
      snap->intenar, snap->intreqr, snap->adkconr, snap->clxdat);
  gtk_label_set_text(GTK_LABEL(self->lbl_live), s_live);

  g_autofree char *s_disp = g_strdup_printf(
      "COP1LC: $%06X   COP2LC: $%06X   COPCON: $%04X\n"
      "BPLCON0: $%04X  BPLCON1: $%04X  BPLCON2: $%04X  FMODE: $%04X\n"
      "DIW: $%04X..$%04X   DDF: $%04X..$%04X   MOD: %d / %d\n"
      "BPL1PT: $%06X   BPL2PT: $%06X   BPL3PT: $%06X",
      snap->cop1lc, snap->cop2lc, snap->copcon,
      snap->bplcon[0], snap->bplcon[1], snap->bplcon[2], snap->fmode,
      snap->diwstrt, snap->diwstop, snap->ddfstrt, snap->ddfstop,
      snap->bpl1mod, snap->bpl2mod,
      snap->bplpt[0], snap->bplpt[1], snap->bplpt[2]);
  gtk_label_set_text(GTK_LABEL(self->lbl_display), s_disp);

  AmigaBltcon0Info bi = amiga_decode_bltcon0(snap->bltcon0);
  uint16_t bh = (uint16_t)((snap->bltsize >> 6) & 0x3FFu);
  uint16_t bw = (uint16_t)(snap->bltsize & 0x3Fu);
  if (snap->bltsize != 0 && bh == 0) bh = 1024;
  if (snap->bltsize != 0 && bw == 0) bw = 64;
  g_autofree char *s_blt = g_strdup_printf(
      "BLTCON0: $%04X (ASH=%u [%c%c%c%c] %s)\n"
      "BLTCON1: $%04X   AFWM: $%04X   ALWM: $%04X   SIZE: %u×%uw\n"
      "BLTAPT: $%06X (%d)   BLTBPT: $%06X (%d)\n"
      "BLTCPT: $%06X (%d)   BLTDPT: $%06X (%d)",
      snap->bltcon0, bi.ash,
      bi.use_a ? 'A' : '-', bi.use_b ? 'B' : '-', bi.use_c ? 'C' : '-', bi.use_d ? 'D' : '-',
      bi.equation,
      snap->bltcon1, snap->bltafwm, snap->bltalwm, bh, bw,
      snap->bltpt[2], snap->bltmod[2], snap->bltpt[1], snap->bltmod[1],
      snap->bltpt[0], snap->bltmod[0], snap->bltpt[3], snap->bltmod[3]);
  gtk_label_set_text(GTK_LABEL(self->lbl_blitter), s_blt);
}

void
amiga_chips_view_set_watch_events(AmigaChipsView *self,
                                  GPtrArray      *events)
{
  g_return_if_fail(AMIGA_IS_CHIPS_VIEW(self));
  GtkWidget *old;
  while ((old = gtk_widget_get_first_child(self->watch_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->watch_list), old);
  }
  if (!events || events->len == 0) {
    GtkWidget *empty = gtk_label_new("No non-breaking blit writes recorded.");
    gtk_label_set_xalign(GTK_LABEL(empty), 0.0f);
    gtk_list_box_append(GTK_LIST_BOX(self->watch_list), empty);
    return;
  }
  for (guint i = 0; i < events->len; i++) {
    const AmigaWatchEvent *ev = g_ptr_array_index(events, i);
    const AmigaHwRegInfo *ri = amiga_hw_reg_lookup((uint16_t)(ev->addr & 0x1FEu));
    g_autofree char *txt = g_strdup_printf(
        "F%u V%u $%06X(%s) ← $%X @PC $%06X",
        ev->frame, ev->vpos, ev->addr, ri ? ri->name : "REG", ev->new_val, ev->pc);
    GtkWidget *l = gtk_label_new(txt);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_list_box_append(GTK_LIST_BOX(self->watch_list), l);
  }
}
