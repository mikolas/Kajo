#include "ide_picker.h"
#include "ide_buffer.h"

#include <gtksourceview/gtksource.h>
#include <string.h>

typedef struct {
  char         *category;
  char         *title;
  char         *label;
  char         *subtitle;
  char         *abs_path;
  guint         line_1based;
  guint         col_1based;
  IdeActionType cmd_action;
  gint          score;
} IdePickerItem;

struct _IdePicker {
  char               *workspace_root;
  IdePickerMode       mode;
  IdePickerOpenFileCb open_file_cb;
  IdePickerCommandCb  command_cb;
  gpointer            cb_data;

  GtkWidget          *backdrop_box;
  GtkWidget          *card_box;
  GtkWidget          *header_title;
  GtkWidget          *count_badge;
  GtkWidget          *search_entry;
  GtkWidget          *category_bar;
  GtkWidget          *help_col_header;
  GtkWidget          *paned;
  GtkWidget          *list_scroll;
  GtkWidget          *list_box;
  GtkWidget          *preview_pane;
  GtkWidget          *preview_header_lbl;
  GtkWidget          *preview_scrolled;
  GtkWidget          *preview_view;
  GtkSourceBuffer    *preview_buf;
  GtkWidget          *footer_left_lbl;
  GtkWidget          *footer_mid_lbl;
  GtkWidget          *footer_right_lbl;

  char               *active_category;
  GPtrArray          *all_items;
  GPtrArray          *filtered_items;
  gint                selected_idx;
  gboolean            updating_selection;
};

