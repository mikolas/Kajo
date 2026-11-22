#include "ide_strip_view_private.h"

/* ═══════════════════════════ Small Utilities ═══════════════════════════ */

double
ide_strip_view_now_sec(void)
{
  return (double)g_get_monotonic_time() / 1000000.0;
}

void
ide_strip_view_emit_status(IdeStripView *sv, const char *msg)
{
  if (sv != NULL && sv->status_cb != NULL && msg != NULL) {
    sv->status_cb(msg, sv->user_data);
  }
}

const char *
ide_strip_view_lens_css_class(IdeStripLens lens)
{
  switch (lens) {
    case IDE_STRIP_LENS_TYPES:  return "lens-types";
    case IDE_STRIP_LENS_IMPACT: return "lens-impact";
    case IDE_STRIP_LENS_DIFF:   return "lens-diff";
    case IDE_STRIP_LENS_CALLS:
    default:                    return "lens-calls";
  }
}

static char *
compute_rel_path(const IdeStripView *sv, const char *abs_path)
{
  if (abs_path == NULL || abs_path[0] == '\0') {
    return g_strdup("");
  }
  const char *root = sv != NULL ? ide_strip_graph_get_workspace_root(sv->graph) : NULL;
  if (root != NULL && root[0] != '\0') {
    g_autofree char *canon = g_canonicalize_filename(abs_path, root);
    gsize rlen = strlen(root);
    if (canon != NULL && strncmp(canon, root, rlen) == 0 && canon[rlen] == '/') {
      return g_strdup(canon + rlen + 1);
    }
  }
  return g_path_get_basename(abs_path);
}

IdeBuffer *
ide_strip_view_find_open_buffer(const IdeStripView *sv, const char *file_path)
{
  if (sv == NULL || sv->get_bufs_cb == NULL || file_path == NULL || file_path[0] == '\0') {
    return NULL;
  }
  GPtrArray *bufs = sv->get_bufs_cb(sv->user_data);
  if (bufs == NULL) {
    return NULL;
  }
  g_autofree char *target = g_canonicalize_filename(file_path, NULL);
  for (guint i = 0; i < bufs->len; i++) {
    IdeBuffer *b = g_ptr_array_index(bufs, i);
    const char *bp = b != NULL ? ide_buffer_get_file_path(b) : NULL;
    if (bp == NULL) {
      continue;
    }
    if (strcmp(bp, file_path) == 0) {
      return b;
    }
    g_autofree char *cb = g_canonicalize_filename(bp, NULL);
    if (target != NULL && cb != NULL && strcmp(cb, target) == 0) {
      return b;
    }
  }
  return NULL;
}

void
ide_strip_view_clear_box(GtkWidget *box)
{
  if (box == NULL) {
    return;
  }
  GtkWidget *child = gtk_widget_get_first_child(box);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(box), child);
    child = next;
  }
}

guint
ide_strip_view_get_d0_cursor_line(const IdeStripView *sv)
{
  if (sv == NULL || sv->d0_tbuf == NULL) {
    return 1;
  }
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(sv->d0_tbuf, &iter, gtk_text_buffer_get_insert(sv->d0_tbuf));
  return (guint)gtk_text_iter_get_line(&iter) + 1;
}

guint
ide_strip_view_get_d0_cursor_col(const IdeStripView *sv)
{
  if (sv == NULL || sv->d0_tbuf == NULL) {
    return 1;
  }
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(sv->d0_tbuf, &iter, gtk_text_buffer_get_insert(sv->d0_tbuf));
  return (guint)gtk_text_iter_get_line_offset(&iter) + 1;
}

void
ide_strip_view_scroll_source_to_line(GtkWidget *source_view,
                                     guint      line_1based,
                                     gboolean   move_cursor,
                                     double     yalign)
{
  if (source_view == NULL) {
    return;
  }
  GtkTextBuffer *tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(source_view));
  if (tbuf == NULL) {
    return;
  }
  int total = gtk_text_buffer_get_line_count(tbuf);
  int line0 = (line_1based > 0) ? (int)(line_1based - 1) : 0;
  line0 = CLAMP(line0, 0, MAX(0, total - 1));

  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_line(tbuf, &iter, line0);
  if (move_cursor) {
    gtk_text_buffer_place_cursor(tbuf, &iter);
  }
  /* Guard against GTK 4.24 GtkTextLayout display-cache corruption when
   * scroll_to_iter() populates layout->cached_displays before realize and
   * gtk_text_view_set_buffer() later swaps buffers. */
  if (gtk_widget_get_mapped(source_view)) {
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(source_view), &iter, 0.0, TRUE, 0.0, yalign);
  }
}

/* ═══════════════════════════ Buffer Signal Management ═══════════════════════════ */

static void
on_d0_buffer_mark_set(GtkTextBuffer *buf G_GNUC_UNUSED,
                      GtkTextIter   *loc G_GNUC_UNUSED,
                      GtkTextMark   *mark,
                      gpointer       user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || !sv->model->strip_active) {
    return;
  }
  if (mark == gtk_text_buffer_get_insert(buf)) {
    ide_strip_tiles_schedule_viewport_band(sv);
  }
}

static void
on_d0_buffer_changed(GtkTextBuffer *buf G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return;
  }
  if (sv->d0_veil_overlay != NULL) {
    gtk_widget_queue_draw(sv->d0_veil_overlay);
  }
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

static void
detach_d0_buffer_signals(IdeStripView *sv)
{
  if (sv == NULL) {
    return;
  }
  if (sv->d0_tbuf != NULL) {
    if (sv->d0_mark_sig_id != 0) {
      g_signal_handler_disconnect(sv->d0_tbuf, sv->d0_mark_sig_id);
      sv->d0_mark_sig_id = 0;
    }
    if (sv->d0_changed_sig_id != 0) {
      g_signal_handler_disconnect(sv->d0_tbuf, sv->d0_changed_sig_id);
      sv->d0_changed_sig_id = 0;
    }
    g_clear_object(&sv->d0_tbuf);
  }
  sv->d0_buf = NULL;
}

