#include "ide_markdown_private.h"
#include "ide_buffer.h"
#include "../common/spawn_detached.h"
#include <gio/gio.h>
#include <gtksourceview/gtksource.h>
#include <string.h>

char *ide_markdown_toggle_checklist_line(const char *markdown_text,
                                         guint       line_1based) {
  if (!markdown_text || line_1based == 0) return g_strdup(markdown_text ? markdown_text : "");
  g_auto(GStrv) lines = g_strsplit(markdown_text, "\n", -1);
  guint n = g_strv_length(lines);
  if (line_1based > n) return g_strdup(markdown_text);

  char *line = lines[line_1based - 1];
  char *unck = strstr(line, "[ ]");
  char *ck_low = strstr(line, "[x]");
  char *ck_up = strstr(line, "[X]");

  if (unck) {
    unck[1] = 'x';
  } else if (ck_low) {
    ck_low[1] = ' ';
  } else if (ck_up) {
    ck_up[1] = ' ';
  }
  return g_strjoinv("\n", lines);
}

/* ─── 1. Link Activation & External / Internal File Navigation ─── */

void ide_markdown_open_or_navigate_target(IdeMarkdownView *md, const char *uri_or_ref) {
  if (!md || !uri_or_ref || !*uri_or_ref) return;

  /* 1. In-document heading anchor (#section-name) */
  if (uri_or_ref[0] == '#') {
    if (g_ascii_isdigit(uri_or_ref[1]) ||
        ((uri_or_ref[1] == 'L' || uri_or_ref[1] == 'l') && g_ascii_isdigit(uri_or_ref[2]))) {
      const char *num = (uri_or_ref[1] == 'L' || uri_or_ref[1] == 'l') ? uri_or_ref + 2 : uri_or_ref + 1;
      guint ln = (guint)g_ascii_strtoull(num, NULL, 10);
      if (ln > 0 && md->jump_cb) md->jump_cb(ln, md->user_data);
      return;
    }
    /* Search current_markdown for matching heading slug */
    if (md->current_markdown && md->jump_cb) {
      g_autofree char *slug = g_ascii_strdown(uri_or_ref + 1, -1);
      g_strdelimit(slug, "-_", ' ');
      g_auto(GStrv) lines = g_strsplit(md->current_markdown, "\n", -1);
      for (guint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '#') {
          g_autofree char *low_line = g_ascii_strdown(lines[i], -1);
          g_strdelimit(low_line, "-_", ' ');
          if (strstr(low_line, slug) != NULL) {
            md->jump_cb(i + 1, md->user_data);
            return;
          }
        }
      }
    }
    return;
  }

  /* 2. Remote Web URL (http://, https://, mailto:) */
  if (g_str_has_prefix(uri_or_ref, "http://") ||
      g_str_has_prefix(uri_or_ref, "https://") ||
      g_str_has_prefix(uri_or_ref, "mailto:")) {
    if (!g_app_info_launch_default_for_uri(uri_or_ref, NULL, NULL)) {
      const char *argv[] = { "xdg-open", uri_or_ref, NULL };
      kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
    }
    return;
  }

  /* 3. Local File or file:// URI (with optional #L123 line anchor) */
  guint target_line = 1;
  g_autofree char *resolved = ide_markdown_resolve_asset_path(
      uri_or_ref, md->doc_dir, md->workspace_root, &target_line);
  if (!resolved) return;

  IdeMarkdownEmbedKind kind = ide_markdown_classify_asset(resolved);
  if ((kind == IDE_MD_EMBED_DOCUMENT || g_str_has_suffix(resolved, ".svg")) &&
      md->open_file_cb && g_file_test(resolved, G_FILE_TEST_IS_REGULAR)) {
    md->open_file_cb(resolved, target_line > 0 ? target_line : 1, 1, md->user_data);
    return;
  }

  g_autofree char *file_uri = g_filename_to_uri(resolved, NULL, NULL);
  if (file_uri) {
    if (!g_app_info_launch_default_for_uri(file_uri, NULL, NULL)) {
      const char *argv[] = { "xdg-open", resolved, NULL };
      kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
    }
  }
}

static gboolean on_markdown_label_activate_link(GtkLabel   *label,
                                                const char *uri,
                                                gpointer    user_data) {
  (void)label;
  IdeMarkdownView *md = user_data;
  ide_markdown_open_or_navigate_target(md, uri);
  return TRUE;
}

static GtkWidget *create_rich_markdown_label(IdeMarkdownView *md,
                                             const char      *raw_markdown,
                                             const char      *css_class) {
  GtkWidget *lbl = gtk_label_new(NULL);
  g_autofree char *mk = ide_markdown_format_inline_pango(raw_markdown);
  gtk_label_set_markup(GTK_LABEL(lbl), mk);
  if (css_class && *css_class)
    gtk_widget_add_css_class(lbl, css_class);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
  g_signal_connect(lbl,
                   "activate-link",
                   G_CALLBACK(on_markdown_label_activate_link),
                   md);
  return lbl;
}

/* ─── 2. Code Block Card ─── */

static void on_copy_code_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  const char *code = user_data;
  if (!code) return;
  GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
  if (cb) gdk_clipboard_set_text(cb, code);
}

