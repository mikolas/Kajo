#include "amiga_window_private.h"
#include <stdlib.h>
#include <string.h>

static void
on_outline_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  guint line = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "outline-line"));
  if (!self->active_buf || line == 0) return;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_line(tb, &it, (int)(line - 1u));
  gtk_text_buffer_place_cursor(tb, &it);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view), &it, 0.2, FALSE, 0, 0.5);
  amiga_window_switch_center_page(self, "source");
}

void
amiga_window_refresh_source_gutter_and_outline(KajoAmigaWindow *self)
{
  if (!self->outline_list) return;
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->outline_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->outline_list), ch);
  }
  if (!self->active_buf) return;
  g_autofree char *txt = ide_buffer_get_text(self->active_buf);
  const char *p = ide_buffer_get_file_path(self->active_buf);
  bool is_c = p && (g_str_has_suffix(p, ".c") || g_str_has_suffix(p, ".h"));
  g_autoptr(GPtrArray) items = amiga_symbols_extract_outline(txt ? txt : "", is_c);

  for (guint i = 0; i < items->len; i++) {
    const AmigaOutlineItem *it = g_ptr_array_index(items, i);
    const char *tag = (it->kind == AMIGA_OUTLINE_SECTION) ? "SEC"
                    : (it->kind == AMIGA_OUTLINE_FUNCTION) ? "FN"
                    : (it->kind == AMIGA_OUTLINE_MACRO) ? "MAC"
                    : (it->kind == AMIGA_OUTLINE_EQU) ? "EQU" : "LBL";
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *l_tag = gtk_label_new(tag);
    gtk_widget_add_css_class(l_tag, "amiga-hunk-tag");
    GtkWidget *l_nm = gtk_label_new(it->name);
    gtk_label_set_xalign(GTK_LABEL(l_nm), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l_nm), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(l_nm, TRUE);
    g_autofree char *ln = g_strdup_printf(":%u", it->line);
    GtkWidget *l_ln = gtk_label_new(ln);
    gtk_widget_add_css_class(l_ln, "amiga-muted-hint");

    gtk_box_append(GTK_BOX(hb), l_tag);
    gtk_box_append(GTK_BOX(hb), l_nm);
    gtk_box_append(GTK_BOX(hb), l_ln);
    gtk_list_box_append(GTK_LIST_BOX(self->outline_list), hb);
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->outline_list), (int)i);
    if (row) g_object_set_data(G_OBJECT(row), "outline-line", GUINT_TO_POINTER(it->line));
  }
}

GtkWidget *
amiga_window_outline_build_box(KajoAmigaWindow *self)
{
  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *out_hdr = gtk_label_new("OUTLINE");
  gtk_widget_add_css_class(out_hdr, "amiga-pane-header");
  gtk_label_set_xalign(GTK_LABEL(out_hdr), 0.0f);
  GtkWidget *out_scr = gtk_scrolled_window_new();
  gtk_widget_set_size_request(out_scr, -1, 160);
  self->outline_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->outline_list, "amiga-diag-list");
  g_signal_connect(self->outline_list, "row-activated", G_CALLBACK(on_outline_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(out_scr), self->outline_list);
  gtk_box_append(GTK_BOX(vb), out_hdr);
  gtk_box_append(GTK_BOX(vb), out_scr);
  return vb;
}

static void
on_search_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  const char *path = g_object_get_data(G_OBJECT(row), "hit-path");
  guint line = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "hit-line"));
  if (path) amiga_window_open_file_at_line(self, path, line);
}

static void
on_rg_finished(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;

  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->search_list)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->search_list), ch);
  }
  if (!ok || !out || !out[0]) return;

  g_auto(GStrv) lines = g_strsplit(out, "\n", 120);
  for (int i = 0; lines[i] && lines[i][0]; i++) {
    g_auto(GStrv) cols = g_strsplit(lines[i], ":", 4);
    if (!cols[0] || !cols[1] || !cols[3]) continue;
    guint ln = (guint)strtoul(cols[1], NULL, 10);
    g_autofree char *abs = (g_path_is_absolute(cols[0]) || !self->project || !self->project->root_dir)
                               ? g_strdup(cols[0])
                               : g_build_filename(self->project->root_dir, cols[0], NULL);
    g_autofree char *rel = g_path_get_basename(cols[0]);
    g_autofree char *disp = g_strdup_printf("%s:%u  %s", rel, ln, g_strstrip(cols[3]));
    GtkWidget *l = gtk_label_new(disp);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_list_box_append(GTK_LIST_BOX(self->search_list), l);
    GtkWidget *last = gtk_widget_get_last_child(self->search_list);
    if (GTK_IS_LIST_BOX_ROW(last)) {
      g_object_set_data_full(G_OBJECT(last), "hit-path", g_steal_pointer(&abs), g_free);
      g_object_set_data(G_OBJECT(last), "hit-line", GUINT_TO_POINTER(ln));
    }
  }
}

