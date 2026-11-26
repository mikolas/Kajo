#include "amiga_disasm_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  bool           is_header;
  uint32_t       addr;
  bool           has_target_addr;
  uint32_t       target_addr;
  char          *full_insn;
} DisasmRowMeta;

struct _AmigaDisasmView {
  GtkBox parent_instance;

  GtkWidget            *entry_goto;
  GtkWidget            *btn_back;
  GtkWidget            *btn_fwd;
  GtkWidget            *toggle_follow;
  GtkWidget            *btn_run_cursor;
  GtkWidget            *btn_set_pc;
  GtkWidget            *btn_patch;
  GtkWidget            *toggle_trace;
  GtkWidget            *listbox;

  GArray               *nav_history; /* uint32_t */
  int                   nav_idx;
  uint32_t              base_addr;
  uint32_t              current_pc;
  GPtrArray            *meta_rows; /* DisasmRowMeta* matching listbox rows */

  AmigaDisasmCallbacks  cbs;
  gpointer              user_data;
};

G_DEFINE_FINAL_TYPE(AmigaDisasmView, amiga_disasm_view, GTK_TYPE_BOX)

static void
disasm_row_meta_free(DisasmRowMeta *m)
{
  if (!m) return;
  g_free(m->full_insn);
  g_free(m);
}

static void
amiga_disasm_view_dispose(GObject *obj)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(obj);
  g_clear_pointer(&self->meta_rows, g_ptr_array_unref);
  g_clear_pointer(&self->nav_history, g_array_unref);
  G_OBJECT_CLASS(amiga_disasm_view_parent_class)->dispose(obj);
}

static void
amiga_disasm_view_class_init(AmigaDisasmViewClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = amiga_disasm_view_dispose;
}

static uint32_t
resolve_jump_expr(AmigaDisasmView *self, const char *expr, bool *ok)
{
  if (ok) *ok = false;
  if (!expr || !expr[0]) return 0;
  if (self->cbs.resolve_expr) {
    bool res_ok = false;
    uint32_t r = self->cbs.resolve_expr(self, expr, &res_ok, self->user_data);
    if (res_ok) {
      if (ok) *ok = true;
      return r;
    }
  }
  const char *p = expr;
  if (p[0] == '$') p++;
  else if (g_ascii_strncasecmp(p, "0x", 2) == 0) p += 2;
  char *end = NULL;
  unsigned long v = strtoul(p, &end, 16);
  if (end && end > p) {
    if (ok) *ok = true;
    return (uint32_t)v;
  }
  return 0;
}

static void
on_goto_activate(GtkEntry *e, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  bool ok = false;
  uint32_t addr = resolve_jump_expr(self, gtk_editable_get_text(GTK_EDITABLE(e)), &ok);
  if (ok) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toggle_follow), FALSE);
    amiga_disasm_view_go_to(self, addr, true);
  }
}

static void
on_back_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  amiga_disasm_view_nav_back(AMIGA_DISASM_VIEW(ud));
}

static void
on_fwd_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  amiga_disasm_view_nav_fwd(AMIGA_DISASM_VIEW(ud));
}

static void
on_bp_dot_clicked(GtkButton *b, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  uint32_t addr = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "row-addr"));
  if (self->cbs.toggle_bp) self->cbs.toggle_bp(self, addr, self->user_data);
}

static uint32_t
selected_first_addr(AmigaDisasmView *self, bool *ok)
{
  if (ok) *ok = false;
  GList *sel = gtk_list_box_get_selected_rows(GTK_LIST_BOX(self->listbox));
  if (!sel) return 0;
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(sel->data);
  int idx = gtk_list_box_row_get_index(row);
  g_list_free(sel);
  if (idx < 0 || !self->meta_rows || (guint)idx >= self->meta_rows->len) return 0;
  DisasmRowMeta *m = g_ptr_array_index(self->meta_rows, (guint)idx);
  if (m->is_header) return 0;
  if (ok) *ok = true;
  return m->addr;
}

static void
on_run_cursor_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  bool ok = false;
  uint32_t addr = selected_first_addr(self, &ok);
  if (ok && self->cbs.run_to_cursor) {
    self->cbs.run_to_cursor(self, addr, self->user_data);
  }
}

static void
on_set_pc_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  bool ok = false;
  uint32_t addr = selected_first_addr(self, &ok);
  if (ok && self->cbs.set_pc) {
    self->cbs.set_pc(self, addr, self->user_data);
  }
}

