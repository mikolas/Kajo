#include "ide_window_private.h"

/* ─── 1. Toast, Disk Conflict & Conditional Breakpoint Bars ─── */

static gboolean on_toast_timeout_cb(gpointer user_data) {
  KajoIdeWindow *self = user_data;
  self->toast_timer_id = 0;
  if (self->toast_banner)
    gtk_widget_set_visible(self->toast_banner, FALSE);
  return G_SOURCE_REMOVE;
}

static void on_toast_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  KajoIdeWindow *self = user_data;
  if (!self) return;
  g_clear_handle_id(&self->toast_timer_id, g_source_remove);
  if (self->toast_banner)
    gtk_widget_set_visible(self->toast_banner, FALSE);
}

void ide_window_show_toast(KajoIdeWindow *self, const char *msg) {
  if (!self || !self->toast_banner || !msg || !*msg) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_widget_set_visible(self->toast_banner, TRUE);
  g_clear_handle_id(&self->toast_timer_id, g_source_remove);
  self->toast_timer_id = g_timeout_add(3200, on_toast_timeout_cb, self);
}

void ide_window_sync_conflict_bar(KajoIdeWindow *self) {
  if (!self || !self->conflict_bar) return;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (buf && ide_buffer_get_collision(buf)) {
    g_autofree char *msg = g_strdup_printf(
        "⚠ %s changed on disk while you have unsaved edits",
        ide_buffer_get_title(buf));
    gtk_label_set_text(GTK_LABEL(self->conflict_label), msg);
    gtk_widget_set_visible(self->conflict_bar, TRUE);
  } else {
    gtk_widget_set_visible(self->conflict_bar, FALSE);
  }
}

static void on_conflict_reload_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  KajoIdeWindow *self = user_data;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;
  if (ide_buffer_load_from_disk(buf, NULL)) {
    ide_window_sync_conflict_bar(self);
    ide_window_rebuild_tab_ribbon(self);
    ide_window_sync_markdown_views(self);
    ide_window_update_status_bar_and_hud(self);
    ide_window_show_toast(self, "↻ Reloaded file from disk");
    gtk_widget_grab_focus(self->source_view);
  }
}

static void on_conflict_keep_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  KajoIdeWindow *self = user_data;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf) return;
  ide_buffer_clear_collision(buf);
  ide_window_sync_conflict_bar(self);
  ide_window_rebuild_tab_ribbon(self);
  ide_window_show_toast(self, "✓ Kept unsaved buffer edits");
  gtk_widget_grab_focus(self->source_view);
}

void ide_window_close_cond_bp_bar(KajoIdeWindow *self) {
  if (!self || !self->cond_bp_bar) return;
  gtk_widget_set_visible(self->cond_bp_bar, FALSE);
  self->cond_bp_line = 0;
  if (self->source_view) gtk_widget_grab_focus(self->source_view);
}

static void apply_cond_bp_from_entry(KajoIdeWindow *self) {
  if (!self || self->cond_bp_line == 0) return;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (buf) {
    const char *expr = gtk_editable_get_text(GTK_EDITABLE(self->cond_bp_entry));
    ide_buffer_set_breakpoint_condition(buf, self->cond_bp_line, expr);
    if (self->dap) ide_dap_view_breakpoints_changed(self->dap);
    if (self->source_view) gtk_widget_queue_draw(self->source_view);
    const char *stored = ide_buffer_get_breakpoint_condition(buf, self->cond_bp_line);
    g_autofree char *msg = stored
        ? g_strdup_printf("◆ Breakpoint L%u condition: %s", self->cond_bp_line, stored)
        : g_strdup_printf("● Breakpoint L%u condition cleared", self->cond_bp_line);
    ide_window_show_toast(self, msg);
  }
  ide_window_close_cond_bp_bar(self);
}

static void on_cond_bp_activate(GtkEntry *entry G_GNUC_UNUSED, gpointer user_data) {
  apply_cond_bp_from_entry(KAJO_IDE_WINDOW(user_data));
}

static void on_cond_bp_set_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data) {
  apply_cond_bp_from_entry(KAJO_IDE_WINDOW(user_data));
}

static void on_cond_bp_clear_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  if (self->cond_bp_entry) gtk_editable_set_text(GTK_EDITABLE(self->cond_bp_entry), "");
  apply_cond_bp_from_entry(self);
}

