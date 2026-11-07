#include "ide_search.h"
#include "ide_tasks.h"

#include <string.h>

/* Files larger than this (or not valid UTF-8) are never previewed or rewritten. */
#define IDE_PROJECT_REPLACE_MAX_BYTES (2u * 1024u * 1024u)

struct _IdeSearchDrawer {
  char             *workspace_root;
  IdeSearchJumpCb   jump_cb;
  IdeSearchStatusCb status_cb;
  gpointer          cb_data;

  GtkWidget        *stack;
  GtkWidget        *build_box;
  GtkWidget        *build_list_box;
  GtkWidget        *build_badge;
  GCancellable     *build_cancel; /* cancels the in-flight build; also the drawer-alive token */
  GSubprocess      *build_proc;

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
  gboolean          confirm_armed; /* project apply pressed once; second press writes disk files */
  guint             active_field; /* 0 = find, 1 = replace */
  GCancellable     *replace_cancel;
  GSubprocess      *replace_proc;
  GPtrArray        *matched_files;

  GPtrArray        *build_issues;
  gint              selected_issue_idx;
  char             *task_label;  /* BUILD / TEST / RUN / CLEAN of the last task */
  char             *parse_root;  /* cwd of the last task: base for relative paths */
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

  /* Maven + javac: "[ERROR] /abs/App.java:[15,25] incompatible types: ..." */
  static GRegex *s_mvn = NULL;
  if (s_mvn == NULL) {
    s_mvn = g_regex_new("^\\[(ERROR|WARNING|INFO)\\]\\s+(\\S.*?\\.java):\\[(\\d+),(\\d+)\\]\\s*(.*)$",
                        G_REGEX_OPTIMIZE, 0, NULL);
  }
  g_autoptr(GMatchInfo) mvn_mi = NULL;
  if (s_mvn != NULL && g_regex_match(s_mvn, raw_line, 0, &mvn_mi)) {
    g_autofree char *sev = g_match_info_fetch(mvn_mi, 1);
    g_autofree char *file = g_match_info_fetch(mvn_mi, 2);
    g_autofree char *ln = g_match_info_fetch(mvn_mi, 3);
    g_autofree char *col = g_match_info_fetch(mvn_mi, 4);
    g_autofree char *msg = g_match_info_fetch(mvn_mi, 5);
    IdeBuildIssue *iss = g_new0(IdeBuildIssue, 1);
    iss->file_path = (g_path_is_absolute(file) || workspace_root == NULL)
                         ? g_canonicalize_filename(file, NULL)
                         : g_canonicalize_filename(file, workspace_root);
    iss->line_1based = (guint)MAX(1, g_ascii_strtoll(ln, NULL, 10));
    iss->col_1based = (guint)MAX(1, g_ascii_strtoll(col, NULL, 10));
    iss->severity = g_strcmp0(sev, "ERROR") == 0 ? IDE_DIAG_ERROR
                    : g_strcmp0(sev, "WARNING") == 0 ? IDE_DIAG_WARNING : IDE_DIAG_INFO;
    iss->message = g_strdup(msg[0] ? msg : raw_line);
    return iss;
  }

