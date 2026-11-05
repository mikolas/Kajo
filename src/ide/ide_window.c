#include "ide_window.h"
#include "../compositor/compositor.h"
#include "ide_agent_ipc.h"
#include "ide_buffer.h"
#include "ide_dap.h"
#include "ide_fsm.h"
#include "ide_gutter.h"
#include "ide_lsp.h"
#include "ide_markdown.h"
#include "ide_picker.h"
#include "ide_search.h"
#include "ide_tree.h"
#include <gdk/gdkkeysyms.h>
#include <gtksourceview/gtksource.h>
#include <string.h>

typedef struct {
  char *path;
  guint line_1based;
  guint col_1based;
} IdeJumpEntry;

struct _KajoIdeWindow {
  GtkWindow parent_instance;

  char            *workspace_root;
  IdeFsm          *fsm;
  GPtrArray       *buffers; /* IdeBuffer* */
  guint            active_buf_idx;

  /* Subsystems */
  IdeTree         *tree;
  IdePicker       *picker;
  IdeSearchDrawer *search_drawer;
  IdeLspManager   *lsp;
  IdeDapView      *dap;
  IdeAgentIpc     *agent_ipc;
  IdeMarkdownView *md_split_view;
  IdeMarkdownView *md_drawer_view;

  /* Top Chrome & Tab Ribbon */
  GtkWidget       *ws_badge;
  GtkWidget       *agent_hdr_badge;
  GtkWidget       *tab_ribbon_box;

  /* Main Editor Stage */
  GtkWidget       *tree_container;
  GtkWidget       *source_scroll;
  GtkWidget       *source_view;
  GtkWidget       *source_map;
  KajoIdeGutterRenderer *gutter_renderer;
  GtkWidget       *md_split_container;

  /* Overlays */
  GtkWidget       *chord_hud_box;
  GtkWidget       *chord_hud_title;
  GtkWidget       *toast_banner;
  GtkWidget       *toast_label;

  /* Bottom Contextual Drawer */
  GtkWidget       *drawer_revealer;
  GtkWidget       *drawer_stack;

  /* Bottom Status Bar */
  GtkWidget       *mode_pill;
  GtkWidget       *chord_badge;
  GtkWidget       *file_status_label;
  GtkWidget       *diag_status_label;
  GtkWidget       *lsp_status_badge;
  GtkWidget       *cursor_pos_label;

  /* In-buffer search bar */
  GtkWidget       *inline_search_bar;
  GtkWidget       *inline_search_entry;
  GString         *search_query;
  char            *yank_register;

  /* Cross-file Jumplist */
  GPtrArray       *jumplist; /* IdeJumpEntry* */
  int              jumplist_idx;
};

G_DEFINE_TYPE(KajoIdeWindow, kajo_ide_window, GTK_TYPE_WINDOW)

static void jump_entry_free(gpointer data) {
  IdeJumpEntry *e = data;
  if (!e) return;
  g_free(e->path);
  g_free(e);
}

static IdeBuffer *get_active_buffer(KajoIdeWindow *self) {
  if (!self || !self->buffers || self->buffers->len == 0) return NULL;
  if (self->active_buf_idx >= self->buffers->len)
    self->active_buf_idx = self->buffers->len - 1;
  return g_ptr_array_index(self->buffers, self->active_buf_idx);
}

static void get_cursor_line_col_1based(KajoIdeWindow *self,
                                       guint         *out_line,
                                       guint         *out_col) {
  if (out_line) *out_line = 1;
  if (out_col) *out_col = 1;
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(tbuf, &iter, gtk_text_buffer_get_insert(tbuf));
  if (out_line) *out_line = (guint)gtk_text_iter_get_line(&iter) + 1;
  if (out_col) *out_col = (guint)gtk_text_iter_get_line_offset(&iter) + 1;
}

static void record_jumplist_Here(KajoIdeWindow *self) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf || !ide_buffer_get_file_path(buf)) return;
  guint line = 1, col = 1;
  get_cursor_line_col_1based(self, &line, &col);

  if (self->jumplist->len > 0 && self->jumplist_idx >= 0 &&
      self->jumplist_idx < (int)self->jumplist->len) {
    IdeJumpEntry *cur = g_ptr_array_index(self->jumplist, self->jumplist_idx);
    if (g_strcmp0(cur->path, ide_buffer_get_file_path(buf)) == 0 &&
        cur->line_1based == line) {
      return;
    }
  }

  IdeJumpEntry *e = g_new0(IdeJumpEntry, 1);
  e->path = g_strdup(ide_buffer_get_file_path(buf));
  e->line_1based = line;
  e->col_1based = col;
  g_ptr_array_add(self->jumplist, e);
  self->jumplist_idx = (int)self->jumplist->len - 1;
}

static void show_toast(KajoIdeWindow *self, const char *msg) {
  if (!self || !self->toast_banner || !msg || !*msg) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_widget_set_visible(self->toast_banner, TRUE);
}

static void sync_markdown_views(KajoIdeWindow *self) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  gboolean split_vis = gtk_widget_get_visible(self->md_split_container);
  gboolean drawer_vis =
      gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)) &&
      g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->drawer_stack)),
                "markdown") == 0;
  if (!split_vis && !drawer_vis) return;

  g_autofree char *text = ide_buffer_get_text(buf);
  if (split_vis) ide_markdown_view_render(self->md_split_view, text);
  if (drawer_vis) ide_markdown_view_render(self->md_drawer_view, text);
}

static void update_status_bar_and_hud(KajoIdeWindow *self) {
  if (!self || !self->mode_pill) return;

  IdeState st = ide_fsm_get_state(self->fsm);
  gtk_label_set_text(GTK_LABEL(self->mode_pill), ide_state_to_string(st));

  const char *mode_classes[] = {
    "mode-normal", "mode-insert", "mode-select", "mode-search", "mode-leader", NULL
  };
  for (int i = 0; mode_classes[i]; i++)
    gtk_widget_remove_css_class(self->mode_pill, mode_classes[i]);
  gtk_widget_add_css_class(self->mode_pill, ide_state_get_css_class(st));

  const char *chord = ide_fsm_get_pending_chord(self->fsm);
  if (chord && *chord) {
    gtk_label_set_text(GTK_LABEL(self->chord_badge), chord);
    gtk_widget_set_visible(self->chord_badge, TRUE);
    g_autofree char *hud_hdr = g_strdup_printf("PENDING CHORD: %s", chord);
    gtk_label_set_text(GTK_LABEL(self->chord_hud_title), hud_hdr);
    gtk_widget_set_visible(self->chord_hud_box, TRUE);
  } else {
    gtk_widget_set_visible(self->chord_badge, FALSE);
    gtk_widget_set_visible(self->chord_hud_box, FALSE);
  }

  gtk_widget_set_visible(self->inline_search_bar, (st == IDE_STATE_SEARCH));
  if (st == IDE_STATE_SEARCH) {
    g_autofree char *s_txt = g_strdup_printf("/%s", self->search_query->str);
    gtk_label_set_text(GTK_LABEL(self->inline_search_entry), s_txt);
  }

  IdeBuffer *buf = get_active_buffer(self);
  if (buf) {
    g_autofree char *f_lbl = g_strdup_printf(
        "%s%s",
        ide_buffer_get_title(buf),
        ide_buffer_get_modified(buf) ? " ●" : "");
    gtk_label_set_text(GTK_LABEL(self->file_status_label), f_lbl);

    const GPtrArray *diags = ide_buffer_get_diagnostics(buf);
    guint errs = 0, warns = 0;
    if (diags) {
      for (guint i = 0; i < diags->len; i++) {
        const IdeDiagnostic *d = g_ptr_array_index(diags, i);
        if (d->severity == IDE_DIAG_ERROR) errs++;
        else if (d->severity == IDE_DIAG_WARNING) warns++;
      }
    }
    g_autofree char *d_lbl = g_strdup_printf("✕ %u  ⚠ %u", errs, warns);
    gtk_label_set_text(GTK_LABEL(self->diag_status_label), d_lbl);

    guint line = 1, col = 1;
    get_cursor_line_col_1based(self, &line, &col);
    g_autofree char *pos_lbl = g_strdup_printf("LN %u, COL %u", line, col);
    gtk_label_set_text(GTK_LABEL(self->cursor_pos_label), pos_lbl);
  }
}

