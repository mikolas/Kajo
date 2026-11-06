#include "ide_markdown.h"
#include "ide_buffer.h"
#include <cairo-svg.h>
#include <gtksourceview/gtksource.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

struct _IdeMarkdownView {
  IdeMarkdownJumpCb       jump_cb;
  IdeMarkdownBufferEditCb edit_cb;
  gpointer                user_data;

  GtkWidget              *root_box;
  GtkWidget              *scroll_win;
  GtkWidget              *doc_box;
  GtkWidget              *task_badge;
  char                   *current_markdown;
  gboolean                updating_checklists;
};

typedef struct {
  IdeMarkdownView *md;
  IdeMermaidGraph *graph;
  GtkWidget       *drawing_area;
  GtkWidget       *zoom_label;
  double           zoom;
} MermaidCardState;

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

  /* Parse ID and optional ["Label"] / (Label) / {Label} */
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

    /* Header line detection */
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
      g->kind_badge = (g_free(g->kind_badge), g_strdup("STATE DIAGRAM"));
      continue;
    }
    if (g_str_has_prefix(line, "classDiagram")) {
      g->kind = IDE_MERMAID_CLASS;
      g->kind_badge = (g_free(g->kind_badge), g_strdup("CLASS DIAGRAM"));
      continue;
    }
    if (g_str_has_prefix(line, "subgraph ") || g_strcmp0(line, "end") == 0 ||
        g_str_has_prefix(line, "style ") || g_str_has_prefix(line, "classDef ")) {
      continue;
    }

    /* Look for arrow operators: -.->, ==>, -->, -->>, ->> */
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

  /* 1. Assign topological ranks via longest path relaxation (bounded by N) */
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

  /* 2. Count nodes per rank and assign coordinates */
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

static void render_mermaid_cairo(const IdeMermaidGraph *graph,
                                 cairo_t               *cr,
                                 double                 zoom,
                                 double                 width,
                                 double                 height) {
  /* 1. Dark background + 20px subtle engineering dot grid */
  cairo_save(cr);
  cairo_set_source_rgb(cr, 0.067, 0.067, 0.067); /* #111111 */
  cairo_paint(cr);

  cairo_set_source_rgb(cr, 0.13, 0.13, 0.13); /* #212121 dots */
  for (double gx = 16.0; gx < width; gx += 20.0) {
    for (double gy = 16.0; gy < height; gy += 20.0) {
      cairo_rectangle(cr, gx, gy, 1.5, 1.5);
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

  /* 2. Draw Cubic Bezier edges & vector arrowheads */
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

    /* Vector arrowhead at (x2, y2) */
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

    /* Optional edge label badge */
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

  /* 3. Draw sharp Metro node boxes (`0px` radius) and centered Pango labels */
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
  render_mermaid_cairo(graph, cr, 1.0, w, h);
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
  render_mermaid_cairo(graph, cr, 1.0, w, h);
  cairo_destroy(cr);
  cairo_status_t st = cairo_surface_write_to_png(surf, out_png_path);
  cairo_surface_destroy(surf);
  return st == CAIRO_STATUS_SUCCESS;
}

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

static void mermaid_card_state_free(gpointer data) {
  MermaidCardState *st = data;
  if (!st) return;
  ide_mermaid_graph_free(st->graph);
  g_free(st);
}

static void on_mermaid_draw(GtkDrawingArea *area,
                            cairo_t        *cr,
                            int             width,
                            int             height,
                            gpointer        user_data) {
  (void)area;
  MermaidCardState *st = user_data;
  if (!st || !st->graph) return;
  render_mermaid_cairo(st->graph, cr, st->zoom, width, height);
}

static void update_mermaid_zoom(MermaidCardState *st, double new_zoom) {
  st->zoom = CLAMP(new_zoom, 0.5, 2.5);
  g_autofree char *lbl = g_strdup_printf("%d%%", (int)round(st->zoom * 100.0));
  gtk_label_set_text(GTK_LABEL(st->zoom_label), lbl);
  gtk_drawing_area_set_content_width(
      GTK_DRAWING_AREA(st->drawing_area),
      (int)(st->graph->canvas_width * st->zoom));
  gtk_drawing_area_set_content_height(
      GTK_DRAWING_AREA(st->drawing_area),
      (int)(st->graph->canvas_height * st->zoom));
  gtk_widget_queue_draw(st->drawing_area);
}

static void on_zoom_out_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  update_mermaid_zoom(st, st->zoom - 0.2);
}

static void on_zoom_reset_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  update_mermaid_zoom(st, 1.0);
}

