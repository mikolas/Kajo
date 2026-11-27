#include "amiga_debug_sidebar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _AmigaDebugSidebar {
  GtkBox parent_instance;

  /* Section 1: CPU 68000 */
  GtkWidget *lbl_beam;
  GtkWidget *btn_pc;
  GtkWidget *btn_cp_save;
  GtkWidget *btn_cp_restore;
  GtkWidget *lbl_sr_hex;
  GtkWidget *sr_btns[8]; /* T, S, I, X, N, Z, V, C */
  GtkWidget *d_btns[8];
  GtkWidget *a_btns[8];

  /* Section 2: Stack (A7) & Guru Traps */
  GtkWidget *exc_pills[5]; /* BUS#2, ADR#3, ILG#4, DIV#5, PRV#8 */
  GtkWidget *lbl_exc_last;
  GtkWidget *stack_list;
  bool       updating_exc;

  /* Section 3: Watches */
  GtkWidget *entry_watch;
  GtkWidget *watches_list;

  /* Section 4: Unified Breakpoints & Watchpoints */
  GtkWidget *bp_list;

  /* Section 5: Relocated Hunks vs Attach Tasks & Memory Map */
  GtkWidget *sec_hunks;
  GtkWidget *hunks_list;
  GtkWidget *sec_tasks;
  GtkWidget *tasks_list;
  GtkWidget *sec_memmap;
  GtkWidget *memmap_list;

  AmigaCpuRegs               cur_regs;
  bool                       has_regs;
  bool                       paused;
  bool                       attach_mode;
  uint64_t                   prev_cycles;
  bool                       has_prev_cycles;
  uint8_t                    exc_mask;

  AmigaDebugSidebarCallbacks cbs;
  gpointer                   user_data;
};

G_DEFINE_FINAL_TYPE(AmigaDebugSidebar, amiga_debug_sidebar, GTK_TYPE_BOX)

static const uint16_t k_sr_masks[8] = {
  0x8000u, 0x2000u, 0x0700u, 0x0010u, 0x0008u, 0x0004u, 0x0002u, 0x0001u
};
static const char *const k_sr_names[8] = { "T", "S", "I", "X", "N", "Z", "V", "C" };
static const uint8_t k_exc_bits[5] = { 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 7 };
static const char *const k_exc_labels[5] = { "BUS#2", "ADR#3", "ILG#4", "DIV#5", "PRV#8" };

static void
clear_listbox(GtkWidget *lb)
{
  if (!lb) return;
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(lb)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(lb), ch);
  }
}

typedef struct {
  AmigaDebugSidebar *sb;
  int                reg_idx; /* 0..7=D, 8..15=A, 17=PC */
  GtkWidget         *popover;
  GtkWidget         *entry;
} RegPopCtx;

static void
reg_pop_ctx_free(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  g_free(data);
}

static uint32_t
read_reg_idx(const AmigaCpuRegs *r, int idx)
{
  if (idx < 8) return r->d[idx];
  if (idx < 16) return r->a[idx - 8];
  return r->pc;
}

static void
on_reg_apply(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  RegPopCtx *ctx = ud;
  AmigaDebugSidebar *self = ctx->sb;
  if (self->has_regs && self->paused && self->cbs.write_regs) {
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
    if (txt && txt[0] == '$') txt++;
    uint32_t val = (uint32_t)strtoul(txt ? txt : "0", NULL, 16);
    AmigaCpuRegs next = self->cur_regs;
    if (ctx->reg_idx < 8) next.d[ctx->reg_idx] = val;
    else if (ctx->reg_idx < 16) next.a[ctx->reg_idx - 8] = val;
    else next.pc = val;
    self->cbs.write_regs(self, &next, self->user_data);
  }
  gtk_popover_popdown(GTK_POPOVER(ctx->popover));
}

static void
on_reg_jump_mem(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  RegPopCtx *ctx = ud;
  if (ctx->sb->has_regs && ctx->sb->cbs.jump_addr) {
    ctx->sb->cbs.jump_addr(ctx->sb, read_reg_idx(&ctx->sb->cur_regs, ctx->reg_idx), true, ctx->sb->user_data);
  }
  gtk_popover_popdown(GTK_POPOVER(ctx->popover));
}

static void
on_reg_jump_dis(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  RegPopCtx *ctx = ud;
  if (ctx->sb->has_regs && ctx->sb->cbs.jump_addr) {
    ctx->sb->cbs.jump_addr(ctx->sb, read_reg_idx(&ctx->sb->cur_regs, ctx->reg_idx), false, ctx->sb->user_data);
  }
  gtk_popover_popdown(GTK_POPOVER(ctx->popover));
}