static const struct {
  const char   *category;
  const char   *title;
  const char   *description;
  const char   *chord;
  IdeActionType action;
} kHelpCommands[] = {
  /* 1. Navigation & Viewport */
  { "NAVIGATION", "Move Left", "Move cursor left 1 column", "h / Left", IDE_ACTION_MOVE_LEFT },
  { "NAVIGATION", "Move Right", "Move cursor right 1 column", "l / Right", IDE_ACTION_MOVE_RIGHT },
  { "NAVIGATION", "Move Up", "Move cursor up 1 row", "k / Up", IDE_ACTION_MOVE_UP },
  { "NAVIGATION", "Move Down", "Move cursor down 1 row", "j / Down", IDE_ACTION_MOVE_DOWN },
  { "NAVIGATION", "Line Start", "Jump to beginning of current line", "0 / Home / g h", IDE_ACTION_MOVE_LINE_START },
  { "NAVIGATION", "Line End", "Jump to end of current line", "$ / End / g l", IDE_ACTION_MOVE_LINE_END },
  { "NAVIGATION", "Buffer Start", "Jump to very first line of buffer", "g g / Ctrl+Home", IDE_ACTION_MOVE_BUFFER_START },
  { "NAVIGATION", "Buffer End", "Jump to very last line of buffer", "G / Ctrl+End", IDE_ACTION_MOVE_BUFFER_END },
  { "NAVIGATION", "Page Up", "Scroll viewport up by 18 lines", "Ctrl+U / PgUp", IDE_ACTION_PAGE_UP },
  { "NAVIGATION", "Page Down", "Scroll viewport down by 18 lines", "Ctrl+D / PgDn", IDE_ACTION_PAGE_DOWN },
  { "NAVIGATION", "Jumplist Back", "Jump backward in cross-file location history", "Ctrl+O", IDE_ACTION_JUMPLIST_BACK },
  { "NAVIGATION", "Jumplist Forward", "Jump forward in cross-file location history", "Ctrl+I", IDE_ACTION_JUMPLIST_FORWARD },
  { "NAVIGATION", "Word Backward", "Move cursor one word backward", "Alt+Left", IDE_ACTION_MOVE_WORD_BACKWARD },
  { "NAVIGATION", "Word Forward", "Move cursor one word forward", "Alt+Right", IDE_ACTION_MOVE_WORD_FORWARD },

  /* 2. Selection-First & Mutations */
  { "SELECT & EDIT", "Select Word", "Select word forward and enter SELECT mode", "w", IDE_ACTION_SELECT_WORD },
  { "SELECT & EDIT", "Select Word Back", "Select word backward and enter SELECT mode", "b", IDE_ACTION_SELECT_WORD_BACKWARD },
  { "SELECT & EDIT", "Select Line", "Select entire current line (repeat x to extend)", "x", IDE_ACTION_SELECT_LINE },
  { "SELECT & EDIT", "Visual Select Mode", "Toggle character-wise SELECT mode at cursor", "v", IDE_ACTION_NONE },
  { "SELECT & EDIT", "Insert Mode", "Enter INSERT mode before cursor", "i", IDE_ACTION_NONE },
  { "SELECT & EDIT", "Append Mode", "Enter INSERT mode after cursor", "a", IDE_ACTION_NONE },
  { "SELECT & EDIT", "Open Line Below", "Insert newline below and enter INSERT mode", "o", IDE_ACTION_NONE },
  { "SELECT & EDIT", "Open Line Above", "Insert newline above and enter INSERT mode", "O", IDE_ACTION_NONE },
  { "SELECT & EDIT", "Fast Exit Insert", "Type j then k within 200ms to return to NORMAL", "j k", IDE_ACTION_NONE },
  { "SELECT & EDIT", "Collapse Selection", "Collapse active selection and return to NORMAL", "Esc", IDE_ACTION_COLLAPSE_SELECTION },
  { "SELECT & EDIT", "Delete Selection", "Delete active selection (or char under cursor)", "d", IDE_ACTION_DELETE_SELECTION },
  { "SELECT & EDIT", "Change Selection", "Delete selection and immediately enter INSERT", "c", IDE_ACTION_CHANGE_SELECTION },
  { "SELECT & EDIT", "Yank to Clipboard", "Copy selection (or current line) to Wayland clipboard", "y / Ctrl+C", IDE_ACTION_YANK },
  { "SELECT & EDIT", "Paste Clipboard", "Paste clipboard contents after / before cursor", "p / P / Ctrl+V", IDE_ACTION_PASTE_AFTER },
  { "SELECT & EDIT", "Undo", "Undo last buffer edit transaction", "u / Ctrl+Z", IDE_ACTION_UNDO },
  { "SELECT & EDIT", "Redo", "Redo previously undone edit transaction", "U / Ctrl+Y / Ctrl+R", IDE_ACTION_REDO },
  { "SELECT & EDIT", "Indent / Outdent", "Indent or outdent selected lines by 1 tab stop", "> / <", IDE_ACTION_INDENT },

  /* 3. Nordic Semantic Text Objects */
  { "OBJECTS", "Function Object", "Select inside / around C/Go/Rust/Python function body", "m i f / m a f", IDE_ACTION_NONE },
  { "OBJECTS", "Class / Struct", "Select inside / around struct, class, or TOML table", "m i c / m a c", IDE_ACTION_NONE },
  { "OBJECTS", "Block Object {}", "Select inside / around curly brace block { ... }", "m i b / m a b", IDE_ACTION_NONE },
  { "OBJECTS", "Array Object []", "Select inside / around bracket array [ ... ]", "m i a / m a a", IDE_ACTION_NONE },
  { "OBJECTS", "Parens Object ()", "Select inside / around parenthesized group ( ... )", "m i p / m a p", IDE_ACTION_NONE },
  { "OBJECTS", "Quotes Object", "Select inside / around quoted string \"...\" or '...'", "m i q / m a q", IDE_ACTION_NONE },
  { "OBJECTS", "Word Object", "Select inside / around identifier word under cursor", "m i w / m a w", IDE_ACTION_NONE },

  /* 4. Space Leader Catalog */
  { "LEADER", "Fuzzy Find Files", "Open workspace file picker with syntax preview", "Space Space / Space f f", IDE_ACTION_PICKER_OPEN_FILES },
  { "LEADER", "Live Ripgrep", "Real-time workspace regex search via ripgrep", "Space / / Space f g", IDE_ACTION_PICKER_OPEN_GREP },
  { "LEADER", "Switch Buffers", "Fuzzy filter across currently open editor buffers", "Space b / Space b b", IDE_ACTION_PICKER_OPEN_BUFFERS },
  { "LEADER", "Save Buffer", "Atomically save active buffer to disk", "Space w / Space f s / Ctrl+S", IDE_ACTION_SAVE },
  { "LEADER", "Save All Buffers", "Atomically save all modified open buffers", "Space w a / Space f a", IDE_ACTION_SAVE_ALL },
  { "LEADER", "Close Buffer", "Close active buffer tab", "Space q / Space b d", IDE_ACTION_BUFFER_CLOSE },
  { "LEADER", "Force Close Buffer", "Force close active buffer without saving", "Space Q / Space b D", IDE_ACTION_BUFFER_FORCE_CLOSE },
  { "LEADER", "Alternate File", "Switch between .c source, .h header, and unit test", "Space a", IDE_ACTION_ALTERNATE_FILE },
  { "LEADER", "In-Buffer Search", "Start incremental case-insensitive search in buffer", "/ / Space s f", IDE_ACTION_SEARCH_OPEN },
  { "LEADER", "Next / Prev Match", "Jump to next or previous in-buffer search match", "n / N", IDE_ACTION_SEARCH_NEXT },
  { "LEADER", "Replace in Buffer", "Open Search & Replace drawer for active buffer", "Space s r", IDE_ACTION_REPLACE_OPEN },
  { "LEADER", "Replace in Project", "Open project-wide Search & Replace with diff preview", "Space s p", IDE_ACTION_PROJECT_REPLACE_OPEN },
  { "LEADER", "Clear Highlights", "Clear active search match highlights", "Space c h", IDE_ACTION_SEARCH_CLEAR_HIGHLIGHT },
  { "LEADER", "Help & Palette", "Toggle this Help & Keyboard Reference modal", "? / F1 / Space h", IDE_ACTION_HELP_TOGGLE },

  /* 5. File Tree Explorer */
  { "FILE TREE", "Toggle File Tree", "Toggle left workspace File Tree sidebar", "Space e", IDE_ACTION_TOGGLE_FILE_TREE },
  { "FILE TREE", "Open / Expand", "Open selected file or toggle folder expansion", "Enter / l / Right", IDE_ACTION_TREE_TOGGLE_OR_OPEN },
  { "FILE TREE", "Collapse / Parent", "Collapse open folder or jump to parent directory", "h / Left / Backspace", IDE_ACTION_TREE_COLLAPSE_OR_PARENT },
  { "FILE TREE", "New File in Tree", "Prompt to create a new file in selected directory", "Tree: a", IDE_ACTION_TREE_CREATE_FILE },
  { "FILE TREE", "New Folder in Tree", "Prompt to create a new folder in selected directory", "Tree: A", IDE_ACTION_TREE_CREATE_DIR },
  { "FILE TREE", "Delete in Tree", "Delete selected file or directory from workspace", "Tree: d", IDE_ACTION_TREE_DELETE },
  { "FILE TREE", "Refresh Git Tree", "Rescan workspace filesystem and git status badges", "Tree: r", IDE_ACTION_TREE_REFRESH },

  /* 6. LSP 3.17 Code Intelligence */
  { "LSP", "Go to Definition", "Jump to symbol definition via clangd / gopls / rust-analyzer", "g d / Space g d", IDE_ACTION_LSP_DEFINITION },
  { "LSP", "Go to Implementation", "Jump to interface or trait implementation", "Space g i", IDE_ACTION_LSP_IMPLEMENTATION },
  { "LSP", "Call Hierarchy", "Inspect incoming and outgoing callers in bottom drawer", "Space g c", IDE_ACTION_LSP_CALL_HIERARCHY },
  { "LSP", "Hover Documentation", "Show pixel-anchored signature and doc comment popover", "K / Space k", IDE_ACTION_LSP_HOVER },
  { "LSP", "Autocomplete", "Trigger pixel-anchored LSP completion popover at cursor", "Ctrl+Space", IDE_ACTION_LSP_COMPLETE },
  { "LSP", "Code Actions", "Open LSP quickfix and refactoring action menu", "Space c a", IDE_ACTION_LSP_CODE_ACTION },
  { "LSP", "Rename Symbol", "Rename identifier across workspace via LSP", "Space c r", IDE_ACTION_LSP_RENAME },
  { "LSP", "Next Diagnostic", "Jump to next LSP error or warning in buffer", "Space n d / ] d", IDE_ACTION_NEXT_DIAGNOSTIC },
  { "LSP", "Prev Diagnostic", "Jump to previous LSP error or warning in buffer", "Space p d / [ d", IDE_ACTION_PREV_DIAGNOSTIC },
  { "LSP", "Nordic Quick Step", "Step forward / backward across diagnostics or tabs", "ä / ö", IDE_ACTION_STEP_FORWARD },

  /* 7. DAP Interactive Debugger */
  { "DEBUGGER", "Toggle Breakpoint", "Set or clear red breakpoint dot on current line", "F9 / Space d b", IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
  { "DEBUGGER", "Start Debug Session", "Launch gdb / lldb / delve DAP session for target", "Space d d", IDE_ACTION_DAP_START },
  { "DEBUGGER", "Continue Execution", "Start or resume paused program execution", "F5 / Space d c", IDE_ACTION_DAP_CONTINUE },
  { "DEBUGGER", "Step Over Line", "Execute current line without stepping into calls", "F10 / Space d n", IDE_ACTION_DAP_STEP_OVER },
  { "DEBUGGER", "Step Into Function", "Step into function call on current line", "F11 / Space d i", IDE_ACTION_DAP_STEP_INTO },
  { "DEBUGGER", "Step Out of Function", "Finish current function and pause at caller", "F12 / Shift+F11 / Space d o", IDE_ACTION_DAP_STEP_OUT },
  { "DEBUGGER", "Stop Debugger", "Terminate active DAP debug session", "Shift+F5 / Space d q", IDE_ACTION_DAP_STOP },
  { "DEBUGGER", "DAP Inspector Drawer", "Open 3-pane Call Stack, Scopes & REPL drawer", "Space d v", IDE_ACTION_DAP_TOGGLE_VIEW },

  /* 8. Graphical Markdown & Cairo Vector Mermaid */
  { "MARKDOWN", "GUI Markdown Split", "Toggle side-by-side GUI Markdown & Mermaid preview", "Space m p", IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW },
  { "MARKDOWN", "Mermaid Vector Drawer", "Open Cairo vector Mermaid diagram viewer in drawer", "Space m m / Space m d", IDE_ACTION_TOGGLE_MERMAID_MODE },
  { "MARKDOWN", "Toggle Task Checkbox", "Toggle GFM task item - [ ] <-> - [x] on current line", "Space m t", IDE_ACTION_TOGGLE_CHECKLIST },

  /* 9. AI Agent IPC & Batch Diff Review */
  { "AGENT", "Review Agent Diffs", "Open AI Agent batch transaction unified diff drawer", "Space d s", IDE_ACTION_AGENT_REVIEW_OPEN },
  { "AGENT", "Accept All Agent Edits", "Accept all pending AI agent modifications across files", "Space d a", IDE_ACTION_AGENT_ACCEPT_ALL },
  { "AGENT", "Revert All Agent Edits", "Atomically roll back all pending AI agent modifications", "Space d r", IDE_ACTION_AGENT_REVERT_ALL },

  /* 10. Panes, Tabs & Typography Zoom */
  { "LAYOUT", "Cycle Pane Focus", "Cycle keyboard focus between Editor, Tree, and Drawer", "Ctrl+W / Space w w", IDE_ACTION_PANE_CYCLE },
  { "LAYOUT", "Prev / Next Tab", "Switch to previous or next open buffer tab", "H / L", IDE_ACTION_TAB_PREV },
  { "LAYOUT", "Direct Tab 1..9", "Jump directly to buffer tab 1 through 9", "Alt+1 .. Alt+9", IDE_ACTION_TAB_SWITCH },
  { "LAYOUT", "Build Output Drawer", "Toggle bottom Build / Quickfix / Search drawer", "Space o", IDE_ACTION_TOGGLE_DRAWER },
  { "LAYOUT", "Resize File Tree", "Grow or shrink File Tree sidebar width (+/- 24px)", "Space w > / <", IDE_ACTION_RESIZE_TREE_GROW },
  { "LAYOUT", "Resize Bottom Drawer", "Grow or shrink Bottom Drawer height (+/- 30px)", "Space w + / -", IDE_ACTION_RESIZE_DRAWER_GROW },
  { "LAYOUT", "Zoom Editor Font", "Increase, decrease, or reset editor font size (10-24px)", "Ctrl++ / Ctrl+- / Ctrl+0", IDE_ACTION_NONE },

  /* 11. Mouse & Touchpad */
  { "MOUSE", "Gutter Breakpoint", "Click column 1 in the left gutter to toggle a breakpoint", "Left Click Gutter", IDE_ACTION_NONE },
  { "MOUSE", "Splitter Drag", "Drag vertical or horizontal paned dividers to resize panes", "Drag Divider", IDE_ACTION_NONE },
  { "MOUSE", "Mermaid Pan & Zoom", "Use toolbar buttons (🔍-/100%/🔍+/⛶ FIT/💾 SVG) on diagrams", "Diagram Toolbar", IDE_ACTION_NONE },
  { NULL, NULL, NULL, NULL, IDE_ACTION_NONE }
};

static const char *const kHelpCategories[] = {
  "ALL",
  "NAVIGATION",
  "SELECT & EDIT",
  "OBJECTS",
  "LEADER",
  "FILE TREE",
  "LSP",
  "DEBUGGER",
  "MARKDOWN",
  "AGENT",
  "LAYOUT",
  "MOUSE",
  NULL
};

static void
ide_picker_item_free(gpointer data)
{
  IdePickerItem *it = data;
  if (it == NULL) {
    return;
  }
  g_free(it->category);
  g_free(it->title);
  g_free(it->label);
  g_free(it->subtitle);
  g_free(it->abs_path);
  g_free(it);
}

gint
ide_fuzzy_score(const char *query, const char *candidate)
{
  if (candidate == NULL) {
    return 0;
  }
  if (query == NULL || query[0] == '\0') {
    return 100;
  }

  gsize qlen = strlen(query);
  gsize clen = strlen(candidate);
  if (qlen > clen) {
    return 0;
  }

  gint score = 100;
  gsize qi = 0;
  gssize prev_match = -1;

  for (gsize ci = 0; ci < clen && qi < qlen; ci++) {
    if (query[qi] == ' ') {
      qi++;
      if (qi >= qlen) {
        break;
      }
    }
    if (g_ascii_tolower(query[qi]) == g_ascii_tolower(candidate[ci])) {
      score += 15;
      if (prev_match >= 0 && (gsize)(prev_match + 1) == ci) {
        score += 25;
      }
      if (ci == 0 || candidate[ci - 1] == '/' || candidate[ci - 1] == '_' ||
          candidate[ci - 1] == '-' || candidate[ci - 1] == '.' ||
          candidate[ci - 1] == ' ') {
        score += 20;
      }
      prev_match = (gssize)ci;
      qi++;
    } else {
      score -= 1;
    }
  }

  while (qi < qlen && query[qi] == ' ') {
    qi++;
  }
  if (qi < qlen) {
    return 0;
  }
  return MAX(1, score);
}

static gint
cmp_picker_items(gconstpointer a, gconstpointer b)
{
  const IdePickerItem *ia = *(const IdePickerItem *const *)a;
  const IdePickerItem *ib = *(const IdePickerItem *const *)b;
  if (ia->score != ib->score) {
    return (ib->score > ia->score) - (ib->score < ia->score);
  }
  return g_ascii_strcasecmp(ia->label, ib->label);
}

static const char *
get_category_badge_class(const char *category)
{
  if (g_strcmp0(category, "DEBUGGER") == 0) return "ide-git-deleted";
  if (g_strcmp0(category, "SELECT & EDIT") == 0 ||
      g_strcmp0(category, "FILE TREE") == 0) return "ide-git-modified";
  if (g_strcmp0(category, "MARKDOWN") == 0) return "ide-git-added";
  return "ide-git-untracked";
}

static void
ide_picker_update_preview(IdePicker *picker)
{
  if (picker->mode == IDE_PICKER_HELP) {
    return;
  }
  if (picker->selected_idx < 0 ||
      (guint)picker->selected_idx >= picker->filtered_items->len) {
    gtk_label_set_text(GTK_LABEL(picker->preview_header_lbl), "PREVIEW: NONE");
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(picker->preview_buf), "", -1);
    return;
  }

  const IdePickerItem *it =
      g_ptr_array_index(picker->filtered_items, picker->selected_idx);
  if (it->abs_path == NULL) {
    gtk_label_set_text(GTK_LABEL(picker->preview_header_lbl), "PREVIEW: [UNSAVED BUFFER]");
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(picker->preview_buf), "", -1);
    return;
  }

  g_autofree char *hdr_txt = g_strdup_printf(
      "PREVIEW: %s:%u", it->label ? it->label : it->abs_path, it->line_1based);
  gtk_label_set_text(GTK_LABEL(picker->preview_header_lbl), hdr_txt);

  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(it->abs_path, &contents, &len, NULL)) {
    gtk_text_buffer_set_text(
        GTK_TEXT_BUFFER(picker->preview_buf), "[Unable to read file preview]", -1);
    return;
  }

  if (len > 24576) {
    contents[24576] = '\0';
    len = 24576;
  }
  g_autofree char *valid = NULL;
  const char *txt = contents;
  if (!g_utf8_validate(contents, (gssize)len, NULL)) {
    valid = g_utf8_make_valid(contents, (gssize)len);
    txt = valid;
  }

  GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
  GtkSourceLanguage *lang =
      gtk_source_language_manager_guess_language(lm, it->abs_path, NULL);
  gtk_source_buffer_set_language(picker->preview_buf, lang);
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(picker->preview_buf), txt, -1);

  GtkTextIter line_iter;
  guint target_line = (it->line_1based > 0) ? (it->line_1based - 1) : 0;
  gtk_text_buffer_get_iter_at_line(
      GTK_TEXT_BUFFER(picker->preview_buf), &line_iter, (gint)target_line);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(picker->preview_buf), &line_iter);
  gtk_text_view_scroll_to_iter(
      GTK_TEXT_VIEW(picker->preview_view), &line_iter, 0.2, TRUE, 0.0, 0.35);
}