static void
attach_d0_buffer_internal(IdeStripView *sv, IdeBuffer *buf)
{
  if (sv == NULL) {
    return;
  }
  if (sv->d0_buf == buf && buf != NULL) {
    return;
  }
  detach_d0_buffer_signals(sv);
  sv->d0_buf = buf;

  if (buf == NULL) {
    if (sv->d0_source_view != NULL && sv->empty_d0_buf != NULL) {
      gtk_text_view_set_buffer(GTK_TEXT_VIEW(sv->d0_source_view),
                               GTK_TEXT_BUFFER(sv->empty_d0_buf));
    }
    if (sv->d0_gutter != NULL) {
      kajo_ide_gutter_renderer_set_buffer(sv->d0_gutter, NULL);
    }
    return;
  }

  GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(buf);
  if (sbuf != NULL) {
    GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(sbuf);
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(sv->d0_source_view), tbuf);
    sv->d0_tbuf = g_object_ref(tbuf);
    sv->d0_mark_sig_id = g_signal_connect(tbuf, "mark-set",
                                          G_CALLBACK(on_d0_buffer_mark_set), sv);
    sv->d0_changed_sig_id = g_signal_connect(tbuf, "changed",
                                             G_CALLBACK(on_d0_buffer_changed), sv);
  }
  if (sv->d0_gutter != NULL) {
    kajo_ide_gutter_renderer_set_buffer(sv->d0_gutter, buf);
  }
}

gboolean
ide_strip_view_open_and_attach_d0(IdeStripView *sv,
                                  const char   *file_path,
                                  guint         line_1based,
                                  gboolean      focus_buffer)
{
  if (sv == NULL || file_path == NULL || file_path[0] == '\0') {
    return FALSE;
  }
  IdeBuffer *buf = NULL;
  if (sv->open_buf_cb != NULL) {
    sv->binding_internal = TRUE;
    buf = sv->open_buf_cb(file_path, line_1based, focus_buffer, sv->user_data);
    sv->binding_internal = FALSE;
  } else {
    buf = ide_strip_view_find_open_buffer(sv, file_path);
  }
  if (buf == NULL) {
    return FALSE;
  }
  attach_d0_buffer_internal(sv, buf);
  if (line_1based > 0) {
    ide_strip_view_scroll_source_to_line(sv->d0_source_view, line_1based, TRUE, 0.15);
  }
  return TRUE;
}

/* ═══════════════════════════ Analytical Spring Tick ═══════════════════════════ */

static gboolean
on_spring_tick(GtkWidget *widget G_GNUC_UNUSED,
               GdkFrameClock *clock G_GNUC_UNUSED,
               gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->model == NULL) {
    return G_SOURCE_REMOVE;
  }

  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    sv->tick_id = 0;
    return G_SOURCE_REMOVE;
  }

  gboolean animating = ide_strip_trail_step_springs(trail, ide_strip_view_now_sec());

  if (sv->d1_scroll != NULL && !trail->d1_unlinked) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d1_scroll));
    if (vadj != NULL) {
      double upper = gtk_adjustment_get_upper(vadj);
      double page = gtk_adjustment_get_page_size(vadj);
      double max_val = MAX(0.0, upper - page);
      double target = CLAMP(trail->d1_scroll_y, 0.0, max_val);
      sv->programmatic_d1_scroll = TRUE;
      gtk_adjustment_set_value(vadj, target);
      sv->programmatic_d1_scroll = FALSE;
    }
  }

  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  if (sv->d0_veil_overlay != NULL) {
    gtk_widget_queue_draw(sv->d0_veil_overlay);
  }

  if (!animating) {
    sv->tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void
ide_strip_view_ensure_spring_tick(IdeStripView *sv)
{
  if (sv == NULL || sv->root_box == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  if (!trail->d1_scroll_animating && !trail->depth_slide_animating) {
    return;
  }
  if (sv->tick_id == 0) {
    sv->tick_id = gtk_widget_add_tick_callback(sv->root_box, on_spring_tick, sv, NULL);
  }
}

/* ═══════════════════════════ Paned 60% | 25% | 15% Sizing ═══════════════════════════ */

static gboolean
apply_paned_layout_idle(gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return G_SOURCE_REMOVE;
  }
  sv->paned_idle_id = 0;

  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || sv->strip_overlay == NULL) {
    return G_SOURCE_REMOVE;
  }

  int total_w = gtk_widget_get_width(sv->strip_overlay);
  if (total_w <= 64) {
    return G_SOURCE_REMOVE;
  }

  guint depth = trail->ancestors != NULL ? trail->ancestors->len : 0;
  IdeStripGeometry geom;
  ide_strip_compute_geometry((double)total_w, depth, trail->zen_mode, &geom);

  if (trail->zen_mode) {
    if (sv->zen_rail_btn != NULL) {
      gtk_widget_set_size_request(sv->zen_rail_btn, (int)geom.zen_rail_w, -1);
    }
    return G_SOURCE_REMOVE;
  }

  sv->programmatic_paned_pos = TRUE;
  if (!sv->user_dragged_outer_paned && sv->outer_paned != NULL) {
    int pos = MAX(180, (int)geom.d0_w);
    gtk_paned_set_position(GTK_PANED(sv->outer_paned), pos);
  }
  if (!sv->user_dragged_right_paned && sv->right_paned != NULL) {
    int pos = MAX(120, (int)(geom.d1_w + geom.bridge_w));
    gtk_paned_set_position(GTK_PANED(sv->right_paned), pos);
  }
  sv->programmatic_paned_pos = FALSE;
  sv->paned_layout_initialized = TRUE;

  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  return G_SOURCE_REMOVE;
}

void
ide_strip_view_schedule_paned_layout(IdeStripView *sv)
{
  if (sv == NULL || sv->paned_idle_id != 0) {
    return;
  }
  sv->paned_idle_id = g_idle_add(apply_paned_layout_idle, sv);
}

static void
on_outer_paned_pos_changed(GObject *obj G_GNUC_UNUSED,
                           GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return;
  }
  if (sv->paned_layout_initialized && !sv->programmatic_paned_pos && sv->last_alloc_w > 64) {
    sv->user_dragged_outer_paned = TRUE;
  }
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

static void
on_right_paned_pos_changed(GObject *obj G_GNUC_UNUSED,
                           GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return;
  }
  if (sv->paned_layout_initialized && !sv->programmatic_paned_pos && sv->last_alloc_w > 64) {
    sv->user_dragged_right_paned = TRUE;
  }
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

/* ═══════════════════════════ Toolbar & D0 Header Rebuilders ═══════════════════════════ */

static void
on_trail_pill_clicked(GtkButton *btn, gpointer user_data)
{
  IdeStripView *sv = user_data;
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "trail-idx"));
  if (sv == NULL || !ide_strip_model_select_trail(sv->model, idx)) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail != NULL && trail->root_tile != NULL) {
    ide_strip_view_open_and_attach_d0(sv, trail->root_tile->file_path, trail->root_tile->start_line, TRUE);
  }
  ide_strip_view_rebuild_all_ui(sv);
  if (trail != NULL && (trail->d1_tiles == NULL || trail->d1_tiles->len == 0)) {
    ide_strip_tiles_refresh_d1(sv);
  }
}