static void
on_reg_clicked(GtkButton *btn, gpointer ud)
{
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  if (!self->has_regs) return;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "reg-idx"));
  uint32_t val = read_reg_idx(&self->cur_regs, idx);

  GtkWidget *pop = gtk_popover_new();
  gtk_widget_set_parent(pop, GTK_WIDGET(btn));

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(vb, 8);
  gtk_widget_set_margin_bottom(vb, 8);
  gtk_widget_set_margin_start(vb, 10);
  gtk_widget_set_margin_end(vb, 10);

  g_autofree char *rname = (idx < 8) ? g_strdup_printf("D%d", idx)
                         : (idx < 16) ? g_strdup_printf("A%d", idx - 8)
                                      : g_strdup("PC");
  g_autofree char *meta = g_strdup_printf("%s = $%08X (%d / %u)", rname, val, (int32_t)val, val);
  GtkWidget *ml = gtk_label_new(meta);
  gtk_label_set_xalign(GTK_LABEL(ml), 0.0f);
  gtk_box_append(GTK_BOX(vb), ml);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_add_css_class(entry, "amiga-entry");
  g_autofree char *hx = g_strdup_printf("$%08X", val);
  gtk_editable_set_text(GTK_EDITABLE(entry), hx);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *b_set = gtk_button_new_with_label("SET (G)");
  gtk_widget_add_css_class(b_set, "ide-toolbar-btn");
  gtk_widget_add_css_class(b_set, "accent-btn");
  gtk_widget_set_sensitive(b_set, self->paused && self->cbs.write_regs != NULL);
  GtkWidget *b_hex = gtk_button_new_with_label("HEX");
  gtk_widget_add_css_class(b_hex, "ide-toolbar-btn");
  GtkWidget *b_dis = gtk_button_new_with_label("DISASM");
  gtk_widget_add_css_class(b_dis, "ide-toolbar-btn");

  gtk_box_append(GTK_BOX(row), entry);
  gtk_box_append(GTK_BOX(row), b_set);
  gtk_box_append(GTK_BOX(row), b_hex);
  gtk_box_append(GTK_BOX(row), b_dis);
  gtk_box_append(GTK_BOX(vb), row);

  RegPopCtx *ctx = g_new0(RegPopCtx, 1);
  ctx->sb = self;
  ctx->reg_idx = idx;
  ctx->popover = pop;
  ctx->entry = entry;

  g_signal_connect_data(b_set, "clicked", G_CALLBACK(on_reg_apply), ctx, reg_pop_ctx_free, 0);
  g_signal_connect(entry, "activate", G_CALLBACK(on_reg_apply), ctx);
  g_signal_connect(b_hex, "clicked", G_CALLBACK(on_reg_jump_mem), ctx);
  g_signal_connect(b_dis, "clicked", G_CALLBACK(on_reg_jump_dis), ctx);
  g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);

  gtk_popover_set_child(GTK_POPOVER(pop), vb);
  gtk_popover_popup(GTK_POPOVER(pop));
}

static void
on_sr_clicked(GtkButton *b, gpointer ud)
{
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  if (!self->has_regs || !self->paused || !self->cbs.write_regs) return;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "sr-idx"));
  AmigaCpuRegs next = self->cur_regs;
  if (idx == 2) {
    uint32_t ipl = (next.sr >> 8) & 0x7u;
    next.sr = (next.sr & ~0x0700u) | ((ipl == 0) ? 0x0700u : 0x0000u);
  } else {
    next.sr ^= k_sr_masks[idx];
  }
  self->cbs.write_regs(self, &next, self->user_data);
}

static void on_cp_save(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  if (self->cbs.checkpoint) self->cbs.checkpoint(self, false, self->user_data);
}
static void on_cp_restore(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  if (self->cbs.checkpoint) self->cbs.checkpoint(self, true, self->user_data);
}

static void
on_exc_pill_toggled(GtkToggleButton *tb G_GNUC_UNUSED, gpointer ud)
{
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  if (self->updating_exc) return;
  uint8_t mask = 0;
  for (int i = 0; i < 5; i++) {
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->exc_pills[i]))) {
      mask |= k_exc_bits[i];
    }
  }
  self->exc_mask = mask;
  if (self->cbs.exc_mask_changed) self->cbs.exc_mask_changed(self, mask, self->user_data);
}

static void
on_watch_add(GtkWidget *w G_GNUC_UNUSED, gpointer ud)
{
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  const char *txt = gtk_editable_get_text(GTK_EDITABLE(self->entry_watch));
  if (!txt || !txt[0]) return;
  if (self->cbs.add_watch) self->cbs.add_watch(self, txt, self->user_data);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_watch), "");
}

static void
on_addr_list_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  if (!self->cbs.jump_addr) return;
  uint32_t addr = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "amiga-addr"));
  bool to_mem = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "amiga-to-mem")) != 0;
  if (addr != 0) self->cbs.jump_addr(self, addr, to_mem, self->user_data);
}

