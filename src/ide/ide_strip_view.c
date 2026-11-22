#include "ide_strip_view.h"

#include "../theme.h"
#include "ide_gutter.h"

#include <cairo.h>
#include <gtksourceview/gtksource.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define STRIP_D1_MAX_ROWS        200u
#define STRIP_D2_MAX_ROWS        120u
#define STRIP_DEBOUNCE_MS         50u
#define STRIP_BRIDGE_D2_MAX        6u
#define STRIP_BRIDGE_SECONDARY_MAX 5u

/* ═══════════════════════════ Internal Types ═══════════════════════════ */

typedef struct {
  GtkWidget *row;        /* GtkListBoxRow */
  GtkWidget *peek_label; /* collapsed unless active_d1_idx == tile_idx */
  guint      tile_idx;
} D1RowEntry;

typedef struct {
  double x;
  double y;
  double w;
  double h;
  int    col_type; /* -1=ancestor, 0=D0, 1=D+1, 2=D+2 */
  guint  idx;
} OverviewHitbox;

typedef struct {
  IdeStripView *sv;
  GCancellable *alive;
  guint64       gen;
  IdeStripLens  lens;
} StripLspCtx;

struct _IdeStripView {
  IdeStripModel        *model;
  IdeStripGraph        *graph;
  IdeLspManager        *lsp; /* borrowed */
  IdeStripOpenBufferCb  open_buf_cb;
  IdeStripGetBuffersCb  get_bufs_cb;
  IdeStripStatusCb      status_cb;
  IdeStripModeExitCb    exit_cb;
  gpointer              user_data;

  IdeStripBpCb          bp_cb;
  IdeStripCondBpCb      cond_bp_cb;
  gpointer              gutter_user_data;

  GCancellable         *alive;

  /* Editor settings */
  char                 *font_family;
  int                   font_size_px;
  int                   tab_width;
  gboolean              insert_spaces;
  gboolean              word_wrap;
  gboolean              show_margin_guide;
  gboolean              highlight_current_line;
  gboolean              show_line_numbers;
  gboolean              relative_line_numbers;
  gboolean              magnetic_follow;

  /* Bound D0 / pin buffers */
  IdeBuffer            *d0_buf;        /* borrowed */
  GtkTextBuffer        *d0_tbuf;       /* reffed while signals are connected */
  gulong                d0_mark_sig_id;
  gulong                d0_changed_sig_id;
  IdeBuffer            *pin_buf;       /* borrowed */
  GtkSourceBuffer      *empty_d0_buf;  /* owned fallback buffer when D0 unbound */
  GtkSourceBuffer      *empty_pin_buf; /* owned fallback buffer when unpinned */

  /* Async, timers & re-entrancy guards */
  guint64               query_gen;
  guint                 debounce_id;
  guint                 tick_id;
  guint                 paned_idle_id;
  gboolean              programmatic_d1_scroll;
  gboolean              programmatic_paned_pos;
  gboolean              paned_layout_initialized;
  gboolean              user_dragged_outer_paned;
  gboolean              user_dragged_right_paned;
  int                   last_alloc_w;
  int                   focused_col; /* 0=D0, 1=D+1, 2=D+2 */
  gboolean              rebuilding_d1;
  gboolean              rebuilding_d2;
  gboolean              updating_filter_text;
  gboolean              binding_internal;

  /* Top-level & toolbar widgets */
  GtkWidget            *root_box;
  GtkWidget            *toolbar;
  GtkWidget            *trails_box;
  GtkWidget            *branch_btn;
  GtkWidget            *lens_btns[4];
  GtkWidget            *ws_badge;
  GtkWidget            *engine_badge;
  GtkWidget            *zen_btn;
  GtkWidget            *overview_btn;
  GtkWidget            *exit_btn;

  /* Stack: "strip" vs "overview" */
  GtkWidget            *view_stack;

  /* Strip view widgets */
  GtkWidget            *strip_overlay;
  GtkWidget            *strip_hbox;
  GtkWidget            *bridge_overlay;
  GtkWidget            *spine_box;
  GtkWidget            *outer_paned;
  GtkWidget            *right_paned;
  GtkWidget            *zen_rail_btn;
  GtkWidget            *zen_rail_label;

  /* Column D0 */
  GtkWidget            *d0_vpaned;
  GtkWidget            *pin_box;
  GtkWidget            *pin_title_label;
  GtkWidget            *pin_close_btn;
  GtkWidget            *pin_scroll;
  GtkWidget            *pin_source_view;
  GtkWidget            *d0_main_box;
  GtkWidget            *d0_header;
  GtkWidget            *d0_kind_badge;
  GtkWidget            *d0_title_label;
  GtkWidget            *d0_loc_label;
  GtkWidget            *d0_fullfile_btn;
  GtkWidget            *d0_editor_overlay;
  GtkWidget            *d0_scroll;
  GtkWidget            *d0_source_view;
  KajoIdeGutterRenderer *d0_gutter;
  GtkWidget            *d0_veil_overlay;

  /* Column D+1 */
  GtkWidget            *d1_wrap;
  GtkWidget            *d1_col;
  GtkWidget            *d1_header_label;
  GtkWidget            *d1_resync_btn;
  GtkWidget            *d1_filter_entry;
  GtkWidget            *d1_scroll;
  GtkWidget            *d1_list_box;
  GPtrArray            *d1_rows; /* D1RowEntry* */

  /* Column D+2 */
  GtkWidget            *d2_wrap;
  GtkWidget            *d2_col;
  GtkWidget            *d2_header_label;
  GtkWidget            *d2_scroll;
  GtkWidget            *d2_list_box;

  /* Overview page */
  GtkWidget            *overview_page;
  GtkWidget            *overview_entry;
  GtkWidget            *overview_match_label;
  GtkWidget            *overview_close_btn;
  GtkWidget            *overview_canvas;
  GArray               *overview_hitboxes; /* OverviewHitbox */
};

/* ═══════════════════════════ Forward Declarations ═══════════════════════════ */

static void     rebuild_all_ui(IdeStripView *sv);
static void     rebuild_toolbar(IdeStripView *sv);
static void     rebuild_spine(IdeStripView *sv);
static void     rebuild_d0_header_and_pin(IdeStripView *sv);
static void     rebuild_d1_list(IdeStripView *sv);
static void     rebuild_d2_list(IdeStripView *sv);
static void     refresh_d1_targets(IdeStripView *sv);
static void     refresh_d2_targets(IdeStripView *sv);
static void     schedule_viewport_band_update(IdeStripView *sv);
static void     run_viewport_band_update(IdeStripView *sv, gboolean force_resync);
static void     ensure_spring_tick(IdeStripView *sv);
static void     schedule_paned_layout(IdeStripView *sv);
static void     sync_d1_active_visuals(IdeStripView *sv);
static void     scroll_source_view_to_line(GtkWidget *source_view,
                                           guint      line_1based,
                                           gboolean   move_cursor,
                                           double     yalign);
static gboolean open_and_attach_d0(IdeStripView *sv,
                                   const char   *file_path,
                                   guint         line_1based,
                                   gboolean      focus_buffer);

/* ═══════════════════════════ Small Utilities ═══════════════════════════ */

static double
strip_now_sec(void)
{
  return (double)g_get_monotonic_time() / 1000000.0;
}

static void
emit_status(IdeStripView *sv, const char *msg)
{
  if (sv != NULL && sv->status_cb != NULL && msg != NULL) {
    sv->status_cb(msg, sv->user_data);
  }
}

