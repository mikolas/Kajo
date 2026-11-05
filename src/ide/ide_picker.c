#include "ide_picker.h"
#include "ide_buffer.h"

#include <gtksourceview/gtksource.h>
#include <string.h>

typedef struct {
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
  GtkWidget          *list_box;
  GtkWidget          *preview_scrolled;
  GtkWidget          *preview_view;
  GtkSourceBuffer    *preview_buf;

  GPtrArray          *all_items;
  GPtrArray          *filtered_items;
  gint                selected_idx;
};

static const struct {
  const char   *category;
  const char   *description;
  const char   *chord;
  IdeActionType action;
} kHelpCommands[] = {
  { "FILES", "Fuzzy Find Files in Workspace", "Space f f", IDE_ACTION_PICKER_OPEN_FILES },
  { "FILES", "Live Workspace Ripgrep Search", "Space f g", IDE_ACTION_PICKER_OPEN_GREP },
  { "FILES", "Switch Open Buffers", "Space b b", IDE_ACTION_PICKER_OPEN_BUFFERS },
  { "FILES", "Save Active Buffer Atomically", "Space f s / Ctrl+S", IDE_ACTION_SAVE },
  { "FILES", "Save All Modified Buffers", "Space f a", IDE_ACTION_SAVE_ALL },
  { "FILES", "Close Active Buffer", "Space b d", IDE_ACTION_BUFFER_CLOSE },
  { "FILES", "Force Close Active Buffer", "Space b D", IDE_ACTION_BUFFER_FORCE_CLOSE },
  { "FILES", "Switch Alternate Header / Source / Test", "Space a", IDE_ACTION_ALTERNATE_FILE },
  { "LAYOUT", "Toggle Project File Tree Sidebar", "Space e", IDE_ACTION_TOGGLE_FILE_TREE },
  { "LAYOUT", "Toggle Ephemeral Bottom Drawer", "Space o", IDE_ACTION_TOGGLE_DRAWER },
  { "LAYOUT", "Cycle Focus Across Panes", "Space w w / Ctrl+W", IDE_ACTION_PANE_CYCLE },
  { "LAYOUT", "Grow / Shrink File Tree Width", "Space w > / <", IDE_ACTION_RESIZE_TREE_GROW },
  { "LAYOUT", "Grow / Shrink Bottom Drawer Height", "Space w + / -", IDE_ACTION_RESIZE_DRAWER_GROW },
  { "SEARCH", "In-Buffer Incremental Search", "/ or Space s f", IDE_ACTION_SEARCH_OPEN },
  { "SEARCH", "Buffer Search & Replace with Live Diff", "Space s r", IDE_ACTION_REPLACE_OPEN },
  { "SEARCH", "Project-Wide Search & Replace with Live Diff", "Space s p", IDE_ACTION_PROJECT_REPLACE_OPEN },
  { "SEARCH", "Clear Search Match Highlights", "Space c h", IDE_ACTION_SEARCH_CLEAR_HIGHLIGHT },
  { "NORDIC", "Nordic Step Forward (Diagnostic / Error / Hunk)", "ä / Space n d", IDE_ACTION_STEP_FORWARD },
  { "NORDIC", "Nordic Step Backward (Diagnostic / Error / Hunk)", "ö / Space p d", IDE_ACTION_STEP_BACKWARD },
  { "NORDIC", "Select Inside / Around Function (m i f / m a f)", "m i f", IDE_ACTION_NONE },
  { "NORDIC", "Select Inside / Around Block {} (m i b / m a b)", "m i b", IDE_ACTION_NONE },
  { "NORDIC", "Select Inside / Around Array [] (m i a / m a a)", "m i a", IDE_ACTION_NONE },
  { "NORDIC", "Select Inside / Around Parens () (m i p / m a p)", "m i p", IDE_ACTION_NONE },
  { "NORDIC", "Select Inside / Around Quotes (m i q / m a q)", "m i q", IDE_ACTION_NONE },
  { "LSP", "Jump to Symbol Definition", "Space g d", IDE_ACTION_LSP_DEFINITION },
  { "LSP", "Jump to Interface / Implementation", "Space g i", IDE_ACTION_LSP_IMPLEMENTATION },
  { "LSP", "Inspect Call Hierarchy in Drawer", "Space g c", IDE_ACTION_LSP_CALL_HIERARCHY },
  { "LSP", "Show Rich Graphical Hover Documentation", "K / Space k", IDE_ACTION_LSP_HOVER },
  { "LSP", "Open Quickfix / Refactor Code Actions", "Space c a", IDE_ACTION_LSP_CODE_ACTION },
  { "LSP", "Rename Symbol Across Workspace", "Space c r", IDE_ACTION_LSP_RENAME },
  { "DEBUG", "DAP: Toggle Breakpoint on Current Line", "F9 / Space d b", IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
  { "DEBUG", "DAP: Start / Launch Debug Session", "Space d d", IDE_ACTION_DAP_START },
  { "DEBUG", "DAP: Continue Execution", "F5 / Space d c", IDE_ACTION_DAP_CONTINUE },
  { "DEBUG", "DAP: Step Over Line", "F10 / Space d n", IDE_ACTION_DAP_STEP_OVER },
  { "DEBUG", "DAP: Step Into Function", "F11 / Space d i", IDE_ACTION_DAP_STEP_INTO },
  { "DEBUG", "DAP: Step Out of Function", "F12 / Space d o", IDE_ACTION_DAP_STEP_OUT },
  { "DEBUG", "DAP: Stop Debug Session", "Shift+F5 / Space d q", IDE_ACTION_DAP_STOP },
  { "DEBUG", "DAP: Toggle 3-Pane Inspector Drawer", "Space d v", IDE_ACTION_DAP_TOGGLE_VIEW },
  { "AGENT", "Open AI Agent Batch Transaction Diff Review", "Space d s", IDE_ACTION_AGENT_REVIEW_OPEN },
  { "AGENT", "Accept All Pending Agent Modifications", "Space d a", IDE_ACTION_AGENT_ACCEPT_ALL },
  { "AGENT", "Atomically Revert All Pending Agent Modifications", "Space d r", IDE_ACTION_AGENT_REVERT_ALL },
  { "MARKDOWN", "Toggle Full GUI Markdown & Vector Mermaid Preview", "Space m p", IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW },
  { "MARKDOWN", "Toggle Markdown Preview Split vs Bottom Drawer", "Space m d", IDE_ACTION_TOGGLE_MERMAID_MODE },
  { "MARKDOWN", "Toggle Markdown Task Checklist Item [ ] / [x]", "Space m t", IDE_ACTION_TOGGLE_CHECKLIST },
  { NULL, NULL, NULL, IDE_ACTION_NONE }
};

static void
ide_picker_item_free(gpointer data)
{
  IdePickerItem *it = data;
  if (it == NULL) {
    return;
  }
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
        /* Contiguous character bonus */
        score += 25;
      }
      if (ci == 0 || candidate[ci - 1] == '/' || candidate[ci - 1] == '_' ||
          candidate[ci - 1] == '-' || candidate[ci - 1] == '.') {
        /* Word/path boundary bonus */
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

static void
ide_picker_update_preview(IdePicker *picker)
{
  if (picker->selected_idx < 0 || (guint)picker->selected_idx >= picker->filtered_items->len) {
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(picker->preview_buf), "", -1);
    return;
  }

  const IdePickerItem *it = g_ptr_array_index(picker->filtered_items, picker->selected_idx);
  if (picker->mode == IDE_PICKER_HELP || it->abs_path == NULL) {
    g_autofree char *doc = g_strdup_printf("Command:  %s\nShortcut: %s",
                                           it->label ? it->label : "",
                                           it->subtitle ? it->subtitle : "");
    gtk_source_buffer_set_language(picker->preview_buf, NULL);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(picker->preview_buf), doc, -1);
    return;
  }

  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(it->abs_path, &contents, &len, NULL)) {
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(picker->preview_buf), "[Unable to read file preview]", -1);
    return;
  }

  /* Cap preview at first 24 KB for instant zero-lag responsiveness */
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
  GtkSourceLanguage *lang = gtk_source_language_manager_guess_language(lm, it->abs_path, NULL);
  gtk_source_buffer_set_language(picker->preview_buf, lang);
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(picker->preview_buf), txt, -1);

  GtkTextIter line_iter;
  guint target_line = (it->line_1based > 0) ? (it->line_1based - 1) : 0;
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(picker->preview_buf), &line_iter, (gint)target_line);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(picker->preview_buf), &line_iter);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(picker->preview_view), &line_iter, 0.2, TRUE, 0.0, 0.35);
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
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(picker->list_box), idx);
  if (row != NULL) {
    gtk_list_box_select_row(GTK_LIST_BOX(picker->list_box), row);
  }
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
    gint s1 = ide_fuzzy_score(query, it->label);
    gint s2 = (it->subtitle != NULL) ? ide_fuzzy_score(query, it->subtitle) : 0;
    gint best = MAX(s1, s2);
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

  guint max_rows = MIN(picker->filtered_items->len, 60u);
  for (guint i = 0; i < max_rows; i++) {
    const IdePickerItem *it = g_ptr_array_index(picker->filtered_items, i);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focusable(row, FALSE);
    gtk_widget_add_css_class(row, "widget-row-card");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl = gtk_label_new(it->label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(lbl, "row-title");
    gtk_box_append(GTK_BOX(hbox), lbl);

    if (it->subtitle != NULL && it->subtitle[0] != '\0') {
      GtkWidget *sub = gtk_label_new(it->subtitle);
      gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
      gtk_widget_set_valign(sub, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(sub,
                               picker->mode == IDE_PICKER_HELP ? "shell-popover-badge" : "row-subtitle");
      gtk_box_append(GTK_BOX(hbox), sub);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(GTK_LIST_BOX(picker->list_box), row);
  }

  g_autofree char *cnt = g_strdup_printf("%u / %u",
                                         picker->filtered_items->len,
                                         picker->all_items->len);
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
on_picker_row_activated(GtkListBox    *box,
                        GtkListBoxRow *row,
                        gpointer       user_data)
{
  IdePicker *picker = user_data;
  if (row == NULL) {
    return;
  }
  picker->selected_idx = gtk_list_box_row_get_index(row);
  ide_picker_confirm(picker);
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
  picker->all_items = g_ptr_array_new_with_free_func(ide_picker_item_free);
  picker->filtered_items = g_ptr_array_new();

  picker->backdrop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(picker->backdrop_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(picker->backdrop_box, GTK_ALIGN_START);
  gtk_widget_set_margin_top(picker->backdrop_box, 52);
  gtk_widget_set_visible(picker->backdrop_box, FALSE);

  picker->card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(picker->card_box, "shell-popover-card");
  gtk_widget_add_css_class(picker->card_box, "ide-picker-modal");
  gtk_widget_set_size_request(picker->card_box, 880, 440);

  /* Header Bar */
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "settings-header-bar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  picker->header_title = gtk_label_new("🔍 FUZZY FIND FILES (Space f f)");
  gtk_label_set_xalign(GTK_LABEL(picker->header_title), 0.0f);
  gtk_widget_set_hexpand(picker->header_title, TRUE);
  gtk_widget_set_valign(picker->header_title, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(picker->header_title, "settings-header-title");
  gtk_box_append(GTK_BOX(hdr), picker->header_title);

  picker->count_badge = gtk_label_new("0 / 0");
  gtk_widget_set_valign(picker->count_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(picker->count_badge, "shell-popover-badge");
  gtk_box_append(GTK_BOX(hdr), picker->count_badge);
  gtk_box_append(GTK_BOX(picker->card_box), hdr);

  /* Search Input */
  picker->search_entry = gtk_entry_new();
  gtk_widget_set_focusable(picker->search_entry, FALSE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(picker->search_entry),
                                 "Type to filter (↑/↓ or Ctrl+P/N, Enter to confirm, Esc to cancel)...");
  gtk_widget_add_css_class(picker->search_entry, "shell-launcher-input");
  gtk_widget_set_margin_start(picker->search_entry, 10);
  gtk_widget_set_margin_end(picker->search_entry, 10);
  gtk_widget_set_margin_top(picker->search_entry, 8);
  gtk_widget_set_margin_bottom(picker->search_entry, 8);
  gtk_box_append(GTK_BOX(picker->card_box), picker->search_entry);

  /* Split Body: Left Match List + Right Syntax-Highlighted Preview */
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(paned), 410);
  gtk_widget_set_vexpand(paned, TRUE);

  GtkWidget *list_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  picker->list_box = gtk_list_box_new();
  gtk_widget_set_focusable(picker->list_box, FALSE);
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(picker->list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(picker->list_box), FALSE);
  g_signal_connect(picker->list_box, "row-activated", G_CALLBACK(on_picker_row_activated), picker);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), picker->list_box);
  gtk_paned_set_start_child(GTK_PANED(paned), list_scroll);

  picker->preview_scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(picker->preview_scrolled),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);

  picker->preview_buf = gtk_source_buffer_new(NULL);
  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(sm, "kajo-metro-dark");
  if (scheme != NULL) {
    gtk_source_buffer_set_style_scheme(picker->preview_buf, scheme);
  }
  picker->preview_view = gtk_source_view_new_with_buffer(picker->preview_buf);
  gtk_widget_set_focusable(picker->preview_view, FALSE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(picker->preview_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(picker->preview_view), TRUE);
  gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(picker->preview_view), TRUE);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(picker->preview_view), TRUE);
  gtk_widget_add_css_class(picker->preview_view, "code-preview-box");

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(picker->preview_scrolled), picker->preview_view);
  gtk_paned_set_end_child(GTK_PANED(paned), picker->preview_scrolled);

  gtk_box_append(GTK_BOX(picker->card_box), paned);
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

void
ide_picker_open_files(IdePicker *picker, const char *workspace_root)
{
  g_return_if_fail(picker != NULL);
  if (workspace_root != NULL) {
    g_free(picker->workspace_root);
    picker->workspace_root = g_strdup(workspace_root);
  }
  picker->mode = IDE_PICKER_FILES;
  gtk_label_set_text(GTK_LABEL(picker->header_title), "🔍 FUZZY FIND FILES (Space f f)");
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
  picker->mode = IDE_PICKER_BUFFERS;
  gtk_label_set_text(GTK_LABEL(picker->header_title), "📑 SWITCH OPEN BUFFER (Space b b)");
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
  picker->mode = IDE_PICKER_GREP;
  gtk_label_set_text(GTK_LABEL(picker->header_title), "⚡ LIVE WORKSPACE RIPGREP (Space f g)");
  gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), "");

  g_ptr_array_set_size(picker->all_items, 0);
  gtk_widget_set_visible(picker->backdrop_box, TRUE);
  ide_picker_rebuild_list(picker);
}