static void
on_sec_hdr_toggled(GtkToggleButton *tb, gpointer ud)
{
  GtkWidget *body = GTK_WIDGET(ud);
  gtk_widget_set_visible(body, gtk_toggle_button_get_active(tb));
}

static GtkWidget *
make_dbg_section(GtkWidget   *parent_vbox,
                 const char  *title,
                 GtkWidget   *right_extra,
                 bool         expanded,
                 GtkWidget  **out_sec_root)
{
  GtkWidget *sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sec, "amiga-dbg-sec");

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(hdr, "amiga-dbg-sec-hdr");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *tog = gtk_toggle_button_new_with_label(title);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tog), expanded);
  gtk_widget_add_css_class(tog, "amiga-dbg-sec-toggle");
  gtk_widget_set_hexpand(tog, TRUE);
  gtk_widget_set_valign(tog, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(hdr), tog);
  if (right_extra) gtk_box_append(GTK_BOX(hdr), right_extra);

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(body, "amiga-dbg-sec-body");
  gtk_widget_set_visible(body, expanded);
  g_signal_connect(tog, "toggled", G_CALLBACK(on_sec_hdr_toggled), body);

  gtk_box_append(GTK_BOX(sec), hdr);
  gtk_box_append(GTK_BOX(sec), body);
  gtk_box_append(GTK_BOX(parent_vbox), sec);
  if (out_sec_root) *out_sec_root = sec;
  return body;
}

static GtkWidget *
make_reg_cell(AmigaDebugSidebar *self, const char *txt, int idx)
{
  GtkWidget *b = gtk_button_new_with_label(txt);
  gtk_widget_set_focusable(b, FALSE);
  gtk_widget_add_css_class(b, "amiga-reg-cell");
  gtk_widget_set_hexpand(b, TRUE);
  g_object_set_data(G_OBJECT(b), "reg-idx", GINT_TO_POINTER(idx));
  g_signal_connect(b, "clicked", G_CALLBACK(on_reg_clicked), self);
  return b;
}

static void
amiga_debug_sidebar_class_init(AmigaDebugSidebarClass *klass G_GNUC_UNUSED)
{
}

static void
build_cpu_section(AmigaDebugSidebar *self, GtkWidget *root)
{
  self->lbl_beam = gtk_label_new("STOPPED");
  gtk_widget_add_css_class(self->lbl_beam, "amiga-beam-meta");
  gtk_widget_set_valign(self->lbl_beam, GTK_ALIGN_CENTER);
  GtkWidget *body = make_dbg_section(root, "▾ CPU 68000", self->lbl_beam, true, NULL);

  GtkWidget *pc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(pc_row, GTK_ALIGN_CENTER);
  self->btn_pc = make_reg_cell(self, "PC --------", 17);
  self->btn_cp_save = gtk_button_new_with_label("💾CP");
  gtk_widget_add_css_class(self->btn_cp_save, "amiga-mini-btn");
  gtk_widget_set_tooltip_text(self->btn_cp_save, "Save savestate checkpoint");
  g_signal_connect(self->btn_cp_save, "clicked", G_CALLBACK(on_cp_save), self);
  self->btn_cp_restore = gtk_button_new_with_label("⏮CP");
  gtk_widget_add_css_class(self->btn_cp_restore, "amiga-mini-btn");
  gtk_widget_set_tooltip_text(self->btn_cp_restore, "Restore savestate checkpoint");
  g_signal_connect(self->btn_cp_restore, "clicked", G_CALLBACK(on_cp_restore), self);
  gtk_box_append(GTK_BOX(pc_row), self->btn_pc);
  gtk_box_append(GTK_BOX(pc_row), self->btn_cp_save);
  gtk_box_append(GTK_BOX(pc_row), self->btn_cp_restore);
  gtk_box_append(GTK_BOX(body), pc_row);

  GtkWidget *sr_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_valign(sr_row, GTK_ALIGN_CENTER);
  self->lbl_sr_hex = gtk_label_new("SR ----");
  gtk_widget_add_css_class(self->lbl_sr_hex, "amiga-sr-hex");
  gtk_box_append(GTK_BOX(sr_row), self->lbl_sr_hex);
  for (int i = 0; i < 8; i++) {
    self->sr_btns[i] = gtk_button_new_with_label(k_sr_names[i]);
    gtk_widget_set_focusable(self->sr_btns[i], FALSE);
    gtk_widget_add_css_class(self->sr_btns[i], "amiga-sr-pill");
    gtk_widget_set_hexpand(self->sr_btns[i], TRUE);
    g_object_set_data(G_OBJECT(self->sr_btns[i]), "sr-idx", GINT_TO_POINTER(i));
    g_signal_connect(self->sr_btns[i], "clicked", G_CALLBACK(on_sr_clicked), self);
    gtk_box_append(GTK_BOX(sr_row), self->sr_btns[i]);
  }
  gtk_box_append(GTK_BOX(body), sr_row);

  GtkWidget *grid = gtk_grid_new();
  gtk_widget_add_css_class(grid, "amiga-reg-grid");
  gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  for (int i = 0; i < 8; i++) {
    g_autofree char *dl = g_strdup_printf("D%d --------", i);
    g_autofree char *al = g_strdup_printf("A%d --------", i);
    self->d_btns[i] = make_reg_cell(self, dl, i);
    self->a_btns[i] = make_reg_cell(self, al, 8 + i);
    gtk_grid_attach(GTK_GRID(grid), self->d_btns[i], 0, i, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), self->a_btns[i], 1, i, 1, 1);
  }
  gtk_box_append(GTK_BOX(body), grid);
}

