#include "ide_strip_view_private.h"

/* ═══════════════════════════ Tile Enrichment & Conversion ═══════════════════════════ */

static void
clear_listbox_children(GtkWidget *listbox)
{
  if (listbox == NULL) {
    return;
  }
  GtkWidget *child = gtk_widget_get_first_child(listbox);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(listbox), child);
    child = next;
  }
}

void
ide_strip_tiles_enrich_tile(const IdeStripView *sv, IdeStripTile *tile)
{
  if (sv == NULL || tile == NULL || tile->file_path == NULL) {
    return;
  }
  IdeBuffer *b = ide_strip_view_find_open_buffer(sv, tile->file_path);
  if (b == NULL) {
    return;
  }
  if (ide_buffer_get_modified(b)) {
    tile->is_dirty = TRUE;
  }
  if (ide_buffer_has_agent_changes(b)) {
    tile->is_agent_modified = TRUE;
  }
  const GPtrArray *diags = ide_buffer_get_diagnostics(b);
  if (diags != NULL) {
    for (guint i = 0; i < diags->len; i++) {
      const IdeDiagnostic *d = g_ptr_array_index(diags, i);
      if (d == NULL) {
        continue;
      }
      if (d->line >= tile->start_line && d->line <= tile->end_line) {
        if (d->severity == IDE_DIAG_ERROR) {
          tile->diag_errors++;
        } else if (d->severity == IDE_DIAG_WARNING) {
          tile->diag_warnings++;
        }
      }
    }
  }
}

static GPtrArray *
graph_entries_to_tiles(const IdeStripView *sv, const GPtrArray *entries)
{
  GPtrArray *tiles = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  if (entries == NULL) {
    return tiles;
  }
  for (guint i = 0; i < entries->len; i++) {
    const IdeStripGraphEntry *e = g_ptr_array_index(entries, i);
    if (e == NULL) {
      continue;
    }
    IdeStripTileKind kind = (IdeStripTileKind)CLAMP(e->kind, 0, 3);
    IdeStripTile *tile = ide_strip_tile_new(NULL,
                                            e->name,
                                            e->file_path,
                                            e->rel_path,
                                            e->start_line,
                                            e->end_line,
                                            e->call_line,
                                            kind);
    tile->call_count = MAX(1u, e->call_count);
    tile->child_count = e->child_count;
    tile->peek_code = g_strdup(e->peek_code);
    tile->is_dirty = e->is_dirty;
    tile->is_agent_modified = e->is_agent_modified;
    tile->is_fallback = e->is_fallback;
    ide_strip_tiles_enrich_tile(sv, tile);
    g_ptr_array_add(tiles, tile);
  }
  return tiles;
}

/* ═══════════════════════════ Viewport Band & Magnetic Follow ═══════════════════════════ */

void
ide_strip_tiles_run_viewport_band(IdeStripView *sv, gboolean force_resync)
{
  if (sv == NULL || sv->model == NULL || sv->d0_source_view == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL || trail->d1_tiles->len == 0) {
    return;
  }

  GdkRectangle vis_rect = { 0, 0, 0, 0 };
  gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(sv->d0_source_view), &vis_rect);

  GtkTextIter top_it, bot_it;
  gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(sv->d0_source_view), &top_it, vis_rect.y, NULL);
  gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(sv->d0_source_view),
                              &bot_it,
                              vis_rect.y + MAX(1, vis_rect.height),
                              NULL);
  guint vis_top = (guint)gtk_text_iter_get_line(&top_it) + 1;
  guint vis_bot = (guint)gtk_text_iter_get_line(&bot_it) + 1;
  guint cur_line = ide_strip_view_get_d0_cursor_line(sv);

  double cursor_rel_y = (double)vis_rect.height * 0.25;
  if (sv->d0_tbuf != NULL) {
    GtkTextIter cur_it;
    gtk_text_buffer_get_iter_at_mark(sv->d0_tbuf, &cur_it,
                                     gtk_text_buffer_get_insert(sv->d0_tbuf));
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(sv->d0_source_view), &cur_it, &y, &h);
    cursor_rel_y = (double)(y - vis_rect.y);
    cursor_rel_y = CLAMP(cursor_rel_y, 0.0, (double)MAX(1, vis_rect.height));
  }

  double d1_vh = (sv->d1_scroll != NULL) ? (double)gtk_widget_get_height(sv->d1_scroll) : 400.0;
  if (d1_vh <= 1.0) {
    d1_vh = 400.0;
  }

  gboolean active_changed = FALSE;
  if (force_resync) {
    active_changed = ide_strip_trail_resync_d1(trail, vis_top, vis_bot, cur_line,
                                               cursor_rel_y, d1_vh, ide_strip_view_now_sec());
  } else {
    active_changed = ide_strip_trail_update_viewport_band(trail, vis_top, vis_bot, cur_line,
                                                          cursor_rel_y, d1_vh,
                                                          sv->magnetic_follow, ide_strip_view_now_sec());
  }

  ide_strip_tiles_sync_d1_active(sv);
  if (active_changed) {
    ide_strip_tiles_refresh_d2(sv);
  }
  if (sv->d1_resync_btn != NULL) {
    gtk_widget_set_visible(sv->d1_resync_btn, trail->d1_unlinked);
  }
  ide_strip_view_ensure_spring_tick(sv);
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

static gboolean
on_viewport_debounce_timeout(gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return G_SOURCE_REMOVE;
  }
  sv->debounce_id = 0;
  if (!sv->model->strip_active) {
    return G_SOURCE_REMOVE;
  }
  ide_strip_tiles_run_viewport_band(sv, FALSE);
  return G_SOURCE_REMOVE;
}

void
ide_strip_tiles_schedule_viewport_band(IdeStripView *sv)
{
  if (sv == NULL || !sv->model->strip_active) {
    return;
  }
  if (sv->debounce_id != 0) {
    g_source_remove(sv->debounce_id);
  }
  sv->debounce_id = g_timeout_add(STRIP_DEBOUNCE_MS, on_viewport_debounce_timeout, sv);
}

