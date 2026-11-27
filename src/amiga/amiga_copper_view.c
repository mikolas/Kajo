#include "amiga_copper_view.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
  COP_FILTER_ALL = 0,
  COP_FILTER_WAITS,
  COP_FILTER_BLT,
  COP_FILTER_DISP,
} CopFilterMode;

struct _AmigaCopperView {
  GtkBox               parent_instance;

  AmigaCopperCallbacks cbs;
  gpointer             user_data;

  AmigaCustomSnapshot  last_snap;
  bool                 has_snap;
  uint32_t             base_addr;
  AmigaCopInsn         insns[256];
  size_t               n_insns;
  CopFilterMode        filter;

  GtkWidget           *prov_badge;
  GtkWidget           *dd_source;
  GtkWidget           *entry_addr;
  GtkWidget           *btn_watch;
  GtkWidget           *filter_btns[4];
  GtkWidget           *lbl_summary;
  GtkWidget           *scan_bar;
  GtkWidget           *listbox;
};

G_DEFINE_FINAL_TYPE(AmigaCopperView, amiga_copper_view, GTK_TYPE_BOX)

static uint32_t
parse_hex_addr(const char *txt, bool *ok)
{
  if (ok) *ok = false;
  if (!txt || !txt[0]) return 0;
  const char *p = txt;
  while (*p == ' ') p++;
  if (p[0] == '$') p++;
  else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
  char *end = NULL;
  unsigned long v = strtoul(p, &end, 16);
  if (end && end != p) {
    if (ok) *ok = true;
    return (uint32_t)v;
  }
  return 0;
}

static bool
matches_filter(const AmigaCopInsn *in, CopFilterMode f)
{
  switch (f) {
    case COP_FILTER_WAITS:
      return in->kind == AMIGA_COP_WAIT || in->kind == AMIGA_COP_SKIP || in->kind == AMIGA_COP_END;
    case COP_FILTER_BLT:
      return in->is_blitter_reg || (in->kind == AMIGA_COP_WAIT && !in->bfd);
    case COP_FILTER_DISP:
      return in->is_display_reg;
    default:
      return true;
  }
}

static void
rebuild_rows(AmigaCopperView *self)
{
  GtkWidget *old;
  while ((old = gtk_widget_get_first_child(self->listbox)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->listbox), old);
  }

  size_t shown = 0;
  size_t copcon_warn = 0;
  for (size_t i = 0; i < self->n_insns; i++) {
    const AmigaCopInsn *in = &self->insns[i];
    if (in->is_blitter_reg) copcon_warn++;
    if (!matches_filter(in, self->filter)) continue;

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);

    g_autofree char *as = g_strdup_printf("$%06X", in->addr);
    GtkWidget *la = gtk_label_new(as);
    gtk_widget_add_css_class(la, "amiga-cop-addr");
    gtk_label_set_xalign(GTK_LABEL(la), 0.0f);
    gtk_widget_set_valign(la, GTK_ALIGN_CENTER);

    g_autofree char *hs = g_strdup_printf("%04X %04X", in->ir1, in->ir2);
    GtkWidget *lh = gtk_label_new(hs);
    gtk_widget_add_css_class(lh, "amiga-cop-hex");
    gtk_label_set_xalign(GTK_LABEL(lh), 0.0f);
    gtk_widget_set_valign(lh, GTK_ALIGN_CENTER);

    GtkWidget *lt = gtk_label_new(in->text);
    gtk_widget_add_css_class(lt, "amiga-cop-text");
    gtk_label_set_xalign(GTK_LABEL(lt), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lt), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lt, TRUE);
    gtk_widget_set_valign(lt, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hb), la);
    gtk_box_append(GTK_BOX(hb), lh);
    gtk_box_append(GTK_BOX(hb), lt);

    if (in->note[0]) {
      const char *short_note = in->is_blitter_reg ? "COPCON!"
                             : (in->is_line255 ? "L255"
                             : (!in->bfd && (in->kind == AMIGA_COP_WAIT || in->kind == AMIGA_COP_SKIP) ? "BFD=0"
                             : in->note));
      GtkWidget *badge = gtk_label_new(short_note);
      gtk_widget_set_tooltip_text(badge, in->note);
      gtk_widget_add_css_class(
          badge,
          in->is_blitter_reg ? "amiga-badge-watched" : "amiga-disasm-badge");
      gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(hb), badge);
    }

    gtk_list_box_append(GTK_LIST_BOX(self->listbox), hb);
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->listbox), (int)shown);
    if (row) g_object_set_data(G_OBJECT(row), "cop-addr", GUINT_TO_POINTER(in->addr));
    shown++;
  }

  g_autofree char *sum = g_strdup_printf(
      "%zu insns (%zu shown) · ~%zu DMA slots · COPCON=$%04X%s",
      self->n_insns, shown, self->n_insns * 2u,
      self->has_snap ? self->last_snap.copcon : 0u,
      (copcon_warn > 0 && self->has_snap && (self->last_snap.copcon & 2u) == 0)
          ? " [BLT BLOCKED!]" : "");
  gtk_label_set_text(GTK_LABEL(self->lbl_summary), sum);
}

