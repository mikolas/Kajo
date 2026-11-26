#include "amiga_mem_view.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _AmigaMemView {
  GtkBox parent_instance;

  GtkWidget          *entry_addr;
  GtkWidget          *entry_patch;
  GtkWidget          *btn_8b;
  GtkWidget          *btn_16b;
  GtkWidget          *btn_32b;
  GtkWidget          *toggle_follow;
  GtkWidget          *text_view;

  int                 group_bytes; /* 1, 2, or 4 */
  uint32_t            base_addr;
  GByteArray         *cur_bytes;
  GByteArray         *prev_bytes;
  uint32_t            prev_addr;

  AmigaCpuRegs        regs;
  bool                has_regs;
  AmigaCustomSnapshot custom;
  bool                has_custom;

  AmigaMemReadCb      read_cb;
  AmigaMemWriteCb     write_cb;
  AmigaMemWatchCb     watch_cb;
  gpointer            user_data;
};

G_DEFINE_FINAL_TYPE(AmigaMemView, amiga_mem_view, GTK_TYPE_BOX)

static void
amiga_mem_view_dispose(GObject *obj)
{
  AmigaMemView *self = AMIGA_MEM_VIEW(obj);
  g_clear_pointer(&self->cur_bytes, g_byte_array_unref);
  g_clear_pointer(&self->prev_bytes, g_byte_array_unref);
  G_OBJECT_CLASS(amiga_mem_view_parent_class)->dispose(obj);
}

static void
amiga_mem_view_class_init(AmigaMemViewClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = amiga_mem_view_dispose;
}

static bool
resolve_mem_expr(AmigaMemView *self, const char *expr, uint32_t *out)
{
  if (!expr || !out) return false;
  g_autofree char *s = g_strdup(expr);
  g_strstrip(s);
  if (!s[0]) return false;

  if (self->has_custom) {
    if (g_ascii_strcasecmp(s, "COP1LC") == 0) { *out = self->custom.cop1lc; return true; }
    if (g_ascii_strcasecmp(s, "COP2LC") == 0) { *out = self->custom.cop2lc; return true; }
    if (g_ascii_strcasecmp(s, "BPL1PT") == 0) { *out = self->custom.bplpt[0]; return true; }
    if (g_ascii_strcasecmp(s, "BLTDPT") == 0) { *out = self->custom.bltpt[3]; return true; }
  }
  if (self->has_regs) {
    if (g_ascii_strcasecmp(s, "PC") == 0) { *out = self->regs.pc; return true; }
    if (g_ascii_strcasecmp(s, "SP") == 0) { *out = self->regs.a[7]; return true; }
    if ((s[0] == 'D' || s[0] == 'd') && s[1] >= '0' && s[1] <= '7' && s[2] == '\0') {
      *out = self->regs.d[s[1] - '0'];
      return true;
    }
    if ((s[0] == 'A' || s[0] == 'a') && s[1] >= '0' && s[1] <= '7' && s[2] == '\0') {
      *out = self->regs.a[s[1] - '0'];
      return true;
    }
  }
  const char *p = s;
  if (p[0] == '$') p++;
  else if (g_ascii_strncasecmp(p, "0x", 2) == 0) p += 2;
  char *end = NULL;
  unsigned long v = strtoul(p, &end, 16);
  if (end && end > p) {
    *out = (uint32_t)v;
    return true;
  }
  return false;
}

