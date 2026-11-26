#include "amiga_hrm_view.h"
#include "amiga_decode.h"
#include "../ide/ide_markdown.h"
#include <stdlib.h>
#include <string.h>

struct _AmigaHrmView {
  GtkBox           parent_instance;

  char            *hrm_dir;
  IdeMarkdownView *md_view;

  GtkWidget       *prov_badge;
  GtkWidget       *entry_query;
};

G_DEFINE_FINAL_TYPE(AmigaHrmView, amiga_hrm_view, GTK_TYPE_BOX)

static const AmigaHwRegInfo *
find_reg_by_query(const char *q)
{
  if (!q || !q[0]) return NULL;
  const char *p = q;
  while (*p == ' ' || *p == '\t') p++;
  if (p[0] == '$' || (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) {
    const char *hp = (p[0] == '$') ? p + 1 : p + 2;
    unsigned long v = strtoul(hp, NULL, 16);
    if (v >= 0xDFF000ul) v -= 0xDFF000ul;
    if (v <= 0x1FEul) return amiga_hw_reg_lookup((uint16_t)v);
  }
  for (uint16_t off = 0; off <= 0x1FEu; off += 2u) {
    const AmigaHwRegInfo *ri = amiga_hw_reg_lookup(off);
    if (ri && g_ascii_strcasecmp(ri->name, p) == 0) return ri;
  }
  return NULL;
}

static bool
try_load_local_hrm(AmigaHrmView *self, const char *q, char **out_md, char **out_path)
{
  if (!self->hrm_dir || !self->hrm_dir[0] || !g_file_test(self->hrm_dir, G_FILE_TEST_IS_DIR)) {
    return false;
  }
  g_autofree char *clean = g_strdup(q ? q : "index");
  g_strstrip(clean);
  if (!clean[0]) { g_free(clean); clean = g_strdup("index"); }

  const char *exts[2] = { ".md", ".txt" };
  for (int i = 0; i < 2; i++) {
    g_autofree char *fn = g_strconcat(clean, exts[i], NULL);
    g_autofree char *path = g_build_filename(self->hrm_dir, fn, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
        g_file_get_contents(path, out_md, NULL, NULL)) {
      *out_path = g_steal_pointer(&path);
      return true;
    }
  }
  return false;
}

static char *
build_builtin_ref_markdown(AmigaHrmView *self, const char *q)
{
  g_autoptr(GString) md = g_string_new("");
  const AmigaHwRegInfo *ri = find_reg_by_query(q);
  if (ri) {
    const char *acc = (ri->access == AMIGA_REG_R) ? "Read-Only (`RO`)"
                    : (ri->access == AMIGA_REG_W) ? "Write-Only (`WO`)" : "Read/Write (`RW`)";
    g_string_append_printf(
        md,
        "# $DFF%03X — %s\n\n"
        "- **Access:** %s\n"
        "- **Offset:** `$%03X`\n"
        "- **Function:** %s\n\n"
        "### Copper & Blitter Notes\n"
        "- Copper writes to `$DFF040..$DFF07E` require `COPCON = $0002`.\n"
        "- Press `K` on any `$DFFxxx` token, `_LVO*` offset, or 68000 mnemonic in Source view.\n",
        ri->offset, ri->name, acc, ri->offset, ri->summary);
  } else if (q && (g_str_has_prefix(q, "_LVO") || q[0] == '-')) {
    int16_t off = (int16_t)strtol(q, NULL, 10);
    const char *lvo = (q[0] == '-') ? amiga_lvo_lookup(NULL, off) : q;
    g_string_append_printf(
        md,
        "# Library Vector Reference — `%s`\n\n"
        "Standard AmigaOS jump-table dispatch (`jsr _LVO...(a6)`).\n"
        "Ensure `a6` holds the matching library base before invoking.\n",
        lvo ? lvo : q);
  } else {
    AmigaCycleInfo ci = (q && q[0]) ? amiga_m68k_line_cycles(q) : (AmigaCycleInfo){0};
    if (ci.has_cycles) {
      g_string_append_printf(
          md,
          "# 68000 Instruction Timing — `%s`\n\n"
          "- **Static Cycle Cost:** `%s` (`%u..%u` cycles, `%u` reads / `%u` writes)\n"
          "- **Source Gutter Rule:** Static line-by-line cost without unrolling loops or subroutines.\n",
          q, ci.display, ci.min_cycles, ci.max_cycles, ci.reads, ci.writes);
    } else {
      g_string_append(
          md,
          "# Amiga Hardware Reference Index\n\n"
          "| Addr | Reg | Access | Summary |\n"
          "| :--- | :--- | :--- | :--- |\n");
      static const uint16_t key_regs[14] = {
        0x002, 0x004, 0x02E, 0x040, 0x058, 0x080, 0x08E,
        0x090, 0x096, 0x09A, 0x0E0, 0x100, 0x108, 0x180
      };
      for (size_t i = 0; i < G_N_ELEMENTS(key_regs); i++) {
        const AmigaHwRegInfo *k = amiga_hw_reg_lookup(key_regs[i]);
        if (!k) continue;
        const char *acc = (k->access == AMIGA_REG_R) ? "RO"
                        : (k->access == AMIGA_REG_W) ? "WO" : "RW";
        g_string_append_printf(md, "| `$DFF%03X` | `%s` | %s | %s |\n",
                               k->offset, k->name, acc, k->summary);
      }
      g_string_append_printf(
          md,
          "\n> Configured `[docs].hrm_dir`: `%s` (configure a directory of `.md` HRM chapters in `kajo-amiga.toml` for local chapter lookup).\n",
          (self->hrm_dir && self->hrm_dir[0]) ? self->hrm_dir : "none");
    }
  }
  return g_string_free(g_steal_pointer(&md), FALSE);
}

static void
on_lookup_activate(GtkWidget *w G_GNUC_UNUSED, gpointer ud)
{
  AmigaHrmView *self = AMIGA_HRM_VIEW(ud);
  amiga_hrm_view_lookup(self, gtk_editable_get_text(GTK_EDITABLE(self->entry_query)));
}

static void
amiga_hrm_view_dispose(GObject *obj)
{
  AmigaHrmView *self = AMIGA_HRM_VIEW(obj);
  g_clear_pointer(&self->md_view, ide_markdown_view_free);
  g_clear_pointer(&self->hrm_dir, g_free);
  G_OBJECT_CLASS(amiga_hrm_view_parent_class)->dispose(obj);
}

static void
amiga_hrm_view_class_init(AmigaHrmViewClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = amiga_hrm_view_dispose;
}

static void
amiga_hrm_view_init(AmigaHrmView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 5);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 8);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 8);

  GtkWidget *r0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->prov_badge = gtk_label_new("BUILT-IN REF");
  gtk_widget_add_css_class(self->prov_badge, "amiga-badge-snapshot");

  self->entry_query = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_query, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_query), "$DFF180 / BPLCON0 / move.w");
  gtk_widget_set_hexpand(self->entry_query, TRUE);
  g_signal_connect(self->entry_query, "activate", G_CALLBACK(on_lookup_activate), self);

  GtkWidget *btn = gtk_button_new_with_label("🔎");
  gtk_widget_add_css_class(btn, "ide-toolbar-btn");
  g_signal_connect(btn, "clicked", G_CALLBACK(on_lookup_activate), self);

  gtk_box_append(GTK_BOX(r0), self->prov_badge);
  gtk_box_append(GTK_BOX(r0), self->entry_query);
  gtk_box_append(GTK_BOX(r0), btn);
  gtk_box_append(GTK_BOX(self), r0);

  self->md_view = ide_markdown_view_new(NULL, NULL, NULL);
  ide_markdown_view_set_read_only(self->md_view, TRUE);
  ide_markdown_view_set_show_header_bar(self->md_view, FALSE);
  GtkWidget *mdw = ide_markdown_view_get_widget(self->md_view);
  gtk_widget_set_vexpand(mdw, TRUE);
  gtk_widget_set_hexpand(mdw, TRUE);
  gtk_box_append(GTK_BOX(self), mdw);

  amiga_hrm_view_lookup(self, "");
}

