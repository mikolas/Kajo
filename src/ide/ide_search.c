#include "ide_search.h"

#include <string.h>

struct _IdeSearchDrawer {
  char             *workspace_root;
  IdeSearchJumpCb   jump_cb;
  IdeSearchStatusCb status_cb;
  gpointer          cb_data;

  GtkWidget        *stack;
  GtkWidget        *build_box;
  GtkWidget        *build_list_box;
  GtkWidget        *build_badge;

  GtkWidget        *replace_box;
  GtkWidget        *replace_title;
  GtkWidget        *badge_case;
  GtkWidget        *badge_word;
  GtkWidget        *badge_regex;
  GtkWidget        *badge_matches;
  GtkWidget        *find_entry;
  GtkWidget        *replace_entry;
  GtkWidget        *diff_view;
  GtkTextBuffer    *diff_buf;

  IdeBuffer        *target_buf;
  gboolean          project_wide;
  gboolean          case_sensitive;
  gboolean          whole_word;
  gboolean          use_regex;
  guint             active_field; /* 0 = find, 1 = replace */

  GPtrArray        *build_issues;
  gint              selected_issue_idx;
};

void
ide_build_issue_free(gpointer data)
{
  IdeBuildIssue *iss = data;
  if (iss == NULL) {
    return;
  }
  g_free(iss->file_path);
  g_free(iss->message);
  g_free(iss);
}

char *
ide_replace_text_all(const char *source_text,
                     const char *find_str,
                     const char *replace_str,
                     gboolean    case_sensitive,
                     gboolean    whole_word,
                     gboolean    use_regex,
                     guint      *out_match_count)
{
  if (out_match_count != NULL) {
    *out_match_count = 0;
  }
  if (source_text == NULL) {
    return g_strdup("");
  }
  if (find_str == NULL || find_str[0] == '\0') {
    return g_strdup(source_text);
  }
  if (replace_str == NULL) {
    replace_str = "";
  }

  g_autofree char *escaped = use_regex ? g_strdup(find_str) : g_regex_escape_string(find_str, -1);
  g_autofree char *pattern = whole_word ? g_strdup_printf("\\b(?:%s)\\b", escaped) : g_strdup(escaped);

  GRegexCompileFlags cflags = G_REGEX_MULTILINE;
  if (!case_sensitive) {
    cflags |= G_REGEX_CASELESS;
  }

  g_autoptr(GRegex) rx = g_regex_new(pattern, cflags, 0, NULL);
  if (rx == NULL) {
    return g_strdup(source_text);
  }

  g_autoptr(GMatchInfo) match_info = NULL;
  guint count = 0;
  g_regex_match(rx, source_text, 0, &match_info);
  while (g_match_info_matches(match_info)) {
    count++;
    g_match_info_next(match_info, NULL);
  }
  if (out_match_count != NULL) {
    *out_match_count = count;
  }
  if (count == 0) {
    return g_strdup(source_text);
  }

  char *replaced = NULL;
  if (use_regex) {
    replaced = g_regex_replace(rx, source_text, -1, 0, replace_str, 0, NULL);
  } else {
    replaced = g_regex_replace_literal(rx, source_text, -1, 0, replace_str, 0, NULL);
  }
  return replaced ? replaced : g_strdup(source_text);
}