void
ide_strip_view_rebuild_toolbar(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);

  /* Trail switcher pills */
  ide_strip_view_clear_box(sv->trails_box);
  if (sv->model->trails != NULL) {
    for (guint i = 0; i < sv->model->trails->len; i++) {
      const IdeStripTrail *t = g_ptr_array_index(sv->model->trails, i);
      g_autofree char *lbl = g_strdup_printf("%02u · %s",
                                             t->id,
                                             t->title != NULL ? t->title : "Main");
      GtkWidget *pill = gtk_button_new_with_label(lbl);
      gtk_widget_set_focusable(pill, FALSE);
      gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(pill, "ide-strip-trail-pill");
      if (i == sv->model->active_trail_idx) {
        gtk_widget_add_css_class(pill, "active");
      }
      g_object_set_data(G_OBJECT(pill), "trail-idx", GUINT_TO_POINTER(i));
      g_signal_connect(pill, "clicked", G_CALLBACK(on_trail_pill_clicked), sv);
      gtk_box_append(GTK_BOX(sv->trails_box), pill);
    }
  }

  /* Lens buttons */
  for (int i = 0; i < 4; i++) {
    if (sv->lens_btns[i] == NULL) {
      continue;
    }
    if (trail != NULL && (int)trail->lens == i) {
      gtk_widget_add_css_class(sv->lens_btns[i], "active");
    } else {
      gtk_widget_remove_css_class(sv->lens_btns[i], "active");
    }
  }

  /* Workspace-only & engine badges */
  if (sv->ws_badge != NULL && trail != NULL) {
    g_autofree char *ws_txt = g_strdup_printf("WS ONLY (%u EXT)",
                                              trail->ext_suppressed_count);
    gtk_label_set_text(GTK_LABEL(sv->ws_badge), ws_txt);
    g_autofree char *ws_tip = g_strdup_printf(
        "Workspace-only tiles: %u external/stdlib symbols suppressed",
        trail->ext_suppressed_count);
    gtk_widget_set_tooltip_text(sv->ws_badge, ws_tip);
  }
  if (sv->engine_badge != NULL && trail != NULL) {
    if (trail->using_fallback) {
      gtk_label_set_text(GTK_LABEL(sv->engine_badge), "⚠ TS+RG");
      gtk_widget_set_tooltip_text(
          sv->engine_badge,
          "Tree-sitter + ripgrep workspace fallback (no active LSP server)");
      gtk_widget_add_css_class(sv->engine_badge, "is-fallback");
      gtk_widget_remove_css_class(sv->engine_badge, "is-lsp");
    } else {
      gtk_label_set_text(GTK_LABEL(sv->engine_badge), "⚡ LSP");
      gtk_widget_set_tooltip_text(sv->engine_badge,
                                  "Live LSP call-hierarchy / definition engine");
      gtk_widget_add_css_class(sv->engine_badge, "is-lsp");
      gtk_widget_remove_css_class(sv->engine_badge, "is-fallback");
    }
  }

  if (sv->zen_btn != NULL && trail != NULL) {
    if (trail->zen_mode) {
      gtk_widget_add_css_class(sv->zen_btn, "active");
    } else {
      gtk_widget_remove_css_class(sv->zen_btn, "active");
    }
  }
  if (sv->overview_btn != NULL && trail != NULL) {
    if (trail->overview_mode) {
      gtk_widget_add_css_class(sv->overview_btn, "active");
    } else {
      gtk_widget_remove_css_class(sv->overview_btn, "active");
    }
  }
}

void
ide_strip_view_rebuild_d0_header_and_pin(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  /* D0 Header */
  if (trail->root_tile != NULL) {
    const IdeStripTile *rt = trail->root_tile;
    gtk_label_set_text(GTK_LABEL(sv->d0_kind_badge), ide_strip_tile_kind_badge(rt->kind));
    gtk_label_set_text(GTK_LABEL(sv->d0_title_label),
                       rt->name != NULL ? rt->name : "(untitled)");
    g_autofree char *loc = g_strdup_printf("%s:%u–%u",
                                           rt->rel_path != NULL ? rt->rel_path : "",
                                           rt->start_line, rt->end_line);
    gtk_label_set_text(GTK_LABEL(sv->d0_loc_label), loc);
  } else {
    gtk_label_set_text(GTK_LABEL(sv->d0_kind_badge), "·");
    gtk_label_set_text(GTK_LABEL(sv->d0_title_label), "No active buffer");
    gtk_label_set_text(GTK_LABEL(sv->d0_loc_label), "");
  }

  if (sv->d0_fullfile_btn != NULL) {
    gtk_button_set_label(GTK_BUTTON(sv->d0_fullfile_btn),
                         trail->show_full_file ? "⤡ SYMBOL FOCUS" : "⤢ FULL FILE");
    if (trail->show_full_file) {
      gtk_widget_add_css_class(sv->d0_fullfile_btn, "active");
    } else {
      gtk_widget_remove_css_class(sv->d0_fullfile_btn, "active");
    }
  }

  /* Optional Top 30% Pinned Reference Pane */
  gboolean has_pin = (trail->pinned_tile != NULL);
  gtk_widget_set_visible(sv->pin_box, has_pin);
  if (has_pin) {
    const IdeStripTile *pt = trail->pinned_tile;
    g_autofree char *ptitle = g_strdup_printf("📌 Pinned: %s · %s:%u",
                                              pt->name != NULL ? pt->name : "",
                                              pt->rel_path != NULL ? pt->rel_path : "",
                                              pt->start_line);
    gtk_label_set_text(GTK_LABEL(sv->pin_title_label), ptitle);
    int vh = gtk_widget_get_height(sv->d0_vpaned);
    if (vh > 100) {
      gtk_paned_set_position(GTK_PANED(sv->d0_vpaned), (int)((double)vh * 0.30));
    }
  }

  /* Zen mode visibility */
  gtk_widget_set_visible(sv->right_paned, !trail->zen_mode);
  gtk_widget_set_visible(sv->zen_rail_btn, trail->zen_mode);
  if (trail->zen_mode && sv->zen_rail_label != NULL) {
    guint n1 = trail->d1_tiles != NULL ? trail->d1_tiles->len : 0;
    guint n2 = trail->d2_tiles != NULL ? trail->d2_tiles->len : 0;
    g_autofree char *ztxt = g_strdup_printf("D+1: %u\nD+2: %u\n[ ⤡ ]", n1, n2);
    gtk_label_set_text(GTK_LABEL(sv->zen_rail_label), ztxt);
  }

  /* Sync D+1 filter entry text cleanly */
  if (sv->d1_filter_entry != NULL) {
    const char *want = trail->d1_filter != NULL ? trail->d1_filter : "";
    const char *cur = gtk_editable_get_text(GTK_EDITABLE(sv->d1_filter_entry));
    if (g_strcmp0(want, cur) != 0) {
      sv->updating_filter_text = TRUE;
      gtk_editable_set_text(GTK_EDITABLE(sv->d1_filter_entry), want);
      sv->updating_filter_text = FALSE;
    }
  }

  /* Stack page: strip vs overview */
  if (sv->view_stack != NULL) {
    gtk_stack_set_visible_child_name(GTK_STACK(sv->view_stack),
                                     trail->overview_mode ? "overview" : "strip");
  }

  if (sv->d0_veil_overlay != NULL) {
    gtk_widget_queue_draw(sv->d0_veil_overlay);
  }
  ide_strip_view_schedule_paned_layout(sv);
}