/* ═══════════════════════════ Async LSP Enrichment & Target Extraction ═══════════════════════════ */

static void
on_lsp_targets_ready(GPtrArray *entries,
                     guint      ext_suppressed,
                     gboolean   is_fallback,
                     gpointer   user_data)
{
  StripLspCtx *ctx = user_data;
  if (ctx == NULL) {
    return;
  }
  if (g_cancellable_is_cancelled(ctx->alive)) {
    g_object_unref(ctx->alive);
    g_free(ctx);
    return;
  }

  IdeStripView *sv = ctx->sv;
  guint64 gen = ctx->gen;
  IdeStripLens lens = ctx->lens;
  g_object_unref(ctx->alive);
  g_free(ctx);

  if (sv == NULL || sv->query_gen != gen) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->lens != lens) {
    return;
  }

  /* Only replace immediate tree-sitter targets if LSP succeeded and returned data,
   * or succeeded cleanly with non-fallback status. */
  if (!is_fallback && entries != NULL && entries->len > 0) {
    GPtrArray *tiles = graph_entries_to_tiles(sv, entries);
    ide_strip_trail_set_d1_tiles(trail, tiles, ext_suppressed, FALSE);
    ide_strip_view_rebuild_toolbar(sv);
    ide_strip_tiles_rebuild_d1(sv);
    ide_strip_tiles_run_viewport_band(sv, FALSE);
    ide_strip_tiles_refresh_d2(sv);
  } else if (!is_fallback) {
    /* LSP answered cleanly with 0 targets; keep fallback indicator accurate */
    trail->using_fallback = FALSE;
    ide_strip_view_rebuild_toolbar(sv);
  }
}

void
ide_strip_tiles_refresh_d1(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL || sv->graph == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  sv->query_gen++;
  guint ext_suppressed = 0;
  gboolean lsp_ready = (sv->lsp != NULL && sv->d0_buf != NULL &&
                        ide_lsp_manager_is_ready(sv->lsp, sv->d0_buf));
  gboolean using_fallback = !lsp_ready;

  GPtrArray *entries = NULL;
  if (trail->lens == IDE_STRIP_LENS_DIFF) {
    GPtrArray *open_bufs = sv->get_bufs_cb != NULL ? sv->get_bufs_cb(sv->user_data) : NULL;
    entries = ide_strip_graph_extract_diff_targets(sv->graph, open_bufs);
    using_fallback = FALSE;
  } else if (trail->root_tile != NULL && trail->root_tile->file_path != NULL) {
    g_autofree char *text = NULL;
    if (sv->d0_buf != NULL) {
      text = ide_buffer_get_text(sv->d0_buf);
    }
    guint s_line = trail->show_full_file ? 1u : trail->root_tile->start_line;
    guint e_line = trail->show_full_file ? G_MAXUINT : trail->root_tile->end_line;
    entries = ide_strip_graph_extract_targets(sv->graph,
                                              trail->root_tile->file_path,
                                              text,
                                              s_line,
                                              e_line,
                                              (int)trail->lens,
                                              &ext_suppressed);
  }

  GPtrArray *tiles = graph_entries_to_tiles(sv, entries);
  g_clear_pointer(&entries, g_ptr_array_unref);

  ide_strip_trail_set_d1_tiles(trail, tiles, ext_suppressed, using_fallback);
  ide_strip_view_rebuild_toolbar(sv);
  ide_strip_tiles_rebuild_d1(sv);
  ide_strip_tiles_run_viewport_band(sv, FALSE);
  ide_strip_tiles_refresh_d2(sv);

  /* Async LSP enrichment for CALLS / IMPACT when a ready server handles D0 */
  if (lsp_ready &&
      (trail->lens == IDE_STRIP_LENS_CALLS || trail->lens == IDE_STRIP_LENS_IMPACT) &&
      trail->root_tile != NULL) {
    guint q_line = trail->root_tile->start_line > 0
                     ? trail->root_tile->start_line
                     : ide_strip_view_get_d0_cursor_line(sv);
    guint q_col = 1;
    if (trail->lens == IDE_STRIP_LENS_IMPACT) {
      q_line = ide_strip_view_get_d0_cursor_line(sv);
      q_col = ide_strip_view_get_d0_cursor_col(sv);
    }
    StripLspCtx *ctx = g_new0(StripLspCtx, 1);
    ctx->sv = sv;
    ctx->alive = g_object_ref(sv->alive);
    ctx->gen = sv->query_gen;
    ctx->lens = trail->lens;

    if (!ide_strip_graph_query_lsp_async(sv->graph,
                                         sv->lsp,
                                         sv->d0_buf,
                                         q_line,
                                         q_col,
                                         (int)trail->lens,
                                         on_lsp_targets_ready,
                                         ctx)) {
      g_object_unref(ctx->alive);
      g_free(ctx);
    }
  }
}

void
ide_strip_tiles_refresh_d2(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL || sv->graph == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  if (trail->d1_tiles == NULL ||
      trail->active_d1_idx < 0 ||
      (guint)trail->active_d1_idx >= trail->d1_tiles->len) {
    ide_strip_trail_set_d2_tiles(trail, NULL);
    ide_strip_tiles_rebuild_d2(sv);
    return;
  }

  const IdeStripTile *active_d1 = g_ptr_array_index(trail->d1_tiles, (guint)trail->active_d1_idx);
  if (active_d1 == NULL || active_d1->file_path == NULL) {
    ide_strip_trail_set_d2_tiles(trail, NULL);
    ide_strip_tiles_rebuild_d2(sv);
    return;
  }

  g_autofree char *override_text = NULL;
  IdeBuffer *open_b = ide_strip_view_find_open_buffer(sv, active_d1->file_path);
  if (open_b != NULL) {
    override_text = ide_buffer_get_text(open_b);
  }

  int d2_lens = (trail->lens == IDE_STRIP_LENS_DIFF)
                  ? IDE_STRIP_GRAPH_LENS_CALLS
                  : (int)trail->lens;
  guint d2_ext = 0;
  GPtrArray *entries = ide_strip_graph_extract_targets(sv->graph,
                                                       active_d1->file_path,
                                                       override_text,
                                                       active_d1->start_line,
                                                       active_d1->end_line,
                                                       d2_lens,
                                                       &d2_ext);
  GPtrArray *tiles = graph_entries_to_tiles(sv, entries);
  g_clear_pointer(&entries, g_ptr_array_unref);

  ide_strip_trail_set_d2_tiles(trail, tiles);
  ide_strip_tiles_rebuild_d2(sv);
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  if (sv->overview_canvas != NULL && trail->overview_mode) {
    gtk_widget_queue_draw(sv->overview_canvas);
  }
}

