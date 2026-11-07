/* Mermaid diagram engine for kajo-ide: parser, topological layout, Cairo/Pango
 * vector renderer and SVG/PNG exporters. Pure (no GTK widgets) so it is unit
 * testable. Split out of ide_markdown.c; the Markdown view embeds it. */
#include "ide_mermaid.h"
#include <cairo-svg.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

/* ─── 1. Mermaid Parser, Topological Layout & Cairo Vector Renderer ─── */

static void mermaid_node_free(gpointer data) {
  IdeMermaidNode *n = data;
  if (!n) return;
  g_free(n->id);
  g_free(n->label);
  g_free(n);
}

static void mermaid_edge_free(gpointer data) {
  IdeMermaidEdge *e = data;
  if (!e) return;
  g_free(e->from_id);
  g_free(e->to_id);
  g_free(e->label);
  g_free(e);
}

void ide_mermaid_graph_free(IdeMermaidGraph *graph) {
  if (!graph) return;
  g_free(graph->kind_badge);
  if (graph->nodes) g_ptr_array_free(graph->nodes, TRUE);
  if (graph->edges) g_ptr_array_free(graph->edges, TRUE);
  g_free(graph);
}

static IdeMermaidNode *ensure_mermaid_node(IdeMermaidGraph *graph,
                                           const char      *raw_token,
                                           guint            line_1based) {
  if (!raw_token || !*raw_token) return NULL;
  g_autofree char *trimmed = g_strstrip(g_strdup(raw_token));
  if (!*trimmed) return NULL;

  g_autofree char *id = NULL;
  g_autofree char *label = NULL;

  const char *open_br = strpbrk(trimmed, "[({");
  if (open_br && open_br > trimmed) {
    id = g_strndup(trimmed, open_br - trimmed);
    g_strstrip(id);
    const char *label_start = open_br + 1;
    if (*label_start == '"' || *label_start == '[' || *label_start == '(')
      label_start++;
    g_autofree char *raw_lbl = g_strdup(label_start);
    gsize len = strlen(raw_lbl);
    while (len > 0 && (raw_lbl[len - 1] == ']' || raw_lbl[len - 1] == ')' ||
                       raw_lbl[len - 1] == '}' || raw_lbl[len - 1] == '"')) {
      raw_lbl[--len] = '\0';
    }
    label = g_strdup(raw_lbl);
  } else {
    id = g_strdup(trimmed);
  }

  for (guint i = 0; i < graph->nodes->len; i++) {
    IdeMermaidNode *existing = g_ptr_array_index(graph->nodes, i);
    if (g_strcmp0(existing->id, id) == 0) {
      if (label && *label && g_strcmp0(existing->label, existing->id) == 0) {
        g_free(existing->label);
        existing->label = g_steal_pointer(&label);
      }
      return existing;
    }
  }

  IdeMermaidNode *node = g_new0(IdeMermaidNode, 1);
  node->id = g_strdup(id);
  node->label = label && *label ? g_steal_pointer(&label) : g_strdup(id);
  node->source_line_1based = line_1based;
  node->is_accent = (graph->nodes->len == 0);
  g_ptr_array_add(graph->nodes, node);
  return node;
}

