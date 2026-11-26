#include "ide_editor_ops.h"
#include <string.h>

static void
move_vertical_clamp_col(GtkTextBuffer *tbuf, GtkTextIter *ins, int delta)
{
  int line = gtk_text_iter_get_line(ins);
  int col = gtk_text_iter_get_line_offset(ins);
  int target = line + delta;
  int total = gtk_text_buffer_get_line_count(tbuf);
  if (target < 0 || target >= total) {
    return;
  }

  gtk_text_buffer_get_iter_at_line_offset(tbuf, ins, target, 0);
  int max_c = gtk_text_iter_get_chars_in_line(ins);
  if (gtk_text_iter_ends_line(ins)) {
    max_c = 0;
  } else if (max_c > 0) {
    max_c--;
  }
  gtk_text_iter_set_line_offset(ins, MIN(col, max_c));
}

static IdeEditorOpOutcome
execute_motion(IdeEditorHost   *host,
               GtkTextBuffer   *tbuf,
               GtkTextIter     *ins,
               GtkTextIter     *bound,
               const IdeAction *act)
{
  gboolean extend = (host->fsm != NULL && ide_fsm_get_state(host->fsm) == IDE_STATE_SELECT) ||
                    act->type == IDE_ACTION_EXTEND_SELECTION;

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
      return IDE_EDITOR_OP_SCROLL;
    case IDE_ACTION_MOVE_WORD_BACKWARD:
      gtk_text_iter_backward_word_start(ins);
      break;
    case IDE_ACTION_SELECT_WORD_BACKWARD:
      if (gtk_text_iter_backward_word_start(ins)) {
        *bound = *ins;
        gtk_text_iter_forward_word_end(bound);
        gtk_text_buffer_select_range(tbuf, ins, bound);
      }
      return IDE_EDITOR_OP_SCROLL;
    case IDE_ACTION_SELECT_LINE: {
      GtkTextIter s_it, e_it;
      if (gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it) &&
          gtk_text_iter_get_line_offset(&s_it) == 0 &&
          (gtk_text_iter_get_line_offset(&e_it) == 0 || gtk_text_iter_is_end(&e_it)) &&
          !gtk_text_iter_equal(&s_it, &e_it)) {
        *bound = s_it;
        *ins = e_it;
        gtk_text_iter_forward_line(ins);
      } else {
        *bound = *ins;
        gtk_text_iter_set_line_offset(bound, 0);
        if (!gtk_text_iter_ends_line(ins)) {
          gtk_text_iter_forward_to_line_end(ins);
        }
        gtk_text_iter_forward_char(ins);
      }
      gtk_text_buffer_select_range(tbuf, ins, bound);
      return IDE_EDITOR_OP_SCROLL;
    }
    case IDE_ACTION_COLLAPSE_SELECTION:
      gtk_text_buffer_place_cursor(tbuf, ins);
      return IDE_EDITOR_OP_DONE;
    case IDE_ACTION_MOVE_LINE_START:
      gtk_text_iter_set_line_offset(ins, 0);
      break;
    case IDE_ACTION_MOVE_LINE_END:
      if (!gtk_text_iter_ends_line(ins)) {
        gtk_text_iter_forward_to_line_end(ins);
      }
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
      g_autofree char *full = ide_buffer_get_text(host->buffer);
      gsize cur_byte = 0, s_byte = 0, e_byte = 0;
      GtkTextIter start_it;
      gtk_text_buffer_get_start_iter(tbuf, &start_it);
      g_autofree char *prefix = gtk_text_buffer_get_text(tbuf, &start_it, ins, TRUE);
      cur_byte = strlen(prefix);
      gboolean inside = (act->arg1[0] == 'i');
      const IdeTsDoc *ts_doc =
          ide_highlight_get_ts_doc(ide_buffer_get_source_buffer(host->buffer));
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
      return IDE_EDITOR_OP_DONE;
    }
    default:
      return IDE_EDITOR_OP_UNHANDLED;
  }

  if (extend) {
    gtk_text_buffer_select_range(tbuf, ins, bound);
  } else {
    gtk_text_buffer_place_cursor(tbuf, ins);
  }
  return IDE_EDITOR_OP_SCROLL;
}

