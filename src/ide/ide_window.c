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

void ide_window_sync_markdown_views(KajoIdeWindow *self) {
  g_clear_handle_id(&self->md_sync_timer_id, g_source_remove);
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;
  gboolean split_vis = gtk_widget_get_visible(self->md_split_container);
  gboolean drawer_vis =
      gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)) &&
      g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->drawer_stack)),
                "markdown") == 0;
  if (!split_vis && !drawer_vis) return;

  const char *doc_path = ide_buffer_get_file_path(buf);
  const char *title = ide_buffer_get_title(buf);
  gboolean is_md = ide_markdown_is_markdown_path(doc_path ? doc_path : title) ||
                   doc_path == NULL;
  if (!is_md) {
    /* Keep the currently rendered Markdown document pinned when the user
     * navigates into a source file (e.g. clicking a code link inside a spec). */
    if (split_vis && !ide_markdown_view_get_doc_path(self->md_split_view)) {
      ide_markdown_view_show_notice(
          self->md_split_view,
          "Open a Markdown (.md) file to render preview. Non-Markdown buffers are not parsed as Markdown.");
    }
    if (drawer_vis && !ide_markdown_view_get_doc_path(self->md_drawer_view)) {
      ide_markdown_view_show_notice(
          self->md_drawer_view,
          "Open a Markdown (.md) file to render preview. Non-Markdown buffers are not parsed as Markdown.");
    }
    return;
  }

  ide_markdown_view_set_context(
      self->md_split_view, doc_path, self->workspace_root);
  ide_markdown_view_set_context(
      self->md_drawer_view, doc_path, self->workspace_root);

  g_autofree char *text = ide_buffer_get_text(buf);
  if (split_vis) ide_markdown_view_render(self->md_split_view, text);
  if (drawer_vis) ide_markdown_view_render(self->md_drawer_view, text);
}

static gboolean on_md_sync_debounced(gpointer user_data) {
  KajoIdeWindow *self = user_data;
  self->md_sync_timer_id = 0;
  ide_window_sync_markdown_views(self);
  return G_SOURCE_REMOVE;
}

void ide_window_schedule_markdown_sync(KajoIdeWindow *self) {
  if (!self) return;
  gboolean split_vis = self->md_split_container &&
                       gtk_widget_get_visible(self->md_split_container);
  gboolean drawer_vis =
      self->drawer_revealer && self->drawer_stack &&
      gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)) &&
      g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->drawer_stack)),
                "markdown") == 0;
  if (!split_vis && !drawer_vis) return;
  g_clear_handle_id(&self->md_sync_timer_id, g_source_remove);
  self->md_sync_timer_id = g_timeout_add(180, on_md_sync_debounced, self);
}

static void apply_ide_editor_font_css(KajoIdeWindow *self) {
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
    kajo_ide_gutter_renderer_set_relative_numbers(
        self->gutter_renderer, cfg->relative_line_numbers);
    gtk_widget_set_visible(GTK_WIDGET(self->gutter_renderer),
                           cfg->show_line_numbers);
  }

  /* Apply user key remappings from ide.toml [keys.<state>] */
  if (self->fsm) {
    ide_fsm_clear_remaps(self->fsm);
    if (cfg->key_remaps) {
      for (guint i = 0; i < cfg->key_remaps->len; i++) {
        const KajoIdeKeyRemap *rm = g_ptr_array_index(cfg->key_remaps, i);
        if (!rm || !rm->state || !rm->key || !rm->action) continue;
        IdeState st = ide_state_from_name(rm->state);
        IdeActionType act = ide_action_type_from_name(rm->action);
        if (st < IDE_STATE_COUNT && rm->key[0] != '\0' && act != IDE_ACTION_NONE) {
          ide_fsm_set_remap(self->fsm, st, rm->key, act);
        }
      }
    }
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

  if (self->strip_view) {
    ide_strip_view_apply_editor_settings(self->strip_view,
                                         self->active_font_family,
                                         self->active_font_size_px,
                                         CLAMP(cfg->tab_width, 2, 8),
                                         cfg->insert_spaces,
                                         cfg->word_wrap,
                                         cfg->show_margin_guide,
                                         cfg->highlight_current_line,
                                         cfg->show_line_numbers,
                                         cfg->relative_line_numbers,
                                         cfg->strip_magnetic_follow);
    if (!self->initial_config_applied && cfg->start_in_strip_mode) {
      ide_window_set_strip_mode(self, TRUE);
    }
  }
  self->initial_config_applied = TRUE;

  if (self->source_view) gtk_widget_queue_draw(self->source_view);
}