static GtkWidget *build_code_block_widget(const char *lang_id, const char *code) {
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");
  gtk_widget_set_hexpand(card, TRUE);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  g_autofree char *upper_lang =
      g_ascii_strup(lang_id && *lang_id ? lang_id : "CODE", -1);
  GtkWidget *badge = gtk_label_new(upper_lang);
  gtk_widget_add_css_class(badge, "shell-popover-badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  GtkWidget *copy_btn = gtk_button_new_with_label("📋 COPY CODE");
  gtk_widget_set_focusable(copy_btn, FALSE);
  gtk_widget_add_css_class(copy_btn, "ide-toolbar-btn");
  gtk_widget_set_valign(copy_btn, GTK_ALIGN_CENTER);
  char *dup_code = g_strdup(code ? code : "");
  g_object_set_data_full(G_OBJECT(copy_btn), "code-str", dup_code, g_free);
  g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_code_clicked), dup_code);

  gtk_box_append(GTK_BOX(hdr), badge);
  gtk_box_append(GTK_BOX(hdr), spacer);
  gtk_box_append(GTK_BOX(hdr), copy_btn);
  gtk_box_append(GTK_BOX(card), hdr);

  GtkSourceBuffer *sbuf = gtk_source_buffer_new(NULL);
  if (lang_id && *lang_id) {
    GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
    GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, lang_id);
    if (lang) gtk_source_buffer_set_language(sbuf, lang);
  }
  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  GtkSourceStyleScheme *scheme =
      gtk_source_style_scheme_manager_get_scheme(sm, "kajo-metro-dark");
  if (scheme) gtk_source_buffer_set_style_scheme(sbuf, scheme);

  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(sbuf), code ? code : "", -1);
  ide_highlight_source_buffer(sbuf, lang_id, "kajo-metro-dark");
  GtkWidget *sview = gtk_source_view_new_with_buffer(sbuf);
  g_object_unref(sbuf);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(sview), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(sview), TRUE);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(sview), 8);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(sview), 8);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(sview), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(sview), 10);
  gtk_widget_add_css_class(sview, "ide-source-view");

  GtkWidget *code_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(code_scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(code_scroll), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(code_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(code_scroll), sview);

  gtk_box_append(GTK_BOX(card), code_scroll);
  return card;
}

/* ─── 3. Block-Level Markdown & HTML Document Renderer ─── */

static void update_task_badge(IdeMarkdownView *md) {
  if (!md || !md->task_badge) return;
  if (md->tasks_total > 0) {
    g_autofree char *tstr =
        g_strdup_printf("✓ %u / %u TASKS DONE", md->tasks_done, md->tasks_total);
    gtk_label_set_text(GTK_LABEL(md->task_badge), tstr);
    gtk_widget_set_visible(md->task_badge, TRUE);
  } else {
    gtk_widget_set_visible(md->task_badge, FALSE);
  }
}

static void on_checklist_toggled(GtkCheckButton *chk, gpointer user_data) {
  IdeMarkdownView *md = user_data;
  if (!md || md->updating_checklists || md->read_only || !md->current_markdown) return;
  guint line_1based =
      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(chk), "md-line"));
  if (line_1based == 0) return;

  if (gtk_check_button_get_active(chk)) {
    if (md->tasks_done < md->tasks_total) md->tasks_done++;
  } else {
    if (md->tasks_done > 0) md->tasks_done--;
  }
  update_task_badge(md);

  char *updated =
      ide_markdown_toggle_checklist_line(md->current_markdown, line_1based);
  g_free(md->current_markdown);
  md->current_markdown = updated;
  if (md->edit_cb) md->edit_cb(md->current_markdown, md->user_data);
}

static void append_inline_embeds_from_text(IdeMarkdownView *md,
                                           GtkWidget       *target_box,
                                           const char      *text) {
  if (!text || !*text) return;
  const char *scan = text;
  while ((scan = strstr(scan, "![")) != NULL) {
    const char *alt_end = strstr(scan + 2, "](");
    if (!alt_end) break;
    const char *src_start = alt_end + 2;
    const char *src_end = strchr(src_start, ')');
    if (!src_end || src_end <= src_start) break;

    g_autofree char *inline_alt = g_strndup(scan + 2, alt_end - (scan + 2));
    g_autofree char *inline_raw_src = g_strndup(src_start, src_end - src_start);
    g_autofree char *inline_src =
        ide_markdown_strip_optional_title(inline_raw_src, NULL);
    gtk_box_append(GTK_BOX(target_box),
                   ide_markdown_build_embed_card(md, inline_alt, inline_src, NULL));
    scan = src_end + 1;
  }
}

static gboolean is_pure_html_wrapper_line(const char *trimmed) {
  if (!trimmed || trimmed[0] != '<') return FALSE;
  if (g_str_has_prefix(trimmed, "<p>") || g_str_has_prefix(trimmed, "<p ") ||
      g_strcmp0(trimmed, "</p>") == 0 || g_str_has_prefix(trimmed, "<div") ||
      g_strcmp0(trimmed, "</div>") == 0 || g_str_has_prefix(trimmed, "<center>") ||
      g_strcmp0(trimmed, "</center>") == 0 || g_str_has_prefix(trimmed, "<!--")) {
    /* Only skip if it does not also contain an <img or text payload */
    if (strstr(trimmed, "<img ") == NULL && strstr(trimmed, "<embed ") == NULL &&
        strstr(trimmed, "<video ") == NULL && strstr(trimmed, "<audio ") == NULL) {
      const char *gt = strchr(trimmed, '>');
      if (gt && (gt[1] == '\0' || g_str_has_prefix(gt + 1, "</")))
        return TRUE;
    }
  }
  return FALSE;
}

