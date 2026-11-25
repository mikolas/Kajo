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

static char *clean_mermaid_label(const char *raw) {
  if (!raw) return g_strdup("");
  GString *out = g_string_new("");
  const char *p = raw;
  while (*p) {
    if (g_ascii_strncasecmp(p, "<br/>", 5) == 0) {
      g_string_append_c(out, '\n');
      p += 5;
    } else if (g_ascii_strncasecmp(p, "<br />", 6) == 0) {
      g_string_append_c(out, '\n');
      p += 6;
    } else if (g_ascii_strncasecmp(p, "<br>", 4) == 0) {
      g_string_append_c(out, '\n');
      p += 4;
    } else {
      g_string_append_c(out, *p);
      p++;
    }
  }
  return g_string_free(out, FALSE);
}

static IdeMermaidNode *ensure_mermaid_node(IdeMermaidGraph *graph,
                                           const char      *raw_token,
                                           guint            line_1based) {
  if (!raw_token || !*raw_token) return NULL;
  g_autofree char *trimmed = g_strstrip(g_strdup(raw_token));
  if (!*trimmed) return NULL;

  g_autofree char *id = NULL;
  g_autofree char *label = NULL;

  /* Support `participant K as Kajo glass effect` alias syntax */
  const char *as_kw = strstr(trimmed, " as ");
  if (as_kw && strpbrk(trimmed, "[({") == NULL) {
    id = g_strstrip(g_strndup(trimmed, as_kw - trimmed));
    label = clean_mermaid_label(as_kw + 4);
    g_strstrip(label);
  } else {
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
      label = clean_mermaid_label(raw_lbl);
    } else {
      id = g_strdup(trimmed);
    }
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

static const char *find_mermaid_arrow(const char *s, gsize *out_len, gboolean *out_dashed) {
  int depth = 0;
  gboolean in_q = FALSE;
  for (const char *p = s; *p; p++) {
    if (*p == '"') {
      in_q = !in_q;
      continue;
    }
    if (in_q) continue;
    if (*p == '[' || *p == '(' || *p == '{') {
      depth++;
      continue;
    }
    if ((*p == ']' || *p == ')' || *p == '}') && depth > 0) {
      depth--;
      continue;
    }
    if (depth > 0) continue;

    if (g_str_has_prefix(p, "-.->")) { *out_len = 4; *out_dashed = TRUE;  return p; }
    if (g_str_has_prefix(p, "-->>")) { *out_len = 4; *out_dashed = TRUE;  return p; }
    if (g_str_has_prefix(p, "==>"))  { *out_len = 3; *out_dashed = FALSE; return p; }
    if (g_str_has_prefix(p, "-->"))  { *out_len = 3; *out_dashed = FALSE; return p; }
    if (g_str_has_prefix(p, "->>"))  { *out_len = 3; *out_dashed = FALSE; return p; }
    if (g_str_has_prefix(p, "->"))   { *out_len = 2; *out_dashed = FALSE; return p; }
  }
  return NULL;
}

static GPtrArray *split_ampersand_tokens(const char *expr) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  if (!expr) return arr;
  int depth = 0;
  gboolean in_q = FALSE;
  const char *start = expr;
  for (const char *p = expr; ; p++) {
    if (*p == '"') in_q = !in_q;
    else if (!in_q) {
      if (*p == '[' || *p == '(' || *p == '{') depth++;
      else if ((*p == ']' || *p == ')' || *p == '}') && depth > 0) depth--;
    }
    if (*p == '\0' || (*p == '&' && !in_q && depth == 0)) {
      char *tok = g_strstrip(g_strndup(start, p - start));
      if (*tok) g_ptr_array_add(arr, tok);
      else g_free(tok);
      if (*p == '\0') break;
      start = p + 1;
    }
  }
  return arr;
}