static void
execute_indent_or_outdent(IdeEditorHost *host,
                          GtkTextBuffer *tbuf,
                          GtkTextIter   *ins,
                          gboolean       is_indent)
{
  guint tab_w = host->source_view
                    ? CLAMP(gtk_source_view_get_tab_width(host->source_view), 1, 8)
                    : 4;
  gboolean use_spaces = host->source_view
                            ? gtk_source_view_get_insert_spaces_instead_of_tabs(host->source_view)
                            : TRUE;
  g_autofree char *indent_str = use_spaces ? g_strnfill(tab_w, ' ') : g_strdup("\t");
  GtkTextIter s_it, e_it;
  if (!gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it)) {
    s_it = *ins;
    e_it = *ins;
  }
  int s_line = gtk_text_iter_get_line(&s_it);
  int e_line = gtk_text_iter_get_line(&e_it);
  if (e_line > s_line && gtk_text_iter_get_line_offset(&e_it) == 0) {
    e_line--;
  }
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
          if (ch == '\t') {
            break;
          }
        } else {
          break;
        }
      }
    }
  }
  gtk_text_buffer_end_user_action(tbuf);
}

static void
execute_paste_action(IdeEditorHost *host,
                     GtkTextBuffer *tbuf,
                     GtkTextIter   *ins,
                     GtkTextIter   *bound,
                     gboolean       paste_after)
{
  const char *yank = (host->yank_register != NULL) ? *host->yank_register : NULL;
  gtk_text_buffer_begin_user_action(tbuf);
  gboolean had_sel = gtk_text_buffer_get_selection_bounds(tbuf, ins, bound);
  if (had_sel) {
    gtk_text_buffer_delete(tbuf, ins, bound);
  }
  if (yank != NULL && *yank != '\0') {
    if (!had_sel) {
      gboolean linewise = g_str_has_suffix(yank, "\n");
      if (paste_after) {
        if (linewise) {
          if (!gtk_text_iter_ends_line(ins)) {
            gtk_text_iter_forward_to_line_end(ins);
          }
          if (gtk_text_iter_forward_line(ins)) {
            gtk_text_buffer_place_cursor(tbuf, ins);
            gtk_text_buffer_insert_at_cursor(tbuf, yank, -1);
          } else {
            gtk_text_buffer_place_cursor(tbuf, ins);
            gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
            gsize ylen = strlen(yank);
            gtk_text_buffer_insert_at_cursor(tbuf, yank, (int)(ylen - 1));
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
    gtk_text_buffer_insert_at_cursor(tbuf, yank, -1);
  } else {
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    if (cb) {
      gtk_text_buffer_paste_clipboard(tbuf, cb, NULL, TRUE);
    }
  }
  gtk_text_buffer_end_user_action(tbuf);
}

static IdeEditorOpOutcome
execute_edit(IdeEditorHost   *host,
             GtkTextBuffer   *tbuf,
             GtkTextIter     *ins,
             GtkTextIter     *bound,
             const IdeAction *act,
             const char     **out_toast)
{
  switch (act->type) {
    case IDE_ACTION_INSERT_CHAR:
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
        gtk_text_buffer_delete(tbuf, ins, bound);
      }
      gtk_text_buffer_insert_at_cursor(tbuf, act->str, -1);
      gtk_text_buffer_end_user_action(tbuf);
      return IDE_EDITOR_OP_POST_EDIT;

    case IDE_ACTION_INSERT_NEWLINE:
      gtk_text_buffer_begin_user_action(tbuf);
      if (g_strcmp0(act->str, "below") == 0) {
        if (!gtk_text_iter_ends_line(ins)) {
          gtk_text_iter_forward_to_line_end(ins);
        }
        gtk_text_buffer_place_cursor(tbuf, ins);
        gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
      } else if (g_strcmp0(act->str, "above") == 0) {
        int line = gtk_text_iter_get_line(ins);
        gtk_text_iter_set_line_offset(ins, 0);
        gtk_text_buffer_insert(tbuf, ins, "\n", 1);
        gtk_text_buffer_get_iter_at_line_offset(tbuf, ins, line, 0);
        gtk_text_buffer_place_cursor(tbuf, ins);
      } else {
        if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
          gtk_text_buffer_delete(tbuf, ins, bound);
        }
        gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
      }
      gtk_text_buffer_end_user_action(tbuf);
      return IDE_EDITOR_OP_POST_EDIT;

    case IDE_ACTION_INSERT_TAB: {
      guint tab_w = host->source_view
                        ? CLAMP(gtk_source_view_get_tab_width(host->source_view), 1, 8)
                        : 4;
      gboolean use_spaces = host->source_view
                                ? gtk_source_view_get_insert_spaces_instead_of_tabs(host->source_view)
                                : TRUE;
      g_autofree char *tab_str = use_spaces ? g_strnfill(tab_w, ' ') : g_strdup("\t");
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
        gtk_text_buffer_delete(tbuf, ins, bound);
      }
      gtk_text_buffer_insert_at_cursor(tbuf, tab_str, -1);
      gtk_text_buffer_end_user_action(tbuf);
      return IDE_EDITOR_OP_POST_EDIT;
    }

    case IDE_ACTION_INDENT:
    case IDE_ACTION_OUTDENT:
      execute_indent_or_outdent(host, tbuf, ins, act->type == IDE_ACTION_INDENT);
      return IDE_EDITOR_OP_POST_EDIT;

    case IDE_ACTION_BACKSPACE:
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
        gtk_text_buffer_delete(tbuf, ins, bound);
      } else {
        gtk_text_buffer_backspace(tbuf, ins, TRUE, TRUE);
      }
      gtk_text_buffer_end_user_action(tbuf);
      return IDE_EDITOR_OP_POST_EDIT;

    case IDE_ACTION_DELETE_CHAR:
    case IDE_ACTION_DELETE_SELECTION:
    case IDE_ACTION_CHANGE_SELECTION:
      gtk_text_buffer_begin_user_action(tbuf);
      if (!gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
        *bound = *ins;
        gtk_text_iter_forward_cursor_position(bound);
      }
      {
        g_autofree char *yank_tmp = gtk_text_buffer_get_text(tbuf, ins, bound, TRUE);
        if (host->yank_register != NULL) {
          g_free(*host->yank_register);
          *host->yank_register = g_strdup(yank_tmp);
        }
        GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
        if (cb && yank_tmp) {
          gdk_clipboard_set_text(cb, yank_tmp);
        }
      }
      gtk_text_buffer_delete(tbuf, ins, bound);
      gtk_text_buffer_end_user_action(tbuf);
      return IDE_EDITOR_OP_POST_EDIT;

    case IDE_ACTION_YANK:
      if (!gtk_text_buffer_get_selection_bounds(tbuf, ins, bound)) {
        gtk_text_iter_set_line_offset(ins, 0);
        *bound = *ins;
        gtk_text_iter_forward_line(bound);
      }
      {
        g_autofree char *yank_tmp = gtk_text_buffer_get_text(tbuf, ins, bound, TRUE);
        if (host->yank_register != NULL) {
          g_free(*host->yank_register);
          *host->yank_register = g_strdup(yank_tmp);
        }
        GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
        if (cb && yank_tmp) {
          gdk_clipboard_set_text(cb, yank_tmp);
        }
      }
      if (out_toast) {
        *out_toast = "Yanked selection to clipboard";
      }
      return IDE_EDITOR_OP_DONE;

    case IDE_ACTION_PASTE_AFTER:
    case IDE_ACTION_PASTE_BEFORE:
      execute_paste_action(host, tbuf, ins, bound, act->type == IDE_ACTION_PASTE_AFTER);
      return IDE_EDITOR_OP_POST_EDIT;

    case IDE_ACTION_UNDO:
      if (gtk_text_buffer_get_can_undo(tbuf)) {
        gtk_text_buffer_undo(tbuf);
        if (out_toast) {
          *out_toast = "↶ Undo";
        }
      }
      return IDE_EDITOR_OP_POST_EDIT;

    case IDE_ACTION_REDO:
      if (gtk_text_buffer_get_can_redo(tbuf)) {
        gtk_text_buffer_redo(tbuf);
        if (out_toast) {
          *out_toast = "↷ Redo";
        }
      }
      return IDE_EDITOR_OP_POST_EDIT;

    default:
      return IDE_EDITOR_OP_UNHANDLED;
  }
}

IdeEditorOpOutcome
ide_editor_ops_execute(IdeEditorHost   *host,
                       const IdeAction *act,
                       const char     **out_toast)
{
  if (out_toast) {
    *out_toast = NULL;
  }
  if (!host || !host->buffer || !act) {
    return IDE_EDITOR_OP_UNHANDLED;
  }

  GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(host->buffer);
  if (!sbuf) {
    return IDE_EDITOR_OP_UNHANDLED;
  }
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(sbuf);
  GtkTextIter ins, bound;
  gtk_text_buffer_get_iter_at_mark(tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
  gtk_text_buffer_get_iter_at_mark(tbuf, &bound, gtk_text_buffer_get_selection_bound(tbuf));

  IdeEditorOpOutcome res = execute_motion(host, tbuf, &ins, &bound, act);
  if (res == IDE_EDITOR_OP_UNHANDLED) {
    res = execute_edit(host, tbuf, &ins, &bound, act, out_toast);
  }
  return res;
}