IdeMermaidGraph *ide_mermaid_parse_graph(const char *mermaid_src,
                                         guint       block_start_line_1based) {
  IdeMermaidGraph *g = g_new0(IdeMermaidGraph, 1);
  g->kind = IDE_MERMAID_FLOWCHART_LR;
  g->kind_badge = g_strdup("FLOWCHART LR");
  g->nodes = g_ptr_array_new_with_free_func(mermaid_node_free);
  g->edges = g_ptr_array_new_with_free_func(mermaid_edge_free);

  if (!mermaid_src) return g;

  g_auto(GStrv) lines = g_strsplit(mermaid_src, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    guint cur_line = block_start_line_1based + i;
    g_autofree char *line = g_strstrip(g_strdup(lines[i]));
    if (!*line || g_str_has_prefix(line, "%%")) continue;

    if (g_str_has_prefix(line, "flowchart ") || g_str_has_prefix(line, "graph ")) {
      if (strstr(line, "TD") || strstr(line, "TB")) {
        g->kind = IDE_MERMAID_FLOWCHART_TD;
        g_free(g->kind_badge);
        g->kind_badge = g_strdup("FLOWCHART TD");
      } else {
        g->kind = IDE_MERMAID_FLOWCHART_LR;
        g_free(g->kind_badge);
        g->kind_badge = g_strdup("FLOWCHART LR");
      }
      continue;
    }
    if (g_str_has_prefix(line, "sequenceDiagram")) {
      g->kind = IDE_MERMAID_SEQUENCE;
      g_free(g->kind_badge);
      g->kind_badge = g_strdup("SEQUENCE DIAGRAM");
      continue;
    }
    if (g_str_has_prefix(line, "stateDiagram")) {
      g->kind = IDE_MERMAID_STATE;
      g_free(g->kind_badge);
      g->kind_badge = g_strdup("STATE DIAGRAM");
      continue;
    }
    if (g_str_has_prefix(line, "classDiagram")) {
      g->kind = IDE_MERMAID_CLASS;
      g_free(g->kind_badge);
      g->kind_badge = g_strdup("CLASS DIAGRAM");
      continue;
    }
    if (g_str_has_prefix(line, "subgraph ") || g_strcmp0(line, "end") == 0 ||
        g_str_has_prefix(line, "style ") || g_str_has_prefix(line, "classDef ")) {
      continue;
    }

    const char *arrow = NULL;
    gsize arrow_len = 0;
    gboolean dashed = FALSE;

    if ((arrow = strstr(line, "-.->")) != NULL) {
      arrow_len = 4;
      dashed = TRUE;
    } else if ((arrow = strstr(line, "-->>")) != NULL) {
      arrow_len = 4;
      dashed = TRUE;
    } else if ((arrow = strstr(line, "==>")) != NULL) {
      arrow_len = 3;
    } else if ((arrow = strstr(line, "-->")) != NULL) {
      arrow_len = 3;
    } else if ((arrow = strstr(line, "->>")) != NULL) {
      arrow_len = 3;
    } else if ((arrow = strstr(line, "->")) != NULL) {
      arrow_len = 2;
    }

    if (arrow) {
      g_autofree char *lhs = g_strndup(line, arrow - line);
      const char *rhs_raw = arrow + arrow_len;
      g_autofree char *edge_lbl = NULL;
      g_autofree char *rhs = NULL;

      while (*rhs_raw == ' ' || *rhs_raw == '\t') rhs_raw++;
      if (*rhs_raw == '|') {
        const char *end_pipe = strchr(rhs_raw + 1, '|');
        if (end_pipe) {
          edge_lbl = g_strndup(rhs_raw + 1, end_pipe - (rhs_raw + 1));
          rhs = g_strdup(end_pipe + 1);
        }
      }
      if (!rhs) {
        const char *colon = strchr(rhs_raw, ':');
        if (colon && (g->kind == IDE_MERMAID_SEQUENCE || g->kind == IDE_MERMAID_STATE)) {
          rhs = g_strndup(rhs_raw, colon - rhs_raw);
          edge_lbl = g_strstrip(g_strdup(colon + 1));
        } else {
          rhs = g_strdup(rhs_raw);
        }
      }

      IdeMermaidNode *src_node = ensure_mermaid_node(g, lhs, cur_line);
      IdeMermaidNode *dst_node = ensure_mermaid_node(g, rhs, cur_line);
      if (src_node && dst_node) {
        IdeMermaidEdge *e = g_new0(IdeMermaidEdge, 1);
        e->from_id = g_strdup(src_node->id);
        e->to_id = g_strdup(dst_node->id);
        e->label = edge_lbl ? g_steal_pointer(&edge_lbl) : NULL;
        e->dashed = dashed;
        g_ptr_array_add(g->edges, e);
      }
    } else if (g_str_has_prefix(line, "participant ")) {
      ensure_mermaid_node(g, line + strlen("participant "), cur_line);
    } else if (strpbrk(line, "[({") != NULL) {
      ensure_mermaid_node(g, line, cur_line);
    }
  }

  ide_mermaid_layout_graph(g);
  return g;
}

static IdeMermaidNode *find_node_by_id(const IdeMermaidGraph *g, const char *id) {
  if (!g || !id) return NULL;
  for (guint i = 0; i < g->nodes->len; i++) {
    IdeMermaidNode *n = g_ptr_array_index(g->nodes, i);
    if (g_strcmp0(n->id, id) == 0) return n;
  }
  return NULL;
}