static void on_tab_btn_clicked(GtkButton *btn, gpointer user_data);

static void rebuild_tab_ribbon(KajoIdeWindow *self) {
  if (!self || !self->tab_ribbon_box) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->tab_ribbon_box)) != NULL)
    gtk_box_remove(GTK_BOX(self->tab_ribbon_box), child);

  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *b = g_ptr_array_index(self->buffers, i);
    g_autofree char *lbl = g_strdup_printf(
        "%u  %s%s",
        i + 1,
        ide_buffer_get_title(b),
        ide_buffer_get_modified(b) ? " ●" : "");
    GtkWidget *btn = gtk_button_new_with_label(lbl);
    gtk_widget_add_css_class(btn, "ide-tab-item");
    if (i == self->active_buf_idx)
      gtk_widget_add_css_class(btn, "active");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(btn), "tab-index", GUINT_TO_POINTER(i));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_btn_clicked), self);
    gtk_box_append(GTK_BOX(self->tab_ribbon_box), btn);
  }
}

static void bind_active_buffer_to_view(KajoIdeWindow *self) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;

  GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(buf);
  gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view),
                           GTK_TEXT_BUFFER(sbuf));
  kajo_ide_gutter_renderer_set_buffer(self->gutter_renderer, buf);
  ide_lsp_manager_notify_open(self->lsp, buf);

  rebuild_tab_ribbon(self);
  sync_markdown_views(self);
  update_status_bar_and_hud(self);
  gtk_widget_queue_draw(self->source_view);
}

static void on_tab_btn_clicked(GtkButton *btn, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "tab-index"));
  if (idx < self->buffers->len) {
    record_jumplist_Here(self);
    self->active_buf_idx = idx;
    bind_active_buffer_to_view(self);
  }
}

static void move_cursor_to_line_col(KajoIdeWindow *self,
                                    guint          line_1based,
                                    guint          col_1based) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter iter;
  int line0 = line_1based > 0 ? (int)line_1based - 1 : 0;
  int col0 = col_1based > 0 ? (int)col_1based - 1 : 0;
  gtk_text_buffer_get_iter_at_line_offset(tbuf, &iter, line0, 0);
  int chars_in_line = gtk_text_iter_get_chars_in_line(&iter);
  if (col0 < chars_in_line)
    gtk_text_iter_set_line_offset(&iter, col0);
  gtk_text_buffer_place_cursor(tbuf, &iter);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view),
                               &iter,
                               0.15,
                               FALSE,
                               0.0,
                               0.5);
  update_status_bar_and_hud(self);
}

static void on_external_buffer_change(IdeBuffer *buf,
                                      gboolean   reloaded_cleanly,
                                      gpointer   user_data) {
  KajoIdeWindow *self = user_data;
  if (reloaded_cleanly) {
    g_autofree char *msg =
        g_strdup_printf("↻ Hot-reloaded %s from disk", ide_buffer_get_title(buf));
    show_toast(self, msg);
  } else {
    g_autofree char *msg = g_strdup_printf(
        "⚠ Disk conflict on %s (unsaved edits preserved)",
        ide_buffer_get_title(buf));
    show_toast(self, msg);
  }
  sync_markdown_views(self);
  update_status_bar_and_hud(self);
}

void kajo_ide_window_open_path(KajoIdeWindow *self,
                               const char    *path,
                               guint          line_1based,
                               guint          col_1based) {
  if (!self || !path || !*path) return;

  record_jumplist_Here(self);

  /* Check if already open */
  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *existing = g_ptr_array_index(self->buffers, i);
    if (g_strcmp0(ide_buffer_get_file_path(existing), path) == 0) {
      self->active_buf_idx = i;
      bind_active_buffer_to_view(self);
      move_cursor_to_line_col(self, line_1based, col_1based);
      return;
    }
  }

  IdeBuffer *nbuf = ide_buffer_new(path);
  ide_buffer_load_from_disk(nbuf, NULL);
  ide_buffer_set_external_change_callback(nbuf, on_external_buffer_change, self);

  /* Replace initial empty [No Name] buffer if unmodified */
  if (self->buffers->len == 1) {
    IdeBuffer *first = g_ptr_array_index(self->buffers, 0);
    if (ide_buffer_get_file_path(first) == NULL &&
        !ide_buffer_get_modified(first)) {
      g_ptr_array_remove_index(self->buffers, 0);
    }
  }

  g_ptr_array_add(self->buffers, nbuf);
  self->active_buf_idx = self->buffers->len - 1;
  bind_active_buffer_to_view(self);
  move_cursor_to_line_col(self, line_1based, col_1based);
}

void kajo_ide_window_open_files(KajoIdeWindow *self,
                                GFile        **files,
                                int            n_files) {
  if (!self || !files || n_files <= 0) return;
  for (int i = 0; i < n_files; i++) {
    g_autofree char *path = g_file_get_path(files[i]);
    if (!path) continue;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      g_free(self->workspace_root);
      self->workspace_root = g_strdup(path);
      ide_tree_set_workspace_root(self->tree, path);
      ide_search_drawer_set_workspace_root(self->search_drawer, path);
      g_autofree char *base = g_path_get_basename(path);
      gtk_label_set_text(GTK_LABEL(self->ws_badge), base);
    } else {
      kajo_ide_window_open_path(self, path, 1, 1);
    }
  }
}

static void show_drawer_page(KajoIdeWindow *self, const char *page_name) {
  gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), page_name);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), TRUE);
}

