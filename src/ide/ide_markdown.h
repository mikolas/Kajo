#ifndef KAJO_IDE_MARKDOWN_H
#define KAJO_IDE_MARKDOWN_H

#include "ide_buffer.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  IDE_MERMAID_FLOWCHART_LR = 0,
  IDE_MERMAID_FLOWCHART_TD,
  IDE_MERMAID_SEQUENCE,
  IDE_MERMAID_STATE,
  IDE_MERMAID_CLASS,
} IdeMermaidKind;

typedef enum {
  IDE_MD_EMBED_NONE = 0,
  IDE_MD_EMBED_IMAGE,
  IDE_MD_EMBED_PDF,
  IDE_MD_EMBED_ARCHIVE,
  IDE_MD_EMBED_MEDIA,
  IDE_MD_EMBED_DOCUMENT,
} IdeMarkdownEmbedKind;

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

typedef void (*IdeMarkdownJumpCb)(guint line_1based, gpointer user_data);
typedef void (*IdeMarkdownBufferEditCb)(const char *new_full_text, gpointer user_data);
typedef void (*IdeMarkdownOpenFileCb)(const char *path,
                                      guint       line_1based,
                                      guint       col_1based,
                                      gpointer    user_data);

typedef struct _IdeMarkdownView IdeMarkdownView;

/* Pure testable Mermaid parser, layout engine, SVG exporter, and checklist mutator */
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

char            *ide_markdown_toggle_checklist_line(const char *markdown_text,
                                                    guint       line_1based);

/* Pure testable inline Pango formatter, embed extractor, and asset path resolver */
char            *ide_markdown_format_inline_pango(const char *raw);
gboolean         ide_markdown_extract_embed(const char  *line,
                                            char       **out_alt,
                                            char       **out_src,
                                            char       **out_link_url);
char            *ide_markdown_resolve_asset_path(const char *raw_ref,
                                                 const char *doc_dir,
                                                 const char *workspace_root,
                                                 guint      *out_line_1based);
IdeMarkdownEmbedKind ide_markdown_classify_asset(const char *path_or_ref);

/* Full Graphical Markdown, Embedded Media/Image/PDF/Archive & Cairo Vector Mermaid View */
IdeMarkdownView *ide_markdown_view_new(IdeMarkdownJumpCb       jump_cb,
                                       IdeMarkdownBufferEditCb edit_cb,
                                       gpointer                user_data);
void             ide_markdown_view_set_open_file_cb(IdeMarkdownView      *md,
                                                    IdeMarkdownOpenFileCb open_file_cb);
void             ide_markdown_view_set_context(IdeMarkdownView *md,
                                               const char      *doc_file_path,
                                               const char      *workspace_root);
void             ide_markdown_view_free(IdeMarkdownView *md);

GtkWidget       *ide_markdown_view_get_widget(IdeMarkdownView *md);
void             ide_markdown_view_render(IdeMarkdownView *md,
                                          const char      *markdown_text);
void             ide_markdown_view_sync_scroll(IdeMarkdownView *md,
                                               double           scroll_fraction);

G_END_DECLS

#endif /* KAJO_IDE_MARKDOWN_H */