gboolean ide_markdown_is_markdown_path(const char *path_or_title) {
  if (!path_or_title || !*path_or_title) return FALSE;
  g_autofree char *low = g_ascii_strdown(path_or_title, -1);
  return g_str_has_suffix(low, ".md") ||
         g_str_has_suffix(low, ".markdown") ||
         g_str_has_suffix(low, ".mdown") ||
         g_str_has_suffix(low, ".mkd") ||
         g_str_has_suffix(low, ".mdx");
}

gboolean ide_markdown_is_table_separator_row(const char *line) {
  if (!line || !*line) return FALSE;
  gboolean has_dash = FALSE;
  gboolean has_pipe = FALSE;
  for (const char *p = line; *p; p++) {
    if (*p == '-') {
      has_dash = TRUE;
    } else if (*p == '|') {
      has_pipe = TRUE;
    } else if (*p == ':' || *p == ' ' || *p == '\t' || *p == '\r') {
      continue;
    } else {
      return FALSE;
    }
  }
  return has_dash && has_pipe;
}

static gboolean is_block_starter_line(const char *trimmed) {
  if (!trimmed || *trimmed == '\0') return TRUE;
  if (g_str_has_prefix(trimmed, "```") || g_str_has_prefix(trimmed, "<details") ||
      g_str_has_prefix(trimmed, ">") || g_str_has_prefix(trimmed, "<hr") ||
      g_strcmp0(trimmed, "---") == 0 || g_strcmp0(trimmed, "***") == 0 ||
      g_strcmp0(trimmed, "___") == 0)
    return TRUE;
  if (trimmed[0] == '#') {
    guint h = 0;
    while (trimmed[h] == '#') h++;
    if (h >= 1 && h <= 6 && trimmed[h] == ' ') return TRUE;
  }
  if ((trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
      trimmed[1] == ' ')
    return TRUE;
  if (g_ascii_isdigit(trimmed[0])) {
    const char *d = trimmed;
    while (g_ascii_isdigit(*d)) d++;
    if (d[0] == '.' && d[1] == ' ') return TRUE;
  }
  if (trimmed[0] == '|' && strchr(trimmed + 1, '|') != NULL) return TRUE;
  if (is_pure_html_wrapper_line(trimmed)) return TRUE;
  if (ide_markdown_extract_embed(trimmed, NULL, NULL, NULL)) return TRUE;
  return FALSE;
}

static void render_fenced_block(IdeMarkdownView *md,
                                GtkWidget       *target_box,
                                char           **lines,
                                guint            n_lines,
                                guint           *io_i,
                                const char      *trimmed,
                                guint            base_line_offset) {
  guint i = *io_i;
  gsize fence_len = 3;
  while (trimmed[fence_len] == '`') fence_len++;
  g_autofree char *fence_str = g_strnfill(fence_len, '`');
  g_autofree char *lang = g_strstrip(g_strdup(trimmed + fence_len));
  guint block_start = base_line_offset + i + 2;
  GString *block = g_string_new("");
  i++;
  while (i < n_lines) {
    const char *btr = lines[i];
    while (*btr == ' ' || *btr == '\t') btr++;
    if (g_str_has_prefix(btr, fence_str)) break;
    if (block->len > 0) g_string_append_c(block, '\n');
    g_string_append(block, lines[i]);
    i++;
  }

  if (g_ascii_strcasecmp(lang, "mermaid") == 0) {
    gtk_box_append(GTK_BOX(target_box),
                   ide_markdown_build_mermaid_card(md, block->str, block_start));
  } else if (g_ascii_strcasecmp(lang, "svg") == 0) {
    gtk_box_append(GTK_BOX(target_box),
                   ide_markdown_build_raw_svg_card(md, block->str));
  } else if (g_ascii_strcasecmp(lang, "carousel") == 0) {
    gtk_box_append(GTK_BOX(target_box),
                   ide_markdown_build_carousel_card(md, block->str, block_start));
  } else {
    gtk_box_append(GTK_BOX(target_box),
                   build_code_block_widget(lang, block->str));
  }
  g_string_free(block, TRUE);
  *io_i = i;
}

static void render_details_block(IdeMarkdownView *md,
                                 GtkWidget       *target_box,
                                 char           **lines,
                                 guint            n_lines,
                                 guint           *io_i,
                                 guint            base_line_offset,
                                 guint           *io_tasks_done,
                                 guint           *io_tasks_total) {
  guint i = *io_i;
  g_autofree char *sum_title = g_strdup("DETAILS");
  GString *inner_md = g_string_new("");
  guint details_body_start = base_line_offset + i + 1;
  gboolean body_started = FALSE;
  i++;
  while (i < n_lines) {
    const char *dtr = lines[i];
    while (*dtr == ' ' || *dtr == '\t') dtr++;
    if (g_str_has_prefix(dtr, "</details>")) break;
    const char *s_open = strstr(dtr, "<summary>");
    const char *s_close = strstr(dtr, "</summary>");
    if (s_open && s_close && s_close > s_open + 9) {
      g_free(sum_title);
      sum_title = g_strndup(s_open + 9, s_close - (s_open + 9));
    } else {
      if (!body_started) {
        details_body_start = base_line_offset + i;
        body_started = TRUE;
      }
      if (inner_md->len > 0) g_string_append_c(inner_md, '\n');
      g_string_append(inner_md, lines[i]);
    }
    i++;
  }

  GtkWidget *exp = gtk_expander_new(sum_title);
  gtk_widget_add_css_class(exp, "ide-mermaid-card");
  GtkWidget *exp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(exp_box, 12);
  gtk_widget_set_margin_end(exp_box, 12);
  gtk_widget_set_margin_top(exp_box, 8);
  gtk_widget_set_margin_bottom(exp_box, 8);
  ide_markdown_render_into_box(
      md, exp_box, inner_md->str, details_body_start, io_tasks_done, io_tasks_total);
  gtk_expander_set_child(GTK_EXPANDER(exp), exp_box);
  gtk_box_append(GTK_BOX(target_box), exp);
  g_string_free(inner_md, TRUE);
  *io_i = i;
}

static void render_blockquote_block(IdeMarkdownView *md,
                                    GtkWidget       *target_box,
                                    char           **lines,
                                    guint            n_lines,
                                    guint           *io_i,
                                    const char      *trimmed) {
  guint i = *io_i;
  const char *first_body = (trimmed[1] == ' ') ? trimmed + 2 : trimmed + 1;
  const char *alert_cls = "ide-md-alert-note";
  const char *alert_hdr = NULL;

  if (strstr(first_body, "[!NOTE]")) {
    alert_cls = "ide-md-alert-note";
    alert_hdr = "ℹ NOTE";
    first_body = strstr(first_body, "[!NOTE]") + 7;
  } else if (strstr(first_body, "[!TIP]")) {
    alert_cls = "ide-md-alert-tip";
    alert_hdr = "💡 TIP";
    first_body = strstr(first_body, "[!TIP]") + 6;
  } else if (strstr(first_body, "[!IMPORTANT]")) {
    alert_cls = "ide-md-alert-important";
    alert_hdr = "✦ IMPORTANT";
    first_body = strstr(first_body, "[!IMPORTANT]") + 12;
  } else if (strstr(first_body, "[!WARNING]")) {
    alert_cls = "ide-md-alert-warning";
    alert_hdr = "⚠ WARNING";
    first_body = strstr(first_body, "[!WARNING]") + 10;
  } else if (strstr(first_body, "[!CAUTION]")) {
    alert_cls = "ide-md-alert-caution";
    alert_hdr = "🛑 CAUTION";
    first_body = strstr(first_body, "[!CAUTION]") + 10;
  }

  while (*first_body == ' ') first_body++;
  GString *quote = g_string_new(first_body);

  while (i + 1 < n_lines) {
    const char *nq = lines[i + 1];
    while (*nq == ' ' || *nq == '\t') nq++;
    if (!g_str_has_prefix(nq, ">")) break;
    i++;
    const char *qbody = (nq[1] == ' ') ? nq + 2 : nq + 1;
    if (quote->len > 0) g_string_append_c(quote, '\n');
    g_string_append(quote, qbody);
  }

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(box, "ide-md-alert");
  gtk_widget_add_css_class(box, alert_cls);

  if (alert_hdr) {
    GtkWidget *hdr_lbl = gtk_label_new(alert_hdr);
    gtk_widget_add_css_class(hdr_lbl, "shell-popover-badge");
    gtk_widget_set_halign(hdr_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), hdr_lbl);
  }

  if (quote->len > 0) {
    GtkWidget *lbl = create_rich_markdown_label(md, quote->str, NULL);
    gtk_box_append(GTK_BOX(box), lbl);
  }

  gtk_box_append(GTK_BOX(target_box), box);
  g_string_free(quote, TRUE);
  *io_i = i;
}