typedef struct {
  AmigaDisasmView *view;
  uint32_t         addr;
  GtkWidget       *popover;
  GtkWidget       *entry;
} PatchCtx;

static void
on_patch_apply(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  PatchCtx *pc = ud;
  const char *txt = gtk_editable_get_text(GTK_EDITABLE(pc->entry));
  if (txt && txt[0] == '$') txt++;
  uint16_t word = (uint16_t)strtoul(txt ? txt : "4e71", NULL, 16);
  if (pc->view->cbs.patch_word) {
    pc->view->cbs.patch_word(pc->view, pc->addr, word, pc->view->user_data);
  }
  gtk_popover_popdown(GTK_POPOVER(pc->popover));
}

static void
patch_ctx_destroy(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  g_free(data);
}

static void
on_patch_clicked(GtkButton *b, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  bool ok = false;
  uint32_t addr = selected_first_addr(self, &ok);
  if (!ok) return;

  GtkWidget *pop = gtk_popover_new();
  gtk_widget_set_parent(pop, GTK_WIDGET(b));
  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(hb, 8);
  gtk_widget_set_margin_bottom(hb, 8);
  gtk_widget_set_margin_start(hb, 8);
  gtk_widget_set_margin_end(hb, 8);

  g_autofree char *lbl = g_strdup_printf("Patch word @$%08X:", addr);
  GtkWidget *l = gtk_label_new(lbl);
  GtkWidget *entry = gtk_entry_new();
  gtk_widget_add_css_class(entry, "amiga-entry");
  gtk_editable_set_text(GTK_EDITABLE(entry), "4E71");
  GtkWidget *apply = gtk_button_new_with_label("WRITE WORD (M)");
  gtk_widget_add_css_class(apply, "ide-toolbar-btn");
  gtk_widget_add_css_class(apply, "accent-btn");

  gtk_box_append(GTK_BOX(hb), l);
  gtk_box_append(GTK_BOX(hb), entry);
  gtk_box_append(GTK_BOX(hb), apply);

  PatchCtx *pc = g_new0(PatchCtx, 1);
  pc->view = self;
  pc->addr = addr;
  pc->popover = pop;
  pc->entry = entry;

  g_signal_connect_data(apply, "clicked", G_CALLBACK(on_patch_apply),
                        pc, patch_ctx_destroy, 0);
  g_signal_connect(entry, "activate", G_CALLBACK(on_patch_apply), pc);
  g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);

  gtk_popover_set_child(GTK_POPOVER(pop), hb);
  gtk_popover_popup(GTK_POPOVER(pop));
}

static void
on_trace_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  if (self->cbs.toggle_history) {
    self->cbs.toggle_history(self, gtk_toggle_button_get_active(tb), self->user_data);
  }
}

static void
on_follow_pc_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  if (gtk_toggle_button_get_active(tb) && self->current_pc != 0) {
    amiga_disasm_view_go_to(self, self->current_pc, false);
  }
}

static void
on_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  int idx = gtk_list_box_row_get_index(row);
  if (idx < 0 || !self->meta_rows || (guint)idx >= self->meta_rows->len) return;
  DisasmRowMeta *m = g_ptr_array_index(self->meta_rows, (guint)idx);
  if (!m->is_header && m->has_target_addr) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toggle_follow), FALSE);
    amiga_disasm_view_go_to(self, m->target_addr, true);
  }
}

static void
on_selected_rows_changed(GtkListBox *lb, gpointer ud)
{
  AmigaDisasmView *self = AMIGA_DISASM_VIEW(ud);
  if (!self->cbs.selection_cycles) return;
  GList *sel = gtk_list_box_get_selected_rows(lb);
  GPtrArray *insns = g_ptr_array_new();
  for (GList *it = sel; it != NULL; it = it->next) {
    int idx = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(it->data));
    if (idx >= 0 && self->meta_rows && (guint)idx < self->meta_rows->len) {
      DisasmRowMeta *m = g_ptr_array_index(self->meta_rows, (guint)idx);
      if (!m->is_header && m->full_insn) {
        g_ptr_array_add(insns, m->full_insn);
      }
    }
  }
  g_list_free(sel);

  if (insns->len >= 2) {
    AmigaCycleSum sum = amiga_m68k_sum_cycles(
        (const char *const *)insns->pdata, insns->len);
    g_autofree char *txt = (sum.min_cycles == sum.max_cycles)
                               ? g_strdup_printf("SEL: %u insn · %u(%u/%u) cyc%s",
                                                 sum.instr_count, sum.min_cycles,
                                                 sum.reads, sum.writes,
                                                 sum.has_variable_shift ? " (+2n)" : "")
                               : g_strdup_printf("SEL: %u insn · %u-%u(%u/%u) cyc",
                                                 sum.instr_count, sum.min_cycles, sum.max_cycles,
                                                 sum.reads, sum.writes);
    self->cbs.selection_cycles(self, txt, self->user_data);
  } else {
    self->cbs.selection_cycles(self, NULL, self->user_data);
  }
  g_ptr_array_unref(insns);
}

