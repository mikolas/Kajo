#include "ide_window_private.h"

G_DEFINE_TYPE(KajoIdeWindow, kajo_ide_window, GTK_TYPE_WINDOW)

static void jump_entry_free(gpointer data) {
  IdeJumpEntry *e = data;
  if (!e) return;
  g_free(e->path);
  g_free(e);
}

IdeBuffer *ide_window_get_active_buffer(KajoIdeWindow *self) {
  if (!self || !self->buffers || self->buffers->len == 0) return NULL;
  if (self->active_buf_idx >= self->buffers->len)
    self->active_buf_idx = self->buffers->len - 1;
  return g_ptr_array_index(self->buffers, self->active_buf_idx);
}

void ide_window_get_cursor_line_col(KajoIdeWindow *self,
                                       guint         *out_line,
                                       guint         *out_col) {
  if (out_line) *out_line = 1;
  if (out_col) *out_col = 1;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(tbuf, &iter, gtk_text_buffer_get_insert(tbuf));
  if (out_line) *out_line = (guint)gtk_text_iter_get_line(&iter) + 1;
  if (out_col) *out_col = (guint)gtk_text_iter_get_line_offset(&iter) + 1;
}

void ide_window_record_jumplist_here(KajoIdeWindow *self) {
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf || !ide_buffer_get_file_path(buf)) return;
  guint line = 1, col = 1;
  ide_window_get_cursor_line_col(self, &line, &col);

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

void ide_window_show_toast(KajoIdeWindow *self, const char *msg) {
  if (!self || !self->toast_banner || !msg || !*msg) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_widget_set_visible(self->toast_banner, TRUE);
}

void ide_window_sync_markdown_views(KajoIdeWindow *self) {
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;
  gboolean split_vis = gtk_widget_get_visible(self->md_split_container);
  gboolean drawer_vis =
      gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)) &&
      g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->drawer_stack)),
                "markdown") == 0;
  if (!split_vis && !drawer_vis) return;

  const char *doc_path = ide_buffer_get_file_path(buf);
  ide_markdown_view_set_context(
      self->md_split_view, doc_path, self->workspace_root);
  ide_markdown_view_set_context(
      self->md_drawer_view, doc_path, self->workspace_root);

  g_autofree char *text = ide_buffer_get_text(buf);
  if (split_vis) ide_markdown_view_render(self->md_split_view, text);
  if (drawer_vis) ide_markdown_view_render(self->md_drawer_view, text);
}

static char *format_breadcrumb_for_path(const char *workspace_root,
                                        const char *file_path,
                                        const char *fallback_title) {
  if (!file_path || !*file_path)
    return g_strdup(fallback_title ? fallback_title : "[No Name]");

  const char *rel = file_path;
  if (workspace_root && *workspace_root &&
      g_str_has_prefix(file_path, workspace_root)) {
    rel = file_path + strlen(workspace_root);
    while (*rel == '/') rel++;
  }
  if (!*rel) return g_path_get_basename(file_path);

  g_auto(GStrv) parts = g_strsplit(rel, "/", -1);
  guint n = g_strv_length(parts);
  /* Deep paths (Java packages: net/mikolas/…/core/analysis/extractor) would
   * otherwise push the right-zone controls off-screen. Keep the first segment
   * (repo/module) and the last three (…parent ▸ dir ▸ file); elide the middle. */
  if (n > 5) {
    return g_strdup_printf("%s ▸ … ▸ %s ▸ %s ▸ %s",
                           parts[0], parts[n - 3], parts[n - 2], parts[n - 1]);
  }
  return g_strjoinv(" ▸ ", parts);
}