static void
render_hex_buffer(AmigaMemView *self)
{
  GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  if (!self->cur_bytes || self->cur_bytes->len == 0) {
    gtk_text_buffer_set_text(tb, "No memory loaded (click READ or stop on breakpoint).", -1);
    return;
  }
  GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(tb);
  GtkTextTag *diff_tag = gtk_text_tag_table_lookup(tt, "changed");
  if (!diff_tag) {
    diff_tag = gtk_text_buffer_create_tag(tb, "changed",
                                          "foreground", "#ff8c00",
                                          "weight", PANGO_WEIGHT_BOLD, NULL);
  }
  gtk_text_buffer_set_text(tb, "", 0);

  const uint8_t *buf = self->cur_bytes->data;
  size_t len = self->cur_bytes->len;
  bool same_base = (self->prev_bytes && self->prev_addr == self->base_addr &&
                    self->prev_bytes->len == len);

  GtkTextIter end;
  gtk_text_buffer_get_end_iter(tb, &end);

  for (size_t off = 0; off < len; off += 16) {
    g_autofree char *addr_s = g_strdup_printf("%08X  ", self->base_addr + (uint32_t)off);
    gtk_text_buffer_insert(tb, &end, addr_s, -1);

    for (size_t col = 0; col < 16; col++) {
      if (off + col < len) {
        uint8_t b = buf[off + col];
        bool changed = same_base && (b != self->prev_bytes->data[off + col]);
        char h[4];
        g_snprintf(h, sizeof(h), "%02X", b);
        if (changed) {
          gtk_text_buffer_insert_with_tags(tb, &end, h, -1, diff_tag, NULL);
        } else {
          gtk_text_buffer_insert(tb, &end, h, -1);
        }
      } else {
        gtk_text_buffer_insert(tb, &end, "  ", -1);
      }
      if (((col + 1u) % (size_t)self->group_bytes) == 0) {
        gtk_text_buffer_insert(tb, &end, " ", -1);
      }
      if (col == 7) gtk_text_buffer_insert(tb, &end, " ", -1);
    }

    gtk_text_buffer_insert(tb, &end, " |", -1);
    for (size_t col = 0; col < 16 && off + col < len; col++) {
      uint8_t c = buf[off + col];
      char ch[2] = { (c >= 32 && c < 127) ? (char)c : '.', '\0' };
      gtk_text_buffer_insert(tb, &end, ch, -1);
    }
    gtk_text_buffer_insert(tb, &end, "|\n", -1);
  }
}

static void
on_addr_activate(GtkEntry *e, gpointer ud)
{
  AmigaMemView *self = AMIGA_MEM_VIEW(ud);
  uint32_t addr = 0;
  if (resolve_mem_expr(self, gtk_editable_get_text(GTK_EDITABLE(e)), &addr)) {
    amiga_mem_view_jump_to(self, addr);
  }
}

static void
on_read_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaMemView *self = AMIGA_MEM_VIEW(ud);
  uint32_t addr = self->base_addr;
  resolve_mem_expr(self, gtk_editable_get_text(GTK_EDITABLE(self->entry_addr)), &addr);
  amiga_mem_view_jump_to(self, addr);
}

static void
on_group_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaMemView *self = AMIGA_MEM_VIEW(ud);
  if (!gtk_toggle_button_get_active(tb)) return;
  int grp = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tb), "grp-bytes"));
  if (grp == 1 || grp == 2 || grp == 4) {
    self->group_bytes = grp;
    render_hex_buffer(self);
  }
}

static void
on_patch_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaMemView *self = AMIGA_MEM_VIEW(ud);
  if (!self->write_cb) return;
  const char *hex = gtk_editable_get_text(GTK_EDITABLE(self->entry_patch));
  g_autofree uint8_t *bytes = NULL;
  size_t len = 0;
  if (amiga_rsp_hex_decode(hex, &bytes, &len) && len > 0) {
    self->write_cb(self, self->base_addr, bytes, len, self->user_data);
  }
}

static void
on_watch_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaMemView *self = AMIGA_MEM_VIEW(ud);
  if (self->watch_cb) {
    self->watch_cb(self, 2, self->base_addr, 4, self->user_data);
  }
}

static void
on_dump_save_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GBytes *bytes = ud;
  g_autoptr(GFile) f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (f) {
    g_autofree char *path = g_file_get_path(f);
    if (path) {
      gsize len = 0;
      const char *data = g_bytes_get_data(bytes, &len);
      g_file_set_contents(path, data, (gssize)len, NULL);
    }
  }
  g_bytes_unref(bytes);
}