void
ide_strip_view_rebuild_all_ui(IdeStripView *sv)
{
  if (sv == NULL) {
    return;
  }
  ide_strip_view_rebuild_toolbar(sv);
  ide_strip_tiles_rebuild_spine(sv);
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  ide_strip_tiles_rebuild_d1(sv);
  ide_strip_tiles_rebuild_d2(sv);
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  if (sv->overview_canvas != NULL) {
    gtk_widget_queue_draw(sv->overview_canvas);
  }
}

/* ═══════════════════════════ Signal Callbacks ═══════════════════════════ */

static void
on_d0_vadj_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return;
  }
  if (sv->d0_veil_overlay != NULL) {
    gtk_widget_queue_draw(sv->d0_veil_overlay);
  }
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  ide_strip_tiles_schedule_viewport_band(sv);
}

static void
on_root_box_destroy(GtkWidget *w G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return;
  }
  sv->root_box = NULL;
  sv->tick_id = 0;
}

static void
on_lens_btn_clicked(GtkButton *btn, gpointer user_data)
{
  IdeStripView *sv = user_data;
  IdeStripLens lens = (IdeStripLens)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "lens-id"));
  ide_strip_view_set_lens(sv, lens);
}

static void
on_branch_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_branch_trail((IdeStripView *)user_data);
}

static void
on_zen_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_toggle_zen((IdeStripView *)user_data);
}

static void
on_overview_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_toggle_overview((IdeStripView *)user_data);
}

static void
on_exit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->exit_cb != NULL) {
    sv->exit_cb(sv->user_data);
  }
}

static void
on_fullfile_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_toggle_full_file((IdeStripView *)user_data);
}

static void
on_pin_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_unpin((IdeStripView *)user_data);
}

static void
on_gutter_bp(IdeBuffer *buf, guint line_1based, gboolean enabled, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->bp_cb != NULL) {
    sv->bp_cb(buf, line_1based, enabled, sv->gutter_user_data);
  }
}

static void
on_gutter_cond_bp(IdeBuffer *buf, guint line_1based, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->cond_bp_cb != NULL) {
    sv->cond_bp_cb(buf, line_1based, sv->gutter_user_data);
  }
}

/* ═══════════════════════════ Modular Widget Construction ═══════════════════════════ */

GtkWidget *
ide_strip_view_make_toolbar_btn(const char *label, const char *css_class, const char *tooltip)
{
  GtkWidget *btn = gtk_button_new_with_label(label);
  gtk_widget_set_focusable(btn, FALSE);
  gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
  if (css_class != NULL) {
    gtk_widget_add_css_class(btn, css_class);
  }
  if (tooltip != NULL) {
    gtk_widget_set_tooltip_text(btn, tooltip);
  }
  return btn;
}

static GtkWidget *
build_toolbar_ui(IdeStripView *sv)
{
  GtkWidget *tb_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tb_scroll),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(tb_scroll), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(tb_scroll), TRUE);
  gtk_widget_set_hexpand(tb_scroll, TRUE);

  sv->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(sv->toolbar, "ide-strip-toolbar");
  gtk_widget_set_size_request(sv->toolbar, -1, 28);

  /* Left: Trail pills + Branch button */
  GtkWidget *left_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_set_valign(left_bar, GTK_ALIGN_CENTER);
  sv->trails_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_set_valign(sv->trails_box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(left_bar), sv->trails_box);

  sv->branch_btn = ide_strip_view_make_toolbar_btn("+ BRANCH", "ide-strip-tool-btn", "Branch vertical trail (Space t b)");
  g_signal_connect(sv->branch_btn, "clicked", G_CALLBACK(on_branch_clicked), sv);
  gtk_box_append(GTK_BOX(left_bar), sv->branch_btn);
  gtk_box_append(GTK_BOX(sv->toolbar), left_bar);

  /* Center spacer + 4 Lens buttons */
  GtkWidget *sp_l = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(sp_l, TRUE);
  gtk_box_append(GTK_BOX(sv->toolbar), sp_l);

  static const char *const kLensLabels[4] = {
    "1 CALLS", "2 TYPES", "3 IMPACT", "4 DIFF"
  };
  GtkWidget *lens_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_valign(lens_box, GTK_ALIGN_CENTER);
  for (int i = 0; i < 4; i++) {
    GtkWidget *lb = ide_strip_view_make_toolbar_btn(kLensLabels[i], "ide-strip-lens-btn", NULL);
    gtk_widget_add_css_class(lb, ide_strip_view_lens_css_class((IdeStripLens)i));
    g_object_set_data(G_OBJECT(lb), "lens-id", GINT_TO_POINTER(i));
    g_signal_connect(lb, "clicked", G_CALLBACK(on_lens_btn_clicked), sv);
    sv->lens_btns[i] = lb;
    gtk_box_append(GTK_BOX(lens_box), lb);
  }
  gtk_box_append(GTK_BOX(sv->toolbar), lens_box);

  GtkWidget *sp_r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(sp_r, TRUE);
  gtk_box_append(GTK_BOX(sv->toolbar), sp_r);

  /* Right: WS ONLY badge, Engine badge, Zen, Overview, Tabs exit */
  GtkWidget *right_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(right_bar, GTK_ALIGN_CENTER);

  sv->ws_badge = gtk_label_new("WS ONLY (0 EXT)");
  gtk_widget_set_valign(sv->ws_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(sv->ws_badge, "shell-popover-badge");
  gtk_box_append(GTK_BOX(right_bar), sv->ws_badge);

  sv->engine_badge = gtk_label_new("⚠ TS+RG");
  gtk_widget_set_valign(sv->engine_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(sv->engine_badge, "shell-popover-badge");
  gtk_widget_add_css_class(sv->engine_badge, "ide-strip-engine-badge");
  gtk_box_append(GTK_BOX(right_bar), sv->engine_badge);

  sv->zen_btn = ide_strip_view_make_toolbar_btn("⤢ ZEN", "ide-strip-tool-btn", "Toggle Zen focus (Space t z)");
  g_signal_connect(sv->zen_btn, "clicked", G_CALLBACK(on_zen_clicked), sv);
  gtk_box_append(GTK_BOX(right_bar), sv->zen_btn);

  sv->overview_btn = ide_strip_view_make_toolbar_btn("⬡ MAP", "ide-strip-tool-btn", "Bird's-eye graph overview (Space t o)");
  g_signal_connect(sv->overview_btn, "clicked", G_CALLBACK(on_overview_clicked), sv);
  gtk_box_append(GTK_BOX(right_bar), sv->overview_btn);

  sv->exit_btn = ide_strip_view_make_toolbar_btn("✕ TABS", "ide-strip-exit-btn", "Return to classic tab editor (Space t s)");
  g_signal_connect(sv->exit_btn, "clicked", G_CALLBACK(on_exit_clicked), sv);
  gtk_box_append(GTK_BOX(right_bar), sv->exit_btn);

  gtk_box_append(GTK_BOX(sv->toolbar), right_bar);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tb_scroll), sv->toolbar);
  return tb_scroll;
}

