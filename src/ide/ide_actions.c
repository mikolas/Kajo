#include "ide_window_private.h"

void ide_window_show_drawer_page(KajoIdeWindow *self, const char *page_name) {
  if (!self || !page_name) return;
  gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), page_name);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), TRUE);

  struct {
    GtkWidget  *btn;
    const char *name;
  } tabs[] = {
    { self->drawer_tab_agent,  "agent"    },
    { self->drawer_tab_dap,    "dap"      },
    { self->drawer_tab_search, "search"   },
    { self->drawer_tab_md,     "markdown" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(tabs); i++) {
    if (!tabs[i].btn) continue;
    if (g_strcmp0(page_name, tabs[i].name) == 0)
      gtk_widget_add_css_class(tabs[i].btn, "active");
    else
      gtk_widget_remove_css_class(tabs[i].btn, "active");
  }
}

/* ─── 1. Buffer Motions & Edits ─── */

typedef enum {
  BUF_ACT_UNHANDLED = 0,
  BUF_ACT_DONE,
  BUF_ACT_SCROLL,
  BUF_ACT_POST_EDIT,
} BufferActionOutcome;

static void move_vertical_clamp_col(GtkTextBuffer *tbuf, GtkTextIter *ins, int delta) {
  int line = gtk_text_iter_get_line(ins);
  int col = gtk_text_iter_get_line_offset(ins);
  int target = line + delta;
  int total = gtk_text_buffer_get_line_count(tbuf);
  if (target < 0 || target >= total) return;

  gtk_text_buffer_get_iter_at_line_offset(tbuf, ins, target, 0);
  int max_c = gtk_text_iter_get_chars_in_line(ins);
  if (gtk_text_iter_ends_line(ins)) max_c = 0;
  else if (max_c > 0) max_c--;
  gtk_text_iter_set_line_offset(ins, MIN(col, max_c));
}

static BufferActionOutcome execute_buffer_motion(KajoIdeWindow   *self,
                                                 IdeBuffer       *buf,
                                                 GtkTextBuffer   *tbuf,
                                                 GtkTextIter     *ins,
                                                 GtkTextIter     *bound,
                                                 const IdeAction *act) {
  gboolean extend = (ide_fsm_get_state(self->fsm) == IDE_STATE_SELECT ||
                     act->type == IDE_ACTION_EXTEND_SELECTION);

  switch (act->type) {
    case IDE_ACTION_MOVE_LEFT:
      gtk_text_iter_backward_cursor_position(ins);
      break;
    case IDE_ACTION_MOVE_RIGHT:
      gtk_text_iter_forward_cursor_position(ins);
      break;
    case IDE_ACTION_MOVE_UP:
      move_vertical_clamp_col(tbuf, ins, -1);
      break;
    case IDE_ACTION_MOVE_DOWN:
      move_vertical_clamp_col(tbuf, ins, 1);
      break;
    case IDE_ACTION_EXTEND_SELECTION:
      if (g_strcmp0(act->str, "left") == 0) {
        gtk_text_iter_backward_cursor_position(ins);
      } else if (g_strcmp0(act->str, "right") == 0) {
        gtk_text_iter_forward_cursor_position(ins);
      } else if (g_strcmp0(act->str, "up") == 0) {
        move_vertical_clamp_col(tbuf, ins, -1);
      } else if (g_strcmp0(act->str, "down") == 0) {
        move_vertical_clamp_col(tbuf, ins, 1);
      }
      extend = TRUE;
      break;
    case IDE_ACTION_MOVE_WORD_FORWARD:
      gtk_text_iter_forward_word_end(ins);
      break;
    case IDE_ACTION_SELECT_WORD:
      if (gtk_text_iter_forward_word_end(ins)) {
        *bound = *ins;
        gtk_text_iter_backward_word_start(bound);
        gtk_text_buffer_select_range(tbuf, ins, bound);
      }
      return BUF_ACT_SCROLL;
    case IDE_ACTION_MOVE_WORD_BACKWARD:
      gtk_text_iter_backward_word_start(ins);
      break;
    case IDE_ACTION_SELECT_WORD_BACKWARD:
      if (gtk_text_iter_backward_word_start(ins)) {
        *bound = *ins;
        gtk_text_iter_forward_word_end(bound);
        gtk_text_buffer_select_range(tbuf, ins, bound);
      }
      return BUF_ACT_SCROLL;
    case IDE_ACTION_SELECT_LINE: {
      GtkTextIter s_it, e_it;
      if (gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it) &&
          gtk_text_iter_get_line_offset(&s_it) == 0 &&
          (gtk_text_iter_get_line_offset(&e_it) == 0 ||
           gtk_text_iter_is_end(&e_it)) &&
          !gtk_text_iter_equal(&s_it, &e_it)) {
        *bound = s_it;
        *ins = e_it;
        gtk_text_iter_forward_line(ins);
      } else {
        *bound = *ins;
        gtk_text_iter_set_line_offset(bound, 0);
        if (!gtk_text_iter_ends_line(ins))
          gtk_text_iter_forward_to_line_end(ins);
        gtk_text_iter_forward_char(ins);
      }
      gtk_text_buffer_select_range(tbuf, ins, bound);
      return BUF_ACT_SCROLL;
    }
    case IDE_ACTION_COLLAPSE_SELECTION:
      gtk_text_buffer_place_cursor(tbuf, ins);
      return BUF_ACT_DONE;
    case IDE_ACTION_MOVE_LINE_START:
      gtk_text_iter_set_line_offset(ins, 0);
      break;
    case IDE_ACTION_MOVE_LINE_END:
      if (!gtk_text_iter_ends_line(ins))
        gtk_text_iter_forward_to_line_end(ins);
      break;
    case IDE_ACTION_MOVE_BUFFER_START:
      gtk_text_buffer_get_start_iter(tbuf, ins);
      break;
    case IDE_ACTION_MOVE_BUFFER_END:
      gtk_text_buffer_get_end_iter(tbuf, ins);
      break;
    case IDE_ACTION_PAGE_DOWN:
      gtk_text_iter_forward_lines(ins, 18);
      break;
    case IDE_ACTION_PAGE_UP:
      gtk_text_iter_backward_lines(ins, 18);
      break;
    case IDE_ACTION_SELECT_TEXT_OBJECT: {
      g_autofree char *full = ide_buffer_get_text(buf);
      gsize cur_byte = 0, s_byte = 0, e_byte = 0;
      GtkTextIter start_it;
      gtk_text_buffer_get_start_iter(tbuf, &start_it);
      g_autofree char *prefix =
          gtk_text_buffer_get_text(tbuf, &start_it, ins, TRUE);
      cur_byte = strlen(prefix);
      gboolean inside = (act->arg1[0] == 'i');
      /* Syntax-tree objects first (f/c/b/a); text heuristics as fallback. */
      const IdeTsDoc *ts_doc = ide_highlight_get_ts_doc(ide_buffer_get_source_buffer(buf));
      gboolean found = ts_doc != NULL &&
                       strchr("fcba", act->arg2[0]) != NULL &&
                       ide_ts_doc_find_object(ts_doc, cur_byte, inside, act->arg2[0],
                                              &s_byte, &e_byte);
      if (!found) {
        found = ide_find_text_object(full, cur_byte, inside, act->arg2[0], &s_byte, &e_byte);
      }
      if (found) {
        glong s_char = g_utf8_pointer_to_offset(full, full + s_byte);
        glong e_char = g_utf8_pointer_to_offset(full, full + e_byte);
        GtkTextIter sit, eit;
        gtk_text_buffer_get_iter_at_offset(tbuf, &sit, (int)s_char);
        gtk_text_buffer_get_iter_at_offset(tbuf, &eit, (int)e_char);
        gtk_text_buffer_select_range(tbuf, &eit, &sit);
      }
      return BUF_ACT_DONE;
    }
    default:
      return BUF_ACT_UNHANDLED;
  }

  if (extend)
    gtk_text_buffer_select_range(tbuf, ins, bound);
  else
    gtk_text_buffer_place_cursor(tbuf, ins);
  return BUF_ACT_SCROLL;
}

