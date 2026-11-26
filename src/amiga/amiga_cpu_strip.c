#include "amiga_cpu_strip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _AmigaCpuStrip {
  GtkBox parent_instance;

  GtkWidget *d_btns[8];
  GtkWidget *a_btns[8];
  GtkWidget *pc_btn;
  GtkWidget *sr_btns[8]; /* T, S, I2..0, X, N, Z, V, C -> wait: display 8 concise flags T S I X N Z V C */
  GtkWidget *beam_label;
  GtkWidget *btn_cp_save;
  GtkWidget *btn_cp_restore;

  AmigaCpuRegs         cur_regs;
  AmigaCpuRegs         prev_regs;
  bool                 has_regs;
  bool                 has_prev_regs;
  bool                 paused;
  uint64_t             prev_cycles;
  bool                 has_prev_cycles;

  AmigaCpuWriteRegsCb  write_cb;
  AmigaCpuJumpAddrCb   jump_cb;
  AmigaCpuCheckpointCb cp_cb;
  gpointer             user_data;
};

G_DEFINE_FINAL_TYPE(AmigaCpuStrip, amiga_cpu_strip, GTK_TYPE_BOX)

static const uint16_t k_sr_bit_masks[8] = {
  0x8000u, /* T */
  0x2000u, /* S */
  0x0700u, /* I (IPL mask cycles/info; click toggles IPL 0 <-> 7) */
  0x0010u, /* X */
  0x0008u, /* N */
  0x0004u, /* Z */
  0x0002u, /* V */
  0x0001u, /* C */
};

static const char *const k_sr_labels[8] = { "T", "S", "I", "X", "N", "Z", "V", "C" };

static void
amiga_cpu_strip_class_init(AmigaCpuStripClass *klass G_GNUC_UNUSED)
{
}

typedef struct {
  AmigaCpuStrip *strip;
  int            reg_index; /* 0..7=D, 8..15=A, 17=PC */
  GtkWidget     *popover;
  GtkWidget     *entry;
} RegEditCtx;

static void
reg_edit_ctx_free(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  g_free(data);
}

static void
on_reg_apply(GtkButton *btn G_GNUC_UNUSED, gpointer ud)
{
  RegEditCtx *ctx = ud;
  AmigaCpuStrip *self = ctx->strip;
  if (!self->has_regs || !self->paused || !self->write_cb) {
    gtk_popover_popdown(GTK_POPOVER(ctx->popover));
    return;
  }
  const char *txt = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
  if (txt && txt[0] == '$') txt++;
  uint32_t val = (uint32_t)strtoul(txt ? txt : "0", NULL, 16);
  AmigaCpuRegs next = self->cur_regs;
  if (ctx->reg_index < 8) next.d[ctx->reg_index] = val;
  else if (ctx->reg_index < 16) next.a[ctx->reg_index - 8] = val;
  else if (ctx->reg_index == 17) next.pc = val;
  self->write_cb(self, &next, self->user_data);
  gtk_popover_popdown(GTK_POPOVER(ctx->popover));
}

static void
on_reg_jump_hex(GtkButton *btn G_GNUC_UNUSED, gpointer ud)
{
  RegEditCtx *ctx = ud;
  AmigaCpuStrip *self = ctx->strip;
  if (self->has_regs && self->jump_cb) {
    uint32_t val = (ctx->reg_index < 8) ? self->cur_regs.d[ctx->reg_index]
                 : (ctx->reg_index < 16) ? self->cur_regs.a[ctx->reg_index - 8]
                                         : self->cur_regs.pc;
    self->jump_cb(self, val, true, self->user_data);
  }
  gtk_popover_popdown(GTK_POPOVER(ctx->popover));
}

static void
on_reg_jump_disasm(GtkButton *btn G_GNUC_UNUSED, gpointer ud)
{
  RegEditCtx *ctx = ud;
  AmigaCpuStrip *self = ctx->strip;
  if (self->has_regs && self->jump_cb) {
    uint32_t val = (ctx->reg_index < 8) ? self->cur_regs.d[ctx->reg_index]
                 : (ctx->reg_index < 16) ? self->cur_regs.a[ctx->reg_index - 8]
                                         : self->cur_regs.pc;
    self->jump_cb(self, val, false, self->user_data);
  }
  gtk_popover_popdown(GTK_POPOVER(ctx->popover));
}

static void
format_binary32(uint32_t v, char out[40])
{
  size_t pos = 0;
  for (int b = 31; b >= 0; b--) {
    out[pos++] = ((v >> b) & 1u) ? '1' : '0';
    if (b > 0 && (b % 8) == 0) out[pos++] = ' ';
  }
  out[pos] = '\0';
}