static void
lens_rgb(IdeStripLens lens, double *r, double *g, double *b)
{
  switch (lens) {
    case IDE_STRIP_LENS_TYPES:
      *r = 0.0;   *g = 0.847; *b = 0.941; /* #00d8f0 */
      break;
    case IDE_STRIP_LENS_IMPACT:
      *r = 1.0;   *g = 0.725; *b = 0.0;   /* #ffb900 */
      break;
    case IDE_STRIP_LENS_DIFF:
      *r = 0.706; *g = 0.0;   *b = 0.620; /* #b4009e */
      break;
    case IDE_STRIP_LENS_CALLS:
    default:
      *r = 0.0;   *g = 0.471; *b = 0.831; /* #0078d4 */
      break;
  }
}

static const char *
lens_css_class(IdeStripLens lens)
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

static IdeBuffer *
find_open_buffer_for_path(const IdeStripView *sv, const char *file_path)
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

static void
enrich_tile_from_open_buffer(const IdeStripView *sv, IdeStripTile *tile)
{
  if (sv == NULL || tile == NULL || tile->file_path == NULL) {
    return;
  }
  IdeBuffer *b = find_open_buffer_for_path(sv, tile->file_path);
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
    enrich_tile_from_open_buffer(sv, tile);
    g_ptr_array_add(tiles, tile);
  }
  return tiles;
}

static void
clear_box_children(GtkWidget *box)
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

static guint
get_d0_cursor_line(const IdeStripView *sv)
{
  if (sv == NULL || sv->d0_tbuf == NULL) {
    return 1;
  }
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(sv->d0_tbuf, &iter, gtk_text_buffer_get_insert(sv->d0_tbuf));
  return (guint)gtk_text_iter_get_line(&iter) + 1;
}

static guint
get_d0_cursor_col(const IdeStripView *sv)
{
  if (sv == NULL || sv->d0_tbuf == NULL) {
    return 1;
  }
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(sv->d0_tbuf, &iter, gtk_text_buffer_get_insert(sv->d0_tbuf));
  return (guint)gtk_text_iter_get_line_offset(&iter) + 1;
}

static void
scroll_source_view_to_line(GtkWidget *source_view,
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
    schedule_viewport_band_update(sv);
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

static gboolean
open_and_attach_d0(IdeStripView *sv,
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
    buf = find_open_buffer_for_path(sv, file_path);
  }
  if (buf == NULL) {
    return FALSE;
  }
  attach_d0_buffer_internal(sv, buf);
  if (line_1based > 0) {
    scroll_source_view_to_line(sv->d0_source_view, line_1based, TRUE, 0.15);
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

  gboolean animating = ide_strip_trail_step_springs(trail, strip_now_sec());

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

static void
ensure_spring_tick(IdeStripView *sv)
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

/* ═══════════════════════════ Viewport Band & Magnetic Follow ═══════════════════════════ */

static void
run_viewport_band_update(IdeStripView *sv, gboolean force_resync)
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
  guint cur_line = get_d0_cursor_line(sv);

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
                                               cursor_rel_y, d1_vh, strip_now_sec());
  } else {
    active_changed = ide_strip_trail_update_viewport_band(trail, vis_top, vis_bot, cur_line,
                                                          cursor_rel_y, d1_vh,
                                                          sv->magnetic_follow, strip_now_sec());
  }

  sync_d1_active_visuals(sv);
  if (active_changed) {
    refresh_d2_targets(sv);
  }
  if (sv->d1_resync_btn != NULL) {
    gtk_widget_set_visible(sv->d1_resync_btn, trail->d1_unlinked);
  }
  ensure_spring_tick(sv);
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
  run_viewport_band_update(sv, FALSE);
  return G_SOURCE_REMOVE;
}

static void
schedule_viewport_band_update(IdeStripView *sv)
{
  if (sv == NULL || !sv->model->strip_active) {
    return;
  }
  if (sv->debounce_id != 0) {
    g_source_remove(sv->debounce_id);
  }
  sv->debounce_id = g_timeout_add(STRIP_DEBOUNCE_MS, on_viewport_debounce_timeout, sv);
}

/* ═══════════════════════════ Async LSP Enrichment ═══════════════════════════ */

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
    rebuild_toolbar(sv);
    rebuild_d1_list(sv);
    run_viewport_band_update(sv, FALSE);
    refresh_d2_targets(sv);
  } else if (!is_fallback) {
    /* LSP answered cleanly with 0 targets; keep fallback indicator accurate */
    trail->using_fallback = FALSE;
    rebuild_toolbar(sv);
  }
}

/* ═══════════════════════════ Target Extraction (D+1 & D+2) ═══════════════════════════ */

static void
refresh_d1_targets(IdeStripView *sv)
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
  rebuild_toolbar(sv);
  rebuild_d1_list(sv);
  run_viewport_band_update(sv, FALSE);
  refresh_d2_targets(sv);

  /* Async LSP enrichment for CALLS / IMPACT when a ready server handles D0 */
  if (lsp_ready &&
      (trail->lens == IDE_STRIP_LENS_CALLS || trail->lens == IDE_STRIP_LENS_IMPACT) &&
      trail->root_tile != NULL) {
    guint q_line = trail->root_tile->start_line > 0 ? trail->root_tile->start_line : get_d0_cursor_line(sv);
    guint q_col = 1;
    if (trail->lens == IDE_STRIP_LENS_IMPACT) {
      q_line = get_d0_cursor_line(sv);
      q_col = get_d0_cursor_col(sv);
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

static void
refresh_d2_targets(IdeStripView *sv)
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
    rebuild_d2_list(sv);
    return;
  }

  const IdeStripTile *active_d1 = g_ptr_array_index(trail->d1_tiles, (guint)trail->active_d1_idx);
  if (active_d1 == NULL || active_d1->file_path == NULL) {
    ide_strip_trail_set_d2_tiles(trail, NULL);
    rebuild_d2_list(sv);
    return;
  }

  g_autofree char *override_text = NULL;
  IdeBuffer *open_b = find_open_buffer_for_path(sv, active_d1->file_path);
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
  rebuild_d2_list(sv);
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  if (sv->overview_canvas != NULL && trail->overview_mode) {
    gtk_widget_queue_draw(sv->overview_canvas);
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

static void
schedule_paned_layout(IdeStripView *sv)
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

static void
on_bridge_overlay_resize(GtkDrawingArea *area G_GNUC_UNUSED,
                         int             width,
                         int             height G_GNUC_UNUSED,
                         gpointer        user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || width <= 0) {
    return;
  }
  if (width != sv->last_alloc_w) {
    sv->last_alloc_w = width;
    sv->user_dragged_outer_paned = FALSE;
    sv->user_dragged_right_paned = FALSE;
    schedule_paned_layout(sv);
  }
}

/* ═══════════════════════════ D0 Focus Veil Drawing ═══════════════════════════ */

static void
draw_d0_veil(GtkDrawingArea *area,
             cairo_t        *cr,
             int             width,
             int             height,
             gpointer        user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->model == NULL || sv->d0_source_view == NULL || sv->d0_tbuf == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->root_tile == NULL || trail->show_full_file) {
    return;
  }

  int total_lines = gtk_text_buffer_get_line_count(sv->d0_tbuf);
  guint s_line = trail->root_tile->start_line;
  guint e_line = trail->root_tile->end_line;
  if (s_line <= 1 && e_line >= (guint)MAX(1, total_lines)) {
    return;
  }

  int start0 = (s_line > 0) ? (int)(s_line - 1) : 0;
  int end0 = (e_line > 0) ? (int)(e_line - 1) : start0;
  start0 = CLAMP(start0, 0, MAX(0, total_lines - 1));
  end0 = CLAMP(end0, start0, MAX(0, total_lines - 1));

  GtkTextView *tv = GTK_TEXT_VIEW(sv->d0_source_view);
  GtkTextIter it_start, it_end;
  gtk_text_buffer_get_iter_at_line(sv->d0_tbuf, &it_start, start0);
  gtk_text_buffer_get_iter_at_line(sv->d0_tbuf, &it_end, end0);

  int by1 = 0, bh1 = 0, by2 = 0, bh2 = 0;
  gtk_text_view_get_line_yrange(tv, &it_start, &by1, &bh1);
  gtk_text_view_get_line_yrange(tv, &it_end, &by2, &bh2);

  int wy1 = 0, wy2 = 0;
  gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, 0, by1, NULL, &wy1);
  gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, 0, by2 + bh2, NULL, &wy2);

  graphene_point_t p1 = GRAPHENE_POINT_INIT(0.0f, (float)wy1);
  graphene_point_t p2 = GRAPHENE_POINT_INIT(0.0f, (float)wy2);
  graphene_point_t o1 = GRAPHENE_POINT_INIT(0.0f, (float)wy1);
  graphene_point_t o2 = GRAPHENE_POINT_INIT(0.0f, (float)wy2);
  if (!gtk_widget_compute_point(sv->d0_source_view, GTK_WIDGET(area), &p1, &o1) ||
      !gtk_widget_compute_point(sv->d0_source_view, GTK_WIDGET(area), &p2, &o2)) {
    return;
  }

  double y1 = CLAMP((double)o1.y, 0.0, (double)height);
  double y2 = CLAMP((double)o2.y, 0.0, (double)height);

  /* Dark veil above start_line */
  if (y1 > 0.5) {
    cairo_set_source_rgba(cr, 0.024, 0.024, 0.024, 0.58);
    cairo_rectangle(cr, 0.0, 0.0, (double)width, y1);
    cairo_fill(cr);
  }
  /* Crisp 2px #0078d4 top scope boundary */
  if (o1.y >= -2.0f && o1.y <= (float)height + 2.0f) {
    cairo_set_source_rgba(cr, 0.0, 0.471, 0.831, 0.90);
    cairo_rectangle(cr, 0.0, (double)o1.y - 1.0, (double)width, 2.0);
    cairo_fill(cr);
  }

  /* Dark veil below end_line */
  if (y2 < (double)height - 0.5) {
    cairo_set_source_rgba(cr, 0.024, 0.024, 0.024, 0.58);
    cairo_rectangle(cr, 0.0, y2, (double)width, (double)height - y2);
    cairo_fill(cr);
  }
  /* Crisp 2px #0078d4 bottom scope boundary */
  if (o2.y >= -2.0f && o2.y <= (float)height + 2.0f) {
    cairo_set_source_rgba(cr, 0.0, 0.471, 0.831, 0.90);
    cairo_rectangle(cr, 0.0, (double)o2.y - 1.0, (double)width, 2.0);
    cairo_fill(cr);
  }
}