static void
scroll_row_into_view(IdePicker *picker, gint idx, gint total)
{
  if (!picker || !picker->list_scroll || total <= 0) return;
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
      GTK_SCROLLED_WINDOW(picker->list_scroll));
  if (!vadj) return;

  double page = gtk_adjustment_get_page_size(vadj);
  double upper = gtk_adjustment_get_upper(vadj);
  double cur = gtk_adjustment_get_value(vadj);

  /* Estimate row height or use upper / total when realized */
  double row_h = (upper > 60.0 && total > 0) ? (upper / (double)total) : 30.0;
  double row_top = idx * row_h;
  double row_bot = row_top + row_h;

  if (page <= 10.0) page = 320.0;

  if (row_top < cur) {
    gtk_adjustment_set_value(vadj, MAX(0.0, row_top));
  } else if (row_bot > cur + page) {
    double max_val = MAX(0.0, upper - page);
    gtk_adjustment_set_value(
        vadj, max_val > 0.0 ? MIN(max_val, row_bot - page + row_h) : (row_bot - page));
  }
}

static void
ide_picker_select_index(IdePicker *picker, gint idx)
{
  if (picker->filtered_items->len == 0) {
    picker->selected_idx = -1;
    ide_picker_update_preview(picker);
    return;
  }
  idx = CLAMP(idx, 0, (gint)picker->filtered_items->len - 1);
  picker->selected_idx = idx;

  picker->updating_selection = TRUE;
  GtkListBoxRow *row =
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(picker->list_box), idx);
  if (row != NULL) {
    gtk_list_box_select_row(GTK_LIST_BOX(picker->list_box), row);
  }
  picker->updating_selection = FALSE;

  scroll_row_into_view(picker, idx, (gint)picker->filtered_items->len);
  ide_picker_update_preview(picker);
}

