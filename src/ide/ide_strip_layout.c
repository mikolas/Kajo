#include "ide_strip_layout.h"

#include <math.h>
#include <string.h>

/* Analytical spring parameters matching kajo-comp's feel:
 * - D+1 magnetic scroll: near-critically damped so tiles glide cleanly without
 *   overshooting past the aligned call line.
 * - Depth slide: matches kajo-comp's horizontal scroll spring (zeta=0.85, k=423). */
static const CompSpringParams k_d1_scroll_spring   = { 0.92, 480.0, 0.0001 };
static const CompSpringParams k_depth_slide_spring = { 0.85, 423.0, 0.0001 };

IdeStripTile *
ide_strip_tile_new(const char *id,
                   const char *name,
                   const char *file_path,
                   const char *rel_path,
                   guint start_line,
                   guint end_line,
                   guint call_line,
                   IdeStripTileKind kind)
{
  IdeStripTile *tile = g_new0(IdeStripTile, 1);
  if (id != NULL && *id != '\0') {
    tile->id = g_strdup(id);
  } else {
    const char *loc = (file_path != NULL && *file_path != '\0')
                        ? file_path
                        : (rel_path != NULL ? rel_path : "");
    tile->id = g_strdup_printf("%s:%u:%s", loc, start_line, name != NULL ? name : "");
  }
  tile->name = g_strdup(name);
  tile->file_path = g_strdup(file_path);
  tile->rel_path = g_strdup(rel_path);
  tile->start_line = start_line;
  tile->end_line = end_line >= start_line ? end_line : start_line;
  tile->call_line = call_line;
  tile->call_count = 1;
  tile->kind = kind;
  return tile;
}

IdeStripTile *
ide_strip_tile_copy(const IdeStripTile *src)
{
  if (src == NULL)
    return NULL;

  IdeStripTile *dst = g_new0(IdeStripTile, 1);
  *dst = *src;
  dst->id = g_strdup(src->id);
  dst->name = g_strdup(src->name);
  dst->file_path = g_strdup(src->file_path);
  dst->rel_path = g_strdup(src->rel_path);
  dst->peek_code = g_strdup(src->peek_code);
  return dst;
}

void
ide_strip_tile_free(gpointer p)
{
  IdeStripTile *tile = p;
  if (tile == NULL)
    return;
  g_free(tile->id);
  g_free(tile->name);
  g_free(tile->file_path);
  g_free(tile->rel_path);
  g_free(tile->peek_code);
  g_free(tile);
}

void
ide_strip_ancestor_free(gpointer p)
{
  IdeStripAncestor *anc = p;
  if (anc == NULL)
    return;
  ide_strip_tile_free(anc->tile);
  g_free(anc);
}

void
ide_strip_compute_geometry(double viewport_w,
                           guint depth,
                           gboolean zen_mode,
                           IdeStripGeometry *out)
{
  if (out == NULL)
    return;

  if (!isfinite(viewport_w) || viewport_w < 0.0)
    viewport_w = 0.0;

  if (zen_mode) {
    out->spine_w = 0.0;
    out->bridge_w = 0.0;
    out->zen_rail_w = MAX(IDE_STRIP_SPINE_WIDTH, floor(viewport_w * 0.04));
    out->d0_w = MAX(200.0, viewport_w - out->zen_rail_w);
    out->d1_w = 0.0;
    out->d2_w = 0.0;
    return;
  }

  out->spine_w = (depth > 0) ? IDE_STRIP_SPINE_WIDTH : 0.0;
  out->bridge_w = IDE_STRIP_BRIDGE_WIDTH;
  out->zen_rail_w = 0.0;

  double usable = viewport_w - out->spine_w - 2.0 * out->bridge_w;
  if (usable < 0.0)
    usable = 0.0;

  out->d0_w = floor(usable * 0.60);
  out->d1_w = floor(usable * 0.25);
  out->d2_w = usable - out->d0_w - out->d1_w;
}

