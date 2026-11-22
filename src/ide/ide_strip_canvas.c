#include "ide_strip_view_private.h"

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

void
ide_strip_canvas_on_bridge_resize(GtkDrawingArea *area G_GNUC_UNUSED,
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
    ide_strip_view_schedule_paned_layout(sv);
  }
}

/* ═══════════════════════════ D0 Focus Veil Drawing ═══════════════════════════ */

void
ide_strip_canvas_draw_d0_veil(GtkDrawingArea *area,
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

void
ide_strip_canvas_draw_bridges(GtkDrawingArea *area,
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
draw_overview_grid_and_headers(cairo_t              *cr,
                               int                   width,
                               int                   height,
                               PangoFontDescription *hdr_desc,
                               const IdeStripTrail  *trail,
                               gboolean              has_anc,
                               double                x_anc,
                               double                x_d0,
                               double                x_d1,
                               double                x_d2)
{
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
}

void
ide_strip_canvas_draw_overview(GtkDrawingArea *area G_GNUC_UNUSED,
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

  IdeStripTrail *trail = ide_strip_model_get_active_trail(sv->model);
  if (trail == NULL) {
    cairo_set_source_rgb(cr, 0.067, 0.067, 0.067);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
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

  int ci = 0;
  double x_anc = 0.0;
  if (has_anc) {
    x_anc = pad_x + (double)ci++ * (col_w + gap_x);
  }
  double x_d0 = pad_x + (double)ci++ * (col_w + gap_x);
  double x_d1 = pad_x + (double)ci++ * (col_w + gap_x);
  double x_d2 = pad_x + (double)ci++ * (col_w + gap_x);

  draw_overview_grid_and_headers(cr, width, height, hdr_desc, trail,
                                 has_anc, x_anc, x_d0, x_d1, x_d2);

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

void
ide_strip_canvas_on_overview_pressed(GtkGestureClick *gesture G_GNUC_UNUSED,
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
          if (ide_strip_trail_surface_to(trail, hb->idx, ide_strip_view_now_sec())) {
            if (trail->root_tile != NULL) {
              ide_strip_view_open_and_attach_d0(sv, trail->root_tile->file_path, restore_line, TRUE);
            }
            ide_strip_view_toggle_overview(sv);
            ide_strip_tiles_refresh_d1(sv);
            ide_strip_view_rebuild_all_ui(sv);
            ide_strip_view_ensure_spring_tick(sv);
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

static void
on_overview_filter_changed(GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  IdeStripView *sv = user_data;
  if (sv != NULL && sv->overview_canvas != NULL) {
    gtk_widget_queue_draw(sv->overview_canvas);
  }
}

static void
on_overview_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ide_strip_view_toggle_overview((IdeStripView *)user_data);
}

GtkWidget *
ide_strip_canvas_build_overview_page(IdeStripView *sv)
{
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

  sv->overview_close_btn = ide_strip_view_make_toolbar_btn(
      "✕ CLOSE OVERVIEW (Esc)", "ide-strip-tool-btn", "Return to Code Strip");
  g_signal_connect(sv->overview_close_btn, "clicked", G_CALLBACK(on_overview_close_clicked), sv);
  gtk_box_append(GTK_BOX(ov_bar), sv->overview_close_btn);
  gtk_box_append(GTK_BOX(sv->overview_page), ov_bar);

  sv->overview_canvas = gtk_drawing_area_new();
  gtk_widget_set_hexpand(sv->overview_canvas, TRUE);
  gtk_widget_set_vexpand(sv->overview_canvas, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sv->overview_canvas),
                                 ide_strip_canvas_draw_overview, sv, NULL);
  GtkGesture *ov_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(ov_click), GDK_BUTTON_PRIMARY);
  g_signal_connect(ov_click, "pressed",
                   G_CALLBACK(ide_strip_canvas_on_overview_pressed), sv);
  gtk_widget_add_controller(sv->overview_canvas, GTK_EVENT_CONTROLLER(ov_click));
  gtk_box_append(GTK_BOX(sv->overview_page), sv->overview_canvas);
  return sv->overview_page;
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
  ide_strip_view_rebuild_toolbar(sv);
  ide_strip_view_rebuild_d0_header_and_pin(sv);
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