static GtkWidget *
build_d0_column_ui(IdeStripView *sv)
{
  sv->d0_vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_shrink_start_child(GTK_PANED(sv->d0_vpaned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(sv->d0_vpaned), TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(sv->d0_vpaned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(sv->d0_vpaned), TRUE);
  gtk_widget_add_css_class(sv->d0_vpaned, "ide-strip-col-d0");

  /* Optional Top 30% Pinned Reference Box */
  sv->pin_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sv->pin_box, "ide-strip-pin-pane");
  gtk_widget_set_visible(sv->pin_box, FALSE);

  GtkWidget *pin_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(pin_hdr, "ide-strip-card-header");
  gtk_widget_set_valign(pin_hdr, GTK_ALIGN_CENTER);

  sv->pin_title_label = gtk_label_new("📌 Pinned");
  gtk_widget_set_valign(sv->pin_title_label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(sv->pin_title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(sv->pin_title_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(sv->pin_title_label, TRUE);
  gtk_box_append(GTK_BOX(pin_hdr), sv->pin_title_label);

  sv->pin_close_btn = ide_strip_view_make_toolbar_btn("×", "ide-strip-tile-btn", "Unpin reference");
  g_signal_connect(sv->pin_close_btn, "clicked", G_CALLBACK(on_pin_close_clicked), sv);
  gtk_box_append(GTK_BOX(pin_hdr), sv->pin_close_btn);
  gtk_box_append(GTK_BOX(sv->pin_box), pin_hdr);

  sv->pin_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sv->pin_scroll), FALSE);
  gtk_widget_set_vexpand(sv->pin_scroll, TRUE);
  sv->empty_pin_buf = gtk_source_buffer_new(NULL);
  sv->pin_source_view = gtk_source_view_new_with_buffer(sv->empty_pin_buf);
  gtk_widget_add_css_class(sv->pin_source_view, "ide-source-view");
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(sv->pin_source_view), TRUE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(sv->pin_source_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(sv->pin_source_view), FALSE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(sv->pin_source_view), 8);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(sv->pin_source_view), 4);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sv->pin_scroll), sv->pin_source_view);
  gtk_box_append(GTK_BOX(sv->pin_box), sv->pin_scroll);
  gtk_paned_set_start_child(GTK_PANED(sv->d0_vpaned), sv->pin_box);

  /* D0 Main Editor Card */
  sv->d0_main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(sv->d0_main_box, TRUE);
  gtk_widget_set_vexpand(sv->d0_main_box, TRUE);

  sv->d0_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(sv->d0_header, "ide-strip-card-header");
  gtk_widget_set_size_request(sv->d0_header, -1, 26);

  sv->d0_kind_badge = gtk_label_new("ƒ");
  gtk_widget_set_valign(sv->d0_kind_badge, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(sv->d0_kind_badge, "ide-strip-kind-badge");
  gtk_box_append(GTK_BOX(sv->d0_header), sv->d0_kind_badge);

  sv->d0_title_label = gtk_label_new("No active buffer");
  gtk_widget_set_valign(sv->d0_title_label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(sv->d0_title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(sv->d0_title_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(sv->d0_title_label, "ide-strip-d0-title");
  gtk_box_append(GTK_BOX(sv->d0_header), sv->d0_title_label);

  sv->d0_loc_label = gtk_label_new("");
  gtk_widget_set_valign(sv->d0_loc_label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(sv->d0_loc_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(sv->d0_loc_label), PANGO_ELLIPSIZE_START);
  gtk_widget_set_hexpand(sv->d0_loc_label, TRUE);
  gtk_widget_add_css_class(sv->d0_loc_label, "ide-strip-tile-loc");
  gtk_box_append(GTK_BOX(sv->d0_header), sv->d0_loc_label);

  sv->d0_fullfile_btn = ide_strip_view_make_toolbar_btn(
      "⤢ FULL FILE", "ide-strip-tool-btn",
      "Toggle symbol focus veil / full file (Space t f)");
  g_signal_connect(sv->d0_fullfile_btn, "clicked", G_CALLBACK(on_fullfile_clicked), sv);
  gtk_box_append(GTK_BOX(sv->d0_header), sv->d0_fullfile_btn);
  gtk_box_append(GTK_BOX(sv->d0_main_box), sv->d0_header);

  sv->d0_editor_overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(sv->d0_editor_overlay, TRUE);
  gtk_widget_set_vexpand(sv->d0_editor_overlay, TRUE);

  sv->d0_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sv->d0_scroll), FALSE);
  gtk_widget_set_hexpand(sv->d0_scroll, TRUE);
  gtk_widget_set_vexpand(sv->d0_scroll, TRUE);
  GtkAdjustment *d0_vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d0_scroll));
  if (d0_vadj != NULL) {
    g_signal_connect(d0_vadj, "value-changed", G_CALLBACK(on_d0_vadj_changed), sv);
  }

  sv->empty_d0_buf = gtk_source_buffer_new(NULL);
  sv->d0_source_view = gtk_source_view_new_with_buffer(sv->empty_d0_buf);
  gtk_widget_add_css_class(sv->d0_source_view, "ide-source-view");
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(sv->d0_source_view), TRUE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(sv->d0_source_view), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(sv->d0_source_view), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(sv->d0_source_view), 6);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(sv->d0_source_view), TRUE);
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(sv->d0_source_view), 2);
  gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(sv->d0_source_view), TRUE);

  GtkSourceGutter *left_gutter = gtk_source_view_get_gutter(
      GTK_SOURCE_VIEW(sv->d0_source_view), GTK_TEXT_WINDOW_LEFT);
  sv->d0_gutter = KAJO_IDE_GUTTER_RENDERER(kajo_ide_gutter_renderer_new(NULL));
  kajo_ide_gutter_renderer_set_breakpoint_callback(sv->d0_gutter, on_gutter_bp, sv);
  kajo_ide_gutter_renderer_set_conditional_bp_callback(sv->d0_gutter, on_gutter_cond_bp, sv);
  gtk_source_gutter_insert(left_gutter, GTK_SOURCE_GUTTER_RENDERER(sv->d0_gutter), 0);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sv->d0_scroll), sv->d0_source_view);
  gtk_overlay_set_child(GTK_OVERLAY(sv->d0_editor_overlay), sv->d0_scroll);

  sv->d0_veil_overlay = gtk_drawing_area_new();
  gtk_widget_set_can_target(sv->d0_veil_overlay, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sv->d0_veil_overlay),
                                 ide_strip_canvas_draw_d0_veil, sv, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(sv->d0_editor_overlay), sv->d0_veil_overlay);

  gtk_box_append(GTK_BOX(sv->d0_main_box), sv->d0_editor_overlay);
  gtk_paned_set_end_child(GTK_PANED(sv->d0_vpaned), sv->d0_main_box);
  return sv->d0_vpaned;
}