static void
apply_ide_editor_font_css(KajoIdeWindow *self)
{
  if (!self) return;
  GdkDisplay *display = gdk_display_get_default();
  if (!self->editor_font_provider) {
    self->editor_font_provider = gtk_css_provider_new();
    if (display) {
      gtk_style_context_add_provider_for_display(
          display,
          GTK_STYLE_PROVIDER(self->editor_font_provider),
          GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
    }
  }
  const char *fam = (self->active_font_family && *self->active_font_family)
      ? self->active_font_family : "Fira Code";
  int px = CLAMP(self->active_font_size_px, 9, 28);
  g_autofree char *css = g_strdup_printf(
      "window.shell-ide-window .ide-source-view,\n"
      "window.shell-ide-window .ide-source-view text {\n"
      "    font-family: \"%s\", \"Fira Code\", \"FiraCode Nerd Font\", monospace;\n"
      "    font-size: %dpx;\n"
      "}\n",
      fam, px);
  gtk_css_provider_load_from_string(self->editor_font_provider, css);
}

void ide_window_apply_config(KajoIdeWindow *self) {
  if (!self) return;
  g_autoptr(KajoIdeCfgRef) cfg = kajo_config_store_acquire_ide(NULL);
  if (!cfg) return;

  g_free(self->active_scheme_id);
  self->active_scheme_id = g_strdup(
      (cfg->color_scheme && *cfg->color_scheme)
          ? cfg->color_scheme
          : "kajo-metro-dark");

  g_free(self->active_font_family);
  self->active_font_family = g_strdup(
      (cfg->font_family && *cfg->font_family)
          ? cfg->font_family
          : "Fira Code");

  self->active_font_size_px = CLAMP(cfg->font_size_px, 9, 28);
  self->config_minimap_enabled = cfg->show_minimap;

  apply_ide_editor_font_css(self);

  if (self->source_view) {
    gtk_source_view_set_tab_width(
        GTK_SOURCE_VIEW(self->source_view),
        (guint)CLAMP(cfg->tab_width, 2, 8));
    gtk_source_view_set_highlight_current_line(
        GTK_SOURCE_VIEW(self->source_view), cfg->highlight_current_line);
    gtk_text_view_set_wrap_mode(
        GTK_TEXT_VIEW(self->source_view),
        cfg->word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_source_view_set_show_right_margin(
        GTK_SOURCE_VIEW(self->source_view), cfg->show_margin_guide);
    gtk_source_view_set_right_margin_position(GTK_SOURCE_VIEW(self->source_view), 100);
    gtk_source_view_set_insert_spaces_instead_of_tabs(
        GTK_SOURCE_VIEW(self->source_view), cfg->insert_spaces);
  }

  if (self->gutter_renderer) {
    int gutter_pt = MAX(8, (self->active_font_size_px - 2) * 3 / 4);
    g_autofree char *font_stack = shell_ide_font_stack(self->active_font_family);
    kajo_ide_gutter_renderer_set_font(self->gutter_renderer, font_stack, gutter_pt);
    gtk_widget_set_visible(GTK_WIDGET(self->gutter_renderer),
                           cfg->show_line_numbers);
  }

  if (self->source_map && self->md_split_container &&
      !gtk_widget_get_visible(self->md_split_container)) {
    gtk_widget_set_visible(self->source_map, self->config_minimap_enabled);
  }

  if (self->buffers) {
    for (guint i = 0; i < self->buffers->len; i++) {
      IdeBuffer *b = g_ptr_array_index(self->buffers, i);
      ide_buffer_set_style_scheme(b, self->active_scheme_id);
    }
  }

  if (self->source_view) gtk_widget_queue_draw(self->source_view);
}

typedef struct {
  int delta_or_zero;
  int out_px;
} IdeZoomCtx;

static void
mutate_ide_zoom(KajoIdeCfg *draft, gpointer user_data)
{
  IdeZoomCtx *ctx = (IdeZoomCtx *)user_data;
  if (ctx->delta_or_zero == 0) {
    draft->font_size_px = 13;
  } else {
    draft->font_size_px = CLAMP(draft->font_size_px + ctx->delta_or_zero, 10, 24);
  }
  ctx->out_px = draft->font_size_px;
}

void ide_window_zoom_editor_font(KajoIdeWindow *self, int delta_or_zero) {
  if (!self) return;
  IdeZoomCtx ctx = { delta_or_zero, 13 };
  kajo_config_store_update_ide(NULL, mutate_ide_zoom, &ctx, NULL);

  ide_window_apply_config(self);
  g_autofree char *msg = g_strdup_printf("🔍 Editor Font: %dpx", ctx.out_px);
  ide_window_show_toast(self, msg);
}

static void on_ide_config_changed(KajoConfigStore *store G_GNUC_UNUSED,
                                  KajoConfigDomain changed_domains G_GNUC_UNUSED,
                                  gpointer         user_data) {
  ide_window_apply_config(KAJO_IDE_WINDOW(user_data));
}

static gboolean on_leader_timeout_cb(gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  self->leader_timer_id = 0;
  if (self->fsm && ide_fsm_expire_leader(self->fsm)) {
    ide_window_update_status_bar_and_hud(self);
  }
  return G_SOURCE_REMOVE;
}

void ide_window_update_status_bar_and_hud(KajoIdeWindow *self) {
  if (!self || !self->mode_pill) return;

  IdeState st = ide_fsm_get_state(self->fsm);
  if (st != IDE_STATE_LEADER) {
    g_clear_handle_id(&self->leader_timer_id, g_source_remove);
  }
  gtk_label_set_text(GTK_LABEL(self->mode_pill), ide_state_to_string(st));

  const char *mode_classes[] = {
    "mode-normal", "mode-insert", "mode-select", "mode-search", "mode-leader", NULL
  };
  for (int i = 0; mode_classes[i]; i++)
    gtk_widget_remove_css_class(self->mode_pill, mode_classes[i]);
  gtk_widget_add_css_class(self->mode_pill, ide_state_get_css_class(st));

  /* Sync Header Toggle Button Active States */
  if (self->btn_hdr_tree && self->tree_container) {
    if (gtk_widget_get_visible(self->tree_container))
      gtk_widget_add_css_class(self->btn_hdr_tree, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_tree, "active");
  }
  if (self->btn_hdr_map && self->source_map) {
    if (gtk_widget_get_visible(self->source_map))
      gtk_widget_add_css_class(self->btn_hdr_map, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_map, "active");
  }
  if (self->btn_hdr_md && self->md_split_container) {
    if (gtk_widget_get_visible(self->md_split_container))
      gtk_widget_add_css_class(self->btn_hdr_md, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_md, "active");
  }
  if (self->btn_hdr_help && self->picker) {
    if (ide_picker_is_visible(self->picker) &&
        ide_picker_get_mode(self->picker) == IDE_PICKER_HELP)
      gtk_widget_add_css_class(self->btn_hdr_help, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_help, "active");
  }

  /* Update Git Branch & Status Summary Badge in Header */
  if (self->ws_badge && self->tree) {
    g_autofree char *git_sum = ide_tree_format_git_summary(self->tree);
    gtk_label_set_text(GTK_LABEL(self->ws_badge), git_sum);
  }

  const char *chord = ide_fsm_get_pending_chord(self->fsm);
  if (st == IDE_STATE_LEADER) {
    g_autofree char *badge =
        (chord && *chord)
            ? g_strdup_printf("Space → %s _", chord)
            : g_strdup("Space → _");
    gtk_label_set_text(GTK_LABEL(self->chord_badge), badge);
    gtk_widget_set_visible(self->chord_badge, TRUE);
    g_autofree char *hud_hdr = g_strdup_printf("LEADER CHORD (%s)", badge);
    gtk_label_set_text(GTK_LABEL(self->chord_hud_title), hud_hdr);
    if (self->chord_hud_grid) {
      gtk_label_set_text(GTK_LABEL(self->chord_hud_grid),
                         ide_fsm_get_chord_hud_text(st, chord));
    }
    gtk_widget_set_visible(self->chord_hud_box, TRUE);
  } else if (st == IDE_STATE_MATCH_PENDING) {
    g_autofree char *badge =
        (chord && *chord) ? g_strdup_printf("m → %s _", chord) : g_strdup("m → _");
    gtk_label_set_text(GTK_LABEL(self->chord_badge), badge);
    gtk_widget_set_visible(self->chord_badge, TRUE);
    gtk_label_set_text(GTK_LABEL(self->chord_hud_title), "TEXT OBJECT MATCH (m)");
    if (self->chord_hud_grid) {
      gtk_label_set_text(GTK_LABEL(self->chord_hud_grid),
                         ide_fsm_get_chord_hud_text(st, chord));
    }
    gtk_widget_set_visible(self->chord_hud_box, TRUE);
  } else if (chord && *chord) {
    g_autofree char *badge = g_strdup_printf("%s → _", chord);
    gtk_label_set_text(GTK_LABEL(self->chord_badge), badge);
    gtk_widget_set_visible(self->chord_badge, TRUE);
    g_autofree char *hud_hdr = g_strdup_printf("PENDING CHORD (%s)", chord);
    gtk_label_set_text(GTK_LABEL(self->chord_hud_title), hud_hdr);
    if (self->chord_hud_grid) {
      gtk_label_set_text(GTK_LABEL(self->chord_hud_grid),
                         ide_fsm_get_chord_hud_text(st, chord));
    }
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

  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (buf) {
    g_autofree char *crumb = format_breadcrumb_for_path(
        self->workspace_root,
        ide_buffer_get_file_path(buf),
        ide_buffer_get_title(buf));
    if (self->hdr_breadcrumb) {
      gtk_label_set_text(GTK_LABEL(self->hdr_breadcrumb), crumb);
      /* Full path on hover, since deep paths are shortened in the bar. */
      const char *fp = ide_buffer_get_file_path(buf);
      gtk_widget_set_tooltip_text(self->hdr_breadcrumb, fp ? fp : crumb);
    }

    g_autofree char *f_lbl = g_strdup_printf(
        "%s%s",
        ide_buffer_get_title(buf),
        ide_buffer_get_modified(buf) ? " [+]" : "");
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
    g_autofree char *d_lbl = g_strdup_printf("✖ %u  ⚠ %u", errs, warns);
    gtk_label_set_text(GTK_LABEL(self->diag_status_label), d_lbl);
    if (errs > 0)
      gtk_widget_add_css_class(self->diag_status_label, "urgent");
    else
      gtk_widget_remove_css_class(self->diag_status_label, "urgent");

    guint line = 1, col = 1;
    ide_window_get_cursor_line_col(self, &line, &col);
    g_autofree char *pos_lbl =
        g_strdup_printf("UTF-8 • LF • Ln %u, Col %u", line, col);
    gtk_label_set_text(GTK_LABEL(self->cursor_pos_label), pos_lbl);
  }
}

static void on_tab_btn_clicked(GtkButton *btn, gpointer user_data);
static void on_tab_close_btn_clicked(GtkButton *btn, gpointer user_data);

static void on_buffer_mark_set(GtkTextBuffer     *tbuf,
                               const GtkTextIter *location,
                               GtkTextMark       *mark,
                               gpointer           user_data) {
  (void)location;
  KajoIdeWindow *self = user_data;
  if (!self || !tbuf) return;
  if (mark == gtk_text_buffer_get_insert(tbuf) ||
      mark == gtk_text_buffer_get_selection_bound(tbuf)) {
    /* A hover card is tied to the symbol it was requested for; moving the
     * cursor away dismisses it. Completion/rename/code-action survive because
     * their own edits move the cursor. */
    if (self->lsp && ide_fsm_get_state(self->fsm) == IDE_STATE_HOVER)
      ide_lsp_manager_cancel_popover(self->lsp);
    ide_window_update_status_bar_and_hud(self);
  }
}

void ide_window_rebuild_tab_ribbon(KajoIdeWindow *self) {
  if (!self || !self->tab_ribbon_box) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->tab_ribbon_box)) != NULL)
    gtk_box_remove(GTK_BOX(self->tab_ribbon_box), child);

  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *b = g_ptr_array_index(self->buffers, i);
    const char *title = ide_buffer_get_title(b);
    gboolean is_md = g_str_has_suffix(title, ".md") ||
                     g_str_has_suffix(title, ".markdown");

    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(pill, "ide-tab-pill");
    if (i == self->active_buf_idx)
      gtk_widget_add_css_class(pill, "active");
    gtk_widget_set_valign(pill, GTK_ALIGN_FILL);

    GtkWidget *btn = gtk_button_new();
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_add_css_class(btn, "ide-tab-main-btn");
    gtk_widget_set_valign(btn, GTK_ALIGN_FILL);

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(inner, GTK_ALIGN_CENTER);

    g_autofree char *idx_txt = g_strdup_printf("[%u]", i + 1);
    GtkWidget *idx_lbl = gtk_label_new(idx_txt);
    gtk_widget_add_css_class(idx_lbl, "ide-tab-idx");
    gtk_widget_set_valign(idx_lbl, GTK_ALIGN_CENTER);

    GtkWidget *icon_lbl = gtk_label_new(is_md ? "📖" : "📄");
    gtk_widget_add_css_class(icon_lbl, "ide-tab-icon");
    gtk_widget_set_valign(icon_lbl, GTK_ALIGN_CENTER);

    GtkWidget *name_lbl = gtk_label_new(title);
    gtk_widget_add_css_class(name_lbl, "ide-tab-title");
    gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(inner), idx_lbl);
    gtk_box_append(GTK_BOX(inner), icon_lbl);
    gtk_box_append(GTK_BOX(inner), name_lbl);

    if (ide_buffer_get_agent_snapshot(b) != NULL) {
      GtkWidget *ag_lbl = gtk_label_new("⚡");
      gtk_widget_add_css_class(ag_lbl, "ide-tab-agent");
      gtk_widget_set_valign(ag_lbl, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(inner), ag_lbl);
    } else if (ide_buffer_get_modified(b)) {
      GtkWidget *mod_lbl = gtk_label_new("[+]");
      gtk_widget_add_css_class(mod_lbl, "ide-tab-mod");
      gtk_widget_set_valign(mod_lbl, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(inner), mod_lbl);
    }

    gtk_button_set_child(GTK_BUTTON(btn), inner);
    g_object_set_data(G_OBJECT(btn), "tab-index", GUINT_TO_POINTER(i));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_btn_clicked), self);

    GtkWidget *close_btn = gtk_button_new_with_label("×");
    gtk_widget_set_focusable(close_btn, FALSE);
    gtk_widget_add_css_class(close_btn, "ide-tab-close");
    gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(close_btn), "tab-index", GUINT_TO_POINTER(i));
    g_signal_connect(
        close_btn, "clicked", G_CALLBACK(on_tab_close_btn_clicked), self);

    gtk_box_append(GTK_BOX(pill), btn);
    gtk_box_append(GTK_BOX(pill), close_btn);
    gtk_box_append(GTK_BOX(self->tab_ribbon_box), pill);
  }
}