/* ═══════════════════════════ Cairo Dependency Bridges ═══════════════════════════ */

static gboolean
compute_d0_line_anchor(IdeStripView *sv,
                       GtkWidget    *target_widget,
                       guint         line_1based,
                       double       *out_x,
                       double       *out_y)
{
  if (sv == NULL || sv->d0_source_view == NULL || sv->d0_editor_overlay == NULL) {
    return FALSE;
  }
  int ed_w = gtk_widget_get_width(sv->d0_editor_overlay);
  int ed_h = gtk_widget_get_height(sv->d0_editor_overlay);
  if (ed_w <= 0 || ed_h <= 0) {
    return FALSE;
  }

  graphene_point_t top_r = GRAPHENE_POINT_INIT((float)ed_w, 0.0f);
  graphene_point_t bot_r = GRAPHENE_POINT_INIT((float)ed_w, (float)ed_h);
  graphene_point_t ot = GRAPHENE_POINT_INIT(0, 0), ob = GRAPHENE_POINT_INIT(0, 0);
  if (!gtk_widget_compute_point(sv->d0_editor_overlay, target_widget, &top_r, &ot) ||
      !gtk_widget_compute_point(sv->d0_editor_overlay, target_widget, &bot_r, &ob)) {
    return FALSE;
  }

  double y = (double)(ot.y + ob.y) * 0.5;
  if (sv->d0_tbuf != NULL && line_1based > 0) {
    int total = gtk_text_buffer_get_line_count(sv->d0_tbuf);
    int line0 = CLAMP((int)(line_1based - 1), 0, MAX(0, total - 1));
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(sv->d0_tbuf, &it, line0);
    int by = 0, bh = 0, wy = 0;
    gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(sv->d0_source_view), &it, &by, &bh);
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(sv->d0_source_view),
                                          GTK_TEXT_WINDOW_WIDGET,
                                          0, by + bh / 2, NULL, &wy);
    graphene_point_t lp = GRAPHENE_POINT_INIT(0.0f, (float)wy);
    graphene_point_t op = GRAPHENE_POINT_INIT(0.0f, 0.0f);
    if (gtk_widget_compute_point(sv->d0_source_view, target_widget, &lp, &op)) {
      y = CLAMP((double)op.y, (double)ot.y + 6.0, (double)ob.y - 6.0);
    }
  }

  *out_x = (double)ot.x;
  *out_y = y;
  return TRUE;
}

static gboolean
widget_vertical_bounds_in(GtkWidget *w, GtkWidget *ref, double *out_top, double *out_bot)
{
  if (w == NULL || ref == NULL || !gtk_widget_get_mapped(w)) {
    return FALSE;
  }
  int h = gtk_widget_get_height(w);
  if (h <= 0) {
    return FALSE;
  }
  graphene_point_t p0 = GRAPHENE_POINT_INIT(0.0f, 0.0f);
  graphene_point_t p1 = GRAPHENE_POINT_INIT(0.0f, (float)h);
  graphene_point_t o0 = GRAPHENE_POINT_INIT(0, 0), o1 = GRAPHENE_POINT_INIT(0, 0);
  if (!gtk_widget_compute_point(w, ref, &p0, &o0) ||
      !gtk_widget_compute_point(w, ref, &p1, &o1)) {
    return FALSE;
  }
  *out_top = (double)o0.y;
  *out_bot = (double)o1.y;
  return TRUE;
}

static void
draw_bezier_connector(cairo_t  *cr,
                      double    x0,
                      double    y0,
                      double    x1,
                      double    y1,
                      double    r,
                      double    g,
                      double    b,
                      double    alpha,
                      double    line_w,
                      gboolean  dashed,
                      gboolean  draw_terminals)
{
  double dx = MAX(10.0, fabs(x1 - x0) * 0.55);

  cairo_save(cr);
  cairo_set_source_rgba(cr, r, g, b, alpha);
  cairo_set_line_width(cr, line_w);
  if (dashed) {
    static const double dash[2] = { 5.0, 4.0 };
    cairo_set_dash(cr, dash, 2, 0.0);
  }

  double end_x = draw_terminals ? (x1 - 5.0) : x1;
  cairo_move_to(cr, x0, y0);
  cairo_curve_to(cr, x0 + dx, y0, end_x - dx, y1, end_x, y1);
  cairo_stroke(cr);

  if (draw_terminals) {
    cairo_set_dash(cr, NULL, 0, 0.0);
    /* Crisp 4px square source anchor */
    cairo_rectangle(cr, x0 - 2.0, y0 - 2.0, 4.0, 4.0);
    cairo_fill(cr);

    /* Crisp right-pointing arrowhead */
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x1 - 6.0, y1 - 3.5);
    cairo_line_to(cr, x1 - 6.0, y1 + 3.5);
    cairo_close_path(cr);
    cairo_fill(cr);
  }
  cairo_restore(cr);
}