static void on_cond_bp_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data) {
  ide_window_close_cond_bp_bar(KAJO_IDE_WINDOW(user_data));
}

void ide_window_open_cond_bp_bar(KajoIdeWindow *self, guint line_1based) {
  if (!self || !self->cond_bp_bar || !self->cond_bp_entry) return;
  IdeBuffer *buf = ide_window_get_active_buffer(self);
  if (!buf || line_1based == 0) return;
  self->cond_bp_line = line_1based;
  g_autofree char *lbl = g_strdup_printf("◆ BP CONDITION (L%u):", line_1based);
  gtk_label_set_text(GTK_LABEL(self->cond_bp_label), lbl);
  const char *existing = ide_buffer_get_breakpoint_condition(buf, line_1based);
  gtk_editable_set_text(GTK_EDITABLE(self->cond_bp_entry), existing ? existing : "");
  gtk_widget_set_visible(self->cond_bp_bar, TRUE);
  gtk_widget_grab_focus(self->cond_bp_entry);
  gtk_editable_select_region(GTK_EDITABLE(self->cond_bp_entry), 0, -1);
}

/* ─── 2. Status Bar, Breadcrumb, Chord HUD & Tab Ribbon ─── */

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
  ide_window_close_buffer_at(self, idx);
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

    if (ide_buffer_get_collision(b)) {
      GtkWidget *col_lbl = gtk_label_new("⚠");
      gtk_widget_add_css_class(col_lbl, "ide-tab-collision");
      gtk_widget_set_valign(col_lbl, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(inner), col_lbl);
    } else if (ide_buffer_get_agent_snapshot(b) != NULL) {
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

/* ─── 3. Header Bar & Drawer Switcher Callbacks ─── */

static void on_hdr_strip_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
  ide_window_toggle_strip_mode(KAJO_IDE_WINDOW(d));
}

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
  IdeBuffer *abuf = ide_window_get_active_buffer(self);
  ide_search_drawer_run_task(
      self->search_drawer, 0, abuf ? ide_buffer_get_file_path(abuf) : NULL);
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

/* ─── 4. Editor Scroll Sync, Context Menu & Pointer Controllers ─── */