static void
build_ui(IdeStripView *sv)
{
  sv->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sv->root_box, "ide-strip-root");
  gtk_widget_set_hexpand(sv->root_box, TRUE);
  gtk_widget_set_vexpand(sv->root_box, TRUE);
  g_signal_connect(sv->root_box, "destroy", G_CALLBACK(on_root_box_destroy), sv);

  /* 1. Top Strip Toolbar */
  gtk_box_append(GTK_BOX(sv->root_box), build_toolbar_ui(sv));

  /* 2. Main View Stack ("strip" vs "overview") */
  sv->view_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(sv->view_stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_hexpand(sv->view_stack, TRUE);
  gtk_widget_set_vexpand(sv->view_stack, TRUE);
  gtk_box_append(GTK_BOX(sv->root_box), sv->view_stack);

  /* Page "strip" */
  sv->strip_overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(sv->strip_overlay, TRUE);
  gtk_widget_set_vexpand(sv->strip_overlay, TRUE);

  sv->strip_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(sv->strip_hbox, TRUE);
  gtk_widget_set_vexpand(sv->strip_hbox, TRUE);

  /* Far-left 28px Ancestor Spine */
  sv->spine_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_add_css_class(sv->spine_box, "ide-strip-spine");
  gtk_widget_set_size_request(sv->spine_box, (int)IDE_STRIP_SPINE_WIDTH, -1);
  gtk_widget_set_visible(sv->spine_box, FALSE);
  gtk_box_append(GTK_BOX(sv->strip_hbox), sv->spine_box);

  /* Outer Paned: D0 (60%) | Right Paned (40% -> D+1 25% | D+2 15%) */
  sv->outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_shrink_start_child(GTK_PANED(sv->outer_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(sv->outer_paned), TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(sv->outer_paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(sv->outer_paned), TRUE);
  gtk_widget_set_hexpand(sv->outer_paned, TRUE);
  gtk_widget_set_vexpand(sv->outer_paned, TRUE);
  g_signal_connect(sv->outer_paned, "notify::position",
                   G_CALLBACK(on_outer_paned_pos_changed), sv);

  gtk_paned_set_start_child(GTK_PANED(sv->outer_paned), build_d0_column_ui(sv));

  sv->right_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_shrink_start_child(GTK_PANED(sv->right_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(sv->right_paned), TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(sv->right_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(sv->right_paned), TRUE);
  g_signal_connect(sv->right_paned, "notify::position",
                   G_CALLBACK(on_right_paned_pos_changed), sv);

  gtk_paned_set_start_child(GTK_PANED(sv->right_paned), ide_strip_tiles_build_d1_column(sv));
  gtk_paned_set_end_child(GTK_PANED(sv->right_paned), ide_strip_tiles_build_d2_column(sv));
  gtk_paned_set_end_child(GTK_PANED(sv->outer_paned), sv->right_paned);
  gtk_box_append(GTK_BOX(sv->strip_hbox), sv->outer_paned);

  /* Zen Right Edge Rail (4%, visible only in Zen mode) */
  sv->zen_rail_btn = gtk_button_new();
  gtk_widget_set_focusable(sv->zen_rail_btn, FALSE);
  gtk_widget_add_css_class(sv->zen_rail_btn, "ide-strip-zen-rail");
  gtk_widget_set_visible(sv->zen_rail_btn, FALSE);
  gtk_widget_set_tooltip_text(sv->zen_rail_btn, "Exit Zen mode (Space t z)");
  sv->zen_rail_label = gtk_label_new("D+1: 0\nD+2: 0\n[ ⤡ ]");
  gtk_label_set_justify(GTK_LABEL(sv->zen_rail_label), GTK_JUSTIFY_CENTER);
  gtk_button_set_child(GTK_BUTTON(sv->zen_rail_btn), sv->zen_rail_label);
  g_signal_connect(sv->zen_rail_btn, "clicked", G_CALLBACK(on_zen_clicked), sv);
  gtk_box_append(GTK_BOX(sv->strip_hbox), sv->zen_rail_btn);

  gtk_overlay_set_child(GTK_OVERLAY(sv->strip_overlay), sv->strip_hbox);

  /* Cairo Bridge Overlay */
  sv->bridge_overlay = gtk_drawing_area_new();
  gtk_widget_set_can_target(sv->bridge_overlay, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sv->bridge_overlay),
                                 ide_strip_canvas_draw_bridges, sv, NULL);
  g_signal_connect(sv->bridge_overlay, "resize",
                   G_CALLBACK(ide_strip_canvas_on_bridge_resize), sv);
  gtk_overlay_add_overlay(GTK_OVERLAY(sv->strip_overlay), sv->bridge_overlay);

  gtk_stack_add_named(GTK_STACK(sv->view_stack), sv->strip_overlay, "strip");
  gtk_stack_add_named(GTK_STACK(sv->view_stack),
                      ide_strip_canvas_build_overview_page(sv), "overview");
  gtk_stack_set_visible_child_name(GTK_STACK(sv->view_stack), "strip");
}

/* ═══════════════════════════ Public Lifecycle API ═══════════════════════════ */

IdeStripView *
ide_strip_view_new(const char           *workspace_root,
                   IdeLspManager        *lsp,
                   IdeStripOpenBufferCb  open_buf_cb,
                   IdeStripGetBuffersCb  get_bufs_cb,
                   IdeStripStatusCb      status_cb,
                   IdeStripModeExitCb    exit_cb,
                   gpointer              user_data)
{
  IdeStripView *sv = g_new0(IdeStripView, 1);
  sv->model = ide_strip_model_new();
  sv->graph = ide_strip_graph_new(workspace_root);
  sv->lsp = lsp;
  sv->open_buf_cb = open_buf_cb;
  sv->get_bufs_cb = get_bufs_cb;
  sv->status_cb = status_cb;
  sv->exit_cb = exit_cb;
  sv->user_data = user_data;
  sv->alive = g_cancellable_new();

  sv->font_family = g_strdup("Fira Code");
  sv->font_size_px = 13;
  sv->tab_width = 2;
  sv->insert_spaces = TRUE;
  sv->highlight_current_line = TRUE;
  sv->show_line_numbers = TRUE;
  sv->magnetic_follow = TRUE;

  sv->d1_rows = g_ptr_array_new_with_free_func(g_free);
  sv->overview_hitboxes = g_array_new(FALSE, FALSE, sizeof(OverviewHitbox));

  build_ui(sv);
  ide_strip_view_rebuild_toolbar(sv);
  return sv;
}

void
ide_strip_view_free(IdeStripView *sv)
{
  if (sv == NULL) {
    return;
  }
  if (sv->alive != NULL) {
    g_cancellable_cancel(sv->alive);
    g_clear_object(&sv->alive);
  }
  if (sv->debounce_id != 0) {
    g_source_remove(sv->debounce_id);
    sv->debounce_id = 0;
  }
  if (sv->paned_idle_id != 0) {
    g_source_remove(sv->paned_idle_id);
    sv->paned_idle_id = 0;
  }
  if (sv->tick_id != 0 && sv->root_box != NULL) {
    gtk_widget_remove_tick_callback(sv->root_box, sv->tick_id);
    sv->tick_id = 0;
  }
  detach_d0_buffer_signals(sv);
  g_clear_object(&sv->empty_d0_buf);
  g_clear_object(&sv->empty_pin_buf);
  g_clear_pointer(&sv->d1_rows, g_ptr_array_unref);
  g_clear_pointer(&sv->overview_hitboxes, g_array_unref);
  g_clear_pointer(&sv->graph, ide_strip_graph_free);
  g_clear_pointer(&sv->model, ide_strip_model_free);
  g_free(sv->font_family);
  g_free(sv);
}

GtkWidget *
ide_strip_view_get_widget(IdeStripView *sv)
{
  return sv != NULL ? sv->root_box : NULL;
}

GtkWidget *
ide_strip_view_get_d0_source_view(IdeStripView *sv)
{
  return sv != NULL ? sv->d0_source_view : NULL;
}

GtkWidget *
ide_strip_view_get_filter_entry(IdeStripView *sv)
{
  return sv != NULL ? sv->d1_filter_entry : NULL;
}

GtkWidget *
ide_strip_view_get_overview_entry(IdeStripView *sv)
{
  return sv != NULL ? sv->overview_entry : NULL;
}

/* ═══════════════════════════ Configuration & Buffer Hooks ═══════════════════════════ */

void
ide_strip_view_set_gutter_callbacks(IdeStripView    *sv,
                                    IdeStripBpCb     bp_cb,
                                    IdeStripCondBpCb cond_bp_cb,
                                    gpointer         user_data)
{
  if (sv == NULL) {
    return;
  }
  sv->bp_cb = bp_cb;
  sv->cond_bp_cb = cond_bp_cb;
  sv->gutter_user_data = user_data;
}

static void
apply_settings_to_source_view(GtkWidget    *view,
                              IdeStripView *sv,
                              gboolean      is_d0)
{
  if (view == NULL || sv == NULL) {
    return;
  }
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(view), (guint)CLAMP(sv->tab_width, 2, 8));
  gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(view), sv->insert_spaces);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),
                              sv->word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
  gtk_source_view_set_show_right_margin(GTK_SOURCE_VIEW(view), is_d0 && sv->show_margin_guide);
  gtk_source_view_set_right_margin_position(GTK_SOURCE_VIEW(view), 100);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(view),
                                             is_d0 && sv->highlight_current_line);
}