IdeBuildIssue *
ide_parse_compiler_error_line(const char *workspace_root,
                              const char *raw_line)
{
  if (raw_line == NULL || raw_line[0] == '\0') {
    return NULL;
  }

  /* Match GCC / Clang / Rust / Go format: "path/to/file.c:142:5: error: msg" */
  static GRegex *s_rx = NULL;
  if (s_rx == NULL) {
    s_rx = g_regex_new("^\\s*([^:\\s][^:]*\\.[a-zA-Z0-9_+]+):(\\d+)(?::(\\d+))?:\\s*(?:(fatal error|error|warning|note|info):\\s*)?(.+)$",
                       G_REGEX_OPTIMIZE | G_REGEX_CASELESS,
                       0,
                       NULL);
  }
  if (s_rx == NULL) {
    return NULL;
  }

  g_autoptr(GMatchInfo) mi = NULL;
  if (!g_regex_match(s_rx, raw_line, 0, &mi)) {
    return NULL;
  }

  g_autofree char *raw_file = g_match_info_fetch(mi, 1);
  g_autofree char *line_str = g_match_info_fetch(mi, 2);
  g_autofree char *col_str = g_match_info_fetch(mi, 3);
  g_autofree char *sev_str = g_match_info_fetch(mi, 4);
  g_autofree char *msg_str = g_match_info_fetch(mi, 5);

  IdeBuildIssue *iss = g_new0(IdeBuildIssue, 1);
  if (g_path_is_absolute(raw_file) || workspace_root == NULL) {
    iss->file_path = g_canonicalize_filename(raw_file, NULL);
  } else {
    /* Also check builddir-relative paths "../src/..." */
    g_autofree char *cand1 = g_build_filename(workspace_root, raw_file, NULL);
    if (g_file_test(cand1, G_FILE_TEST_EXISTS)) {
      iss->file_path = g_canonicalize_filename(cand1, NULL);
    } else {
      g_autofree char *bdir = g_build_filename(workspace_root, "builddir", NULL);
      iss->file_path = g_canonicalize_filename(raw_file, bdir);
    }
  }

  iss->line_1based = (guint)MAX(1, g_ascii_strtoll(line_str ? line_str : "1", NULL, 10));
  iss->col_1based = (guint)MAX(1, (col_str && col_str[0]) ? g_ascii_strtoll(col_str, NULL, 10) : 1);

  iss->severity = IDE_DIAG_ERROR;
  if (sev_str != NULL) {
    if (g_ascii_strncasecmp(sev_str, "warn", 4) == 0) {
      iss->severity = IDE_DIAG_WARNING;
    } else if (g_ascii_strncasecmp(sev_str, "note", 4) == 0 ||
               g_ascii_strncasecmp(sev_str, "info", 4) == 0) {
      iss->severity = IDE_DIAG_INFO;
    }
  }
  iss->message = g_strdup(msg_str ? msg_str : raw_line);
  g_strchomp(iss->message);
  return iss;
}

static void
ide_search_drawer_refresh_replace_preview(IdeSearchDrawer *drawer)
{
  gtk_label_set_text(GTK_LABEL(drawer->badge_case),
                     drawer->case_sensitive ? "Aa Alt+C: ON" : "Aa Alt+C: OFF");
  gtk_label_set_text(GTK_LABEL(drawer->badge_word),
                     drawer->whole_word ? "\\b Alt+W: ON" : "\\b Alt+W: OFF");
  gtk_label_set_text(GTK_LABEL(drawer->badge_regex),
                     drawer->use_regex ? ".* Alt+R: ON" : ".* Alt+R: OFF");

  const char *find_str = gtk_editable_get_text(GTK_EDITABLE(drawer->find_entry));
  const char *rep_str = gtk_editable_get_text(GTK_EDITABLE(drawer->replace_entry));

  if (find_str == NULL || find_str[0] == '\0') {
    gtk_label_set_text(GTK_LABEL(drawer->badge_matches), "0 matches");
    gtk_text_buffer_set_text(drawer->diff_buf,
                             "Type a search pattern above to preview unified before/after diff...",
                             -1);
    return;
  }

  if (!drawer->project_wide && drawer->target_buf != NULL) {
    g_autofree char *orig = ide_buffer_get_text(drawer->target_buf);
    guint matches = 0;
    g_autofree char *after = ide_replace_text_all(orig,
                                                  find_str,
                                                  rep_str,
                                                  drawer->case_sensitive,
                                                  drawer->whole_word,
                                                  drawer->use_regex,
                                                  &matches);
    g_autofree char *m_txt = g_strdup_printf("%u matches in %s",
                                             matches,
                                             ide_buffer_get_title(drawer->target_buf));
    gtk_label_set_text(GTK_LABEL(drawer->badge_matches), m_txt);

    g_autofree char *udiff = ide_format_unified_diff(ide_buffer_get_title(drawer->target_buf),
                                                     orig,
                                                     after);
    gtk_text_buffer_set_text(drawer->diff_buf,
                             (udiff && udiff[0]) ? udiff : "[No matching occurrences]",
                             -1);
    return;
  }

  /* Project-wide preview using ripgrep -l */
  const char *argv[] = {
    "rg", "-l", drawer->case_sensitive ? "-s" : "-i",
    "--glob=!.git", "--glob=!builddir", "--", find_str,
    drawer->workspace_root ? drawer->workspace_root : ".", NULL
  };
  g_autofree char *stdout_buf = NULL;
  g_spawn_sync(NULL,
               (char **)argv,
               NULL,
               G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
               NULL,
               NULL,
               &stdout_buf,
               NULL,
               NULL,
               NULL);

  guint total_matches = 0;
  guint total_files = 0;
  GString *combined = g_string_new(NULL);

  if (stdout_buf != NULL) {
    g_auto(GStrv) files = g_strsplit(stdout_buf, "\n", 40);
    for (gsize i = 0; files[i] != NULL && total_files < 20; i++) {
      if (files[i][0] == '\0') {
        continue;
      }
      g_autofree char *orig = NULL;
      if (!g_file_get_contents(files[i], &orig, NULL, NULL)) {
        continue;
      }
      guint file_matches = 0;
      g_autofree char *after = ide_replace_text_all(orig,
                                                    find_str,
                                                    rep_str,
                                                    drawer->case_sensitive,
                                                    drawer->whole_word,
                                                    drawer->use_regex,
                                                    &file_matches);
      if (file_matches > 0) {
        total_matches += file_matches;
        total_files++;
        g_autofree char *base = g_path_get_basename(files[i]);
        g_autofree char *udiff = ide_format_unified_diff(base, orig, after);
        g_string_append(combined, udiff);
        g_string_append_c(combined, '\n');
      }
    }
  }

  g_autofree char *m_txt = g_strdup_printf("%u matches / %u files", total_matches, total_files);
  gtk_label_set_text(GTK_LABEL(drawer->badge_matches), m_txt);
  gtk_text_buffer_set_text(drawer->diff_buf,
                           combined->len > 0 ? combined->str : "[No project matches]",
                           -1);
  g_string_free(combined, TRUE);
}