static void
on_reg_chip_clicked(GtkButton *btn, gpointer ud)
{
  AmigaCpuStrip *self = AMIGA_CPU_STRIP(ud);
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "reg-idx"));
  if (!self->has_regs) return;

  uint32_t val = (idx < 8) ? self->cur_regs.d[idx]
               : (idx < 16) ? self->cur_regs.a[idx - 8]
                            : self->cur_regs.pc;

  GtkWidget *pop = gtk_popover_new();
  gtk_widget_set_parent(pop, GTK_WIDGET(btn));

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);

  char binbuf[40];
  format_binary32(val, binbuf);
  const char *rname = (idx < 8) ? g_strdup_printf("D%d", idx)
                    : (idx < 16) ? g_strdup_printf("A%d", idx - 8)
                                 : g_strdup("PC");
  g_autofree char *rname_owned = (char *)rname;
  g_autofree char *meta = g_strdup_printf(
      "%s = $%08X\nSigned: %d   Unsigned: %u\nBin: %s",
      rname_owned, val, (int32_t)val, val, binbuf);
  GtkWidget *meta_lbl = gtk_label_new(meta);
  gtk_label_set_xalign(GTK_LABEL(meta_lbl), 0.0f);
  gtk_box_append(GTK_BOX(vbox), meta_lbl);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_add_css_class(entry, "amiga-entry");
  g_autofree char *hex = g_strdup_printf("$%08X", val);
  gtk_editable_set_text(GTK_EDITABLE(entry), hex);

  GtkWidget *act_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign(act_row, GTK_ALIGN_CENTER);
  GtkWidget *btn_set = gtk_button_new_with_label("SET (G)");
  gtk_widget_add_css_class(btn_set, "ide-toolbar-btn");
  gtk_widget_add_css_class(btn_set, "accent-btn");
  gtk_widget_set_sensitive(btn_set, self->paused && self->write_cb != NULL);

  GtkWidget *btn_hex = gtk_button_new_with_label("HEX DUMP");
  gtk_widget_add_css_class(btn_hex, "ide-toolbar-btn");
  GtkWidget *btn_dis = gtk_button_new_with_label("DISASM");
  gtk_widget_add_css_class(btn_dis, "ide-toolbar-btn");

  gtk_box_append(GTK_BOX(act_row), entry);
  gtk_box_append(GTK_BOX(act_row), btn_set);
  gtk_box_append(GTK_BOX(act_row), btn_hex);
  gtk_box_append(GTK_BOX(act_row), btn_dis);
  gtk_box_append(GTK_BOX(vbox), act_row);

  RegEditCtx *ctx = g_new0(RegEditCtx, 1);
  ctx->strip = self;
  ctx->reg_index = idx;
  ctx->popover = pop;
  ctx->entry = entry;

  g_signal_connect_data(btn_set, "clicked", G_CALLBACK(on_reg_apply),
                        ctx, reg_edit_ctx_free, 0);
  g_signal_connect(entry, "activate", G_CALLBACK(on_reg_apply), ctx);
  g_signal_connect(btn_hex, "clicked", G_CALLBACK(on_reg_jump_hex), ctx);
  g_signal_connect(btn_dis, "clicked", G_CALLBACK(on_reg_jump_disasm), ctx);
  g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);

  gtk_popover_set_child(GTK_POPOVER(pop), vbox);
  gtk_popover_popup(GTK_POPOVER(pop));
}

static void
on_sr_pill_clicked(GtkButton *btn, gpointer ud)
{
  AmigaCpuStrip *self = AMIGA_CPU_STRIP(ud);
  if (!self->has_regs || !self->paused || !self->write_cb) return;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "sr-idx"));
  AmigaCpuRegs next = self->cur_regs;
  if (idx == 2) {
    /* IPL cycles 0 -> 7 -> 0 */
    uint32_t ipl = (next.sr >> 8) & 0x7u;
    next.sr = (next.sr & ~0x0700u) | ((ipl == 0) ? 0x0700u : 0x0000u);
  } else {
    next.sr ^= k_sr_bit_masks[idx];
  }
  self->write_cb(self, &next, self->user_data);
}

static void
on_cp_save_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer ud)
{
  AmigaCpuStrip *self = AMIGA_CPU_STRIP(ud);
  if (self->cp_cb) self->cp_cb(self, false, self->user_data);
}

static void
on_cp_restore_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer ud)
{
  AmigaCpuStrip *self = AMIGA_CPU_STRIP(ud);
  if (self->cp_cb) self->cp_cb(self, true, self->user_data);
}