void ide_mermaid_layout_graph(IdeMermaidGraph *graph) {
  if (!graph || graph->nodes->len == 0) {
    if (graph) {
      graph->canvas_width = 480;
      graph->canvas_height = 180;
    }
    return;
  }

  for (guint i = 0; i < graph->nodes->len; i++) {
    IdeMermaidNode *n = g_ptr_array_index(graph->nodes, i);
    n->rank = 0;
    n->order_in_rank = 0;
    gsize lbl_len = n->label ? g_utf8_strlen(n->label, -1) : 4;
    n->width = CLAMP((double)lbl_len * 8.5 + 32.0, 110.0, 240.0);
    n->height = 42.0;
  }

  guint max_iters = graph->nodes->len;
  for (guint iter = 0; iter < max_iters; iter++) {
    gboolean changed = FALSE;
    for (guint e_idx = 0; e_idx < graph->edges->len; e_idx++) {
      IdeMermaidEdge *e = g_ptr_array_index(graph->edges, e_idx);
      IdeMermaidNode *u = find_node_by_id(graph, e->from_id);
      IdeMermaidNode *v = find_node_by_id(graph, e->to_id);
      if (u && v && u != v && v->rank < u->rank + 1 &&
          u->rank + 1 < (int)graph->nodes->len) {
        v->rank = u->rank + 1;
        changed = TRUE;
      }
    }
    if (!changed) break;
  }

  int max_rank = 0;
  for (guint i = 0; i < graph->nodes->len; i++) {
    IdeMermaidNode *n = g_ptr_array_index(graph->nodes, i);
    if (n->rank > max_rank) max_rank = n->rank;
  }

  g_autofree int *rank_counts = g_new0(int, max_rank + 1);
  for (guint i = 0; i < graph->nodes->len; i++) {
    IdeMermaidNode *n = g_ptr_array_index(graph->nodes, i);
    n->order_in_rank = rank_counts[n->rank]++;
  }

  gboolean horizontal = (graph->kind == IDE_MERMAID_FLOWCHART_LR ||
                         graph->kind == IDE_MERMAID_SEQUENCE);
  double max_x = 320.0;
  double max_y = 160.0;

  for (guint i = 0; i < graph->nodes->len; i++) {
    IdeMermaidNode *n = g_ptr_array_index(graph->nodes, i);
    if (horizontal) {
      n->x = 36.0 + n->rank * 210.0;
      n->y = 36.0 + n->order_in_rank * 78.0;
    } else {
      n->x = 48.0 + n->order_in_rank * 220.0;
      n->y = 32.0 + n->rank * 86.0;
    }
    if (n->x + n->width + 40.0 > max_x) max_x = n->x + n->width + 40.0;
    if (n->y + n->height + 40.0 > max_y) max_y = n->y + n->height + 40.0;
  }

  graph->canvas_width = max_x;
  graph->canvas_height = max_y;
}

const IdeMermaidNode *ide_mermaid_hit_test_node(const IdeMermaidGraph *graph,
                                                double                 zoom,
                                                double                 px,
                                                double                 py) {
  if (!graph || zoom <= 0.0) return NULL;
  double gx = px / zoom;
  double gy = py / zoom;
  for (guint i = 0; i < graph->nodes->len; i++) {
    const IdeMermaidNode *n = g_ptr_array_index(graph->nodes, i);
    if (gx >= n->x && gx <= n->x + n->width &&
        gy >= n->y && gy <= n->y + n->height) {
      return n;
    }
  }
  return NULL;
}