static void render_checklist_item(IdeMarkdownView *md,
                                  GtkWidget       *target_box,
                                  const char      *trimmed,
                                  guint            indent_spaces,
                                  guint            line_1based,
                                  guint           *io_tasks_done,
                                  guint           *io_tasks_total) {
  gboolean checked = (trimmed[3] == 'x' || trimmed[3] == 'X');
  if (io_tasks_total) (*io_tasks_total)++;
  if (checked && io_tasks_done) (*io_tasks_done)++;

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(row, (int)(indent_spaces * 8));

  GtkWidget *chk = gtk_check_button_new();
  gtk_widget_set_focusable(chk, FALSE);
  gtk_widget_set_valign(chk, GTK_ALIGN_CENTER);
  if (md->read_only || !md->edit_cb) {
    gtk_widget_set_sensitive(chk, FALSE);
  }
  md->updating_checklists = TRUE;
  gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), checked);
  md->updating_checklists = FALSE;
  g_object_set_data(G_OBJECT(chk), "md-line", GUINT_TO_POINTER(line_1based));
  g_signal_connect(chk, "toggled", G_CALLBACK(on_checklist_toggled), md);

  GtkWidget *lbl = create_rich_markdown_label(md, trimmed + 6, NULL);
  gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(lbl, TRUE);

  gtk_box_append(GTK_BOX(row), chk);
  gtk_box_append(GTK_BOX(row), lbl);
  gtk_box_append(GTK_BOX(target_box), row);
}