static void
amiga_disasm_view_init(AmigaDisasmView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  self->nav_history = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  self->nav_idx = -1;
  self->meta_rows = g_ptr_array_new_with_free_func((GDestroyNotify)disasm_row_meta_free);

  GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(tb, "amiga-disasm-toolbar");
  gtk_widget_set_valign(tb, GTK_ALIGN_CENTER);

  self->entry_goto = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_goto, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_goto), "$Addr / Symbol / PC (Ctrl+G)");
  gtk_widget_set_hexpand(self->entry_goto, TRUE);
  gtk_widget_set_valign(self->entry_goto, GTK_ALIGN_CENTER);
  g_signal_connect(self->entry_goto, "activate", G_CALLBACK(on_goto_activate), self);

  self->btn_back = gtk_button_new_with_label("←");
  gtk_widget_add_css_class(self->btn_back, "ide-toolbar-btn");
  gtk_widget_set_sensitive(self->btn_back, FALSE);
  g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back_clicked), self);

  self->btn_fwd = gtk_button_new_with_label("→");
  gtk_widget_add_css_class(self->btn_fwd, "ide-toolbar-btn");
  gtk_widget_set_sensitive(self->btn_fwd, FALSE);
  g_signal_connect(self->btn_fwd, "clicked", G_CALLBACK(on_fwd_clicked), self);

  self->toggle_follow = gtk_toggle_button_new_with_label("📌 FOLLOW PC");
  gtk_widget_add_css_class(self->toggle_follow, "amiga-seg-btn");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toggle_follow), TRUE);
  g_signal_connect(self->toggle_follow, "toggled", G_CALLBACK(on_follow_pc_toggled), self);

  self->btn_run_cursor = gtk_button_new_with_label("⏩ TO CURSOR");
  gtk_widget_add_css_class(self->btn_run_cursor, "ide-toolbar-btn");
  g_signal_connect(self->btn_run_cursor, "clicked", G_CALLBACK(on_run_cursor_clicked), self);

  self->btn_set_pc = gtk_button_new_with_label("🎯 SET PC");
  gtk_widget_add_css_class(self->btn_set_pc, "ide-toolbar-btn");
  g_signal_connect(self->btn_set_pc, "clicked", G_CALLBACK(on_set_pc_clicked), self);

  self->btn_patch = gtk_button_new_with_label("✏ PATCH");
  gtk_widget_add_css_class(self->btn_patch, "ide-toolbar-btn");
  g_signal_connect(self->btn_patch, "clicked", G_CALLBACK(on_patch_clicked), self);

  self->toggle_trace = gtk_toggle_button_new_with_label("📜 TRACE");
  gtk_widget_add_css_class(self->toggle_trace, "amiga-seg-btn");
  g_signal_connect(self->toggle_trace, "toggled", G_CALLBACK(on_trace_toggled), self);

  gtk_box_append(GTK_BOX(tb), self->btn_back);
  gtk_box_append(GTK_BOX(tb), self->btn_fwd);
  gtk_box_append(GTK_BOX(tb), self->entry_goto);
  gtk_box_append(GTK_BOX(tb), self->toggle_follow);
  gtk_box_append(GTK_BOX(tb), self->btn_run_cursor);
  gtk_box_append(GTK_BOX(tb), self->btn_set_pc);
  gtk_box_append(GTK_BOX(tb), self->btn_patch);
  gtk_box_append(GTK_BOX(tb), self->toggle_trace);
  gtk_box_append(GTK_BOX(self), tb);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  self->listbox = gtk_list_box_new();
  gtk_widget_add_css_class(self->listbox, "amiga-disasm-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->listbox), GTK_SELECTION_MULTIPLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(self->listbox), FALSE);
  g_signal_connect(self->listbox, "row-activated", G_CALLBACK(on_row_activated), self);
  g_signal_connect(self->listbox, "selected-rows-changed",
                   G_CALLBACK(on_selected_rows_changed), self);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->listbox);
  gtk_box_append(GTK_BOX(self), scroll);
}