static void on_zoom_in_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  update_mermaid_zoom(st, st->zoom + 0.2);
}

static void on_export_svg_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  g_autofree char *out =
      g_build_filename(g_get_tmp_dir(), "kajo-mermaid-diagram.svg", NULL);
  ide_mermaid_export_svg(st->graph, out);
}

static void on_mermaid_canvas_clicked(GtkGestureClick *gesture,
                                      int              n_press,
                                      double           x,
                                      double           y,
                                      gpointer         user_data) {
  (void)gesture;
  (void)n_press;
  MermaidCardState *st = user_data;
  if (!st || !st->graph || !st->md) return;
  const IdeMermaidNode *hit = ide_mermaid_hit_test_node(st->graph, st->zoom, x, y);
  if (hit && st->md->jump_cb && hit->source_line_1based > 0) {
    st->md->jump_cb(hit->source_line_1based, st->md->user_data);
  }
}

static GtkWidget *build_mermaid_card_widget(IdeMarkdownView *md,
                                            const char      *mermaid_code,
                                            guint            start_line_1based) {
  MermaidCardState *st = g_new0(MermaidCardState, 1);
  st->md = md;
  st->zoom = 1.0;
  st->graph = ide_mermaid_parse_graph(mermaid_code, start_line_1based);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");
  g_object_set_data_full(G_OBJECT(card),
                         "mermaid-state",
                         st,
                         mermaid_card_state_free);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("❖ MERMAID VECTOR DIAGRAM");
  gtk_widget_add_css_class(title, "ide-tree-header-title");
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

  GtkWidget *kind_lbl = gtk_label_new(st->graph->kind_badge);
  gtk_widget_add_css_class(kind_lbl, "shell-popover-badge");
  gtk_widget_set_valign(kind_lbl, GTK_ALIGN_CENTER);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  GtkWidget *btn_zout = gtk_button_new_with_label("🔍 −");
  gtk_widget_set_focusable(btn_zout, FALSE);
  gtk_widget_add_css_class(btn_zout, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_zout, GTK_ALIGN_CENTER);
  g_signal_connect(btn_zout, "clicked", G_CALLBACK(on_zoom_out_clicked), st);

  st->zoom_label = gtk_label_new("100%");
  gtk_widget_add_css_class(st->zoom_label, "shell-popover-badge");
  gtk_widget_add_css_class(st->zoom_label, "accent");
  gtk_widget_set_valign(st->zoom_label, GTK_ALIGN_CENTER);

  GtkWidget *btn_zin = gtk_button_new_with_label("🔍 +");
  gtk_widget_set_focusable(btn_zin, FALSE);
  gtk_widget_add_css_class(btn_zin, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_zin, GTK_ALIGN_CENTER);
  g_signal_connect(btn_zin, "clicked", G_CALLBACK(on_zoom_in_clicked), st);

  GtkWidget *btn_fit = gtk_button_new_with_label("⛶ FIT");
  gtk_widget_set_focusable(btn_fit, FALSE);
  gtk_widget_add_css_class(btn_fit, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_fit, GTK_ALIGN_CENTER);
  g_signal_connect(btn_fit, "clicked", G_CALLBACK(on_zoom_reset_clicked), st);

  GtkWidget *btn_svg = gtk_button_new_with_label("💾 EXPORT SVG");
  gtk_widget_set_focusable(btn_svg, FALSE);
  gtk_widget_add_css_class(btn_svg, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_svg, GTK_ALIGN_CENTER);
  g_signal_connect(btn_svg, "clicked", G_CALLBACK(on_export_svg_clicked), st);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), kind_lbl);
  gtk_box_append(GTK_BOX(hdr), spacer);
  gtk_box_append(GTK_BOX(hdr), btn_zout);
  gtk_box_append(GTK_BOX(hdr), st->zoom_label);
  gtk_box_append(GTK_BOX(hdr), btn_zin);
  gtk_box_append(GTK_BOX(hdr), btn_fit);
  gtk_box_append(GTK_BOX(hdr), btn_svg);
  gtk_box_append(GTK_BOX(card), hdr);

  st->drawing_area = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(st->drawing_area),
                                     (int)st->graph->canvas_width);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(st->drawing_area),
                                      (int)st->graph->canvas_height);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(st->drawing_area),
                                 on_mermaid_draw,
                                 st,
                                 NULL);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click,
                   "pressed",
                   G_CALLBACK(on_mermaid_canvas_clicked),
                   st);
  gtk_widget_add_controller(st->drawing_area, GTK_EVENT_CONTROLLER(click));

  GtkWidget *canvas_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(canvas_scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(canvas_scroll), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(canvas_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(canvas_scroll),
                                st->drawing_area);

  gtk_box_append(GTK_BOX(card), canvas_scroll);
  return card;
}