static void execute_indent_or_outdent(KajoIdeWindow *self,
                                      GtkTextBuffer *tbuf,
                                      GtkTextIter   *ins,
                                      gboolean       is_indent) {
  guint tab_w = CLAMP(
      gtk_source_view_get_tab_width(GTK_SOURCE_VIEW(self->source_view)), 1, 8);
  gboolean use_spaces = gtk_source_view_get_insert_spaces_instead_of_tabs(
      GTK_SOURCE_VIEW(self->source_view));
  g_autofree char *indent_str =
      use_spaces ? g_strnfill(tab_w, ' ') : g_strdup("\t");
  GtkTextIter s_it, e_it;
  if (!gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it)) {
    s_it = *ins;
    e_it = *ins;
  }
  int s_line = gtk_text_iter_get_line(&s_it);
  int e_line = gtk_text_iter_get_line(&e_it);
  if (e_line > s_line && gtk_text_iter_get_line_offset(&e_it) == 0)
    e_line--;
  gtk_text_buffer_begin_user_action(tbuf);
  for (int l = s_line; l <= e_line; l++) {
    GtkTextIter lit;
    gtk_text_buffer_get_iter_at_line_offset(tbuf, &lit, l, 0);
    if (is_indent) {
      gtk_text_buffer_insert(tbuf, &lit, indent_str, -1);
    } else {
      for (guint k = 0; k < tab_w; k++) {
        gunichar ch = gtk_text_iter_get_char(&lit);
        if (ch == ' ' || ch == '\t') {
          GtkTextIter next = lit;
          gtk_text_iter_forward_char(&next);
          gtk_text_buffer_delete(tbuf, &lit, &next);
          if (ch == '\t') break;
        } else {
          break;
        }
      }
    }
  }
  gtk_text_buffer_end_user_action(tbuf);
}

static void execute_paste_action(KajoIdeWindow *self,
                                 GtkTextBuffer *tbuf,
                                 GtkTextIter   *ins,
                                 GtkTextIter   *bound,
                                 gboolean       paste_after) {
  gtk_text_buffer_begin_user_action(tbuf);
  gboolean had_sel = gtk_text_buffer_get_selection_bounds(tbuf, ins, bound);
  if (had_sel)
    gtk_text_buffer_delete(tbuf, ins, bound);
  if (self->yank_register && *self->yank_register) {
    if (!had_sel) {
      gboolean linewise = g_str_has_suffix(self->yank_register, "\n");
      if (paste_after) {
        if (linewise) {
          if (!gtk_text_iter_ends_line(ins))
            gtk_text_iter_forward_to_line_end(ins);
          if (gtk_text_iter_forward_line(ins)) {
            gtk_text_buffer_place_cursor(tbuf, ins);
            gtk_text_buffer_insert_at_cursor(tbuf, self->yank_register, -1);
          } else {
            gtk_text_buffer_place_cursor(tbuf, ins);
            gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
            gsize ylen = strlen(self->yank_register);
            gtk_text_buffer_insert_at_cursor(
                tbuf, self->yank_register, (int)(ylen - 1));
          }
          gtk_text_buffer_end_user_action(tbuf);
          return;
        } else if (!gtk_text_iter_ends_line(ins)) {
          gtk_text_iter_forward_cursor_position(ins);
          gtk_text_buffer_place_cursor(tbuf, ins);
        }
      } else if (linewise) {
        gtk_text_iter_set_line_offset(ins, 0);
        gtk_text_buffer_place_cursor(tbuf, ins);
      }
    }
    gtk_text_buffer_insert_at_cursor(tbuf, self->yank_register, -1);
  } else {
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    if (cb) gtk_text_buffer_paste_clipboard(tbuf, cb, NULL, TRUE);
  }
  gtk_text_buffer_end_user_action(tbuf);
}

