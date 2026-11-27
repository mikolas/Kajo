#include "amiga_chips_view.h"
#include "amiga_decode.h"
#include <string.h>

struct _AmigaChipsView {
  GtkBox              parent_instance;

  AmigaChipsCallbacks cbs;
  gpointer            user_data;

  GtkWidget          *dma_pills[7];
  GtkWidget          *grid_live;
  GtkWidget          *grid_disp;
  GtkWidget          *grid_blt;
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
clear_grid(GtkWidget *grid)
{
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(grid)) != NULL) {
    gtk_grid_remove(GTK_GRID(grid), ch);
  }
}

static void
on_ptr_btn_clicked(GtkButton *b, gpointer ud)
{
  AmigaChipsView *self = AMIGA_CHIPS_VIEW(ud);
  uint32_t addr = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "ptr-addr"));
  if (self->cbs.jump_mem && addr != 0) self->cbs.jump_mem(self, addr, self->user_data);
}

static void
grid_add_kv(AmigaChipsView *self,
            GtkWidget      *grid,
            int             col,
            int             row,
            const char     *key,
            const char     *val,
            uint32_t        jump_ptr)
{
  GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(cell, "amiga-kv-cell");
  gtk_widget_set_hexpand(cell, TRUE);
  GtkWidget *lk = gtk_label_new(key);
  gtk_widget_add_css_class(lk, "amiga-muted-hint");
  gtk_label_set_xalign(GTK_LABEL(lk), 0.0f);
  gtk_widget_set_hexpand(lk, TRUE);
  gtk_box_append(GTK_BOX(cell), lk);

  if (jump_ptr != 0) {
    GtkWidget *bp = gtk_button_new_with_label(val);
    gtk_widget_add_css_class(bp, "amiga-kv-ptr");
    g_object_set_data(G_OBJECT(bp), "ptr-addr", GUINT_TO_POINTER(jump_ptr));
    g_signal_connect(bp, "clicked", G_CALLBACK(on_ptr_btn_clicked), self);
    gtk_box_append(GTK_BOX(cell), bp);
  } else {
    GtkWidget *lv = gtk_label_new(val);
    gtk_widget_add_css_class(lv, "amiga-kv-val");
    gtk_label_set_xalign(GTK_LABEL(lv), 1.0f);
    gtk_box_append(GTK_BOX(cell), lv);
  }
  gtk_grid_attach(GTK_GRID(grid), cell, col, row, 1, 1);
}

static GtkWidget *
make_kv_grid(GtkWidget *card)
{
  GtkWidget *g = gtk_grid_new();
  gtk_widget_add_css_class(g, "amiga-kv-grid");
  gtk_grid_set_column_spacing(GTK_GRID(g), 4);
  gtk_grid_set_row_spacing(GTK_GRID(g), 3);
  gtk_grid_set_column_homogeneous(GTK_GRID(g), TRUE);
  gtk_box_append(GTK_BOX(card), g);
  return g;
}