static void
ide_picker_run_live_grep(IdePicker *picker, const char *query)
{
  g_ptr_array_set_size(picker->all_items, 0);
  if (query == NULL || strlen(query) < 2 || picker->workspace_root == NULL) {
    return;
  }

  const char *argv[] = {
    "rg", "--vimgrep", "--no-heading", "--smart-case", "--max-count=80",
    "--glob=!.git", "--glob=!builddir", "--", query, picker->workspace_root, NULL
  };
  g_autofree char *stdout_buf = NULL;
  if (!g_spawn_sync(NULL,
                    (char **)argv,
                    NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                    NULL,
                    NULL,
                    &stdout_buf,
                    NULL,
                    NULL,
                    NULL) ||
      stdout_buf == NULL) {
    return;
  }

  gsize root_len = strlen(picker->workspace_root);
  g_auto(GStrv) lines = g_strsplit(stdout_buf, "\n", 120);
  for (gsize i = 0; lines[i] != NULL && picker->all_items->len < 100; i++) {
    const char *l = lines[i];
    if (l[0] == '\0') {
      continue;
    }
    g_auto(GStrv) parts = g_strsplit(l, ":", 4);
    if (g_strv_length(parts) < 4) {
      continue;
    }
    const char *abs_p = parts[0];
    const char *rel_p = abs_p;
    if (g_str_has_prefix(abs_p, picker->workspace_root)) {
      rel_p = abs_p + root_len;
      while (*rel_p == '/') {
        rel_p++;
      }
    }

    IdePickerItem *it = g_new0(IdePickerItem, 1);
    it->abs_path = g_strdup(abs_p);
    it->line_1based = (guint)MAX(1, g_ascii_strtoll(parts[1], NULL, 10));
    it->col_1based = (guint)MAX(1, g_ascii_strtoll(parts[2], NULL, 10));
    it->label = g_strdup_printf("%s:%u", rel_p, it->line_1based);
    g_autofree char *snippet = g_strdup(parts[3]);
    g_strstrip(snippet);
    it->subtitle = g_strdup(snippet);
    it->score = 200 - (gint)i;
    g_ptr_array_add(picker->all_items, it);
  }
}