typedef struct {
  int delta_or_zero;
  int out_px;
} IdeZoomCtx;

static void mutate_ide_zoom(KajoIdeCfg *draft, gpointer user_data) {
  IdeZoomCtx *ctx = (IdeZoomCtx *)user_data;
  if (ctx->delta_or_zero == 0) {
    draft->font_size_px = 13;
  } else {
    draft->font_size_px = CLAMP(draft->font_size_px + ctx->delta_or_zero, 10, 24);
  }
  ctx->out_px = draft->font_size_px;
}

static void mutate_ide_rel_lines(KajoIdeCfg *draft, gpointer user_data) {
  gboolean *out_val = (gboolean *)user_data;
  draft->relative_line_numbers = !draft->relative_line_numbers;
  *out_val = draft->relative_line_numbers;
}

void ide_window_toggle_relative_lines(KajoIdeWindow *self) {
  if (!self) return;
  gboolean next_rel = FALSE;
  kajo_config_store_update_ide(NULL, mutate_ide_rel_lines, &next_rel, NULL);
  ide_window_apply_config(self);
  ide_window_show_toast(self, next_rel ? "# Line Numbers: RELATIVE" : "# Line Numbers: ABSOLUTE");
}

void ide_window_on_gutter_bp_toggled(IdeBuffer *buf G_GNUC_UNUSED,
                                     guint      line_1based G_GNUC_UNUSED,
                                     gboolean   enabled G_GNUC_UNUSED,
                                     gpointer   user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  if (self && self->dap) ide_dap_view_breakpoints_changed(self->dap);
}

void ide_window_on_gutter_cond_bp_requested(IdeBuffer *buf G_GNUC_UNUSED,
                                            guint      line_1based,
                                            gpointer   user_data) {
  ide_window_open_cond_bp_bar(KAJO_IDE_WINDOW(user_data), line_1based);
}

GtkWidget *ide_window_get_active_source_view(KajoIdeWindow *self) {
  if (!self) return NULL;
  if (self->strip_view && ide_strip_view_is_active(self->strip_view)) {
    GtkWidget *d0 = ide_strip_view_get_d0_source_view(self->strip_view);
    if (d0) return d0;
  }
  return self->source_view;
}

void ide_window_set_strip_mode(KajoIdeWindow *self, gboolean enabled) {
  if (!self || !self->strip_view || !self->workspace_stack) return;
  gboolean cur = ide_strip_view_is_active(self->strip_view);
  if (cur == enabled) return;

  ide_strip_view_set_active(self->strip_view, enabled);
  gtk_stack_set_visible_child_name(GTK_STACK(self->workspace_stack),
                                   enabled ? "strip" : "classic");
  if (self->tab_ribbon_box)
    gtk_widget_set_visible(self->tab_ribbon_box, !enabled);
  if (self->btn_hdr_strip) {
    if (enabled)
      gtk_widget_add_css_class(self->btn_hdr_strip, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_strip, "active");
  }

  if (enabled) {
    IdeBuffer *buf = ide_window_get_active_buffer(self);
    guint line = 1, col = 1;
    ide_window_get_cursor_line_col(self, &line, &col);
    if (buf) ide_strip_view_bind_buffer(self->strip_view, buf, line, FALSE);
    GtkWidget *d0 = ide_strip_view_get_d0_source_view(self->strip_view);
    if (d0) gtk_widget_grab_focus(d0);
  } else {
    if (self->source_view) gtk_widget_grab_focus(self->source_view);
  }
  ide_window_update_status_bar_and_hud(self);
}

void ide_window_toggle_strip_mode(KajoIdeWindow *self) {
  if (!self || !self->strip_view) return;
  ide_window_set_strip_mode(self, !ide_strip_view_is_active(self->strip_view));
}