void ide_window_bind_active_buffer_to_view(KajoIdeWindow *self) {
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;

  if (self->active_scheme_id && *self->active_scheme_id)
    ide_buffer_set_style_scheme(buf, self->active_scheme_id);

  GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(buf);
  g_signal_handlers_disconnect_by_func(
      sbuf, G_CALLBACK(on_buffer_mark_set), self);
  g_signal_connect(
      sbuf, "mark-set", G_CALLBACK(on_buffer_mark_set), self);

  gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view),
                           GTK_TEXT_BUFFER(sbuf));
  kajo_ide_gutter_renderer_set_buffer(self->gutter_renderer, buf);
  ide_lsp_manager_notify_open(self->lsp, buf);

  ide_window_rebuild_tab_ribbon(self);
  ide_window_sync_markdown_views(self);
  ide_window_update_status_bar_and_hud(self);
  gtk_widget_grab_focus(self->source_view);
  gtk_widget_queue_draw(self->source_view);
}

static void on_tab_btn_clicked(GtkButton *btn, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "tab-index"));
  if (idx < self->buffers->len) {
    ide_window_record_jumplist_here(self);
    self->active_buf_idx = idx;
    ide_window_bind_active_buffer_to_view(self);
  }
}

static void on_tab_close_btn_clicked(GtkButton *btn, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "tab-index"));
  if (self->buffers->len <= 1) {
    IdeBuffer *only = g_ptr_array_index(self->buffers, 0);
    if (ide_buffer_get_file_path(only) != NULL ||
        ide_buffer_get_modified(only)) {
      IdeBuffer *fresh = ide_buffer_new(NULL);
      if (self->active_scheme_id)
        ide_buffer_set_style_scheme(fresh, self->active_scheme_id);
      g_ptr_array_remove_index(self->buffers, 0);
      g_ptr_array_add(self->buffers, fresh);
      self->active_buf_idx = 0;
      ide_window_bind_active_buffer_to_view(self);
    }
    return;
  }
  if (idx < self->buffers->len) {
    g_ptr_array_remove_index(self->buffers, idx);
    if (self->active_buf_idx >= self->buffers->len)
      self->active_buf_idx = self->buffers->len - 1;
    else if (idx < self->active_buf_idx && self->active_buf_idx > 0)
      self->active_buf_idx--;
    ide_window_bind_active_buffer_to_view(self);
  }
}

void ide_window_move_cursor_to_line_col(KajoIdeWindow *self,
                                    guint          line_1based,
                                    guint          col_1based) {
  IdeBuffer *buf = ide_window_get_active_buffer(self);
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
  ide_window_update_status_bar_and_hud(self);
}

static void on_external_buffer_change(IdeBuffer *buf,
                                      gboolean   reloaded_cleanly,
                                      gpointer   user_data) {
  KajoIdeWindow *self = user_data;
  if (reloaded_cleanly) {
    g_autofree char *msg =
        g_strdup_printf("↻ Hot-reloaded %s from disk", ide_buffer_get_title(buf));
    ide_window_show_toast(self, msg);
  } else {
    g_autofree char *msg = g_strdup_printf(
        "⚠ Disk conflict on %s (unsaved edits preserved)",
        ide_buffer_get_title(buf));
    ide_window_show_toast(self, msg);
  }
  ide_window_sync_markdown_views(self);
  ide_window_update_status_bar_and_hud(self);
}

void kajo_ide_window_open_path(KajoIdeWindow *self,
                               const char    *path,
                               guint          line_1based,
                               guint          col_1based) {
  if (!self || !path || !*path) return;

  ide_window_record_jumplist_here(self);

  /* Check if already open */
  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *existing = g_ptr_array_index(self->buffers, i);
    if (g_strcmp0(ide_buffer_get_file_path(existing), path) == 0) {
      self->active_buf_idx = i;
      ide_window_bind_active_buffer_to_view(self);
      ide_window_move_cursor_to_line_col(self, line_1based, col_1based);
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
  ide_window_bind_active_buffer_to_view(self);
  ide_window_move_cursor_to_line_col(self, line_1based, col_1based);
}

static GPtrArray *on_dap_breakpoints(gpointer user_data);
static void on_dap_frame(const char *path, guint line_1based, gpointer user_data);

static void ide_window_set_workspace_root(KajoIdeWindow *self,
                                          const char    *path) {
  if (!self || !path || !*path) return;
  char *copy = g_strdup(path);
  g_free(self->workspace_root);
  self->workspace_root = copy;
  if (self->tree) ide_tree_set_workspace_root(self->tree, self->workspace_root);
  if (self->search_drawer)
    ide_search_drawer_set_workspace_root(self->search_drawer, self->workspace_root);
  if (self->lsp)
    ide_lsp_manager_set_workspace_root(self->lsp, self->workspace_root);
  if (self->picker)
    ide_picker_set_workspace_root(self->picker, self->workspace_root);
  if (self->dap)
    ide_dap_view_set_context(self->dap, self->workspace_root,
                             on_dap_breakpoints, on_dap_frame, NULL, self);
  ide_window_update_status_bar_and_hud(self);
}

void kajo_ide_window_open_files(KajoIdeWindow *self,
                                GFile        **files,
                                int            n_files) {
  if (!self || !files || n_files <= 0) return;
  for (int i = 0; i < n_files; i++) {
    g_autofree char *path = g_file_get_path(files[i]);
    if (!path) continue;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      ide_window_set_workspace_root(self, path);
    } else {
      kajo_ide_window_open_path(self, path, 1, 1);
    }
  }
}

static char *translate_gdk_key_to_fsm(guint keyval, GdkModifierType state) {
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean alt = (state & GDK_ALT_MASK) != 0;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;

  if (alt) {
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9)
      return g_strdup_printf("alt+%c", (char)('1' + (keyval - GDK_KEY_1)));
    switch (keyval) {
      case GDK_KEY_Left:  return g_strdup("alt+left");
      case GDK_KEY_Right: return g_strdup("alt+right");
      default: break;
    }
    gunichar uc = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
    if (uc >= 'a' && uc <= 'z')
      return g_strdup_printf("alt+%c", (char)uc);
  }

  if (ctrl) {
    switch (keyval) {
      case GDK_KEY_Left:  return g_strdup("ctrl+left");
      case GDK_KEY_Right: return g_strdup("ctrl+right");
      case GDK_KEY_Up:    return g_strdup("ctrl+up");
      case GDK_KEY_Down:  return g_strdup("ctrl+down");
      case GDK_KEY_Home:  return g_strdup("ctrl+home");
      case GDK_KEY_End:   return g_strdup("ctrl+end");
      case GDK_KEY_space: return g_strdup("ctrl+space");
      default: break;
    }
    gunichar uc = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
    if (uc >= 'a' && uc <= 'z') {
      if (shift && uc == 'z')
        return g_strdup("ctrl+shift+z");
      return g_strdup_printf("ctrl+%c", (char)uc);
    }
  }

  if (shift) {
    switch (keyval) {
      case GDK_KEY_Tab:
      case GDK_KEY_ISO_Left_Tab: return g_strdup("shift+tab");
      case GDK_KEY_Left:         return g_strdup("shift+left");
      case GDK_KEY_Right:        return g_strdup("shift+right");
      case GDK_KEY_Up:           return g_strdup("shift+up");
      case GDK_KEY_Down:         return g_strdup("shift+down");
      case GDK_KEY_F5:           return g_strdup("shift+f5");
      case GDK_KEY_F11:          return g_strdup("shift+f11");
      default: break;
    }
  }

  switch (keyval) {
    case GDK_KEY_Escape:       return g_strdup("esc");
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:     return g_strdup("enter");
    case GDK_KEY_Tab:          return g_strdup("tab");
    case GDK_KEY_ISO_Left_Tab: return g_strdup("shift+tab");
    case GDK_KEY_BackSpace:    return g_strdup("backspace");
    case GDK_KEY_Delete:       return g_strdup("delete");
    case GDK_KEY_space:        return g_strdup("Space");
    case GDK_KEY_Up:           return g_strdup("up");
    case GDK_KEY_Down:         return g_strdup("down");
    case GDK_KEY_Left:         return g_strdup("left");
    case GDK_KEY_Right:        return g_strdup("right");
    case GDK_KEY_Page_Up:      return g_strdup("pgup");
    case GDK_KEY_Page_Down:    return g_strdup("pgdown");
    case GDK_KEY_Home:         return g_strdup("home");
    case GDK_KEY_End:          return g_strdup("end");
    case GDK_KEY_F1:           return g_strdup("f1");
    case GDK_KEY_F5:           return g_strdup("f5");
    case GDK_KEY_F9:           return g_strdup("f9");
    case GDK_KEY_F10:          return g_strdup("f10");
    case GDK_KEY_F11:          return g_strdup("f11");
    case GDK_KEY_F12:          return g_strdup("f12");
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

  /* Live Editor Font Zoom (Ctrl++ / Ctrl+=, Ctrl+-, Ctrl+0) */
  if ((state & GDK_CONTROL_MASK) != 0) {
    if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal ||
        keyval == GDK_KEY_KP_Add) {
      ide_window_zoom_editor_font(self, +1);
      return TRUE;
    }
    if (keyval == GDK_KEY_minus || keyval == GDK_KEY_KP_Subtract) {
      ide_window_zoom_editor_font(self, -1);
      return TRUE;
    }
    if (keyval == GDK_KEY_0 || keyval == GDK_KEY_KP_0) {
      ide_window_zoom_editor_font(self, 0);
      return TRUE;
    }
  }

  /* F1 toggles Help Viewer & Command Palette from any mode */
  if (keyval == GDK_KEY_F1 &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK)) == 0) {
    IdeAction act = { .type = IDE_ACTION_HELP_TOGGLE };
    ide_window_dispatch_action(self, &act);
    return TRUE;
  }

  /* Allow the DAP REPL GtkEntry to receive direct text input & Enter activation */
  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(self));
  GtkWidget *repl_entry = self->dap ? ide_dap_view_get_repl_entry(self->dap) : NULL;
  if (focus && repl_entry &&
      (focus == repl_entry || gtk_widget_is_ancestor(focus, repl_entry))) {
    if (keyval == GDK_KEY_Escape) {
      ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      gtk_widget_grab_focus(self->source_view);
      ide_window_update_status_bar_and_hud(self);
      return TRUE;
    }
    return FALSE;
  }

  /* In HELP mode, 'q' only closes Help when the search query is empty;
   * if the user has already typed a filter query, 'q' appends to the filter. */
  if (ide_fsm_get_state(self->fsm) == IDE_STATE_HELP &&
      ide_picker_has_query(self->picker) &&
      (keyval == GDK_KEY_q || keyval == GDK_KEY_Q) &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) == 0) {
    ide_picker_append_input(self->picker, keyval == GDK_KEY_Q ? "Q" : "q");
    ide_window_update_status_bar_and_hud(self);
    return TRUE;
  }

  g_autofree char *key_str = translate_gdk_key_to_fsm(keyval, state);
  if (!key_str) return FALSE;

  IdeAction act = ide_fsm_handle_key(self->fsm, key_str);
  g_clear_handle_id(&self->leader_timer_id, g_source_remove);
  if (ide_fsm_get_state(self->fsm) == IDE_STATE_LEADER) {
    self->leader_timer_id = g_timeout_add(
        ide_fsm_get_leader_timeout_ms(self->fsm), on_leader_timeout_cb, self);
  }
  ide_window_dispatch_action(self, &act);
  return TRUE;
}