static void
on_build_row_activated(GtkListBox    *box,
                       GtkListBoxRow *row,
                       gpointer       user_data)
{
  IdeSearchDrawer *drawer = user_data;
  if (row == NULL) {
    return;
  }
  drawer->selected_issue_idx = gtk_list_box_row_get_index(row);
  ide_search_drawer_jump_selected(drawer);
}

IdeSearchDrawer *
ide_search_drawer_new(const char       *workspace_root,
                      IdeSearchJumpCb   jump_cb,
                      IdeSearchStatusCb status_cb,
                      gpointer          user_data)
{
  IdeSearchDrawer *drawer = g_new0(IdeSearchDrawer, 1);
  drawer->workspace_root = g_strdup(workspace_root ? workspace_root : ".");
  drawer->jump_cb = jump_cb;
  drawer->status_cb = status_cb;
  drawer->cb_data = user_data;
  drawer->build_issues = g_ptr_array_new_with_free_func(ide_build_issue_free);
  drawer->selected_issue_idx = -1;

  drawer->stack = gtk_stack_new();
  gtk_widget_set_size_request(drawer->stack, -1, 210);

  /* 1. Build & Quickfix Output View */
  drawer->build_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bhdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(bhdr, "settings-header-bar");
  gtk_widget_set_valign(bhdr, GTK_ALIGN_CENTER);

  GtkWidget *btitle = gtk_label_new("⚙ BUILD & QUICKFIX OUTPUT (Space o) — [Enter: Jump | ö/ä: Step | Esc/q: Close]");
  gtk_label_set_xalign(GTK_LABEL(btitle), 0.0f);
  gtk_widget_set_hexpand(btitle, TRUE);
  gtk_widget_set_valign(btitle, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(btitle, "settings-header-title");
  gtk_box_append(GTK_BOX(bhdr), btitle);

  drawer->build_badge = gtk_label_new("READY");
  gtk_widget_set_valign(drawer->build_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(drawer->build_badge, "shell-popover-badge");
  gtk_box_append(GTK_BOX(bhdr), drawer->build_badge);
  gtk_box_append(GTK_BOX(drawer->build_box), bhdr);

  GtkWidget *bscroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(bscroll, TRUE);
  drawer->build_list_box = gtk_list_box_new();
  gtk_widget_set_focusable(drawer->build_list_box, FALSE);
  gtk_widget_add_css_class(drawer->build_list_box, "ide-tree-listbox");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(drawer->build_list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(drawer->build_list_box), FALSE);
  g_signal_connect(drawer->build_list_box, "row-activated", G_CALLBACK(on_build_row_activated), drawer);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(bscroll), drawer->build_list_box);
  gtk_box_append(GTK_BOX(drawer->build_box), bscroll);

  gtk_stack_add_named(GTK_STACK(drawer->stack), drawer->build_box, "build");

  /* 2. Interactive Search & Replace View */
  drawer->replace_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *rhdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(rhdr, "settings-header-bar");
  gtk_widget_set_valign(rhdr, GTK_ALIGN_CENTER);

  drawer->replace_title = gtk_label_new("🔎 SEARCH & REPLACE (Space s r)");
  gtk_label_set_xalign(GTK_LABEL(drawer->replace_title), 0.0f);
  gtk_widget_set_hexpand(drawer->replace_title, TRUE);
  gtk_widget_set_valign(drawer->replace_title, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(drawer->replace_title, "settings-header-title");
  gtk_box_append(GTK_BOX(rhdr), drawer->replace_title);

  drawer->badge_case = gtk_label_new("Aa Alt+C: OFF");
  gtk_widget_add_css_class(drawer->badge_case, "shell-popover-badge");
  gtk_box_append(GTK_BOX(rhdr), drawer->badge_case);

  drawer->badge_word = gtk_label_new("\\b Alt+W: OFF");
  gtk_widget_add_css_class(drawer->badge_word, "shell-popover-badge");
  gtk_box_append(GTK_BOX(rhdr), drawer->badge_word);

  drawer->badge_regex = gtk_label_new(".* Alt+R: OFF");
  gtk_widget_add_css_class(drawer->badge_regex, "shell-popover-badge");
  gtk_box_append(GTK_BOX(rhdr), drawer->badge_regex);

  drawer->badge_matches = gtk_label_new("0 matches");
  gtk_widget_add_css_class(drawer->badge_matches, "shell-popover-badge");
  gtk_box_append(GTK_BOX(rhdr), drawer->badge_matches);
  gtk_box_append(GTK_BOX(drawer->replace_box), rhdr);

  GtkWidget *inputs_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(inputs_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(inputs_row, 10);
  gtk_widget_set_margin_end(inputs_row, 10);
  gtk_widget_set_margin_top(inputs_row, 6);
  gtk_widget_set_margin_bottom(inputs_row, 6);

  GtkWidget *flbl = gtk_label_new("FIND:");
  gtk_widget_add_css_class(flbl, "shell-popover-badge");
  gtk_box_append(GTK_BOX(inputs_row), flbl);

  drawer->find_entry = gtk_entry_new();
  gtk_widget_set_focusable(drawer->find_entry, FALSE);
  gtk_widget_set_hexpand(drawer->find_entry, TRUE);
  gtk_widget_add_css_class(drawer->find_entry, "shell-launcher-input");
  gtk_box_append(GTK_BOX(inputs_row), drawer->find_entry);

  GtkWidget *rlbl = gtk_label_new("REPLACE:");
  gtk_widget_add_css_class(rlbl, "shell-popover-badge");
  gtk_box_append(GTK_BOX(inputs_row), rlbl);

  drawer->replace_entry = gtk_entry_new();
  gtk_widget_set_focusable(drawer->replace_entry, FALSE);
  gtk_widget_set_hexpand(drawer->replace_entry, TRUE);
  gtk_widget_add_css_class(drawer->replace_entry, "shell-launcher-input");
  gtk_box_append(GTK_BOX(inputs_row), drawer->replace_entry);

  gtk_box_append(GTK_BOX(drawer->replace_box), inputs_row);

  GtkWidget *dscroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(dscroll, TRUE);
  drawer->diff_buf = gtk_text_buffer_new(NULL);
  drawer->diff_view = gtk_text_view_new_with_buffer(drawer->diff_buf);
  gtk_widget_set_focusable(drawer->diff_view, FALSE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(drawer->diff_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(drawer->diff_view), TRUE);
  gtk_widget_add_css_class(drawer->diff_view, "code-preview-box");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dscroll), drawer->diff_view);
  gtk_box_append(GTK_BOX(drawer->replace_box), dscroll);

  gtk_stack_add_named(GTK_STACK(drawer->stack), drawer->replace_box, "replace");
  return drawer;
}

void
ide_search_drawer_free(IdeSearchDrawer *drawer)
{
  if (drawer == NULL) {
    return;
  }
  g_free(drawer->workspace_root);
  g_clear_pointer(&drawer->build_issues, g_ptr_array_unref);
  g_clear_object(&drawer->diff_buf);
  g_free(drawer);
}

GtkWidget *
ide_search_drawer_get_widget(IdeSearchDrawer *drawer)
{
  g_return_val_if_fail(drawer != NULL, NULL);
  return drawer->stack;
}

void
ide_search_drawer_set_workspace_root(IdeSearchDrawer *drawer, const char *workspace_root)
{
  g_return_if_fail(drawer != NULL);
  g_free(drawer->workspace_root);
  drawer->workspace_root = g_strdup(workspace_root ? workspace_root : ".");
}

void
ide_search_drawer_show_build_output(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  gtk_stack_set_visible_child_name(GTK_STACK(drawer->stack), "build");
}

void
ide_search_drawer_append_output_line(IdeSearchDrawer *drawer, const char *line)
{
  g_return_if_fail(drawer != NULL);
  if (line == NULL || line[0] == '\0') {
    return;
  }

  IdeBuildIssue *iss = ide_parse_compiler_error_line(drawer->workspace_root, line);
  GtkWidget *row = gtk_list_box_row_new();
  gtk_widget_add_css_class(row, "widget-row-card");

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

  if (iss != NULL) {
    const char *b_txt = (iss->severity == IDE_DIAG_ERROR) ? "✖ ERROR" :
                        (iss->severity == IDE_DIAG_WARNING) ? "⚠ WARN" : "ℹ NOTE";
    GtkWidget *badge = gtk_label_new(b_txt);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    gtk_box_append(GTK_BOX(hbox), badge);

    g_autofree char *base = g_path_get_basename(iss->file_path);
    g_autofree char *summary = g_strdup_printf("%s:%u:%u: %s",
                                               base,
                                               iss->line_1based,
                                               iss->col_1based,
                                               iss->message);
    GtkWidget *lbl = gtk_label_new(summary);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), lbl);
    g_ptr_array_add(drawer->build_issues, iss);
  } else {
    IdeBuildIssue *dummy = g_new0(IdeBuildIssue, 1);
    dummy->message = g_strdup(line);
    g_ptr_array_add(drawer->build_issues, dummy);

    GtkWidget *lbl = gtk_label_new(line);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(lbl, "row-subtitle");
    gtk_box_append(GTK_BOX(hbox), lbl);
  }

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
  gtk_list_box_append(GTK_LIST_BOX(drawer->build_list_box), row);
}