static IdeStripTrail *
ide_strip_trail_new(guint id, const char *title, IdeStripTile *root_tile)
{
  IdeStripTrail *t = g_new0(IdeStripTrail, 1);
  t->id = id;
  if (title != NULL && *title != '\0')
    t->title = g_strdup(title);
  else if (root_tile != NULL && root_tile->name != NULL && *root_tile->name != '\0')
    t->title = g_strdup(root_tile->name);
  else
    t->title = g_strdup("Main");

  t->ancestors = g_ptr_array_new_with_free_func(ide_strip_ancestor_free);
  t->root_tile = root_tile;
  t->d1_tiles = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  t->d2_tiles = g_ptr_array_new_with_free_func(ide_strip_tile_free);
  t->active_d1_idx = -1;
  t->active_d2_idx = -1;
  t->lens = IDE_STRIP_LENS_CALLS;
  t->show_full_file = TRUE;
  return t;
}

static void
ide_strip_trail_free(gpointer p)
{
  IdeStripTrail *t = p;
  if (t == NULL)
    return;
  g_free(t->title);
  g_clear_pointer(&t->ancestors, g_ptr_array_unref);
  ide_strip_tile_free(t->root_tile);
  ide_strip_tile_free(t->pinned_tile);
  g_clear_pointer(&t->d1_tiles, g_ptr_array_unref);
  g_clear_pointer(&t->d2_tiles, g_ptr_array_unref);
  g_free(t->d1_filter);
  g_free(t);
}

static void
ide_strip_trail_reset(IdeStripTrail *t)
{
  g_free(t->title);
  t->title = g_strdup("Main");
  g_ptr_array_set_size(t->ancestors, 0);
  g_clear_pointer(&t->root_tile, ide_strip_tile_free);
  g_clear_pointer(&t->pinned_tile, ide_strip_tile_free);
  g_ptr_array_set_size(t->d1_tiles, 0);
  g_ptr_array_set_size(t->d2_tiles, 0);
  t->active_d1_idx = -1;
  t->active_d2_idx = -1;
  t->lens = IDE_STRIP_LENS_CALLS;
  t->zen_mode = FALSE;
  t->overview_mode = FALSE;
  t->show_full_file = TRUE;
  t->d1_unlinked = FALSE;
  t->using_fallback = FALSE;
  t->ext_suppressed_count = 0;
  g_clear_pointer(&t->d1_filter, g_free);
  t->d1_scroll_y = 0.0;
  t->d1_target_scroll_y = 0.0;
  t->d1_scroll_animating = FALSE;
  t->depth_slide_offset = 0.0;
  t->depth_slide_animating = FALSE;
}

IdeStripModel *
ide_strip_model_new(void)
{
  IdeStripModel *m = g_new0(IdeStripModel, 1);
  m->trails = g_ptr_array_new_with_free_func(ide_strip_trail_free);
  m->next_trail_id = 1;
  m->strip_active = FALSE;

  IdeStripTrail *initial = ide_strip_trail_new(m->next_trail_id++, "Main", NULL);
  g_ptr_array_add(m->trails, initial);
  m->active_trail_idx = 0;
  return m;
}

void
ide_strip_model_free(IdeStripModel *m)
{
  if (m == NULL)
    return;
  g_clear_pointer(&m->trails, g_ptr_array_unref);
  g_free(m);
}

IdeStripTrail *
ide_strip_model_get_active_trail(IdeStripModel *m)
{
  if (m == NULL || m->trails == NULL || m->trails->len == 0)
    return NULL;
  if (m->active_trail_idx >= m->trails->len)
    m->active_trail_idx = m->trails->len - 1;
  return g_ptr_array_index(m->trails, m->active_trail_idx);
}

IdeStripTrail *
ide_strip_model_add_trail(IdeStripModel *m,
                          const char *title,
                          IdeStripTile *root_tile)
{
  if (m == NULL) {
    ide_strip_tile_free(root_tile);
    return NULL;
  }

  IdeStripTrail *t = ide_strip_trail_new(m->next_trail_id++, title, root_tile);
  g_ptr_array_add(m->trails, t);
  m->active_trail_idx = m->trails->len - 1;
  return t;
}