/* Subsystem Callbacks */
static void on_tree_open_file(const char *abs_path, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  kajo_ide_window_open_path(self, abs_path, 1, 1);
  ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  ide_window_update_status_bar_and_hud(self);
}

static void on_picker_open_file(const char *abs_path,
                                guint       line_1based,
                                guint       col_1based,
                                gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  kajo_ide_window_open_path(self, abs_path, line_1based, col_1based);
  ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  ide_window_update_status_bar_and_hud(self);
}

static void on_picker_command(IdeActionType action, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  if (action == IDE_ACTION_PICKER_OPEN_FILES ||
      action == IDE_ACTION_PICKER_OPEN_BUFFERS ||
      action == IDE_ACTION_PICKER_OPEN_GREP) {
    ide_fsm_set_state(self->fsm, IDE_STATE_PICKER);
  } else if (action == IDE_ACTION_SEARCH_OPEN) {
    ide_fsm_set_state(self->fsm, IDE_STATE_SEARCH);
  } else if (action == IDE_ACTION_REPLACE_OPEN ||
             action == IDE_ACTION_PROJECT_REPLACE_OPEN) {
    ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
    ide_fsm_set_drawer_editable(self->fsm, TRUE);
  } else if (action == IDE_ACTION_DAP_TOGGLE_VIEW ||
             action == IDE_ACTION_AGENT_REVIEW_OPEN) {
    ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
    ide_fsm_set_drawer_editable(self->fsm, FALSE);
  } else if (action == IDE_ACTION_TOGGLE_DRAWER) {
    gboolean open = gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer));
    ide_fsm_set_state(self->fsm, open ? IDE_STATE_NORMAL : IDE_STATE_DRAWER);
  } else if (action == IDE_ACTION_TOGGLE_FILE_TREE) {
    gboolean vis = gtk_widget_get_visible(self->tree_container);
    ide_fsm_set_state(self->fsm, vis ? IDE_STATE_NORMAL : IDE_STATE_TREE);
  } else if (action == IDE_ACTION_PANE_FOCUS_TREE) {
    ide_fsm_set_state(self->fsm, IDE_STATE_TREE);
  } else if (action == IDE_ACTION_PANE_FOCUS_DRAWER) {
    ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
  } else if (action == IDE_ACTION_TREE_CREATE_FILE ||
             action == IDE_ACTION_TREE_CREATE_DIR) {
    gtk_widget_set_visible(self->tree_container, TRUE);
    ide_fsm_set_state(self->fsm, IDE_STATE_TREE);
    ide_fsm_set_tree_editable(self->fsm, TRUE);
  } else {
    ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  }
  IdeAction act = { .type = action };
  ide_window_dispatch_action(self, &act);
}

static void on_lsp_diagnostics(IdeBuffer *buf, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  (void)buf;
  if (self->source_view) gtk_widget_queue_draw(self->source_view);
  ide_window_update_status_bar_and_hud(self);
}

static void on_lsp_message(const char *message, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  ide_window_show_toast(self, message);
}

/* Popover visibility drives the modal state so Esc / q / Enter reach the right
 * handler, and the editor returns to NORMAL / INSERT when the popover closes. */
static void on_lsp_ui_state(IdeLspUiState state, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  IdeState cur = ide_fsm_get_state(self->fsm);
  switch (state) {
    case IDE_LSP_UI_HOVER:
      ide_fsm_set_state(self->fsm, IDE_STATE_HOVER);
      break;
    case IDE_LSP_UI_COMPLETION:
      ide_fsm_set_state(self->fsm, IDE_STATE_COMPLETION);
      break;
    case IDE_LSP_UI_CODE_ACTION:
      ide_fsm_set_state(self->fsm, IDE_STATE_CODE_ACTION);
      break;
    case IDE_LSP_UI_RENAME:
      ide_fsm_set_state(self->fsm, IDE_STATE_RENAME);
      break;
    case IDE_LSP_UI_CLOSED:
      if (cur == IDE_STATE_COMPLETION) {
        IdeState prev = ide_fsm_get_previous_state(self->fsm);
        ide_fsm_set_state(self->fsm,
                          (prev == IDE_STATE_NORMAL) ? IDE_STATE_NORMAL : IDE_STATE_INSERT);
      } else if (cur == IDE_STATE_HOVER || cur == IDE_STATE_CODE_ACTION ||
                 cur == IDE_STATE_RENAME) {
        ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      }
      break;
  }
  ide_window_update_status_bar_and_hud(self);
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
  /* Gallery: ready server = green success badge; otherwise the muted pill. */
  if (active)
    gtk_widget_add_css_class(self->lsp_status_badge, "success");
  else
    gtk_widget_remove_css_class(self->lsp_status_badge, "success");
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
  ide_window_rebuild_tab_ribbon(self);
  ide_window_sync_markdown_views(self);
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
    if (self->agent_status_badge) {
      gtk_label_set_text(GTK_LABEL(self->agent_status_badge), badge_text);
      gtk_widget_set_visible(self->agent_status_badge, TRUE);
    }
  } else if (pending_tx_count > 0) {
    g_autofree char *txt =
        g_strdup_printf("🤖 AGENT: %u HUNKS", pending_tx_count);
    gtk_label_set_text(GTK_LABEL(self->agent_hdr_badge), txt);
    gtk_widget_set_visible(self->agent_hdr_badge, TRUE);
    if (self->agent_status_badge) {
      gtk_label_set_text(GTK_LABEL(self->agent_status_badge), txt);
      gtk_widget_set_visible(self->agent_status_badge, TRUE);
    }
  } else {
    gtk_widget_set_visible(self->agent_hdr_badge, FALSE);
    if (self->agent_status_badge)
      gtk_widget_set_visible(self->agent_status_badge, FALSE);
  }
  if (toast_msg && *toast_msg) ide_window_show_toast(self, toast_msg);
}