void
ide_search_drawer_run_build(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  ide_search_drawer_show_build_output(drawer);

  GtkWidget *child = gtk_widget_get_first_child(drawer->build_list_box);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(drawer->build_list_box), child);
    child = next;
  }
  g_ptr_array_set_size(drawer->build_issues, 0);

  /* Auto-detect build system in workspace_root */
  const char *root = drawer->workspace_root ? drawer->workspace_root : ".";
  g_autofree char *meson_f = g_build_filename(root, "meson.build", NULL);
  g_autofree char *cargo_f = g_build_filename(root, "Cargo.toml", NULL);
  g_autofree char *gomod_f = g_build_filename(root, "go.mod", NULL);

  const char *cmd = "make";
  if (g_file_test(meson_f, G_FILE_TEST_EXISTS)) {
    cmd = "meson compile -C builddir";
  } else if (g_file_test(cargo_f, G_FILE_TEST_EXISTS)) {
    cmd = "cargo check";
  } else if (g_file_test(gomod_f, G_FILE_TEST_EXISTS)) {
    cmd = "go build ./...";
  }

  const char *argv[] = { "sh", "-c", cmd, NULL };
  g_autofree char *out_str = NULL;
  g_autofree char *err_str = NULL;
  gint wait_status = 0;

  g_spawn_sync(root,
               (char **)argv,
               NULL,
               G_SPAWN_SEARCH_PATH,
               NULL,
               NULL,
               &out_str,
               &err_str,
               &wait_status,
               NULL);

  g_autofree char *merged = g_strconcat(out_str ? out_str : "", "\n", err_str ? err_str : "", NULL);
  g_auto(GStrv) lines = g_strsplit(merged, "\n", 200);
  guint err_count = 0;
  for (gsize i = 0; lines[i] != NULL; i++) {
    if (lines[i][0] != '\0') {
      ide_search_drawer_append_output_line(drawer, lines[i]);
    }
  }
  for (guint i = 0; i < drawer->build_issues->len; i++) {
    const IdeBuildIssue *iss = g_ptr_array_index(drawer->build_issues, i);
    if (iss->file_path != NULL && iss->severity == IDE_DIAG_ERROR) {
      err_count++;
    }
  }

  g_autofree char *btxt = (wait_status == 0)
                              ? g_strdup("✓ BUILD OK")
                              : g_strdup_printf("✖ %u ERRORS", err_count);
  gtk_label_set_text(GTK_LABEL(drawer->build_badge), btxt);

  if (drawer->build_issues->len > 0) {
    drawer->selected_issue_idx = 0;
    GtkListBoxRow *r0 = gtk_list_box_get_row_at_index(GTK_LIST_BOX(drawer->build_list_box), 0);
    if (r0 != NULL) {
      gtk_list_box_select_row(GTK_LIST_BOX(drawer->build_list_box), r0);
    }
  }
}