/* ═══════════════════════════ Ancestor Spine ═══════════════════════════ */

static void
on_ancestor_btn_clicked(GtkButton *btn, gpointer user_data)
{
  IdeStripView *sv = user_data;
  guint anc_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "anc-idx"));
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->ancestors == NULL || anc_idx >= trail->ancestors->len) {
    return;
  }

  const IdeStripAncestor *anc = g_ptr_array_index(trail->ancestors, anc_idx);
  guint restore_line = anc != NULL ? anc->cursor_line : 1;
  gint saved_d1 = anc != NULL ? anc->active_d1_idx : -1;
  double saved_scroll = anc != NULL ? anc->d1_scroll_y : 0.0;

  if (!ide_strip_trail_surface_to(trail, anc_idx, ide_strip_view_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    ide_strip_view_open_and_attach_d0(sv, trail->root_tile->file_path, restore_line, TRUE);
  }
  ide_strip_tiles_refresh_d1(sv);
  if (saved_d1 >= 0 && trail->d1_tiles != NULL && (guint)saved_d1 < trail->d1_tiles->len) {
    trail->active_d1_idx = saved_d1;
    trail->d1_scroll_y = saved_scroll;
    if (sv->d1_scroll != NULL) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d1_scroll));
      if (vadj != NULL) {
        sv->programmatic_d1_scroll = TRUE;
        gtk_adjustment_set_value(vadj, MAX(0.0, saved_scroll));
        sv->programmatic_d1_scroll = FALSE;
      }
    }
    ide_strip_tiles_sync_d1_active(sv);
    ide_strip_tiles_refresh_d2(sv);
  }
  ide_strip_view_rebuild_all_ui(sv);
  ide_strip_view_ensure_spring_tick(sv);
}

void
ide_strip_tiles_rebuild_spine(IdeStripView *sv)
{
  if (sv == NULL || sv->spine_box == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  ide_strip_view_clear_box(sv->spine_box);

  gboolean show_spine = (trail != NULL &&
                         !trail->zen_mode &&
                         trail->ancestors != NULL &&
                         trail->ancestors->len > 0);
  gtk_widget_set_visible(sv->spine_box, show_spine);
  if (!show_spine) {
    return;
  }

  for (guint i = 0; i < trail->ancestors->len; i++) {
    const IdeStripAncestor *anc = g_ptr_array_index(trail->ancestors, i);
    const IdeStripTile *t = anc != NULL ? anc->tile : NULL;
    if (t == NULL) {
      continue;
    }

    g_autofree char *short_lbl = g_strdup_printf("D%u", i);
    GtkWidget *btn = gtk_button_new_with_label(short_lbl);
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(btn, "ide-strip-spine-item");
    g_autofree char *tip = g_strdup_printf("%s · %s:%u",
                                           t->name != NULL ? t->name : "(file)",
                                           t->rel_path != NULL ? t->rel_path : "",
                                           t->start_line);
    gtk_widget_set_tooltip_text(btn, tip);
    g_object_set_data(G_OBJECT(btn), "anc-idx", GUINT_TO_POINTER(i));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_ancestor_btn_clicked), sv);
    gtk_box_append(GTK_BOX(sv->spine_box), btn);
  }
}

/* ═══════════════════════════ D+1 Peekish & D+2 Micro-Tile Builders ═══════════════════════════ */

static void
on_d1_tile_pin_clicked(GtkButton *btn, gpointer user_data)
{
  IdeStripView *sv = user_data;
  guint tile_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "tile-idx"));
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL || tile_idx >= trail->d1_tiles->len) {
    return;
  }
  trail->active_d1_idx = (gint)tile_idx;
  ide_strip_view_pin_active_d1(sv);
}

static void
on_d1_tile_promote_clicked(GtkButton *btn, gpointer user_data)
{
  IdeStripView *sv = user_data;
  guint tile_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "tile-idx"));
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL || tile_idx >= trail->d1_tiles->len) {
    return;
  }
  trail->active_d1_idx = (gint)tile_idx;
  ide_strip_view_promote_active_d1(sv);
}

void
ide_strip_tiles_sync_d1_active(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL || sv->d1_rows == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL) {
    return;
  }

  GtkListBoxRow *active_lb_row = NULL;
  for (guint i = 0; i < sv->d1_rows->len; i++) {
    D1RowEntry *re = g_ptr_array_index(sv->d1_rows, i);
    if (re == NULL || re->row == NULL || re->tile_idx >= trail->d1_tiles->len) {
      continue;
    }
    const IdeStripTile *tile = g_ptr_array_index(trail->d1_tiles, re->tile_idx);
    gboolean is_active = ((gint)re->tile_idx == trail->active_d1_idx);

    if (tile->in_viewport_band) {
      gtk_widget_add_css_class(re->row, "in-band");
      gtk_widget_remove_css_class(re->row, "out-of-band");
    } else {
      gtk_widget_add_css_class(re->row, "out-of-band");
      gtk_widget_remove_css_class(re->row, "in-band");
    }

    if (is_active) {
      gtk_widget_add_css_class(re->row, "active-peek");
      active_lb_row = GTK_LIST_BOX_ROW(re->row);
    } else {
      gtk_widget_remove_css_class(re->row, "active-peek");
    }

    if (re->peek_label != NULL) {
      gboolean show_peek = (is_active && tile->peek_code != NULL && tile->peek_code[0] != '\0');
      gtk_widget_set_visible(re->peek_label, show_peek);
    }
  }

  sv->rebuilding_d1 = TRUE;
  gtk_list_box_select_row(GTK_LIST_BOX(sv->d1_list_box), active_lb_row);
  sv->rebuilding_d1 = FALSE;
}