IdeStripTrail *
ide_strip_model_branch_trail(IdeStripModel *m)
{
  if (m == NULL)
    return NULL;

  IdeStripTrail *cur = ide_strip_model_get_active_trail(m);
  IdeStripTile *root_copy = (cur != NULL) ? ide_strip_tile_copy(cur->root_tile) : NULL;
  const char *title = NULL;
  if (cur != NULL) {
    if (cur->root_tile != NULL && cur->root_tile->name != NULL && *cur->root_tile->name != '\0')
      title = cur->root_tile->name;
    else
      title = cur->title;
  }

  IdeStripTrail *branched = ide_strip_model_add_trail(m, title, root_copy);
  if (branched != NULL && cur != NULL) {
    branched->lens = cur->lens;
    branched->show_full_file = cur->show_full_file;
  }
  return branched;
}

gboolean
ide_strip_model_switch_trail(IdeStripModel *m, int delta)
{
  if (m == NULL || m->trails == NULL || m->trails->len <= 1 || delta == 0)
    return FALSE;

  gint n = (gint)m->trails->len;
  gint next = ((gint)m->active_trail_idx + (delta % n) + n) % n;
  if ((guint)next == m->active_trail_idx)
    return FALSE;

  m->active_trail_idx = (guint)next;
  return TRUE;
}

gboolean
ide_strip_model_select_trail(IdeStripModel *m, guint idx)
{
  if (m == NULL || m->trails == NULL || idx >= m->trails->len)
    return FALSE;

  m->active_trail_idx = idx;
  return TRUE;
}

gboolean
ide_strip_model_close_trail(IdeStripModel *m, guint idx)
{
  if (m == NULL || m->trails == NULL || idx >= m->trails->len)
    return FALSE;

  if (m->trails->len == 1) {
    IdeStripTrail *only = g_ptr_array_index(m->trails, 0);
    ide_strip_trail_reset(only);
    m->active_trail_idx = 0;
    return TRUE;
  }

  g_ptr_array_remove_index(m->trails, idx);
  if (idx < m->active_trail_idx)
    m->active_trail_idx--;
  else if (m->active_trail_idx >= m->trails->len)
    m->active_trail_idx = m->trails->len - 1;
  return TRUE;
}

void
ide_strip_trail_set_root(IdeStripTrail *t,
                         IdeStripTile *root_tile,
                         gboolean clear_ancestors)
{
  if (t == NULL) {
    ide_strip_tile_free(root_tile);
    return;
  }

  if (clear_ancestors)
    g_ptr_array_set_size(t->ancestors, 0);

  if (t->root_tile != root_tile) {
    ide_strip_tile_free(t->root_tile);
    t->root_tile = root_tile;
  }

  if (root_tile != NULL && root_tile->name != NULL && *root_tile->name != '\0' &&
      (t->title == NULL || *t->title == '\0' || g_strcmp0(t->title, "Main") == 0)) {
    char *next_title = g_strdup(root_tile->name);
    g_free(t->title);
    t->title = next_title;
  }

  t->d1_unlinked = FALSE;
  t->d1_scroll_y = 0.0;
  t->d1_target_scroll_y = 0.0;
  t->d1_scroll_animating = FALSE;
  g_clear_pointer(&t->d1_filter, g_free);
}

void
ide_strip_trail_set_d1_tiles(IdeStripTrail *t,
                             GPtrArray *tiles,
                             guint ext_suppressed,
                             gboolean using_fallback)
{
  if (t == NULL) {
    if (tiles != NULL)
      g_ptr_array_unref(tiles);
    return;
  }

  if (t->d1_tiles != tiles) {
    g_clear_pointer(&t->d1_tiles, g_ptr_array_unref);
    t->d1_tiles = tiles != NULL ? tiles : g_ptr_array_new_with_free_func(ide_strip_tile_free);
  }

  t->ext_suppressed_count = ext_suppressed;
  t->using_fallback = using_fallback;

  if (t->d1_tiles->len == 0) {
    t->active_d1_idx = -1;
  } else if (t->active_d1_idx < 0 || (guint)t->active_d1_idx >= t->d1_tiles->len) {
    t->active_d1_idx = 0;
  }
}