static void render_list_item(IdeMarkdownView *md,
                             GtkWidget       *target_box,
                             const char      *marker_text,
                             const char      *body_text,
                             guint            indent_spaces) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(row, GTK_ALIGN_START);
  gtk_widget_set_margin_start(row, (int)(indent_spaces * 8));

  GtkWidget *m_lbl = gtk_label_new(marker_text);
  gtk_widget_add_css_class(m_lbl, "ide-md-list-bullet");
  gtk_widget_set_valign(m_lbl, GTK_ALIGN_START);

  GtkWidget *item_lbl = create_rich_markdown_label(md, body_text, NULL);
  gtk_widget_set_hexpand(item_lbl, TRUE);

  gtk_box_append(GTK_BOX(row), m_lbl);
  gtk_box_append(GTK_BOX(row), item_lbl);
  gtk_box_append(GTK_BOX(target_box), row);
  append_inline_embeds_from_text(md, target_box, body_text);
}

static void render_pipe_table_block(IdeMarkdownView *md,
                                    GtkWidget       *target_box,
                                    char           **lines,
                                    guint            n_lines,
                                    guint           *io_i) {
  guint i = *io_i;
  GtkWidget *grid = gtk_grid_new();
  gtk_widget_add_css_class(grid, "ide-md-table");
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  int row_idx = 0;

  while (i < n_lines) {
    const char *tline = lines[i];
    while (*tline == ' ') tline++;
    if (tline[0] != '|') {
      i--;
      break;
    }
    if (!ide_markdown_is_table_separator_row(tline)) {
      g_auto(GStrv) cells = g_strsplit(tline, "|", -1);
      int col_idx = 0;
      for (guint c = 1; cells[c] != NULL && cells[c + 1] != NULL; c++) {
        g_autofree char *cell_txt = g_strstrip(g_strdup(cells[c]));
        g_autofree char *c_alt = NULL;
        g_autofree char *c_src = NULL;
        g_autofree char *c_lnk = NULL;
        GtkWidget *cell_widget = NULL;

        if (ide_markdown_extract_embed(cell_txt, &c_alt, &c_src, &c_lnk)) {
          g_autofree char *res = ide_markdown_resolve_asset_path(
              c_src,
              md ? md->doc_dir : NULL,
              md ? md->workspace_root : NULL,
              NULL);
          if (res && g_file_test(res, G_FILE_TEST_EXISTS) &&
              ide_markdown_classify_asset(res) == IDE_MD_EMBED_IMAGE) {
            g_autoptr(GFile) gf = g_file_new_for_path(res);
            GdkTexture *tex = gdk_texture_new_from_file(gf, NULL);
            if (tex) {
              GtkWidget *pic =
                  gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
              gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
              gtk_picture_set_content_fit(GTK_PICTURE(pic),
                                          GTK_CONTENT_FIT_CONTAIN);
              gtk_widget_set_size_request(pic, 32, 32);
              gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
              gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
              if (c_alt && *c_alt)
                gtk_widget_set_tooltip_text(pic, c_alt);
              g_object_unref(tex);
              cell_widget = pic;
            }
          }
        }

        if (!cell_widget) {
          cell_widget = create_rich_markdown_label(
              md, cell_txt, row_idx == 0 ? "shell-popover-badge" : NULL);
          gtk_label_set_wrap(GTK_LABEL(cell_widget), FALSE);
          gtk_widget_set_valign(cell_widget, GTK_ALIGN_CENTER);
        }
        gtk_grid_attach(
            GTK_GRID(grid), cell_widget, col_idx++, row_idx, 1, 1);
      }
      row_idx++;
    }
    i++;
  }
  GtkWidget *tbl_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tbl_scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(tbl_scroll), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(tbl_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tbl_scroll), grid);
  gtk_box_append(GTK_BOX(target_box), tbl_scroll);
  *io_i = i;
}

static void render_paragraph_block(IdeMarkdownView *md,
                                   GtkWidget       *target_box,
                                   char           **lines,
                                   guint            n_lines,
                                   guint           *io_i,
                                   const char      *trimmed) {
  guint i = *io_i;
  GString *para = g_string_new(trimmed);
  while (i + 1 < n_lines) {
    const char *nxt = lines[i + 1];
    while (*nxt == ' ' || *nxt == '\t') nxt++;
    if (is_block_starter_line(nxt)) break;
    g_string_append_c(para, ' ');
    g_string_append(para, nxt);
    i++;
  }
  GtkWidget *p_lbl = create_rich_markdown_label(md, para->str, NULL);
  gtk_box_append(GTK_BOX(target_box), p_lbl);
  append_inline_embeds_from_text(md, target_box, para->str);
  g_string_free(para, TRUE);
  *io_i = i;
}