static void
ide_picker_rebuild_list(IdePicker *picker)
{
  const char *query = gtk_editable_get_text(GTK_EDITABLE(picker->search_entry));
  if (picker->mode == IDE_PICKER_GREP) {
    ide_picker_run_live_grep(picker, query);
  }

  g_ptr_array_set_size(picker->filtered_items, 0);
  for (guint i = 0; i < picker->all_items->len; i++) {
    IdePickerItem *it = g_ptr_array_index(picker->all_items, i);
    if (picker->mode == IDE_PICKER_GREP) {
      g_ptr_array_add(picker->filtered_items, it);
      continue;
    }
    if (picker->mode == IDE_PICKER_HELP &&
        picker->active_category != NULL &&
        g_strcmp0(picker->active_category, "ALL") != 0 &&
        g_strcmp0(it->category, picker->active_category) != 0) {
      continue;
    }

    gint s1 = ide_fuzzy_score(query, it->label);
    gint s2 = (it->subtitle != NULL) ? ide_fuzzy_score(query, it->subtitle) : 0;
    gint s3 = (it->title != NULL) ? ide_fuzzy_score(query, it->title) : 0;
    gint s4 = (it->category != NULL) ? ide_fuzzy_score(query, it->category) : 0;
    gint best = MAX(MAX(s1, s2), MAX(s3, s4));
    if (best > 0) {
      it->score = best;
      g_ptr_array_add(picker->filtered_items, it);
    }
  }

  if (picker->mode != IDE_PICKER_GREP && query != NULL && query[0] != '\0') {
    g_ptr_array_sort(picker->filtered_items, cmp_picker_items);
  }

  GtkWidget *child = gtk_widget_get_first_child(picker->list_box);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(picker->list_box), child);
    child = next;
  }

  guint limit = (picker->mode == IDE_PICKER_HELP) ? 200u : 80u;
  guint max_rows = MIN(picker->filtered_items->len, limit);
  for (guint i = 0; i < max_rows; i++) {
    const IdePickerItem *it = g_ptr_array_index(picker->filtered_items, i);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focusable(row, FALSE);
    gtk_widget_add_css_class(row, "ide-tree-row");
    gtk_widget_add_css_class(row, "ide-picker-row");

    if (picker->mode == IDE_PICKER_HELP) {
      /* Full-width 4-Column Help & Command Palette Row */
      GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

      /* Column 1: Key Chord Keycap Pill */
      GtkWidget *chord_lbl = gtk_label_new(it->subtitle ? it->subtitle : "");
      gtk_label_set_xalign(GTK_LABEL(chord_lbl), 0.0f);
      gtk_widget_set_size_request(chord_lbl, 190, -1);
      gtk_widget_set_valign(chord_lbl, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(chord_lbl, "ide-help-chord");
      gtk_box_append(GTK_BOX(hbox), chord_lbl);

      /* Column 2: Command Title */
      GtkWidget *title_lbl = gtk_label_new(it->title ? it->title : "");
      gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0f);
      gtk_widget_set_size_request(title_lbl, 165, -1);
      gtk_widget_set_valign(title_lbl, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(title_lbl, "ide-help-title");
      gtk_box_append(GTK_BOX(hbox), title_lbl);

      /* Column 3: Semantic Category Badge */
      GtkWidget *cat_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_size_request(cat_box, 118, -1);
      gtk_widget_set_valign(cat_box, GTK_ALIGN_CENTER);
      GtkWidget *cat_lbl = gtk_label_new(it->category ? it->category : "GENERAL");
      gtk_widget_set_valign(cat_lbl, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(cat_lbl, "ide-git-badge");
      gtk_widget_add_css_class(cat_lbl, get_category_badge_class(it->category));
      gtk_box_append(GTK_BOX(cat_box), cat_lbl);
      gtk_box_append(GTK_BOX(hbox), cat_box);

      /* Column 4: Full Description */
      GtkWidget *desc_lbl = gtk_label_new(it->label ? it->label : "");
      gtk_label_set_xalign(GTK_LABEL(desc_lbl), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(desc_lbl), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand(desc_lbl, TRUE);
      gtk_widget_set_valign(desc_lbl, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(desc_lbl, "ide-help-desc");
      gtk_box_append(GTK_BOX(hbox), desc_lbl);

      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    } else {
      /* File / Buffer / Live Grep Row */
      GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

      gboolean is_md = it->label &&
                       (g_str_has_suffix(it->label, ".md") ||
                        g_str_has_suffix(it->label, ".markdown"));
      GtkWidget *ic = gtk_label_new(is_md ? "📖" : "📄");
      gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(hbox), ic);

      GtkWidget *lbl = gtk_label_new(it->label);
      gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_set_hexpand(lbl, TRUE);
      gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(hbox), lbl);

      if (it->subtitle != NULL && it->subtitle[0] != '\0') {
        GtkWidget *sub = gtk_label_new(it->subtitle);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
        gtk_widget_set_valign(sub, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(sub, "ide-help-desc");
        gtk_box_append(GTK_BOX(hbox), sub);
      }

      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    }

    gtk_list_box_append(GTK_LIST_BOX(picker->list_box), row);
  }

  const char *unit = (picker->mode == IDE_PICKER_HELP)
                         ? "COMMANDS"
                         : (picker->mode == IDE_PICKER_GREP ? "MATCHES" : "FILES");
  g_autofree char *cnt = g_strdup_printf("%u / %u %s",
                                         picker->filtered_items->len,
                                         picker->all_items->len,
                                         unit);
  gtk_label_set_text(GTK_LABEL(picker->count_badge), cnt);

  ide_picker_select_index(picker, 0);
}

static void
collect_workspace_files_recursive(IdePicker  *picker,
                                  const char *dir_path,
                                  guint       depth)
{
  if (depth > 8 || picker->all_items->len >= 2000) {
    return;
  }
  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (dir == NULL) {
    return;
  }

  gsize root_len = strlen(picker->workspace_root);
  const char *name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (name[0] == '.' || g_strcmp0(name, "builddir") == 0 ||
        g_strcmp0(name, "node_modules") == 0 || g_strcmp0(name, "target") == 0) {
      continue;
    }
    char *abs_p = g_build_filename(dir_path, name, NULL);
    if (g_file_test(abs_p, G_FILE_TEST_IS_DIR)) {
      collect_workspace_files_recursive(picker, abs_p, depth + 1);
      g_free(abs_p);
      continue;
    }

    const char *rel = abs_p;
    if (g_str_has_prefix(abs_p, picker->workspace_root)) {
      rel = abs_p + root_len;
      while (*rel == '/') {
        rel++;
      }
    }

    IdePickerItem *it = g_new0(IdePickerItem, 1);
    it->label = g_strdup(rel);
    it->abs_path = abs_p;
    it->line_1based = 1;
    it->col_1based = 1;
    g_ptr_array_add(picker->all_items, it);
  }
  g_dir_close(dir);
}

static void
on_picker_row_selected(GtkListBox    *box,
                       GtkListBoxRow *row,
                       gpointer       user_data)
{
  (void)box;
  IdePicker *picker = user_data;
  if (!picker || picker->updating_selection || row == NULL) {
    return;
  }
  picker->selected_idx = gtk_list_box_row_get_index(row);
  ide_picker_update_preview(picker);
}

static void
on_picker_row_activated(GtkListBox    *box,
                        GtkListBoxRow *row,
                        gpointer       user_data)
{
  (void)box;
  IdePicker *picker = user_data;
  if (row == NULL) {
    return;
  }
  picker->selected_idx = gtk_list_box_row_get_index(row);
  ide_picker_confirm(picker);
}

static void
on_picker_esc_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  IdePicker *picker = user_data;
  ide_picker_close(picker);
  if (picker->command_cb != NULL) {
    picker->command_cb(IDE_ACTION_HELP_CANCEL, picker->cb_data);
  }
}