void
ide_strip_trail_set_d2_tiles(IdeStripTrail *t, GPtrArray *tiles)
{
  if (t == NULL) {
    if (tiles != NULL)
      g_ptr_array_unref(tiles);
    return;
  }

  if (t->d2_tiles != tiles) {
    g_clear_pointer(&t->d2_tiles, g_ptr_array_unref);
    t->d2_tiles = tiles != NULL ? tiles : g_ptr_array_new_with_free_func(ide_strip_tile_free);
  }

  if (t->d2_tiles->len == 0) {
    t->active_d2_idx = -1;
  } else if (t->active_d2_idx < 0 || (guint)t->active_d2_idx >= t->d2_tiles->len) {
    t->active_d2_idx = 0;
  }
}

gboolean
ide_strip_trail_promote_d1(IdeStripTrail *t,
                           guint d1_idx,
                           guint current_cursor_line,
                           double now_sec)
{
  if (t == NULL || t->d1_tiles == NULL || d1_idx >= t->d1_tiles->len)
    return FALSE;

  const IdeStripTile *target = g_ptr_array_index(t->d1_tiles, d1_idx);
  if (target == NULL)
    return FALSE;

  IdeStripTile *promoted = ide_strip_tile_copy(target);
  gint old_active_d1 = t->active_d1_idx;

  if (t->root_tile != NULL) {
    IdeStripAncestor *anc = g_new0(IdeStripAncestor, 1);
    anc->tile = t->root_tile;
    anc->cursor_line = current_cursor_line;
    anc->active_d1_idx = (gint)d1_idx;
    anc->d1_scroll_y = t->d1_scroll_y;
    anc->show_full_file = t->show_full_file;
    g_ptr_array_add(t->ancestors, anc);
    t->root_tile = NULL;
  }

  t->root_tile = promoted;

  /* Slide cached D+2 horizon left into D+1 when promoting the active D+1 tile. */
  if ((gint)d1_idx == old_active_d1 && t->d2_tiles != NULL && t->d2_tiles->len > 0) {
    g_ptr_array_unref(t->d1_tiles);
    t->d1_tiles = t->d2_tiles;
    t->d2_tiles = g_ptr_array_new_with_free_func(ide_strip_tile_free);
    t->active_d1_idx = 0;
    t->active_d2_idx = -1;
  } else {
    g_ptr_array_set_size(t->d1_tiles, 0);
    g_ptr_array_set_size(t->d2_tiles, 0);
    t->active_d1_idx = -1;
    t->active_d2_idx = -1;
  }

  t->show_full_file = FALSE;
  t->d1_unlinked = FALSE;
  g_clear_pointer(&t->d1_filter, g_free);
  t->d1_scroll_y = 0.0;
  t->d1_target_scroll_y = 0.0;
  t->d1_scroll_animating = FALSE;

  comp_spring_init(&t->depth_slide_spring, &k_depth_slide_spring, 1.0, 0.0, now_sec);
  t->depth_slide_offset = 1.0;
  t->depth_slide_animating = TRUE;
  return TRUE;
}

gboolean
ide_strip_trail_promote_d2(IdeStripTrail *t,
                           guint d2_idx,
                           guint current_cursor_line,
                           double now_sec)
{
  if (t == NULL || t->d2_tiles == NULL || d2_idx >= t->d2_tiles->len)
    return FALSE;

  const IdeStripTile *target = g_ptr_array_index(t->d2_tiles, d2_idx);
  if (target == NULL)
    return FALSE;

  IdeStripTile *promoted = ide_strip_tile_copy(target);

  if (t->root_tile != NULL) {
    IdeStripAncestor *anc0 = g_new0(IdeStripAncestor, 1);
    anc0->tile = t->root_tile;
    anc0->cursor_line = current_cursor_line;
    anc0->active_d1_idx = t->active_d1_idx;
    anc0->d1_scroll_y = t->d1_scroll_y;
    anc0->show_full_file = t->show_full_file;
    g_ptr_array_add(t->ancestors, anc0);
    t->root_tile = NULL;
  }

  if (t->d1_tiles != NULL &&
      t->active_d1_idx >= 0 &&
      (guint)t->active_d1_idx < t->d1_tiles->len) {
    const IdeStripTile *mid = g_ptr_array_index(t->d1_tiles, (guint)t->active_d1_idx);
    if (mid != NULL) {
      IdeStripAncestor *anc1 = g_new0(IdeStripAncestor, 1);
      anc1->tile = ide_strip_tile_copy(mid);
      anc1->cursor_line = mid->start_line;
      anc1->active_d1_idx = (gint)d2_idx;
      anc1->d1_scroll_y = 0.0;
      anc1->show_full_file = FALSE;
      g_ptr_array_add(t->ancestors, anc1);
    }
  }

  t->root_tile = promoted;
  g_ptr_array_set_size(t->d1_tiles, 0);
  g_ptr_array_set_size(t->d2_tiles, 0);
  t->active_d1_idx = -1;
  t->active_d2_idx = -1;

  t->show_full_file = FALSE;
  t->d1_unlinked = FALSE;
  g_clear_pointer(&t->d1_filter, g_free);
  t->d1_scroll_y = 0.0;
  t->d1_target_scroll_y = 0.0;
  t->d1_scroll_animating = FALSE;

  comp_spring_init(&t->depth_slide_spring, &k_depth_slide_spring, 1.0, 0.0, now_sec);
  t->depth_slide_offset = 1.0;
  t->depth_slide_animating = TRUE;
  return TRUE;
}