static void
amiga_debug_sidebar_init(AmigaDebugSidebar *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);

  GtkWidget *scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scr, TRUE);
  gtk_widget_set_hexpand(scr, TRUE);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  build_cpu_section(self, vbox);

  /* Section 2: Stack (A7) & Guru Traps */
  GtkWidget *stk_body = make_dbg_section(vbox, "▾ STACK (A7) & TRAPS", NULL, true, NULL);
  GtkWidget *trap_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  for (int i = 0; i < 5; i++) {
    self->exc_pills[i] = gtk_toggle_button_new_with_label(k_exc_labels[i]);
    gtk_widget_add_css_class(self->exc_pills[i], "amiga-trap-pill");
    gtk_widget_set_hexpand(self->exc_pills[i], TRUE);
    g_signal_connect(self->exc_pills[i], "toggled", G_CALLBACK(on_exc_pill_toggled), self);
    gtk_box_append(GTK_BOX(trap_row), self->exc_pills[i]);
  }
  gtk_box_append(GTK_BOX(stk_body), trap_row);

  self->lbl_exc_last = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_exc_last, "amiga-diag-error");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_exc_last), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_exc_last), TRUE);
  gtk_widget_set_visible(self->lbl_exc_last, FALSE);
  gtk_box_append(GTK_BOX(stk_body), self->lbl_exc_last);

  self->stack_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->stack_list, "amiga-diag-list");
  g_signal_connect(self->stack_list, "row-activated", G_CALLBACK(on_addr_list_activated), self);
  gtk_box_append(GTK_BOX(stk_body), self->stack_list);

  /* Section 3: Watches */
  GtkWidget *wat_body = make_dbg_section(vbox, "▾ WATCHES", NULL, true, NULL);
  GtkWidget *wadd = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  self->entry_watch = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_watch, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_watch), "$addr / sym");
  gtk_widget_set_hexpand(self->entry_watch, TRUE);
  g_signal_connect(self->entry_watch, "activate", G_CALLBACK(on_watch_add), self);
  GtkWidget *bw = gtk_button_new_with_label("+ADD");
  gtk_widget_add_css_class(bw, "amiga-mini-btn");
  g_signal_connect(bw, "clicked", G_CALLBACK(on_watch_add), self);
  gtk_box_append(GTK_BOX(wadd), self->entry_watch);
  gtk_box_append(GTK_BOX(wadd), bw);
  gtk_box_append(GTK_BOX(wat_body), wadd);
  self->watches_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->watches_list, "amiga-diag-list");
  gtk_box_append(GTK_BOX(wat_body), self->watches_list);

  /* Section 4: Breakpoints & Watchpoints */
  GtkWidget *bp_body = make_dbg_section(vbox, "▾ BREAK & WATCHPOINTS", NULL, true, NULL);
  self->bp_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->bp_list, "amiga-diag-list");
  g_signal_connect(self->bp_list, "row-activated", G_CALLBACK(on_addr_list_activated), self);
  gtk_box_append(GTK_BOX(bp_body), self->bp_list);

  /* Section 5a: Relocated Hunks (Project) */
  GtkWidget *hnk_body = make_dbg_section(vbox, "▾ RELOCATED HUNKS", NULL, true, &self->sec_hunks);
  self->hunks_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->hunks_list, "amiga-diag-list");
  g_signal_connect(self->hunks_list, "row-activated", G_CALLBACK(on_addr_list_activated), self);
  gtk_box_append(GTK_BOX(hnk_body), self->hunks_list);

  /* Section 5b/5c: Tasks & Memory Map (Attach) */
  GtkWidget *tsk_body = make_dbg_section(vbox, "▾ AMIGADOS TASKS", NULL, true, &self->sec_tasks);
  self->tasks_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->tasks_list, "amiga-diag-list");
  g_signal_connect(self->tasks_list, "row-activated", G_CALLBACK(on_addr_list_activated), self);
  gtk_box_append(GTK_BOX(tsk_body), self->tasks_list);
  gtk_widget_set_visible(self->sec_tasks, FALSE);

  GtkWidget *map_body = make_dbg_section(vbox, "▾ MEMORY MAP", NULL, true, &self->sec_memmap);
  self->memmap_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->memmap_list, "amiga-diag-list");
  g_signal_connect(self->memmap_list, "row-activated", G_CALLBACK(on_addr_list_activated), self);
  gtk_box_append(GTK_BOX(map_body), self->memmap_list);
  gtk_widget_set_visible(self->sec_memmap, FALSE);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), vbox);
  gtk_box_append(GTK_BOX(self), scr);
}