void ide_markdown_render_into_box(IdeMarkdownView *md,
                                  GtkWidget       *target_box,
                                  const char      *markdown_text,
                                  guint            base_line_offset,
                                  guint           *io_tasks_done,
                                  guint           *io_tasks_total) {
  g_auto(GStrv) lines = g_strsplit(markdown_text ? markdown_text : "", "\n", -1);
  guint n_lines = g_strv_length(lines);

  for (guint i = 0; i < n_lines; i++) {
    const char *line = lines[i];
    const char *trimmed = line;
    guint indent_spaces = 0;
    while (*trimmed == ' ' || *trimmed == '\t') {
      indent_spaces += (*trimmed == '\t') ? 4 : 1;
      trimmed++;
    }

    if (*trimmed == '\0') continue;

    /* 1. Multi-backtick Fenced Blocks (````carousel or ```mermaid / ```svg / ```lang) */
    if (g_str_has_prefix(trimmed, "```")) {
      render_fenced_block(md, target_box, lines, n_lines, &i, trimmed, base_line_offset);
      continue;
    }

    /* 2. Collapsible HTML <details><summary>Title</summary> ... </details> */
    if (g_str_has_prefix(trimmed, "<details")) {
      render_details_block(
          md, target_box, lines, n_lines, &i, base_line_offset, io_tasks_done, io_tasks_total);
      continue;
    }

    /* 3. Standalone Embedded Asset Line: ![alt](src), [![alt](src)](url), or <img src="..."> */
    {
      g_autofree char *alt = NULL;
      g_autofree char *src = NULL;
      g_autofree char *link_url = NULL;
      if (ide_markdown_extract_embed(trimmed, &alt, &src, &link_url)) {
        gtk_box_append(GTK_BOX(target_box),
                       ide_markdown_build_embed_card(md, alt, src, link_url));
        continue;
      }
    }

    /* 4. Skip pure HTML layout wrapper lines (<p align="center">, </p>, <div>, </div>) */
    if (is_pure_html_wrapper_line(trimmed)) {
      continue;
    }

    /* 5. Headings (# H1 through ###### H6) */
    if (trimmed[0] == '#') {
      guint h_level = 0;
      while (trimmed[h_level] == '#') h_level++;
      if (h_level >= 1 && h_level <= 6 && trimmed[h_level] == ' ') {
        const char *h_text = trimmed + h_level + 1;
        const char *h_cls;
        if (h_level == 1) h_cls = "ide-md-h1";
        else if (h_level == 2) h_cls = "ide-md-h2";
        else if (h_level == 3) h_cls = "ide-md-h3";
        else if (h_level == 4) h_cls = "ide-md-h4";
        else h_cls = "ide-md-h5";

        GtkWidget *lbl = create_rich_markdown_label(md, h_text, h_cls);
        gtk_box_append(GTK_BOX(target_box), lbl);
        continue;
      }
    }

    /* 6. GitHub Alert Callouts (> [!NOTE], [!TIP], [!IMPORTANT], [!WARNING], [!CAUTION]) & Blockquotes */
    if (g_str_has_prefix(trimmed, ">")) {
      render_blockquote_block(md, target_box, lines, n_lines, &i, trimmed);
      continue;
    }

    /* 7. Horizontal Rules (---, ***, ___, <hr>) */
    if (g_strcmp0(trimmed, "---") == 0 || g_strcmp0(trimmed, "***") == 0 ||
        g_strcmp0(trimmed, "___") == 0 || g_str_has_prefix(trimmed, "<hr")) {
      GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
      gtk_widget_add_css_class(sep, "ide-md-hr");
      gtk_box_append(GTK_BOX(target_box), sep);
      continue;
    }

    /* 8. Interactive Task Checklists (- [ ] / - [x]) */
    if (g_str_has_prefix(trimmed, "- [ ] ") ||
        g_str_has_prefix(trimmed, "- [x] ") ||
        g_str_has_prefix(trimmed, "- [X] ") ||
        g_str_has_prefix(trimmed, "* [ ] ") ||
        g_str_has_prefix(trimmed, "* [x] ")) {
      render_checklist_item(
          md, target_box, trimmed, indent_spaces, base_line_offset + i + 1,
          io_tasks_done, io_tasks_total);
      continue;
    }

    /* 9. Unordered Bullet Lists (- item, * item, + item) */
    if ((trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
        trimmed[1] == ' ') {
      const char *bullet_glyph =
          (indent_spaces >= 4) ? "▪" : ((indent_spaces >= 2) ? "◦" : "•");
      render_list_item(md, target_box, bullet_glyph, trimmed + 2, indent_spaces);
      continue;
    }

    /* 10. Ordered Numbered Lists (1. item, 2. item) */
    if (g_ascii_isdigit(trimmed[0])) {
      const char *d = trimmed;
      while (g_ascii_isdigit(*d)) d++;
      if (d[0] == '.' && d[1] == ' ') {
        g_autofree char *num_txt = g_strndup(trimmed, (d - trimmed) + 1);
        render_list_item(md, target_box, num_txt, d + 2, indent_spaces);
        continue;
      }
    }

    /* 11. Native GtkGrid Pipe Tables (| Col 1 | Col 2 |) */
    if (trimmed[0] == '|' && strchr(trimmed + 1, '|') != NULL) {
      render_pipe_table_block(md, target_box, lines, n_lines, &i);
      continue;
    }

    /* 12. Regular Paragraph (join consecutive wrapped lines + inline embeds) */
    render_paragraph_block(md, target_box, lines, n_lines, &i, trimmed);
  }
}

/* ─── 4. View Lifecycle & Public API ─── */

static gboolean on_md_restore_scroll_idle(gpointer user_data) {
  IdeMarkdownView *md = user_data;
  md->scroll_restore_id = 0;
  if (!md->has_pending_scroll || !md->scroll_win) return G_SOURCE_REMOVE;
  md->has_pending_scroll = FALSE;
  ide_markdown_view_sync_scroll(md, md->pending_scroll_frac);
  return G_SOURCE_REMOVE;
}

void ide_markdown_view_render(IdeMarkdownView *md, const char *markdown_text) {
  if (!md || !md->doc_box) return;
  const char *incoming = markdown_text ? markdown_text : "";
  if (!md->context_dirty && md->current_markdown &&
      g_strcmp0(md->current_markdown, incoming) == 0) {
    return;
  }

  /* Preserve current scroll fraction across DOM rebuilds on the same document. */
  gboolean keep_scroll = !md->context_dirty;
  double saved_frac = 0.0;
  if (keep_scroll && md->scroll_win) {
    GtkAdjustment *vadj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(md->scroll_win));
    if (vadj) {
      double upper = gtk_adjustment_get_upper(vadj);
      double page = gtk_adjustment_get_page_size(vadj);
      if (upper > page) {
        saved_frac = CLAMP(gtk_adjustment_get_value(vadj) / (upper - page), 0.0, 1.0);
      }
    }
  }

  char *new_md = g_strdup(incoming);
  g_free(md->current_markdown);
  md->current_markdown = new_md;
  md->context_dirty = FALSE;

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(md->doc_box)) != NULL)
    gtk_box_remove(GTK_BOX(md->doc_box), child);

  guint tasks_done = 0;
  guint tasks_total = 0;

  ide_markdown_render_into_box(
      md, md->doc_box, md->current_markdown, 0, &tasks_done, &tasks_total);

  md->tasks_done = tasks_done;
  md->tasks_total = tasks_total;
  update_task_badge(md);

  g_clear_handle_id(&md->scroll_restore_id, g_source_remove);
  if (keep_scroll && saved_frac > 0.0001) {
    md->pending_scroll_frac = saved_frac;
    md->has_pending_scroll = TRUE;
    md->scroll_restore_id = g_idle_add(on_md_restore_scroll_idle, md);
  }
}