gboolean
ide_strip_trail_surface_to(IdeStripTrail *t,
                           guint ancestor_idx,
                           double now_sec)
{
  if (t == NULL || t->ancestors == NULL || ancestor_idx >= t->ancestors->len)
    return FALSE;

  IdeStripAncestor *anc = g_ptr_array_index(t->ancestors, ancestor_idx);
  IdeStripTile *restored = anc->tile;
  anc->tile = NULL;

  gboolean restored_full_file = anc->show_full_file;
  double restored_scroll_y = anc->d1_scroll_y;
  gint restored_active_d1 = anc->active_d1_idx;

  g_ptr_array_set_size(t->ancestors, ancestor_idx);

  ide_strip_tile_free(t->root_tile);
  t->root_tile = restored;

  g_ptr_array_set_size(t->d1_tiles, 0);
  g_ptr_array_set_size(t->d2_tiles, 0);
  t->active_d1_idx = restored_active_d1;
  t->active_d2_idx = -1;

  t->show_full_file = restored_full_file;
  t->d1_scroll_y = restored_scroll_y;
  t->d1_target_scroll_y = restored_scroll_y;
  t->d1_scroll_animating = FALSE;
  t->d1_unlinked = FALSE;
  g_clear_pointer(&t->d1_filter, g_free);

  comp_spring_init(&t->depth_slide_spring, &k_depth_slide_spring, -1.0, 0.0, now_sec);
  t->depth_slide_offset = -1.0;
  t->depth_slide_animating = TRUE;
  return TRUE;
}

gboolean
ide_strip_trail_surface_one(IdeStripTrail *t, double now_sec)
{
  if (t == NULL || t->ancestors == NULL || t->ancestors->len == 0)
    return FALSE;

  return ide_strip_trail_surface_to(t, t->ancestors->len - 1, now_sec);
}

void
ide_strip_trail_pin_tile(IdeStripTrail *t, const IdeStripTile *tile)
{
  if (t == NULL)
    return;

  IdeStripTile *copy = ide_strip_tile_copy(tile);
  ide_strip_tile_free(t->pinned_tile);
  t->pinned_tile = copy;
}