static void execute_buffer_motion_or_edit(KajoIdeWindow *self,
                                          const IdeAction *act) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter ins, bound;
  gtk_text_buffer_get_iter_at_mark(tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
  gtk_text_buffer_get_iter_at_mark(
      tbuf, &bound, gtk_text_buffer_get_selection_bound(tbuf));

  gboolean extend = (ide_fsm_get_state(self->fsm) == IDE_STATE_SELECT ||
                     act->type == IDE_ACTION_EXTEND_SELECTION);

  switch (act->type) {
    case IDE_ACTION_MOVE_LEFT:
      gtk_text_iter_backward_cursor_position(&ins);
      break;
    case IDE_ACTION_MOVE_RIGHT:
      gtk_text_iter_forward_cursor_position(&ins);
      break;
    case IDE_ACTION_MOVE_UP: {
      int line = gtk_text_iter_get_line(&ins);
      int col = gtk_text_iter_get_line_offset(&ins);
      if (line > 0) {
        gtk_text_buffer_get_iter_at_line_offset(tbuf, &ins, line - 1, 0);
        int max_c = gtk_text_iter_get_chars_in_line(&ins);
        if (col < max_c) gtk_text_iter_set_line_offset(&ins, col);
      }
      break;
    }
    case IDE_ACTION_MOVE_DOWN: {
      int line = gtk_text_iter_get_line(&ins);
      int col = gtk_text_iter_get_line_offset(&ins);
      int total = gtk_text_buffer_get_line_count(tbuf);
      if (line + 1 < total) {
        gtk_text_buffer_get_iter_at_line_offset(tbuf, &ins, line + 1, 0);
        int max_c = gtk_text_iter_get_chars_in_line(&ins);
        if (col < max_c) gtk_text_iter_set_line_offset(&ins, col);
      }
      break;
    }
    case IDE_ACTION_MOVE_WORD_FORWARD:
    case IDE_ACTION_SELECT_WORD:
      if (act->type == IDE_ACTION_SELECT_WORD) {
        gtk_text_iter_backward_word_start(&bound);
        gtk_text_iter_forward_word_end(&ins);
        gtk_text_buffer_select_range(tbuf, &ins, &bound);
        goto done;
      }
      gtk_text_iter_forward_word_end(&ins);
      break;
    case IDE_ACTION_MOVE_WORD_BACKWARD:
    case IDE_ACTION_SELECT_WORD_BACKWARD:
      if (act->type == IDE_ACTION_SELECT_WORD_BACKWARD) {
        bound = ins;
        gtk_text_iter_backward_word_start(&ins);
        gtk_text_buffer_select_range(tbuf, &ins, &bound);
        goto done;
      }
      gtk_text_iter_backward_word_start(&ins);
      break;
    case IDE_ACTION_SELECT_LINE:
    case IDE_ACTION_EXTEND_SELECTION:
      gtk_text_iter_set_line_offset(&bound, 0);
      if (!gtk_text_iter_ends_line(&ins))
        gtk_text_iter_forward_to_line_end(&ins);
      gtk_text_iter_forward_char(&ins);
      gtk_text_buffer_select_range(tbuf, &ins, &bound);
      goto done;
    case IDE_ACTION_COLLAPSE_SELECTION:
      gtk_text_buffer_place_cursor(tbuf, &ins);
      goto done;
    case IDE_ACTION_MOVE_LINE_START:
      gtk_text_iter_set_line_offset(&ins, 0);
      break;
    case IDE_ACTION_MOVE_LINE_END:
      if (!gtk_text_iter_ends_line(&ins))
        gtk_text_iter_forward_to_line_end(&ins);
      break;
    case IDE_ACTION_MOVE_BUFFER_START:
      gtk_text_buffer_get_start_iter(tbuf, &ins);
      break;
    case IDE_ACTION_MOVE_BUFFER_END:
      gtk_text_buffer_get_end_iter(tbuf, &ins);
      break;
    case IDE_ACTION_PAGE_DOWN:
      gtk_text_iter_forward_lines(&ins, 18);
      break;
    case IDE_ACTION_PAGE_UP:
      gtk_text_iter_backward_lines(&ins, 18);
      break;
    case IDE_ACTION_SELECT_TEXT_OBJECT: {
      g_autofree char *full = ide_buffer_get_text(buf);
      gsize cur_byte = 0, s_byte = 0, e_byte = 0;
      GtkTextIter start_it;
      gtk_text_buffer_get_start_iter(tbuf, &start_it);
      g_autofree char *prefix =
          gtk_text_buffer_get_text(tbuf, &start_it, &ins, TRUE);
      cur_byte = strlen(prefix);
      gboolean inside = (act->arg1[0] == 'i');
      if (ide_find_text_object(full,
                               cur_byte,
                               inside,
                               act->arg2[0],
                               &s_byte,
                               &e_byte)) {
        glong s_char = g_utf8_pointer_to_offset(full, full + s_byte);
        glong e_char = g_utf8_pointer_to_offset(full, full + e_byte);
        GtkTextIter sit, eit;
        gtk_text_buffer_get_iter_at_offset(tbuf, &sit, (int)s_char);
        gtk_text_buffer_get_iter_at_offset(tbuf, &eit, (int)e_char);
        gtk_text_buffer_select_range(tbuf, &eit, &sit);
      }
      goto done;
    }
    case IDE_ACTION_INSERT_CHAR:
      gtk_text_buffer_insert_at_cursor(tbuf, act->str, -1);
      goto post_edit;
    case IDE_ACTION_INSERT_NEWLINE:
      gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
      goto post_edit;
    case IDE_ACTION_INSERT_TAB:
      gtk_text_buffer_insert_at_cursor(tbuf, "  ", 2);
      goto post_edit;
    case IDE_ACTION_BACKSPACE:
      gtk_text_buffer_backspace(tbuf, &ins, TRUE, TRUE);
      goto post_edit;
    case IDE_ACTION_DELETE_CHAR:
    case IDE_ACTION_DELETE_SELECTION:
    case IDE_ACTION_CHANGE_SELECTION: {
      if (!gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound)) {
        bound = ins;
        gtk_text_iter_forward_cursor_position(&bound);
      }
      g_free(self->yank_register);
      self->yank_register = gtk_text_buffer_get_text(tbuf, &ins, &bound, TRUE);
      gtk_text_buffer_delete(tbuf, &ins, &bound);
      goto post_edit;
    }
    case IDE_ACTION_YANK: {
      if (!gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound)) {
        gtk_text_iter_set_line_offset(&ins, 0);
        bound = ins;
        gtk_text_iter_forward_line(&bound);
      }
      g_free(self->yank_register);
      self->yank_register = gtk_text_buffer_get_text(tbuf, &ins, &bound, TRUE);
      GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
      if (cb) gdk_clipboard_set_text(cb, self->yank_register);
      show_toast(self, "Yanked selection to clipboard");
      goto done;
    }
    case IDE_ACTION_PASTE_AFTER:
    case IDE_ACTION_PASTE_BEFORE:
      if (self->yank_register && *self->yank_register)
        gtk_text_buffer_insert_at_cursor(tbuf, self->yank_register, -1);
      goto post_edit;
    case IDE_ACTION_UNDO:
      if (gtk_text_buffer_get_can_undo(tbuf)) gtk_text_buffer_undo(tbuf);
      goto post_edit;
    case IDE_ACTION_REDO:
      if (gtk_text_buffer_get_can_redo(tbuf)) gtk_text_buffer_redo(tbuf);
      goto post_edit;
    default:
      return;
  }

  if (extend)
    gtk_text_buffer_select_range(tbuf, &ins, &bound);
  else
    gtk_text_buffer_place_cursor(tbuf, &ins);

  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(self->source_view),
                                     gtk_text_buffer_get_insert(tbuf));
  goto done;