static void
draw_bridges(GtkDrawingArea *area,
             cairo_t        *cr,
             int             width G_GNUC_UNUSED,
             int             height G_GNUC_UNUSED,
             gpointer        user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->model == NULL || !sv->model->strip_active) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL || trail->zen_mode || trail->overview_mode) {
    return;
  }
  if (sv->d1_rows == NULL || sv->d1_rows->len == 0) {
    return;
  }

  double r, g, b;
  lens_rgb(trail->lens, &r, &g, &b);
  GtkWidget *ref = GTK_WIDGET(area);

  double d1_top = 0.0, d1_bot = 0.0;
  if (!widget_vertical_bounds_in(sv->d1_scroll, ref, &d1_top, &d1_bot)) {
    return;
  }

  /* 1. Draw subtle secondary bridges for visible in-band D+1 tiles */
  guint secondary_drawn = 0;
  GtkWidget *active_row = NULL;
  const IdeStripTile *active_tile = NULL;

  for (guint i = 0; i < sv->d1_rows->len; i++) {
    const D1RowEntry *re = g_ptr_array_index(sv->d1_rows, i);
    if (re == NULL || re->row == NULL || !gtk_widget_get_mapped(re->row)) {
      continue;
    }
    if (re->tile_idx >= trail->d1_tiles->len) {
      continue;
    }
    const IdeStripTile *tile = g_ptr_array_index(trail->d1_tiles, re->tile_idx);
    if ((gint)re->tile_idx == trail->active_d1_idx) {
      active_row = re->row;
      active_tile = tile;
      continue;
    }
    if (!tile->in_viewport_band || secondary_drawn >= STRIP_BRIDGE_SECONDARY_MAX) {
      continue;
    }

    int rh = gtk_widget_get_height(re->row);
    graphene_point_t dst_p = GRAPHENE_POINT_INIT(0.0f, (float)MIN(rh, 32) * 0.5f);
    graphene_point_t dst_o = GRAPHENE_POINT_INIT(0, 0);
    if (!gtk_widget_compute_point(re->row, ref, &dst_p, &dst_o)) {
      continue;
    }
    if ((double)dst_o.y < d1_top + 2.0 || (double)dst_o.y > d1_bot - 2.0) {
      continue;
    }
    double sx = 0.0, sy = 0.0;
    if (compute_d0_line_anchor(sv, ref, tile->call_line, &sx, &sy)) {
      draw_bezier_connector(cr, sx, sy, (double)dst_o.x, (double)dst_o.y,
                            r, g, b, 0.28, 1.1, trail->using_fallback, FALSE);
      secondary_drawn++;
    }
  }

  /* 2. Draw primary bright bridge D0 -> active D+1 row */
  if (active_row == NULL || active_tile == NULL) {
    return;
  }
  int active_rw = gtk_widget_get_width(active_row);
  int active_rh = gtk_widget_get_height(active_row);
  float header_mid_y = (float)MIN(active_rh, 32) * 0.5f;

  graphene_point_t d1_left_p = GRAPHENE_POINT_INIT(0.0f, header_mid_y);
  graphene_point_t d1_right_p = GRAPHENE_POINT_INIT((float)active_rw, header_mid_y);
  graphene_point_t d1_left_o = GRAPHENE_POINT_INIT(0, 0);
  graphene_point_t d1_right_o = GRAPHENE_POINT_INIT(0, 0);
  if (!gtk_widget_compute_point(active_row, ref, &d1_left_p, &d1_left_o) ||
      !gtk_widget_compute_point(active_row, ref, &d1_right_p, &d1_right_o)) {
    return;
  }

  double d1_y = CLAMP((double)d1_left_o.y, d1_top + 4.0, d1_bot - 4.0);
  double sx = 0.0, sy = 0.0;
  if (compute_d0_line_anchor(sv, ref, active_tile->call_line, &sx, &sy)) {
    draw_bezier_connector(cr, sx, sy, (double)d1_left_o.x, d1_y,
                          r, g, b, 0.95, 2.0, trail->using_fallback, TRUE);
  }

  /* 3. Draw Bridge 2: active D+1 -> first 6 visible D+2 rows */
  double d2_top = 0.0, d2_bot = 0.0;
  if (sv->d2_list_box == NULL ||
      !widget_vertical_bounds_in(sv->d2_scroll, ref, &d2_top, &d2_bot)) {
    return;
  }

  guint idx = 0;
  guint drawn = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(sv->d2_list_box);
       child != NULL && drawn < STRIP_BRIDGE_D2_MAX;
       child = gtk_widget_get_next_sibling(child), idx++) {
    if (!gtk_widget_get_mapped(child)) {
      continue;
    }
    int rh = gtk_widget_get_height(child);
    if (rh <= 0) {
      continue;
    }
    graphene_point_t dp = GRAPHENE_POINT_INIT(0.0f, (float)rh * 0.5f);
    graphene_point_t do_pt = GRAPHENE_POINT_INIT(0, 0);
    if (!gtk_widget_compute_point(child, ref, &dp, &do_pt)) {
      continue;
    }
    if ((double)do_pt.y < d2_top + 2.0 || (double)do_pt.y > d2_bot - 2.0) {
      continue;
    }
    gboolean is_active_d2 = ((gint)idx == trail->active_d2_idx);
    draw_bezier_connector(cr,
                          (double)d1_right_o.x, d1_y,
                          (double)do_pt.x, (double)do_pt.y,
                          r, g, b,
                          is_active_d2 ? 0.88 : 0.36,
                          is_active_d2 ? 1.6 : 1.0,
                          trail->using_fallback,
                          is_active_d2);
    drawn++;
  }
}

/* ═══════════════════════════ Bird's-Eye Overview Canvas ═══════════════════════════ */

static gboolean
overview_matches_query(const char *name, const char *rel_path, const char *query_lower)
{
  if (query_lower == NULL || query_lower[0] == '\0') {
    return TRUE;
  }
  if (name != NULL) {
    g_autofree char *low = g_utf8_casefold(name, -1);
    if (strstr(low, query_lower) != NULL) {
      return TRUE;
    }
  }
  if (rel_path != NULL) {
    g_autofree char *low = g_utf8_casefold(rel_path, -1);
    if (strstr(low, query_lower) != NULL) {
      return TRUE;
    }
  }
  return FALSE;
}