void ide_mermaid_render_cairo(const IdeMermaidGraph *graph,
                              cairo_t               *cr,
                              double                 zoom,
                              double                 width,
                              double                 height) {
  cairo_save(cr);
  cairo_set_source_rgb(cr, 0.067, 0.067, 0.067); /* #111111 */
  cairo_paint(cr);

  cairo_set_source_rgb(cr, 0.13, 0.13, 0.13); /* #212121 dots */
  int iwidth = (int)width;
  int iheight = (int)height;
  for (int gx = 16; gx < iwidth; gx += 20) {
    for (int gy = 16; gy < iheight; gy += 20) {
      cairo_rectangle(cr, (double)gx, (double)gy, 1.5, 1.5);
    }
  }
  cairo_fill(cr);

  cairo_scale(cr, zoom, zoom);

  PangoFontDescription *font =
      pango_font_description_from_string("Fira Code SemiBold 9.5");
  PangoFontDescription *edge_font =
      pango_font_description_from_string("Fira Code 8");

  gboolean horizontal = (graph->kind == IDE_MERMAID_FLOWCHART_LR ||
                         graph->kind == IDE_MERMAID_SEQUENCE);

  for (guint i = 0; i < graph->edges->len; i++) {
    const IdeMermaidEdge *e = g_ptr_array_index(graph->edges, i);
    const IdeMermaidNode *u = find_node_by_id(graph, e->from_id);
    const IdeMermaidNode *v = find_node_by_id(graph, e->to_id);
    if (!u || !v) continue;

    double x1, y1, x2, y2, cx1, cy1, cx2, cy2;
    if (horizontal) {
      x1 = u->x + u->width;
      y1 = u->y + u->height * 0.5;
      x2 = v->x;
      y2 = v->y + v->height * 0.5;
      if (x2 < x1) {
        x1 = u->x;
        x2 = v->x + v->width;
      }
      double dx = fabs(x2 - x1) * 0.45;
      cx1 = x1 + (x2 >= x1 ? dx : -dx);
      cy1 = y1;
      cx2 = x2 - (x2 >= x1 ? dx : -dx);
      cy2 = y2;
    } else {
      x1 = u->x + u->width * 0.5;
      y1 = u->y + u->height;
      x2 = v->x + v->width * 0.5;
      y2 = v->y;
      double dy = fabs(y2 - y1) * 0.45;
      cx1 = x1;
      cy1 = y1 + dy;
      cx2 = x2;
      cy2 = y2 - dy;
    }

    if (e->dashed) {
      double dashes[] = { 5.0, 4.0 };
      cairo_set_dash(cr, dashes, 2, 0.0);
      cairo_set_source_rgb(cr, 0.0, 0.47, 0.83); /* #0078d4 */
    } else {
      cairo_set_dash(cr, NULL, 0, 0.0);
      cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    }

    cairo_set_line_width(cr, 1.6);
    cairo_move_to(cr, x1, y1);
    cairo_curve_to(cr, cx1, cy1, cx2, cy2, x2, y2);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0.0);

    double angle = atan2(y2 - cy2, x2 - cx2);
    double arrow_len = 8.0;
    cairo_move_to(cr, x2, y2);
    cairo_line_to(cr,
                  x2 - arrow_len * cos(angle - 0.45),
                  y2 - arrow_len * sin(angle - 0.45));
    cairo_line_to(cr,
                  x2 - arrow_len * cos(angle + 0.45),
                  y2 - arrow_len * sin(angle + 0.45));
    cairo_close_path(cr);
    cairo_fill(cr);

    if (e->label && *e->label) {
      double mx = (x1 + x2) * 0.5;
      double my = (y1 + y2) * 0.5 - 10.0;
      PangoLayout *elayout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(elayout, edge_font);
      pango_layout_set_text(elayout, e->label, -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(elayout, &tw, &th);

      cairo_set_source_rgb(cr, 0.09, 0.09, 0.09);
      cairo_rectangle(cr, mx - tw * 0.5 - 4, my - 2, tw + 8, th + 4);
      cairo_fill(cr);

      cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
      cairo_move_to(cr, mx - tw * 0.5, my);
      pango_cairo_show_layout(cr, elayout);
      g_object_unref(elayout);
    }
  }

  for (guint i = 0; i < graph->nodes->len; i++) {
    const IdeMermaidNode *n = g_ptr_array_index(graph->nodes, i);

    cairo_set_source_rgb(cr, 0.086, 0.086, 0.086); /* #161616 */
    cairo_rectangle(cr, n->x, n->y, n->width, n->height);
    cairo_fill_preserve(cr);

    if (n->is_accent)
      cairo_set_source_rgb(cr, 0.0, 0.47, 0.83); /* #0078d4 */
    else if (i == graph->nodes->len - 1 && graph->nodes->len > 1)
      cairo_set_source_rgb(cr, 0.063, 0.725, 0.506); /* #10b981 */
    else
      cairo_set_source_rgb(cr, 0.28, 0.28, 0.28);

    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, n->label, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);
    cairo_move_to(cr,
                  n->x + (n->width - tw) * 0.5,
                  n->y + (n->height - th) * 0.5);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
  }

  pango_font_description_free(font);
  pango_font_description_free(edge_font);
  cairo_restore(cr);
}

gboolean ide_mermaid_export_svg(const IdeMermaidGraph *graph,
                                const char            *out_svg_path) {
  if (!graph || !out_svg_path) return FALSE;
  double w = MAX(graph->canvas_width, 320.0);
  double h = MAX(graph->canvas_height, 160.0);
  cairo_surface_t *surf = cairo_svg_surface_create(out_svg_path, w, h);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return FALSE;
  }
  cairo_t *cr = cairo_create(surf);
  ide_mermaid_render_cairo(graph, cr, 1.0, w, h);
  cairo_destroy(cr);
  cairo_surface_flush(surf);
  cairo_surface_finish(surf);
  cairo_surface_destroy(surf);
  return TRUE;
}

gboolean ide_mermaid_export_png(const IdeMermaidGraph *graph,
                                const char            *out_png_path) {
  if (!graph || !out_png_path) return FALSE;
  int w = (int)MAX(graph->canvas_width, 320.0);
  int h = (int)MAX(graph->canvas_height, 160.0);
  cairo_surface_t *surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(surf);
  ide_mermaid_render_cairo(graph, cr, 1.0, w, h);
  cairo_destroy(cr);
  cairo_status_t st = cairo_surface_write_to_png(surf, out_png_path);
  cairo_surface_destroy(surf);
  return st == CAIRO_STATUS_SUCCESS;
}