static char *format_inline_markdown_to_pango(const char *raw) {
  if (!raw) return g_strdup("");
  GString *out = g_string_new("");
  const char *p = raw;

  while (*p) {
    /* Inline `code` */
    if (*p == '`') {
      const char *end = strchr(p + 1, '`');
      if (end && end > p + 1) {
        g_autofree char *inner = g_strndup(p + 1, end - (p + 1));
        g_autofree char *esc = g_markup_escape_text(inner, -1);
        g_string_append_printf(
            out,
            "<span font_family=\"Fira Code\" background=\"#1f1f1f\" foreground=\"#7dd3fc\"> %s </span>",
            esc);
        p = end + 1;
        continue;
      }
    }
    /* Inline **bold** */
    if (p[0] == '*' && p[1] == '*') {
      const char *end = strstr(p + 2, "**");
      if (end && end > p + 2) {
        g_autofree char *inner = g_strndup(p + 2, end - (p + 2));
        g_autofree char *esc = g_markup_escape_text(inner, -1);
        g_string_append_printf(out, "<b>%s</b>", esc);
        p = end + 2;
        continue;
      }
    }
    /* Escape XML special chars */
    if (*p == '&') g_string_append(out, "&amp;");
    else if (*p == '<') g_string_append(out, "&lt;");
    else if (*p == '>') g_string_append(out, "&gt;");
    else g_string_append_c(out, *p);
    p++;
  }
  return g_string_free(out, FALSE);
}

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

static void on_checklist_toggled(GtkCheckButton *chk, gpointer user_data) {
  IdeMarkdownView *md = user_data;
  if (!md || md->updating_checklists || !md->current_markdown) return;
  guint line_1based =
      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(chk), "md-line"));
  if (line_1based == 0) return;

  char *updated =
      ide_markdown_toggle_checklist_line(md->current_markdown, line_1based);
  g_free(md->current_markdown);
  md->current_markdown = updated;
  if (md->edit_cb) md->edit_cb(md->current_markdown, md->user_data);
}