void
ide_strip_tiles_rebuild_d1(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL || sv->d1_list_box == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  sv->rebuilding_d1 = TRUE;
  clear_listbox_children(sv->d1_list_box);
  g_ptr_array_set_size(sv->d1_rows, 0);

  g_autoptr(GArray) filtered = ide_strip_trail_filtered_d1_indices(trail);
  guint total = trail->d1_tiles != NULL ? trail->d1_tiles->len : 0;

  if (sv->d1_header_label != NULL) {
    g_autofree char *hdr = (filtered->len == total)
                             ? g_strdup_printf("D+1 TARGETS (%u)", total)
                             : g_strdup_printf("D+1 TARGETS (%u/%u)", filtered->len, total);
    gtk_label_set_text(GTK_LABEL(sv->d1_header_label), hdr);
  }
  if (sv->d1_resync_btn != NULL) {
    gtk_widget_set_visible(sv->d1_resync_btn, trail->d1_unlinked);
  }

  /* Update lens color class on D+1 column for border accents */
  for (int l = 0; l < 4; l++) {
    gtk_widget_remove_css_class(sv->d1_col, ide_strip_view_lens_css_class((IdeStripLens)l));
  }
  gtk_widget_add_css_class(sv->d1_col, ide_strip_view_lens_css_class(trail->lens));

  guint cap = MIN(filtered->len, STRIP_D1_MAX_ROWS);
  for (guint k = 0; k < cap; k++) {
    guint tile_idx = g_array_index(filtered, guint, k);
    const IdeStripTile *tile = g_ptr_array_index(trail->d1_tiles, tile_idx);
    if (tile == NULL) {
      continue;
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "ide-strip-d1-row");
    g_object_set_data(G_OBJECT(row), "tile-idx", GUINT_TO_POINTER(tile_idx));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(vbox, TRUE);

    /* Tile header row (valign = CENTER on hbox and every child per AGENTS.md §4A) */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(hbox, TRUE);

    GtkWidget *kind_lbl = gtk_label_new(ide_strip_tile_kind_badge(tile->kind));
    gtk_widget_set_valign(kind_lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(kind_lbl, "ide-strip-kind-badge");
    gtk_box_append(GTK_BOX(hbox), kind_lbl);

    GtkWidget *name_lbl = gtk_label_new(tile->name != NULL ? tile->name : "(unnamed)");
    gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name_lbl, TRUE);
    gtk_widget_add_css_class(name_lbl, "ide-strip-tile-name");
    gtk_box_append(GTK_BOX(hbox), name_lbl);

    g_autofree char *loc_txt = g_strdup_printf("%s:%u",
                                               tile->rel_path != NULL ? tile->rel_path : "",
                                               tile->start_line);
    GtkWidget *loc_lbl = gtk_label_new(loc_txt);
    gtk_widget_set_valign(loc_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(loc_lbl), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars(GTK_LABEL(loc_lbl), 22);
    gtk_widget_add_css_class(loc_lbl, "ide-strip-tile-loc");
    gtk_box_append(GTK_BOX(hbox), loc_lbl);

    if (tile->call_count > 1) {
      g_autofree char *cnt_txt = g_strdup_printf("×%u", tile->call_count);
      GtkWidget *cnt_lbl = gtk_label_new(cnt_txt);
      gtk_widget_set_valign(cnt_lbl, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(cnt_lbl, "shell-popover-badge");
      gtk_box_append(GTK_BOX(hbox), cnt_lbl);
    }

    if (tile->diag_errors > 0) {
      g_autofree char *et = g_strdup_printf("✕%u", tile->diag_errors);
      GtkWidget *el = gtk_label_new(et);
      gtk_widget_set_valign(el, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(el, "ide-strip-diag-err");
      gtk_box_append(GTK_BOX(hbox), el);
    } else if (tile->is_agent_modified) {
      GtkWidget *al = gtk_label_new("AI");
      gtk_widget_set_valign(al, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(al, "shell-popover-badge");
      gtk_box_append(GTK_BOX(hbox), al);
    }

    GtkWidget *pin_btn = gtk_button_new_with_label("📌");
    gtk_widget_set_focusable(pin_btn, FALSE);
    gtk_widget_set_valign(pin_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(pin_btn, "ide-strip-tile-btn");
    gtk_widget_set_tooltip_text(pin_btn, "Pin reference above D0 (Space t p)");
    g_object_set_data(G_OBJECT(pin_btn), "tile-idx", GUINT_TO_POINTER(tile_idx));
    g_signal_connect(pin_btn, "clicked", G_CALLBACK(on_d1_tile_pin_clicked), sv);
    gtk_box_append(GTK_BOX(hbox), pin_btn);

    GtkWidget *prom_btn = gtk_button_new_with_label("→");
    gtk_widget_set_focusable(prom_btn, FALSE);
    gtk_widget_set_valign(prom_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(prom_btn, "ide-strip-tile-btn");
    gtk_widget_set_tooltip_text(prom_btn, "Promote into D0 (Enter)");
    g_object_set_data(G_OBJECT(prom_btn), "tile-idx", GUINT_TO_POINTER(tile_idx));
    g_signal_connect(prom_btn, "clicked", G_CALLBACK(on_d1_tile_promote_clicked), sv);
    gtk_box_append(GTK_BOX(hbox), prom_btn);

    gtk_box_append(GTK_BOX(vbox), hbox);

    /* Peekish Accordion preview label (visible ONLY when active_d1_idx == tile_idx) */
    GtkWidget *peek_lbl = gtk_label_new(tile->peek_code != NULL ? tile->peek_code : "");
    gtk_label_set_xalign(GTK_LABEL(peek_lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(peek_lbl), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(peek_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(peek_lbl, "ide-strip-peek-code");
    gtk_widget_set_visible(peek_lbl, FALSE);
    gtk_box_append(GTK_BOX(vbox), peek_lbl);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), vbox);
    gtk_list_box_append(GTK_LIST_BOX(sv->d1_list_box), row);

    D1RowEntry *re = g_new0(D1RowEntry, 1);
    re->row = row;
    re->peek_label = peek_lbl;
    re->tile_idx = tile_idx;
    g_ptr_array_add(sv->d1_rows, re);
  }

  if (filtered->len > STRIP_D1_MAX_ROWS) {
    GtkWidget *more_row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(more_row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(more_row), FALSE);
    gtk_widget_add_css_class(more_row, "ide-strip-overflow-row");
    g_autofree char *mtxt = g_strdup_printf("+%u more — type in / filter to narrow",
                                            filtered->len - STRIP_D1_MAX_ROWS);
    GtkWidget *mlbl = gtk_label_new(mtxt);
    gtk_label_set_xalign(GTK_LABEL(mlbl), 0.0f);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(more_row), mlbl);
    gtk_list_box_append(GTK_LIST_BOX(sv->d1_list_box), more_row);
  } else if (filtered->len == 0) {
    GtkWidget *empty_row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(empty_row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(empty_row), FALSE);
    gtk_widget_add_css_class(empty_row, "ide-strip-overflow-row");
    GtkWidget *elbl = gtk_label_new(
        (trail->d1_filter != NULL && trail->d1_filter[0] != '\0')
          ? "No matching workspace targets"
          : "No workspace targets for this lens");
    gtk_label_set_xalign(GTK_LABEL(elbl), 0.0f);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(empty_row), elbl);
    gtk_list_box_append(GTK_LIST_BOX(sv->d1_list_box), empty_row);
  }

  sv->rebuilding_d1 = FALSE;
  ide_strip_tiles_sync_d1_active(sv);

  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

void
ide_strip_tiles_rebuild_d2(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL || sv->d2_list_box == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  sv->rebuilding_d2 = TRUE;
  clear_listbox_children(sv->d2_list_box);

  guint total = trail->d2_tiles != NULL ? trail->d2_tiles->len : 0;
  if (sv->d2_header_label != NULL) {
    g_autofree char *hdr = g_strdup_printf("D+2 NEXT (%u)", total);
    gtk_label_set_text(GTK_LABEL(sv->d2_header_label), hdr);
  }

  guint cap = MIN(total, STRIP_D2_MAX_ROWS);
  GtkListBoxRow *active_row = NULL;
  for (guint i = 0; i < cap; i++) {
    const IdeStripTile *tile = g_ptr_array_index(trail->d2_tiles, i);
    if (tile == NULL) {
      continue;
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "ide-strip-d2-row");
    g_object_set_data(G_OBJECT(row), "d2-idx", GUINT_TO_POINTER(i));

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(hbox, TRUE);

    GtkWidget *kind_lbl = gtk_label_new(ide_strip_tile_kind_badge(tile->kind));
    gtk_widget_set_valign(kind_lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(kind_lbl, "ide-strip-kind-badge");
    gtk_box_append(GTK_BOX(hbox), kind_lbl);

    GtkWidget *name_lbl = gtk_label_new(tile->name != NULL ? tile->name : "(unnamed)");
    gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name_lbl, TRUE);
    gtk_widget_add_css_class(name_lbl, "ide-strip-micro-name");
    gtk_box_append(GTK_BOX(hbox), name_lbl);

    if (tile->child_count > 0) {
      g_autofree char *cnt = g_strdup_printf("→%u", tile->child_count);
      GtkWidget *clbl = gtk_label_new(cnt);
      gtk_widget_set_valign(clbl, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(clbl, "ide-strip-micro-count");
      gtk_box_append(GTK_BOX(hbox), clbl);
    }

    g_autofree char *tip = g_strdup_printf("%s · %s:%u (double-click to dive 2×)",
                                           tile->name != NULL ? tile->name : "",
                                           tile->rel_path != NULL ? tile->rel_path : "",
                                           tile->start_line);
    gtk_widget_set_tooltip_text(row, tip);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(GTK_LIST_BOX(sv->d2_list_box), row);
    if ((gint)i == trail->active_d2_idx) {
      active_row = GTK_LIST_BOX_ROW(row);
    }
  }

  if (total == 0) {
    GtkWidget *empty_row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(empty_row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(empty_row), FALSE);
    gtk_widget_add_css_class(empty_row, "ide-strip-overflow-row");
    GtkWidget *elbl = gtk_label_new("Leaf / no outgoing targets");
    gtk_label_set_xalign(GTK_LABEL(elbl), 0.0f);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(empty_row), elbl);
    gtk_list_box_append(GTK_LIST_BOX(sv->d2_list_box), empty_row);
  }

  if (active_row != NULL) {
    gtk_list_box_select_row(GTK_LIST_BOX(sv->d2_list_box), active_row);
  }
  sv->rebuilding_d2 = FALSE;
}

/* ═══════════════════════════ Tile List & Inline Filter Callbacks ═══════════════════════════ */

static void
on_d1_vadj_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

static void
on_d2_vadj_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

static void
on_resync_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_resync_d1((IdeStripView *)user_data);
}

static gboolean
on_d1_manual_scroll(GtkEventControllerScroll *ctrl G_GNUC_UNUSED,
                    double                    dx G_GNUC_UNUSED,
                    double                    dy,
                    gpointer                  user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->programmatic_d1_scroll || fabs(dy) < 1e-6) {
    return GDK_EVENT_PROPAGATE;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return GDK_EVENT_PROPAGATE;
  }
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d1_scroll));
  double cur_y = vadj != NULL ? gtk_adjustment_get_value(vadj) : trail->d1_scroll_y;
  ide_strip_trail_manual_scroll_d1(trail, cur_y);
  if (sv->d1_resync_btn != NULL) {
    gtk_widget_set_visible(sv->d1_resync_btn, TRUE);
  }
  return GDK_EVENT_PROPAGATE;
}

static void
on_d1_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->rebuilding_d1 || row == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL) {
    return;
  }
  guint tile_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "tile-idx"));
  if (tile_idx >= trail->d1_tiles->len) {
    return;
  }
  if (trail->active_d1_idx != (gint)tile_idx) {
    trail->active_d1_idx = (gint)tile_idx;
    ide_strip_tiles_sync_d1_active(sv);
    ide_strip_tiles_refresh_d2(sv);
    if (sv->bridge_overlay != NULL) {
      gtk_widget_queue_draw(sv->bridge_overlay);
    }
  }
}