void
ide_picker_open_help(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  picker->mode = IDE_PICKER_HELP;
  gtk_label_set_text(GTK_LABEL(picker->header_title),
                     "❓ COMMAND PALETTE & NORDIC KEYBINDING CHEATSHEET (? / F1)");
  gtk_editable_set_text(GTK_EDITABLE(picker->search_entry), "");

  g_ptr_array_set_size(picker->all_items, 0);
  for (gsize i = 0; kHelpCommands[i].description != NULL; i++) {
    IdePickerItem *it = g_new0(IdePickerItem, 1);
    it->label = g_strdup_printf("[%s]  %s", kHelpCommands[i].category, kHelpCommands[i].description);
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
  ide_picker_select_index(picker, picker->selected_idx + 1);
}

void
ide_picker_move_prev(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  ide_picker_select_index(picker, picker->selected_idx - 1);
}

void
ide_picker_page_down(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  ide_picker_select_index(picker, picker->selected_idx + 8);
}

void
ide_picker_page_up(IdePicker *picker)
{
  g_return_if_fail(picker != NULL);
  ide_picker_select_index(picker, picker->selected_idx - 8);
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
  if (picker->selected_idx < 0 || (guint)picker->selected_idx >= picker->filtered_items->len) {
    ide_picker_close(picker);
    return;
  }

  const IdePickerItem *it = g_ptr_array_index(picker->filtered_items, picker->selected_idx);
  IdePickerMode mode = picker->mode;
  g_autofree char *abs_path = g_strdup(it->abs_path);
  guint line = it->line_1based;
  guint col = it->col_1based;
  IdeActionType cmd = it->cmd_action;

  ide_picker_close(picker);

  if (mode == IDE_PICKER_HELP) {
    if (cmd != IDE_ACTION_NONE && picker->command_cb != NULL) {
      picker->command_cb(cmd, picker->cb_data);
    }
    return;
  }

  if (abs_path != NULL && picker->open_file_cb != NULL) {
    picker->open_file_cb(abs_path, line, col, picker->cb_data);
  }
}