gboolean
ide_strip_trail_update_viewport_band(IdeStripTrail *t,
                                     guint vis_top_line,
                                     guint vis_bot_line,
                                     guint cursor_line,
                                     double d0_cursor_rel_y,
                                     double d1_viewport_h,
                                     gboolean magnetic_enabled,
                                     double now_sec)
{
  if (t == NULL || t->d1_tiles == NULL)
    return FALSE;

  if (vis_top_line > vis_bot_line) {
    guint tmp = vis_top_line;
    vis_top_line = vis_bot_line;
    vis_bot_line = tmp;
  }

  gboolean any_in_band = FALSE;
  for (guint i = 0; i < t->d1_tiles->len; i++) {
    IdeStripTile *tile = g_ptr_array_index(t->d1_tiles, i);
    gboolean in_band = (tile->call_line >= vis_top_line && tile->call_line <= vis_bot_line);
    tile->in_viewport_band = in_band;
    if (in_band)
      any_in_band = TRUE;
  }

  if (!magnetic_enabled || t->d1_unlinked || t->d1_tiles->len == 0)
    return FALSE;

  guint best_idx = 0;
  guint best_dist = G_MAXUINT;
  gboolean best_le_cursor = FALSE;
  gboolean found = FALSE;

  for (guint i = 0; i < t->d1_tiles->len; i++) {
    const IdeStripTile *tile = g_ptr_array_index(t->d1_tiles, i);
    if (any_in_band && !tile->in_viewport_band)
      continue;

    guint dist = (tile->call_line >= cursor_line)
                   ? (tile->call_line - cursor_line)
                   : (cursor_line - tile->call_line);
    gboolean le_cursor = (tile->call_line <= cursor_line);

    if (!found ||
        dist < best_dist ||
        (dist == best_dist && le_cursor && !best_le_cursor)) {
      best_idx = i;
      best_dist = dist;
      best_le_cursor = le_cursor;
      found = TRUE;
    }
  }

  if (!found)
    return FALSE;

  if (!isfinite(d0_cursor_rel_y))
    d0_cursor_rel_y = 0.0;
  if (!isfinite(d1_viewport_h) || d1_viewport_h < 0.0)
    d1_viewport_h = 0.0;

  double total_h = (double)t->d1_tiles->len * IDE_STRIP_D1_TILE_STRIDE + IDE_STRIP_D1_PEEK_EXTRA_H;
  double max_scroll = MAX(0.0, total_h - d1_viewport_h);
  double target_y = (double)best_idx * IDE_STRIP_D1_TILE_STRIDE - d0_cursor_rel_y + 16.0;
  target_y = CLAMP(target_y, 0.0, max_scroll);

  t->d1_target_scroll_y = target_y;
  if (t->d1_scroll_animating) {
    if (fabs(target_y - t->d1_scroll_spring.to) > 1e-6)
      comp_spring_retarget(&t->d1_scroll_spring, target_y, now_sec);
  } else if (fabs(target_y - t->d1_scroll_y) > 1e-6) {
    comp_spring_init(&t->d1_scroll_spring, &k_d1_scroll_spring, t->d1_scroll_y, target_y, now_sec);
    t->d1_scroll_animating = !comp_spring_is_done(&t->d1_scroll_spring, now_sec);
    if (!t->d1_scroll_animating)
      t->d1_scroll_y = target_y;
  } else {
    t->d1_scroll_y = target_y;
    t->d1_scroll_animating = FALSE;
  }

  gboolean changed = (t->active_d1_idx != (gint)best_idx);
  t->active_d1_idx = (gint)best_idx;
  return changed;
}

void
ide_strip_trail_manual_scroll_d1(IdeStripTrail *t, double scroll_y)
{
  if (t == NULL)
    return;

  if (!isfinite(scroll_y) || scroll_y < 0.0)
    scroll_y = 0.0;

  t->d1_unlinked = TRUE;
  t->d1_scroll_animating = FALSE;
  t->d1_scroll_y = scroll_y;
  t->d1_target_scroll_y = scroll_y;
}

gboolean
ide_strip_trail_resync_d1(IdeStripTrail *t,
                          guint vis_top_line,
                          guint vis_bot_line,
                          guint cursor_line,
                          double d0_cursor_rel_y,
                          double d1_viewport_h,
                          double now_sec)
{
  if (t == NULL)
    return FALSE;

  t->d1_unlinked = FALSE;
  return ide_strip_trail_update_viewport_band(t,
                                              vis_top_line,
                                              vis_bot_line,
                                              cursor_line,
                                              d0_cursor_rel_y,
                                              d1_viewport_h,
                                              TRUE,
                                              now_sec);
}