post_edit:
  ide_buffer_recompute_diffs(buf);
  ide_lsp_manager_notify_change(self->lsp, buf);
  sync_markdown_views(self);
  rebuild_tab_ribbon(self);
  gtk_widget_queue_draw(self->source_view);

done:
  update_status_bar_and_hud(self);
}

static void dispatch_ide_action(KajoIdeWindow *self, const IdeAction *act) {
  if (!self || !act || act->type == IDE_ACTION_NONE) {
    update_status_bar_and_hud(self);
    return;
  }

  IdeBuffer *buf = get_active_buffer(self);
  guint cur_line = 1, cur_col = 1;
  get_cursor_line_col_1based(self, &cur_line, &cur_col);

  switch (act->type) {
    /* File Tree & Panes */
    case IDE_ACTION_TOGGLE_FILE_TREE:
      gtk_widget_set_visible(self->tree_container,
                             !gtk_widget_get_visible(self->tree_container));
      break;
    case IDE_ACTION_PANE_FOCUS_TREE:
      gtk_widget_set_visible(self->tree_container, TRUE);
      break;
    case IDE_ACTION_PANE_FOCUS_BUFFER:
      gtk_widget_grab_focus(self->source_view);
      break;
    case IDE_ACTION_PANE_FOCUS_DRAWER:
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), TRUE);
      break;
    case IDE_ACTION_TOGGLE_DRAWER:
      gtk_revealer_set_reveal_child(
          GTK_REVEALER(self->drawer_revealer),
          !gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)));
      break;
    case IDE_ACTION_DRAWER_CLOSE:
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);
      break;

    /* Graphical Markdown & Cairo Vector Mermaid */
    case IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW:
      gtk_widget_set_visible(self->md_split_container,
                             !gtk_widget_get_visible(self->md_split_container));
      sync_markdown_views(self);
      break;
    case IDE_ACTION_TOGGLE_MERMAID_MODE:
      show_drawer_page(self, "markdown");
      sync_markdown_views(self);
      break;
    case IDE_ACTION_TOGGLE_CHECKLIST:
      if (buf) {
        g_autofree char *txt = ide_buffer_get_text(buf);
        g_autofree char *toggled =
            ide_markdown_toggle_checklist_line(txt, cur_line);
        ide_buffer_set_text(buf, toggled);
        move_cursor_to_line_col(self, cur_line, cur_col);
        sync_markdown_views(self);
      }
      break;

    /* Fuzzy Picker */
    case IDE_ACTION_PICKER_OPEN_FILES:
      ide_picker_open_files(self->picker, self->workspace_root);
      break;
    case IDE_ACTION_PICKER_OPEN_BUFFERS:
      ide_picker_open_buffers(self->picker, self->buffers);
      break;
    case IDE_ACTION_PICKER_OPEN_GREP:
      ide_picker_open_grep(self->picker, self->workspace_root);
      break;
    case IDE_ACTION_HELP_OPEN:
    case IDE_ACTION_HELP_TOGGLE:
      ide_picker_open_help(self->picker);
      break;
    case IDE_ACTION_PICKER_NEXT:
    case IDE_ACTION_HELP_NEXT:
      ide_picker_move_next(self->picker);
      break;
    case IDE_ACTION_PICKER_PREV:
    case IDE_ACTION_HELP_PREV:
      ide_picker_move_prev(self->picker);
      break;
    case IDE_ACTION_HELP_PAGE_DOWN:
      ide_picker_page_down(self->picker);
      break;
    case IDE_ACTION_HELP_PAGE_UP:
      ide_picker_page_up(self->picker);
      break;
    case IDE_ACTION_PICKER_INPUT:
    case IDE_ACTION_HELP_INPUT:
      ide_picker_append_input(self->picker, act->str);
      break;
    case IDE_ACTION_PICKER_BACKSPACE:
    case IDE_ACTION_HELP_BACKSPACE:
      ide_picker_backspace(self->picker);
      break;
    case IDE_ACTION_PICKER_SELECT:
    case IDE_ACTION_HELP_SELECT:
      ide_picker_confirm(self->picker);
      break;
    case IDE_ACTION_PICKER_CANCEL:
    case IDE_ACTION_HELP_CANCEL:
      ide_picker_close(self->picker);
      break;

    /* File Tree Navigation & CRUD */
    case IDE_ACTION_TREE_UP:
      ide_tree_move_up(self->tree);
      break;
    case IDE_ACTION_TREE_DOWN:
      ide_tree_move_down(self->tree);
      break;
    case IDE_ACTION_TREE_PAGE_UP:
      ide_tree_page_up(self->tree);
      break;
    case IDE_ACTION_TREE_PAGE_DOWN:
      ide_tree_page_down(self->tree);
      break;
    case IDE_ACTION_TREE_TOP:
      ide_tree_move_top(self->tree);
      break;
    case IDE_ACTION_TREE_BOTTOM:
      ide_tree_move_bottom(self->tree);
      break;
    case IDE_ACTION_TREE_TOGGLE_OR_OPEN:
      if (ide_fsm_get_tree_editable(self->fsm)) {
        ide_tree_prompt_confirm(self->tree);
        ide_fsm_set_tree_editable(self->fsm, FALSE);
      } else {
        ide_tree_toggle_or_open(self->tree);
      }
      break;
    case IDE_ACTION_TREE_COLLAPSE_OR_PARENT:
      ide_tree_collapse_or_parent(self->tree);
      break;
    case IDE_ACTION_TREE_REFRESH:
      ide_tree_refresh(self->tree);
      break;
    case IDE_ACTION_TREE_CREATE_FILE:
      ide_tree_prompt_create_file(self->tree);
      break;
    case IDE_ACTION_TREE_CREATE_DIR:
      ide_tree_prompt_create_dir(self->tree);
      break;
    case IDE_ACTION_TREE_DELETE:
      ide_tree_delete_selected(self->tree);
      break;

    /* Tabs & Save */
    case IDE_ACTION_TAB_NEXT:
    case IDE_ACTION_STEP_FORWARD:
      if (self->buffers->len > 1) {
        self->active_buf_idx = (self->active_buf_idx + 1) % self->buffers->len;
        bind_active_buffer_to_view(self);
      }
      break;
    case IDE_ACTION_TAB_PREV:
    case IDE_ACTION_STEP_BACKWARD:
      if (self->buffers->len > 1) {
        self->active_buf_idx =
            (self->active_buf_idx + self->buffers->len - 1) % self->buffers->len;
        bind_active_buffer_to_view(self);
      }
      break;
    case IDE_ACTION_TAB_SWITCH:
      if (act->int_val >= 1 && (guint)act->int_val <= self->buffers->len) {
        self->active_buf_idx = (guint)(act->int_val - 1);
        bind_active_buffer_to_view(self);
      }
      break;
    case IDE_ACTION_SAVE:
      if (buf && ide_buffer_save(buf, NULL)) {
        ide_lsp_manager_notify_save(self->lsp, buf);
        ide_tree_refresh(self->tree);
        rebuild_tab_ribbon(self);
        show_toast(self, "✓ Saved file atomically");
      }
      break;
    case IDE_ACTION_SAVE_ALL:
      for (guint i = 0; i < self->buffers->len; i++) {
        IdeBuffer *b = g_ptr_array_index(self->buffers, i);
        if (ide_buffer_get_file_path(b)) ide_buffer_save(b, NULL);
      }
      ide_tree_refresh(self->tree);
      rebuild_tab_ribbon(self);
      show_toast(self, "✓ Saved all open buffers");
      break;
    case IDE_ACTION_BUFFER_CLOSE:
    case IDE_ACTION_BUFFER_FORCE_CLOSE:
      if (self->buffers->len > 1) {
        g_ptr_array_remove_index(self->buffers, self->active_buf_idx);
        if (self->active_buf_idx >= self->buffers->len)
          self->active_buf_idx = self->buffers->len - 1;
        bind_active_buffer_to_view(self);
      }
      break;

    /* Alternate File (Space a) & Jumplist (Ctrl+o / Ctrl+i) */
    case IDE_ACTION_ALTERNATE_FILE:
      if (buf && ide_buffer_get_file_path(buf)) {
        g_autofree char *alt =
            ide_resolve_alternate_path(ide_buffer_get_file_path(buf));
        if (alt) kajo_ide_window_open_path(self, alt, 1, 1);
      }
      break;
    case IDE_ACTION_JUMPLIST_BACK:
      if (self->jumplist_idx > 0) {
        self->jumplist_idx--;
        IdeJumpEntry *e =
            g_ptr_array_index(self->jumplist, self->jumplist_idx);
        kajo_ide_window_open_path(self, e->path, e->line_1based, e->col_1based);
      }
      break;
    case IDE_ACTION_JUMPLIST_FORWARD:
      if (self->jumplist_idx + 1 < (int)self->jumplist->len) {
        self->jumplist_idx++;
        IdeJumpEntry *e =
            g_ptr_array_index(self->jumplist, self->jumplist_idx);
        kajo_ide_window_open_path(self, e->path, e->line_1based, e->col_1based);
      }
      break;

    /* LSP 3.17 Actions */
    case IDE_ACTION_LSP_HOVER:
      if (buf)
        ide_lsp_manager_request_hover(
            self->lsp, self->source_view, buf, cur_line, cur_col);
      break;
    case IDE_ACTION_LSP_COMPLETE:
      if (buf)
        ide_lsp_manager_request_completion(
            self->lsp, self->source_view, buf, cur_line, cur_col);
      break;
    case IDE_ACTION_LSP_DEFINITION:
      if (buf)
        ide_lsp_manager_request_definition(self->lsp, buf, cur_line, cur_col);
      break;
    case IDE_ACTION_LSP_CANCEL:
      ide_lsp_manager_cancel_popover(self->lsp);
      break;
    case IDE_ACTION_LSP_SELECT_NEXT:
      ide_lsp_manager_select_next(self->lsp);
      break;
    case IDE_ACTION_LSP_SELECT_PREV:
      ide_lsp_manager_select_prev(self->lsp);
      break;
    case IDE_ACTION_LSP_SELECT_CONFIRM:
      if (buf) ide_lsp_manager_confirm_selection(self->lsp, buf);
      break;

    /* Search, Replace & Build Quickfix Drawer */
    case IDE_ACTION_SEARCH_OPEN:
      g_string_truncate(self->search_query, 0);
      break;
    case IDE_ACTION_SEARCH_INPUT:
      g_string_append(self->search_query, act->str);
      break;
    case IDE_ACTION_SEARCH_BACKSPACE:
      if (self->search_query->len > 0)
        g_string_truncate(self->search_query, self->search_query->len - 1);
      break;
    case IDE_ACTION_SEARCH_CONFIRM:
    case IDE_ACTION_SEARCH_NEXT:
      if (buf && self->search_query->len > 0) {
        GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
        GtkTextIter ins, mstart, mend;
        gtk_text_buffer_get_iter_at_mark(
            tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
        gtk_text_iter_forward_char(&ins);
        if (gtk_text_iter_forward_search(&ins,
                                         self->search_query->str,
                                         GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                         &mstart,
                                         &mend,
                                         NULL)) {
          gtk_text_buffer_select_range(tbuf, &mend, &mstart);
          gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view),
                                       &mstart,
                                       0.2,
                                       FALSE,
                                       0.0,
                                       0.5);
        }
      }
      break;
    case IDE_ACTION_REPLACE_OPEN:
      show_drawer_page(self, "search");
      ide_search_drawer_open_replace(self->search_drawer, buf, FALSE);
      break;
    case IDE_ACTION_PROJECT_REPLACE_OPEN:
      show_drawer_page(self, "search");
      ide_search_drawer_open_replace(self->search_drawer, buf, TRUE);
      break;
    case IDE_ACTION_REPLACE_APPLY_ALL:
      ide_search_drawer_apply_all(self->search_drawer, self->buffers);
      break;
    case IDE_ACTION_NEXT_ERROR:
      ide_search_drawer_step_error(self->search_drawer, 1);
      break;
    case IDE_ACTION_PREV_ERROR:
      ide_search_drawer_step_error(self->search_drawer, -1);
      break;

    /* DAP Debugger */
    case IDE_ACTION_DAP_TOGGLE_BREAKPOINT:
      if (buf) {
        ide_buffer_toggle_breakpoint(buf, cur_line);
        gtk_widget_queue_draw(self->source_view);
      }
      break;
    case IDE_ACTION_DAP_TOGGLE_VIEW:
      show_drawer_page(self, "dap");
      break;
    case IDE_ACTION_DAP_START:
      show_drawer_page(self, "dap");
      ide_dap_view_start_session(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_CONTINUE:
      ide_dap_view_continue(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STEP_OVER:
      ide_dap_view_step_over(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STEP_INTO:
      ide_dap_view_step_into(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STEP_OUT:
      ide_dap_view_step_out(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STOP:
      ide_dap_view_stop(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;

    /* Agent IPC & Batch Transaction Review (Space d s / d a / d r) */
    case IDE_ACTION_AGENT_REVIEW_OPEN:
      show_drawer_page(self, "agent");
      break;
    case IDE_ACTION_AGENT_ACCEPT_ALL:
      ide_agent_ipc_accept_all(self->agent_ipc);
      break;
    case IDE_ACTION_AGENT_REVERT_ALL:
      ide_agent_ipc_reject_all(self->agent_ipc);
      break;

    default:
      execute_buffer_motion_or_edit(self, act);
      break;
  }

  update_status_bar_and_hud(self);
}

static char *translate_gdk_key_to_fsm(guint keyval, GdkModifierType state) {
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean alt = (state & GDK_ALT_MASK) != 0;

  if (alt && keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9)
    return g_strdup_printf("alt+%c", (char)('1' + (keyval - GDK_KEY_1)));

  if (ctrl) {
    gunichar uc = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
    if (uc >= 'a' && uc <= 'z')
      return g_strdup_printf("ctrl+%c", (char)uc);
  }

  switch (keyval) {
    case GDK_KEY_Escape:    return g_strdup("esc");
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:  return g_strdup("enter");
    case GDK_KEY_Tab:       return g_strdup("tab");
    case GDK_KEY_ISO_Left_Tab: return g_strdup("shift+tab");
    case GDK_KEY_BackSpace: return g_strdup("backspace");
    case GDK_KEY_Delete:    return g_strdup("delete");
    case GDK_KEY_space:     return g_strdup("space");
    case GDK_KEY_Up:        return g_strdup("up");
    case GDK_KEY_Down:      return g_strdup("down");
    case GDK_KEY_Left:      return g_strdup("left");
    case GDK_KEY_Right:     return g_strdup("right");
    case GDK_KEY_Page_Up:   return g_strdup("pgup");
    case GDK_KEY_Page_Down: return g_strdup("pgdn");
    case GDK_KEY_Home:      return g_strdup("home");
    case GDK_KEY_End:       return g_strdup("end");
    case GDK_KEY_F1:        return g_strdup("f1");
    case GDK_KEY_F5:        return g_strdup("f5");
    case GDK_KEY_F9:        return g_strdup("f9");
    case GDK_KEY_F10:       return g_strdup("f10");
    case GDK_KEY_F11:       return g_strdup("f11");
    default: break;
  }

  gunichar uc = gdk_keyval_to_unicode(keyval);
  if (uc >= 32 && uc != 127) {
    char utf8[8] = {0};
    int len = g_unichar_to_utf8(uc, utf8);
    utf8[len] = '\0';
    return g_strdup(utf8);
  }
  return NULL;
}

static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint                  keyval,
                                      guint                  keycode,
                                      GdkModifierType        state,
                                      gpointer               user_data) {
  (void)controller;
  (void)keycode;
  KajoIdeWindow *self = user_data;

  g_autofree char *key_str = translate_gdk_key_to_fsm(keyval, state);
  if (!key_str) return FALSE;

  IdeAction act = ide_fsm_handle_key(self->fsm, key_str);
  dispatch_ide_action(self, &act);
  return TRUE;
}

/* Subsystem Callbacks */
static void on_tree_open_file(const char *abs_path, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  kajo_ide_window_open_path(self, abs_path, 1, 1);
  ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  update_status_bar_and_hud(self);
}

static void on_picker_open_file(const char *abs_path,
                                guint       line_1based,
                                guint       col_1based,
                                gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  kajo_ide_window_open_path(self, abs_path, line_1based, col_1based);
  ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  update_status_bar_and_hud(self);
}

static void on_picker_command(IdeActionType action, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  IdeAction act = { .type = action };
  dispatch_ide_action(self, &act);
}

static void on_lsp_status_changed(const char *server_name,
                                  gboolean    active,
                                  gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  g_autofree char *lbl =
      g_strdup_printf("LSP: %s%s",
                      server_name ? server_name : "none",
                      active ? " ●" : "");
  gtk_label_set_text(GTK_LABEL(self->lsp_status_badge), lbl);
}

static void on_agent_reload_cb(const char *path,
                               const char *old_snapshot_opt,
                               gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  if (!path || !*path) return;
  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *b = g_ptr_array_index(self->buffers, i);
    if (g_strcmp0(ide_buffer_get_file_path(b), path) == 0) {
      if (old_snapshot_opt) ide_buffer_record_agent_snapshot(b);
      else ide_buffer_accept_agent_changes(b);
      ide_buffer_load_from_disk(b, NULL);
    }
  }
  sync_markdown_views(self);
  gtk_widget_queue_draw(self->source_view);
}

static void on_agent_status_cb(const char *badge_text,
                               const char *toast_msg,
                               guint       pending_tx_count,
                               gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  if (badge_text && *badge_text) {
    gtk_label_set_text(GTK_LABEL(self->agent_hdr_badge), badge_text);
    gtk_widget_set_visible(self->agent_hdr_badge, TRUE);
  } else if (pending_tx_count > 0) {
    g_autofree char *txt =
        g_strdup_printf("🤖 %u AGENT EDITS (Space d s)", pending_tx_count);
    gtk_label_set_text(GTK_LABEL(self->agent_hdr_badge), txt);
    gtk_widget_set_visible(self->agent_hdr_badge, TRUE);
  } else {
    gtk_widget_set_visible(self->agent_hdr_badge, FALSE);
  }
  if (toast_msg && *toast_msg) show_toast(self, toast_msg);
}

static void on_markdown_node_jump(guint line_1based, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  move_cursor_to_line_col(self, line_1based, 1);
}

static void on_markdown_checklist_edit(const char *new_full_text,
                                       gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf || !new_full_text) return;
  ide_buffer_set_text(buf, new_full_text);
  rebuild_tab_ribbon(self);
}

/* Header Bar Button Callbacks */
static void on_hdr_tree_clicked(GtkButton *b, gpointer d) {
  (void)b;
  IdeAction act = { .type = IDE_ACTION_TOGGLE_FILE_TREE };
  dispatch_ide_action(KAJO_IDE_WINDOW(d), &act);
}

static void on_hdr_find_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  ide_fsm_set_state(self->fsm, IDE_STATE_PICKER);
  ide_picker_open_files(self->picker, self->workspace_root);
  update_status_bar_and_hud(self);
}