static void
on_d1_row_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || row == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL) {
    return;
  }
  guint tile_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "tile-idx"));
  if (tile_idx >= trail->d1_tiles->len) {
    return;
  }
  trail->active_d1_idx = (gint)tile_idx;
  ide_strip_view_promote_active_d1(sv);
}

static void
on_d2_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->rebuilding_d2 || row == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d2_tiles == NULL) {
    return;
  }
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "d2-idx"));
  if (idx < trail->d2_tiles->len) {
    trail->active_d2_idx = (gint)idx;
    if (sv->bridge_overlay != NULL) {
      gtk_widget_queue_draw(sv->bridge_overlay);
    }
  }
}

static void
on_d2_row_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || row == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d2_tiles == NULL) {
    return;
  }
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "d2-idx"));
  if (idx < trail->d2_tiles->len) {
    trail->active_d2_idx = (gint)idx;
    ide_strip_view_promote_active_d2(sv);
  }
}

static void
on_d1_filter_changed(GtkEditable *editable, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->updating_filter_text) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  ide_strip_trail_set_filter(trail, gtk_editable_get_text(editable));
  ide_strip_tiles_rebuild_d1(sv);
  ide_strip_tiles_refresh_d2(sv);
}

static void
on_d1_filter_activate(GtkEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->d1_rows == NULL || sv->d1_rows->len == 0) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  const D1RowEntry *first = g_ptr_array_index(sv->d1_rows, 0);
  if (first != NULL) {
    trail->active_d1_idx = (gint)first->tile_idx;
    ide_strip_view_promote_active_d1(sv);
  }
}