static void
on_category_pill_clicked(GtkButton *btn, gpointer user_data)
{
  IdePicker *picker = user_data;
  const char *cat = g_object_get_data(G_OBJECT(btn), "help-category");
  if (!cat) return;

  g_free(picker->active_category);
  picker->active_category = g_strdup(cat);

  for (GtkWidget *c = gtk_widget_get_first_child(picker->category_bar);
       c != NULL;
       c = gtk_widget_get_next_sibling(c)) {
    const char *c_cat = g_object_get_data(G_OBJECT(c), "help-category");
    if (g_strcmp0(c_cat, cat) == 0)
      gtk_widget_add_css_class(c, "active");
    else
      gtk_widget_remove_css_class(c, "active");
  }

  ide_picker_rebuild_list(picker);
}

static void
configure_picker_layout_for_mode(IdePicker *picker, IdePickerMode mode)
{
  picker->mode = mode;
  gboolean is_help = (mode == IDE_PICKER_HELP);

  gtk_widget_set_visible(picker->category_bar, is_help);
  gtk_widget_set_visible(picker->help_col_header, is_help);
  gtk_widget_set_visible(picker->preview_pane, !is_help);

  if (is_help) {
    gtk_paned_set_position(GTK_PANED(picker->paned), 920);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(picker->search_entry),
        "Filter commands by key chord, title, category, or description (↑/↓/Tab to select, Enter to run)...");
    gtk_label_set_text(GTK_LABEL(picker->footer_left_lbl),
                       "[ ↑/↓/Tab: Select Command ]   [ PgUp/PgDn: Page ]");
    gtk_label_set_text(GTK_LABEL(picker->footer_mid_lbl),
                       "[ Enter: Execute Command Immediately ]");
    gtk_label_set_text(GTK_LABEL(picker->footer_right_lbl),
                       "[ Esc / ? / F1: Dismiss ]");
  } else {
    gtk_paned_set_position(GTK_PANED(picker->paned), 420);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(picker->search_entry),
        "Type to fuzzy-filter files or symbols (↑/↓ or Ctrl+P/N, Enter to open, Esc to cancel)...");
    gtk_label_set_text(GTK_LABEL(picker->footer_left_lbl),
                       "[ ↑/↓ or Ctrl+P/N: Navigate ]");
    gtk_label_set_text(GTK_LABEL(picker->footer_mid_lbl),
                       "[ Enter: Open Selected File ]");
    gtk_label_set_text(GTK_LABEL(picker->footer_right_lbl),
                       "[ Space f g: Live Ripgrep • Esc: Cancel ]");
  }
}