static GtkWidget *
make_reg_btn(const char *label, int reg_idx, AmigaCpuStrip *self)
{
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_set_focusable(b, FALSE);
  gtk_widget_set_hexpand(b, FALSE);
  gtk_widget_add_css_class(b, "amiga-reg-chip");
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(b), "reg-idx", GINT_TO_POINTER(reg_idx));
  g_signal_connect(b, "clicked", G_CALLBACK(on_reg_chip_clicked), self);
  return b;
}

static void
amiga_cpu_strip_init(AmigaCpuStrip *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 0);
  gtk_widget_add_css_class(GTK_WIDGET(self), "amiga-cpu-strip");

  GtkWidget *scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scr), TRUE);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scr), FALSE);
  gtk_widget_set_hexpand(scr, TRUE);

  GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_valign(top, GTK_ALIGN_CENTER);
  GtkWidget *bot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_valign(bot, GTK_ALIGN_CENTER);

  for (int i = 0; i < 8; i++) {
    g_autofree char *lbl = g_strdup_printf("D%d:--------", i);
    self->d_btns[i] = make_reg_btn(lbl, i, self);
    gtk_box_append(GTK_BOX(top), self->d_btns[i]);
  }
  self->pc_btn = make_reg_btn("PC:--------", 17, self);
  gtk_box_append(GTK_BOX(top), self->pc_btn);

  GtkWidget *top_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(top_spacer, TRUE);
  gtk_box_append(GTK_BOX(top), top_spacer);

  GtkWidget *sr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_valign(sr_box, GTK_ALIGN_CENTER);
  for (int i = 0; i < 8; i++) {
    self->sr_btns[i] = gtk_button_new_with_label(k_sr_labels[i]);
    gtk_widget_set_focusable(self->sr_btns[i], FALSE);
    gtk_widget_add_css_class(self->sr_btns[i], "amiga-sr-pill");
    gtk_widget_set_valign(self->sr_btns[i], GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(self->sr_btns[i]), "sr-idx", GINT_TO_POINTER(i));
    g_signal_connect(self->sr_btns[i], "clicked", G_CALLBACK(on_sr_pill_clicked), self);
    gtk_box_append(GTK_BOX(sr_box), self->sr_btns[i]);
  }
  gtk_box_append(GTK_BOX(top), sr_box);

  for (int i = 0; i < 8; i++) {
    g_autofree char *lbl = g_strdup_printf("A%d:--------", i);
    self->a_btns[i] = make_reg_btn(lbl, 8 + i, self);
    gtk_box_append(GTK_BOX(bot), self->a_btns[i]);
  }

  GtkWidget *bot_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(bot_spacer, TRUE);
  gtk_box_append(GTK_BOX(bot), bot_spacer);

  self->beam_label = gtk_label_new("DISCONNECTED");
  gtk_widget_add_css_class(self->beam_label, "amiga-beam-meta");
  gtk_widget_set_valign(self->beam_label, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(bot), self->beam_label);

  GtkWidget *cp_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_set_valign(cp_box, GTK_ALIGN_CENTER);
  self->btn_cp_save = gtk_button_new_with_label("💾 CP");
  gtk_widget_set_focusable(self->btn_cp_save, FALSE);
  gtk_widget_add_css_class(self->btn_cp_save, "ide-toolbar-btn");
  gtk_widget_set_tooltip_text(self->btn_cp_save, "Save Savestate Checkpoint (qRcmd,checkpoint save)");
  g_signal_connect(self->btn_cp_save, "clicked", G_CALLBACK(on_cp_save_clicked), self);

  self->btn_cp_restore = gtk_button_new_with_label("⏮ CP");
  gtk_widget_set_focusable(self->btn_cp_restore, FALSE);
  gtk_widget_add_css_class(self->btn_cp_restore, "ide-toolbar-btn");
  gtk_widget_set_tooltip_text(self->btn_cp_restore, "Restore Checkpoint (qRcmd,checkpoint restore)");
  g_signal_connect(self->btn_cp_restore, "clicked", G_CALLBACK(on_cp_restore_clicked), self);

  gtk_box_append(GTK_BOX(cp_box), self->btn_cp_save);
  gtk_box_append(GTK_BOX(cp_box), self->btn_cp_restore);
  gtk_box_append(GTK_BOX(bot), cp_box);

  gtk_box_append(GTK_BOX(rows), top);
  gtk_box_append(GTK_BOX(rows), bot);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), rows);
  gtk_box_append(GTK_BOX(self), scr);
}

AmigaCpuStrip *
amiga_cpu_strip_new(void)
{
  return g_object_new(AMIGA_TYPE_CPU_STRIP, NULL);
}

