#ifndef KAJO_IDE_STRIP_GRAPH_PRIVATE_H
#define KAJO_IDE_STRIP_GRAPH_PRIVATE_H

/* Internal to src/ide/: IdeStripGraph internals shared between
 * ide_strip_graph.c and ide_strip_graph_ts.c. Not a public API. */

#include "ide_strip_graph.h"
#include "ide_ts.h"

G_BEGIN_DECLS

#define STRIP_MAX_FILES       2500u
#define STRIP_MAX_FILE_BYTES  (512u * 1024u)
#define STRIP_DEFAULT_PEEK    5u

typedef struct {
  char  *name;
  char  *file_path;          /* canonical absolute path */
  char  *rel_path;           /* relative to workspace_root */
  guint  start_line;         /* 1-based */
  guint  end_line;           /* 1-based */
  int    kind;               /* IdeStripSymKind */
  gint   cached_child_count; /* -1 = uncomputed */
} SymDef;

typedef struct {
  char      *file_path;
  char      *rel_path;
  GPtrArray *defs;           /* owned SymDef* */
  guint      line_count;
} FileEntry;

struct _IdeStripGraph {
  char         *workspace_root; /* canonical, no trailing slash (except "/") */
  gboolean      indexed;
  GHashTable   *files;          /* canonical path -> FileEntry* */
  GHashTable   *by_name;        /* char* name -> GPtrArray<SymDef*> (borrowed) */
  GCancellable *alive;          /* cancelled on free to guard async LSP cbs */
};

typedef struct {
  GArray *starts; /* gsize byte offset of start of each line (0-indexed) */
  gsize   len;
} LineIndex;

typedef struct {
  char      *text;
  gsize      len;
  LineIndex  lines;
} CachedFileText;

/* Defined in ide_strip_graph.c */
void                   ide_strip_graph_sym_def_free(gpointer data);
char                  *ide_strip_graph_canonicalize_dir(const char *path);
void                   ide_strip_graph_line_index_init(LineIndex *li, const char *text, gsize len);
void                   ide_strip_graph_line_index_clear(LineIndex *li);
guint                  ide_strip_graph_line_index_count(const LineIndex *li);
guint                  ide_strip_graph_line_index_line_for_byte(const LineIndex *li, gsize byte);
gsize                  ide_strip_graph_line_index_byte_for_line(const LineIndex *li, guint line_1based);
gsize                  ide_strip_graph_line_index_end_byte_for_line(const LineIndex *li, guint line_1based);
void                   ide_strip_graph_cached_file_text_free(gpointer data);
const CachedFileText  *ide_strip_graph_get_cached_text(GHashTable *cache,
                                                       const char *file_path,
                                                       const char *override_path,
                                                       const char *override_text,
                                                       gsize       override_len);
char                  *ide_strip_graph_slice_peek(const char      *text,
                                                  gsize            len,
                                                  const LineIndex *li,
                                                  guint            start_line,
                                                  guint            max_lines);
SymDef                *ide_strip_graph_resolve_symbol(IdeStripGraph *g,
                                                      const char    *name,
                                                      const char    *prefer_file,
                                                      gboolean       want_type);
GPtrArray             *ide_strip_graph_extract_impact_fallback(IdeStripGraph *g,
                                                               const char    *canon_file,
                                                               const char    *text,
                                                               gsize          len,
                                                               guint          start_line,
                                                               GHashTable    *file_cache);

/* Defined in ide_strip_graph_ts.c */
gboolean               ide_strip_graph_is_ident_start(char c);
gboolean               ide_strip_graph_is_ident_char(char c);
gboolean               ide_strip_graph_skip_comment_or_string_c(const char *t, gsize len, gsize *pi);
gboolean               ide_strip_graph_looks_like_c_prototype(const char *t, gsize len, gsize ws, gsize we);
GPtrArray             *ide_strip_graph_extract_file_defs(const char      *file_path,
                                                         const char      *rel_path,
                                                         const char      *text,
                                                         gsize            len,
                                                         const LineIndex *li);
guint                  ide_strip_graph_compute_child_count(IdeStripGraph *g,
                                                           SymDef        *def,
                                                           GHashTable    *file_cache,
                                                           const char    *override_path,
                                                           const char    *override_text,
                                                           gsize          override_len);

G_END_DECLS

#endif /* KAJO_IDE_STRIP_GRAPH_PRIVATE_H */