IdePicker *
ide_picker_new(const char         *workspace_root,
               IdePickerOpenFileCb open_file_cb,
               IdePickerCommandCb  command_cb,
               gpointer            user_data)
{
  IdePicker *picker = g_new0(IdePicker, 1);
  picker->workspace_root = g_strdup(workspace_root ? workspace_root : ".");
  picker->open_file_cb = open_file_cb;
  picker->command_cb = command_cb;
  picker->cb_data = user_data;
  picker->active_category = g_strdup("ALL");
  picker->all_items = g_ptr_array_new_with_free_func(ide_picker_item_free);
  picker->filtered_items = g_ptr_array_new();

  picker->backdrop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(picker->backdrop_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(picker->backdrop_box, GTK_ALIGN_START);
  gtk_widget_set_margin_top(picker->backdrop_box, 36);
  gtk_widget_set_visible(picker->backdrop_box, FALSE);

  picker->card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(picker->card_box, "ide-picker-card");
  gtk_widget_add_css_class(picker->card_box, "ide-picker-modal");
  gtk_widget_set_size_request(picker->card_box, 920, 520);

  /* 1. Header Bar */
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "settings-header-bar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  picker->header_title = gtk_label_new("🔍 FUZZY FIND FILES (Space f f)");
  gtk_label_set_xalign(GTK_LABEL(picker->header_title), 0.0f);
  gtk_widget_set_hexpand(picker->header_title, TRUE);
  gtk_widget_set_valign(picker->header_title, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(picker->header_title, "settings-header-title");
  gtk_box_append(GTK_BOX(hdr), picker->header_title);

  picker->count_badge = gtk_label_new("0 / 0 FILES");
  gtk_widget_set_valign(picker->count_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(picker->count_badge, "shell-popover-badge");
  gtk_widget_add_css_class(picker->count_badge, "accent");
  gtk_box_append(GTK_BOX(hdr), picker->count_badge);

  GtkWidget *esc_btn = gtk_button_new_with_label("✕ ESC");
  gtk_widget_set_focusable(esc_btn, FALSE);
  gtk_widget_set_valign(esc_btn, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(esc_btn, "ide-toolbar-btn");
  g_signal_connect(esc_btn, "clicked", G_CALLBACK(on_picker_esc_clicked), picker);
  gtk_box_append(GTK_BOX(hdr), esc_btn);
  gtk_box_append(GTK_BOX(picker->card_box), hdr);

  /* 2. Search Input Strip */
  GtkWidget *search_strip = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(search_strip, "ide-picker-search-strip");

  picker->search_entry = gtk_entry_new();
  gtk_widget_set_focusable(picker->search_entry, FALSE);
  gtk_widget_add_css_class(picker->search_entry, "shell-launcher-input");
  gtk_box_append(GTK_BOX(search_strip), picker->search_entry);

  /* Category Filter Pills (shown in IDE_PICKER_HELP mode) */
  picker->category_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(picker->category_bar, GTK_ALIGN_CENTER);
  for (gsize i = 0; kHelpCategories[i] != NULL; i++) {
    GtkWidget *pill = gtk_button_new_with_label(kHelpCategories[i]);
    gtk_widget_set_focusable(pill, FALSE);
    gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(pill, "files-nav-btn");
    if (i == 0) gtk_widget_add_css_class(pill, "active");
    g_object_set_data(G_OBJECT(pill), "help-category", (gpointer)kHelpCategories[i]);
    g_signal_connect(pill, "clicked", G_CALLBACK(on_category_pill_clicked), picker);
    gtk_box_append(GTK_BOX(picker->category_bar), pill);
  }
  gtk_box_append(GTK_BOX(search_strip), picker->category_bar);
  gtk_box_append(GTK_BOX(picker->card_box), search_strip);

  /* 3. Sticky 4-Column Header for Help & Command Palette */
  picker->help_col_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(picker->help_col_header, "ide-help-col-header");
  gtk_widget_set_valign(picker->help_col_header, GTK_ALIGN_CENTER);

  GtkWidget *col_chord = gtk_label_new("CHORD / KEYBINDING");
  gtk_label_set_xalign(GTK_LABEL(col_chord), 0.0f);
  gtk_widget_set_size_request(col_chord, 190, -1);
  gtk_widget_set_valign(col_chord, GTK_ALIGN_CENTER);

  GtkWidget *col_cmd = gtk_label_new("COMMAND");
  gtk_label_set_xalign(GTK_LABEL(col_cmd), 0.0f);
  gtk_widget_set_size_request(col_cmd, 165, -1);
  gtk_widget_set_valign(col_cmd, GTK_ALIGN_CENTER);

  GtkWidget *col_cat = gtk_label_new("CATEGORY");
  gtk_label_set_xalign(GTK_LABEL(col_cat), 0.0f);
  gtk_widget_set_size_request(col_cat, 118, -1);
  gtk_widget_set_valign(col_cat, GTK_ALIGN_CENTER);

  GtkWidget *col_desc = gtk_label_new("ACTION DESCRIPTION (ENTER TO EXECUTE)");
  gtk_label_set_xalign(GTK_LABEL(col_desc), 0.0f);
  gtk_widget_set_hexpand(col_desc, TRUE);
  gtk_widget_set_valign(col_desc, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(picker->help_col_header), col_chord);
  gtk_box_append(GTK_BOX(picker->help_col_header), col_cmd);
  gtk_box_append(GTK_BOX(picker->help_col_header), col_cat);
  gtk_box_append(GTK_BOX(picker->help_col_header), col_desc);
  gtk_box_append(GTK_BOX(picker->card_box), picker->help_col_header);

  /* 4. Split Body: Left Match List + Right Syntax-Highlighted Preview */
  picker->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(picker->paned, "ide-paned");
  gtk_paned_set_position(GTK_PANED(picker->paned), 420);
  gtk_widget_set_vexpand(picker->paned, TRUE);
  gtk_widget_set_hexpand(picker->paned, TRUE);

  picker->list_scroll = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(picker->list_scroll, TRUE);
  gtk_widget_set_vexpand(picker->list_scroll, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(picker->list_scroll),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  picker->list_box = gtk_list_box_new();
  gtk_widget_set_focusable(picker->list_box, FALSE);
  gtk_widget_add_css_class(picker->list_box, "ide-tree-listbox");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(picker->list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(picker->list_box), FALSE);
  g_signal_connect(
      picker->list_box, "row-selected", G_CALLBACK(on_picker_row_selected), picker);
  g_signal_connect(
      picker->list_box, "row-activated", G_CALLBACK(on_picker_row_activated), picker);
  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(picker->list_scroll), picker->list_box);
  gtk_paned_set_start_child(GTK_PANED(picker->paned), picker->list_scroll);
  gtk_paned_set_resize_start_child(GTK_PANED(picker->paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(picker->paned), FALSE);

  /* Right Preview Pane (Files / Buffers / Grep) */
  picker->preview_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(picker->preview_pane, "ide-picker-preview");
  gtk_widget_set_hexpand(picker->preview_pane, TRUE);
  gtk_widget_set_vexpand(picker->preview_pane, TRUE);

  GtkWidget *prev_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(prev_hdr, "ide-drawer-header");
  gtk_widget_set_valign(prev_hdr, GTK_ALIGN_CENTER);
  picker->preview_header_lbl = gtk_label_new("PREVIEW");
  gtk_label_set_xalign(GTK_LABEL(picker->preview_header_lbl), 0.0f);
  gtk_widget_set_valign(picker->preview_header_lbl, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(prev_hdr), picker->preview_header_lbl);
  gtk_box_append(GTK_BOX(picker->preview_pane), prev_hdr);

  picker->preview_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(picker->preview_scrolled, TRUE);
  gtk_widget_set_vexpand(picker->preview_scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(picker->preview_scrolled),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);

  picker->preview_buf = gtk_source_buffer_new(NULL);
  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  GtkSourceStyleScheme *scheme =
      gtk_source_style_scheme_manager_get_scheme(sm, "kajo-metro-dark");
  if (scheme != NULL) {
    gtk_source_buffer_set_style_scheme(picker->preview_buf, scheme);
  }
  picker->preview_view = gtk_source_view_new_with_buffer(picker->preview_buf);
  gtk_widget_set_focusable(picker->preview_view, FALSE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(picker->preview_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(picker->preview_view), TRUE);
  gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(picker->preview_view), TRUE);
  gtk_source_view_set_highlight_current_line(
      GTK_SOURCE_VIEW(picker->preview_view), TRUE);
  gtk_widget_add_css_class(picker->preview_view, "ide-source-view");

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(picker->preview_scrolled), picker->preview_view);
  gtk_box_append(GTK_BOX(picker->preview_pane), picker->preview_scrolled);
  gtk_paned_set_end_child(GTK_PANED(picker->paned), picker->preview_pane);
  gtk_paned_set_resize_end_child(GTK_PANED(picker->paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(picker->paned), FALSE);

  gtk_box_append(GTK_BOX(picker->card_box), picker->paned);

  /* 5. Canonical Modal Footer Bar */
  GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(footer, "ide-picker-footer");
  gtk_widget_set_valign(footer, GTK_ALIGN_CENTER);

  picker->footer_left_lbl = gtk_label_new("[ ↑/↓ or Ctrl+P/N: Navigate ]");
  gtk_widget_set_valign(picker->footer_left_lbl, GTK_ALIGN_CENTER);

  GtkWidget *f_sp1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(f_sp1, TRUE);

  picker->footer_mid_lbl = gtk_label_new("[ Enter: Open File ]");
  gtk_widget_set_valign(picker->footer_mid_lbl, GTK_ALIGN_CENTER);

  GtkWidget *f_sp2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(f_sp2, TRUE);

  picker->footer_right_lbl = gtk_label_new("[ Esc: Cancel ]");
  gtk_widget_set_valign(picker->footer_right_lbl, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(footer), picker->footer_left_lbl);
  gtk_box_append(GTK_BOX(footer), f_sp1);
  gtk_box_append(GTK_BOX(footer), picker->footer_mid_lbl);
  gtk_box_append(GTK_BOX(footer), f_sp2);
  gtk_box_append(GTK_BOX(footer), picker->footer_right_lbl);
  gtk_box_append(GTK_BOX(picker->card_box), footer);

  gtk_box_append(GTK_BOX(picker->backdrop_box), picker->card_box);

  return picker;
}

void
ide_picker_free(IdePicker *picker)
{
  if (picker == NULL) {
    return;
  }
  g_free(picker->workspace_root);
  g_free(picker->active_category);
  g_clear_pointer(&picker->filtered_items, g_ptr_array_unref);
  g_clear_pointer(&picker->all_items, g_ptr_array_unref);
  g_clear_object(&picker->preview_buf);
  g_free(picker);
}

GtkWidget *
ide_picker_get_widget(IdePicker *picker)
{
  g_return_val_if_fail(picker != NULL, NULL);
  return picker->backdrop_box;
}

gboolean
ide_picker_is_visible(const IdePicker *picker)
{
  g_return_val_if_fail(picker != NULL, FALSE);
  return gtk_widget_get_visible(picker->backdrop_box);
}

IdePickerMode
ide_picker_get_mode(const IdePicker *picker)
{
  g_return_val_if_fail(picker != NULL, IDE_PICKER_FILES);
  return picker->mode;
}

gboolean
ide_picker_has_query(const IdePicker *picker)
{
  g_return_val_if_fail(picker != NULL, FALSE);
  const char *q = gtk_editable_get_text(GTK_EDITABLE(picker->search_entry));
  return (q != NULL && q[0] != '\0');
}

void
ide_picker_open_files(IdePicker *picker, const char *workspace_root)
{
  g_return_if_fail(picker != NULL);
  if (workspace_root != NULL) {
    g_free(picker->workspace_root);
    picker->workspace_root = g_strdup(workspace_root);
  }
  configure_picker_layout_for_mode(picker, IDE_PICKER_FILES);
  gtk_label_set_text(
      GTK_LABEL(picker->header_title), "🔍 FUZZY FIND FILES (Space f f)");
  gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), "");

  g_ptr_array_set_size(picker->all_items, 0);
  collect_workspace_files_recursive(picker, picker->workspace_root, 0);
  gtk_widget_set_visible(picker->backdrop_box, TRUE);
  ide_picker_rebuild_list(picker);
}

void
ide_picker_open_buffers(IdePicker *picker, GPtrArray *open_buffers)
{
  g_return_if_fail(picker != NULL);
  configure_picker_layout_for_mode(picker, IDE_PICKER_BUFFERS);
  gtk_label_set_text(
      GTK_LABEL(picker->header_title), "📑 SWITCH OPEN BUFFER (Space b b)");
  gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), "");

  g_ptr_array_set_size(picker->all_items, 0);
  if (open_buffers != NULL) {
    for (guint i = 0; i < open_buffers->len; i++) {
      IdeBuffer *b = g_ptr_array_index(open_buffers, i);
      IdePickerItem *it = g_new0(IdePickerItem, 1);
      it->label = g_strdup(ide_buffer_get_title(b));
      it->subtitle = g_strdup(ide_buffer_get_modified(b) ? "[+]" : "");
      it->abs_path = g_strdup(ide_buffer_get_file_path(b));
      it->line_1based = 1;
      it->col_1based = 1;
      g_ptr_array_add(picker->all_items, it);
    }
  }
  gtk_widget_set_visible(picker->backdrop_box, TRUE);
  ide_picker_rebuild_list(picker);
}

void
ide_picker_open_grep(IdePicker *picker, const char *workspace_root)
{
  g_return_if_fail(picker != NULL);
  if (workspace_root != NULL) {
    g_free(picker->workspace_root);
    picker->workspace_root = g_strdup(workspace_root);
  }
  configure_picker_layout_for_mode(picker, IDE_PICKER_GREP);
  gtk_label_set_text(
      GTK_LABEL(picker->header_title), "⚡ LIVE WORKSPACE RIPGREP (Space f g)");
  gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), "");

  g_ptr_array_set_size(picker->all_items, 0);
  gtk_widget_set_visible(picker->backdrop_box, TRUE);
  ide_picker_rebuild_list(picker);
}