void
ide_search_drawer_open_replace(IdeSearchDrawer *drawer,
                               IdeBuffer       *active_buf,
                               gboolean         project_wide)
{
  g_return_if_fail(drawer != NULL);
  drawer->target_buf = active_buf;
  drawer->project_wide = project_wide;
  drawer->active_field = 0;

  gtk_label_set_text(GTK_LABEL(drawer->replace_title),
                     project_wide
                         ? "🔎 PROJECT SEARCH & REPLACE (Space s p) — [Tab: Next Field | Alt+A/Enter: Apply All]"
                         : "🔎 BUFFER SEARCH & REPLACE (Space s r) — [Tab: Next Field | Alt+A/Enter: Apply All]");
  gtk_stack_set_visible_child_name(GTK_STACK(drawer->stack), "replace");
  ide_search_drawer_refresh_replace_preview(drawer);
}

void
ide_search_drawer_move_down(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  if (drawer->build_issues->len == 0) {
    return;
  }
  drawer->selected_issue_idx = CLAMP(drawer->selected_issue_idx + 1,
                                     0,
                                     (gint)drawer->build_issues->len - 1);
  GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(drawer->build_list_box),
                                                   drawer->selected_issue_idx);
  if (r != NULL) {
    gtk_list_box_select_row(GTK_LIST_BOX(drawer->build_list_box), r);
  }
}