AmigaHrmView *
amiga_hrm_view_new(void)
{
  return g_object_new(AMIGA_TYPE_HRM_VIEW, NULL);
}

void
amiga_hrm_view_set_hrm_dir(AmigaHrmView *self, const char *hrm_dir)
{
  g_return_if_fail(AMIGA_IS_HRM_VIEW(self));
  g_free(self->hrm_dir);
  self->hrm_dir = g_strdup(hrm_dir);
}

void
amiga_hrm_view_lookup(AmigaHrmView *self, const char *query)
{
  g_return_if_fail(AMIGA_IS_HRM_VIEW(self));
  if (query && query[0] &&
      g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(self->entry_query)), query) != 0) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_query), query);
  }

  g_autofree char *local_md = NULL;
  g_autofree char *local_path = NULL;
  if (try_load_local_hrm(self, query, &local_md, &local_path)) {
    gtk_label_set_text(GTK_LABEL(self->prov_badge), "LOCAL HRM");
    gtk_widget_remove_css_class(self->prov_badge, "amiga-badge-snapshot");
    gtk_widget_add_css_class(self->prov_badge, "amiga-badge-live");
    ide_markdown_view_set_context(self->md_view, local_path, self->hrm_dir);
    ide_markdown_view_render(self->md_view, local_md);
    return;
  }

  gtk_label_set_text(GTK_LABEL(self->prov_badge), "BUILT-IN REF");
  gtk_widget_remove_css_class(self->prov_badge, "amiga-badge-live");
  gtk_widget_add_css_class(self->prov_badge, "amiga-badge-snapshot");
  g_autofree char *built = build_builtin_ref_markdown(self, query);
  ide_markdown_view_set_context(self->md_view, NULL, self->hrm_dir);
  ide_markdown_view_render(self->md_view, built);
}
