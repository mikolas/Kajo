#ifndef KAJO_IDE_BUFFER_H
#define KAJO_IDE_BUFFER_H

#include <gio/gio.h>
#include <glib.h>
#include <gtksourceview/gtksource.h>
#include "ide_ts.h"

G_BEGIN_DECLS

typedef enum {
  IDE_DIFF_NONE = 0,
  IDE_DIFF_ADDED,
  IDE_DIFF_MODIFIED,
  IDE_DIFF_DELETED,
  IDE_DIFF_AGENT
} IdeDiffState;

typedef enum {
  IDE_DIAG_NONE = 0,
  IDE_DIAG_HINT,
  IDE_DIAG_INFO,
  IDE_DIAG_WARNING,
  IDE_DIAG_ERROR
} IdeDiagSeverity;

typedef struct {
  guint           line;      /* 1-based line number */
  guint           col;       /* 1-based column */
  IdeDiagSeverity severity;
  char           *message;
  char           *source;
} IdeDiagnostic;

typedef struct _IdeBuffer IdeBuffer;

typedef void (*IdeBufferExternalChangeCb)(IdeBuffer *buf,
                                          gboolean   reloaded_cleanly,
                                          gpointer   user_data);

/* Pure testable text, UTF-16 LSP coordinate, structural text object & diff helpers */
guint         ide_utf8_byte_to_utf16_col(const char *line_utf8, gsize byte_offset);
gsize         ide_utf16_col_to_utf8_byte(const char *line_utf8, guint utf16_col);

gboolean      ide_find_text_object(const char *text,
                                   gsize       cursor_byte,
                                   gboolean    inside,
                                   char        target,
                                   gsize      *out_start_byte,
                                   gsize      *out_end_byte);

IdeDiffState *ide_compute_line_diffs(const char *old_text,
                                     const char *new_text,
                                     gsize      *out_line_count);

char         *ide_format_unified_diff(const char *rel_path,
                                      const char *old_text,
                                      const char *new_text);

char         *ide_resolve_alternate_path(const char *file_path);

gboolean      ide_save_file_atomic(const char  *path,
                                   const char  *contents,
                                   gssize       len,
                                   GError     **error);

/* GtkSourceBuffer-backed IdeBuffer lifecycle & operations */
IdeBuffer    *ide_buffer_new(const char *file_path);
void          ide_buffer_free(IdeBuffer *buf);

GtkSourceBuffer *ide_buffer_get_source_buffer(IdeBuffer *buf);
const char      *ide_buffer_get_file_path(const IdeBuffer *buf);
const char      *ide_buffer_get_title(const IdeBuffer *buf);
char            *ide_buffer_get_text(IdeBuffer *buf);
void             ide_buffer_set_text(IdeBuffer *buf, const char *text);
/* Registers the GtkSourceView scheme search paths (user dir, install prefix, XDG
 * system dirs, dev source tree). Idempotent. */
void             ide_buffer_register_scheme_paths(void);
void             ide_buffer_set_style_scheme(IdeBuffer *buf, const char *scheme_id);
const IdeTsDoc *ide_highlight_get_ts_doc(GtkSourceBuffer *sbuf);
void             ide_highlight_source_buffer(GtkSourceBuffer *sbuf,
                                             const char      *file_path,
                                             const char      *scheme_id);

gboolean      ide_buffer_load_from_disk(IdeBuffer *buf, GError **error);
gboolean      ide_buffer_save(IdeBuffer *buf, GError **error);

gboolean      ide_buffer_get_modified(const IdeBuffer *buf);
gboolean      ide_buffer_get_external_reloaded(const IdeBuffer *buf);
void          ide_buffer_clear_external_reloaded(IdeBuffer *buf);
gboolean      ide_buffer_get_collision(const IdeBuffer *buf);

void          ide_buffer_set_external_change_callback(IdeBuffer                *buf,
                                                      IdeBufferExternalChangeCb cb,
                                                      gpointer                  user_data);

/* Breakpoints, Diagnostics, DAP PC & Line Diffs for 6-Column Gutter */
gboolean      ide_buffer_has_breakpoint(const IdeBuffer *buf, guint line_1based);
gboolean      ide_buffer_toggle_breakpoint(IdeBuffer *buf, guint line_1based);
GArray       *ide_buffer_get_breakpoints(const IdeBuffer *buf);

void          ide_buffer_clear_diagnostics(IdeBuffer *buf);
void          ide_buffer_add_diagnostic(IdeBuffer      *buf,
                                        guint           line_1based,
                                        guint           col_1based,
                                        IdeDiagSeverity severity,
                                        const char     *message,
                                        const char     *source);
const GPtrArray *ide_buffer_get_diagnostics(const IdeBuffer *buf);
const IdeDiagnostic *ide_buffer_get_line_diagnostic(const IdeBuffer *buf, guint line_1based);

void          ide_buffer_set_dap_pc_line(IdeBuffer *buf, guint line_1based);
guint         ide_buffer_get_dap_pc_line(const IdeBuffer *buf);

void          ide_buffer_recompute_diffs(IdeBuffer *buf);
IdeDiffState  ide_buffer_get_line_diff(const IdeBuffer *buf, guint line_1based);

/* Agent Batch Transaction Snapshot Support */
void          ide_buffer_record_agent_snapshot(IdeBuffer *buf);
gboolean      ide_buffer_has_agent_changes(const IdeBuffer *buf);
const char   *ide_buffer_get_agent_snapshot(const IdeBuffer *buf);
void          ide_buffer_accept_agent_changes(IdeBuffer *buf);
gboolean      ide_buffer_revert_agent_changes(IdeBuffer *buf, GError **error);

void          ide_diagnostic_free(gpointer data);

G_END_DECLS

#endif /* KAJO_IDE_BUFFER_H */