static void
on_dump_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaMemView *self = AMIGA_MEM_VIEW(ud);
  if (!self->cur_bytes || self->cur_bytes->len == 0) return;
  GBytes *copy = g_bytes_new(self->cur_bytes->data, self->cur_bytes->len);
  g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
  g_autofree char *fname = g_strdup_printf("mem_%08x.bin", self->base_addr);
  gtk_file_dialog_set_initial_name(dlg, fname);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  gtk_file_dialog_save(dlg, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                       NULL, on_dump_save_done, copy);
}

static void
amiga_mem_view_init(AmigaMemView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  self->group_bytes = 2;
  self->cur_bytes = g_byte_array_new();
  self->prev_bytes = g_byte_array_new();

  GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(tb, "amiga-disasm-toolbar");
  gtk_widget_set_valign(tb, GTK_ALIGN_CENTER);

  self->entry_addr = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_addr, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_addr), "$00020000 / COP1LC / BPL1PT / PC");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_addr), "$00DFF000");
  gtk_widget_set_hexpand(self->entry_addr, TRUE);
  g_signal_connect(self->entry_addr, "activate", G_CALLBACK(on_addr_activate), self);

  GtkWidget *btn_read = gtk_button_new_with_label("↻ READ");
  gtk_widget_add_css_class(btn_read, "ide-toolbar-btn");
  g_signal_connect(btn_read, "clicked", G_CALLBACK(on_read_clicked), self);

  self->btn_8b = gtk_toggle_button_new_with_label("8b");
  self->btn_16b = gtk_toggle_button_new_with_label("16b");
  self->btn_32b = gtk_toggle_button_new_with_label("32b");
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_16b), GTK_TOGGLE_BUTTON(self->btn_8b));
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_32b), GTK_TOGGLE_BUTTON(self->btn_8b));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_16b), TRUE);
  gtk_widget_add_css_class(self->btn_8b, "amiga-seg-btn");
  gtk_widget_add_css_class(self->btn_16b, "amiga-seg-btn");
  gtk_widget_add_css_class(self->btn_32b, "amiga-seg-btn");
  g_object_set_data(G_OBJECT(self->btn_8b), "grp-bytes", GINT_TO_POINTER(1));
  g_object_set_data(G_OBJECT(self->btn_16b), "grp-bytes", GINT_TO_POINTER(2));
  g_object_set_data(G_OBJECT(self->btn_32b), "grp-bytes", GINT_TO_POINTER(4));
  g_signal_connect(self->btn_8b, "toggled", G_CALLBACK(on_group_toggled), self);
  g_signal_connect(self->btn_16b, "toggled", G_CALLBACK(on_group_toggled), self);
  g_signal_connect(self->btn_32b, "toggled", G_CALLBACK(on_group_toggled), self);

  self->toggle_follow = gtk_toggle_button_new_with_label("📌 FOLLOW");
  gtk_widget_add_css_class(self->toggle_follow, "amiga-seg-btn");

  self->entry_patch = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_patch, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_patch), "Hex patch");
  gtk_widget_set_size_request(self->entry_patch, 96, -1);

  GtkWidget *btn_patch = gtk_button_new_with_label("WRITE");
  gtk_widget_add_css_class(btn_patch, "ide-toolbar-btn");
  g_signal_connect(btn_patch, "clicked", G_CALLBACK(on_patch_clicked), self);

  GtkWidget *btn_watch = gtk_button_new_with_label("👁 Z2 WATCH");
  gtk_widget_add_css_class(btn_watch, "ide-toolbar-btn");
  g_signal_connect(btn_watch, "clicked", G_CALLBACK(on_watch_clicked), self);

  GtkWidget *btn_dump = gtk_button_new_with_label("💾 .BIN");
  gtk_widget_add_css_class(btn_dump, "ide-toolbar-btn");
  g_signal_connect(btn_dump, "clicked", G_CALLBACK(on_dump_clicked), self);

  gtk_box_append(GTK_BOX(tb), self->entry_addr);
  gtk_box_append(GTK_BOX(tb), btn_read);
  gtk_box_append(GTK_BOX(tb), self->btn_8b);
  gtk_box_append(GTK_BOX(tb), self->btn_16b);
  gtk_box_append(GTK_BOX(tb), self->btn_32b);
  gtk_box_append(GTK_BOX(tb), self->toggle_follow);
  gtk_box_append(GTK_BOX(tb), self->entry_patch);
  gtk_box_append(GTK_BOX(tb), btn_patch);
  gtk_box_append(GTK_BOX(tb), btn_watch);
  gtk_box_append(GTK_BOX(tb), btn_dump);
  gtk_box_append(GTK_BOX(self), tb);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  self->text_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(self->text_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->text_view), TRUE);
  gtk_widget_add_css_class(self->text_view, "amiga-hex-view");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->text_view);
  gtk_box_append(GTK_BOX(self), scroll);
  render_hex_buffer(self);
}

