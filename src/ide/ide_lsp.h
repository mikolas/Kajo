#ifndef KAJO_IDE_LSP_H
#define KAJO_IDE_LSP_H

#include "ide_buffer.h"
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
  char *label;
  char *detail;
  char *insert_text;
  char *documentation;
  char  kind_badge[4]; /* "ƒ", "S", "v", "k", "m" */
} IdeCompletionItem;

typedef struct _IdeLspManager IdeLspManager;

typedef void (*IdeLspJumpCb)(const char *abs_path,
                             guint       line_1based,
                             guint       col_1based,
                             gpointer    user_data);

typedef void (*IdeLspStatusCb)(const char *server_name,
                               gboolean    active,
                               gpointer    user_data);

/* Pure testable JSON-RPC 2.0 Content-Length framing & server detection */
char       *ide_jsonrpc_frame_message(const char *json_body);
char       *ide_jsonrpc_extract_body(const char *stream_buf,
                                     gsize       stream_len,
                                     gsize      *out_consumed_bytes);
const char *ide_lsp_detect_server_for_file(const char  *file_path,
                                           const char **out_server_id);

/* LSP Client & Pixel-Anchored Popover UI */
IdeLspManager *ide_lsp_manager_new(const char    *workspace_root,
                                   IdeLspJumpCb   jump_cb,
                                   IdeLspStatusCb status_cb,
                                   gpointer       user_data);
void           ide_lsp_manager_free(IdeLspManager *lsp);

void           ide_lsp_manager_notify_open(IdeLspManager *lsp, IdeBuffer *buf);
void           ide_lsp_manager_notify_change(IdeLspManager *lsp, IdeBuffer *buf);
void           ide_lsp_manager_notify_save(IdeLspManager *lsp, IdeBuffer *buf);

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

void           ide_lsp_manager_cancel_popover(IdeLspManager *lsp);
void           ide_lsp_manager_select_next(IdeLspManager *lsp);
void           ide_lsp_manager_select_prev(IdeLspManager *lsp);
void           ide_lsp_manager_confirm_selection(IdeLspManager *lsp, IdeBuffer *buf);

void           ide_completion_item_free(gpointer data);

G_END_DECLS

#endif /* KAJO_IDE_LSP_H */
