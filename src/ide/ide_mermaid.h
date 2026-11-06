#ifndef KAJO_IDE_MERMAID_H
#define KAJO_IDE_MERMAID_H

#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  IDE_MERMAID_FLOWCHART_LR = 0,
  IDE_MERMAID_FLOWCHART_TD,
  IDE_MERMAID_SEQUENCE,
  IDE_MERMAID_STATE,
  IDE_MERMAID_CLASS,
} IdeMermaidKind;

typedef struct {
  char   *id;
  char   *label;
  guint   source_line_1based;
  int     rank;
  int     order_in_rank;
  double  x;
  double  y;
  double  width;
  double  height;
  gboolean is_accent;
} IdeMermaidNode;

typedef struct {
  char    *from_id;
  char    *to_id;
  char    *label;
  gboolean dashed;
} IdeMermaidEdge;

typedef struct {
  IdeMermaidKind kind;
  char          *kind_badge;
  GPtrArray     *nodes; /* IdeMermaidNode* */
  GPtrArray     *edges; /* IdeMermaidEdge* */
  double         canvas_width;
  double         canvas_height;
} IdeMermaidGraph;

/* Pure testable Mermaid parser, layout engine and SVG/PNG exporters */
IdeMermaidGraph *ide_mermaid_parse_graph(const char *mermaid_src,
                                         guint       block_start_line_1based);
void             ide_mermaid_layout_graph(IdeMermaidGraph *graph);
const IdeMermaidNode *ide_mermaid_hit_test_node(const IdeMermaidGraph *graph,
                                                double                 zoom,
                                                double                 px,
                                                double                 py);
gboolean         ide_mermaid_export_svg(const IdeMermaidGraph *graph,
                                        const char            *out_svg_path);
gboolean         ide_mermaid_export_png(const IdeMermaidGraph *graph,
                                        const char            *out_png_path);
void             ide_mermaid_graph_free(IdeMermaidGraph *graph);

/* Draws the laid-out graph at `zoom` onto `cr` (used by the Markdown card). */
void             ide_mermaid_render_cairo(const IdeMermaidGraph *graph,
                                          cairo_t               *cr,
                                          double                 zoom,
                                          double                 width,
                                          double                 height);

G_END_DECLS

#endif /* KAJO_IDE_MERMAID_H */