static void
on_filter_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaCopperView *self = AMIGA_COPPER_VIEW(ud);
  if (!self->listbox || !gtk_toggle_button_get_active(tb)) return;
  for (int i = 0; i < 4; i++) {
    if (GTK_WIDGET(tb) == self->filter_btns[i]) {
      self->filter = (CopFilterMode)i;
      rebuild_rows(self);
      break;
    }
  }
}

static void
on_load_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaCopperView *self = AMIGA_COPPER_VIEW(ud);
  guint src = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_source));
  uint32_t addr = 0;
  if (src == 0 && self->has_snap) addr = self->last_snap.cop1lc;
  else if (src == 1 && self->has_snap) addr = self->last_snap.cop2lc;
  else {
    bool ok = false;
    addr = parse_hex_addr(gtk_editable_get_text(GTK_EDITABLE(self->entry_addr)), &ok);
    if (!ok) return;
  }
  if (self->cbs.request_mem && addr > 0) {
    self->cbs.request_mem(self, addr, 1024u, self->user_data);
  }
}

static void
on_scan_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaCopperView *self = AMIGA_COPPER_VIEW(ud);
  if (self->cbs.scan_chip_ram) {
    self->cbs.scan_chip_ram(self, self->user_data);
  }
}

static void
on_watch_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaCopperView *self = AMIGA_COPPER_VIEW(ud);
  if (self->cbs.toggle_watch_cop1lc) {
    self->cbs.toggle_watch_cop1lc(self, gtk_toggle_button_get_active(tb), self->user_data);
  }
}

static void
on_scan_hit_clicked(GtkButton *b, gpointer ud)
{
  AmigaCopperView *self = AMIGA_COPPER_VIEW(ud);
  uint32_t addr = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "hit-addr"));
  g_autofree char *s = g_strdup_printf("$%06X", addr);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_addr), s);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_source), 2);
  if (self->cbs.request_mem) {
    self->cbs.request_mem(self, addr, 1024u, self->user_data);
  }
}

static void
on_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  AmigaCopperView *self = AMIGA_COPPER_VIEW(ud);
  uint32_t addr = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "cop-addr"));
  if (self->cbs.jump_mem && addr > 0) {
    self->cbs.jump_mem(self, addr, self->user_data);
  }
}

static void
amiga_copper_view_class_init(AmigaCopperViewClass *klass G_GNUC_UNUSED)
{
}

static void
amiga_copper_view_init(AmigaCopperView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 5);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 8);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 8);

  GtkWidget *r0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(r0, GTK_ALIGN_CENTER);
  self->prov_badge = gtk_label_new("EMU SNAPSHOT");
  gtk_widget_add_css_class(self->prov_badge, "amiga-badge-snapshot");

  self->dd_source = gtk_drop_down_new_from_strings(
      (const char *[]){"COP1LC", "COP2LC", "MANUAL", NULL});
  gtk_widget_add_css_class(self->dd_source, "amiga-header-dd");

  self->entry_addr = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_addr, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_addr), "$020100");
  gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_addr), 7);
  gtk_widget_set_hexpand(self->entry_addr, TRUE);
  g_signal_connect(self->entry_addr, "activate", G_CALLBACK(on_load_clicked), self);

  GtkWidget *btn_go = gtk_button_new_with_label("↻");
  gtk_widget_add_css_class(btn_go, "ide-toolbar-btn");
  g_signal_connect(btn_go, "clicked", G_CALLBACK(on_load_clicked), self);

  GtkWidget *btn_scan = gtk_button_new_with_label("🔎 SCAN");
  gtk_widget_add_css_class(btn_scan, "ide-toolbar-btn");
  g_signal_connect(btn_scan, "clicked", G_CALLBACK(on_scan_clicked), self);

  self->btn_watch = gtk_toggle_button_new_with_label("👁 $080");
  gtk_widget_add_css_class(self->btn_watch, "amiga-insp-tab");
  gtk_widget_set_tooltip_text(self->btn_watch, "Watch $DFF080 COP1LC writes (log_only=1)");
  g_signal_connect(self->btn_watch, "toggled", G_CALLBACK(on_watch_toggled), self);

  gtk_box_append(GTK_BOX(r0), self->prov_badge);
  gtk_box_append(GTK_BOX(r0), self->dd_source);
  gtk_box_append(GTK_BOX(r0), self->entry_addr);
  gtk_box_append(GTK_BOX(r0), btn_go);
  gtk_box_append(GTK_BOX(r0), btn_scan);
  gtk_box_append(GTK_BOX(r0), self->btn_watch);
  gtk_box_append(GTK_BOX(self), r0);

  GtkWidget *fbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  static const char *const flabels[4] = { "ALL", "WAITS", "BLT", "DISP" };
  for (int i = 0; i < 4; i++) {
    self->filter_btns[i] = gtk_toggle_button_new_with_label(flabels[i]);
    gtk_widget_add_css_class(self->filter_btns[i], "amiga-insp-tab");
    if (i > 0) {
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->filter_btns[i]),
                                  GTK_TOGGLE_BUTTON(self->filter_btns[0]));
    }
    g_signal_connect(self->filter_btns[i], "toggled", G_CALLBACK(on_filter_toggled), self);
    gtk_box_append(GTK_BOX(fbar), self->filter_btns[i]);
  }
  self->lbl_summary = gtk_label_new("No Copperlist decoded.");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_summary), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_summary), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(self->lbl_summary, TRUE);
  gtk_box_append(GTK_BOX(fbar), self->lbl_summary);
  gtk_box_append(GTK_BOX(self), fbar);

  self->scan_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_visible(self->scan_bar, FALSE);
  gtk_box_append(GTK_BOX(self), self->scan_bar);

  GtkWidget *scr = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scr, TRUE);
  gtk_widget_set_hexpand(scr, TRUE);
  self->listbox = gtk_list_box_new();
  gtk_widget_add_css_class(self->listbox, "amiga-disasm-list");
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(self->listbox), FALSE);
  g_signal_connect(self->listbox, "row-activated", G_CALLBACK(on_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), self->listbox);
  gtk_box_append(GTK_BOX(self), scr);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->filter_btns[0]), TRUE);
}