static void on_editor_vadj_changed(GtkAdjustment *vadj, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  if (!self) return;
  /* Keep any open LSP popover pinned to the cursor (or dismiss it when the
   * cursor scrolls out of view) so it never floats over unrelated code. */
  if (self->lsp && ide_lsp_manager_popover_visible(self->lsp))
    ide_lsp_manager_reposition_popover(self->lsp, self->source_view);
  if (!gtk_widget_get_visible(self->md_split_container)) return;
  IdeBuffer *abuf = ide_window_get_active_buffer(self);
  const char *active_path = abuf ? ide_buffer_get_file_path(abuf) : NULL;
  const char *md_path = ide_markdown_view_get_doc_path(self->md_split_view);
  if (active_path && md_path && g_strcmp0(active_path, md_path) != 0)
    return;
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
    { "references", IDE_ACTION_LSP_REFERENCES },
    { "signature", IDE_ACTION_LSP_SIGNATURE_HELP },
    { "call_in", IDE_ACTION_LSP_CALL_HIERARCHY_IN },
    { "call_out", IDE_ACTION_LSP_CALL_HIERARCHY_OUT },
    { "hover", IDE_ACTION_LSP_HOVER },
    { "rename", IDE_ACTION_LSP_RENAME },
    { "codeaction", IDE_ACTION_LSP_CODE_ACTION },
    { "format", IDE_ACTION_LSP_FORMAT },
    { "breakpoint", IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
    { "cond_bp", IDE_ACTION_DAP_CONDITIONAL_BREAKPOINT },
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
  const char *acts[] = { "definition", "implementation", "references", "signature",
                         "call_in", "call_out", "hover", "rename",
                         "codeaction", "format", "breakpoint", "cond_bp" };
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
  g_menu_append(nav, "Find References\t g r", "code.references");
  g_menu_append(nav, "Signature Help\t Space g s", "code.signature");
  g_menu_append(nav, "Call Hierarchy (Incoming)\t Space g c", "code.call_in");
  g_menu_append(nav, "Call Hierarchy (Outgoing)\t Space g o", "code.call_out");
  g_menu_append(nav, "Show Documentation\t K", "code.hover");
  GMenu *edit = g_menu_new();
  g_menu_append(edit, "Rename Symbol…\t Space c r", "code.rename");
  g_menu_append(edit, "Code Actions…\t Space c a", "code.codeaction");
  g_menu_append(edit, "Format Buffer\t Space c f", "code.format");
  GMenu *dbg = g_menu_new();
  g_menu_append(dbg, "Toggle Breakpoint\t F9", "code.breakpoint");
  g_menu_append(dbg, "Conditional Breakpoint…\t Shift+F9", "code.cond_bp");

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
  const char *server_only[] = { "implementation", "references", "signature",
                                "call_in", "call_out", "codeaction", "format", NULL };
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
  if (!self || !self->lsp || !ide_lsp_manager_popover_visible(self->lsp))
    return;
  /* Do not dismiss if focus moved into a GtkPopover owned by the editor
   * (e.g. clicking an item inside completion / code actions). */
  GtkWidget *nf = gtk_window_get_focus(GTK_WINDOW(self));
  if (nf && (nf == self->source_view ||
             gtk_widget_is_ancestor(nf, self->source_view) ||
             gtk_widget_get_ancestor(nf, GTK_TYPE_POPOVER) != NULL)) {
    return;
  }
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

/* ─── 5. Modular Window Chrome & Layout Builders ─── */

static GtkWidget *build_header_bar(KajoIdeWindow *self) {
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
  g_signal_connect(self->btn_hdr_tree, "clicked", G_CALLBACK(on_hdr_tree_clicked), self);

  self->btn_hdr_map = gtk_button_new_with_label("🗺 MAP");
  gtk_widget_set_focusable(self->btn_hdr_map, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_map, "files-nav-btn");
  gtk_widget_add_css_class(self->btn_hdr_map, "active");
  gtk_widget_set_valign(self->btn_hdr_map, GTK_ALIGN_CENTER);
  g_signal_connect(self->btn_hdr_map, "clicked", G_CALLBACK(on_hdr_map_clicked), self);

  self->btn_hdr_md = gtk_button_new_with_label("📖 MD");
  gtk_widget_set_focusable(self->btn_hdr_md, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_md, "files-nav-btn");
  gtk_widget_set_valign(self->btn_hdr_md, GTK_ALIGN_CENTER);
  g_signal_connect(self->btn_hdr_md, "clicked", G_CALLBACK(on_hdr_md_clicked), self);

  self->btn_hdr_strip = gtk_button_new_with_label("▥ STRIP");
  gtk_widget_set_focusable(self->btn_hdr_strip, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_strip, "files-nav-btn");
  gtk_widget_set_valign(self->btn_hdr_strip, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(self->btn_hdr_strip,
                              "Toggle 60/25/15 Call-Depth Code Strip (Space t s)");
  g_signal_connect(self->btn_hdr_strip, "clicked", G_CALLBACK(on_hdr_strip_clicked), self);

  GtkWidget *btn_find = gtk_button_new_with_label("🔍 FIND");
  gtk_widget_set_focusable(btn_find, FALSE);
  gtk_widget_add_css_class(btn_find, "files-nav-btn");
  gtk_widget_set_valign(btn_find, GTK_ALIGN_CENTER);
  g_signal_connect(btn_find, "clicked", G_CALLBACK(on_hdr_find_clicked), self);

  self->btn_hdr_help = gtk_button_new_with_label("❓ HELP");
  gtk_widget_set_focusable(self->btn_hdr_help, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_help, "files-nav-btn");
  gtk_widget_set_valign(self->btn_hdr_help, GTK_ALIGN_CENTER);
  g_signal_connect(self->btn_hdr_help, "clicked", G_CALLBACK(on_hdr_help_clicked), self);

  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_tree);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_map);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_md);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_strip);
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
  return win_handle;
}