  /* vasm: error 2001 in line 3 of "main.s": instruction not supported ... */
  static GRegex *s_vasm = NULL;
  if (s_vasm == NULL) {
    s_vasm = g_regex_new("^\\s*(fatal error|error|warning|message)\\s+(\\d+)\\s+in line\\s+(\\d+)\\s+of\\s+\"([^\"]+)\":\\s*(.*)$",
                         G_REGEX_OPTIMIZE | G_REGEX_CASELESS, 0, NULL);
  }
  g_autoptr(GMatchInfo) vasm_mi = NULL;
  if (s_vasm != NULL && g_regex_match(s_vasm, raw_line, 0, &vasm_mi)) {
    g_autofree char *sev = g_match_info_fetch(vasm_mi, 1);
    g_autofree char *code = g_match_info_fetch(vasm_mi, 2);
    g_autofree char *ln = g_match_info_fetch(vasm_mi, 3);
    g_autofree char *file = g_match_info_fetch(vasm_mi, 4);
    g_autofree char *msg = g_match_info_fetch(vasm_mi, 5);
    IdeBuildIssue *iss = g_new0(IdeBuildIssue, 1);
    iss->file_path = (g_path_is_absolute(file) || workspace_root == NULL)
                         ? g_canonicalize_filename(file, NULL)
                         : g_canonicalize_filename(file, workspace_root);
    iss->line_1based = (guint)MAX(1, g_ascii_strtoll(ln, NULL, 10));
    iss->col_1based = 1;
    iss->severity = g_ascii_strncasecmp(sev, "warning", 7) == 0 ? IDE_DIAG_WARNING
                    : g_ascii_strncasecmp(sev, "message", 7) == 0 ? IDE_DIAG_INFO
                                                                  : IDE_DIAG_ERROR;
    iss->message = g_strdup_printf("[%s] %s", code, msg);
    return iss;
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

typedef struct {
  IdeSearchDrawer *drawer;
  GCancellable    *cancellable;
} SearchAsyncCtx;

static SearchAsyncCtx *
search_async_ctx_new(IdeSearchDrawer *drawer, GCancellable *cancellable)
{
  SearchAsyncCtx *ctx = g_new0(SearchAsyncCtx, 1);
  ctx->drawer = drawer;
  ctx->cancellable = g_object_ref(cancellable);
  return ctx;
}

static void
search_async_ctx_free(SearchAsyncCtx *ctx)
{
  if (ctx == NULL) {
    return;
  }
  g_clear_object(&ctx->cancellable);
  g_free(ctx);
}

static void
on_project_replace_rg_finished(GObject *source, GAsyncResult *res, gpointer user_data)
{
  SearchAsyncCtx *ctx = user_data;
  g_autofree char *stdout_buf = NULL;
  g_autoptr(GError) err = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res,
                                                     &stdout_buf, NULL, &err);
  if (g_cancellable_is_cancelled(ctx->cancellable) ||
      (!ok && err != NULL && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))) {
    search_async_ctx_free(ctx);
    return;
  }
  IdeSearchDrawer *drawer = ctx->drawer;
  search_async_ctx_free(ctx);

  g_clear_object(&drawer->replace_proc);
  g_clear_object(&drawer->replace_cancel);

  const char *find_str = gtk_editable_get_text(GTK_EDITABLE(drawer->find_entry));
  const char *rep_str = gtk_editable_get_text(GTK_EDITABLE(drawer->replace_entry));
  g_ptr_array_set_size(drawer->matched_files, 0);

  if (find_str == NULL || find_str[0] == '\0') {
    return;
  }

  guint total_matches = 0;
  guint total_files = 0;
  GString *combined = g_string_new(NULL);