static void
draw_overview_card(cairo_t              *cr,
                   PangoFontDescription *font_desc,
                   double                x,
                   double                y,
                   double                w,
                   double                h,
                   const char           *badge,
                   const char           *title,
                   const char           *subtitle,
                   double                ar,
                   double                ag,
                   double                ab,
                   gboolean              is_active,
                   gboolean              is_match,
                   gboolean              has_filter)
{
  double alpha = (!has_filter || is_match) ? 1.0 : 0.28;

  cairo_save(cr);
  /* Metro 0px sharp rectangle fill */
  if (is_active) {
    cairo_set_source_rgba(cr, 0.08, 0.14, 0.20, 0.96 * alpha);
  } else if (has_filter && is_match) {
    cairo_set_source_rgba(cr, 0.07, 0.16, 0.20, 0.96);
  } else {
    cairo_set_source_rgba(cr, 0.095, 0.095, 0.095, 0.95 * alpha);
  }
  cairo_rectangle(cr, x, y, w, h);
  cairo_fill(cr);

  /* Border */
  if (is_active) {
    cairo_set_source_rgba(cr, 0.0, 0.847, 0.941, 1.0); /* #00d8f0 active viewport box */
    cairo_set_line_width(cr, 2.0);
  } else if (has_filter && is_match) {
    cairo_set_source_rgba(cr, 0.0, 0.847, 0.941, 0.90);
    cairo_set_line_width(cr, 1.5);
  } else {
    cairo_set_source_rgba(cr, 0.22, 0.22, 0.22, 0.95 * alpha);
    cairo_set_line_width(cr, 1.0);
  }
  cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
  cairo_stroke(cr);

  /* Left 3px lens bar */
  cairo_set_source_rgba(cr, ar, ag, ab, (is_active ? 1.0 : 0.72) * alpha);
  cairo_rectangle(cr, x, y, 3.0, h);
  cairo_fill(cr);

  /* Title + subtitle via PangoLayout */
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, font_desc);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_width(layout, (int)((w - 16.0) * PANGO_SCALE));

  g_autofree char *line1 = g_strdup_printf("[%s] %s",
                                           badge != NULL ? badge : "·",
                                           title != NULL ? title : "(unnamed)");
  pango_layout_set_text(layout, line1, -1);
  cairo_set_source_rgba(cr, 0.93, 0.93, 0.93, alpha);
  cairo_move_to(cr, x + 9.0, y + 5.0);
  pango_cairo_show_layout(cr, layout);

  if (subtitle != NULL && subtitle[0] != '\0' && h >= 36.0) {
    pango_layout_set_text(layout, subtitle, -1);
    cairo_set_source_rgba(cr, 0.62, 0.62, 0.62, alpha);
    cairo_move_to(cr, x + 9.0, y + 21.0);
    pango_cairo_show_layout(cr, layout);
  }

  g_object_unref(layout);
  cairo_restore(cr);
}

static void
draw_overview_canvas(GtkDrawingArea *area G_GNUC_UNUSED,
                     cairo_t        *cr,
                     int             width,
                     int             height,
                     gpointer        user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  g_array_set_size(sv->overview_hitboxes, 0);

  /* Dark Metro background + subtle 24px technical grid */
  cairo_set_source_rgb(cr, 0.067, 0.067, 0.067);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.03);
  cairo_set_line_width(cr, 1.0);
  for (int gx = 24; gx < width; gx += 24) {
    cairo_move_to(cr, gx + 0.5, 0);
    cairo_line_to(cr, gx + 0.5, height);
  }
  for (int gy = 24; gy < height; gy += 24) {
    cairo_move_to(cr, 0, gy + 0.5);
    cairo_line_to(cr, width, gy + 0.5);
  }
  cairo_stroke(cr);

  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  const char *raw_q = sv->overview_entry != NULL
                        ? gtk_editable_get_text(GTK_EDITABLE(sv->overview_entry))
                        : "";
  gboolean has_filter = (raw_q != NULL && raw_q[0] != '\0');
  g_autofree char *q_lower = has_filter ? g_utf8_casefold(raw_q, -1) : NULL;

  double lr, lg, lb;
  lens_rgb(trail->lens, &lr, &lg, &lb);

  g_autofree char *font_stack = shell_ide_font_stack(sv->font_family ? sv->font_family : "Fira Code");
  PangoFontDescription *font_desc = pango_font_description_new();
  pango_font_description_set_family(font_desc, font_stack);
  pango_font_description_set_size(font_desc, 9 * PANGO_SCALE);

  PangoFontDescription *hdr_desc = pango_font_description_new();
  pango_font_description_set_family(hdr_desc, font_stack);
  pango_font_description_set_weight(hdr_desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_size(hdr_desc, 9 * PANGO_SCALE);

  gboolean has_anc = (trail->ancestors != NULL && trail->ancestors->len > 0);
  int num_cols = has_anc ? 4 : 3;
  double pad_x = 24.0;
  double gap_x = 34.0;
  double col_w = MAX(140.0, ((double)width - 2.0 * pad_x - (double)(num_cols - 1) * gap_x) / (double)num_cols);
  double top_y = 36.0;
  double avail_h = MAX(80.0, (double)height - top_y - 20.0);

  double col_x[4];
  int ci = 0;
  double x_anc = 0.0, x_d0 = 0.0, x_d1 = 0.0, x_d2 = 0.0;
  if (has_anc) {
    x_anc = pad_x + (double)ci * (col_w + gap_x);
    col_x[ci++] = x_anc;
  }
  x_d0 = pad_x + (double)ci * (col_w + gap_x);
  col_x[ci++] = x_d0;
  x_d1 = pad_x + (double)ci * (col_w + gap_x);
  col_x[ci++] = x_d1;
  x_d2 = pad_x + (double)ci * (col_w + gap_x);
  col_x[ci++] = x_d2;
  (void)col_x;

  /* Column Headers */
  PangoLayout *hl = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(hl, hdr_desc);
  cairo_set_source_rgba(cr, 0.65, 0.65, 0.65, 1.0);
  if (has_anc) {
    pango_layout_set_text(hl, "ANCESTORS", -1);
    cairo_move_to(cr, x_anc, 12.0);
    pango_cairo_show_layout(cr, hl);
  }
  pango_layout_set_text(hl, "D0 · ACTIVE FOCUS", -1);
  cairo_move_to(cr, x_d0, 12.0);
  pango_cairo_show_layout(cr, hl);

  g_autofree char *d1_hdr = g_strdup_printf("D+1 · %s (%u)",
                                            ide_strip_lens_name(trail->lens),
                                            trail->d1_tiles != NULL ? trail->d1_tiles->len : 0u);
  pango_layout_set_text(hl, d1_hdr, -1);
  cairo_move_to(cr, x_d1, 12.0);
  pango_cairo_show_layout(cr, hl);

  g_autofree char *d2_hdr = g_strdup_printf("D+2 · NEXT (%u)",
                                            trail->d2_tiles != NULL ? trail->d2_tiles->len : 0u);
  pango_layout_set_text(hl, d2_hdr, -1);
  cairo_move_to(cr, x_d2, 12.0);
  pango_cairo_show_layout(cr, hl);
  g_object_unref(hl);

  guint match_count = 0;

  /* D0 Root card centered vertically */
  double d0_h = 56.0;
  double d0_y = top_y + MAX(0.0, (avail_h - d0_h) * 0.35);
  double d0_mid_y = d0_y + d0_h * 0.5;

  /* Draw Ancestors -> D0 */
  if (has_anc) {
    guint n_anc = MIN(trail->ancestors->len, 12u);
    double card_h = MIN(42.0, (avail_h - (double)(n_anc > 1 ? n_anc - 1 : 0) * 6.0) / (double)MAX(1u, n_anc));
    card_h = MAX(26.0, card_h);
    for (guint i = 0; i < n_anc; i++) {
      const IdeStripAncestor *anc = g_ptr_array_index(trail->ancestors, i);
      const IdeStripTile *t = anc != NULL ? anc->tile : NULL;
      if (t == NULL) {
        continue;
      }
      double cy = top_y + (double)i * (card_h + 6.0);
      gboolean matched = overview_matches_query(t->name, t->rel_path, q_lower);
      if (matched) match_count++;
      g_autofree char *sub = g_strdup_printf("%s:%u", t->rel_path ? t->rel_path : "", t->start_line);
      draw_overview_card(cr, font_desc, x_anc, cy, col_w, card_h,
                         ide_strip_tile_kind_badge(t->kind), t->name, sub,
                         0.48, 0.48, 0.48, FALSE, matched, has_filter);
      draw_bezier_connector(cr, x_anc + col_w, cy + card_h * 0.5, x_d0, d0_mid_y,
                            lr, lg, lb, 0.40, 1.2, FALSE, FALSE);
      OverviewHitbox hb = { x_anc, cy, col_w, card_h, -1, i };
      g_array_append_val(sv->overview_hitboxes, hb);
    }
  }

  /* Draw D0 Root Card */
  if (trail->root_tile != NULL) {
    const IdeStripTile *rt = trail->root_tile;
    gboolean matched = overview_matches_query(rt->name, rt->rel_path, q_lower);
    if (matched) match_count++;
    g_autofree char *sub = g_strdup_printf("%s:%u–%u",
                                           rt->rel_path ? rt->rel_path : "",
                                           rt->start_line, rt->end_line);
    draw_overview_card(cr, font_desc, x_d0, d0_y, col_w, d0_h,
                       ide_strip_tile_kind_badge(rt->kind), rt->name, sub,
                       lr, lg, lb, TRUE, matched, has_filter);
    OverviewHitbox hb = { x_d0, d0_y, col_w, d0_h, 0, 0 };
    g_array_append_val(sv->overview_hitboxes, hb);
  }

  /* Draw D+1 Cards (up to 18) */
  double active_d1_mid_y = d0_mid_y;
  gboolean have_active_d1 = FALSE;
  if (trail->d1_tiles != NULL && trail->d1_tiles->len > 0) {
    guint n_d1 = MIN(trail->d1_tiles->len, 18u);
    double card_h = MIN(42.0, (avail_h - (double)(n_d1 > 1 ? n_d1 - 1 : 0) * 5.0) / (double)MAX(1u, n_d1));
    card_h = MAX(24.0, card_h);

    for (guint i = 0; i < n_d1; i++) {
      const IdeStripTile *t = g_ptr_array_index(trail->d1_tiles, i);
      double cy = top_y + (double)i * (card_h + 5.0);
      gboolean is_act = ((gint)i == trail->active_d1_idx);
      gboolean matched = overview_matches_query(t->name, t->rel_path, q_lower);
      if (matched) match_count++;
      g_autofree char *sub = g_strdup_printf("%s:%u", t->rel_path ? t->rel_path : "", t->start_line);

      draw_bezier_connector(cr, x_d0 + col_w, d0_mid_y, x_d1, cy + card_h * 0.5,
                            lr, lg, lb,
                            is_act ? 0.92 : 0.30,
                            is_act ? 1.8 : 1.0,
                            trail->using_fallback,
                            is_act);
      draw_overview_card(cr, font_desc, x_d1, cy, col_w, card_h,
                         ide_strip_tile_kind_badge(t->kind), t->name, sub,
                         lr, lg, lb, is_act, matched, has_filter);
      if (is_act) {
        active_d1_mid_y = cy + card_h * 0.5;
        have_active_d1 = TRUE;
      }
      OverviewHitbox hb = { x_d1, cy, col_w, card_h, 1, i };
      g_array_append_val(sv->overview_hitboxes, hb);
    }
  }

  /* Draw D+2 Cards (up to 18) */
  if (trail->d2_tiles != NULL && trail->d2_tiles->len > 0) {
    guint n_d2 = MIN(trail->d2_tiles->len, 18u);
    double card_h = MIN(38.0, (avail_h - (double)(n_d2 > 1 ? n_d2 - 1 : 0) * 5.0) / (double)MAX(1u, n_d2));
    card_h = MAX(22.0, card_h);

    for (guint i = 0; i < n_d2; i++) {
      const IdeStripTile *t = g_ptr_array_index(trail->d2_tiles, i);
      double cy = top_y + (double)i * (card_h + 5.0);
      gboolean is_act = ((gint)i == trail->active_d2_idx);
      gboolean matched = overview_matches_query(t->name, t->rel_path, q_lower);
      if (matched) match_count++;
      g_autofree char *sub = g_strdup_printf("%s:%u", t->rel_path ? t->rel_path : "", t->start_line);

      if (have_active_d1) {
        draw_bezier_connector(cr, x_d1 + col_w, active_d1_mid_y, x_d2, cy + card_h * 0.5,
                              lr, lg, lb,
                              is_act ? 0.85 : 0.28,
                              is_act ? 1.5 : 1.0,
                              trail->using_fallback,
                              is_act);
      }
      draw_overview_card(cr, font_desc, x_d2, cy, col_w, card_h,
                         ide_strip_tile_kind_badge(t->kind), t->name, sub,
                         lr, lg, lb, is_act, matched, has_filter);
      OverviewHitbox hb = { x_d2, cy, col_w, card_h, 2, i };
      g_array_append_val(sv->overview_hitboxes, hb);
    }
  }

  if (sv->overview_match_label != NULL) {
    if (has_filter) {
      g_autofree char *m = g_strdup_printf("%u matching", match_count);
      gtk_label_set_text(GTK_LABEL(sv->overview_match_label), m);
    } else {
      gtk_label_set_text(GTK_LABEL(sv->overview_match_label), "Click any card to focus");
    }
  }

  pango_font_description_free(font_desc);
  pango_font_description_free(hdr_desc);
}