AmigaDebugSidebar *
amiga_debug_sidebar_new(void)
{
  return g_object_new(AMIGA_TYPE_DEBUG_SIDEBAR, NULL);
}

void
amiga_debug_sidebar_set_callbacks(AmigaDebugSidebar                *self,
                                  const AmigaDebugSidebarCallbacks *cbs,
                                  gpointer                          user_data)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  if (cbs) self->cbs = *cbs;
  else memset(&self->cbs, 0, sizeof(self->cbs));
  self->user_data = user_data;
}

void
amiga_debug_sidebar_set_attach_mode(AmigaDebugSidebar *self, bool attach_mode)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  self->attach_mode = attach_mode;
  gtk_widget_set_visible(self->sec_hunks, !attach_mode);
  gtk_widget_set_visible(self->sec_tasks, attach_mode);
  gtk_widget_set_visible(self->sec_memmap, attach_mode);
}

static void
update_reg_cell(GtkWidget *btn, const char *label, bool changed)
{
  gtk_button_set_label(GTK_BUTTON(btn), label);
  if (changed) gtk_widget_add_css_class(btn, "changed");
  else gtk_widget_remove_css_class(btn, "changed");
}

void
amiga_debug_sidebar_update_cpu(AmigaDebugSidebar      *self,
                               const AmigaCpuRegs     *regs,
                               const AmigaExecStatus  *exec_st,
                               const AmigaSymbolTable *syms,
                               bool                    paused)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  self->paused = paused;
  if (!regs) return;

  for (int i = 0; i < 8; i++) {
    g_autofree char *dl = g_strdup_printf("D%d %08X", i, regs->d[i]);
    update_reg_cell(self->d_btns[i], dl, self->has_regs && regs->d[i] != self->cur_regs.d[i]);
    g_autofree char *al = g_strdup_printf("A%d %08X", i, regs->a[i]);
    update_reg_cell(self->a_btns[i], al, self->has_regs && regs->a[i] != self->cur_regs.a[i]);
  }

  const char *sname = NULL;
  uint32_t sdelta = 0;
  g_autofree char *pc_s = NULL;
  if (syms && amiga_symbols_resolve_addr(syms, regs->pc, &sname, &sdelta)) {
    pc_s = sdelta == 0 ? g_strdup_printf("PC $%06X %s", regs->pc, sname)
                       : g_strdup_printf("PC $%06X %s+$%x", regs->pc, sname, sdelta);
  } else {
    pc_s = g_strdup_printf("PC $%08X", regs->pc);
  }
  update_reg_cell(self->btn_pc, pc_s, self->has_regs && regs->pc != self->cur_regs.pc);

  g_autofree char *sr_s = g_strdup_printf("SR %04X", (unsigned)(regs->sr & 0xFFFFu));
  gtk_label_set_text(GTK_LABEL(self->lbl_sr_hex), sr_s);

  AmigaSrFlags f = amiga_decode_sr((uint16_t)(regs->sr & 0xFFFFu));
  bool active[8] = {
    f.trace, f.supervisor, f.ipl > 0, f.extend, f.negative, f.zero, f.overflow, f.carry
  };
  for (int i = 0; i < 8; i++) {
    if (i == 2) {
      g_autofree char *il = g_strdup_printf("I%u", f.ipl);
      gtk_button_set_label(GTK_BUTTON(self->sr_btns[i]), il);
    }
    if (active[i]) gtk_widget_add_css_class(self->sr_btns[i], "active");
    else gtk_widget_remove_css_class(self->sr_btns[i], "active");
  }

  if (exec_st) {
    uint64_t dc = (self->has_prev_cycles && exec_st->cycles >= self->prev_cycles)
                      ? (exec_st->cycles - self->prev_cycles) : 0;
    g_autofree char *b = g_strdup_printf("F%u V%u H%u +%llu",
                                         exec_st->frame, exec_st->vpos, exec_st->hpos,
                                         (unsigned long long)dc);
    gtk_label_set_text(GTK_LABEL(self->lbl_beam), b);
    self->prev_cycles = exec_st->cycles;
    self->has_prev_cycles = true;
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_beam), paused ? "STOPPED" : "RUNNING");
  }

  self->cur_regs = *regs;
  self->has_regs = true;
}