void
ide_strip_view_apply_editor_settings(IdeStripView *sv,
                                     const char   *font_family,
                                     int           font_size_px,
                                     int           tab_width,
                                     gboolean      insert_spaces,
                                     gboolean      word_wrap,
                                     gboolean      show_margin_guide,
                                     gboolean      highlight_current_line,
                                     gboolean      show_line_numbers,
                                     gboolean      relative_line_numbers,
                                     gboolean      magnetic_follow)
{
  if (sv == NULL) {
    return;
  }
  char *copy = g_strdup((font_family != NULL && font_family[0] != '\0') ? font_family : "Fira Code");
  g_free(sv->font_family);
  sv->font_family = copy;

  sv->font_size_px = CLAMP(font_size_px, 9, 28);
  sv->tab_width = CLAMP(tab_width, 2, 8);
  sv->insert_spaces = insert_spaces;
  sv->word_wrap = word_wrap;
  sv->show_margin_guide = show_margin_guide;
  sv->highlight_current_line = highlight_current_line;
  sv->show_line_numbers = show_line_numbers;
  sv->relative_line_numbers = relative_line_numbers;
  sv->magnetic_follow = magnetic_follow;

  apply_settings_to_source_view(sv->d0_source_view, sv, TRUE);
  apply_settings_to_source_view(sv->pin_source_view, sv, FALSE);

  if (sv->d0_gutter != NULL) {
    int gutter_pt = MAX(8, (sv->font_size_px - 2) * 3 / 4);
    g_autofree char *font_stack = shell_ide_font_stack(sv->font_family);
    kajo_ide_gutter_renderer_set_font(sv->d0_gutter, font_stack, gutter_pt);
    kajo_ide_gutter_renderer_set_relative_numbers(sv->d0_gutter, sv->relative_line_numbers);
    gtk_widget_set_visible(GTK_WIDGET(sv->d0_gutter), sv->show_line_numbers);
  }
}

void
ide_strip_view_set_workspace_root(IdeStripView *sv, const char *workspace_root)
{
  if (sv == NULL) {
    return;
  }
  ide_strip_graph_set_workspace_root(sv->graph, workspace_root);
  if (sv->model->strip_active && sv->d0_buf != NULL) {
    ide_strip_tiles_refresh_d1(sv);
  }
}