void
ide_search_drawer_move_up(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  if (drawer->build_issues->len == 0) {
    return;
  }
  drawer->selected_issue_idx = CLAMP(drawer->selected_issue_idx - 1,
                                     0,
                                     (gint)drawer->build_issues->len - 1);
  GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(drawer->build_list_box),
                                                   drawer->selected_issue_idx);
  if (r != NULL) {
    gtk_list_box_select_row(GTK_LIST_BOX(drawer->build_list_box), r);
  }
}

void
ide_search_drawer_jump_selected(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  if (drawer->selected_issue_idx < 0 ||
      (guint)drawer->selected_issue_idx >= drawer->build_issues->len) {
    return;
  }
  const IdeBuildIssue *iss = g_ptr_array_index(drawer->build_issues, drawer->selected_issue_idx);
  if (iss->file_path != NULL && drawer->jump_cb != NULL) {
    drawer->jump_cb(iss->file_path, iss->line_1based, iss->col_1based, drawer->cb_data);
  }
}

gboolean
ide_search_drawer_step_error(IdeSearchDrawer *drawer, int delta)
{
  g_return_val_if_fail(drawer != NULL, FALSE);
  if (drawer->build_issues->len == 0) {
    return FALSE;
  }

  gint n = (gint)drawer->build_issues->len;
  gint start = (drawer->selected_issue_idx < 0) ? 0 : drawer->selected_issue_idx;
  for (gint step = 1; step <= n; step++) {
    gint idx = (start + step * delta) % n;
    if (idx < 0) {
      idx += n;
    }
    const IdeBuildIssue *iss = g_ptr_array_index(drawer->build_issues, idx);
    if (iss->file_path != NULL) {
      drawer->selected_issue_idx = idx;
      ide_search_drawer_jump_selected(drawer);
      return TRUE;
    }
  }
  return FALSE;
}