static GtkWidget *
make_bridge_spacer(void)
{
  GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(sp, (int)IDE_STRIP_BRIDGE_WIDTH, -1);
  gtk_widget_set_can_target(sp, FALSE);
  gtk_widget_add_css_class(sp, "ide-strip-bridge-spacer");
  return sp;
}

GtkWidget *
ide_strip_tiles_build_d1_column(IdeStripView *sv)
{
  sv->d1_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(sv->d1_wrap), make_bridge_spacer());

  sv->d1_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sv->d1_col, "ide-strip-col-d1");
  gtk_widget_set_hexpand(sv->d1_col, TRUE);
  gtk_widget_set_vexpand(sv->d1_col, TRUE);

  GtkWidget *d1_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(d1_hdr, "ide-strip-col-header");
  gtk_widget_set_size_request(d1_hdr, -1, 26);

  sv->d1_header_label = gtk_label_new("D+1 TARGETS (0)");
  gtk_widget_set_valign(sv->d1_header_label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(sv->d1_header_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(sv->d1_header_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(sv->d1_header_label, TRUE);
  gtk_box_append(GTK_BOX(d1_hdr), sv->d1_header_label);

  sv->d1_resync_btn = ide_strip_view_make_toolbar_btn(
      "⛓ RESYNC", "ide-strip-resync-btn", "Re-engage magnetic viewport follow");
  gtk_widget_set_visible(sv->d1_resync_btn, FALSE);
  g_signal_connect(sv->d1_resync_btn, "clicked", G_CALLBACK(on_resync_clicked), sv);
  gtk_box_append(GTK_BOX(d1_hdr), sv->d1_resync_btn);
  gtk_box_append(GTK_BOX(sv->d1_col), d1_hdr);

  sv->d1_filter_entry = gtk_entry_new();
  gtk_editable_set_width_chars(GTK_EDITABLE(sv->d1_filter_entry), 8);
  gtk_widget_add_css_class(sv->d1_filter_entry, "ide-strip-filter");
  gtk_entry_set_placeholder_text(GTK_ENTRY(sv->d1_filter_entry), "Filter workspace tiles (/)");
  g_signal_connect(sv->d1_filter_entry, "changed", G_CALLBACK(on_d1_filter_changed), sv);
  g_signal_connect(sv->d1_filter_entry, "activate", G_CALLBACK(on_d1_filter_activate), sv);
  gtk_box_append(GTK_BOX(sv->d1_col), sv->d1_filter_entry);

  sv->d1_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sv->d1_scroll), FALSE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sv->d1_scroll),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(sv->d1_scroll, TRUE);
  GtkAdjustment *d1_vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d1_scroll));
  if (d1_vadj != NULL) {
    g_signal_connect(d1_vadj, "value-changed", G_CALLBACK(on_d1_vadj_changed), sv);
  }
  GtkEventController *d1_scroll_ctrl =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(d1_scroll_ctrl, "scroll", G_CALLBACK(on_d1_manual_scroll), sv);
  gtk_widget_add_controller(sv->d1_scroll, d1_scroll_ctrl);

  sv->d1_list_box = gtk_list_box_new();
  gtk_widget_add_css_class(sv->d1_list_box, "ide-strip-listbox");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(sv->d1_list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(sv->d1_list_box), FALSE);
  g_signal_connect(sv->d1_list_box, "row-selected", G_CALLBACK(on_d1_row_selected), sv);
  g_signal_connect(sv->d1_list_box, "row-activated", G_CALLBACK(on_d1_row_activated), sv);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sv->d1_scroll), sv->d1_list_box);
  gtk_box_append(GTK_BOX(sv->d1_col), sv->d1_scroll);

  gtk_box_append(GTK_BOX(sv->d1_wrap), sv->d1_col);
  return sv->d1_wrap;
}