static void on_hdr_md_clicked(GtkButton *b, gpointer d) {
  (void)b;
  IdeAction act = { .type = IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW };
  dispatch_ide_action(KAJO_IDE_WINDOW(d), &act);
}

static void on_hdr_build_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  show_drawer_page(self, "search");
  ide_search_drawer_run_build(self->search_drawer);
}

static void on_hdr_float_clicked(GtkButton *b, gpointer d) {
  (void)b;
  (void)d;
  niri_send_action("{\"Action\":{\"ToggleWindowFloating\":{}}}");
}

static void on_hdr_close_clicked(GtkButton *b, gpointer d) {
  (void)b;
  gtk_window_close(GTK_WINDOW(d));
}

static void kajo_ide_window_dispose(GObject *object) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(object);
  g_clear_pointer(&self->fsm, ide_fsm_free);
  g_clear_pointer(&self->tree, ide_tree_free);
  g_clear_pointer(&self->picker, ide_picker_free);
  g_clear_pointer(&self->search_drawer, ide_search_drawer_free);
  g_clear_pointer(&self->lsp, ide_lsp_manager_free);
  g_clear_pointer(&self->dap, ide_dap_view_free);
  g_clear_pointer(&self->agent_ipc, ide_agent_ipc_free);
  g_clear_pointer(&self->md_split_view, ide_markdown_view_free);
  g_clear_pointer(&self->md_drawer_view, ide_markdown_view_free);
  if (self->buffers) {
    g_ptr_array_free(self->buffers, TRUE);
    self->buffers = NULL;
  }
  if (self->jumplist) {
    g_ptr_array_free(self->jumplist, TRUE);
    self->jumplist = NULL;
  }
  if (self->search_query) {
    g_string_free(self->search_query, TRUE);
    self->search_query = NULL;
  }
  g_clear_pointer(&self->yank_register, g_free);
  g_clear_pointer(&self->workspace_root, g_free);
  G_OBJECT_CLASS(kajo_ide_window_parent_class)->dispose(object);
}