static void
on_overview_canvas_pressed(GtkGestureClick *gesture G_GNUC_UNUSED,
                           int              n_press G_GNUC_UNUSED,
                           double           x,
                           double           y,
                           gpointer         user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL || sv->overview_hitboxes == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }

  for (guint i = 0; i < sv->overview_hitboxes->len; i++) {
    const OverviewHitbox *hb = &g_array_index(sv->overview_hitboxes, OverviewHitbox, i);
    if (x >= hb->x && x <= hb->x + hb->w && y >= hb->y && y <= hb->y + hb->h) {
      if (hb->col_type == -1) {
        /* Surface directly to clicked ancestor */
        if (trail->ancestors != NULL && hb->idx < trail->ancestors->len) {
          const IdeStripAncestor *anc = g_ptr_array_index(trail->ancestors, hb->idx);
          guint restore_line = anc != NULL ? anc->cursor_line : 1;
          if (ide_strip_trail_surface_to(trail, hb->idx, strip_now_sec())) {
            if (trail->root_tile != NULL) {
              open_and_attach_d0(sv, trail->root_tile->file_path, restore_line, TRUE);
            }
            ide_strip_view_toggle_overview(sv);
            refresh_d1_targets(sv);
            rebuild_all_ui(sv);
            ensure_spring_tick(sv);
          }
        }
      } else if (hb->col_type == 0) {
        ide_strip_view_toggle_overview(sv);
      } else if (hb->col_type == 1) {
        trail->active_d1_idx = (gint)hb->idx;
        ide_strip_view_toggle_overview(sv);
        ide_strip_view_promote_active_d1(sv);
      } else if (hb->col_type == 2) {
        trail->active_d2_idx = (gint)hb->idx;
        ide_strip_view_toggle_overview(sv);
        ide_strip_view_promote_active_d2(sv);
      }
      return;
    }
  }
}

/* ═══════════════════════════ Toolbar, Spine & D0 Header Builders ═══════════════════════════ */

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
    open_and_attach_d0(sv, trail->root_tile->file_path, trail->root_tile->start_line, TRUE);
  }
  rebuild_all_ui(sv);
  if (trail != NULL && (trail->d1_tiles == NULL || trail->d1_tiles->len == 0)) {
    refresh_d1_targets(sv);
  }
}

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

  if (!ide_strip_trail_surface_to(trail, anc_idx, strip_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    open_and_attach_d0(sv, trail->root_tile->file_path, restore_line, TRUE);
  }
  refresh_d1_targets(sv);
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
    sync_d1_active_visuals(sv);
    refresh_d2_targets(sv);
  }
  rebuild_all_ui(sv);
  ensure_spring_tick(sv);
}