GtkWidget *
ide_strip_tiles_build_d2_column(IdeStripView *sv)
{
  sv->d2_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(sv->d2_wrap), make_bridge_spacer());

  sv->d2_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sv->d2_col, "ide-strip-col-d2");
  gtk_widget_set_hexpand(sv->d2_col, TRUE);
  gtk_widget_set_vexpand(sv->d2_col, TRUE);

  GtkWidget *d2_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(d2_hdr, "ide-strip-col-header");
  gtk_widget_set_size_request(d2_hdr, -1, 26);

  sv->d2_header_label = gtk_label_new("D+2 NEXT (0)");
  gtk_widget_set_valign(sv->d2_header_label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(sv->d2_header_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(sv->d2_header_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(sv->d2_header_label, TRUE);
  gtk_box_append(GTK_BOX(d2_hdr), sv->d2_header_label);
  gtk_box_append(GTK_BOX(sv->d2_col), d2_hdr);

  sv->d2_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sv->d2_scroll), FALSE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sv->d2_scroll),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(sv->d2_scroll, TRUE);
  GtkAdjustment *d2_vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d2_scroll));
  if (d2_vadj != NULL) {
    g_signal_connect(d2_vadj, "value-changed", G_CALLBACK(on_d2_vadj_changed), sv);
  }

  sv->d2_list_box = gtk_list_box_new();
  gtk_widget_add_css_class(sv->d2_list_box, "ide-strip-listbox");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(sv->d2_list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(sv->d2_list_box), FALSE);
  g_signal_connect(sv->d2_list_box, "row-selected", G_CALLBACK(on_d2_row_selected), sv);
  g_signal_connect(sv->d2_list_box, "row-activated", G_CALLBACK(on_d2_row_activated), sv);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sv->d2_scroll), sv->d2_list_box);
  gtk_box_append(GTK_BOX(sv->d2_col), sv->d2_scroll);

  gtk_box_append(GTK_BOX(sv->d2_wrap), sv->d2_col);
  return sv->d2_wrap;
}

/* ═══════════════════════════ Public Pin / Step / Promote / Surface Handlers ═══════════════════════════ */

void
ide_strip_view_pin_active_d1(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL ||
      trail->active_d1_idx < 0 || (guint)trail->active_d1_idx >= trail->d1_tiles->len) {
    return;
  }
  const IdeStripTile *tile = g_ptr_array_index(trail->d1_tiles, (guint)trail->active_d1_idx);
  if (tile == NULL) {
    return;
  }

  ide_strip_trail_pin_tile(trail, tile);
  IdeBuffer *pbuf = NULL;
  if (sv->open_buf_cb != NULL && tile->file_path != NULL) {
    sv->binding_internal = TRUE;
    pbuf = sv->open_buf_cb(tile->file_path, tile->start_line, FALSE, sv->user_data);
    sv->binding_internal = FALSE;
  } else {
    pbuf = ide_strip_view_find_open_buffer(sv, tile->file_path);
  }

  sv->pin_buf = pbuf;
  if (pbuf != NULL && sv->pin_source_view != NULL) {
    GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(pbuf);
    if (sbuf != NULL) {
      gtk_text_view_set_buffer(GTK_TEXT_VIEW(sv->pin_source_view), GTK_TEXT_BUFFER(sbuf));
      ide_strip_view_scroll_source_to_line(sv->pin_source_view, tile->start_line, FALSE, 0.05);
    }
  }
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  g_autofree char *msg = g_strdup_printf("Pinned: %s", tile->name != NULL ? tile->name : "");
  ide_strip_view_emit_status(sv, msg);
}

void
ide_strip_view_unpin(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  ide_strip_trail_pin_tile(trail, NULL);
  sv->pin_buf = NULL;
  if (sv->pin_source_view != NULL && sv->empty_pin_buf != NULL) {
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(sv->pin_source_view),
                             GTK_TEXT_BUFFER(sv->empty_pin_buf));
  }
  ide_strip_view_rebuild_d0_header_and_pin(sv);
}

void
ide_strip_view_resync_d1(IdeStripView *sv)
{
  if (sv == NULL) {
    return;
  }
  ide_strip_tiles_run_viewport_band(sv, TRUE);
  ide_strip_view_emit_status(sv, "D+1 magnetic follow resynced");
}

void
ide_strip_view_step_d1(IdeStripView *sv, int delta)
{
  if (sv == NULL || sv->model == NULL || delta == 0) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || sv->d1_rows == NULL || sv->d1_rows->len == 0) {
    return;
  }

  gint cur_pos = -1;
  for (guint i = 0; i < sv->d1_rows->len; i++) {
    const D1RowEntry *re = g_ptr_array_index(sv->d1_rows, i);
    if ((gint)re->tile_idx == trail->active_d1_idx) {
      cur_pos = (gint)i;
      break;
    }
  }
  gint n = (gint)sv->d1_rows->len;
  gint next_pos = (cur_pos < 0) ? 0 : CLAMP(cur_pos + delta, 0, n - 1);
  const D1RowEntry *next_re = g_ptr_array_index(sv->d1_rows, (guint)next_pos);
  if (next_re == NULL) {
    return;
  }

  trail->active_d1_idx = (gint)next_re->tile_idx;
  ide_strip_tiles_sync_d1_active(sv);
  ide_strip_tiles_refresh_d2(sv);

  /* Ensure the newly selected row is visible in d1_scroll */
  if (sv->d1_scroll != NULL) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d1_scroll));
    if (vadj != NULL) {
      double target_y = (double)next_pos * IDE_STRIP_D1_TILE_STRIDE;
      double cur_y = gtk_adjustment_get_value(vadj);
      double page = gtk_adjustment_get_page_size(vadj);
      if (target_y < cur_y || target_y + IDE_STRIP_D1_PEEK_EXTRA_H > cur_y + page) {
        sv->programmatic_d1_scroll = TRUE;
        gtk_adjustment_set_value(vadj, MAX(0.0, target_y - 24.0));
        sv->programmatic_d1_scroll = FALSE;
      }
    }
  }
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

void
ide_strip_view_step_d2(IdeStripView *sv, int delta)
{
  if (sv == NULL || sv->model == NULL || delta == 0) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d2_tiles == NULL || trail->d2_tiles->len == 0) {
    return;
  }
  gint n = (gint)MIN(trail->d2_tiles->len, STRIP_D2_MAX_ROWS);
  gint next = (trail->active_d2_idx < 0) ? 0 : CLAMP(trail->active_d2_idx + delta, 0, n - 1);
  trail->active_d2_idx = next;

  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sv->d2_list_box), next);
  if (row != NULL) {
    sv->rebuilding_d2 = TRUE;
    gtk_list_box_select_row(GTK_LIST_BOX(sv->d2_list_box), row);
    sv->rebuilding_d2 = FALSE;
  }
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