void
ide_picker_open_help(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  configure_picker_layout_for_mode(picker, IDE_PICKER_HELP);
  gtk_label_set_text(
      GTK_LABEL(picker->header_title),
      "❓ HELP & KEYBOARD REFERENCE — INTERACTIVE COMMAND PALETTE (? / F1)");
  gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), "");

  g_free(picker->active_category);
  picker->active_category = g_strdup("ALL");
  for (GtkWidget *c = gtk_widget_get_first_child(picker->category_bar);
       c != NULL;
       c = gtk_widget_get_next_sibling(c)) {
    const char *c_cat = g_object_get_data(G_OBJECT(c), "help-category");
    if (g_strcmp0(c_cat, "ALL") == 0)
      gtk_widget_add_css_class(c, "active");
    else
      gtk_widget_remove_css_class(c, "active");
  }

  g_ptr_array_set_size(picker->all_items, 0);
  for (gsize i = 0; kHelpCommands[i].description != NULL; i++) {
    IdePickerItem *it = g_new0(IdePickerItem, 1);
    it->category = g_strdup(kHelpCommands[i].category);
    it->title = g_strdup(kHelpCommands[i].title);
    it->label = g_strdup(kHelpCommands[i].description);
    it->subtitle = g_strdup(kHelpCommands[i].chord);
    it->cmd_action = kHelpCommands[i].action;
    g_ptr_array_add(picker->all_items, it);
  }
  gtk_widget_set_visible(picker->backdrop_box, TRUE);
  ide_picker_rebuild_list(picker);
}

void
ide_picker_close(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  gtk_widget_set_visible(picker->backdrop_box, FALSE);
}

void
ide_picker_move_next(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  if (picker->filtered_items->len == 0) return;
  gint next = picker->selected_idx + 1;
  if (next >= (gint)picker->filtered_items->len) next = 0;
  ide_picker_select_index(picker, next);
}

void
ide_picker_move_prev(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  if (picker->filtered_items->len == 0) return;
  gint prev = picker->selected_idx - 1;
  if (prev < 0) prev = (gint)picker->filtered_items->len - 1;
  ide_picker_select_index(picker, prev);
}

void
ide_picker_page_down(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  ide_picker_select_index(picker, picker->selected_idx + 10);
}

void
ide_picker_page_up(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  ide_picker_select_index(picker, picker->selected_idx - 10);
}

void
ide_picker_append_input(IdePicker *picker, const char *ch)
{
  g_return_if_fail(picker != NULL);
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(picker->search_entry));
  g_autofree char *next = g_strconcat(cur ? cur : "", ch ? ch : "", NULL);
  gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), next);
  ide_picker_rebuild_list(picker);
}

void
ide_picker_backspace(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(picker->search_entry));
  if (cur == NULL || cur[0] == '\0') {
    return;
  }
  glong chars = g_utf8_strlen(cur, -1);
  if (chars <= 1) {
    gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), "");
  } else {
    const char *end = g_utf8_offset_to_pointer(cur, chars - 1);
    g_autofree char *trimmed = g_strndup(cur, (gsize)(end - cur));
    gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), trimmed);
  }
  ide_picker_rebuild_list(picker);
}

void
ide_picker_confirm(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  if (picker->selected_idx < 0 ||
      (guint)picker->selected_idx >= picker->filtered_items->len) {
    ide_picker_close(picker);
    return;
  }

  const IdePickerItem *it =
      g_ptr_array_index(picker->filtered_items, picker->selected_idx);
  IdePickerMode mode = picker->mode;
  g_autofree char *abs_path = g_strdup(it->abs_path);
  guint line = it->line_1based;
  guint col = it->col_1based;
  IdeActionType cmd = it->cmd_action;

  ide_picker_close(picker);

  if (mode == IDE_PICKER_HELP) {
    if (picker->command_cb != NULL) {
      picker->command_cb(
          cmd != IDE_ACTION_NONE ? cmd : IDE_ACTION_HELP_CANCEL,
          picker->cb_data);
    }
    return;
  }

  if (abs_path != NULL && picker->open_file_cb != NULL) {
    picker->open_file_cb(abs_path, line, col, picker->cb_data);
  }
}