void
amiga_debug_sidebar_set_stack(AmigaDebugSidebar      *self,
                              uint32_t                sp,
                              const uint8_t          *bytes,
                              size_t                  len,
                              const AmigaSymbolTable *syms)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  clear_listbox(self->stack_list);
  if (!bytes || len < 4) return;
  for (size_t off = 0; off + 3 < len && off < 24; off += 4) {
    uint32_t val = ((uint32_t)bytes[off] << 24) | ((uint32_t)bytes[off + 1] << 16) |
                   ((uint32_t)bytes[off + 2] << 8) | (uint32_t)bytes[off + 3];
    const char *sname = NULL;
    uint32_t delta = 0;
    g_autofree char *row = NULL;
    if (syms && amiga_symbols_resolve_addr(syms, val, &sname, &delta)) {
      row = delta == 0 ? g_strdup_printf("+$%02zX $%08X %s", off, val, sname)
                       : g_strdup_printf("+$%02zX $%08X %s+$%x", off, val, sname, delta);
    } else {
      row = g_strdup_printf("+$%02zX $%08X", off, val);
    }
    GtkWidget *l = gtk_label_new(row);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_list_box_append(GTK_LIST_BOX(self->stack_list), l);
    GtkListBoxRow *lbr = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->stack_list), (int)(off / 4));
    if (lbr) {
      uint32_t target = (val != 0 && !(val & 1u)) ? val : (sp + (uint32_t)off);
      g_object_set_data(G_OBJECT(lbr), "amiga-addr", GUINT_TO_POINTER(target));
      g_object_set_data(G_OBJECT(lbr), "amiga-to-mem", GINT_TO_POINTER(val == 0 ? 1 : 0));
    }
  }
}

void
amiga_debug_sidebar_set_exc_mask(AmigaDebugSidebar *self, uint8_t mask)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  self->exc_mask = mask;
  self->updating_exc = true;
  for (int i = 0; i < 5; i++) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->exc_pills[i]), (mask & k_exc_bits[i]) != 0);
  }
  self->updating_exc = false;
}

void
amiga_debug_sidebar_set_exc_last(AmigaDebugSidebar *self, const char *text)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  if (text && text[0]) {
    gtk_label_set_text(GTK_LABEL(self->lbl_exc_last), text);
    gtk_widget_set_visible(self->lbl_exc_last, TRUE);
  } else {
    gtk_widget_set_visible(self->lbl_exc_last, FALSE);
  }
}

static void on_watch_mode_btn(GtkButton *b, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx"));
  if (self->cbs.cycle_watch_mode) self->cbs.cycle_watch_mode(self, idx, self->user_data);
}
static void on_watch_del_btn(GtkButton *b, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx"));
  if (self->cbs.remove_watch) self->cbs.remove_watch(self, idx, self->user_data);
}

void
amiga_debug_sidebar_set_watches(AmigaDebugSidebar *self, GPtrArray *watches)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  clear_listbox(self->watches_list);
  if (!watches || watches->len == 0) {
    GtkWidget *e = gtk_label_new("No watches (type symbol or $addr above)");
    gtk_label_set_xalign(GTK_LABEL(e), 0.0f);
    gtk_widget_add_css_class(e, "amiga-muted-hint");
    gtk_list_box_append(GTK_LIST_BOX(self->watches_list), e);
    return;
  }
  static const char *const m_lbl[3] = { ".w", ".l", "ptr" };
  for (guint i = 0; i < watches->len; i++) {
    const AmigaWatchItem *w = g_ptr_array_index(watches, i);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);

    GtkWidget *lbl_expr = gtk_label_new(w->expr ? w->expr : "?");
    gtk_label_set_xalign(GTK_LABEL(lbl_expr), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_expr), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl_expr, TRUE);

    GtkWidget *bm = gtk_button_new_with_label(m_lbl[w->mode % 3]);
    gtk_widget_add_css_class(bm, "amiga-mini-btn");
    g_object_set_data(G_OBJECT(bm), "idx", GUINT_TO_POINTER(i));
    g_signal_connect(bm, "clicked", G_CALLBACK(on_watch_mode_btn), self);

    g_autofree char *vs = !w->valid ? g_strdup("?")
                        : (w->mode == 0) ? g_strdup_printf("$%04X", w->val & 0xFFFFu)
                                         : g_strdup_printf("$%08X", w->val);
    GtkWidget *lv = gtk_label_new(vs);
    if (w->changed) gtk_widget_add_css_class(lv, "amiga-diag-warn");

    GtkWidget *bx = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(bx, "amiga-mini-btn");
    g_object_set_data(G_OBJECT(bx), "idx", GUINT_TO_POINTER(i));
    g_signal_connect(bx, "clicked", G_CALLBACK(on_watch_del_btn), self);

    gtk_box_append(GTK_BOX(hb), lbl_expr);
    gtk_box_append(GTK_BOX(hb), bm);
    gtk_box_append(GTK_BOX(hb), lv);
    gtk_box_append(GTK_BOX(hb), bx);
    gtk_list_box_append(GTK_LIST_BOX(self->watches_list), hb);
  }
}