static void on_trace_blits_toggled(GtkToggleButton *tb, gpointer ud) {
  AmigaChipsView *self = AMIGA_CHIPS_VIEW(ud);
  if (self->cbs.toggle_blit_trace) self->cbs.toggle_blit_trace(self, gtk_toggle_button_get_active(tb), self->user_data);
}
static void on_read_blits_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  AmigaChipsView *self = AMIGA_CHIPS_VIEW(ud);
  if (self->cbs.refresh_blit_trace) self->cbs.refresh_blit_trace(self, self->user_data);
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
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scr, TRUE);
  gtk_widget_set_hexpand(scr, TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);

  GtkWidget *c_live = make_card(box, "LIVE", "amiga-badge-live", "READABLE CUSTOM REGS");
  GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  static const char *const pnames[7] = { "MST", "BPL", "COP", "BLT", "SPR", "DSK", "AUD" };
  for (int i = 0; i < 7; i++) {
    self->dma_pills[i] = gtk_label_new(pnames[i]);
    gtk_widget_add_css_class(self->dma_pills[i], "amiga-chip-pill");
    gtk_box_append(GTK_BOX(prow), self->dma_pills[i]);
  }
  gtk_box_append(GTK_BOX(c_live), prow);
  self->grid_live = make_kv_grid(c_live);

  GtkWidget *c_disp = make_card(box, "EMU SNAPSHOT", "amiga-badge-snapshot", "DISPLAY & COPPER (CLICKABLE PTRS)");
  self->grid_disp = make_kv_grid(c_disp);

  GtkWidget *c_blt = make_card(box, "EMU SNAPSHOT", "amiga-badge-snapshot", "BLITTER & DMA POINTERS");
  self->grid_blt = make_kv_grid(c_blt);

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

  GtkWidget *c_wo = make_card(box, "WO — UNAVAILABLE", "amiga-badge-placeholder", "OMITTED BY AMIBERRY SHADOW");
  GtkWidget *wo_txt = gtk_label_new(
      "AUD0..3 ($DFF0A0..0DF) return zero and AGA COLOR32..255 are absent in save_custom().");
  gtk_label_set_xalign(GTK_LABEL(wo_txt), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(wo_txt), TRUE);
  gtk_box_append(GTK_BOX(c_wo), wo_txt);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), box);
  gtk_box_append(GTK_BOX(self), scr);
}

AmigaChipsView *amiga_chips_view_new(void) { return g_object_new(AMIGA_TYPE_CHIPS_VIEW, NULL); }

void amiga_chips_view_set_callbacks(AmigaChipsView *self, const AmigaChipsCallbacks *cbs, gpointer ud) {
  g_return_if_fail(AMIGA_IS_CHIPS_VIEW(self));
  if (cbs) self->cbs = *cbs; else memset(&self->cbs, 0, sizeof(self->cbs));
  self->user_data = ud;
}