AmigaDisasmView *
amiga_disasm_view_new(void)
{
  return g_object_new(AMIGA_TYPE_DISASM_VIEW, NULL);
}

void
amiga_disasm_view_set_callbacks(AmigaDisasmView            *self,
                                const AmigaDisasmCallbacks *cbs,
                                gpointer                    user_data)
{
  g_return_if_fail(AMIGA_IS_DISASM_VIEW(self));
  if (cbs) self->cbs = *cbs;
  else memset(&self->cbs, 0, sizeof(self->cbs));
  self->user_data = user_data;
}

void
amiga_disasm_view_set_rows(AmigaDisasmView *self,
                           GPtrArray       *rows,
                           uint32_t         current_pc,
                           GHashTable      *bp_set)
{
  g_return_if_fail(AMIGA_IS_DISASM_VIEW(self));
  self->current_pc = current_pc;
  g_ptr_array_set_size(self->meta_rows, 0);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->listbox)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->listbox), child);
  }

  if (!rows) return;

  for (guint i = 0; i < rows->len; i++) {
    const AmigaDisasmRow *r = g_ptr_array_index(rows, i);
    DisasmRowMeta *m = g_new0(DisasmRowMeta, 1);
    m->is_header = r->is_header;
    m->addr = r->addr;
    m->has_target_addr = r->has_target_addr;
    m->target_addr = r->target_addr;
    if (!r->is_header && r->mnemonic) {
      m->full_insn = (r->operands && r->operands[0])
                         ? g_strdup_printf("%s %s", r->mnemonic, r->operands)
                         : g_strdup(r->mnemonic);
    }
    g_ptr_array_add(self->meta_rows, m);

    if (r->is_header) {
      GtkWidget *hl = gtk_label_new(r->header_text ? r->header_text : "");
      gtk_label_set_xalign(GTK_LABEL(hl), 0.0f);
      gtk_widget_add_css_class(hl, "amiga-disasm-header");
      gtk_list_box_append(GTK_LIST_BOX(self->listbox), hl);
      continue;
    }

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);

    bool has_bp = bp_set && g_hash_table_contains(bp_set, GUINT_TO_POINTER(r->addr));
    GtkWidget *bp_btn = gtk_button_new_with_label(
        (r->addr == current_pc) ? "▶" : (has_bp ? "●" : "·"));
    gtk_widget_set_focusable(bp_btn, FALSE);
    gtk_widget_add_css_class(bp_btn, "amiga-disasm-bp-dot");
    if (has_bp) gtk_widget_add_css_class(bp_btn, "bp-set");
    gtk_widget_set_valign(bp_btn, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(bp_btn), "row-addr", GUINT_TO_POINTER(r->addr));
    g_signal_connect(bp_btn, "clicked", G_CALLBACK(on_bp_dot_clicked), self);

    g_autofree char *addr_s = g_strdup_printf("$%08X", r->addr);
    GtkWidget *addr_lbl = gtk_label_new(addr_s);
    gtk_label_set_xalign(GTK_LABEL(addr_lbl), 0.0f);
    gtk_widget_add_css_class(addr_lbl, "amiga-disasm-addr");
    gtk_widget_set_valign(addr_lbl, GTK_ALIGN_CENTER);

    GtkWidget *op_lbl = gtk_label_new(r->opcode_hex ? r->opcode_hex : "");
    gtk_label_set_xalign(GTK_LABEL(op_lbl), 0.0f);
    gtk_widget_add_css_class(op_lbl, "amiga-disasm-opcodes");
    gtk_widget_set_valign(op_lbl, GTK_ALIGN_CENTER);

    g_autofree char *insn_s = g_strdup_printf("%-8s %s",
                                              r->mnemonic ? r->mnemonic : "",
                                              r->operands ? r->operands : "");
    GtkWidget *insn_lbl = gtk_label_new(insn_s);
    gtk_label_set_xalign(GTK_LABEL(insn_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(insn_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(insn_lbl, "amiga-disasm-insn");
    gtk_widget_set_hexpand(insn_lbl, TRUE);
    gtk_widget_set_valign(insn_lbl, GTK_ALIGN_CENTER);

    GtkWidget *cyc_lbl = gtk_label_new(r->cycles.has_cycles ? r->cycles.display : "");
    gtk_label_set_xalign(GTK_LABEL(cyc_lbl), 1.0f);
    gtk_widget_add_css_class(cyc_lbl, "amiga-disasm-cycle-col");
    gtk_widget_set_valign(cyc_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hb), bp_btn);
    gtk_box_append(GTK_BOX(hb), addr_lbl);
    gtk_box_append(GTK_BOX(hb), op_lbl);
    gtk_box_append(GTK_BOX(hb), insn_lbl);
    gtk_box_append(GTK_BOX(hb), cyc_lbl);

    if (r->symbol_name) {
      GtkWidget *sb = gtk_label_new(r->symbol_name);
      gtk_widget_add_css_class(sb, "amiga-disasm-badge");
      gtk_widget_set_valign(sb, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(hb), sb);
    }
    if (r->hw_reg_name || r->lvo_name || r->comment || r->branch_taken >= 0) {
      g_autoptr(GString) note = g_string_new("");
      if (r->hw_reg_name) g_string_append_printf(note, "%s ", r->hw_reg_name);
      if (r->lvo_name) g_string_append_printf(note, "%s ", r->lvo_name);
      if (r->comment) g_string_append_printf(note, "%s ", r->comment);
      if (r->branch_taken == 1) g_string_append(note, "(T)");
      else if (r->branch_taken == 0) g_string_append(note, "(F)");
      GtkWidget *nl = gtk_label_new(note->str);
      gtk_widget_add_css_class(nl, "amiga-disasm-badge");
      gtk_widget_set_valign(nl, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(hb), nl);
    }

    gtk_list_box_append(GTK_LIST_BOX(self->listbox), hb);
    if (r->addr == current_pc) {
      GtkListBoxRow *lbr = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->listbox), (int)i);
      if (lbr) gtk_widget_add_css_class(GTK_WIDGET(lbr), "pc-row");
    }
  }
}