static void on_src_bp_check(GtkCheckButton *cb, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(cb), "idx"));
  if (self->cbs.toggle_source_bp) {
    self->cbs.toggle_source_bp(self, idx, gtk_check_button_get_active(cb), self->user_data);
  }
}
static void on_src_bp_del(GtkButton *b, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx"));
  if (self->cbs.remove_source_bp) self->cbs.remove_source_bp(self, idx, self->user_data);
}
static void on_wp_check(GtkCheckButton *cb, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(cb), "idx"));
  if (self->cbs.toggle_watchpoint) {
    self->cbs.toggle_watchpoint(self, idx, gtk_check_button_get_active(cb), self->user_data);
  }
}
static void on_wp_del(GtkButton *b, gpointer ud) {
  AmigaDebugSidebar *self = AMIGA_DEBUG_SIDEBAR(ud);
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx"));
  if (self->cbs.remove_watchpoint) self->cbs.remove_watchpoint(self, idx, self->user_data);
}

void
amiga_debug_sidebar_set_breakpoints(AmigaDebugSidebar *self,
                                    GPtrArray         *source_bps,
                                    GHashTable        *bp_addrs,
                                    GPtrArray         *watchpoints)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  clear_listbox(self->bp_list);
  int count = 0;

  for (guint i = 0; source_bps && i < source_bps->len; i++) {
    const AmigaSourceBp *bp = g_ptr_array_index(source_bps, i);
    g_autofree char *bname = bp->file ? g_path_get_basename(bp->file) : g_strdup("?");
    g_autofree char *lbl = bp->addr
                               ? g_strdup_printf("%s:%u ($%06X)", bname, bp->line, bp->addr)
                               : g_strdup_printf("%s:%u (pending)", bname, bp->line);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
    GtkWidget *chk = gtk_check_button_new_with_label(lbl);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), !bp->disabled);
    gtk_widget_set_hexpand(chk, TRUE);
    g_object_set_data(G_OBJECT(chk), "idx", GUINT_TO_POINTER(i));
    g_signal_connect(chk, "toggled", G_CALLBACK(on_src_bp_check), self);

    GtkWidget *bx = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(bx, "amiga-mini-btn");
    g_object_set_data(G_OBJECT(bx), "idx", GUINT_TO_POINTER(i));
    g_signal_connect(bx, "clicked", G_CALLBACK(on_src_bp_del), self);

    gtk_box_append(GTK_BOX(hb), chk);
    gtk_box_append(GTK_BOX(hb), bx);
    gtk_list_box_append(GTK_LIST_BOX(self->bp_list), hb);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->bp_list), count++);
    if (r && bp->addr) g_object_set_data(G_OBJECT(r), "amiga-addr", GUINT_TO_POINTER(bp->addr));
  }

  if (bp_addrs) {
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, bp_addrs);
    while (g_hash_table_iter_next(&it, &k, NULL)) {
      uint32_t a = GPOINTER_TO_UINT(k);
      bool is_src = false;
      for (guint i = 0; source_bps && i < source_bps->len; i++) {
        const AmigaSourceBp *bp = g_ptr_array_index(source_bps, i);
        if (bp->addr == a) { is_src = true; break; }
      }
      if (is_src) continue;
      g_autofree char *lbl = g_strdup_printf("Z0 $%08X", a);
      GtkWidget *l = gtk_label_new(lbl);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
      gtk_list_box_append(GTK_LIST_BOX(self->bp_list), l);
      GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->bp_list), count++);
      if (r) g_object_set_data(G_OBJECT(r), "amiga-addr", GUINT_TO_POINTER(a));
    }
  }

  for (guint i = 0; watchpoints && i < watchpoints->len; i++) {
    const AmigaWatchpointItem *wp = g_ptr_array_index(watchpoints, i);
    const char *kstr = (wp->kind == 5) ? "DMA" : (wp->kind == 3) ? "Z3 R"
                     : (wp->kind == 4) ? "Z4 RW" : "Z2 W";
    g_autofree char *lbl = g_strdup_printf("%s $%06X (+%u)", kstr, wp->addr, wp->len);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *chk = gtk_check_button_new_with_label(lbl);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), wp->enabled);
    gtk_widget_set_hexpand(chk, TRUE);
    g_object_set_data(G_OBJECT(chk), "idx", GUINT_TO_POINTER(i));
    g_signal_connect(chk, "toggled", G_CALLBACK(on_wp_check), self);

    GtkWidget *bx = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(bx, "amiga-mini-btn");
    g_object_set_data(G_OBJECT(bx), "idx", GUINT_TO_POINTER(i));
    g_signal_connect(bx, "clicked", G_CALLBACK(on_wp_del), self);

    gtk_box_append(GTK_BOX(hb), chk);
    gtk_box_append(GTK_BOX(hb), bx);
    gtk_list_box_append(GTK_LIST_BOX(self->bp_list), hb);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->bp_list), count++);
    if (r) {
      g_object_set_data(G_OBJECT(r), "amiga-addr", GUINT_TO_POINTER(wp->addr));
      g_object_set_data(G_OBJECT(r), "amiga-to-mem", GINT_TO_POINTER(1));
    }
  }

  if (count == 0) {
    GtkWidget *e = gtk_label_new("No active breakpoints or watchpoints.");
    gtk_label_set_xalign(GTK_LABEL(e), 0.0f);
    gtk_widget_add_css_class(e, "amiga-muted-hint");
    gtk_list_box_append(GTK_LIST_BOX(self->bp_list), e);
  }
}

