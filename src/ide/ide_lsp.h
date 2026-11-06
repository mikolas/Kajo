#ifndef KAJO_IDE_LSP_H
#define KAJO_IDE_LSP_H

#include "ide_buffer.h"
#include "ide_lsp_client.h"
#include "ide_lsp_servers.h"
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* One LSP TextEdit: replaces [start, end) (0-based line, UTF-16 character). */
typedef struct {
  guint line_start;
  guint char_start;
  guint line_end;
  guint char_end;
  char *new_text;
} IdeLspTextEdit;

typedef struct {
  char *label;
  char *detail;
  char *insert_text;
  char *documentation;
  char *filter_text;
  char *sort_text;
  char  kind_badge[4];      /* "ƒ", "v", "T", "k", "m", "c" */
  gboolean has_edit;        /* server supplied textEdit (replaces `edit`) */
  IdeLspTextEdit edit;
  GPtrArray *extra_edits;   /* IdeLspTextEdit*: additionalTextEdits (auto-imports) */
  gint   score;             /* client-side fuzzy score against the typed prefix */
} IdeCompletionItem;

typedef struct _IdeLspManager IdeLspManager;

typedef void (*IdeLspJumpCb)(const char *abs_path,
                             guint       line_1based,
                             guint       col_1based,
                             gpointer    user_data);

/* `server_name` is shown in the status bar; `active` means "ready". */
typedef void (*IdeLspStatusCb)(const char *server_name,
                               gboolean    active,
                               gpointer    user_data);

/* Diagnostics of `buf` changed (buffer may be redrawn / counters refreshed). */
typedef void (*IdeLspDiagnosticsCb)(IdeBuffer *buf, gpointer user_data);

/* Which popover the manager currently shows; drives the modal FSM state. */
typedef enum {
  IDE_LSP_UI_CLOSED = 0,
  IDE_LSP_UI_HOVER,
  IDE_LSP_UI_COMPLETION,
  IDE_LSP_UI_CODE_ACTION,
  IDE_LSP_UI_RENAME,
} IdeLspUiState;

typedef void (*IdeLspUiStateCb)(IdeLspUiState state, gpointer user_data);

/* Short user-facing messages (rename results, "no definition found", ...). */
typedef void (*IdeLspMessageCb)(const char *message, gpointer user_data);

/* Pure helpers (also used by tests). Framing lives in ide_lsp_client.h. */
const char *ide_lsp_detect_server_for_file(const char  *file_path,
                                           const char **out_server_id);

/* Applies LSP text edits to a UTF-8 string (edits may be in any order; they must
 * not overlap). Returns a newly allocated string, or NULL if an edit is out of
 * range. */
char          *ide_lsp_apply_edits_to_string(const char *text, const GPtrArray *edits);
IdeLspTextEdit *ide_lsp_text_edit_new(guint line_start, guint char_start,
                                      guint line_end, guint char_end,
                                      const char *new_text);
void           ide_lsp_text_edit_free(gpointer data);
/* Parses a JSON TextEdit array into IdeLspTextEdit*; NULL-safe. */
GPtrArray     *ide_lsp_parse_text_edits(JsonNode *edits_node);
/* Removes LSP snippet syntax ($1, ${2:text}, $0) from `snippet`. */
char          *ide_lsp_strip_snippet(const char *snippet);
/* Flattens hover `contents` (MarkupContent / MarkedString / array) to markdown. */
char          *ide_lsp_hover_to_markdown(JsonNode *contents);

/* Manager: owns one client per (server, project root). */
IdeLspManager *ide_lsp_manager_new(const char    *workspace_root,
                                   IdeLspJumpCb   jump_cb,
                                   IdeLspStatusCb status_cb,
                                   gpointer       user_data);
void           ide_lsp_manager_free(IdeLspManager *lsp);
void           ide_lsp_manager_set_callbacks(IdeLspManager      *lsp,
                                             IdeLspDiagnosticsCb diagnostics_cb,
                                             IdeLspMessageCb     message_cb,
                                             IdeLspUiStateCb     ui_state_cb,
                                             gpointer            user_data);

void           ide_lsp_manager_notify_open(IdeLspManager *lsp, IdeBuffer *buf);
void           ide_lsp_manager_notify_change(IdeLspManager *lsp, IdeBuffer *buf);
void           ide_lsp_manager_notify_save(IdeLspManager *lsp, IdeBuffer *buf);
void           ide_lsp_manager_notify_close(IdeLspManager *lsp, IdeBuffer *buf);

/* TRUE when a language server is attached to `buf` (starting or ready). */
gboolean       ide_lsp_manager_has_server(IdeLspManager *lsp, IdeBuffer *buf);
/* TRUE when the server for `buf` finished initialization. */
gboolean       ide_lsp_manager_is_ready(IdeLspManager *lsp, IdeBuffer *buf);

void           ide_lsp_manager_request_hover(IdeLspManager *lsp,
                                             GtkWidget     *anchor_view,
                                             IdeBuffer     *buf,
                                             guint          line_1based,
                                             guint          col_1based);

void           ide_lsp_manager_request_completion(IdeLspManager *lsp,
                                                  GtkWidget     *anchor_view,
                                                  IdeBuffer     *buf,
                                                  guint          line_1based,
                                                  guint          col_1based);

void           ide_lsp_manager_request_definition(IdeLspManager *lsp,
                                                  IdeBuffer     *buf,
                                                  guint          line_1based,
                                                  guint          col_1based);

void           ide_lsp_manager_request_rename(IdeLspManager *lsp,
                                              IdeBuffer     *buf,
                                              guint          line_1based,
                                              guint          col_1based,
                                              const char    *new_name);

/* textDocument/implementation (jumps like definition). */
void           ide_lsp_manager_request_implementation(IdeLspManager *lsp, IdeBuffer *buf);

/* Interactive rename prompt: begin -> input/backspace -> confirm | cancel_popover. */
void           ide_lsp_manager_rename_begin(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf);
void           ide_lsp_manager_rename_input(IdeLspManager *lsp, const char *text);
void           ide_lsp_manager_rename_backspace(IdeLspManager *lsp);
gboolean       ide_lsp_manager_rename_active(IdeLspManager *lsp);
void           ide_lsp_manager_rename_confirm(IdeLspManager *lsp, IdeBuffer *buf);

/* Quick fixes / refactorings at the cursor or selection (popover list; confirm
 * with ide_lsp_manager_confirm_selection). */
void           ide_lsp_manager_request_code_actions(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf);

/* Auto-trigger completion after a server trigger character ('.', ':' ...) was
 * typed in INSERT mode. Returns TRUE if a request was started. */
gboolean       ide_lsp_manager_maybe_trigger_completion(IdeLspManager *lsp, GtkWidget *anchor_view,
                                                        IdeBuffer *buf, const char *typed);
/* Re-run the active completion after the prefix changed. */
gboolean       ide_lsp_manager_completion_active(IdeLspManager *lsp);

/* textDocument/formatting; no-op (with a message) when unsupported. */
void           ide_lsp_manager_request_format(IdeLspManager *lsp, IdeBuffer *buf);

void           ide_lsp_manager_cancel_popover(IdeLspManager *lsp);
void           ide_lsp_manager_select_next(IdeLspManager *lsp);
void           ide_lsp_manager_select_prev(IdeLspManager *lsp);
void           ide_lsp_manager_confirm_selection(IdeLspManager *lsp, IdeBuffer *buf);

void           ide_completion_item_free(gpointer data);

G_END_DECLS

#endif /* KAJO_IDE_LSP_H */