static void build_conflict_and_cond_bp_bars(KajoIdeWindow *self, GtkWidget *editor_col) {
  /* Inline Disk Conflict Resolution Bar (shown only on external edit conflict) */
  self->conflict_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->conflict_bar, "ide-conflict-bar");
  gtk_widget_set_valign(self->conflict_bar, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->conflict_bar, FALSE);

  self->conflict_label = gtk_label_new("");
  gtk_widget_set_hexpand(self->conflict_label, TRUE);
  gtk_widget_set_valign(self->conflict_label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(self->conflict_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(self->conflict_label), PANGO_ELLIPSIZE_END);

  GtkWidget *btn_conflict_reload = gtk_button_new_with_label("↻ RELOAD FROM DISK");
  gtk_widget_set_focusable(btn_conflict_reload, FALSE);
  gtk_widget_add_css_class(btn_conflict_reload, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_conflict_reload, GTK_ALIGN_CENTER);
  g_signal_connect(btn_conflict_reload, "clicked", G_CALLBACK(on_conflict_reload_clicked), self);

  GtkWidget *btn_conflict_keep = gtk_button_new_with_label("✓ KEEP MY EDITS");
  gtk_widget_set_focusable(btn_conflict_keep, FALSE);
  gtk_widget_add_css_class(btn_conflict_keep, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_conflict_keep, GTK_ALIGN_CENTER);
  g_signal_connect(btn_conflict_keep, "clicked", G_CALLBACK(on_conflict_keep_clicked), self);

  gtk_box_append(GTK_BOX(self->conflict_bar), self->conflict_label);
  gtk_box_append(GTK_BOX(self->conflict_bar), btn_conflict_reload);
  gtk_box_append(GTK_BOX(self->conflict_bar), btn_conflict_keep);
  gtk_box_append(GTK_BOX(editor_col), self->conflict_bar);

  /* Inline Conditional Breakpoint Prompt Bar (Shift+F9 / Space d B / Shift+Click gutter) */
  self->cond_bp_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->cond_bp_bar, "ide-cond-bp-bar");
  gtk_widget_set_valign(self->cond_bp_bar, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->cond_bp_bar, FALSE);

  self->cond_bp_label = gtk_label_new("◆ BP CONDITION:");
  gtk_widget_set_valign(self->cond_bp_label, GTK_ALIGN_CENTER);

  self->cond_bp_entry = gtk_entry_new();
  gtk_widget_set_hexpand(self->cond_bp_entry, TRUE);
  gtk_widget_set_valign(self->cond_bp_entry, GTK_ALIGN_CENTER);
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->cond_bp_entry),
                                 "Condition expression (e.g. count > 5; leave empty to clear)...");
  gtk_widget_add_css_class(self->cond_bp_entry, "shell-launcher-input");
  g_signal_connect(self->cond_bp_entry, "activate", G_CALLBACK(on_cond_bp_activate), self);

  GtkWidget *btn_cond_set = gtk_button_new_with_label("✓ SET");
  gtk_widget_set_focusable(btn_cond_set, FALSE);
  gtk_widget_add_css_class(btn_cond_set, "ide-toolbar-btn");
  gtk_widget_add_css_class(btn_cond_set, "action-btn-primary");
  gtk_widget_set_valign(btn_cond_set, GTK_ALIGN_CENTER);
  g_signal_connect(btn_cond_set, "clicked", G_CALLBACK(on_cond_bp_set_clicked), self);

  GtkWidget *btn_cond_clear = gtk_button_new_with_label("✕ CLEAR");
  gtk_widget_set_focusable(btn_cond_clear, FALSE);
  gtk_widget_add_css_class(btn_cond_clear, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_cond_clear, GTK_ALIGN_CENTER);
  g_signal_connect(btn_cond_clear, "clicked", G_CALLBACK(on_cond_bp_clear_clicked), self);

  GtkWidget *btn_cond_close = gtk_button_new_with_label("×");
  gtk_widget_set_focusable(btn_cond_close, FALSE);
  gtk_widget_add_css_class(btn_cond_close, "ide-tab-close");
  gtk_widget_set_valign(btn_cond_close, GTK_ALIGN_CENTER);
  g_signal_connect(btn_cond_close, "clicked", G_CALLBACK(on_cond_bp_close_clicked), self);

  gtk_box_append(GTK_BOX(self->cond_bp_bar), self->cond_bp_label);
  gtk_box_append(GTK_BOX(self->cond_bp_bar), self->cond_bp_entry);
  gtk_box_append(GTK_BOX(self->cond_bp_bar), btn_cond_set);
  gtk_box_append(GTK_BOX(self->cond_bp_bar), btn_cond_clear);
  gtk_box_append(GTK_BOX(self->cond_bp_bar), btn_cond_close);
  gtk_box_append(GTK_BOX(editor_col), self->cond_bp_bar);
}