static void
rebuild_toolbar(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);

  /* Trail switcher pills */
  clear_box_children(sv->trails_box);
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

static void
rebuild_spine(IdeStripView *sv)
{
  if (sv == NULL || sv->spine_box == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  clear_box_children(sv->spine_box);

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

static void
rebuild_d0_header_and_pin(IdeStripView *sv)
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
  schedule_paned_layout(sv);
}

/* ═══════════════════════════ D+1 & D+2 List Builders ═══════════════════════════ */

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

static void
sync_d1_active_visuals(IdeStripView *sv)
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

static void
rebuild_d1_list(IdeStripView *sv)
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
    gtk_widget_remove_css_class(sv->d1_col, lens_css_class((IdeStripLens)l));
  }
  gtk_widget_add_css_class(sv->d1_col, lens_css_class(trail->lens));

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
  sync_d1_active_visuals(sv);

  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
}

static void
rebuild_d2_list(IdeStripView *sv)
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

static void
rebuild_all_ui(IdeStripView *sv)
{
  if (sv == NULL) {
    return;
  }
  rebuild_toolbar(sv);
  rebuild_spine(sv);
  rebuild_d0_header_and_pin(sv);
  rebuild_d1_list(sv);
  rebuild_d2_list(sv);
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
  schedule_viewport_band_update(sv);
}

static void
on_d1_vadj_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv == NULL) {
    return;
  }
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
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
on_d2_vadj_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
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
    sync_d1_active_visuals(sv);
    refresh_d2_targets(sv);
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
  rebuild_d1_list(sv);
  refresh_d2_targets(sv);
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

static void
on_overview_filter_changed(GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->overview_canvas != NULL) {
    gtk_widget_queue_draw(sv->overview_canvas);
  }
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
on_resync_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_resync_d1((IdeStripView *)user_data);
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

/* ═══════════════════════════ Widget Construction ═══════════════════════════ */

static GtkWidget *
make_toolbar_btn(const char *label, const char *css_class, const char *tooltip)
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
make_bridge_spacer(void)
{
  GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(sp, (int)IDE_STRIP_BRIDGE_WIDTH, -1);
  gtk_widget_set_can_target(sp, FALSE);
  gtk_widget_add_css_class(sp, "ide-strip-bridge-spacer");
  return sp;
}

static void
build_ui(IdeStripView *sv)
{
  sv->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sv->root_box, "ide-strip-root");
  gtk_widget_set_hexpand(sv->root_box, TRUE);
  gtk_widget_set_vexpand(sv->root_box, TRUE);
  g_signal_connect(sv->root_box, "destroy", G_CALLBACK(on_root_box_destroy), sv);

  /* ── 1. Top Strip Toolbar (28px, non-widening horizontal strip) ── */
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

  sv->branch_btn = make_toolbar_btn("+ BRANCH", "ide-strip-tool-btn", "Branch vertical trail (Space t b)");
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
    GtkWidget *lb = make_toolbar_btn(kLensLabels[i], "ide-strip-lens-btn", NULL);
    gtk_widget_add_css_class(lb, lens_css_class((IdeStripLens)i));
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

  sv->zen_btn = make_toolbar_btn("⤢ ZEN", "ide-strip-tool-btn", "Toggle Zen focus (Space t z)");
  g_signal_connect(sv->zen_btn, "clicked", G_CALLBACK(on_zen_clicked), sv);
  gtk_box_append(GTK_BOX(right_bar), sv->zen_btn);

  sv->overview_btn = make_toolbar_btn("⬡ MAP", "ide-strip-tool-btn", "Bird's-eye graph overview (Space t o)");
  g_signal_connect(sv->overview_btn, "clicked", G_CALLBACK(on_overview_clicked), sv);
  gtk_box_append(GTK_BOX(right_bar), sv->overview_btn);

  sv->exit_btn = make_toolbar_btn("✕ TABS", "ide-strip-exit-btn", "Return to classic tab editor (Space t s)");
  g_signal_connect(sv->exit_btn, "clicked", G_CALLBACK(on_exit_clicked), sv);
  gtk_box_append(GTK_BOX(right_bar), sv->exit_btn);

  gtk_box_append(GTK_BOX(sv->toolbar), right_bar);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tb_scroll), sv->toolbar);
  gtk_box_append(GTK_BOX(sv->root_box), tb_scroll);

  /* ── 2. Main View Stack ("strip" vs "overview") ── */
  sv->view_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(sv->view_stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_hexpand(sv->view_stack, TRUE);
  gtk_widget_set_vexpand(sv->view_stack, TRUE);
  gtk_box_append(GTK_BOX(sv->root_box), sv->view_stack);

  /* ── Page "strip" ── */
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

  /* ── Column D0 ── */
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

  sv->pin_close_btn = make_toolbar_btn("×", "ide-strip-tile-btn", "Unpin reference");
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

  sv->d0_fullfile_btn = make_toolbar_btn("⤢ FULL FILE", "ide-strip-tool-btn",
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
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sv->d0_veil_overlay), draw_d0_veil, sv, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(sv->d0_editor_overlay), sv->d0_veil_overlay);

  gtk_box_append(GTK_BOX(sv->d0_main_box), sv->d0_editor_overlay);
  gtk_paned_set_end_child(GTK_PANED(sv->d0_vpaned), sv->d0_main_box);
  gtk_paned_set_start_child(GTK_PANED(sv->outer_paned), sv->d0_vpaned);

  /* ── Right Paned: [Spacer + D+1] | [Spacer + D+2] ── */
  sv->right_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_shrink_start_child(GTK_PANED(sv->right_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(sv->right_paned), TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(sv->right_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(sv->right_paned), TRUE);
  g_signal_connect(sv->right_paned, "notify::position",
                   G_CALLBACK(on_right_paned_pos_changed), sv);

  /* Column D+1 (12px bridge spacer + column body) */
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

  sv->d1_resync_btn = make_toolbar_btn("⛓ RESYNC", "ide-strip-resync-btn",
                                       "Re-engage magnetic viewport follow");
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
  gtk_paned_set_start_child(GTK_PANED(sv->right_paned), sv->d1_wrap);

  /* Column D+2 (12px bridge spacer + column body) */
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
  gtk_paned_set_end_child(GTK_PANED(sv->right_paned), sv->d2_wrap);

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
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sv->bridge_overlay), draw_bridges, sv, NULL);
  g_signal_connect(sv->bridge_overlay, "resize", G_CALLBACK(on_bridge_overlay_resize), sv);
  gtk_overlay_add_overlay(GTK_OVERLAY(sv->strip_overlay), sv->bridge_overlay);

  gtk_stack_add_named(GTK_STACK(sv->view_stack), sv->strip_overlay, "strip");

  /* ── Page "overview" ── */
  sv->overview_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sv->overview_page, "ide-strip-overview");

  GtkWidget *ov_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(ov_bar, "ide-strip-col-header");

  sv->overview_entry = gtk_entry_new();
  gtk_widget_add_css_class(sv->overview_entry, "ide-strip-filter");
  gtk_widget_set_hexpand(sv->overview_entry, TRUE);
  gtk_widget_set_valign(sv->overview_entry, GTK_ALIGN_CENTER);
  gtk_entry_set_placeholder_text(GTK_ENTRY(sv->overview_entry),
                                 "Filter symbols in overview...");
  g_signal_connect(sv->overview_entry, "changed", G_CALLBACK(on_overview_filter_changed), sv);
  gtk_box_append(GTK_BOX(ov_bar), sv->overview_entry);

  sv->overview_match_label = gtk_label_new("Click any card to focus");
  gtk_widget_set_valign(sv->overview_match_label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(sv->overview_match_label, "ide-strip-tile-loc");
  gtk_box_append(GTK_BOX(ov_bar), sv->overview_match_label);

  sv->overview_close_btn = make_toolbar_btn("✕ CLOSE OVERVIEW (Esc)", "ide-strip-tool-btn",
                                            "Return to Code Strip");
  g_signal_connect(sv->overview_close_btn, "clicked", G_CALLBACK(on_overview_clicked), sv);
  gtk_box_append(GTK_BOX(ov_bar), sv->overview_close_btn);
  gtk_box_append(GTK_BOX(sv->overview_page), ov_bar);

  sv->overview_canvas = gtk_drawing_area_new();
  gtk_widget_set_hexpand(sv->overview_canvas, TRUE);
  gtk_widget_set_vexpand(sv->overview_canvas, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sv->overview_canvas),
                                 draw_overview_canvas, sv, NULL);
  GtkGesture *ov_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(ov_click), GDK_BUTTON_PRIMARY);
  g_signal_connect(ov_click, "pressed", G_CALLBACK(on_overview_canvas_pressed), sv);
  gtk_widget_add_controller(sv->overview_canvas, GTK_EVENT_CONTROLLER(ov_click));
  gtk_box_append(GTK_BOX(sv->overview_page), sv->overview_canvas);

  gtk_stack_add_named(GTK_STACK(sv->view_stack), sv->overview_page, "overview");
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
  rebuild_toolbar(sv);
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
    refresh_d1_targets(sv);
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
    schedule_paned_layout(sv);
    if (sv->d0_buf != NULL) {
      refresh_d1_targets(sv);
    }
    rebuild_all_ui(sv);
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
    rebuild_all_ui(sv);
    return;
  }

  const char *path = ide_buffer_get_file_path(buf);
  g_autofree char *text = ide_buffer_get_text(buf);
  g_autofree char *rel = compute_rel_path(sv, path);
  guint line = cursor_line_1based > 0 ? cursor_line_1based : get_d0_cursor_line(sv);

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
  enrich_tile_from_open_buffer(sv, root);
  ide_strip_trail_set_root(trail, root, clear_ancestors);

  if (cursor_line_1based > 0) {
    scroll_source_view_to_line(sv->d0_source_view, cursor_line_1based, FALSE, 0.25);
  }

  rebuild_spine(sv);
  rebuild_d0_header_and_pin(sv);
  if (sv->model->strip_active) {
    refresh_d1_targets(sv);
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
    refresh_d1_targets(sv);
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
    schedule_viewport_band_update(sv);
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail != NULL && trail->lens == IDE_STRIP_LENS_DIFF) {
    refresh_d1_targets(sv);
  }
}