void
ide_strip_view_promote_active_d1(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d1_tiles == NULL ||
      trail->active_d1_idx < 0 || (guint)trail->active_d1_idx >= trail->d1_tiles->len) {
    return;
  }

  guint cur_line = ide_strip_view_get_d0_cursor_line(sv);
  if (!ide_strip_trail_promote_d1(trail, (guint)trail->active_d1_idx, cur_line, ide_strip_view_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    ide_strip_view_open_and_attach_d0(sv, trail->root_tile->file_path, trail->root_tile->start_line, TRUE);
  }
  ide_strip_tiles_rebuild_spine(sv);
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  ide_strip_tiles_refresh_d1(sv);
  ide_strip_view_ensure_spring_tick(sv);
  if (sv->d0_source_view != NULL) {
    gtk_widget_grab_focus(sv->d0_source_view);
  }
}

void
ide_strip_view_promote_active_d2(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->d2_tiles == NULL ||
      trail->active_d2_idx < 0 || (guint)trail->active_d2_idx >= trail->d2_tiles->len) {
    return;
  }

  guint cur_line = ide_strip_view_get_d0_cursor_line(sv);
  if (!ide_strip_trail_promote_d2(trail, (guint)trail->active_d2_idx, cur_line, ide_strip_view_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    ide_strip_view_open_and_attach_d0(sv, trail->root_tile->file_path, trail->root_tile->start_line, TRUE);
  }
  ide_strip_tiles_rebuild_spine(sv);
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  ide_strip_tiles_refresh_d1(sv);
  ide_strip_view_ensure_spring_tick(sv);
  if (sv->d0_source_view != NULL) {
    gtk_widget_grab_focus(sv->d0_source_view);
  }
}

void
ide_strip_view_surface_one(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->ancestors == NULL || trail->ancestors->len == 0) {
    return;
  }

  guint top_idx = trail->ancestors->len - 1;
  const IdeStripAncestor *anc = g_ptr_array_index(trail->ancestors, top_idx);
  guint restore_line = anc != NULL ? anc->cursor_line : 1;
  gint saved_d1 = anc != NULL ? anc->active_d1_idx : -1;
  double saved_scroll = anc != NULL ? anc->d1_scroll_y : 0.0;

  if (!ide_strip_trail_surface_one(trail, ide_strip_view_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    ide_strip_view_open_and_attach_d0(sv, trail->root_tile->file_path, restore_line, TRUE);
  }
  ide_strip_tiles_rebuild_spine(sv);
  ide_strip_view_rebuild_d0_header_and_pin(sv);
  ide_strip_tiles_refresh_d1(sv);

  if (saved_d1 >= 0 && trail->d1_tiles != NULL && (guint)saved_d1 < trail->d1_tiles->len) {
    trail->active_d1_idx = saved_d1;
    trail->d1_scroll_y = saved_scroll;
    if (sv->d1_scroll != NULL) {
      GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sv->d1_scroll));
      if (vadj != NULL) {
        sv->programmatic_d1_scroll = TRUE;
        gtk_adjustment_set_value(vadj, MAX(0.0, saved_scroll));
        sv->programmatic_d1_scroll = FALSE;
      }
    }
    ide_strip_tiles_sync_d1_active(sv);
    ide_strip_tiles_refresh_d2(sv);
  }
  ide_strip_view_ensure_spring_tick(sv);
  if (sv->d0_source_view != NULL) {
    gtk_widget_grab_focus(sv->d0_source_view);
  }
}

void
ide_strip_view_set_lens(IdeStripView *sv, IdeStripLens lens)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  trail->lens = lens;
  trail->d1_unlinked = FALSE;
  ide_strip_tiles_refresh_d1(sv);
  g_autofree char *msg = g_strdup_printf("Code Strip lens: %s", ide_strip_lens_name(lens));
  ide_strip_view_emit_status(sv, msg);
}

void
ide_strip_view_branch_trail(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *branched = ide_strip_model_branch_trail(sv->model);
  if (branched == NULL) {
    return;
  }
  ide_strip_view_rebuild_all_ui(sv);
  ide_strip_tiles_refresh_d1(sv);
  g_autofree char *msg = g_strdup_printf("Branched trail %02u · %s",
                                         branched->id,
                                         branched->title != NULL ? branched->title : "Main");
  ide_strip_view_emit_status(sv, msg);
}

void
ide_strip_view_switch_trail(IdeStripView *sv, int delta)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  if (!ide_strip_model_switch_trail(sv->model, delta)) {
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
ide_strip_view_focus_filter(IdeStripView *sv)
{
  if (sv == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail != NULL && trail->overview_mode && sv->overview_entry != NULL) {
    gtk_widget_grab_focus(sv->overview_entry);
  } else if (sv->d1_filter_entry != NULL) {
    if (trail != NULL && trail->zen_mode) {
      ide_strip_view_toggle_zen(sv);
    }
    gtk_widget_grab_focus(sv->d1_filter_entry);
  }
}

void
ide_strip_view_focus_column(IdeStripView *sv, int delta)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  if (trail->zen_mode) {
    sv->focused_col = 0;
    if (sv->d0_source_view != NULL) {
      gtk_widget_grab_focus(sv->d0_source_view);
    }
    return;
  }

  sv->focused_col = CLAMP(sv->focused_col + delta, 0, 2);
  if (sv->focused_col == 0 && sv->d0_source_view != NULL) {
    gtk_widget_grab_focus(sv->d0_source_view);
  } else if (sv->focused_col == 1 && sv->d1_list_box != NULL) {
    gtk_widget_grab_focus(sv->d1_list_box);
  } else if (sv->focused_col == 2 && sv->d2_list_box != NULL) {
    gtk_widget_grab_focus(sv->d2_list_box);
  }
}