static GtkWidget *build_editor_stage(KajoIdeWindow *self) {
  self->editor_md_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->editor_md_paned, "ide-paned");
  gtk_widget_set_hexpand(self->editor_md_paned, TRUE);
  gtk_widget_set_vexpand(self->editor_md_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->editor_md_paned), TRUE);

  GtkWidget *editor_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(editor_col, TRUE);
  gtk_widget_set_vexpand(editor_col, TRUE);

  build_conflict_and_cond_bp_bars(self, editor_col);

  GtkWidget *editor_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(editor_hbox, TRUE);
  gtk_widget_set_vexpand(editor_hbox, TRUE);
  gtk_box_append(GTK_BOX(editor_col), editor_hbox);

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
  g_signal_connect(ed_click, "pressed", G_CALLBACK(on_source_view_pressed), self);
  g_signal_connect(ed_click, "released", G_CALLBACK(on_source_view_released), self);
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
  kajo_ide_gutter_renderer_set_breakpoint_callback(
      self->gutter_renderer, ide_window_on_gutter_bp_toggled, self);
  kajo_ide_gutter_renderer_set_conditional_bp_callback(
      self->gutter_renderer, ide_window_on_gutter_cond_bp_requested, self);
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
  self->md_split_container = ide_markdown_view_get_widget(self->md_split_view);
  gtk_widget_set_size_request(self->md_split_container, 280, -1);
  gtk_widget_set_visible(self->md_split_container, FALSE);

  gtk_paned_set_start_child(GTK_PANED(self->editor_md_paned), editor_col);
  gtk_paned_set_end_child(GTK_PANED(self->editor_md_paned), self->md_split_container);
  gtk_paned_set_resize_start_child(GTK_PANED(self->editor_md_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->editor_md_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(self->editor_md_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->editor_md_paned), TRUE);
  gtk_paned_set_position(GTK_PANED(self->editor_md_paned), 540);
  return self->editor_md_paned;
}

static GtkWidget *build_workspace_overlay(KajoIdeWindow *self) {
  GtkWidget *stage_overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(stage_overlay, TRUE);
  gtk_widget_set_vexpand(stage_overlay, TRUE);

  self->tree_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->tree_paned, "ide-paned");
  gtk_widget_set_hexpand(self->tree_paned, TRUE);
  gtk_widget_set_vexpand(self->tree_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->tree_paned), TRUE);

  self->tree_container = ide_tree_get_widget(self->tree);
  gtk_paned_set_start_child(GTK_PANED(self->tree_paned), self->tree_container);
  gtk_paned_set_resize_start_child(GTK_PANED(self->tree_paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->tree_paned), FALSE);
  gtk_paned_set_position(GTK_PANED(self->tree_paned), 240);

  GtkWidget *editor_stage = build_editor_stage(self);

  self->workspace_stack = gtk_stack_new();
  gtk_widget_set_hexpand(self->workspace_stack, TRUE);
  gtk_widget_set_vexpand(self->workspace_stack, TRUE);
  gtk_stack_set_transition_type(GTK_STACK(self->workspace_stack),
                                GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_add_named(GTK_STACK(self->workspace_stack), editor_stage, "classic");
  gtk_stack_add_named(GTK_STACK(self->workspace_stack),
                      ide_strip_view_get_widget(self->strip_view), "strip");
  gtk_stack_set_visible_child_name(GTK_STACK(self->workspace_stack), "classic");

  gtk_paned_set_end_child(GTK_PANED(self->tree_paned), self->workspace_stack);
  gtk_paned_set_resize_end_child(GTK_PANED(self->tree_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->tree_paned), FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(stage_overlay), self->tree_paned);

  /* Centered Fuzzy Picker Overlay */
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

  /* Top-Right Toast / External Reload Banner (auto-dismissing + dismiss button) */
  self->toast_banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->toast_banner, "ide-chord-hud");
  gtk_widget_set_halign(self->toast_banner, GTK_ALIGN_END);
  gtk_widget_set_valign(self->toast_banner, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->toast_banner, 12);
  gtk_widget_set_margin_end(self->toast_banner, 16);
  gtk_widget_set_visible(self->toast_banner, FALSE);
  self->toast_label = gtk_label_new("");
  gtk_widget_set_valign(self->toast_label, GTK_ALIGN_CENTER);
  GtkWidget *toast_close = gtk_button_new_with_label("×");
  gtk_widget_set_focusable(toast_close, FALSE);
  gtk_widget_add_css_class(toast_close, "ide-tab-close");
  gtk_widget_set_valign(toast_close, GTK_ALIGN_CENTER);
  g_signal_connect(toast_close, "clicked", G_CALLBACK(on_toast_close_clicked), self);
  gtk_box_append(GTK_BOX(self->toast_banner), self->toast_label);
  gtk_box_append(GTK_BOX(self->toast_banner), toast_close);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), self->toast_banner);

  return stage_overlay;
}