void
amiga_debug_sidebar_set_segments(AmigaDebugSidebar *self, const AmigaSegmentList *sl)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  clear_listbox(self->hunks_list);
  if (!sl || !sl->segments || sl->segments->len == 0) return;
  for (guint i = 0; i < sl->segments->len; i++) {
    const AmigaSegmentInfo *seg = g_ptr_array_index(sl->segments, i);
    g_autofree char *row = g_strdup_printf("HUNK %u  $%08X  (%u B)", seg->index, seg->address, seg->size);
    GtkWidget *l = gtk_label_new(row);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_list_box_append(GTK_LIST_BOX(self->hunks_list), l);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->hunks_list), (int)i);
    if (r) g_object_set_data(G_OBJECT(r), "amiga-addr", GUINT_TO_POINTER(seg->address));
  }
}

void
amiga_debug_sidebar_set_tasks(AmigaDebugSidebar *self, GPtrArray *tasks)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  clear_listbox(self->tasks_list);
  if (!tasks) return;
  for (guint i = 0; i < tasks->len; i++) {
    const AmigaTaskInfo *ti = g_ptr_array_index(tasks, i);
    g_autofree char *row = g_strdup_printf("[%s] %s · SP $%06X",
                                           ti->list_name ? ti->list_name : "TSK",
                                           ti->name ? ti->name : "?", ti->saved_sp);
    GtkWidget *l = gtk_label_new(row);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_list_box_append(GTK_LIST_BOX(self->tasks_list), l);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->tasks_list), (int)i);
    if (r) {
      g_object_set_data(G_OBJECT(r), "amiga-addr", GUINT_TO_POINTER(ti->saved_sp));
      g_object_set_data(G_OBJECT(r), "amiga-to-mem", GINT_TO_POINTER(1));
    }
  }
}

void
amiga_debug_sidebar_set_memmap(AmigaDebugSidebar *self, GPtrArray *regions)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  clear_listbox(self->memmap_list);
  if (!regions) return;
  for (guint i = 0; i < regions->len && i < 16; i++) {
    const AmigaMemRegion *mr = g_ptr_array_index(regions, i);
    g_autofree char *row = g_strdup_printf("$%06X (%4uK) %s",
                                           mr->start, mr->length / 1024u,
                                           mr->name ? mr->name : (mr->type ? mr->type : "RAM"));
    GtkWidget *l = gtk_label_new(row);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_list_box_append(GTK_LIST_BOX(self->memmap_list), l);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->memmap_list), (int)i);
    if (r) {
      g_object_set_data(G_OBJECT(r), "amiga-addr", GUINT_TO_POINTER(mr->start));
      g_object_set_data(G_OBJECT(r), "amiga-to-mem", GINT_TO_POINTER(1));
    }
  }
}

void
amiga_debug_sidebar_reset(AmigaDebugSidebar *self)
{
  g_return_if_fail(AMIGA_IS_DEBUG_SIDEBAR(self));
  self->has_regs = false;
  self->has_prev_cycles = false;
  self->paused = false;
  for (int i = 0; i < 8; i++) {
    g_autofree char *dl = g_strdup_printf("D%d --------", i);
    g_autofree char *al = g_strdup_printf("A%d --------", i);
    update_reg_cell(self->d_btns[i], dl, false);
    update_reg_cell(self->a_btns[i], al, false);
    gtk_widget_remove_css_class(self->sr_btns[i], "active");
  }
  gtk_button_set_label(GTK_BUTTON(self->sr_btns[2]), "I");
  update_reg_cell(self->btn_pc, "PC --------", false);
  gtk_label_set_text(GTK_LABEL(self->lbl_sr_hex), "SR ----");
  gtk_label_set_text(GTK_LABEL(self->lbl_beam), "DISCONNECTED");
  gtk_widget_set_visible(self->lbl_exc_last, FALSE);
  clear_listbox(self->stack_list);
  clear_listbox(self->hunks_list);
  clear_listbox(self->tasks_list);
  clear_listbox(self->memmap_list);
}