static void add_mermaid_edges_between(IdeMermaidGraph *g,
                                      const char      *lhs_expr,
                                      const char      *rhs_expr,
                                      const char      *edge_lbl,
                                      gboolean         dashed,
                                      guint            cur_line) {
  g_autoptr(GPtrArray) srcs = split_ampersand_tokens(lhs_expr);
  g_autoptr(GPtrArray) dsts = split_ampersand_tokens(rhs_expr);
  for (guint si = 0; si < srcs->len; si++) {
    IdeMermaidNode *src_node = ensure_mermaid_node(g, g_ptr_array_index(srcs, si), cur_line);
    if (!src_node) continue;
    for (guint di = 0; di < dsts->len; di++) {
      IdeMermaidNode *dst_node = ensure_mermaid_node(g, g_ptr_array_index(dsts, di), cur_line);
      if (!dst_node) continue;
      IdeMermaidEdge *e = g_new0(IdeMermaidEdge, 1);
      e->from_id = g_strdup(src_node->id);
      e->to_id = g_strdup(dst_node->id);
      e->label = (edge_lbl && *edge_lbl) ? clean_mermaid_label(edge_lbl) : NULL;
      e->dashed = dashed;
      g_ptr_array_add(g->edges, e);
    }
  }
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
        g_str_has_prefix(line, "style ") || g_str_has_prefix(line, "classDef ") ||
        g_str_has_prefix(line, "Note ") || g_str_has_prefix(line, "note ")) {
      continue;
    }

    gsize arrow_len = 0;
    gboolean dashed = FALSE;
    const char *arrow = find_mermaid_arrow(line, &arrow_len, &dashed);

    if (arrow) {
      const char *seg_start = line;
      while (arrow) {
        g_autofree char *lhs = g_strstrip(g_strndup(seg_start, arrow - seg_start));
        const char *rhs_raw = arrow + arrow_len;
        g_autofree char *edge_lbl = NULL;

        /* Handle mid-arrow label syntax: `A -- "label" --> B` */
        char *mid = strstr(lhs, " -- ");
        if (mid) {
          g_autofree char *lbl_part = g_strstrip(g_strdup(mid + 4));
          gsize ll = strlen(lbl_part);
          if (ll >= 2 && lbl_part[0] == '"' && lbl_part[ll - 1] == '"') {
            lbl_part[ll - 1] = '\0';
            edge_lbl = g_strdup(lbl_part + 1);
          } else {
            edge_lbl = g_strdup(lbl_part);
          }
          *mid = '\0';
          g_strstrip(lhs);
        }

        while (*rhs_raw == ' ' || *rhs_raw == '\t') rhs_raw++;
        if (*rhs_raw == '|') {
          const char *end_pipe = strchr(rhs_raw + 1, '|');
          if (end_pipe) {
            g_free(edge_lbl);
            edge_lbl = g_strstrip(g_strndup(rhs_raw + 1, end_pipe - (rhs_raw + 1)));
            gsize ll = strlen(edge_lbl);
            if (ll >= 2 && edge_lbl[0] == '"' && edge_lbl[ll - 1] == '"') {
              edge_lbl[ll - 1] = '\0';
              char *unq = g_strdup(edge_lbl + 1);
              g_free(edge_lbl);
              edge_lbl = unq;
            }
            rhs_raw = end_pipe + 1;
            while (*rhs_raw == ' ' || *rhs_raw == '\t') rhs_raw++;
          }
        }

        gsize next_len = 0;
        gboolean next_dashed = FALSE;
        const char *next_arrow = (g->kind == IDE_MERMAID_SEQUENCE || g->kind == IDE_MERMAID_STATE)
                                     ? NULL
                                     : find_mermaid_arrow(rhs_raw, &next_len, &next_dashed);
        g_autofree char *rhs = NULL;
        if (next_arrow) {
          rhs = g_strstrip(g_strndup(rhs_raw, next_arrow - rhs_raw));
          /* Strip trailing ` -- "label"` belonging to next_arrow */
          char *next_mid = strstr(rhs, " -- ");
          if (next_mid) {
            *next_mid = '\0';
            g_strstrip(rhs);
          }
        } else {
          const char *colon = strchr(rhs_raw, ':');
          if (colon && (g->kind == IDE_MERMAID_SEQUENCE || g->kind == IDE_MERMAID_STATE)) {
            rhs = g_strstrip(g_strndup(rhs_raw, colon - rhs_raw));
            g_free(edge_lbl);
            edge_lbl = g_strstrip(g_strdup(colon + 1));
          } else {
            rhs = g_strstrip(g_strdup(rhs_raw));
          }
        }

        add_mermaid_edges_between(g, lhs, rhs, edge_lbl, dashed, cur_line);

        seg_start = rhs_raw;
        arrow = next_arrow;
        arrow_len = next_len;
        dashed = next_dashed;
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

typedef struct {
  double          bary;
  int             orig;
  IdeMermaidNode *node;
} MermaidBaryItem;

static void sort_rank_by_barycenter(const IdeMermaidGraph *g,
                                    GPtrArray             *nodes,
                                    gboolean               by_predecessors) {
  if (!nodes || nodes->len <= 1) return;
  guint len = nodes->len;
  g_autofree MermaidBaryItem *items = g_new0(MermaidBaryItem, len);

  for (guint i = 0; i < len; i++) {
    IdeMermaidNode *n = g_ptr_array_index(nodes, i);
    double sum = 0.0;
    int cnt = 0;
    for (guint e_idx = 0; e_idx < g->edges->len; e_idx++) {
      const IdeMermaidEdge *e = g_ptr_array_index(g->edges, e_idx);
      if (by_predecessors) {
        if (g_strcmp0(e->to_id, n->id) == 0) {
          const IdeMermaidNode *u = find_node_by_id(g, e->from_id);
          if (u && u->rank < n->rank) {
            sum += (double)u->order_in_rank;
            cnt++;
          }
        }
      } else {
        if (g_strcmp0(e->from_id, n->id) == 0) {
          const IdeMermaidNode *v = find_node_by_id(g, e->to_id);
          if (v && v->rank > n->rank) {
            sum += (double)v->order_in_rank;
            cnt++;
          }
        }
      }
    }
    items[i].bary = (cnt > 0) ? (sum / (double)cnt) : (double)n->order_in_rank;
    items[i].orig = (int)i;
    items[i].node = n;
  }

  for (guint i = 1; i < len; i++) {
    MermaidBaryItem key = items[i];
    guint j = i;
    while (j > 0 &&
           (items[j - 1].bary > key.bary + 1e-6 ||
            (fabs(items[j - 1].bary - key.bary) <= 1e-6 &&
             items[j - 1].orig > key.orig))) {
      items[j] = items[j - 1];
      j--;
    }
    items[j] = key;
  }

  for (guint i = 0; i < len; i++) {
    nodes->pdata[i] = items[i].node;
    items[i].node->order_in_rank = (int)i;
  }
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

    guint max_line_chars = 4;
    guint wrapped_lines = 1;
    if (n->label && *n->label) {
      g_auto(GStrv) lines = g_strsplit(n->label, "\n", -1);
      max_line_chars = 0;
      wrapped_lines = 0;
      for (guint li = 0; lines[li] != NULL; li++) {
        guint len = (guint)g_utf8_strlen(lines[li], -1);
        if (len > max_line_chars) max_line_chars = len;
        wrapped_lines += MAX(1u, (len + 25u) / 26u);
      }
      if (max_line_chars < 4) max_line_chars = 4;
      if (wrapped_lines == 0) wrapped_lines = 1;
    }

    n->width = CLAMP((double)MIN(max_line_chars, 26u) * 8.2 + 32.0, 112.0, 248.0);
    n->height = MAX(42.0, (double)wrapped_lines * 18.0 + 20.0);
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

  GPtrArray **rank_lists = g_new0(GPtrArray *, max_rank + 1);
  for (int r = 0; r <= max_rank; r++) {
    rank_lists[r] = g_ptr_array_new();
  }
  for (guint i = 0; i < graph->nodes->len; i++) {
    IdeMermaidNode *n = g_ptr_array_index(graph->nodes, i);
    n->order_in_rank = (int)rank_lists[n->rank]->len;
    g_ptr_array_add(rank_lists[n->rank], n);
  }

  /* 4 alternating forward/backward barycenter sweeps to reduce crossings */
  for (int pass = 0; pass < 2; pass++) {
    for (int r = 1; r <= max_rank; r++) {
      sort_rank_by_barycenter(graph, rank_lists[r], TRUE);
    }
    for (int r = max_rank - 1; r >= 0; r--) {
      sort_rank_by_barycenter(graph, rank_lists[r], FALSE);
    }
  }

  const double PAD_X = 36.0;
  const double PAD_Y = 32.0;
  const double MAIN_GAP = 60.0;
  const double CROSS_GAP = 28.0;

  g_autofree double *gap_after = g_new0(double, max_rank + 1);
  for (int r = 0; r <= max_rank; r++) {
    gap_after[r] = MAIN_GAP;
  }
  for (guint e_idx = 0; e_idx < graph->edges->len; e_idx++) {
    const IdeMermaidEdge *e = g_ptr_array_index(graph->edges, e_idx);
    if (!e->label || !*e->label) continue;
    const IdeMermaidNode *u = find_node_by_id(graph, e->from_id);
    const IdeMermaidNode *v = find_node_by_id(graph, e->to_id);
    if (u && v && u->rank < v->rank && u->rank <= max_rank) {
      gap_after[u->rank] = MAIN_GAP + 22.0;
    }
  }

  gboolean horizontal = (graph->kind == IDE_MERMAID_FLOWCHART_LR ||
                         graph->kind == IDE_MERMAID_SEQUENCE);
  double max_x = 320.0;
  double max_y = 160.0;

  if (!horizontal) {
    g_autofree double *rank_h = g_new0(double, max_rank + 1);
    g_autofree double *rank_total_w = g_new0(double, max_rank + 1);
    double max_span = 0.0;

    for (int r = 0; r <= max_rank; r++) {
      GPtrArray *rl = rank_lists[r];
      double tot = 0.0;
      double mh = 42.0;
      for (guint k = 0; k < rl->len; k++) {
        IdeMermaidNode *n = g_ptr_array_index(rl, k);
        if (k > 0) tot += CROSS_GAP;
        tot += n->width;
        if (n->height > mh) mh = n->height;
      }
      rank_h[r] = mh;
      rank_total_w[r] = tot;
      if (tot > max_span) max_span = tot;
    }

    double y_cur = PAD_Y;
    for (int r = 0; r <= max_rank; r++) {
      GPtrArray *rl = rank_lists[r];
      double x_cur = PAD_X + (max_span - rank_total_w[r]) * 0.5;
      for (guint k = 0; k < rl->len; k++) {
        IdeMermaidNode *n = g_ptr_array_index(rl, k);
        n->x = x_cur;
        n->y = y_cur + (rank_h[r] - n->height) * 0.5;
        x_cur += n->width + CROSS_GAP;
      }
      y_cur += rank_h[r] + (r < max_rank ? gap_after[r] : 0.0);
    }
    max_x = PAD_X + max_span;
    max_y = y_cur;
  } else {
    g_autofree double *rank_w = g_new0(double, max_rank + 1);
    g_autofree double *rank_total_h = g_new0(double, max_rank + 1);
    double max_span = 0.0;

    for (int r = 0; r <= max_rank; r++) {
      GPtrArray *rl = rank_lists[r];
      double tot = 0.0;
      double mw = 112.0;
      for (guint k = 0; k < rl->len; k++) {
        IdeMermaidNode *n = g_ptr_array_index(rl, k);
        if (k > 0) tot += CROSS_GAP;
        tot += n->height;
        if (n->width > mw) mw = n->width;
      }
      rank_w[r] = mw;
      rank_total_h[r] = tot;
      if (tot > max_span) max_span = tot;
    }

    double x_cur = PAD_X;
    for (int r = 0; r <= max_rank; r++) {
      GPtrArray *rl = rank_lists[r];
      double y_cur = PAD_Y + (max_span - rank_total_h[r]) * 0.5;
      for (guint k = 0; k < rl->len; k++) {
        IdeMermaidNode *n = g_ptr_array_index(rl, k);
        n->x = x_cur + (rank_w[r] - n->width) * 0.5;
        n->y = y_cur;
        y_cur += n->height + CROSS_GAP;
      }
      x_cur += rank_w[r] + (r < max_rank ? gap_after[r] : 0.0);
    }
    max_x = x_cur;
    max_y = PAD_Y + max_span;
  }

  for (int r = 0; r <= max_rank; r++) {
    g_ptr_array_free(rank_lists[r], TRUE);
  }
  g_free(rank_lists);

  graph->canvas_width = MAX(320.0, max_x + PAD_X);
  graph->canvas_height = MAX(160.0, max_y + PAD_Y);
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

  double offset_x = MAX(0.0, (width - graph->canvas_width * zoom) * 0.5);
  cairo_translate(cr, offset_x, 0.0);
  cairo_scale(cr, zoom, zoom);

  PangoFontDescription *font =
      pango_font_description_from_string("Fira Code SemiBold 9.5");
  PangoFontDescription *edge_font =
      pango_font_description_from_string("Fira Code 8");

  gboolean horizontal = (graph->kind == IDE_MERMAID_FLOWCHART_LR ||
                         graph->kind == IDE_MERMAID_SEQUENCE);
  double graph_mid_cross = horizontal
      ? (graph->canvas_height * 0.5)
      : (graph->canvas_width * 0.5);

  for (guint i = 0; i < graph->edges->len; i++) {
    const IdeMermaidEdge *e = g_ptr_array_index(graph->edges, i);
    const IdeMermaidNode *u = find_node_by_id(graph, e->from_id);
    const IdeMermaidNode *v = find_node_by_id(graph, e->to_id);
    if (!u || !v) continue;

    /* Compute spread port slots ordered by opposite endpoint cross coordinate */
    double v_cross = horizontal ? (v->y + v->height * 0.5) : (v->x + v->width * 0.5);
    double u_cross = horizontal ? (u->y + u->height * 0.5) : (u->x + u->width * 0.5);
    int out_cnt = 0, out_slot = 0;
    int in_cnt = 0, in_slot = 0;
    for (guint j = 0; j < graph->edges->len; j++) {
      const IdeMermaidEdge *ej = g_ptr_array_index(graph->edges, j);
      const IdeMermaidNode *uj = find_node_by_id(graph, ej->from_id);
      const IdeMermaidNode *vj = find_node_by_id(graph, ej->to_id);
      if (!uj || !vj) continue;
      if (uj == u) {
        out_cnt++;
        if (j != i) {
          double vj_cross = horizontal ? (vj->y + vj->height * 0.5) : (vj->x + vj->width * 0.5);
          if (vj_cross < v_cross - 1e-6 || (fabs(vj_cross - v_cross) <= 1e-6 && j < i))
            out_slot++;
        }
      }
      if (vj == v) {
        in_cnt++;
        if (j != i) {
          double uj_cross = horizontal ? (uj->y + uj->height * 0.5) : (uj->x + uj->width * 0.5);
          if (uj_cross < u_cross - 1e-6 || (fabs(uj_cross - u_cross) <= 1e-6 && j < i))
            in_slot++;
        }
      }
    }
    double out_frac = (out_cnt <= 1) ? 0.0 : (((double)out_slot / (double)(out_cnt - 1)) - 0.5) * 0.45;
    double in_frac  = (in_cnt  <= 1) ? 0.0 : (((double)in_slot  / (double)(in_cnt  - 1)) - 0.5) * 0.45;

    int dr = v->rank - u->rank;
    double x1, y1, x2, y2, cx1, cy1, cx2, cy2;
    if (horizontal) {
      if (dr <= 0 || u == v) {
        double drop = 36.0 + 18.0 * (double)(u == v ? 0 : -dr);
        x1 = u->x + u->width * 0.5;
        y1 = u->y + u->height;
        x2 = v->x + v->width * 0.5;
        y2 = v->y + v->height;
        double base_y = MAX(y1, y2) + drop;
        cx1 = x1;
        cy1 = base_y;
        cx2 = x2;
        cy2 = base_y;
      } else {
        x1 = u->x + u->width;
        y1 = u->y + u->height * (0.5 + out_frac);
        x2 = v->x;
        y2 = v->y + v->height * (0.5 + in_frac);
        double dx = fabs(x2 - x1) * 0.45;
        double bow = 0.0;
        if (dr > 1) {
          double side = ((y1 + y2) * 0.5 >= graph_mid_cross) ? 1.0 : -1.0;
          bow = side * 26.0 * (double)(dr - 1);
        }
        cx1 = x1 + dx;
        cy1 = y1 + bow;
        cx2 = x2 - dx;
        cy2 = y2 + bow;
      }
    } else {
      if (dr <= 0 || u == v) {
        double swing = 36.0 + 18.0 * (double)(u == v ? 0 : -dr);
        x1 = u->x + u->width;
        y1 = u->y + u->height * 0.5;
        x2 = v->x + v->width;
        y2 = v->y + v->height * 0.5;
        double side_x = MAX(x1, x2) + swing;
        cx1 = side_x;
        cy1 = y1;
        cx2 = side_x;
        cy2 = y2;
      } else {
        x1 = u->x + u->width * (0.5 + out_frac);
        y1 = u->y + u->height;
        x2 = v->x + v->width * (0.5 + in_frac);
        y2 = v->y;
        double dy = fabs(y2 - y1) * 0.45;
        double bow = 0.0;
        if (dr > 1) {
          double side = ((x1 + x2) * 0.5 >= graph_mid_cross) ? 1.0 : -1.0;
          bow = side * 26.0 * (double)(dr - 1);
        }
        cx1 = x1 + bow;
        cy1 = y1 + dy;
        cx2 = x2 + bow;
        cy2 = y2 - dy;
      }
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
      double mx = 0.125 * x1 + 0.375 * cx1 + 0.375 * cx2 + 0.125 * x2;
      double my = 0.125 * y1 + 0.375 * cy1 + 0.375 * cy2 + 0.125 * y2;
      PangoLayout *elayout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(elayout, edge_font);
      pango_layout_set_text(elayout, e->label, -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(elayout, &tw, &th);

      double rx = mx - tw * 0.5 - 5.0;
      double ry = my - th * 0.5 - 2.0;
      double rw = tw + 10.0;
      double rh = th + 4.0;
      cairo_set_source_rgb(cr, 0.078, 0.078, 0.078); /* #141414 */
      cairo_rectangle(cr, rx, ry, rw, rh);
      cairo_fill_preserve(cr);
      cairo_set_source_rgb(cr, 0.169, 0.169, 0.169); /* #2b2b2b */
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
      cairo_move_to(cr, mx - tw * 0.5, my - th * 0.5);
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
    pango_layout_set_width(layout, (int)((n->width - 20.0) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_text(layout, n->label, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    (void)tw;

    cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);
    cairo_move_to(cr,
                  n->x + 10.0,
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