static void
on_search_activate(GtkEntry *e, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  const char *q = gtk_editable_get_text(GTK_EDITABLE(e));
  if (!q || !q[0] || !self->project || !self->project->root_dir) return;

  g_cancellable_cancel(self->search_cancel);
  g_clear_object(&self->search_cancel);
  self->search_cancel = g_cancellable_new();

  g_autoptr(GError) err = NULL;
  const char *argv[] = {
    "rg", "--vimgrep", "--no-heading", "--max-count", "120", "--", q, self->project->root_dir, NULL
  };
  g_autoptr(GSubprocess) proc = g_subprocess_newv(
      argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
  if (!proc) return;
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  g_subprocess_communicate_utf8_async(proc, NULL, self->search_cancel, on_rg_finished, weak);
}

GtkWidget *
amiga_window_search_build_page(KajoAmigaWindow *self)
{
  GtkWidget *srch_pg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  self->search_entry = gtk_entry_new();
  gtk_widget_add_css_class(self->search_entry, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->search_entry), "rg workspace (Enter)");
  g_signal_connect(self->search_entry, "activate", G_CALLBACK(on_search_activate), self);
  GtkWidget *sr_scr = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sr_scr, TRUE);
  self->search_list = gtk_list_box_new();
  gtk_widget_add_css_class(self->search_list, "amiga-diag-list");
  g_signal_connect(self->search_list, "row-activated", G_CALLBACK(on_search_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sr_scr), self->search_list);
  gtk_box_append(GTK_BOX(srch_pg), self->search_entry);
  gtk_box_append(GTK_BOX(srch_pg), sr_scr);
  return srch_pg;
}

static gboolean
on_hrm_modal_key(GtkEventControllerKey *c G_GNUC_UNUSED,
                 guint                  keyval,
                 guint                  kc G_GNUC_UNUSED,
                 GdkModifierType        st G_GNUC_UNUSED,
                 gpointer               ud)
{
  if (keyval == GDK_KEY_Escape) {
    gtk_window_destroy(GTK_WINDOW(ud));
    return TRUE;
  }
  return FALSE;
}

void
amiga_window_show_hrm_quickdoc(KajoAmigaWindow *self, const char *query)
{
  bool insp_open = (self->session_state == AMIGA_SESSION_PAUSED ||
                    self->session_state == AMIGA_SESSION_ATTACH ||
                    (self->session_state == AMIGA_SESSION_RUNNING && self->inspector_frozen_snap));
  if (insp_open) {
    amiga_window_inspector_lookup_hrm(self, query);
    return;
  }

  GtkWindow *modal = GTK_WINDOW(gtk_window_new());
  gtk_window_set_decorated(modal, FALSE);
  gtk_window_set_modal(modal, TRUE);
  gtk_window_set_transient_for(modal, GTK_WINDOW(self));
  gtk_window_set_default_size(modal, 540, 420);
  gtk_widget_add_css_class(GTK_WIDGET(modal), "shell-amiga-modal");

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "settings-header-bar");
  GtkWidget *tl = gtk_label_new("📖 HRM HARDWARE QUICK-DOC (OFFLINE POPOVER)");
  gtk_widget_add_css_class(tl, "settings-header-title");
  gtk_widget_set_hexpand(tl, TRUE);
  gtk_label_set_xalign(GTK_LABEL(tl), 0.0f);
  GtkWidget *bx = gtk_button_new_with_label("✕");
  gtk_widget_add_css_class(bx, "settings-close-btn");
  g_signal_connect_swapped(bx, "clicked", G_CALLBACK(gtk_window_destroy), modal);
  gtk_box_append(GTK_BOX(hdr), tl);
  gtk_box_append(GTK_BOX(hdr), bx);
  gtk_box_append(GTK_BOX(outer), hdr);

  AmigaHrmView *hrm = amiga_hrm_view_new();
  if (self->project && self->project->hrm_dir && self->project->hrm_dir[0]) {
    g_autofree char *abs_hrm = g_path_is_absolute(self->project->hrm_dir)
                                   ? g_strdup(self->project->hrm_dir)
                                   : g_build_filename(self->project->root_dir, self->project->hrm_dir, NULL);
    amiga_hrm_view_set_hrm_dir(hrm, abs_hrm);
  }
  amiga_hrm_view_lookup(hrm, query ? query : "");
  gtk_widget_set_vexpand(GTK_WIDGET(hrm), TRUE);
  gtk_box_append(GTK_BOX(outer), GTK_WIDGET(hrm));

  GtkEventController *kc = gtk_event_controller_key_new();
  g_signal_connect(kc, "key-pressed", G_CALLBACK(on_hrm_modal_key), modal);
  gtk_widget_add_controller(GTK_WIDGET(modal), kc);

  gtk_window_set_child(modal, outer);
  gtk_window_present(modal);
}
