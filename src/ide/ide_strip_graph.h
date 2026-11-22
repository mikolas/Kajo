#ifndef KAJO_IDE_STRIP_GRAPH_H
#define KAJO_IDE_STRIP_GRAPH_H

/* Workspace-only call-graph indexer, tree-sitter source-order target extractor,
 * and async LSP raw query bridge for Kajo IDE Code Strip (Milestone M2).
 *
 * Self-contained: has zero dependency on ide_strip_layout.h so ide_strip_view.c
 * converts IdeStripGraphEntry items into IdeStripTile cleanly. */

#include "ide_buffer.h"
#include "ide_lsp.h"
#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  IDE_STRIP_SYM_FUNC  = 0,
  IDE_STRIP_SYM_TYPE  = 1,
  IDE_STRIP_SYM_FILE  = 2,
  IDE_STRIP_SYM_MACRO = 3,
} IdeStripSymKind;

typedef enum {
  IDE_STRIP_GRAPH_LENS_CALLS  = 0,
  IDE_STRIP_GRAPH_LENS_TYPES  = 1,
  IDE_STRIP_GRAPH_LENS_IMPACT = 2,
  IDE_STRIP_GRAPH_LENS_DIFF   = 3,
} IdeStripLensMode;

typedef struct {
  char     *name;
  char     *file_path;
  char     *rel_path;
  guint     start_line;        /* 1-based */
  guint     end_line;          /* 1-based */
  guint     call_line;         /* 1-based call/reference line */
  guint     call_count;        /* deduplicated occurrence count (>= 1) */
  guint     child_count;       /* distinct workspace targets inside this symbol */
  int       kind;              /* 0=func, 1=type, 2=file, 3=macro */
  char     *peek_code;         /* up to 5-line preview */
  gboolean  is_agent_modified;
  gboolean  is_dirty;
  gboolean  is_fallback;
} IdeStripGraphEntry;

void ide_strip_graph_entry_free(gpointer data);

typedef struct _IdeStripGraph IdeStripGraph;

/* Async LSP query completion callback. `entries` is a GPtrArray<IdeStripGraphEntry*>
 * borrowed for the duration of the callback (call g_ptr_array_ref() if retaining). */
typedef void (*IdeStripGraphAsyncCb)(GPtrArray *entries,
                                     guint      ext_suppressed,
                                     gboolean   is_fallback,
                                     gpointer   user_data);

/* Strict workspace-only boundary check: canonicalizes paths, rejects NULL/empty,
 * non-file URIs (e.g. jdt://), paths outside workspace_root (strict separator
 * boundary), and excluded build/vendor directories (.git, builddir, build,
 * node_modules, target, subprojects). */
gboolean ide_strip_path_in_workspace(const char *path, const char *workspace_root);

/* Workspace symbol index lifecycle */
IdeStripGraph *ide_strip_graph_new(const char *workspace_root);
void           ide_strip_graph_free(IdeStripGraph *g);
void           ide_strip_graph_set_workspace_root(IdeStripGraph *g,
                                                  const char    *workspace_root);
const char    *ide_strip_graph_get_workspace_root(const IdeStripGraph *g);
void           ide_strip_graph_invalidate_file(IdeStripGraph *g,
                                               const char    *file_path);
void           ide_strip_graph_ensure_indexed(IdeStripGraph *g);
guint          ide_strip_graph_file_count(const IdeStripGraph *g);
guint          ide_strip_graph_symbol_count(const IdeStripGraph *g);

/* Preview reader: reads up to `max_lines` starting at 1-based `start_line` from
 * `override_text` (when non-NULL) or disk, trimming trailing blank lines. */
char *ide_strip_graph_read_peek(const char *file_path,
                                guint       start_line,
                                guint       max_lines,
                                const char *override_text);

/* Locates the enclosing function/class/struct around `cursor_line_1based` using
 * tree-sitter (with indexed/heuristic fallback, then full-file [1..line_count]). */
gboolean ide_strip_graph_find_enclosing_symbol(IdeStripGraph *g,
                                               const char    *file_path,
                                               const char    *text,
                                               guint          cursor_line_1based,
                                               char         **out_name,
                                               guint         *out_start_line,
                                               guint         *out_end_line,
                                               int           *out_kind);

/* Source-order workspace target extraction for CALLS (0), TYPES (1), IMPACT (2),
 * and DIFF (3). Returns GPtrArray<IdeStripGraphEntry*> with free func set.
 * Non-workspace calls/types increment `*out_ext_suppressed` and are omitted. */
GPtrArray *ide_strip_graph_extract_targets(IdeStripGraph *g,
                                           const char    *file_path,
                                           const char    *text,
                                           guint          start_line,
                                           guint          end_line,
                                           int            lens_mode,
                                           guint         *out_ext_suppressed);

/* Extracts modified / AI-agent-changed workspace symbols across `open_buffers`
 * (GPtrArray<IdeBuffer*>). */
GPtrArray *ide_strip_graph_extract_diff_targets(IdeStripGraph *g,
                                                GPtrArray     *open_buffers);

/* Asynchronous LSP enrichment for CALLS (0: prepareCallHierarchy -> outgoingCalls)
 * and IMPACT (2: textDocument/references). Returns FALSE without invoking `cb`
 * if no ready server handles `buf` or `lens_mode` is not LSP-backed.
 * Safe against ide_strip_graph_free() while requests are in flight. */
gboolean ide_strip_graph_query_lsp_async(IdeStripGraph       *g,
                                         IdeLspManager       *lsp,
                                         IdeBuffer           *buf,
                                         guint                line_1based,
                                         guint                col_1based,
                                         int                  lens_mode,
                                         IdeStripGraphAsyncCb cb,
                                         gpointer             user_data);

G_END_DECLS

#endif /* KAJO_IDE_STRIP_GRAPH_H */