static BufferActionOutcome execute_buffer_edit(KajoIdeWindow   *self,
                                               GtkTextBuffer   *tbuf,
                                               GtkTextIter     *ins,
                                               GtkTextIter     *bound,
                                               const IdeAction *act) {
  switch (act->type) {
    case IDE_ACTION_INSERT_CHAR:
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound))
        gtk_text_buffer_delete(tbuf, ins, bound);
      gtk_text_buffer_insert_at_cursor(tbuf, act->str, -1);
      gtk_text_buffer_end_user_action(tbuf);
      return BUF_ACT_POST_EDIT;

    case IDE_ACTION_INSERT_NEWLINE:
      gtk_text_buffer_begin_user_action(tbuf);
      if (g_strcmp0(act->str, "below") == 0) {
        if (!gtk_text_iter_ends_line(ins))
          gtk_text_iter_forward_to_line_end(ins);
        gtk_text_buffer_place_cursor(tbuf, ins);
        gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
      } else if (g_strcmp0(act->str, "above") == 0) {
        int line = gtk_text_iter_get_line(ins);
        gtk_text_iter_set_line_offset(ins, 0);
        gtk_text_buffer_insert(tbuf, ins, "\n", 1);
        gtk_text_buffer_get_iter_at_line_offset(tbuf, ins, line, 0);
        gtk_text_buffer_place_cursor(tbuf, ins);
      } else {
        if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound))
          gtk_text_buffer_delete(tbuf, ins, bound);
        gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
      }
      gtk_text_buffer_end_user_action(tbuf);
      return BUF_ACT_POST_EDIT;

    case IDE_ACTION_INSERT_TAB: {
      guint tab_w = CLAMP(
          gtk_source_view_get_tab_width(GTK_SOURCE_VIEW(self->source_view)), 1, 8);
      gboolean use_spaces = gtk_source_view_get_insert_spaces_instead_of_tabs(
          GTK_SOURCE_VIEW(self->source_view));
      g_autofree char *tab_str =
          use_spaces ? g_strnfill(tab_w, ' ') : g_strdup("\t");
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound))
        gtk_text_buffer_delete(tbuf, ins, bound);
      gtk_text_buffer_insert_at_cursor(tbuf, tab_str, -1);
      gtk_text_buffer_end_user_action(tbuf);
      return BUF_ACT_POST_EDIT;
    }

    case IDE_ACTION_INDENT:
    case IDE_ACTION_OUTDENT:
      execute_indent_or_outdent(self, tbuf, ins, act->type == IDE_ACTION_INDENT);
      return BUF_ACT_POST_EDIT;

    case IDE_ACTION_BACKSPACE:
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound))
        gtk_text_buffer_delete(tbuf, ins, bound);
      else
        gtk_text_buffer_backspace(tbuf, ins, TRUE, TRUE);
      gtk_text_buffer_end_user_action(tbuf);
      return BUF_ACT_POST_EDIT;

    case IDE_ACTION_DELETE_CHAR:
    case IDE_ACTION_DELETE_SELECTION:
    case IDE_ACTION_CHANGE_SELECTION:
      gtk_text_buffer_begin_user_action(tbuf);
      if (!gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
        *bound = *ins;
        gtk_text_iter_forward_cursor_position(bound);
      }
      g_free(self->yank_register);
      self->yank_register = gtk_text_buffer_get_text(tbuf, ins, bound, TRUE);
      {
        GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
        if (cb && self->yank_register)
          gdk_clipboard_set_text(cb, self->yank_register);
      }
      gtk_text_buffer_delete(tbuf, ins, bound);
      gtk_text_buffer_end_user_action(tbuf);
      return BUF_ACT_POST_EDIT;

    case IDE_ACTION_YANK:
      if (!gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
        gtk_text_iter_set_line_offset(ins, 0);
        *bound = *ins;
        gtk_text_iter_forward_line(bound);
      }
      g_free(self->yank_register);
      self->yank_register = gtk_text_buffer_get_text(tbuf, ins, bound, TRUE);
      {
        GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
        if (cb) gdk_clipboard_set_text(cb, self->yank_register);
      }
      ide_window_show_toast(self, "Yanked selection to clipboard");
      return BUF_ACT_DONE;

    case IDE_ACTION_PASTE_AFTER:
    case IDE_ACTION_PASTE_BEFORE:
      execute_paste_action(
          self, tbuf, ins, bound, act->type == IDE_ACTION_PASTE_AFTER);
      return BUF_ACT_POST_EDIT;

    case IDE_ACTION_UNDO:
      if (gtk_text_buffer_get_can_undo(tbuf)) {
        gtk_text_buffer_undo(tbuf);
        ide_window_show_toast(self, "↶ Undo");
      }
      return BUF_ACT_POST_EDIT;

    case IDE_ACTION_REDO:
      if (gtk_text_buffer_get_can_redo(tbuf)) {
        gtk_text_buffer_redo(tbuf);
        ide_window_show_toast(self, "↷ Redo");
      }
      return BUF_ACT_POST_EDIT;

    default:
      return BUF_ACT_UNHANDLED;
  }
}

static void execute_buffer_motion_or_edit(KajoIdeWindow   *self,
                                          const IdeAction *act) {
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter ins, bound;
  gtk_text_buffer_get_iter_at_mark(tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
  gtk_text_buffer_get_iter_at_mark(
      tbuf, &bound, gtk_text_buffer_get_selection_bound(tbuf));
  gboolean was_modified = ide_buffer_get_modified(buf);

  BufferActionOutcome res = execute_buffer_motion(self, buf, tbuf, &ins, &bound, act);
  if (res == BUF_ACT_UNHANDLED)
    res = execute_buffer_edit(self, tbuf, &ins, &bound, act);
  if (res == BUF_ACT_UNHANDLED) return;

  if (res == BUF_ACT_SCROLL) {
    GtkWidget *av = ide_window_get_active_source_view(self);
    if (av)
      gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(av),
                                         gtk_text_buffer_get_insert(tbuf));
  } else if (res == BUF_ACT_POST_EDIT) {
    ide_buffer_recompute_diffs(buf);
    ide_lsp_manager_notify_change(self->lsp, buf);
    if (self->strip_view) ide_strip_view_notify_buffer_changed(self->strip_view, buf);
    ide_window_schedule_markdown_sync(self);
    if (ide_buffer_get_modified(buf) != was_modified)
      ide_window_rebuild_tab_ribbon(self);
    GtkWidget *av = ide_window_get_active_source_view(self);
    if (av) {
      gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(av),
                                         gtk_text_buffer_get_insert(tbuf));
      gtk_widget_queue_draw(av);
    }
  }

  ide_window_update_status_bar_and_hud(self);
}

/* ─── 2. Per-Domain Action Dispatchers ─── */

static gboolean dispatch_modal_input_intercept(KajoIdeWindow   *self,
                                               IdeState         cur_st,
                                               const IdeAction *act) {
  /* The LSP rename prompt owns text input while it is open. */
  if (ide_lsp_manager_rename_active(self->lsp)) {
    if (act->type == IDE_ACTION_INSERT_CHAR) {
      ide_lsp_manager_rename_input(self->lsp, act->str);
      return TRUE;
    }
    if (act->type == IDE_ACTION_BACKSPACE) {
      ide_lsp_manager_rename_backspace(self->lsp);
      return TRUE;
    }
  }

  /* Route text input & navigation when inside editable Tree prompt or Drawer */
  if (cur_st == IDE_STATE_TREE && ide_fsm_get_tree_editable(self->fsm)) {
    if (act->type == IDE_ACTION_INSERT_CHAR) {
      ide_tree_prompt_append_char(self->tree, act->str);
      return TRUE;
    }
    if (act->type == IDE_ACTION_BACKSPACE) {
      ide_tree_prompt_backspace(self->tree);
      return TRUE;
    }
  } else if (cur_st == IDE_STATE_DRAWER) {
    if (act->type == IDE_ACTION_MOVE_DOWN) {
      ide_search_drawer_move_down(self->search_drawer);
      return TRUE;
    }
    if (act->type == IDE_ACTION_MOVE_UP) {
      ide_search_drawer_move_up(self->search_drawer);
      return TRUE;
    }
    if (ide_fsm_get_drawer_editable(self->fsm)) {
      if (act->type == IDE_ACTION_INSERT_CHAR) {
        ide_search_drawer_append_char(self->search_drawer, act->str);
        return TRUE;
      }
      if (act->type == IDE_ACTION_BACKSPACE) {
        ide_search_drawer_backspace(self->search_drawer);
        return TRUE;
      }
    }
  }
  return FALSE;
}