void ide_markdown_view_show_notice(IdeMarkdownView *md, const char *notice_text) {
  if (!md || !md->doc_box) return;
  g_clear_handle_id(&md->scroll_restore_id, g_source_remove);
  g_clear_pointer(&md->current_markdown, g_free);
  md->context_dirty = TRUE;
  md->tasks_done = 0;
  md->tasks_total = 0;
  update_task_badge(md);
  if (md->doc_badge) gtk_widget_set_visible(md->doc_badge, FALSE);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(md->doc_box)) != NULL)
    gtk_box_remove(GTK_BOX(md->doc_box), child);

  GtkWidget *lbl = gtk_label_new(notice_text ? notice_text : "No Markdown document open");
  gtk_widget_add_css_class(lbl, "empty-state-label");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_box_append(GTK_BOX(md->doc_box), lbl);
}

const char *ide_markdown_view_get_doc_path(const IdeMarkdownView *md) {
  return md ? md->doc_file_path : NULL;
}

void ide_markdown_view_sync_scroll(IdeMarkdownView *md, double scroll_fraction) {
  if (!md || !md->scroll_win) return;
  GtkAdjustment *vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(md->scroll_win));
  if (!vadj) return;
  double upper = gtk_adjustment_get_upper(vadj);
  double page = gtk_adjustment_get_page_size(vadj);
  if (upper > page) {
    gtk_adjustment_set_value(vadj, CLAMP(scroll_fraction, 0.0, 1.0) * (upper - page));
  }
}

GtkWidget *ide_markdown_view_get_widget(IdeMarkdownView *md) {
  return md ? md->root_box : NULL;
}

GtkWidget *ide_markdown_view_get_scroll_window(IdeMarkdownView *md) {
  return md ? md->scroll_win : NULL;
}

GtkWidget *ide_markdown_view_get_doc_box(IdeMarkdownView *md) {
  return md ? md->doc_box : NULL;
}

void ide_markdown_view_set_read_only(IdeMarkdownView *md, gboolean read_only) {
  if (!md) return;
  md->read_only = read_only;
}

void ide_markdown_view_set_show_header_bar(IdeMarkdownView *md, gboolean show) {
  if (!md || !md->header_bar) return;
  gtk_widget_set_visible(md->header_bar, show);
}

void ide_markdown_view_scroll_to_line(IdeMarkdownView *md,
                                      guint            line_1based,
                                      guint            total_lines) {
  if (!md || !md->scroll_win || total_lines <= 1) return;
  double frac = (double)(line_1based > 0 ? line_1based - 1 : 0) / (double)(total_lines - 1);
  ide_markdown_view_sync_scroll(md, CLAMP(frac, 0.0, 1.0));
}

static void clear_heading_entry(gpointer data) {
  IdeMarkdownHeading *h = data;
  if (h) g_free(h->title);
}