/* Open buffers' breakpoints for the debug adapter (setBreakpoints). */
static GPtrArray *on_dap_breakpoints(gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  GPtrArray *sets = g_ptr_array_new_with_free_func(ide_dap_breakpoint_set_free);
  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *b = g_ptr_array_index(self->buffers, i);
    const char *path = ide_buffer_get_file_path(b);
    if (!path) continue;
    GArray *lines = ide_buffer_get_breakpoints(b);
    if (!lines) continue;
    IdeDapBreakpointSet *bs = g_new0(IdeDapBreakpointSet, 1);
    bs->path = g_strdup(path);
    bs->lines = lines;
    g_ptr_array_add(sets, bs);
  }
  return sets;
}

/* Execution marker: show the stopped frame, clear the marker everywhere else. */
static void on_dap_frame(const char *path, guint line_1based, gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  for (guint i = 0; i < self->buffers->len; i++)
    ide_buffer_set_dap_pc_line(g_ptr_array_index(self->buffers, i), 0);
  if (path) {
    kajo_ide_window_open_path(self, path, line_1based, 1);
    for (guint i = 0; i < self->buffers->len; i++) {
      IdeBuffer *b = g_ptr_array_index(self->buffers, i);
      const char *bp = ide_buffer_get_file_path(b);
      if (bp && g_strcmp0(bp, path) == 0) ide_buffer_set_dap_pc_line(b, line_1based);
    }
  }
  if (self->source_view) gtk_widget_queue_draw(self->source_view);
}

/* The Java debugger drives jdtls through workspace/executeCommand. */
static gboolean on_dap_java_exec(const char *file_path, const char *command, JsonNode *arguments,
                                 IdeJavaExecCb cb, gpointer cb_data, gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  return ide_lsp_manager_execute_command(self->lsp, file_path, command, arguments, cb, cb_data);
}

static void on_dap_action_cb(const char *action, gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  if (!self || !action) return;
  IdeAction act = { .type = IDE_ACTION_NONE };
  if (g_strcmp0(action, "continue") == 0)
    act.type = IDE_ACTION_DAP_CONTINUE;
  else if (g_strcmp0(action, "pause") == 0)
    act.type = IDE_ACTION_DAP_PAUSE;
  else if (g_strcmp0(action, "step_over") == 0 || g_strcmp0(action, "step-over") == 0)
    act.type = IDE_ACTION_DAP_STEP_OVER;
  else if (g_strcmp0(action, "step_into") == 0 || g_strcmp0(action, "step-into") == 0)
    act.type = IDE_ACTION_DAP_STEP_INTO;
  else if (g_strcmp0(action, "step_out") == 0 || g_strcmp0(action, "step-out") == 0)
    act.type = IDE_ACTION_DAP_STEP_OUT;
  else if (g_strcmp0(action, "stop") == 0)
    act.type = IDE_ACTION_DAP_STOP;
  if (act.type != IDE_ACTION_NONE)
    ide_window_dispatch_action(self, &act);
}

static void on_markdown_node_jump(guint line_1based, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  ide_window_move_cursor_to_line_col(self, line_1based, 1);
}

static void on_markdown_checklist_edit(const char *new_full_text,
                                       gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf || !new_full_text) return;
  ide_buffer_set_text(buf, new_full_text);
  ide_window_rebuild_tab_ribbon(self);
}

/* Header Bar & Drawer Switcher Callbacks */
static void on_hdr_tree_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  gboolean vis = gtk_widget_get_visible(self->tree_container);
  ide_fsm_set_state(self->fsm, vis ? IDE_STATE_NORMAL : IDE_STATE_TREE);
  IdeAction act = { .type = IDE_ACTION_TOGGLE_FILE_TREE };
  ide_window_dispatch_action(self, &act);
}

static void on_hdr_map_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  if (!self || !self->source_map) return;
  gboolean next = !gtk_widget_get_visible(self->source_map);
  self->config_minimap_enabled = next;
  gtk_widget_set_visible(self->source_map, next);
  ide_window_update_status_bar_and_hud(self);
}

static void on_hdr_find_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  ide_fsm_set_state(self->fsm, IDE_STATE_PICKER);
  ide_picker_open_files(self->picker, self->workspace_root);
  ide_window_update_status_bar_and_hud(self);
}

static void on_hdr_md_clicked(GtkButton *b, gpointer d) {
  (void)b;
  IdeAction act = { .type = IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW };
  ide_window_dispatch_action(KAJO_IDE_WINDOW(d), &act);
}

static void on_hdr_help_clicked(GtkButton *b, gpointer d) {
  (void)b;
  IdeAction act = { .type = IDE_ACTION_HELP_TOGGLE };
  ide_window_dispatch_action(KAJO_IDE_WINDOW(d), &act);
}

static void on_hdr_build_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  ide_window_show_drawer_page(self, "search");
  {
    IdeBuffer *abuf = ide_window_get_active_buffer(self);
    ide_search_drawer_run_task(self->search_drawer, 0, abuf ? ide_buffer_get_file_path(abuf) : NULL);
  }
}

static void on_hdr_float_clicked(GtkButton *b, gpointer d) {
  (void)b;
  (void)d;
  niri_toggle_window_floating();
}

static void on_hdr_close_clicked(GtkButton *b, gpointer d) {
  (void)b;
  gtk_window_close(GTK_WINDOW(d));
}

static void on_drawer_tab_clicked(GtkButton *b, gpointer d) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  const char *page = g_object_get_data(G_OBJECT(b), "drawer-page");
  if (!page) return;
  ide_window_show_drawer_page(self, page);
  if (g_strcmp0(page, "markdown") == 0)
    ide_window_sync_markdown_views(self);
}

static void on_drawer_close_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);
  if (ide_fsm_get_state(self->fsm) == IDE_STATE_DRAWER)
    ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  gtk_widget_grab_focus(self->source_view);
  ide_window_update_status_bar_and_hud(self);
}

static void kajo_ide_window_dispose(GObject *object) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(object);
  g_clear_handle_id(&self->leader_timer_id, g_source_remove);
  if (self->config_sub_id != 0) {
    kajo_config_store_unsubscribe(NULL, self->config_sub_id);
    self->config_sub_id = 0;
  }
  if (self->editor_font_provider) {
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
      gtk_style_context_remove_provider_for_display(
          display, GTK_STYLE_PROVIDER(self->editor_font_provider));
    }
    g_clear_object(&self->editor_font_provider);
  }
  g_clear_pointer(&self->active_scheme_id, g_free);
  g_clear_pointer(&self->active_font_family, g_free);
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

static void on_editor_vadj_changed(GtkAdjustment *vadj, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  if (!self) return;
  /* Keep any open LSP popover pinned to the cursor (or dismiss it when the
   * cursor scrolls out of view) so it never floats over unrelated code. */
  if (self->lsp && ide_lsp_manager_popover_visible(self->lsp))
    ide_lsp_manager_reposition_popover(self->lsp, self->source_view);
  if (!gtk_widget_get_visible(self->md_split_container)) return;
  double upper = gtk_adjustment_get_upper(vadj);
  double page = gtk_adjustment_get_page_size(vadj);
  if (upper > page) {
    double frac = gtk_adjustment_get_value(vadj) / (upper - page);
    ide_markdown_view_sync_scroll(self->md_split_view, frac);
  }
}

/* Places the cursor at a source-view pixel coordinate; returns FALSE when the
 * point is past the text (empty area). */
static gboolean place_cursor_at_view_xy(KajoIdeWindow *self, double x, double y) {
  GtkTextView *tv = GTK_TEXT_VIEW(self->source_view);
  int bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  GtkTextIter it;
  gtk_text_view_get_iter_at_location(tv, &it, bx, by);
  GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
  /* keep an existing selection if the click landed inside it */
  GtkTextIter sel_a, sel_b;
  if (gtk_text_buffer_get_selection_bounds(buf, &sel_a, &sel_b) &&
      gtk_text_iter_in_range(&it, &sel_a, &sel_b)) {
    return TRUE;
  }
  gtk_text_buffer_place_cursor(buf, &it);
  return TRUE;
}