static void on_external_buffer_change(IdeBuffer *buf,
                                      gboolean   reloaded_cleanly,
                                      gpointer   user_data);

static IdeBuffer *on_strip_open_buffer(const char *abs_path,
                                       guint       line_1based,
                                       gboolean    focus_buffer,
                                       gpointer    user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  if (!self || !abs_path || !*abs_path) return NULL;
  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *b = g_ptr_array_index(self->buffers, i);
    if (g_strcmp0(ide_buffer_get_file_path(b), abs_path) == 0) {
      if (focus_buffer) {
        self->active_buf_idx = i;
        ide_window_bind_active_buffer_to_view(self);
        if (line_1based > 0) ide_window_move_cursor_to_line_col(self, line_1based, 1);
      }
      return b;
    }
  }
  IdeBuffer *nbuf = ide_buffer_new(abs_path);
  if (!ide_buffer_load_from_disk(nbuf, NULL)) {
    ide_buffer_free(nbuf);
    return NULL;
  }
  if (self->active_scheme_id)
    ide_buffer_set_style_scheme(nbuf, self->active_scheme_id);
  ide_buffer_set_external_change_callback(nbuf, on_external_buffer_change, self);
  ide_lsp_manager_notify_open(self->lsp, nbuf);
  g_ptr_array_add(self->buffers, nbuf);
  if (focus_buffer) {
    self->active_buf_idx = self->buffers->len - 1;
    ide_window_bind_active_buffer_to_view(self);
    if (line_1based > 0) ide_window_move_cursor_to_line_col(self, line_1based, 1);
  }
  return nbuf;
}

static GPtrArray *on_strip_get_buffers(gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  return self ? self->buffers : NULL;
}

static void on_strip_status(const char *msg, gpointer user_data) {
  ide_window_show_toast(KAJO_IDE_WINDOW(user_data), msg);
}

static void on_strip_exit_mode(gpointer user_data) {
  ide_window_set_strip_mode(KAJO_IDE_WINDOW(user_data), FALSE);
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
  KajoIdeWindow *self = user_data;
  self->leader_timer_id = 0;
  if (self->fsm && ide_fsm_expire_leader(self->fsm)) {
    ide_window_update_status_bar_and_hud(self);
  }
  return G_SOURCE_REMOVE;
}

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
  ide_window_sync_conflict_bar(self);
  ide_window_sync_markdown_views(self);
  if (self->strip_view && ide_strip_view_is_active(self->strip_view)) {
    guint line = 1, col = 1;
    ide_window_get_cursor_line_col(self, &line, &col);
    ide_strip_view_bind_buffer(self->strip_view, buf, line, FALSE);
  }
  ide_window_update_status_bar_and_hud(self);
  GtkWidget *active_view = ide_window_get_active_source_view(self);
  if (active_view) {
    gtk_widget_grab_focus(active_view);
    gtk_widget_queue_draw(active_view);
  }
}