void
amiga_chips_view_set_snapshot(AmigaChipsView *self, const AmigaCustomSnapshot *snap)
{
  g_return_if_fail(AMIGA_IS_CHIPS_VIEW(self));
  if (!snap) return;

  static const uint16_t k_dma_bits[7] = { 0x0200u, 0x0100u, 0x0080u, 0x0040u, 0x0020u, 0x0010u, 0x000Fu };
  bool master = (snap->dmaconr & 0x0200u) != 0;
  for (int i = 0; i < 7; i++) {
    bool on = (i == 0) ? master : (master && (snap->dmaconr & k_dma_bits[i]) != 0);
    if (on) gtk_widget_add_css_class(self->dma_pills[i], "on");
    else gtk_widget_remove_css_class(self->dma_pills[i], "on");
  }

  clear_grid(self->grid_live);
  g_autofree char *s_dma = g_strdup_printf("$%04X", snap->dmaconr);
  g_autofree char *s_vp = g_strdup_printf("$%04X %s", snap->vposr, snap->ntsc ? "NTSC" : "PAL");
  g_autofree char *s_vhp = g_strdup_printf("$%04X", snap->vhposr);
  g_autofree char *s_ien = g_strdup_printf("$%04X", snap->intenar);
  g_autofree char *s_irq = g_strdup_printf("$%04X", snap->intreqr);
  g_autofree char *s_clx = g_strdup_printf("$%04X", snap->clxdat);
  grid_add_kv(self, self->grid_live, 0, 0, "DMACONR", s_dma, 0);
  grid_add_kv(self, self->grid_live, 1, 0, "VPOSR", s_vp, 0);
  grid_add_kv(self, self->grid_live, 0, 1, "VHPOSR", s_vhp, 0);
  grid_add_kv(self, self->grid_live, 1, 1, "INTENAR", s_ien, 0);
  grid_add_kv(self, self->grid_live, 0, 2, "INTREQR", s_irq, 0);
  grid_add_kv(self, self->grid_live, 1, 2, "CLXDAT", s_clx, 0);

  clear_grid(self->grid_disp);
  g_autofree char *s_cop1 = g_strdup_printf("$%06X", snap->cop1lc);
  g_autofree char *s_cop2 = g_strdup_printf("$%06X", snap->cop2lc);
  g_autofree char *s_bc0 = g_strdup_printf("$%04X", snap->bplcon[0]);
  g_autofree char *s_bpl1 = g_strdup_printf("$%06X", snap->bplpt[0]);
  g_autofree char *s_diw = g_strdup_printf("$%04X..%04X", snap->diwstrt, snap->diwstop);
  g_autofree char *s_ddf = g_strdup_printf("$%04X..%04X", snap->ddfstrt, snap->ddfstop);
  g_autofree char *s_mod = g_strdup_printf("%d / %d", snap->bpl1mod, snap->bpl2mod);
  g_autofree char *s_copc = g_strdup_printf("$%04X", snap->copcon);
  grid_add_kv(self, self->grid_disp, 0, 0, "COP1LC", s_cop1, snap->cop1lc ? snap->cop1lc : 1u);
  grid_add_kv(self, self->grid_disp, 1, 0, "COP2LC", s_cop2, snap->cop2lc ? snap->cop2lc : 1u);
  grid_add_kv(self, self->grid_disp, 0, 1, "BPL1PT", s_bpl1, snap->bplpt[0] ? snap->bplpt[0] : 1u);
  grid_add_kv(self, self->grid_disp, 1, 1, "BPLCON0", s_bc0, 0);
  grid_add_kv(self, self->grid_disp, 0, 2, "DIW", s_diw, 0);
  grid_add_kv(self, self->grid_disp, 1, 2, "DDF", s_ddf, 0);
  grid_add_kv(self, self->grid_disp, 0, 3, "MOD 1/2", s_mod, 0);
  grid_add_kv(self, self->grid_disp, 1, 3, "COPCON", s_copc, 0);

  clear_grid(self->grid_blt);
  AmigaBltcon0Info bi = amiga_decode_bltcon0(snap->bltcon0);
  uint16_t bh = (uint16_t)((snap->bltsize >> 6) & 0x3FFu);
  uint16_t bw = (uint16_t)(snap->bltsize & 0x3Fu);
  if (snap->bltsize != 0 && bh == 0) bh = 1024;
  if (snap->bltsize != 0 && bw == 0) bw = 64;
  g_autofree char *s_bltc0 = g_strdup_printf("$%04X %c%c%c%c", snap->bltcon0,
                                             bi.use_a ? 'A' : '-', bi.use_b ? 'B' : '-',
                                             bi.use_c ? 'C' : '-', bi.use_d ? 'D' : '-');
  g_autofree char *s_bltsz = g_strdup_printf("%u×%uw", bh, bw);
  g_autofree char *s_ba = g_strdup_printf("$%06X", snap->bltpt[2]);
  g_autofree char *s_bb = g_strdup_printf("$%06X", snap->bltpt[1]);
  g_autofree char *s_bc = g_strdup_printf("$%06X", snap->bltpt[0]);
  g_autofree char *s_bd = g_strdup_printf("$%06X", snap->bltpt[3]);
  grid_add_kv(self, self->grid_blt, 0, 0, "BLTCON0", s_bltc0, 0);
  grid_add_kv(self, self->grid_blt, 1, 0, "BLTSIZE", s_bltsz, 0);
  grid_add_kv(self, self->grid_blt, 0, 1, "BLTAPT", s_ba, snap->bltpt[2] ? snap->bltpt[2] : 1u);
  grid_add_kv(self, self->grid_blt, 1, 1, "BLTBPT", s_bb, snap->bltpt[1] ? snap->bltpt[1] : 1u);
  grid_add_kv(self, self->grid_blt, 0, 2, "BLTCPT", s_bc, snap->bltpt[0] ? snap->bltpt[0] : 1u);
  grid_add_kv(self, self->grid_blt, 1, 2, "BLTDPT", s_bd, snap->bltpt[3] ? snap->bltpt[3] : 1u);
}

void
amiga_chips_view_set_watch_events(AmigaChipsView *self, GPtrArray *events)
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