GArray *ide_markdown_extract_headings(const char *markdown_text) {
  GArray *arr = g_array_new(FALSE, TRUE, sizeof(IdeMarkdownHeading));
  g_array_set_clear_func(arr, clear_heading_entry);
  if (!markdown_text || !*markdown_text) return arr;

  g_auto(GStrv) lines = g_strsplit(markdown_text, "\n", -1);
  gboolean in_fence = FALSE;
  char fence_char = '\0';

  for (guint i = 0; lines[i] != NULL; i++) {
    const char *p = lines[i];
    while (*p == ' ' || *p == '\t') p++;
    if (g_str_has_prefix(p, "```") || g_str_has_prefix(p, "~~~")) {
      char fc = p[0];
      if (!in_fence) {
        in_fence = TRUE;
        fence_char = fc;
      } else if (fc == fence_char) {
        in_fence = FALSE;
      }
      continue;
    }
    if (in_fence) continue;

    if (p[0] == '#') {
      guint lvl = 0;
      while (p[lvl] == '#' && lvl < 6) lvl++;
      if (lvl >= 1 && lvl <= 4 && (p[lvl] == ' ' || p[lvl] == '\t')) {
        const char *title = p + lvl;
        while (*title == ' ' || *title == '\t') title++;
        g_autofree char *clean = g_strdup(title);
        g_strstrip(clean);
        /* Strip trailing ATX closing hashes if preceded by whitespace (or alone) */
        size_t len = strlen(clean);
        if (len > 0 && clean[len - 1] == '#') {
          size_t pos = len;
          while (pos > 0 && clean[pos - 1] == '#') pos--;
          if (pos == 0 || clean[pos - 1] == ' ' || clean[pos - 1] == '\t') {
            clean[pos] = '\0';
            g_strstrip(clean);
          }
        }
        if (*clean) {
          IdeMarkdownHeading h = {
            .level = lvl,
            .line_1based = i + 1,
            .title = g_strdup(clean),
          };
          g_array_append_val(arr, h);
        }
      }
    }
  }
  return arr;
}

void ide_markdown_view_set_open_file_cb(IdeMarkdownView      *md,
                                        IdeMarkdownOpenFileCb open_file_cb) {
  if (!md) return;
  md->open_file_cb = open_file_cb;
}

void ide_markdown_view_set_context(IdeMarkdownView *md,
                                   const char      *doc_file_path,
                                   const char      *workspace_root) {
  if (!md) return;
  if (g_strcmp0(md->doc_file_path, doc_file_path) != 0 ||
      g_strcmp0(md->workspace_root, workspace_root) != 0) {
    md->context_dirty = TRUE;
  }
  char *new_path = doc_file_path ? g_strdup(doc_file_path) : NULL;
  char *new_dir = (doc_file_path && *doc_file_path)
                      ? g_path_get_dirname(doc_file_path)
                      : NULL;
  char *new_root = workspace_root ? g_strdup(workspace_root) : NULL;

  g_free(md->doc_file_path);
  g_free(md->doc_dir);
  g_free(md->workspace_root);

  md->doc_file_path = new_path;
  md->doc_dir = new_dir;
  md->workspace_root = new_root;

  if (md->doc_badge) {
    if (md->doc_file_path && *md->doc_file_path) {
      g_autofree char *base = g_path_get_basename(md->doc_file_path);
      gtk_label_set_text(GTK_LABEL(md->doc_badge), base);
      gtk_widget_set_visible(md->doc_badge, TRUE);
    } else {
      gtk_widget_set_visible(md->doc_badge, FALSE);
    }
  }
}

IdeMarkdownView *ide_markdown_view_new(IdeMarkdownJumpCb       jump_cb,
                                       IdeMarkdownBufferEditCb edit_cb,
                                       gpointer                user_data) {
  IdeMarkdownView *md = g_new0(IdeMarkdownView, 1);
  md->jump_cb = jump_cb;
  md->edit_cb = edit_cb;
  md->user_data = user_data;

  md->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(md->root_box, "ide-markdown-pane");
  gtk_widget_set_hexpand(md->root_box, TRUE);
  gtk_widget_set_vexpand(md->root_box, TRUE);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  md->header_bar = hdr;
  gtk_widget_add_css_class(hdr, "ide-drawer-header");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("📖 MARKDOWN PREVIEW");
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);

  md->doc_badge = gtk_label_new("");
  gtk_widget_add_css_class(md->doc_badge, "shell-popover-badge");
  gtk_widget_add_css_class(md->doc_badge, "accent");
  gtk_widget_set_valign(md->doc_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(md->doc_badge, FALSE);

  md->task_badge = gtk_label_new("✓ 0 / 0 TASKS DONE");
  gtk_widget_add_css_class(md->task_badge, "shell-popover-badge");
  gtk_widget_add_css_class(md->task_badge, "success");
  gtk_widget_set_valign(md->task_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(md->task_badge, FALSE);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), md->doc_badge);
  gtk_box_append(GTK_BOX(hdr), md->task_badge);
  gtk_box_append(GTK_BOX(md->root_box), hdr);

  md->scroll_win = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(md->scroll_win),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(md->scroll_win, TRUE);
  gtk_widget_set_vexpand(md->scroll_win, TRUE);

  md->doc_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(md->doc_box, 16);
  gtk_widget_set_margin_end(md->doc_box, 16);
  gtk_widget_set_margin_top(md->doc_box, 12);
  gtk_widget_set_margin_bottom(md->doc_box, 16);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(md->scroll_win), md->doc_box);
  gtk_box_append(GTK_BOX(md->root_box), md->scroll_win);

  return md;
}

void ide_markdown_view_free(IdeMarkdownView *md) {
  if (!md) return;
  g_clear_handle_id(&md->scroll_restore_id, g_source_remove);
  g_free(md->current_markdown);
  g_free(md->doc_file_path);
  g_free(md->doc_dir);
  g_free(md->workspace_root);
  g_free(md);
}