void ide_window_close_buffer_at(KajoIdeWindow *self, guint idx) {
  if (!self || !self->buffers) return;
  if (self->buffers->len <= 1) {
    IdeBuffer *only = g_ptr_array_index(self->buffers, 0);
    if (ide_buffer_get_file_path(only) != NULL ||
        ide_buffer_get_modified(only)) {
      GtkSourceBuffer *old_sb = ide_buffer_get_source_buffer(only);
      if (old_sb)
        g_signal_handlers_disconnect_by_func(old_sb, G_CALLBACK(on_buffer_mark_set), self);
      IdeBuffer *fresh = ide_buffer_new(NULL);
      if (self->active_scheme_id)
        ide_buffer_set_style_scheme(fresh, self->active_scheme_id);
      if (self->strip_view) ide_strip_view_notify_buffer_closed(self->strip_view, only);
      g_ptr_array_remove_index(self->buffers, 0);
      g_ptr_array_add(self->buffers, fresh);
      self->active_buf_idx = 0;
      ide_window_bind_active_buffer_to_view(self);
    }
    return;
  }
  if (idx < self->buffers->len) {
    IdeBuffer *closing = g_ptr_array_index(self->buffers, idx);
    GtkSourceBuffer *old_sb = closing ? ide_buffer_get_source_buffer(closing) : NULL;
    if (old_sb)
      g_signal_handlers_disconnect_by_func(old_sb, G_CALLBACK(on_buffer_mark_set), self);
    if (self->strip_view) ide_strip_view_notify_buffer_closed(self->strip_view, closing);
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
  int max_line = MAX(0, gtk_text_buffer_get_line_count(tbuf) - 1);
  int line0 = line_1based > 0 ? CLAMP((int)line_1based - 1, 0, max_line) : 0;
  int col0 = col_1based > 0 ? (int)col_1based - 1 : 0;
  gtk_text_buffer_get_iter_at_line_offset(tbuf, &iter, line0, 0);
  if (col0 > 0) {
    GtkTextIter line_end = iter;
    if (!gtk_text_iter_ends_line(&line_end))
      gtk_text_iter_forward_to_line_end(&line_end);
    int max_col = gtk_text_iter_get_line_offset(&line_end);
    gtk_text_iter_set_line_offset(&iter, MIN(col0, max_col));
  }
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
  ide_window_rebuild_tab_ribbon(self);
  ide_window_sync_conflict_bar(self);
  if (buf == ide_window_get_active_buffer(self)) {
    ide_window_sync_markdown_views(self);
  }
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
      if (self->strip_view) ide_strip_view_notify_buffer_closed(self->strip_view, first);
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
  if (self->strip_view)
    ide_strip_view_set_workspace_root(self->strip_view, self->workspace_root);
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

/* ─── Key Translation & Capture-Phase FSM Dispatcher ─── */

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
      case GDK_KEY_space: return g_strdup(shift ? "ctrl+shift+space" : "ctrl+space");
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
      case GDK_KEY_F9:           return g_strdup("shift+f9");
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

  /* Allow DAP REPL, DAP Watch, and Conditional Breakpoint GtkEntries to receive
   * direct text input & Enter activation without being intercepted by the FSM. */
  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(self));
  GtkWidget *repl_entry = self->dap ? ide_dap_view_get_repl_entry(self->dap) : NULL;
  GtkWidget *watch_entry = self->dap ? ide_dap_view_get_watch_entry(self->dap) : NULL;
  GtkWidget *strip_filter = self->strip_view ? ide_strip_view_get_filter_entry(self->strip_view) : NULL;
  GtkWidget *strip_ov = self->strip_view ? ide_strip_view_get_overview_entry(self->strip_view) : NULL;
  gboolean in_cond_bp = (focus && self->cond_bp_entry &&
                         (focus == self->cond_bp_entry ||
                          gtk_widget_is_ancestor(focus, self->cond_bp_entry)));
  gboolean in_strip_entry = (focus &&
                             ((strip_filter &&
                               (focus == strip_filter || gtk_widget_is_ancestor(focus, strip_filter))) ||
                              (strip_ov &&
                               (focus == strip_ov || gtk_widget_is_ancestor(focus, strip_ov)))));
  gboolean in_dap_entry = (focus &&
                           ((repl_entry &&
                             (focus == repl_entry || gtk_widget_is_ancestor(focus, repl_entry))) ||
                            (watch_entry &&
                             (focus == watch_entry || gtk_widget_is_ancestor(focus, watch_entry)))));
  if (in_cond_bp) {
    if (keyval == GDK_KEY_Escape) {
      ide_window_close_cond_bp_bar(self);
      return TRUE;
    }
    return FALSE;
  }
  if (in_strip_entry) {
    if (keyval == GDK_KEY_Escape) {
      if (strip_ov && (focus == strip_ov || gtk_widget_is_ancestor(focus, strip_ov)) &&
          ide_strip_view_is_overview(self->strip_view)) {
        ide_strip_view_toggle_overview(self->strip_view);
      }
      GtkWidget *av = ide_window_get_active_source_view(self);
      if (av) gtk_widget_grab_focus(av);
      return TRUE;
    }
    return FALSE;
  }
  if (in_dap_entry) {
    if (keyval == GDK_KEY_Escape) {
      ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      GtkWidget *av = ide_window_get_active_source_view(self);
      if (av) gtk_widget_grab_focus(av);
      ide_window_update_status_bar_and_hud(self);
      return TRUE;
    }
    return FALSE;
  }

  /* In Code Strip Overview, Escape closes Overview cleanly */
  if (self->strip_view && ide_strip_view_is_active(self->strip_view) &&
      ide_strip_view_is_overview(self->strip_view) && keyval == GDK_KEY_Escape) {
    ide_strip_view_toggle_overview(self->strip_view);
    GtkWidget *av = ide_window_get_active_source_view(self);
    if (av) gtk_widget_grab_focus(av);
    return TRUE;
  }

  /* In Code Strip mode, Alt+Left surfaces back -1 depth via Ancestor Spine */
  if (self->strip_view && ide_strip_view_is_active(self->strip_view) &&
      (state & GDK_ALT_MASK) != 0 && keyval == GDK_KEY_Left) {
    ide_strip_view_surface_one(self->strip_view);
    return TRUE;
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

/* ─── Subsystem Callbacks ─── */

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
    case IDE_LSP_UI_REFERENCES:
    case IDE_LSP_UI_SIGNATURE:
    case IDE_LSP_UI_CALL_HIERARCHY:
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
    bs->conditions = g_ptr_array_new_with_free_func(g_free);
    for (guint k = 0; k < lines->len; k++) {
      guint line_1based = g_array_index(lines, guint, k);
      const char *cond = ide_buffer_get_breakpoint_condition(b, line_1based);
      g_ptr_array_add(bs->conditions, g_strdup(cond ? cond : ""));
    }
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
  guint saved_line = 1, saved_col = 1;
  ide_window_get_cursor_line_col(self, &saved_line, &saved_col);
  ide_buffer_set_text(buf, new_full_text);
  ide_window_move_cursor_to_line_col(self, saved_line, saved_col);
  ide_window_rebuild_tab_ribbon(self);
}

/* ─── Lifecycle ─── */

static void kajo_ide_window_dispose(GObject *object) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(object);
  g_clear_handle_id(&self->leader_timer_id, g_source_remove);
  g_clear_handle_id(&self->toast_timer_id, g_source_remove);
  g_clear_handle_id(&self->md_sync_timer_id, g_source_remove);
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
  g_clear_pointer(&self->strip_view, ide_strip_view_free);
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

static void init_subsystems(KajoIdeWindow *self) {
  self->tree = ide_tree_new(self->workspace_root, on_tree_open_file, self);

  self->md_split_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);
  ide_markdown_view_set_open_file_cb(self->md_split_view, on_picker_open_file);

  self->lsp = ide_lsp_manager_new(
      self->workspace_root, on_picker_open_file, on_lsp_status_changed, self);
  ide_lsp_manager_set_callbacks(
      self->lsp, on_lsp_diagnostics, on_lsp_message, on_lsp_ui_state, self);

  self->strip_view = ide_strip_view_new(self->workspace_root,
                                        self->lsp,
                                        on_strip_open_buffer,
                                        on_strip_get_buffers,
                                        on_strip_status,
                                        on_strip_exit_mode,
                                        self);
  ide_strip_view_set_gutter_callbacks(self->strip_view,
                                      ide_window_on_gutter_bp_toggled,
                                      ide_window_on_gutter_cond_bp_requested,
                                      self);

  self->picker = ide_picker_new(
      self->workspace_root, on_picker_open_file, on_picker_command, self);
  self->search_drawer = ide_search_drawer_new(
      self->workspace_root, on_picker_open_file, NULL, self);
  self->dap = ide_dap_view_new(on_dap_action_cb, self);
  ide_dap_view_set_context(
      self->dap, self->workspace_root, on_dap_breakpoints, on_dap_frame, NULL, self);
  ide_dap_view_set_java_exec(self->dap, on_dap_java_exec, self);
  self->agent_ipc = ide_agent_ipc_new(
      on_picker_open_file, on_agent_reload_cb, on_agent_status_cb, self);
  self->md_drawer_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);
  ide_markdown_view_set_open_file_cb(self->md_drawer_view, on_picker_open_file);
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

  init_subsystems(self);
  ide_window_build_ui(self);
  ide_agent_ipc_start_default(self->agent_ipc);

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