static GtkWidget *build_bottom_drawer(KajoIdeWindow *self) {
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
  g_signal_connect(self->drawer_tab_agent, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_dap = gtk_button_new_with_label("🐞 DAP DEBUGGER (Space d v)");
  gtk_widget_set_focusable(self->drawer_tab_dap, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_dap, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_dap, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_dap), "drawer-page", (gpointer)"dap");
  g_signal_connect(self->drawer_tab_dap, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_search = gtk_button_new_with_label("🔎 SEARCH / BUILD (Space s r)");
  gtk_widget_set_focusable(self->drawer_tab_search, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_search, "ide-drawer-tab");
  gtk_widget_add_css_class(self->drawer_tab_search, "active");
  gtk_widget_set_valign(self->drawer_tab_search, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_search), "drawer-page", (gpointer)"search");
  g_signal_connect(self->drawer_tab_search, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_md = gtk_button_new_with_label("❖ MERMAID / MD (Space m m)");
  gtk_widget_set_focusable(self->drawer_tab_md, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_md, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_md, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_md), "drawer-page", (gpointer)"markdown");
  g_signal_connect(self->drawer_tab_md, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

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
  return self->drawer_revealer;
}

static void build_status_bar(KajoIdeWindow *self, GtkWidget *root_vbox) {
  /* Inline Search Bar (/ query) */
  self->inline_search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->inline_search_bar, "ide-drawer-header");
  gtk_widget_set_visible(self->inline_search_bar, FALSE);
  self->inline_search_entry = gtk_label_new("/");
  gtk_box_append(GTK_BOX(self->inline_search_bar), self->inline_search_entry);
  gtk_box_append(GTK_BOX(root_vbox), self->inline_search_bar);

  /* Bottom Status Bar — gallery 3-zone layout: mode+file (left),
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
}

void ide_window_build_ui(KajoIdeWindow *self) {
  GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(self), root_vbox);

  /* 1. Aero + Metro Window Chrome Drag Header (3-Zone Layout) */
  gtk_box_append(GTK_BOX(root_vbox), build_header_bar(self));

  /* 2. Multi-Buffer Tab Ribbon */
  self->tab_ribbon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->tab_ribbon_box, "ide-tab-ribbon");
  gtk_widget_set_valign(self->tab_ribbon_box, GTK_ALIGN_FILL);
  gtk_box_append(GTK_BOX(root_vbox), self->tab_ribbon_box);

  /* 3. Main Workspace Overlay & 4. Contextual Bottom Drawer inside Vertical Paned */
  GtkWidget *stage_overlay = build_workspace_overlay(self);
  GtkWidget *drawer = build_bottom_drawer(self);

  self->vertical_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(self->vertical_paned, "ide-paned");
  gtk_widget_set_hexpand(self->vertical_paned, TRUE);
  gtk_widget_set_vexpand(self->vertical_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->vertical_paned), TRUE);
  gtk_paned_set_start_child(GTK_PANED(self->vertical_paned), stage_overlay);
  gtk_paned_set_end_child(GTK_PANED(self->vertical_paned), drawer);
  gtk_paned_set_resize_start_child(GTK_PANED(self->vertical_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->vertical_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(self->vertical_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->vertical_paned), FALSE);
  gtk_box_append(GTK_BOX(root_vbox), self->vertical_paned);

  /* 5. Inline Search Bar & 6. Bottom Status Bar */
  build_status_bar(self, root_vbox);
}