void
ide_strip_view_set_active(IdeStripView *sv, gboolean active)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  sv->model->strip_active = active;
  if (active) {
    sv->user_dragged_outer_paned = FALSE;
    sv->user_dragged_right_paned = FALSE;
    ide_strip_view_schedule_paned_layout(sv);
    if (sv->d0_buf != NULL) {
      ide_strip_tiles_refresh_d1(sv);
    }
    ide_strip_view_rebuild_all_ui(sv);
  } else {
    if (sv->debounce_id != 0) {
      g_source_remove(sv->debounce_id);
      sv->debounce_id = 0;
    }
    if (sv->tick_id != 0 && sv->root_box != NULL) {
      gtk_widget_remove_tick_callback(sv->root_box, sv->tick_id);
      sv->tick_id = 0;
    }
  }
}

gboolean
ide_strip_view_is_active(const IdeStripView *sv)
{
  return sv != NULL && sv->model != NULL && sv->model->strip_active;
}

void
ide_strip_view_bind_buffer(IdeStripView *sv,
                           IdeBuffer    *buf,
                           guint         cursor_line_1based,
                           gboolean      clear_ancestors)
{
  if (sv == NULL || sv->model == NULL || sv->binding_internal) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  attach_d0_buffer_internal(sv, buf);
  if (buf == NULL) {
    ide_strip_trail_set_root(trail, NULL, clear_ancestors);
    ide_strip_trail_set_d1_tiles(trail, NULL, 0, FALSE);
    ide_strip_trail_set_d2_tiles(trail, NULL);
    ide_strip_view_rebuild_all_ui(sv);
    return;
  }

  const char *path = ide_buffer_get_file_path(buf);
  g_autofree char *text = ide_buffer_get_text(buf);
  g_autofree char *rel = compute_rel_path(sv, path);
  guint line = cursor_line_1based > 0 ? cursor_line_1based : ide_strip_view_get_d0_cursor_line(sv);

  g_autofree char *sym_name = NULL;
  guint s_line = 1, e_line = 1;
  int kind = IDE_STRIP_SYM_FILE;
  if (!ide_strip_graph_find_enclosing_symbol(sv->graph, path, text, line,
                                             &sym_name, &s_line, &e_line, &kind)) {
    sym_name = g_strdup(ide_buffer_get_title(buf));
    s_line = 1;
    e_line = sv->d0_tbuf != NULL ? (guint)MAX(1, gtk_text_buffer_get_line_count(sv->d0_tbuf)) : 1;
    kind = IDE_STRIP_SYM_FILE;
  }

  IdeStripTile *root = ide_strip_tile_new(NULL,
                                          sym_name,
                                          path,
                                          rel,
                                          s_line,
                                          e_line,
                                          line,
                                          (IdeStripTileKind)CLAMP(kind, 0, 3));
  ide_strip_tiles_enrich_tile(sv, root);
  ide_strip_trail_set_root(trail, root, clear_ancestors);

  if (cursor_line_1based > 0) {
    ide_strip_view_scroll_source_to_line(sv->d0_source_view, cursor_line_1based, FALSE, 0.25);
  }

  ide_strip_tiles_rebuild_spine(sv);
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  if (sv->model->strip_active) {
    ide_strip_tiles_refresh_d1(sv);
  }
}

void
ide_strip_view_notify_buffer_saved(IdeStripView *sv, IdeBuffer *buf)
{
  if (sv == NULL || buf == NULL) {
    return;
  }
  const char *path = ide_buffer_get_file_path(buf);
  if (path != NULL) {
    ide_strip_graph_invalidate_file(sv->graph, path);
  }
  if (sv->model->strip_active && buf == sv->d0_buf) {
    ide_strip_tiles_refresh_d1(sv);
  }
}

void
ide_strip_view_notify_buffer_closed(IdeStripView *sv, IdeBuffer *buf)
{
  if (sv == NULL || buf == NULL) {
    return;
  }
  if (sv->d0_buf == buf) {
    attach_d0_buffer_internal(sv, NULL);
  }
  if (sv->pin_buf == buf) {
    sv->pin_buf = NULL;
    if (sv->pin_source_view != NULL && sv->empty_pin_buf != NULL) {
      gtk_text_view_set_buffer(GTK_TEXT_VIEW(sv->pin_source_view),
                               GTK_TEXT_BUFFER(sv->empty_pin_buf));
    }
  }
}

void
ide_strip_view_notify_buffer_changed(IdeStripView *sv, IdeBuffer *buf)
{
  if (sv == NULL || buf == NULL || !sv->model->strip_active) {
    return;
  }
  if (buf == sv->d0_buf) {
    if (sv->d0_veil_overlay != NULL) {
      gtk_widget_queue_draw(sv->d0_veil_overlay);
    }
    ide_strip_tiles_schedule_viewport_band(sv);
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail != NULL && trail->lens == IDE_STRIP_LENS_DIFF) {
    ide_strip_tiles_refresh_d1(sv);
  }
}

/* ═══════════════════════════ Modes & Status API ═══════════════════════════ */

void
ide_strip_view_toggle_zen(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  trail->zen_mode = !trail->zen_mode;
  if (!trail->zen_mode) {
    sv->user_dragged_outer_paned = FALSE;
    sv->user_dragged_right_paned = FALSE;
  }
  ide_strip_view_rebuild_toolbar(sv);
  ide_strip_tiles_rebuild_spine(sv);
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  ide_strip_view_emit_status(sv, trail->zen_mode ? "Code Strip: Zen mode" : "Code Strip: 3-column mode");
}

void
ide_strip_view_toggle_full_file(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  trail->show_full_file = !trail->show_full_file;
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  ide_strip_tiles_refresh_d1(sv);
  ide_strip_view_emit_status(sv, trail->show_full_file ? "D0: Full file scope" : "D0: Symbol focus veil");
}

char *
ide_strip_view_get_status_summary(const IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return g_strdup("STRIP");
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return g_strdup("STRIP");
  }
  guint depth = trail->ancestors != NULL ? trail->ancestors->len : 0;
  guint n1 = trail->d1_tiles != NULL ? trail->d1_tiles->len : 0;
  guint n2 = trail->d2_tiles != NULL ? trail->d2_tiles->len : 0;
  return g_strdup_printf("STRIP %s · D%u · D+1:%u · D+2:%u%s%s",
                         ide_strip_lens_name(trail->lens),
                         depth,
                         n1,
                         n2,
                         trail->zen_mode ? " · ZEN" : "",
                         trail->using_fallback ? " · FALLBACK" : " · LSP");
}