gboolean
ide_strip_trail_step_springs(IdeStripTrail *t, double now_sec)
{
  if (t == NULL)
    return FALSE;

  gboolean animating = FALSE;

  if (t->d1_scroll_animating) {
    if (comp_spring_is_done(&t->d1_scroll_spring, now_sec)) {
      t->d1_scroll_y = t->d1_scroll_spring.to;
      t->d1_scroll_animating = FALSE;
    } else {
      t->d1_scroll_y = comp_spring_value(&t->d1_scroll_spring, now_sec);
      animating = TRUE;
    }
  }

  if (t->depth_slide_animating) {
    if (comp_spring_is_done(&t->depth_slide_spring, now_sec)) {
      t->depth_slide_offset = t->depth_slide_spring.to;
      t->depth_slide_animating = FALSE;
    } else {
      t->depth_slide_offset = comp_spring_value(&t->depth_slide_spring, now_sec);
      animating = TRUE;
    }
  }

  return animating;
}

void
ide_strip_trail_set_filter(IdeStripTrail *t, const char *query)
{
  if (t == NULL)
    return;

  char *copy = (query != NULL && *query != '\0') ? g_strdup(query) : NULL;
  g_free(t->d1_filter);
  t->d1_filter = copy;
}

static gboolean
ascii_is_pure(const char *s)
{
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
    if (*p >= 0x80)
      return FALSE;
  }
  return TRUE;
}

static gboolean
ascii_contains_ci(const char *haystack, const char *needle_lower, gsize needle_len)
{
  if (haystack == NULL || *haystack == '\0')
    return FALSE;
  if (needle_len == 0)
    return TRUE;

  for (const char *h = haystack; *h != '\0'; h++) {
    gsize j = 0;
    while (j < needle_len && h[j] != '\0' &&
           g_ascii_tolower(h[j]) == needle_lower[j]) {
      j++;
    }
    if (j == needle_len)
      return TRUE;
  }
  return FALSE;
}

static gboolean
utf8_contains_ci(const char *haystack, const char *needle_folded)
{
  if (haystack == NULL || *haystack == '\0')
    return FALSE;
  g_autofree char *folded = g_utf8_casefold(haystack, -1);
  return strstr(folded, needle_folded) != NULL;
}

GArray *
ide_strip_trail_filtered_d1_indices(const IdeStripTrail *t)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(guint));
  if (t == NULL || t->d1_tiles == NULL || t->d1_tiles->len == 0)
    return out;

  if (t->d1_filter == NULL || *t->d1_filter == '\0') {
    g_array_set_size(out, t->d1_tiles->len);
    for (guint i = 0; i < t->d1_tiles->len; i++)
      g_array_index(out, guint, i) = i;
    return out;
  }

  gboolean is_ascii = ascii_is_pure(t->d1_filter);
  g_autofree char *needle = is_ascii
                              ? g_ascii_strdown(t->d1_filter, -1)
                              : g_utf8_casefold(t->d1_filter, -1);
  gsize needle_len = strlen(needle);

  for (guint i = 0; i < t->d1_tiles->len; i++) {
    const IdeStripTile *tile = g_ptr_array_index(t->d1_tiles, i);
    gboolean match = FALSE;
    if (is_ascii) {
      match = ascii_contains_ci(tile->name, needle, needle_len) ||
              ascii_contains_ci(tile->rel_path, needle, needle_len);
    } else {
      match = utf8_contains_ci(tile->name, needle) ||
              utf8_contains_ci(tile->rel_path, needle);
    }
    if (match)
      g_array_append_val(out, i);
  }

  return out;
}

const char *
ide_strip_lens_name(IdeStripLens lens)
{
  switch (lens) {
    case IDE_STRIP_LENS_CALLS:  return "CALLS";
    case IDE_STRIP_LENS_TYPES:  return "TYPES";
    case IDE_STRIP_LENS_IMPACT: return "IMPACT";
    case IDE_STRIP_LENS_DIFF:   return "DIFF";
    default:                    return "CALLS";
  }
}

const char *
ide_strip_tile_kind_badge(IdeStripTileKind kind)
{
  switch (kind) {
    case IDE_STRIP_TILE_FUNCTION: return "ƒ";
    case IDE_STRIP_TILE_TYPE:     return "T";
    case IDE_STRIP_TILE_FILE:     return "F";
    case IDE_STRIP_TILE_MACRO:    return "M";
    default:                      return "·";
  }
}