static void
sync_nav_buttons(AmigaDisasmView *self)
{
  gtk_widget_set_sensitive(self->btn_back, self->nav_idx > 0);
  gtk_widget_set_sensitive(
      self->btn_fwd, self->nav_idx >= 0 && (guint)(self->nav_idx + 1) < self->nav_history->len);
}

void
amiga_disasm_view_go_to(AmigaDisasmView *self, uint32_t addr, bool push_history)
{
  g_return_if_fail(AMIGA_IS_DISASM_VIEW(self));
  self->base_addr = addr;
  if (push_history) {
    if (self->nav_idx + 1 < (int)self->nav_history->len) {
      g_array_set_size(self->nav_history, (guint)(self->nav_idx + 1));
    }
    g_array_append_val(self->nav_history, addr);
    self->nav_idx = (int)self->nav_history->len - 1;
    sync_nav_buttons(self);
  }
  g_autofree char *s = g_strdup_printf("$%08X", addr);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_goto), s);
  if (self->cbs.fetch_disasm) {
    self->cbs.fetch_disasm(self, addr, 64, self->user_data);
  }
}

void
amiga_disasm_view_focus_goto(AmigaDisasmView *self)
{
  g_return_if_fail(AMIGA_IS_DISASM_VIEW(self));
  gtk_widget_grab_focus(self->entry_goto);
  gtk_editable_select_region(GTK_EDITABLE(self->entry_goto), 0, -1);
}

bool
amiga_disasm_view_nav_back(AmigaDisasmView *self)
{
  g_return_val_if_fail(AMIGA_IS_DISASM_VIEW(self), false);
  if (self->nav_idx <= 0) return false;
  self->nav_idx--;
  sync_nav_buttons(self);
  uint32_t addr = g_array_index(self->nav_history, uint32_t, (guint)self->nav_idx);
  amiga_disasm_view_go_to(self, addr, false);
  return true;
}

bool
amiga_disasm_view_nav_fwd(AmigaDisasmView *self)
{
  g_return_val_if_fail(AMIGA_IS_DISASM_VIEW(self), false);
  if (self->nav_idx < 0 || (guint)(self->nav_idx + 1) >= self->nav_history->len) return false;
  self->nav_idx++;
  sync_nav_buttons(self);
  uint32_t addr = g_array_index(self->nav_history, uint32_t, (guint)self->nav_idx);
  amiga_disasm_view_go_to(self, addr, false);
  return true;
}

bool
amiga_disasm_view_get_follow_pc(AmigaDisasmView *self)
{
  g_return_val_if_fail(AMIGA_IS_DISASM_VIEW(self), true);
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->toggle_follow));
}

uint32_t
amiga_disasm_view_get_base_addr(AmigaDisasmView *self)
{
  g_return_val_if_fail(AMIGA_IS_DISASM_VIEW(self), 0);
  return self->base_addr;
}