static void code_menu_dispatch(GSimpleAction *action, GVariant *p, gpointer user_data) {
  (void)p;
  KajoIdeWindow *self = user_data;
  const char *name = g_action_get_name(G_ACTION(action));
  static const struct { const char *name; IdeActionType act; } map[] = {
    { "definition", IDE_ACTION_LSP_DEFINITION },
    { "implementation", IDE_ACTION_LSP_IMPLEMENTATION },
    { "hover", IDE_ACTION_LSP_HOVER },
    { "rename", IDE_ACTION_LSP_RENAME },
    { "codeaction", IDE_ACTION_LSP_CODE_ACTION },
    { "format", IDE_ACTION_LSP_FORMAT },
    { "breakpoint", IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(map); i++) {
    if (g_strcmp0(name, map[i].name) == 0) {
      IdeAction a = { .type = map[i].act };
      ide_window_dispatch_action(self, &a);
      return;
    }
  }
}

/* Builds the right-click code menu once; action enable-state is refreshed per
 * popup from the LSP manager so nothing is offered that can't run. */
static void ensure_code_menu(KajoIdeWindow *self) {
  if (self->code_menu_built) return;

  GSimpleActionGroup *group = g_simple_action_group_new();
  const char *acts[] = { "definition", "implementation", "hover", "rename",
                         "codeaction", "format", "breakpoint" };
  for (gsize i = 0; i < G_N_ELEMENTS(acts); i++) {
    GSimpleAction *a = g_simple_action_new(acts[i], NULL);
    g_signal_connect(a, "activate", G_CALLBACK(code_menu_dispatch), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
    g_object_unref(a);
  }
  gtk_widget_insert_action_group(self->source_view, "code", G_ACTION_GROUP(group));
  self->code_actions = G_ACTION_GROUP(group);
  g_object_unref(group);

  GMenu *nav = g_menu_new();
  g_menu_append(nav, "Go to Definition\t g d", "code.definition");
  g_menu_append(nav, "Go to Implementation\t Space g i", "code.implementation");
  g_menu_append(nav, "Show Documentation\t K", "code.hover");
  GMenu *edit = g_menu_new();
  g_menu_append(edit, "Rename Symbol…\t Space c r", "code.rename");
  g_menu_append(edit, "Code Actions…\t Space c a", "code.codeaction");
  g_menu_append(edit, "Format Buffer\t Space c f", "code.format");
  GMenu *dbg = g_menu_new();
  g_menu_append(dbg, "Toggle Breakpoint\t F9", "code.breakpoint");

  GMenu *root = g_menu_new();
  g_menu_append_section(root, NULL, G_MENU_MODEL(nav));
  g_menu_append_section(root, NULL, G_MENU_MODEL(edit));
  g_menu_append_section(root, NULL, G_MENU_MODEL(dbg));
  g_object_unref(nav);
  g_object_unref(edit);
  g_object_unref(dbg);

  /* GTK merges this into the view's built-in cut/copy/paste menu. */
  gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(self->source_view), G_MENU_MODEL(root));
  g_object_unref(root); /* the view holds its own reference */
  self->code_menu_built = TRUE;
}

static void refresh_code_menu_state(KajoIdeWindow *self) {
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  GActionGroup *group = self->code_actions;
  if (group == NULL) return;
  gboolean ready = self->lsp && buf && ide_lsp_manager_is_ready(self->lsp, buf);
  /* definition/hover have ripgrep fallbacks, so stay enabled; the rest need a
   * ready server to do anything useful. */
  const char *server_only[] = { "implementation", "codeaction", "format", NULL };
  for (gsize i = 0; server_only[i] != NULL; i++) {
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(group), server_only[i]);
    if (a) g_simple_action_set_enabled(G_SIMPLE_ACTION(a), ready);
  }
}

/* Ctrl+click: jump to the definition of the symbol under the pointer. */
static void on_source_view_ctrl_click(GtkGestureClick *gesture, int n_press, double x, double y,
                                      gpointer user_data) {
  (void)n_press;
  KajoIdeWindow *self = user_data;
  GdkModifierType mods =
      gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  if (!(mods & GDK_CONTROL_MASK)) return;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_widget_grab_focus(self->source_view);
  place_cursor_at_view_xy(self, x, y);
  ide_window_update_status_bar_and_hud(self);
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (buf) {
    guint l = 1, c = 1;
    ide_window_get_cursor_line_col(self, &l, &c);
    ide_lsp_manager_request_definition(self->lsp, buf, l, c);
  }
}

/* Secondary (right) click: place the cursor first so menu actions act there,
 * and refresh which items are enabled. GTK then shows the extra menu. */
static void on_source_view_secondary(GtkGestureClick *gesture, int n_press, double x, double y,
                                      gpointer user_data) {
  (void)n_press;
  KajoIdeWindow *self = user_data;
  gtk_widget_grab_focus(self->source_view);
  place_cursor_at_view_xy(self, x, y);
  if (ide_fsm_get_state(self->fsm) == IDE_STATE_SELECT) {
    /* keep SELECT if a selection remains; otherwise normalize */
    IdeBuffer *b = ide_window_get_active_buffer(self);
    GtkTextBuffer *tb = b ? GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(b)) : NULL;
    if (tb && !gtk_text_buffer_get_has_selection(tb))
      ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  }
  refresh_code_menu_state(self);
  ide_window_update_status_bar_and_hud(self);
  (void)gesture;
}

static void on_source_view_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data) {
  (void)ctrl;
  KajoIdeWindow *self = user_data;
  /* Leaving the editor (another pane, another window) dismisses hover/completion
   * popovers so they never linger detached from the cursor. */
  if (self->lsp && ide_lsp_manager_popover_visible(self->lsp))
    ide_lsp_manager_cancel_popover(self->lsp);
}

static void on_source_view_pressed(GtkGestureClick *gesture,
                                   int              n_press,
                                   double           x,
                                   double           y,
                                   gpointer         user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  KajoIdeWindow *self = user_data;
  if (!self) return;
  gtk_widget_grab_focus(self->source_view);
  /* A click elsewhere dismisses a sticky hover/completion popover. */
  if (self->lsp && ide_lsp_manager_popover_visible(self->lsp) &&
      ide_fsm_get_state(self->fsm) != IDE_STATE_RENAME)
    ide_lsp_manager_cancel_popover(self->lsp);
  IdeState st = ide_fsm_get_state(self->fsm);
  if (st == IDE_STATE_TREE || st == IDE_STATE_DRAWER ||
      st == IDE_STATE_LEADER || st == IDE_STATE_MATCH_PENDING) {
    ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
    ide_window_update_status_bar_and_hud(self);
  }
}