AmigaCopperView *
amiga_copper_view_new(void)
{
  return g_object_new(AMIGA_TYPE_COPPER_VIEW, NULL);
}

void
amiga_copper_view_set_callbacks(AmigaCopperView            *self,
                                const AmigaCopperCallbacks *cbs,
                                gpointer                    ud)
{
  g_return_if_fail(AMIGA_IS_COPPER_VIEW(self));
  if (cbs) self->cbs = *cbs;
  else memset(&self->cbs, 0, sizeof(self->cbs));
  self->user_data = ud;
}

void
amiga_copper_view_set_snapshot(AmigaCopperView           *self,
                               const AmigaCustomSnapshot *snap)
{
  g_return_if_fail(AMIGA_IS_COPPER_VIEW(self));
  if (!snap) return;
  self->last_snap = *snap;
  self->has_snap = true;
  guint src = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_source));
  if (src == 0 || src == 1) {
    uint32_t addr = (src == 1) ? snap->cop2lc : snap->cop1lc;
    g_autofree char *s = g_strdup_printf("$%06X", addr);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_addr), s);
  }
  if (self->n_insns > 0) rebuild_rows(self);
}

void
amiga_copper_view_set_bytes(AmigaCopperView *self,
                            uint32_t         base_addr,
                            const uint8_t   *data,
                            size_t           len,
                            const char      *provenance_label)
{
  g_return_if_fail(AMIGA_IS_COPPER_VIEW(self));
  self->base_addr = base_addr;
  self->n_insns = amiga_copper_disasm(data, len, base_addr, G_N_ELEMENTS(self->insns), self->insns);
  if (provenance_label) {
    gtk_label_set_text(GTK_LABEL(self->prov_badge), provenance_label);
  }
  rebuild_rows(self);
}

void
amiga_copper_view_set_scan_hits(AmigaCopperView *self,
                                const uint32_t  *addrs,
                                size_t           count)
{
  g_return_if_fail(AMIGA_IS_COPPER_VIEW(self));
  GtkWidget *old;
  while ((old = gtk_widget_get_first_child(self->scan_bar)) != NULL) {
    gtk_box_remove(GTK_BOX(self->scan_bar), old);
  }
  if (!addrs || count == 0) {
    gtk_widget_set_visible(self->scan_bar, FALSE);
    return;
  }
  GtkWidget *hdr = gtk_label_new("SCAN HITS:");
  gtk_widget_add_css_class(hdr, "amiga-badge-watched");
  gtk_box_append(GTK_BOX(self->scan_bar), hdr);
  size_t lim = MIN(count, 6u);
  for (size_t i = 0; i < lim; i++) {
    g_autofree char *s = g_strdup_printf("$%06X", addrs[i]);
    GtkWidget *b = gtk_button_new_with_label(s);
    gtk_widget_add_css_class(b, "amiga-insp-tab");
    g_object_set_data(G_OBJECT(b), "hit-addr", GUINT_TO_POINTER(addrs[i]));
    g_signal_connect(b, "clicked", G_CALLBACK(on_scan_hit_clicked), self);
    gtk_box_append(GTK_BOX(self->scan_bar), b);
  }
  gtk_widget_set_visible(self->scan_bar, TRUE);
}