void
amiga_cpu_strip_set_callbacks(AmigaCpuStrip       *self,
                              AmigaCpuWriteRegsCb  write_cb,
                              AmigaCpuJumpAddrCb   jump_cb,
                              AmigaCpuCheckpointCb cp_cb,
                              gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_CPU_STRIP(self));
  self->write_cb = write_cb;
  self->jump_cb = jump_cb;
  self->cp_cb = cp_cb;
  self->user_data = user_data;
}

static void
update_reg_btn(GtkWidget *btn, const char *text, bool changed)
{
  gtk_button_set_label(GTK_BUTTON(btn), text);
  if (changed) gtk_widget_add_css_class(btn, "amiga-reg-changed");
  else gtk_widget_remove_css_class(btn, "amiga-reg-changed");
}

void
amiga_cpu_strip_update(AmigaCpuStrip          *self,
                       const AmigaCpuRegs     *regs,
                       const AmigaExecStatus  *exec_st,
                       const AmigaSymbolTable *syms,
                       bool                    paused)
{
  g_return_if_fail(AMIGA_IS_CPU_STRIP(self));
  self->paused = paused;
  if (!regs) return;

  for (int i = 0; i < 8; i++) {
    g_autofree char *dt = g_strdup_printf("D%d:%08X", i, regs->d[i]);
    update_reg_btn(self->d_btns[i], dt, self->has_regs && regs->d[i] != self->cur_regs.d[i]);
    g_autofree char *at = g_strdup_printf("A%d:%08X", i, regs->a[i]);
    update_reg_btn(self->a_btns[i], at, self->has_regs && regs->a[i] != self->cur_regs.a[i]);
  }

  const char *sname = NULL;
  uint32_t sdelta = 0;
  g_autofree char *pc_txt = NULL;
  if (syms && amiga_symbols_resolve_addr(syms, regs->pc, &sname, &sdelta)) {
    pc_txt = sdelta == 0 ? g_strdup_printf("PC:%08X (%s)", regs->pc, sname)
                         : g_strdup_printf("PC:%08X (%s+$%x)", regs->pc, sname, sdelta);
  } else {
    pc_txt = g_strdup_printf("PC:%08X", regs->pc);
  }
  update_reg_btn(self->pc_btn, pc_txt, self->has_regs && regs->pc != self->cur_regs.pc);

  AmigaSrFlags f = amiga_decode_sr((uint16_t)(regs->sr & 0xFFFFu));
  bool active[8] = {
    f.trace, f.supervisor, f.ipl > 0, f.extend, f.negative, f.zero, f.overflow, f.carry
  };
  for (int i = 0; i < 8; i++) {
    if (i == 2) {
      g_autofree char *ilbl = g_strdup_printf("I%u", f.ipl);
      gtk_button_set_label(GTK_BUTTON(self->sr_btns[i]), ilbl);
    }
    if (active[i]) gtk_widget_add_css_class(self->sr_btns[i], "active");
    else gtk_widget_remove_css_class(self->sr_btns[i], "active");
  }

  if (exec_st) {
    uint64_t delta_cyc = (self->has_prev_cycles && exec_st->cycles >= self->prev_cycles)
                             ? (exec_st->cycles - self->prev_cycles)
                             : 0;
    g_autofree char *beam = g_strdup_printf(
        "F%u V%u H%u +%llu CYC",
        exec_st->frame, exec_st->vpos, exec_st->hpos,
        (unsigned long long)delta_cyc);
    gtk_label_set_text(GTK_LABEL(self->beam_label), beam);
    self->prev_cycles = exec_st->cycles;
    self->has_prev_cycles = true;
  } else {
    gtk_label_set_text(GTK_LABEL(self->beam_label), paused ? "STOPPED" : "RUNNING");
  }

  self->prev_regs = self->cur_regs;
  self->has_prev_regs = self->has_regs;
  self->cur_regs = *regs;
  self->has_regs = true;
}

void
amiga_cpu_strip_reset(AmigaCpuStrip *self)
{
  g_return_if_fail(AMIGA_IS_CPU_STRIP(self));
  self->has_regs = false;
  self->has_prev_regs = false;
  self->has_prev_cycles = false;
  self->paused = false;
  for (int i = 0; i < 8; i++) {
    g_autofree char *dt = g_strdup_printf("D%d:--------", i);
    update_reg_btn(self->d_btns[i], dt, false);
    g_autofree char *at = g_strdup_printf("A%d:--------", i);
    update_reg_btn(self->a_btns[i], at, false);
    gtk_widget_remove_css_class(self->sr_btns[i], "active");
  }
  gtk_button_set_label(GTK_BUTTON(self->sr_btns[2]), "I");
  update_reg_btn(self->pc_btn, "PC:--------", false);
  gtk_label_set_text(GTK_LABEL(self->beam_label), "DISCONNECTED");
}