static gboolean dispatch_layout_drawer_action(KajoIdeWindow   *self,
                                              IdeState         cur_st,
                                              IdeBuffer       *buf,
                                              guint            cur_line,
                                              guint            cur_col,
                                              const IdeAction *act) {
  switch (act->type) {
    case IDE_ACTION_TOGGLE_FILE_TREE:
      if (cur_st == IDE_STATE_TREE) {
        gtk_widget_set_visible(self->tree_container, TRUE);
      } else {
        gtk_widget_set_visible(self->tree_container,
                               !gtk_widget_get_visible(self->tree_container));
        gtk_widget_grab_focus(self->source_view);
      }
      return TRUE;
    case IDE_ACTION_PANE_FOCUS_TREE:
      gtk_widget_set_visible(self->tree_container, TRUE);
      return TRUE;
    case IDE_ACTION_PANE_FOCUS_BUFFER:
      if (ide_fsm_tree_prompt_active(self->fsm, ide_tree_prompt_is_visible(self->tree))) {
        ide_tree_prompt_cancel(self->tree);
        ide_fsm_set_tree_editable(self->fsm, FALSE);
      }
      gtk_widget_grab_focus(self->source_view);
      return TRUE;
    case IDE_ACTION_PANE_FOCUS_DRAWER:
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), TRUE);
      return TRUE;
    case IDE_ACTION_PANE_CYCLE:
      if (cur_st == IDE_STATE_TREE) {
        if (ide_fsm_tree_prompt_active(self->fsm, ide_tree_prompt_is_visible(self->tree))) {
          ide_tree_prompt_cancel(self->tree);
          ide_fsm_set_tree_editable(self->fsm, FALSE);
        }
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)))
          ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
        else
          ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      } else if (cur_st == IDE_STATE_DRAWER) {
        ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      } else {
        if (gtk_widget_get_visible(self->tree_container))
          ide_fsm_set_state(self->fsm, IDE_STATE_TREE);
        else if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)))
          ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
      }
      gtk_widget_grab_focus(self->source_view);
      return TRUE;
    case IDE_ACTION_TOGGLE_DRAWER:
      if (!gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer))) {
        ide_window_show_drawer_page(self, "search");
      } else if (cur_st != IDE_STATE_DRAWER) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);
      }
      return TRUE;
    case IDE_ACTION_DRAWER_CLOSE:
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);
      gtk_widget_grab_focus(self->source_view);
      return TRUE;
    case IDE_ACTION_DRAWER_MAXIMIZE:
      if (self->vertical_paned) {
        if (!self->drawer_maximized) {
          self->saved_drawer_pos =
              gtk_paned_get_position(GTK_PANED(self->vertical_paned));
          gtk_paned_set_position(GTK_PANED(self->vertical_paned), 120);
          self->drawer_maximized = TRUE;
        } else {
          gtk_paned_set_position(
              GTK_PANED(self->vertical_paned),
              self->saved_drawer_pos > 120 ? self->saved_drawer_pos : 480);
          self->drawer_maximized = FALSE;
        }
      }
      return TRUE;
    case IDE_ACTION_DRAWER_JUMP:
      ide_search_drawer_jump_selected(self->search_drawer);
      gtk_widget_grab_focus(self->source_view);
      return TRUE;
    case IDE_ACTION_RESIZE_TREE_GROW:
      if (self->tree_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->tree_paned));
        gtk_paned_set_position(GTK_PANED(self->tree_paned), CLAMP(pos + 24, 150, 520));
      }
      return TRUE;
    case IDE_ACTION_RESIZE_TREE_SHRINK:
      if (self->tree_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->tree_paned));
        gtk_paned_set_position(GTK_PANED(self->tree_paned), CLAMP(pos - 24, 150, 520));
      }
      return TRUE;
    case IDE_ACTION_RESIZE_DRAWER_GROW:
      if (self->vertical_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->vertical_paned));
        gtk_paned_set_position(GTK_PANED(self->vertical_paned), MAX(200, pos - 30));
      }
      return TRUE;
    case IDE_ACTION_RESIZE_DRAWER_SHRINK:
      if (self->vertical_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->vertical_paned));
        gtk_paned_set_position(GTK_PANED(self->vertical_paned), pos + 30);
      }
      return TRUE;
    case IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW: {
      gboolean next_vis = !gtk_widget_get_visible(self->md_split_container);
      gtk_widget_set_visible(self->md_split_container, next_vis);
      gtk_widget_set_visible(self->source_map, !next_vis && self->config_minimap_enabled);
      if (next_vis && self->editor_md_paned) {
        int avail_w = gtk_widget_get_width(self->editor_md_paned);
        if (avail_w <= 200) avail_w = 1040;
        gtk_paned_set_position(GTK_PANED(self->editor_md_paned), avail_w / 2);
      }
      ide_window_sync_markdown_views(self);
      gtk_widget_grab_focus(self->source_view);
      return TRUE;
    }
    case IDE_ACTION_TOGGLE_MERMAID_MODE:
      ide_window_show_drawer_page(self, "markdown");
      ide_window_sync_markdown_views(self);
      return TRUE;
    case IDE_ACTION_TOGGLE_CHECKLIST:
      if (buf) {
        g_autofree char *txt = ide_buffer_get_text(buf);
        g_autofree char *toggled =
            ide_markdown_toggle_checklist_line(txt, cur_line);
        ide_buffer_set_text(buf, toggled);
        ide_window_move_cursor_to_line_col(self, cur_line, cur_col);
        ide_window_sync_markdown_views(self);
      }
      return TRUE;
    case IDE_ACTION_TOGGLE_RELATIVE_LINES:
      ide_window_toggle_relative_lines(self);
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean dispatch_picker_and_tree_action(KajoIdeWindow   *self,
                                                const IdeAction *act) {
  switch (act->type) {
    /* Fuzzy Picker & Help */
    case IDE_ACTION_PICKER_OPEN_FILES:
      ide_picker_open_files(self->picker, self->workspace_root);
      return TRUE;
    case IDE_ACTION_PICKER_OPEN_BUFFERS:
      ide_picker_open_buffers(self->picker, self->buffers);
      return TRUE;
    case IDE_ACTION_PICKER_OPEN_GREP:
      ide_picker_open_grep(self->picker, self->workspace_root);
      return TRUE;
    case IDE_ACTION_HELP_OPEN:
      ide_fsm_set_state(self->fsm, IDE_STATE_HELP);
      ide_picker_open_help(self->picker);
      return TRUE;
    case IDE_ACTION_HELP_TOGGLE:
      if (ide_picker_is_visible(self->picker) &&
          ide_picker_get_mode(self->picker) == IDE_PICKER_HELP) {
        ide_picker_close(self->picker);
        ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      } else {
        ide_fsm_set_state(self->fsm, IDE_STATE_HELP);
        ide_picker_open_help(self->picker);
      }
      return TRUE;
    case IDE_ACTION_PICKER_NEXT:
    case IDE_ACTION_HELP_NEXT:
      ide_picker_move_next(self->picker);
      return TRUE;
    case IDE_ACTION_PICKER_PREV:
    case IDE_ACTION_HELP_PREV:
      ide_picker_move_prev(self->picker);
      return TRUE;
    case IDE_ACTION_HELP_PAGE_DOWN:
      ide_picker_page_down(self->picker);
      return TRUE;
    case IDE_ACTION_HELP_PAGE_UP:
      ide_picker_page_up(self->picker);
      return TRUE;
    case IDE_ACTION_PICKER_INPUT:
    case IDE_ACTION_HELP_INPUT:
      ide_picker_append_input(self->picker, act->str);
      return TRUE;
    case IDE_ACTION_PICKER_BACKSPACE:
    case IDE_ACTION_HELP_BACKSPACE:
      ide_picker_backspace(self->picker);
      return TRUE;
    case IDE_ACTION_PICKER_SELECT:
    case IDE_ACTION_HELP_SELECT:
      ide_picker_confirm(self->picker);
      gtk_widget_grab_focus(self->source_view);
      return TRUE;
    case IDE_ACTION_PICKER_CANCEL:
    case IDE_ACTION_HELP_CANCEL:
      ide_picker_close(self->picker);
      gtk_widget_grab_focus(self->source_view);
      return TRUE;

    /* File Tree Navigation & CRUD */
    case IDE_ACTION_TREE_UP:
      ide_tree_move_up(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_DOWN:
      ide_tree_move_down(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_PAGE_UP:
      ide_tree_page_up(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_PAGE_DOWN:
      ide_tree_page_down(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_TOP:
      ide_tree_move_top(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_BOTTOM:
      ide_tree_move_bottom(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_TOGGLE_OR_OPEN:
      if (ide_fsm_get_tree_editable(self->fsm)) {
        ide_tree_prompt_confirm(self->tree);
        ide_fsm_set_tree_editable(self->fsm, FALSE);
      } else {
        ide_tree_toggle_or_open(self->tree);
      }
      return TRUE;
    case IDE_ACTION_TREE_COLLAPSE_OR_PARENT:
      ide_tree_collapse_or_parent(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_REFRESH:
      ide_tree_refresh(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_CREATE_FILE:
      gtk_widget_set_visible(self->tree_container, TRUE);
      ide_fsm_set_state(self->fsm, IDE_STATE_TREE);
      ide_fsm_set_tree_editable(self->fsm, TRUE);
      ide_tree_prompt_create_file(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_CREATE_DIR:
      gtk_widget_set_visible(self->tree_container, TRUE);
      ide_fsm_set_state(self->fsm, IDE_STATE_TREE);
      ide_fsm_set_tree_editable(self->fsm, TRUE);
      ide_tree_prompt_create_dir(self->tree);
      return TRUE;
    case IDE_ACTION_TREE_DELETE:
      ide_tree_delete_selected(self->tree);
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean dispatch_file_buffer_action(KajoIdeWindow   *self,
                                            IdeBuffer       *buf,
                                            const IdeAction *act) {
  switch (act->type) {
    case IDE_ACTION_TAB_NEXT:
    case IDE_ACTION_STEP_FORWARD:
      if (self->buffers->len > 1) {
        self->active_buf_idx = (self->active_buf_idx + 1) % self->buffers->len;
        ide_window_bind_active_buffer_to_view(self);
      }
      return TRUE;
    case IDE_ACTION_TAB_PREV:
    case IDE_ACTION_STEP_BACKWARD:
      if (self->buffers->len > 1) {
        self->active_buf_idx =
            (self->active_buf_idx + self->buffers->len - 1) % self->buffers->len;
        ide_window_bind_active_buffer_to_view(self);
      }
      return TRUE;
    case IDE_ACTION_TAB_SWITCH:
      if (act->int_val >= 1 && (guint)act->int_val <= self->buffers->len) {
        self->active_buf_idx = (guint)(act->int_val - 1);
        ide_window_bind_active_buffer_to_view(self);
      }
      return TRUE;
    case IDE_ACTION_SAVE:
      if (buf && ide_buffer_save(buf, NULL)) {
        ide_lsp_manager_notify_save(self->lsp, buf);
        if (self->strip_view) ide_strip_view_notify_buffer_saved(self->strip_view, buf);
        ide_tree_refresh(self->tree);
        ide_window_sync_conflict_bar(self);
        ide_window_rebuild_tab_ribbon(self);
        ide_window_show_toast(self, "✓ Saved file atomically");
      }
      return TRUE;
    case IDE_ACTION_SAVE_ALL:
      for (guint i = 0; i < self->buffers->len; i++) {
        IdeBuffer *b = g_ptr_array_index(self->buffers, i);
        if (ide_buffer_get_file_path(b) && ide_buffer_save(b, NULL)) {
          ide_lsp_manager_notify_save(self->lsp, b);
          if (self->strip_view) ide_strip_view_notify_buffer_saved(self->strip_view, b);
        }
      }
      ide_tree_refresh(self->tree);
      ide_window_sync_conflict_bar(self);
      ide_window_rebuild_tab_ribbon(self);
      ide_window_show_toast(self, "✓ Saved all open buffers");
      return TRUE;
    case IDE_ACTION_BUFFER_CLOSE:
    case IDE_ACTION_BUFFER_FORCE_CLOSE:
      if (self->buffers->len > 1) {
        IdeBuffer *closing = g_ptr_array_index(self->buffers, self->active_buf_idx);
        if (closing) {
          GtkSourceBuffer *old_sb = ide_buffer_get_source_buffer(closing);
          if (old_sb)
            g_signal_handlers_disconnect_by_data(old_sb, self);
          if (self->strip_view) ide_strip_view_notify_buffer_closed(self->strip_view, closing);
        }
        g_ptr_array_remove_index(self->buffers, self->active_buf_idx);
        if (self->active_buf_idx >= self->buffers->len)
          self->active_buf_idx = self->buffers->len - 1;
        ide_window_bind_active_buffer_to_view(self);
      }
      return TRUE;
    case IDE_ACTION_ALTERNATE_FILE:
      if (buf && ide_buffer_get_file_path(buf)) {
        g_autofree char *alt =
            ide_resolve_alternate_path(ide_buffer_get_file_path(buf));
        if (alt) kajo_ide_window_open_path(self, alt, 1, 1);
      }
      return TRUE;
    case IDE_ACTION_JUMPLIST_BACK:
      if (self->jumplist_idx > 0) {
        self->jumplist_idx--;
        IdeJumpEntry *e =
            g_ptr_array_index(self->jumplist, self->jumplist_idx);
        kajo_ide_window_open_path(self, e->path, e->line_1based, e->col_1based);
      }
      return TRUE;
    case IDE_ACTION_JUMPLIST_FORWARD:
      if (self->jumplist_idx + 1 < (int)self->jumplist->len) {
        self->jumplist_idx++;
        IdeJumpEntry *e =
            g_ptr_array_index(self->jumplist, self->jumplist_idx);
        kajo_ide_window_open_path(self, e->path, e->line_1based, e->col_1based);
      }
      return TRUE;
    case IDE_ACTION_TASK_BUILD:
    case IDE_ACTION_TASK_TEST:
    case IDE_ACTION_TASK_RUN:
    case IDE_ACTION_TASK_CLEAN: {
      int kind = act->type == IDE_ACTION_TASK_BUILD ? 0
                 : act->type == IDE_ACTION_TASK_TEST ? 1
                 : act->type == IDE_ACTION_TASK_RUN ? 2 : 3;
      /* save modified buffers first so the build sees the editor's state */
      for (guint i = 0; i < self->buffers->len; i++) {
        IdeBuffer *b = g_ptr_array_index(self->buffers, i);
        if (ide_buffer_get_file_path(b) && ide_buffer_get_modified(b)) ide_buffer_save(b, NULL);
      }
      ide_window_show_drawer_page(self, "search");
      ide_search_drawer_run_task(self->search_drawer, kind,
                                 buf ? ide_buffer_get_file_path(buf) : NULL);
      return TRUE;
    }
    default:
      return FALSE;
  }
}

static gboolean dispatch_lsp_action(KajoIdeWindow   *self,
                                    IdeState         cur_st,
                                    IdeBuffer       *buf,
                                    guint            cur_line,
                                    guint            cur_col,
                                    const IdeAction *act) {
  switch (act->type) {
    case IDE_ACTION_LSP_HOVER:
      if (buf)
        ide_lsp_manager_request_hover(
            self->lsp, ide_window_get_active_source_view(self), buf, cur_line, cur_col);
      return TRUE;
    case IDE_ACTION_LSP_COMPLETE:
      if (buf)
        ide_lsp_manager_request_completion(
            self->lsp, ide_window_get_active_source_view(self), buf, cur_line, cur_col);
      return TRUE;
    case IDE_ACTION_LSP_DEFINITION:
      if (buf)
        ide_lsp_manager_request_definition(self->lsp, buf, cur_line, cur_col);
      return TRUE;
    case IDE_ACTION_LSP_IMPLEMENTATION:
      if (buf) ide_lsp_manager_request_implementation(self->lsp, buf);
      return TRUE;
    case IDE_ACTION_LSP_REFERENCES:
      if (buf)
        ide_lsp_manager_request_references(
            self->lsp, ide_window_get_active_source_view(self), buf, cur_line, cur_col);
      return TRUE;
    case IDE_ACTION_LSP_SIGNATURE_HELP:
      if (buf)
        ide_lsp_manager_request_signature_help(
            self->lsp, ide_window_get_active_source_view(self), buf, cur_line, cur_col);
      return TRUE;
    case IDE_ACTION_LSP_CALL_HIERARCHY_IN:
      if (buf)
        ide_lsp_manager_request_call_hierarchy(
            self->lsp, ide_window_get_active_source_view(self), buf, FALSE);
      return TRUE;
    case IDE_ACTION_LSP_CALL_HIERARCHY_OUT:
      if (buf)
        ide_lsp_manager_request_call_hierarchy(
            self->lsp, ide_window_get_active_source_view(self), buf, TRUE);
      return TRUE;
    case IDE_ACTION_LSP_CALL_HIERARCHY_TOGGLE:
      ide_lsp_manager_toggle_call_hierarchy_dir(self->lsp);
      return TRUE;
    case IDE_ACTION_LSP_CODE_ACTION:
      if (buf) ide_lsp_manager_request_code_actions(self->lsp, ide_window_get_active_source_view(self), buf);
      return TRUE;
    case IDE_ACTION_LSP_RENAME:
      if (buf) ide_lsp_manager_rename_begin(self->lsp, ide_window_get_active_source_view(self), buf);
      return TRUE;
    case IDE_ACTION_LSP_FORMAT:
      if (buf) {
        guint tab_w = CLAMP(
            gtk_source_view_get_tab_width(GTK_SOURCE_VIEW(self->source_view)), 1, 8);
        gboolean use_spaces = gtk_source_view_get_insert_spaces_instead_of_tabs(
            GTK_SOURCE_VIEW(self->source_view));
        ide_lsp_manager_request_format_with_options(
            self->lsp, buf, (int)tab_w, use_spaces);
      }
      return TRUE;
    case IDE_ACTION_LSP_CANCEL:
      if (ide_fsm_tree_prompt_active(self->fsm, ide_tree_prompt_is_visible(self->tree))) {
        ide_tree_prompt_cancel(self->tree);
        ide_fsm_set_tree_editable(self->fsm, FALSE);
      }
      ide_lsp_manager_cancel_popover(self->lsp);
      return TRUE;
    case IDE_ACTION_LSP_SELECT_NEXT:
      ide_lsp_manager_select_next(self->lsp);
      return TRUE;
    case IDE_ACTION_LSP_SELECT_PREV:
      ide_lsp_manager_select_prev(self->lsp);
      return TRUE;
    case IDE_ACTION_LSP_SELECT_CONFIRM:
      if (cur_st == IDE_STATE_TREE &&
          ide_fsm_tree_prompt_active(self->fsm, ide_tree_prompt_is_visible(self->tree))) {
        if (!ide_tree_prompt_confirm(self->tree)) {
          ide_window_show_toast(self, "Tree create cancelled (invalid path or file already exists)");
        }
        ide_fsm_set_tree_editable(self->fsm, FALSE);
      } else if (ide_lsp_manager_rename_active(self->lsp)) {
        if (buf) ide_lsp_manager_rename_confirm(self->lsp, buf);
      } else if (buf) {
        if (act->int_val > 0) {
          if (!ide_lsp_manager_select_index(self->lsp, act->int_val - 1)) {
            ide_fsm_set_state(self->fsm, IDE_STATE_CODE_ACTION);
            return TRUE;
          }
        }
        ide_lsp_manager_confirm_selection(self->lsp, buf);
      }
      return TRUE;
    case IDE_ACTION_NEXT_DIAGNOSTIC:
    case IDE_ACTION_PREV_DIAGNOSTIC:
      if (buf) {
        const GPtrArray *diags = ide_buffer_get_diagnostics(buf);
        if (diags && diags->len > 0) {
          const IdeDiagnostic *target = NULL;
          if (act->type == IDE_ACTION_NEXT_DIAGNOSTIC) {
            for (guint i = 0; i < diags->len; i++) {
              const IdeDiagnostic *d = g_ptr_array_index(diags, i);
              if (d->line > cur_line) {
                target = d;
                break;
              }
            }
            if (!target) target = g_ptr_array_index(diags, 0);
          } else {
            for (int i = (int)diags->len - 1; i >= 0; i--) {
              const IdeDiagnostic *d = g_ptr_array_index(diags, (guint)i);
              if (d->line < cur_line) {
                target = d;
                break;
              }
            }
            if (!target) target = g_ptr_array_index(diags, diags->len - 1);
          }
          if (target) {
            ide_window_move_cursor_to_line_col(self, target->line, target->col);
            ide_window_show_toast(self, target->message);
          }
        }
      }
      return TRUE;
    default:
      return FALSE;
  }
}

static void jump_diff_hunk(KajoIdeWindow *self, IdeBuffer *buf, guint cur_line, gboolean forward) {
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  guint total = (guint)MAX(1, gtk_text_buffer_get_line_count(tbuf));
  guint target_line = 0;
  if (forward) {
    guint l = cur_line;
    while (l <= total && ide_buffer_get_line_diff(buf, l) != IDE_DIFF_NONE)
      l++;
    for (; l <= total; l++) {
      if (ide_buffer_get_line_diff(buf, l) != IDE_DIFF_NONE) {
        target_line = l;
        break;
      }
    }
    if (target_line == 0) {
      for (l = 1; l <= total; l++) {
        if (ide_buffer_get_line_diff(buf, l) != IDE_DIFF_NONE) {
          target_line = l;
          break;
        }
      }
    }
  } else {
    int l = (int)cur_line;
    while (l >= 1 && ide_buffer_get_line_diff(buf, (guint)l) != IDE_DIFF_NONE)
      l--;
    for (; l >= 1; l--) {
      if (ide_buffer_get_line_diff(buf, (guint)l) != IDE_DIFF_NONE) {
        while (l > 1 &&
               ide_buffer_get_line_diff(buf, (guint)(l - 1)) != IDE_DIFF_NONE)
          l--;
        target_line = (guint)l;
        break;
      }
    }
    if (target_line == 0) {
      for (l = (int)total; l >= 1; l--) {
        if (ide_buffer_get_line_diff(buf, (guint)l) != IDE_DIFF_NONE) {
          while (l > 1 &&
                 ide_buffer_get_line_diff(buf, (guint)(l - 1)) != IDE_DIFF_NONE)
            l--;
          target_line = (guint)l;
          break;
        }
      }
    }
  }
  if (target_line > 0) {
    ide_window_move_cursor_to_line_col(self, target_line, 1);
  } else {
    ide_window_show_toast(self, "No diff hunks in buffer");
  }
}

static gboolean dispatch_search_replace_action(KajoIdeWindow   *self,
                                               IdeBuffer       *buf,
                                               guint            cur_line,
                                               const IdeAction *act) {
  switch (act->type) {
    case IDE_ACTION_SEARCH_OPEN:
      g_string_truncate(self->search_query, 0);
      return TRUE;
    case IDE_ACTION_SEARCH_INPUT:
      g_string_append(self->search_query, act->str);
      return TRUE;
    case IDE_ACTION_SEARCH_BACKSPACE:
      if (self->search_query->len > 0)
        g_string_truncate(self->search_query, self->search_query->len - 1);
      return TRUE;
    case IDE_ACTION_SEARCH_CLEAR_HIGHLIGHT:
      g_string_truncate(self->search_query, 0);
      if (buf) {
        GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
        GtkTextIter ins;
        gtk_text_buffer_get_iter_at_mark(
            tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
        gtk_text_buffer_place_cursor(tbuf, &ins);
      }
      return TRUE;
    case IDE_ACTION_SEARCH_CONFIRM:
    case IDE_ACTION_SEARCH_NEXT:
      if (buf && self->search_query->len > 0) {
        GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
        GtkTextIter ins, mstart, mend;
        gtk_text_buffer_get_iter_at_mark(
            tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
        gtk_text_iter_forward_char(&ins);
        if (!gtk_text_iter_forward_search(&ins,
                                          self->search_query->str,
                                          GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                          &mstart,
                                          &mend,
                                          NULL)) {
          GtkTextIter start_it;
          gtk_text_buffer_get_start_iter(tbuf, &start_it);
          if (!gtk_text_iter_forward_search(&start_it,
                                            self->search_query->str,
                                            GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                            &mstart,
                                            &mend,
                                            NULL)) {
            return TRUE;
          }
        }
        gtk_text_buffer_select_range(tbuf, &mend, &mstart);
        gtk_text_view_scroll_to_iter(
            GTK_TEXT_VIEW(self->source_view), &mstart, 0.2, FALSE, 0.0, 0.5);
      }
      return TRUE;
    case IDE_ACTION_SEARCH_PREV:
      if (buf && self->search_query->len > 0) {
        GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
        GtkTextIter s_it, e_it, mstart, mend;
        if (!gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it)) {
          gtk_text_buffer_get_iter_at_mark(
              tbuf, &s_it, gtk_text_buffer_get_insert(tbuf));
        }
        if (!gtk_text_iter_backward_search(&s_it,
                                           self->search_query->str,
                                           GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                           &mstart,
                                           &mend,
                                           NULL)) {
          GtkTextIter end_it;
          gtk_text_buffer_get_end_iter(tbuf, &end_it);
          if (!gtk_text_iter_backward_search(&end_it,
                                             self->search_query->str,
                                             GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                             &mstart,
                                             &mend,
                                             NULL)) {
            return TRUE;
          }
        }
        gtk_text_buffer_select_range(tbuf, &mend, &mstart);
        gtk_text_view_scroll_to_iter(
            GTK_TEXT_VIEW(self->source_view), &mstart, 0.2, FALSE, 0.0, 0.5);
      }
      return TRUE;
    case IDE_ACTION_REPLACE_OPEN:
      ide_window_show_drawer_page(self, "search");
      ide_search_drawer_open_replace(self->search_drawer, buf, FALSE);
      return TRUE;
    case IDE_ACTION_PROJECT_REPLACE_OPEN:
      ide_window_show_drawer_page(self, "search");
      ide_search_drawer_open_replace(self->search_drawer, buf, TRUE);
      return TRUE;
    case IDE_ACTION_REPLACE_NEXT_FIELD:
      ide_search_drawer_next_field(self->search_drawer);
      return TRUE;
    case IDE_ACTION_REPLACE_PREV_FIELD:
      ide_search_drawer_prev_field(self->search_drawer);
      return TRUE;
    case IDE_ACTION_REPLACE_TOGGLE_CASE:
      ide_search_drawer_toggle_case(self->search_drawer);
      return TRUE;
    case IDE_ACTION_REPLACE_TOGGLE_WORD:
      ide_search_drawer_toggle_word(self->search_drawer);
      return TRUE;
    case IDE_ACTION_REPLACE_TOGGLE_REGEX:
      ide_search_drawer_toggle_regex(self->search_drawer);
      return TRUE;
    case IDE_ACTION_REPLACE_APPLY_CURRENT:
      ide_search_drawer_apply_current(self->search_drawer, buf);
      return TRUE;
    case IDE_ACTION_REPLACE_APPLY_ALL:
      ide_search_drawer_apply_all(self->search_drawer, self->buffers);
      return TRUE;
    case IDE_ACTION_NEXT_HUNK:
    case IDE_ACTION_PREV_HUNK:
      jump_diff_hunk(self, buf, cur_line, act->type == IDE_ACTION_NEXT_HUNK);
      return TRUE;
    case IDE_ACTION_NEXT_ERROR:
      ide_search_drawer_step_error(self->search_drawer, 1);
      return TRUE;
    case IDE_ACTION_PREV_ERROR:
      ide_search_drawer_step_error(self->search_drawer, -1);
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean dispatch_dap_agent_action(KajoIdeWindow   *self,
                                          IdeBuffer       *buf,
                                          guint            cur_line,
                                          const IdeAction *act) {
  switch (act->type) {
    case IDE_ACTION_DAP_TOGGLE_BREAKPOINT:
      if (buf) {
        ide_buffer_toggle_breakpoint(buf, cur_line);
        ide_dap_view_breakpoints_changed(self->dap);
        gtk_widget_queue_draw(self->source_view);
      }
      return TRUE;
    case IDE_ACTION_DAP_CONDITIONAL_BREAKPOINT:
      if (buf) ide_window_open_cond_bp_bar(self, cur_line);
      return TRUE;
    case IDE_ACTION_DAP_TOGGLE_VIEW:
      ide_window_show_drawer_page(self, "dap");
      return TRUE;
    case IDE_ACTION_DAP_START:
      ide_window_show_drawer_page(self, "dap");
      ide_dap_view_start_session(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      return TRUE;
    case IDE_ACTION_DAP_CONTINUE:
      ide_dap_view_continue(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      return TRUE;
    case IDE_ACTION_DAP_PAUSE:
      ide_dap_view_pause(self->dap);
      gtk_widget_queue_draw(self->source_view);
      return TRUE;
    case IDE_ACTION_DAP_STEP_OVER:
      ide_dap_view_step_over(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      return TRUE;
    case IDE_ACTION_DAP_STEP_INTO:
      ide_dap_view_step_into(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      return TRUE;
    case IDE_ACTION_DAP_STEP_OUT:
      ide_dap_view_step_out(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      return TRUE;
    case IDE_ACTION_DAP_STOP:
      ide_dap_view_stop(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      return TRUE;
    case IDE_ACTION_AGENT_REVIEW_OPEN:
      ide_window_show_drawer_page(self, "agent");
      return TRUE;
    case IDE_ACTION_AGENT_ACCEPT_ALL:
      ide_agent_ipc_accept_all(self->agent_ipc);
      return TRUE;
    case IDE_ACTION_AGENT_REVERT_ALL:
      ide_agent_ipc_reject_all(self->agent_ipc);
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean dispatch_strip_action(KajoIdeWindow   *self,
                                      const IdeAction *act) {
  switch (act->type) {
    case IDE_ACTION_STRIP_TOGGLE:
      ide_window_toggle_strip_mode(self);
      return TRUE;
    case IDE_ACTION_STRIP_FOCUS_LEFT:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_focus_column(self->strip_view, -1);
      return TRUE;
    case IDE_ACTION_STRIP_FOCUS_RIGHT:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_focus_column(self->strip_view, 1);
      return TRUE;
    case IDE_ACTION_STRIP_STEP_DOWN:
      if (self->strip_view) {
        if (!ide_strip_view_is_active(self->strip_view)) ide_window_set_strip_mode(self, TRUE);
        ide_strip_view_step_d1(self->strip_view, 1);
      }
      return TRUE;
    case IDE_ACTION_STRIP_STEP_UP:
      if (self->strip_view) {
        if (!ide_strip_view_is_active(self->strip_view)) ide_window_set_strip_mode(self, TRUE);
        ide_strip_view_step_d1(self->strip_view, -1);
      }
      return TRUE;
    case IDE_ACTION_STRIP_PROMOTE:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_promote_active_d1(self->strip_view);
      return TRUE;
    case IDE_ACTION_STRIP_SURFACE:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_surface_one(self->strip_view);
      return TRUE;
    case IDE_ACTION_STRIP_TOGGLE_FULL_FILE:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_toggle_full_file(self->strip_view);
      return TRUE;
    case IDE_ACTION_STRIP_TOGGLE_ZEN:
      if (self->strip_view) {
        if (!ide_strip_view_is_active(self->strip_view)) ide_window_set_strip_mode(self, TRUE);
        ide_strip_view_toggle_zen(self->strip_view);
      }
      return TRUE;
    case IDE_ACTION_STRIP_TOGGLE_OVERVIEW:
      if (self->strip_view) {
        if (!ide_strip_view_is_active(self->strip_view)) ide_window_set_strip_mode(self, TRUE);
        ide_strip_view_toggle_overview(self->strip_view);
      }
      return TRUE;
    case IDE_ACTION_STRIP_PIN_TOGGLE:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_pin_active_d1(self->strip_view);
      return TRUE;
    case IDE_ACTION_STRIP_RESYNC:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_resync_d1(self->strip_view);
      return TRUE;
    case IDE_ACTION_STRIP_LENS_CALLS:
    case IDE_ACTION_STRIP_LENS_TYPES:
    case IDE_ACTION_STRIP_LENS_IMPACT:
    case IDE_ACTION_STRIP_LENS_DIFF:
      if (self->strip_view) {
        if (!ide_strip_view_is_active(self->strip_view)) ide_window_set_strip_mode(self, TRUE);
        IdeStripLens lens = act->type == IDE_ACTION_STRIP_LENS_CALLS ? IDE_STRIP_LENS_CALLS
                          : act->type == IDE_ACTION_STRIP_LENS_TYPES ? IDE_STRIP_LENS_TYPES
                          : act->type == IDE_ACTION_STRIP_LENS_IMPACT ? IDE_STRIP_LENS_IMPACT
                                                                      : IDE_STRIP_LENS_DIFF;
        ide_strip_view_set_lens(self->strip_view, lens);
      }
      return TRUE;
    case IDE_ACTION_STRIP_BRANCH_TRAIL:
      if (self->strip_view) {
        if (!ide_strip_view_is_active(self->strip_view)) ide_window_set_strip_mode(self, TRUE);
        ide_strip_view_branch_trail(self->strip_view);
      }
      return TRUE;
    case IDE_ACTION_STRIP_PREV_TRAIL:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_switch_trail(self->strip_view, -1);
      return TRUE;
    case IDE_ACTION_STRIP_NEXT_TRAIL:
      if (self->strip_view && ide_strip_view_is_active(self->strip_view))
        ide_strip_view_switch_trail(self->strip_view, 1);
      return TRUE;
    case IDE_ACTION_STRIP_FILTER:
      if (self->strip_view) {
        if (!ide_strip_view_is_active(self->strip_view)) ide_window_set_strip_mode(self, TRUE);
        ide_strip_view_focus_filter(self->strip_view);
      }
      return TRUE;
    default:
      return FALSE;
  }
}

void ide_window_dispatch_action(KajoIdeWindow *self, const IdeAction *act) {
  if (!self || !act || act->type == IDE_ACTION_NONE) {
    ide_window_update_status_bar_and_hud(self);
    return;
  }

  IdeState cur_st = ide_fsm_get_state(self->fsm);
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  guint cur_line = 1, cur_col = 1;
  ide_window_get_cursor_line_col(self, &cur_line, &cur_col);

  gboolean completion_was_open = ide_lsp_manager_completion_active(self->lsp);

  if (dispatch_modal_input_intercept(self, cur_st, act)) {
    ide_window_update_status_bar_and_hud(self);
    return;
  }

  gboolean handled =
      dispatch_layout_drawer_action(self, cur_st, buf, cur_line, cur_col, act) ||
      dispatch_picker_and_tree_action(self, act) ||
      dispatch_file_buffer_action(self, buf, act) ||
      dispatch_lsp_action(self, cur_st, buf, cur_line, cur_col, act) ||
      dispatch_search_replace_action(self, buf, cur_line, act) ||
      dispatch_dap_agent_action(self, buf, cur_line, act) ||
      dispatch_strip_action(self, act);

  if (!handled) {
    execute_buffer_motion_or_edit(self, act);
  }

  /* Completion follows typing: refresh while open, auto-trigger on '.', '::' ... */
  if (buf != NULL && (act->type == IDE_ACTION_INSERT_CHAR || act->type == IDE_ACTION_BACKSPACE)) {
    if (completion_was_open) {
      ide_lsp_manager_request_completion(self->lsp, self->source_view, buf, 0, 0);
    } else if (act->type == IDE_ACTION_INSERT_CHAR) {
      ide_lsp_manager_maybe_trigger_completion(self->lsp, self->source_view, buf, act->str);
    }
  }

  ide_window_update_status_bar_and_hud(self);
}