static void kajo_ide_window_class_init(KajoIdeWindowClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = kajo_ide_window_dispose;
}

static void kajo_ide_window_init(KajoIdeWindow *self) {
  gtk_window_set_title(GTK_WINDOW(self), "Kajo IDE");
  gtk_window_set_default_size(GTK_WINDOW(self), 1320, 840);
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-settings-window");
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-ide-window");

  self->workspace_root = g_get_current_dir();
  self->fsm = ide_fsm_new();
  self->buffers = g_ptr_array_new_with_free_func((GDestroyNotify)ide_buffer_free);
  self->jumplist = g_ptr_array_new_with_free_func(jump_entry_free);
  self->jumplist_idx = -1;
  self->search_query = g_string_new("");

  GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(self), root_vbox);

  /* 1. Aero + Metro Window Chrome Drag Header */
  GtkWidget *win_handle = gtk_window_handle_new();
  GtkWidget *hdr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr_box, "settings-header-bar");
  gtk_widget_set_valign(hdr_box, GTK_ALIGN_CENTER);

  GtkWidget *title_lbl = gtk_label_new("⌨ KAJO IDE");
  gtk_widget_add_css_class(title_lbl, "settings-header-title");
  gtk_widget_set_valign(title_lbl, GTK_ALIGN_CENTER);

  g_autofree char *ws_base = g_path_get_basename(self->workspace_root);
  self->ws_badge = gtk_label_new(ws_base);
  gtk_widget_add_css_class(self->ws_badge, "shell-popover-badge");
  gtk_widget_set_valign(self->ws_badge, GTK_ALIGN_CENTER);

  self->agent_hdr_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->agent_hdr_badge, "shell-popover-badge");
  gtk_widget_set_valign(self->agent_hdr_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->agent_hdr_badge, FALSE);

  GtkWidget *hdr_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(hdr_spacer, TRUE);

  GtkWidget *btn_tree = gtk_button_new_with_label("🌲 TREE (SPC e)");
  gtk_widget_set_valign(btn_tree, GTK_ALIGN_CENTER);
  g_signal_connect(btn_tree, "clicked", G_CALLBACK(on_hdr_tree_clicked), self);

  GtkWidget *btn_find = gtk_button_new_with_label("🔍 FIND (SPC f)");
  gtk_widget_set_valign(btn_find, GTK_ALIGN_CENTER);
  g_signal_connect(btn_find, "clicked", G_CALLBACK(on_hdr_find_clicked), self);

  GtkWidget *btn_md = gtk_button_new_with_label("📖 MD GUI (SPC m p)");
  gtk_widget_set_valign(btn_md, GTK_ALIGN_CENTER);
  g_signal_connect(btn_md, "clicked", G_CALLBACK(on_hdr_md_clicked), self);

  GtkWidget *btn_build = gtk_button_new_with_label("⚙ BUILD");
  gtk_widget_set_valign(btn_build, GTK_ALIGN_CENTER);
  g_signal_connect(btn_build, "clicked", G_CALLBACK(on_hdr_build_clicked), self);

  GtkWidget *btn_float = gtk_button_new_with_label("🗗 FLOAT / TILE");
  gtk_widget_set_valign(btn_float, GTK_ALIGN_CENTER);
  g_signal_connect(btn_float, "clicked", G_CALLBACK(on_hdr_float_clicked), self);

  GtkWidget *btn_close = gtk_button_new_with_label("✕");
  gtk_widget_add_css_class(btn_close, "settings-close-btn");
  gtk_widget_set_valign(btn_close, GTK_ALIGN_CENTER);
  g_signal_connect(btn_close, "clicked", G_CALLBACK(on_hdr_close_clicked), self);

  gtk_box_append(GTK_BOX(hdr_box), title_lbl);
  gtk_box_append(GTK_BOX(hdr_box), self->ws_badge);
  gtk_box_append(GTK_BOX(hdr_box), self->agent_hdr_badge);
  gtk_box_append(GTK_BOX(hdr_box), hdr_spacer);
  gtk_box_append(GTK_BOX(hdr_box), btn_tree);
  gtk_box_append(GTK_BOX(hdr_box), btn_find);
  gtk_box_append(GTK_BOX(hdr_box), btn_md);
  gtk_box_append(GTK_BOX(hdr_box), btn_build);
  gtk_box_append(GTK_BOX(hdr_box), btn_float);
  gtk_box_append(GTK_BOX(hdr_box), btn_close);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(win_handle), hdr_box);
  gtk_box_append(GTK_BOX(root_vbox), win_handle);

  /* 2. Multi-Buffer Tab Ribbon */
  self->tab_ribbon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(self->tab_ribbon_box, "ide-tab-ribbon");
  gtk_widget_set_valign(self->tab_ribbon_box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(root_vbox), self->tab_ribbon_box);

  /* 3. Main Workspace Overlay & Split */
  GtkWidget *stage_overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(stage_overlay, TRUE);
  gtk_widget_set_vexpand(stage_overlay, TRUE);

  GtkWidget *workspace_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(workspace_hbox, TRUE);
  gtk_widget_set_vexpand(workspace_hbox, TRUE);

  /* Left File Tree Sidebar */
  self->tree = ide_tree_new(self->workspace_root, on_tree_open_file, self);
  self->tree_container = ide_tree_get_widget(self->tree);
  gtk_box_append(GTK_BOX(workspace_hbox), self->tree_container);

  /* Center GtkSourceView + 6-Column Cairo Gutter + GtkSourceMap Minimap */
  GtkWidget *editor_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(editor_hbox, TRUE);
  gtk_widget_set_vexpand(editor_hbox, TRUE);

  self->source_scroll = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(self->source_scroll, TRUE);
  gtk_widget_set_vexpand(self->source_scroll, TRUE);

  self->source_view = gtk_source_view_new();
  gtk_widget_add_css_class(self->source_view, "ide-source-view");
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->source_view), TRUE);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(self->source_view), TRUE);
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->source_view), 2);
  gtk_source_view_set_insert_spaces_instead_of_tabs(
      GTK_SOURCE_VIEW(self->source_view), TRUE);

  GtkSourceGutter *left_gutter = gtk_source_view_get_gutter(
      GTK_SOURCE_VIEW(self->source_view), GTK_TEXT_WINDOW_LEFT);
  self->gutter_renderer =
      KAJO_IDE_GUTTER_RENDERER(kajo_ide_gutter_renderer_new(NULL));
  gtk_source_gutter_insert(
      left_gutter, GTK_SOURCE_GUTTER_RENDERER(self->gutter_renderer), 0);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->source_scroll),
                                self->source_view);
  gtk_box_append(GTK_BOX(editor_hbox), self->source_scroll);

  self->source_map = gtk_source_map_new();
  gtk_source_map_set_view(GTK_SOURCE_MAP(self->source_map),
                          GTK_SOURCE_VIEW(self->source_view));
  gtk_widget_set_size_request(self->source_map, 84, -1);
  gtk_box_append(GTK_BOX(editor_hbox), self->source_map);

  /* Side-by-Side Graphical Markdown & Vector Mermaid Split (Space m p) */
  self->md_split_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);
  self->md_split_container = ide_markdown_view_get_widget(self->md_split_view);
  gtk_widget_set_size_request(self->md_split_container, 460, -1);
  gtk_widget_set_visible(self->md_split_container, FALSE);
  gtk_box_append(GTK_BOX(editor_hbox), self->md_split_container);

  gtk_box_append(GTK_BOX(workspace_hbox), editor_hbox);
  gtk_overlay_set_child(GTK_OVERLAY(stage_overlay), workspace_hbox);

  /* Centered Fuzzy Picker Overlay */
  self->picker = ide_picker_new(
      self->workspace_root, on_picker_open_file, on_picker_command, self);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay),
                          ide_picker_get_widget(self->picker));

  /* Bottom-Right Which-Key Chord Helper HUD */
  self->chord_hud_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(self->chord_hud_box, "ide-chord-hud");
  gtk_widget_set_halign(self->chord_hud_box, GTK_ALIGN_END);
  gtk_widget_set_valign(self->chord_hud_box, GTK_ALIGN_END);
  gtk_widget_set_margin_end(self->chord_hud_box, 18);
  gtk_widget_set_margin_bottom(self->chord_hud_box, 18);
  gtk_widget_set_visible(self->chord_hud_box, FALSE);

  self->chord_hud_title = gtk_label_new("LEADER CHORDS");
  gtk_widget_add_css_class(self->chord_hud_title, "shell-popover-badge");
  gtk_widget_set_halign(self->chord_hud_title, GTK_ALIGN_START);

  GtkWidget *hud_grid = gtk_label_new(
      "f  Find Files      b  Buffers         /  Live Grep\n"
      "e  File Tree       a  Alternate C/H   w  Save File\n"
      "m p Markdown GUI   d s Agent Diffs    d b Breakpoint");
  gtk_widget_set_halign(hud_grid, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(self->chord_hud_box), self->chord_hud_title);
  gtk_box_append(GTK_BOX(self->chord_hud_box), hud_grid);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), self->chord_hud_box);

  /* Top-Right Toast / External Reload Banner */
  self->toast_banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->toast_banner, "ide-chord-hud");
  gtk_widget_set_halign(self->toast_banner, GTK_ALIGN_END);
  gtk_widget_set_valign(self->toast_banner, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->toast_banner, 12);
  gtk_widget_set_margin_end(self->toast_banner, 16);
  gtk_widget_set_visible(self->toast_banner, FALSE);
  self->toast_label = gtk_label_new("");
  gtk_box_append(GTK_BOX(self->toast_banner), self->toast_label);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), self->toast_banner);

  gtk_box_append(GTK_BOX(root_vbox), stage_overlay);

  /* 4. Contextual Bottom Drawer (Search/Build, DAP, Agent Transactions, Markdown) */
  self->drawer_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->drawer_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);

  self->drawer_stack = gtk_stack_new();
  self->search_drawer = ide_search_drawer_new(
      self->workspace_root, on_picker_open_file, NULL, self);
  self->dap = ide_dap_view_new(NULL, self);
  self->agent_ipc = ide_agent_ipc_new(
      on_picker_open_file, on_agent_reload_cb, on_agent_status_cb, self);
  ide_agent_ipc_start_default(self->agent_ipc);
  self->md_drawer_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);

  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_search_drawer_get_widget(self->search_drawer),
                      "search");
  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_dap_view_get_widget(self->dap),
                      "dap");
  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_agent_ipc_get_drawer_widget(self->agent_ipc),
                      "agent");
  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_markdown_view_get_widget(self->md_drawer_view),
                      "markdown");

  gtk_revealer_set_child(GTK_REVEALER(self->drawer_revealer), self->drawer_stack);
  gtk_box_append(GTK_BOX(root_vbox), self->drawer_revealer);

  /* 5. Inline Search Bar (/ query) */
  self->inline_search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->inline_search_bar, "ide-drawer-header");
  gtk_widget_set_visible(self->inline_search_bar, FALSE);
  self->inline_search_entry = gtk_label_new("/");
  gtk_box_append(GTK_BOX(self->inline_search_bar), self->inline_search_entry);
  gtk_box_append(GTK_BOX(root_vbox), self->inline_search_bar);

  /* 6. Bottom Status Bar */
  GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(status_bar, "ide-status-bar");
  gtk_widget_set_valign(status_bar, GTK_ALIGN_CENTER);

  self->mode_pill = gtk_label_new("NORMAL");
  gtk_widget_add_css_class(self->mode_pill, "ide-mode-pill");
  gtk_widget_add_css_class(self->mode_pill, "mode-normal");
  gtk_widget_set_valign(self->mode_pill, GTK_ALIGN_CENTER);

  self->chord_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->chord_badge, "shell-popover-badge");
  gtk_widget_set_valign(self->chord_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->chord_badge, FALSE);

  self->file_status_label = gtk_label_new("[No Name]");
  gtk_widget_set_valign(self->file_status_label, GTK_ALIGN_CENTER);

  self->diag_status_label = gtk_label_new("✕ 0  ⚠ 0");
  gtk_widget_add_css_class(self->diag_status_label, "shell-popover-badge");
  gtk_widget_set_valign(self->diag_status_label, GTK_ALIGN_CENTER);

  GtkWidget *st_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(st_spacer, TRUE);

  self->lsp_status_badge = gtk_label_new("LSP: READY");
  gtk_widget_add_css_class(self->lsp_status_badge, "shell-popover-badge");
  gtk_widget_set_valign(self->lsp_status_badge, GTK_ALIGN_CENTER);

  self->cursor_pos_label = gtk_label_new("LN 1, COL 1");
  gtk_widget_add_css_class(self->cursor_pos_label, "shell-popover-badge");
  gtk_widget_set_valign(self->cursor_pos_label, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(status_bar), self->mode_pill);
  gtk_box_append(GTK_BOX(status_bar), self->chord_badge);
  gtk_box_append(GTK_BOX(status_bar), self->file_status_label);
  gtk_box_append(GTK_BOX(status_bar), self->diag_status_label);
  gtk_box_append(GTK_BOX(status_bar), st_spacer);
  gtk_box_append(GTK_BOX(status_bar), self->lsp_status_badge);
  gtk_box_append(GTK_BOX(status_bar), self->cursor_pos_label);
  gtk_box_append(GTK_BOX(root_vbox), status_bar);

  /* LSP Manager */
  self->lsp = ide_lsp_manager_new(
      self->workspace_root, on_picker_open_file, on_lsp_status_changed, self);

  /* Initial empty buffer */
  IdeBuffer *init_buf = ide_buffer_new(NULL);
  g_ptr_array_add(self->buffers, init_buf);
  self->active_buf_idx = 0;
  bind_active_buffer_to_view(self);

  /* Capture-phase Modal FSM Key Controller */
  GtkEventController *key_ctrl = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
  g_signal_connect(
      key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);
}

KajoIdeWindow *kajo_ide_window_new(GtkApplication *app,
                                   const char     *workspace_dir) {
  KajoIdeWindow *win =
      g_object_new(KAJO_TYPE_IDE_WINDOW, "application", app, NULL);
  if (workspace_dir && *workspace_dir &&
      g_file_test(workspace_dir, G_FILE_TEST_IS_DIR)) {
    g_free(win->workspace_root);
    win->workspace_root = g_strdup(workspace_dir);
    ide_tree_set_workspace_root(win->tree, workspace_dir);
    ide_search_drawer_set_workspace_root(win->search_drawer, workspace_dir);
    g_autofree char *base = g_path_get_basename(workspace_dir);
    gtk_label_set_text(GTK_LABEL(win->ws_badge), base);
  }
  return win;
}