AmigaMemView *
amiga_mem_view_new(void)
{
  return g_object_new(AMIGA_TYPE_MEM_VIEW, NULL);
}

void
amiga_mem_view_set_callbacks(AmigaMemView    *self,
                             AmigaMemReadCb   read_cb,
                             AmigaMemWriteCb  write_cb,
                             AmigaMemWatchCb  watch_cb,
                             gpointer         user_data)
{
  g_return_if_fail(AMIGA_IS_MEM_VIEW(self));
  self->read_cb = read_cb;
  self->write_cb = write_cb;
  self->watch_cb = watch_cb;
  self->user_data = user_data;
}

void
amiga_mem_view_set_regs_and_custom(AmigaMemView              *self,
                                   const AmigaCpuRegs        *regs,
                                   const AmigaCustomSnapshot *custom)
{
  g_return_if_fail(AMIGA_IS_MEM_VIEW(self));
  if (regs) { self->regs = *regs; self->has_regs = true; }
  if (custom) { self->custom = *custom; self->has_custom = true; }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->toggle_follow))) {
    uint32_t addr = 0;
    if (resolve_mem_expr(self, gtk_editable_get_text(GTK_EDITABLE(self->entry_addr)), &addr)) {
      if (self->read_cb) self->read_cb(self, addr, 256, self->user_data);
    }
  }
}

void
amiga_mem_view_set_data(AmigaMemView  *self,
                        uint32_t       base_addr,
                        const uint8_t *bytes,
                        size_t         len)
{
  g_return_if_fail(AMIGA_IS_MEM_VIEW(self));
  g_byte_array_set_size(self->prev_bytes, 0);
  if (self->cur_bytes->len > 0) {
    g_byte_array_append(self->prev_bytes, self->cur_bytes->data, self->cur_bytes->len);
    self->prev_addr = self->base_addr;
  }
  g_byte_array_set_size(self->cur_bytes, 0);
  self->base_addr = base_addr;
  if (bytes && len > 0) {
    g_byte_array_append(self->cur_bytes, bytes, (guint)len);
  }
  render_hex_buffer(self);
}

void
amiga_mem_view_jump_to(AmigaMemView *self, uint32_t addr)
{
  g_return_if_fail(AMIGA_IS_MEM_VIEW(self));
  self->base_addr = addr;
  g_autofree char *s = g_strdup_printf("$%08X", addr);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_addr), s);
  if (self->read_cb) self->read_cb(self, addr, 256, self->user_data);
}

uint32_t
amiga_mem_view_get_base_addr(AmigaMemView *self)
{
  g_return_val_if_fail(AMIGA_IS_MEM_VIEW(self), 0);
  return self->base_addr;
}