void ide_markdown_view_render(IdeMarkdownView *md, const char *markdown_text) {
  if (!md || !md->doc_box) return;

  g_free(md->current_markdown);
  md->current_markdown = g_strdup(markdown_text ? markdown_text : "");

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(md->doc_box)) != NULL)
    gtk_box_remove(GTK_BOX(md->doc_box), child);

  g_auto(GStrv) lines = g_strsplit(md->current_markdown, "\n", -1);
  guint n_lines = g_strv_length(lines);
  guint tasks_done = 0;
  guint tasks_total = 0;

  for (guint i = 0; i < n_lines; i++) {
    const char *line = lines[i];

    /* 1. Fenced code block or ```mermaid diagram */
    if (g_str_has_prefix(line, "```")) {
      g_autofree char *lang = g_strstrip(g_strdup(line + 3));
      guint block_start = i + 2; /* 1-based line inside block */
      GString *block = g_string_new("");
      i++;
      while (i < n_lines && !g_str_has_prefix(lines[i], "```")) {
        if (block->len > 0) g_string_append_c(block, '\n');
        g_string_append(block, lines[i]);
        i++;
      }
      if (g_ascii_strcasecmp(lang, "mermaid") == 0) {
        gtk_box_append(GTK_BOX(md->doc_box),
                       build_mermaid_card_widget(md, block->str, block_start));
      } else {
        gtk_box_append(GTK_BOX(md->doc_box),
                       build_code_block_widget(lang, block->str));
      }
      g_string_free(block, TRUE);
      continue;
    }

    /* 2. Headings (#, ##, ###) */
    if (g_str_has_prefix(line, "# ")) {
      GtkWidget *lbl = gtk_label_new(NULL);
      g_autofree char *mk = format_inline_markdown_to_pango(line + 2);
      gtk_label_set_markup(GTK_LABEL(lbl), mk);
      gtk_widget_add_css_class(lbl, "ide-md-h1");
      gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
      gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
      gtk_box_append(GTK_BOX(md->doc_box), lbl);
      continue;
    }
    if (g_str_has_prefix(line, "## ")) {
      GtkWidget *lbl = gtk_label_new(NULL);
      g_autofree char *mk = format_inline_markdown_to_pango(line + 3);
      gtk_label_set_markup(GTK_LABEL(lbl), mk);
      gtk_widget_add_css_class(lbl, "ide-md-h2");
      gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
      gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
      gtk_box_append(GTK_BOX(md->doc_box), lbl);
      continue;
    }
    if (g_str_has_prefix(line, "### ")) {
      GtkWidget *lbl = gtk_label_new(NULL);
      g_autofree char *mk = format_inline_markdown_to_pango(line + 4);
      gtk_label_set_markup(GTK_LABEL(lbl), mk);
      gtk_widget_add_css_class(lbl, "ide-md-h3");
      gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
      gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
      gtk_box_append(GTK_BOX(md->doc_box), lbl);
      continue;
    }

    /* 3. GitHub-Style Alert Callouts (> [!NOTE], > [!TIP], > [!WARNING], etc.) */
    if (g_str_has_prefix(line, "> ")) {
      const char *alert_cls = "ide-md-alert-note";
      const char *body = line + 2;
      if (strstr(line, "[!TIP]")) alert_cls = "ide-md-alert-tip";
      else if (strstr(line, "[!IMPORTANT]")) alert_cls = "ide-md-alert-important";
      else if (strstr(line, "[!WARNING]")) alert_cls = "ide-md-alert-warning";
      else if (strstr(line, "[!CAUTION]")) alert_cls = "ide-md-alert-caution";

      GString *quote = g_string_new(body);
      while (i + 1 < n_lines && g_str_has_prefix(lines[i + 1], "> ")) {
        i++;
        g_string_append_c(quote, '\n');
        g_string_append(quote, lines[i] + 2);
      }
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_add_css_class(box, "ide-md-alert");
      gtk_widget_add_css_class(box, alert_cls);

      GtkWidget *lbl = gtk_label_new(NULL);
      g_autofree char *mk = format_inline_markdown_to_pango(quote->str);
      gtk_label_set_markup(GTK_LABEL(lbl), mk);
      gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
      gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
      gtk_box_append(GTK_BOX(box), lbl);
      gtk_box_append(GTK_BOX(md->doc_box), box);
      g_string_free(quote, TRUE);
      continue;
    }

    /* 4. Interactive Checklists (- [ ] / - [x]) & Horizontal Rules (---) */
    const char *trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
    if (g_strcmp0(trimmed, "---") == 0 || g_strcmp0(trimmed, "***") == 0 ||
        g_strcmp0(trimmed, "___") == 0) {
      GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
      gtk_widget_add_css_class(sep, "ide-md-hr");
      gtk_box_append(GTK_BOX(md->doc_box), sep);
      continue;
    }

    if (g_str_has_prefix(trimmed, "- [ ] ") ||
        g_str_has_prefix(trimmed, "- [x] ") ||
        g_str_has_prefix(trimmed, "- [X] ")) {
      gboolean checked = (trimmed[3] == 'x' || trimmed[3] == 'X');
      tasks_total++;
      if (checked) tasks_done++;
      GtkWidget *chk = gtk_check_button_new_with_label(trimmed + 6);
      gtk_widget_set_focusable(chk, FALSE);
      md->updating_checklists = TRUE;
      gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), checked);
      md->updating_checklists = FALSE;
      g_object_set_data(G_OBJECT(chk), "md-line", GUINT_TO_POINTER(i + 1));
      g_signal_connect(chk, "toggled", G_CALLBACK(on_checklist_toggled), md);
      gtk_box_append(GTK_BOX(md->doc_box), chk);
      continue;
    }

    /* 5. Native GtkGrid Tables (| Col 1 | Col 2 |) */
    if (trimmed[0] == '|' && strchr(trimmed + 1, '|') != NULL) {
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
        if (strstr(tline, "---") == NULL) {
          g_auto(GStrv) cells = g_strsplit(tline, "|", -1);
          int col_idx = 0;
          for (guint c = 1; cells[c] != NULL && cells[c + 1] != NULL; c++) {
            g_autofree char *cell_txt = g_strstrip(g_strdup(cells[c]));
            GtkWidget *clbl = gtk_label_new(NULL);
            g_autofree char *mk = format_inline_markdown_to_pango(cell_txt);
            gtk_label_set_markup(GTK_LABEL(clbl), mk);
            gtk_label_set_xalign(GTK_LABEL(clbl), 0.0f);
            gtk_label_set_wrap(GTK_LABEL(clbl), TRUE);
            if (row_idx == 0) gtk_widget_add_css_class(clbl, "shell-popover-badge");
            gtk_grid_attach(GTK_GRID(grid), clbl, col_idx++, row_idx, 1, 1);
          }
          row_idx++;
        }
        i++;
      }
      gtk_box_append(GTK_BOX(md->doc_box), grid);
      continue;
    }

    /* 6. Regular Paragraph / Bullet Line */
    if (*trimmed != '\0') {
      GtkWidget *p = gtk_label_new(NULL);
      g_autofree char *mk = format_inline_markdown_to_pango(line);
      gtk_label_set_markup(GTK_LABEL(p), mk);
      gtk_label_set_xalign(GTK_LABEL(p), 0.0f);
      gtk_label_set_wrap(GTK_LABEL(p), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(p), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_selectable(GTK_LABEL(p), TRUE);
      gtk_box_append(GTK_BOX(md->doc_box), p);
    }
  }

  if (md->task_badge) {
    if (tasks_total > 0) {
      g_autofree char *tstr = g_strdup_printf("✓ %u / %u TASKS DONE", tasks_done, tasks_total);
      gtk_label_set_text(GTK_LABEL(md->task_badge), tstr);
      gtk_widget_set_visible(md->task_badge, TRUE);
    } else {
      gtk_widget_set_visible(md->task_badge, FALSE);
    }
  }
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
  gtk_widget_add_css_class(hdr, "ide-drawer-header");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title =
      gtk_label_new("📖 GRAPHICAL MARKDOWN & VECTOR MERMAID PREVIEW");
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

  md->task_badge = gtk_label_new("✓ 0 / 0 TASKS DONE");
  gtk_widget_add_css_class(md->task_badge, "shell-popover-badge");
  gtk_widget_add_css_class(md->task_badge, "success");
  gtk_widget_set_valign(md->task_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(md->task_badge, FALSE);

  GtkWidget *sync_badge = gtk_label_new("SYNC SCROLL: ON");
  gtk_widget_add_css_class(sync_badge, "shell-popover-badge");
  gtk_widget_set_valign(sync_badge, GTK_ALIGN_CENTER);

  GtkWidget *badge = gtk_label_new("CAIRO + PANGO GUI");
  gtk_widget_add_css_class(badge, "shell-popover-badge");
  gtk_widget_add_css_class(badge, "accent");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), md->task_badge);
  gtk_box_append(GTK_BOX(hdr), sync_badge);
  gtk_box_append(GTK_BOX(hdr), badge);
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
  g_free(md->current_markdown);
  g_free(md);
}