static void on_source_view_released(GtkGestureClick *gesture,
                                    int              n_press,
                                    double           x,
                                    double           y,
                                    gpointer         user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  KajoIdeWindow *self = user_data;
  if (!self) return;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  gboolean has_sel = gtk_text_buffer_get_has_selection(tbuf);
  IdeState st = ide_fsm_get_state(self->fsm);
  if (has_sel && st == IDE_STATE_NORMAL) {
    ide_fsm_set_state(self->fsm, IDE_STATE_SELECT);
  } else if (!has_sel && st == IDE_STATE_SELECT) {
    ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  }
  ide_window_update_status_bar_and_hud(self);
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
  self->config_minimap_enabled = TRUE;

  GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(self), root_vbox);

  /* 1. Aero + Metro Window Chrome Drag Header (3-Zone Layout) */
  GtkWidget *win_handle = gtk_window_handle_new();
  GtkWidget *hdr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(hdr_box, "settings-header-bar");
  gtk_widget_set_valign(hdr_box, GTK_ALIGN_CENTER);

  /* Left Zone: Title + View Toggles */
  GtkWidget *title_lbl = gtk_label_new("⌨ KAJO IDE");
  gtk_widget_add_css_class(title_lbl, "settings-header-title");
  gtk_widget_set_valign(title_lbl, GTK_ALIGN_CENTER);

  GtkWidget *nav_cluster = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(nav_cluster, GTK_ALIGN_CENTER);

  self->btn_hdr_tree = gtk_button_new_with_label("🌲 TREE");
  gtk_widget_set_focusable(self->btn_hdr_tree, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_tree, "files-nav-btn");
  gtk_widget_add_css_class(self->btn_hdr_tree, "active");
  gtk_widget_set_valign(self->btn_hdr_tree, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_tree, "clicked", G_CALLBACK(on_hdr_tree_clicked), self);

  self->btn_hdr_map = gtk_button_new_with_label("🗺 MAP");
  gtk_widget_set_focusable(self->btn_hdr_map, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_map, "files-nav-btn");
  gtk_widget_add_css_class(self->btn_hdr_map, "active");
  gtk_widget_set_valign(self->btn_hdr_map, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_map, "clicked", G_CALLBACK(on_hdr_map_clicked), self);

  self->btn_hdr_md = gtk_button_new_with_label("📖 MD");
  gtk_widget_set_focusable(self->btn_hdr_md, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_md, "files-nav-btn");
  gtk_widget_set_valign(self->btn_hdr_md, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_md, "clicked", G_CALLBACK(on_hdr_md_clicked), self);

  GtkWidget *btn_find = gtk_button_new_with_label("🔍 FIND");
  gtk_widget_set_focusable(btn_find, FALSE);
  gtk_widget_add_css_class(btn_find, "files-nav-btn");
  gtk_widget_set_valign(btn_find, GTK_ALIGN_CENTER);
  g_signal_connect(btn_find, "clicked", G_CALLBACK(on_hdr_find_clicked), self);

  self->btn_hdr_help = gtk_button_new_with_label("❓ HELP");
  gtk_widget_set_focusable(self->btn_hdr_help, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_help, "files-nav-btn");
  gtk_widget_set_valign(self->btn_hdr_help, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_help, "clicked", G_CALLBACK(on_hdr_help_clicked), self);

  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_tree);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_map);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_md);
  gtk_box_append(GTK_BOX(nav_cluster), btn_find);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_help);

  /* Center Zone: Active File Breadcrumb + Git Branch & Status Summary Badge */
  GtkWidget *hdr_center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_hexpand(hdr_center, TRUE);
  gtk_widget_set_halign(hdr_center, GTK_ALIGN_FILL);
  gtk_widget_set_valign(hdr_center, GTK_ALIGN_CENTER);

  self->hdr_breadcrumb = gtk_label_new("[No Name]");
  gtk_widget_add_css_class(self->hdr_breadcrumb, "ide-header-breadcrumb");
  gtk_widget_set_valign(self->hdr_breadcrumb, GTK_ALIGN_CENTER);
  /* Shrink and ellipsize from the start so the filename tail stays visible and
   * the right-zone BUILD / FLOAT / close controls are never pushed off-screen. */
  gtk_label_set_ellipsize(GTK_LABEL(self->hdr_breadcrumb), PANGO_ELLIPSIZE_START);
  gtk_label_set_xalign(GTK_LABEL(self->hdr_breadcrumb), 0.0f);
  gtk_widget_set_hexpand(self->hdr_breadcrumb, TRUE);
  gtk_widget_set_halign(self->hdr_breadcrumb, GTK_ALIGN_FILL);

  self->ws_badge = gtk_label_new("⎇ main");
  gtk_widget_add_css_class(self->ws_badge, "shell-popover-badge");
  gtk_widget_add_css_class(self->ws_badge, "accent");
  gtk_widget_set_valign(self->ws_badge, GTK_ALIGN_CENTER);

  self->agent_hdr_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->agent_hdr_badge, "shell-popover-badge");
  gtk_widget_add_css_class(self->agent_hdr_badge, "accent");
  gtk_widget_set_valign(self->agent_hdr_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->agent_hdr_badge, FALSE);

  gtk_box_append(GTK_BOX(hdr_center), self->hdr_breadcrumb);
  gtk_box_append(GTK_BOX(hdr_center), self->ws_badge);
  gtk_box_append(GTK_BOX(hdr_center), self->agent_hdr_badge);

  /* Right Zone: Primary Build Action + Window Controls */
  GtkWidget *btn_build = gtk_button_new_with_label("▶ BUILD");
  gtk_widget_set_focusable(btn_build, FALSE);
  gtk_widget_add_css_class(btn_build, "action-btn-primary");
  gtk_widget_set_valign(btn_build, GTK_ALIGN_CENTER);
  g_signal_connect(btn_build, "clicked", G_CALLBACK(on_hdr_build_clicked), self);

  GtkWidget *btn_float = gtk_button_new_with_label("🗗 FLOAT / TILE");
  gtk_widget_set_focusable(btn_float, FALSE);
  gtk_widget_add_css_class(btn_float, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_float, GTK_ALIGN_CENTER);
  g_signal_connect(btn_float, "clicked", G_CALLBACK(on_hdr_float_clicked), self);

  GtkWidget *btn_close = gtk_button_new_with_label("✕");
  gtk_widget_set_focusable(btn_close, FALSE);
  gtk_widget_add_css_class(btn_close, "settings-close-btn");
  gtk_widget_set_valign(btn_close, GTK_ALIGN_CENTER);
  g_signal_connect(btn_close, "clicked", G_CALLBACK(on_hdr_close_clicked), self);

  gtk_box_append(GTK_BOX(hdr_box), title_lbl);
  gtk_box_append(GTK_BOX(hdr_box), nav_cluster);
  gtk_box_append(GTK_BOX(hdr_box), hdr_center);
  gtk_box_append(GTK_BOX(hdr_box), btn_build);
  gtk_box_append(GTK_BOX(hdr_box), btn_float);
  gtk_box_append(GTK_BOX(hdr_box), btn_close);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(win_handle), hdr_box);
  gtk_box_append(GTK_BOX(root_vbox), win_handle);

  /* 2. Multi-Buffer Tab Ribbon */
  self->tab_ribbon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->tab_ribbon_box, "ide-tab-ribbon");
  gtk_widget_set_valign(self->tab_ribbon_box, GTK_ALIGN_FILL);
  gtk_box_append(GTK_BOX(root_vbox), self->tab_ribbon_box);

  /* 3. Main Workspace Overlay & Resizable GtkPaned Splitters */
  GtkWidget *stage_overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(stage_overlay, TRUE);
  gtk_widget_set_vexpand(stage_overlay, TRUE);

  self->tree_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->tree_paned, "ide-paned");
  gtk_widget_set_hexpand(self->tree_paned, TRUE);
  gtk_widget_set_vexpand(self->tree_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->tree_paned), TRUE);

  /* Left File Tree Sidebar */
  self->tree = ide_tree_new(self->workspace_root, on_tree_open_file, self);
  self->tree_container = ide_tree_get_widget(self->tree);
  gtk_paned_set_start_child(GTK_PANED(self->tree_paned), self->tree_container);
  gtk_paned_set_resize_start_child(GTK_PANED(self->tree_paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->tree_paned), FALSE);
  gtk_paned_set_position(GTK_PANED(self->tree_paned), 240);

  /* Right of Tree: Horizontal GtkPaned between Code Editor and Markdown Split */
  self->editor_md_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->editor_md_paned, "ide-paned");
  gtk_widget_set_hexpand(self->editor_md_paned, TRUE);
  gtk_widget_set_vexpand(self->editor_md_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->editor_md_paned), TRUE);

  GtkWidget *editor_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(editor_hbox, TRUE);
  gtk_widget_set_vexpand(editor_hbox, TRUE);

  self->source_scroll = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(self->source_scroll, TRUE);
  gtk_widget_set_vexpand(self->source_scroll, TRUE);

  GtkAdjustment *ed_vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->source_scroll));
  if (ed_vadj) {
    g_signal_connect(ed_vadj, "value-changed", G_CALLBACK(on_editor_vadj_changed), self);
  }

  self->source_view = gtk_source_view_new();
  gtk_widget_add_css_class(self->source_view, "ide-source-view");
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->source_view), TRUE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->source_view), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->source_view), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->source_view), 6);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(self->source_view), TRUE);
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->source_view), 2);
  gtk_source_view_set_insert_spaces_instead_of_tabs(
      GTK_SOURCE_VIEW(self->source_view), TRUE);

  GtkGesture *ed_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(ed_click), 1);
  g_signal_connect(
      ed_click, "pressed", G_CALLBACK(on_source_view_pressed), self);
  g_signal_connect(
      ed_click, "released", G_CALLBACK(on_source_view_released), self);
  gtk_widget_add_controller(self->source_view, GTK_EVENT_CONTROLLER(ed_click));

  /* Right-click code menu (go to definition, rename, …). */
  ensure_code_menu(self);
  GtkGesture *ed_ctx = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(ed_ctx), GDK_BUTTON_SECONDARY);
  g_signal_connect(ed_ctx, "pressed", G_CALLBACK(on_source_view_secondary), self);
  gtk_widget_add_controller(self->source_view, GTK_EVENT_CONTROLLER(ed_ctx));

  /* Ctrl+click jumps to definition (capture phase, before the primary click
   * handler moves into SELECT mode). */
  GtkGesture *ed_ctrl = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(ed_ctrl), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(ed_ctrl), GTK_PHASE_CAPTURE);
  g_signal_connect(ed_ctrl, "pressed", G_CALLBACK(on_source_view_ctrl_click), self);
  gtk_widget_add_controller(self->source_view, GTK_EVENT_CONTROLLER(ed_ctrl));

  /* Dismiss any sticky popover when the editor loses focus. */
  GtkEventController *ed_focus = gtk_event_controller_focus_new();
  g_signal_connect(ed_focus, "leave", G_CALLBACK(on_source_view_focus_leave), self);
  gtk_widget_add_controller(self->source_view, ed_focus);

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
  gtk_widget_set_focusable(self->source_map, FALSE);
  gtk_widget_add_css_class(self->source_map, "ide-minimap");
  gtk_source_map_set_view(GTK_SOURCE_MAP(self->source_map),
                          GTK_SOURCE_VIEW(self->source_view));
  gtk_widget_set_size_request(self->source_map, 84, -1);
  gtk_box_append(GTK_BOX(editor_hbox), self->source_map);

  /* Side-by-Side Graphical Markdown & Vector Mermaid Split (Space m p) */
  self->md_split_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);
  ide_markdown_view_set_open_file_cb(self->md_split_view, on_picker_open_file);
  self->md_split_container = ide_markdown_view_get_widget(self->md_split_view);
  gtk_widget_set_size_request(self->md_split_container, 280, -1);
  gtk_widget_set_visible(self->md_split_container, FALSE);

  gtk_paned_set_start_child(GTK_PANED(self->editor_md_paned), editor_hbox);
  gtk_paned_set_end_child(GTK_PANED(self->editor_md_paned), self->md_split_container);
  gtk_paned_set_resize_start_child(GTK_PANED(self->editor_md_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->editor_md_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(self->editor_md_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->editor_md_paned), FALSE);
  gtk_paned_set_position(GTK_PANED(self->editor_md_paned), 540);

  gtk_paned_set_end_child(GTK_PANED(self->tree_paned), self->editor_md_paned);
  gtk_paned_set_resize_end_child(GTK_PANED(self->tree_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->tree_paned), FALSE);

  gtk_overlay_set_child(GTK_OVERLAY(stage_overlay), self->tree_paned);

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

  self->chord_hud_grid = gtk_label_new(
      "f / SPC  Find Files      b  Buffers         /  Live Grep\n"
      "e        File Tree       a  Alternate C/H   w  Save File\n"
      "m p      Markdown GUI    s  Search/Replace  d  Agent / Debug\n"
      "c        Code Actions    o  Build Output    q  Close Buffer");
  gtk_widget_set_halign(self->chord_hud_grid, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(self->chord_hud_box), self->chord_hud_title);
  gtk_box_append(GTK_BOX(self->chord_hud_box), self->chord_hud_grid);
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

  /* 4. Contextual Bottom Drawer inside Resizable Vertical GtkPaned */
  self->drawer_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->drawer_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);

  GtkWidget *drawer_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(drawer_outer, "ide-drawer-shell");

  /* Unified Drawer Tab Switcher Bar */
  GtkWidget *drawer_tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(drawer_tab_bar, "ide-drawer-tab-strip");
  gtk_widget_set_valign(drawer_tab_bar, GTK_ALIGN_CENTER);

  self->drawer_tab_agent = gtk_button_new_with_label("🤖 AGENT DIFF (Space d s)");
  gtk_widget_set_focusable(self->drawer_tab_agent, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_agent, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_agent, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_agent), "drawer-page", (gpointer)"agent");
  g_signal_connect(
      self->drawer_tab_agent, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_dap = gtk_button_new_with_label("🐞 DAP DEBUGGER (Space d v)");
  gtk_widget_set_focusable(self->drawer_tab_dap, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_dap, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_dap, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_dap), "drawer-page", (gpointer)"dap");
  g_signal_connect(
      self->drawer_tab_dap, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_search = gtk_button_new_with_label("🔎 SEARCH / BUILD (Space s r)");
  gtk_widget_set_focusable(self->drawer_tab_search, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_search, "ide-drawer-tab");
  gtk_widget_add_css_class(self->drawer_tab_search, "active");
  gtk_widget_set_valign(self->drawer_tab_search, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_search), "drawer-page", (gpointer)"search");
  g_signal_connect(
      self->drawer_tab_search, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_md = gtk_button_new_with_label("❖ MERMAID / MD (Space m m)");
  gtk_widget_set_focusable(self->drawer_tab_md, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_md, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_md, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_md), "drawer-page", (gpointer)"markdown");
  g_signal_connect(
      self->drawer_tab_md, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  GtkWidget *dr_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(dr_spacer, TRUE);

  GtkWidget *dr_close = gtk_button_new_with_label("✕ CLOSE (Esc)");
  gtk_widget_set_focusable(dr_close, FALSE);
  gtk_widget_add_css_class(dr_close, "ide-toolbar-btn");
  gtk_widget_set_valign(dr_close, GTK_ALIGN_CENTER);
  g_signal_connect(dr_close, "clicked", G_CALLBACK(on_drawer_close_clicked), self);

  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_agent);
  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_dap);
  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_search);
  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_md);
  gtk_box_append(GTK_BOX(drawer_tab_bar), dr_spacer);
  gtk_box_append(GTK_BOX(drawer_tab_bar), dr_close);

  self->drawer_stack = gtk_stack_new();
  self->search_drawer = ide_search_drawer_new(
      self->workspace_root, on_picker_open_file, NULL, self);
  self->dap = ide_dap_view_new(on_dap_action_cb, self);
  ide_dap_view_set_context(self->dap, self->workspace_root, on_dap_breakpoints, on_dap_frame, NULL, self);
  ide_dap_view_set_java_exec(self->dap, on_dap_java_exec, self);
  self->agent_ipc = ide_agent_ipc_new(
      on_picker_open_file, on_agent_reload_cb, on_agent_status_cb, self);
  ide_agent_ipc_start_default(self->agent_ipc);
  self->md_drawer_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);
  ide_markdown_view_set_open_file_cb(self->md_drawer_view, on_picker_open_file);

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

  gtk_box_append(GTK_BOX(drawer_outer), drawer_tab_bar);
  gtk_box_append(GTK_BOX(drawer_outer), self->drawer_stack);
  gtk_revealer_set_child(GTK_REVEALER(self->drawer_revealer), drawer_outer);

  self->vertical_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(self->vertical_paned, "ide-paned");
  gtk_widget_set_hexpand(self->vertical_paned, TRUE);
  gtk_widget_set_vexpand(self->vertical_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->vertical_paned), TRUE);
  gtk_paned_set_start_child(GTK_PANED(self->vertical_paned), stage_overlay);
  gtk_paned_set_end_child(GTK_PANED(self->vertical_paned), self->drawer_revealer);
  gtk_paned_set_resize_start_child(GTK_PANED(self->vertical_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->vertical_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(self->vertical_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->vertical_paned), FALSE);

  gtk_box_append(GTK_BOX(root_vbox), self->vertical_paned);

  /* 5. Inline Search Bar (/ query) */
  self->inline_search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->inline_search_bar, "ide-drawer-header");
  gtk_widget_set_visible(self->inline_search_bar, FALSE);
  self->inline_search_entry = gtk_label_new("/");
  gtk_box_append(GTK_BOX(self->inline_search_bar), self->inline_search_entry);
  gtk_box_append(GTK_BOX(root_vbox), self->inline_search_bar);

  /* 6. Bottom Status Bar — gallery 3-zone layout: mode+file (left),
   * chord (center), agent/LSP/diagnostics/position (right). */
  GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(status_bar, "ide-status-bar");
  gtk_widget_set_valign(status_bar, GTK_ALIGN_FILL);

  self->mode_pill = gtk_label_new("NORMAL");
  gtk_widget_add_css_class(self->mode_pill, "ide-mode-pill");
  gtk_widget_add_css_class(self->mode_pill, "mode-normal");
  gtk_widget_set_valign(self->mode_pill, GTK_ALIGN_FILL);

  self->file_status_label = gtk_label_new("[No Name]");
  gtk_widget_add_css_class(self->file_status_label, "ide-status-file");
  gtk_widget_set_valign(self->file_status_label, GTK_ALIGN_CENTER);

  /* Center: pending leader/match chord (hidden otherwise). */
  self->chord_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->chord_badge, "ide-status-chord");
  gtk_widget_set_valign(self->chord_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(self->chord_badge, TRUE);
  gtk_widget_set_halign(self->chord_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->chord_badge, FALSE);

  /* A spacer keeps the right cluster flush-right even when the chord is hidden. */
  GtkWidget *st_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(st_spacer, TRUE);

  self->agent_status_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->agent_status_badge, "shell-popover-badge");
  gtk_widget_add_css_class(self->agent_status_badge, "agent");
  gtk_widget_set_valign(self->agent_status_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->agent_status_badge, FALSE);

  self->lsp_status_badge = gtk_label_new("LSP: native");
  gtk_widget_add_css_class(self->lsp_status_badge, "shell-popover-badge");
  gtk_widget_set_valign(self->lsp_status_badge, GTK_ALIGN_CENTER);

  self->diag_status_label = gtk_label_new("✖ 0  ⚠ 0");
  gtk_widget_add_css_class(self->diag_status_label, "shell-popover-badge");
  gtk_widget_set_valign(self->diag_status_label, GTK_ALIGN_CENTER);

  self->cursor_pos_label = gtk_label_new("UTF-8 • LF • Ln 1, Col 1");
  gtk_widget_add_css_class(self->cursor_pos_label, "ide-status-cursor");
  gtk_widget_set_valign(self->cursor_pos_label, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(status_bar), self->mode_pill);
  gtk_box_append(GTK_BOX(status_bar), self->file_status_label);
  gtk_box_append(GTK_BOX(status_bar), self->chord_badge);
  gtk_box_append(GTK_BOX(status_bar), st_spacer);
  gtk_box_append(GTK_BOX(status_bar), self->agent_status_badge);
  gtk_box_append(GTK_BOX(status_bar), self->lsp_status_badge);
  gtk_box_append(GTK_BOX(status_bar), self->diag_status_label);
  gtk_box_append(GTK_BOX(status_bar), self->cursor_pos_label);
  gtk_box_append(GTK_BOX(root_vbox), status_bar);

  /* LSP Manager */
  self->lsp = ide_lsp_manager_new(
      self->workspace_root, on_picker_open_file, on_lsp_status_changed, self);
  ide_lsp_manager_set_callbacks(
      self->lsp, on_lsp_diagnostics, on_lsp_message, on_lsp_ui_state, self);

  /* Initial empty buffer */
  IdeBuffer *init_buf = ide_buffer_new(NULL);
  g_ptr_array_add(self->buffers, init_buf);
  self->active_buf_idx = 0;

  /* Apply IDE Config & Subscribe to Live ide.toml Changes */
  ide_window_apply_config(self);
  self->config_sub_id = kajo_config_store_subscribe(
      NULL, KAJO_CONFIG_DOMAIN_IDE, on_ide_config_changed, self, NULL);

  ide_window_bind_active_buffer_to_view(self);

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
    ide_window_set_workspace_root(win, workspace_dir);
  }
  return win;
}