  if (stdout_buf != NULL) {
    g_auto(GStrv) files = g_strsplit(stdout_buf, "\n", -1);
    for (gsize i = 0; files[i] != NULL; i++) {
      if (files[i][0] == '\0') {
        continue;
      }
      g_autofree char *orig = NULL;
      gsize orig_len = 0;
      if (!g_file_get_contents(files[i], &orig, &orig_len, NULL) ||
          orig_len > IDE_PROJECT_REPLACE_MAX_BYTES ||
          !g_utf8_validate(orig, (gssize)orig_len, NULL)) {
        continue; /* too large or not UTF-8 text: never rewritten */
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
        g_ptr_array_add(drawer->matched_files, g_strdup(files[i]));
        if (total_files <= 20) {
          g_autofree char *base = g_path_get_basename(files[i]);
          g_autofree char *udiff = ide_format_unified_diff(base, orig, after);
          g_string_append(combined, udiff);
          g_string_append_c(combined, '\n');
        }
      }
    }
    if (total_files > 20) {
      g_string_append_printf(combined,
                             "... (+%u more matched files not shown in preview; Apply All updates all %u files)\n",
                             total_files - 20, total_files);
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
ide_search_drawer_refresh_replace_preview(IdeSearchDrawer *drawer)
{
  if (drawer->replace_cancel != NULL) {
    g_cancellable_cancel(drawer->replace_cancel);
  }
  if (drawer->replace_proc != NULL) {
    g_subprocess_force_exit(drawer->replace_proc);
  }
  g_clear_object(&drawer->replace_proc);
  g_clear_object(&drawer->replace_cancel);
  g_ptr_array_set_size(drawer->matched_files, 0);
  drawer->confirm_armed = FALSE;

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

  /* Project-wide preview using async ripgrep -l */
  g_autoptr(GPtrArray) argv = g_ptr_array_new();
  g_ptr_array_add(argv, (gpointer)"rg");
  /* ripgrep is only a candidate prefilter; ide_replace_text_all() decides the real
   * matches. Literal mode narrows with -F (a superset of the word-bounded matches, so
   * no -w). Regex mode must not hand the pattern to ripgrep (Rust regex syntax differs
   * from GRegex/PCRE), so every non-ignored file is a candidate. */
  if (drawer->use_regex) {
    g_ptr_array_add(argv, (gpointer)"--files");
  } else {
    g_ptr_array_add(argv, (gpointer)"-l");
    g_ptr_array_add(argv, (gpointer)(drawer->case_sensitive ? "-s" : "-i"));
    g_ptr_array_add(argv, (gpointer)"-F");
  }
  g_ptr_array_add(argv, (gpointer)"--glob=!.git");
  g_ptr_array_add(argv, (gpointer)"--glob=!builddir");
  g_ptr_array_add(argv, (gpointer)"--");
  if (!drawer->use_regex) {
    g_ptr_array_add(argv, (gpointer)find_str);
  }
  g_ptr_array_add(argv, (gpointer)(drawer->workspace_root ? drawer->workspace_root : "."));
  g_ptr_array_add(argv, NULL);

  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  drawer->replace_proc = g_subprocess_launcher_spawnv(
      launcher, (const char *const *)argv->pdata, NULL);
  if (drawer->replace_proc == NULL) {
    gtk_label_set_text(GTK_LABEL(drawer->badge_matches), "rg unavailable");
    return;
  }
  gtk_label_set_text(GTK_LABEL(drawer->badge_matches), "Searching…");
  drawer->replace_cancel = g_cancellable_new();
  g_subprocess_communicate_utf8_async(
      drawer->replace_proc, NULL, drawer->replace_cancel,
      on_project_replace_rg_finished,
      search_async_ctx_new(drawer, drawer->replace_cancel));
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
  drawer->matched_files = g_ptr_array_new_with_free_func(g_free);
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
  if (drawer->build_cancel != NULL) {
    g_cancellable_cancel(drawer->build_cancel);
  }
  if (drawer->build_proc != NULL) {
    g_subprocess_force_exit(drawer->build_proc);
  }
  if (drawer->replace_cancel != NULL) {
    g_cancellable_cancel(drawer->replace_cancel);
  }
  if (drawer->replace_proc != NULL) {
    g_subprocess_force_exit(drawer->replace_proc);
  }
  g_clear_object(&drawer->replace_proc);
  g_clear_object(&drawer->replace_cancel);
  g_clear_pointer(&drawer->matched_files, g_ptr_array_unref);
  g_clear_object(&drawer->build_proc);
  g_clear_object(&drawer->build_cancel);
  g_free(drawer->workspace_root);
  g_free(drawer->task_label);
  g_free(drawer->parse_root);
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
  char *dup = g_strdup(workspace_root ? workspace_root : ".");
  g_free(drawer->workspace_root);
  drawer->workspace_root = dup;
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

  IdeBuildIssue *iss = ide_parse_compiler_error_line(
      drawer->parse_root ? drawer->parse_root : drawer->workspace_root, line);
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

static void
on_build_finished(GObject *source, GAsyncResult *res, gpointer user_data)
{
  SearchAsyncCtx *ctx = user_data;
  g_autofree char *out_str = NULL;
  g_autofree char *err_str = NULL;
  g_autoptr(GError) err = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res,
                                                     &out_str, &err_str, &err);
  /* Cancelled means the drawer was freed or a newer build replaced this one:
   * never touch `drawer` in that case. */
  if (g_cancellable_is_cancelled(ctx->cancellable) ||
      (!ok && err != NULL && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))) {
    search_async_ctx_free(ctx);
    return;
  }
  IdeSearchDrawer *drawer = ctx->drawer;
  search_async_ctx_free(ctx);

  gint wait_status = ok ? g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
  g_clear_object(&drawer->build_proc);
  g_clear_object(&drawer->build_cancel);

  g_autofree char *merged = g_strconcat(out_str ? out_str : "", "\n", err_str ? err_str : "", NULL);
  /* Keep every diagnostic line but only the tail of plain output: Maven and
   * Gradle print hundreds of lines before the errors that matter. */
  g_auto(GStrv) lines = g_strsplit(merged, "\n", -1);
  gsize n_lines = g_strv_length(lines);
  gsize tail_from = n_lines > 400 ? n_lines - 400 : 0;
  guint err_count = 0;
  for (gsize i = 0; lines[i] != NULL; i++) {
    if (lines[i][0] == '\0') {
      continue;
    }
    if (i >= tail_from) {
      ide_search_drawer_append_output_line(drawer, lines[i]);
    } else {
      IdeBuildIssue *early = ide_parse_compiler_error_line(
          drawer->parse_root ? drawer->parse_root : drawer->workspace_root, lines[i]);
      if (early != NULL) {
        ide_build_issue_free(early);
        ide_search_drawer_append_output_line(drawer, lines[i]);
      }
    }
  }
  for (guint i = 0; i < drawer->build_issues->len; i++) {
    const IdeBuildIssue *iss = g_ptr_array_index(drawer->build_issues, i);
    if (iss->file_path != NULL && iss->severity == IDE_DIAG_ERROR) {
      err_count++;
    }
  }

  g_autofree char *btxt = (wait_status == 0)
                              ? g_strdup_printf("✓ %s OK", drawer->task_label ? drawer->task_label : "BUILD")
                              : (err_count > 0 ? g_strdup_printf("✖ %u ERRORS", err_count)
                                               : g_strdup_printf("✖ FAILED (exit %d)", wait_status));
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
ide_search_drawer_run_task(IdeSearchDrawer *drawer, int kind, const char *active_file)
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

  const char *root = drawer->workspace_root ? drawer->workspace_root : ".";
  IdeTask *task = ide_task_resolve(root, active_file, (IdeTaskKind)kind);
  if (task == NULL) {
    static const char *const kNames[] = { "build", "test", "run", "clean" };
    g_autofree char *msg = g_strdup_printf("No %s task for this project", kNames[CLAMP(kind, 0, 3)]);
    gtk_label_set_text(GTK_LABEL(drawer->build_badge), "ℹ NO TASK");
    ide_search_drawer_append_output_line(drawer, msg);
    return;
  }

  /* Run without blocking the GTK main loop: a long `mvn test` or
   * `cargo check` must not freeze the IDE. */
  if (drawer->build_cancel != NULL) {
    g_cancellable_cancel(drawer->build_cancel);
  }
  if (drawer->build_proc != NULL) {
    g_subprocess_force_exit(drawer->build_proc);
  }
  g_clear_object(&drawer->build_proc);
  g_clear_object(&drawer->build_cancel);

  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_set_cwd(launcher, task->cwd);
  g_free(drawer->task_label);
  static const char *const kBadge[] = { "BUILD", "TEST", "RUN", "CLEAN" };
  drawer->task_label = g_strdup(kBadge[CLAMP(kind, 0, 3)]);
  /* paths in compiler output are relative to the task's working directory */
  g_free(drawer->parse_root);
  drawer->parse_root = g_strdup(task->cwd);

  g_autoptr(GError) err = NULL;
  drawer->build_proc = g_subprocess_launcher_spawn(launcher, &err, "sh", "-c", task->command, NULL);
  if (drawer->build_proc == NULL) {
    gtk_label_set_text(GTK_LABEL(drawer->build_badge), "✖ FAILED TO START");
    ide_search_drawer_append_output_line(drawer, err != NULL ? err->message : "spawn failed");
    ide_task_free(task);
    return;
  }

  g_autofree char *badge = g_strdup_printf("⟳ %s…", task->label);
  gtk_label_set_text(GTK_LABEL(drawer->build_badge), badge);
  ide_search_drawer_append_output_line(drawer, task->command);
  ide_task_free(task);
  drawer->build_cancel = g_cancellable_new();
  g_subprocess_communicate_utf8_async(
      drawer->build_proc, NULL, drawer->build_cancel,
      on_build_finished,
      search_async_ctx_new(drawer, drawer->build_cancel));
}

void
ide_search_drawer_run_build(IdeSearchDrawer *drawer)
{
  ide_search_drawer_run_task(drawer, IDE_TASK_BUILD, NULL);
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

  /* Project-wide atomic replacement across open buffers and matched disk files */
  if (drawer->replace_proc != NULL) {
    if (drawer->status_cb != NULL) {
      drawer->status_cb("Project search still in progress — wait for preview before applying",
                        drawer->cb_data);
    }
    return 0;
  }

  g_autoptr(GHashTable) seen_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (open_buffers != NULL) {
    for (guint i = 0; i < open_buffers->len; i++) {
      const char *bp = ide_buffer_get_file_path(g_ptr_array_index(open_buffers, i));
      if (bp != NULL) {
        g_hash_table_add(seen_paths, g_canonicalize_filename(bp, NULL));
      }
    }
  }

  /* Disk-only files are rewritten without undo: require a second, explicit press. */
  guint disk_only = 0;
  for (guint i = 0; i < drawer->matched_files->len; i++) {
    g_autofree char *canon = g_canonicalize_filename(g_ptr_array_index(drawer->matched_files, i), NULL);
    if (!g_hash_table_contains(seen_paths, canon)) {
      disk_only++;
    }
  }
  if (disk_only > 0 && !drawer->confirm_armed) {
    drawer->confirm_armed = TRUE;
    g_autofree char *msg = g_strdup_printf(
        "%u file%s on disk will be rewritten and this cannot be undone - press apply again to confirm "
        "(any edit to the fields cancels)",
        disk_only, disk_only == 1 ? "" : "s");
    if (drawer->status_cb != NULL) {
      drawer->status_cb(msg, drawer->cb_data);
    }
    return 0;
  }
  drawer->confirm_armed = FALSE;

  if (open_buffers != NULL) {
    for (guint i = 0; i < open_buffers->len; i++) {
      IdeBuffer *b = g_ptr_array_index(open_buffers, i);
      const char *bp = ide_buffer_get_file_path(b);
      gboolean was_modified = ide_buffer_get_modified(b);
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
        if (!was_modified && bp != NULL) {
          ide_buffer_save(b, NULL);
        }
        total_replaced += cnt;
      }
    }
  }
  for (guint i = 0; i < drawer->matched_files->len; i++) {
    const char *fpath = g_ptr_array_index(drawer->matched_files, i);
    g_autofree char *canon = g_canonicalize_filename(fpath, NULL);
    if (g_hash_table_contains(seen_paths, canon)) {
      continue;
    }
    g_autofree char *orig = NULL;
    gsize orig_len = 0;
    if (!g_file_get_contents(fpath, &orig, &orig_len, NULL) ||
        orig_len > IDE_PROJECT_REPLACE_MAX_BYTES ||
        !g_utf8_validate(orig, (gssize)orig_len, NULL)) {
      continue;
    }
    guint cnt = 0;
    g_autofree char *after = ide_replace_text_all(orig,
                                                  find_str,
                                                  rep_str,
                                                  drawer->case_sensitive,
                                                  drawer->whole_word,
                                                  drawer->use_regex,
                                                  &cnt);
    if (cnt > 0 && ide_save_file_atomic(fpath, after, -1, NULL)) {
      total_replaced += cnt;
    }
  }
  ide_search_drawer_refresh_replace_preview(drawer);
  return total_replaced;
}

gboolean
ide_search_drawer_apply_current(IdeSearchDrawer *drawer, IdeBuffer *active_buf)
{
  g_return_val_if_fail(drawer != NULL, FALSE);
  IdeBuffer *buf = drawer->target_buf ? drawer->target_buf : active_buf;
  if (buf == NULL) {
    return FALSE;
  }
  const char *find_str = gtk_editable_get_text(GTK_EDITABLE(drawer->find_entry));
  const char *rep_str = gtk_editable_get_text(GTK_EDITABLE(drawer->replace_entry));
  if (find_str == NULL || find_str[0] == '\0') {
    return FALSE;
  }
  if (rep_str == NULL) {
    rep_str = "";
  }

  g_autofree char *escaped = drawer->use_regex ? g_strdup(find_str) : g_regex_escape_string(find_str, -1);
  g_autofree char *pattern = drawer->whole_word ? g_strdup_printf("\\b(?:%s)\\b", escaped) : g_strdup(escaped);
  GRegexCompileFlags cflags = G_REGEX_MULTILINE;
  if (!drawer->case_sensitive) {
    cflags |= G_REGEX_CASELESS;
  }
  g_autoptr(GRegex) rx = g_regex_new(pattern, cflags, 0, NULL);
  if (rx == NULL) {
    return FALSE;
  }

  g_autofree char *orig = ide_buffer_get_text(buf);
  g_autoptr(GMatchInfo) mi = NULL;
  if (!g_regex_match(rx, orig, 0, &mi) || !g_match_info_matches(mi)) {
    return FALSE;
  }
  gint start_pos = 0, end_pos = 0;
  if (!g_match_info_fetch_pos(mi, 0, &start_pos, &end_pos) || start_pos < 0 || end_pos < start_pos) {
    return FALSE;
  }
  g_autofree char *expanded = drawer->use_regex
                                  ? g_match_info_expand_references(mi, rep_str, NULL)
                                  : g_strdup(rep_str);
  if (expanded == NULL) {
    expanded = g_strdup(rep_str);
  }
  GString *updated = g_string_new_len(orig, start_pos);
  g_string_append(updated, expanded);
  g_string_append(updated, orig + end_pos);
  ide_buffer_set_text(buf, updated->str);
  g_string_free(updated, TRUE);
  ide_search_drawer_refresh_replace_preview(drawer);
  return TRUE;
}