void
ide_search_drawer_next_field(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  drawer->active_field = (drawer->active_field + 1) % 2;
}

void
ide_search_drawer_prev_field(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  drawer->active_field = (drawer->active_field + 1) % 2;
}

void
ide_search_drawer_toggle_case(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  drawer->case_sensitive = !drawer->case_sensitive;
  ide_search_drawer_refresh_replace_preview(drawer);
}

void
ide_search_drawer_toggle_word(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  drawer->whole_word = !drawer->whole_word;
  ide_search_drawer_refresh_replace_preview(drawer);
}

void
ide_search_drawer_toggle_regex(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  drawer->use_regex = !drawer->use_regex;
  ide_search_drawer_refresh_replace_preview(drawer);
}

void
ide_search_drawer_append_char(IdeSearchDrawer *drawer, const char *ch)
{
  g_return_if_fail(drawer != NULL);
  GtkWidget *entry = (drawer->active_field == 0) ? drawer->find_entry : drawer->replace_entry;
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(entry));
  g_autofree char *next = g_strconcat(cur ? cur : "", ch ? ch : "", NULL);
  gtk_editable_set_text(GTK_EDITABLE(entry), next);
  ide_search_drawer_refresh_replace_preview(drawer);
}

void
ide_search_drawer_backspace(IdeSearchDrawer *drawer)
{
  g_return_if_fail(drawer != NULL);
  GtkWidget *entry = (drawer->active_field == 0) ? drawer->find_entry : drawer->replace_entry;
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (cur == NULL || cur[0] == '\0') {
    return;
  }
  glong chars = g_utf8_strlen(cur, -1);
  if (chars <= 1) {
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
  } else {
    const char *end = g_utf8_offset_to_pointer(cur, chars - 1);
    g_autofree char *trimmed = g_strndup(cur, (gsize)(end - cur));
    gtk_editable_set_text(GTK_EDITABLE(entry), trimmed);
  }
  ide_search_drawer_refresh_replace_preview(drawer);
}

guint
ide_search_drawer_apply_all(IdeSearchDrawer *drawer, GPtrArray *open_buffers)
{
  g_return_val_if_fail(drawer != NULL, 0);
  const char *find_str = gtk_editable_get_text(GTK_EDITABLE(drawer->find_entry));
  const char *rep_str = gtk_editable_get_text(GTK_EDITABLE(drawer->replace_entry));
  if (find_str == NULL || find_str[0] == '\0') {
    return 0;
  }

  guint total_replaced = 0;

  if (!drawer->project_wide && drawer->target_buf != NULL) {
    g_autofree char *orig = ide_buffer_get_text(drawer->target_buf);
    guint cnt = 0;
    g_autofree char *after = ide_replace_text_all(orig,
                                                  find_str,
                                                  rep_str,
                                                  drawer->case_sensitive,
                                                  drawer->whole_word,
                                                  drawer->use_regex,
                                                  &cnt);
    if (cnt > 0) {
      ide_buffer_set_text(drawer->target_buf, after);
      total_replaced += cnt;
    }
    ide_search_drawer_refresh_replace_preview(drawer);
    return total_replaced;
  }

  /* Project-wide atomic replacement */
  if (open_buffers != NULL) {
    for (guint i = 0; i < open_buffers->len; i++) {
      IdeBuffer *b = g_ptr_array_index(open_buffers, i);
      g_autofree char *orig = ide_buffer_get_text(b);
      guint cnt = 0;
      g_autofree char *after = ide_replace_text_all(orig,
                                                    find_str,
                                                    rep_str,
                                                    drawer->case_sensitive,
                                                    drawer->whole_word,
                                                    drawer->use_regex,
                                                    &cnt);
      if (cnt > 0) {
        ide_buffer_set_text(b, after);
        ide_buffer_save(b, NULL);
        total_replaced += cnt;
      }
    }
  }
  ide_search_drawer_refresh_replace_preview(drawer);
  return total_replaced;
}