/* ═══════════════════════════ Actions & Navigation ═══════════════════════════ */

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
  refresh_d1_targets(sv);
  g_autofree char *msg = g_strdup_printf("Code Strip lens: %s", ide_strip_lens_name(lens));
  emit_status(sv, msg);
}

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
  rebuild_toolbar(sv);
  rebuild_spine(sv);
  rebuild_d0_header_and_pin(sv);
  if (sv->bridge_overlay != NULL) {
    gtk_widget_queue_draw(sv->bridge_overlay);
  }
  emit_status(sv, trail->zen_mode ? "Code Strip: Zen mode" : "Code Strip: 3-column mode");
}

void
ide_strip_view_toggle_overview(IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    return;
  }
  trail->overview_mode = !trail->overview_mode;
  rebuild_toolbar(sv);
  rebuild_d0_header_and_pin(sv);
  if (trail->overview_mode) {
    if (sv->overview_canvas != NULL) {
      gtk_widget_queue_draw(sv->overview_canvas);
    }
    if (sv->overview_entry != NULL) {
      gtk_widget_grab_focus(sv->overview_entry);
    }
  } else if (sv->d0_source_view != NULL) {
    gtk_widget_grab_focus(sv->d0_source_view);
  }
}

gboolean
ide_strip_view_is_overview(const IdeStripView *sv)
{
  if (sv == NULL || sv->model == NULL) {
    return FALSE;
  }
  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  return trail != NULL && trail->overview_mode;
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
  rebuild_d0_header_and_pin(sv);
  refresh_d1_targets(sv);
  emit_status(sv, trail->show_full_file ? "D0: Full file scope" : "D0: Symbol focus veil");
}

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
    pbuf = find_open_buffer_for_path(sv, tile->file_path);
  }

  sv->pin_buf = pbuf;
  if (pbuf != NULL && sv->pin_source_view != NULL) {
    GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(pbuf);
    if (sbuf != NULL) {
      gtk_text_view_set_buffer(GTK_TEXT_VIEW(sv->pin_source_view), GTK_TEXT_BUFFER(sbuf));
      scroll_source_view_to_line(sv->pin_source_view, tile->start_line, FALSE, 0.05);
    }
  }
  rebuild_d0_header_and_pin(sv);
  g_autofree char *msg = g_strdup_printf("Pinned: %s", tile->name != NULL ? tile->name : "");
  emit_status(sv, msg);
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
  rebuild_d0_header_and_pin(sv);
}

void
ide_strip_view_resync_d1(IdeStripView *sv)
{
  if (sv == NULL) {
    return;
  }
  run_viewport_band_update(sv, TRUE);
  emit_status(sv, "D+1 magnetic follow resynced");
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
  sync_d1_active_visuals(sv);
  refresh_d2_targets(sv);

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

  guint cur_line = get_d0_cursor_line(sv);
  if (!ide_strip_trail_promote_d1(trail, (guint)trail->active_d1_idx, cur_line, strip_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    open_and_attach_d0(sv, trail->root_tile->file_path, trail->root_tile->start_line, TRUE);
  }
  rebuild_spine(sv);
  rebuild_d0_header_and_pin(sv);
  refresh_d1_targets(sv);
  ensure_spring_tick(sv);
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

  guint cur_line = get_d0_cursor_line(sv);
  if (!ide_strip_trail_promote_d2(trail, (guint)trail->active_d2_idx, cur_line, strip_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    open_and_attach_d0(sv, trail->root_tile->file_path, trail->root_tile->start_line, TRUE);
  }
  rebuild_spine(sv);
  rebuild_d0_header_and_pin(sv);
  refresh_d1_targets(sv);
  ensure_spring_tick(sv);
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

  if (!ide_strip_trail_surface_one(trail, strip_now_sec())) {
    return;
  }
  if (trail->root_tile != NULL) {
    open_and_attach_d0(sv, trail->root_tile->file_path, restore_line, TRUE);
  }
  rebuild_spine(sv);
  rebuild_d0_header_and_pin(sv);
  refresh_d1_targets(sv);

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
    sync_d1_active_visuals(sv);
    refresh_d2_targets(sv);
  }
  ensure_spring_tick(sv);
  if (sv->d0_source_view != NULL) {
    gtk_widget_grab_focus(sv->d0_source_view);
  }
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
  rebuild_all_ui(sv);
  refresh_d1_targets(sv);
  g_autofree char *msg = g_strdup_printf("Branched trail %02u · %s",
                                         branched->id,
                                         branched->title != NULL ? branched->title : "Main");
  emit_status(sv, msg);
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
    open_and_attach_d0(sv, trail->root_tile->file_path, trail->root_tile->start_line, TRUE);
  }
  rebuild_all_ui(sv);
  if (trail != NULL && (trail->d1_tiles == NULL || trail->d1_tiles->len == 0)) {
    refresh_d1_targets(sv);
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
