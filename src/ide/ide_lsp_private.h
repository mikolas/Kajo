#ifndef KAJO_IDE_LSP_PRIVATE_H
#define KAJO_IDE_LSP_PRIVATE_H

#include "ide_lsp.h"
#include "ide_picker.h"
#include "ide_search.h"

G_BEGIN_DECLS

typedef struct _DocState DocState;

typedef struct {
  IdeLspClient *client;
  guint         restarts;
} ClientEntry;

typedef enum {
  CTX_HOVER,
  CTX_COMPLETION,
  CTX_DEFINITION,
  CTX_RENAME,
  CTX_FORMAT,
  CTX_COMMAND,
  CTX_REFERENCES,
  CTX_SIGNATURE,
  CTX_CALL_PREPARE,
  CTX_CALL_LIST,
  CTX_RAW,
} CtxKind;

typedef struct {
  IdeLspManager *mgr;
  CtxKind        kind;
  char          *uri;
  guint          token;
  GtkWidget     *anchor;     /* weak: cleared when the widget is destroyed */
  gboolean       had_anchor; /* TRUE if caller supplied a non-NULL anchor */
  gboolean       outgoing;   /* call hierarchy direction */
  IdeBuffer     *buf;
  guint          line_1based;
  guint          col_1based;
  char          *prefix;     /* completion: text typed before the request */
  char          *new_name;
  guint          tab_size;
  IdeLspCommandCb command_cb;
  gpointer        command_data;
  IdeLspRawCb     raw_cb;
  gpointer        raw_data;
} RequestCtx;

struct _DocState {
  IdeLspManager          *mgr;
  IdeBuffer              *buf;
  char                   *path;
  char                   *uri;
  char                   *language_id;
  const IdeLspServerSpec *spec;
  char                   *root;
  IdeLspClient           *client;
  gint                    version;
  JsonNode               *raw_diagnostics; /* last publishDiagnostics array (for code actions) */
  guint                   change_timer;
  gboolean                sent_open;
};

struct _IdeLspManager {
  char                *workspace_root;
  IdeLspJumpCb         jump_cb;
  IdeLspStatusCb       status_cb;
  gpointer             cb_data;
  IdeLspDiagnosticsCb  diagnostics_cb;
  IdeLspMessageCb      message_cb;
  IdeLspUiStateCb      ui_state_cb;
  IdeLspUiState        ui_state;
  gpointer             ui_data;

  GHashTable          *clients;      /* "id|root" -> ClientEntry* */
  GHashTable          *docs;         /* IdeBuffer* -> DocState* */
  GHashTable          *docs_by_uri;  /* uri (borrowed from DocState) -> DocState* */
  GHashTable          *missing;      /* server_id -> TRUE */
  GHashTable          *live_ctx;     /* RequestCtx* set */
  DocState            *active_doc;
  gboolean             shutting_down;

  /* popover (hover / completion) */
  char                *active_server_name;
  GtkWidget           *active_popover;
  GtkWidget           *active_anchor;   /* source view the popover points into */
  IdeBuffer           *active_buf;      /* buffer whose cursor it tracks */
  GtkWidget           *comp_list_box;
  GtkWidget           *comp_scroller;
  GtkWidget           *comp_doc_label;
  GPtrArray           *comp_items;
  gint                 selected_comp_idx;
  char                *comp_prefix;
  guint                popover_token;
  gboolean             comp_is_code_actions; /* the list shows code actions */
  GPtrArray           *code_actions;         /* JsonNode* parallel to comp_items */
  GString             *rename_input;
  char                *rename_old;
  GtkWidget           *rename_label;

  /* references / signature help / call hierarchy */
  GPtrArray           *ref_items;       /* IdeLspLocation* */
  GPtrArray           *sig_items;       /* IdeLspSignatureInfo* */
  guint                sig_active_idx;
  gint                 sig_active_param;
  GtkWidget           *sig_hdr_label;
  GtkWidget           *sig_code_label;
  GtkWidget           *sig_doc_label;
  GPtrArray           *call_items;      /* IdeLspCallEntry* */
  JsonNode            *call_root_item;  /* cloned CallHierarchyItem for dir toggle */
  char                *call_root_uri;
  gboolean             call_outgoing;

  /* the single in-flight popover request, cancelled by the next one */
  IdeLspClient        *inflight_client;
  guint                inflight_id;
  RequestCtx          *inflight_ctx;
  GCancellable        *rg_cancellable;
};

/* ide_lsp_parse.c */
const char        *ide_lsp_json_str(JsonObject *o, const char *name);
JsonObject        *ide_lsp_json_obj_member(JsonObject *o, const char *name);
gboolean           ide_lsp_json_range_to_edit(JsonObject *range, const char *new_text, IdeLspTextEdit *out);
gint               ide_lsp_edit_cmp(gconstpointer a, gconstpointer b);
IdeCompletionItem *ide_lsp_parse_completion_item(JsonObject *o, const char *prefix);
gint               ide_lsp_completion_cmp(gconstpointer a, gconstpointer b);

/* ide_lsp.c */
RequestCtx        *ide_lsp_ctx_new(IdeLspManager *mgr, CtxKind kind, const char *uri);
void               ide_lsp_ctx_done(RequestCtx *ctx);
void               ide_lsp_ctx_set_anchor(RequestCtx *ctx, GtkWidget *anchor);
void               ide_lsp_cancel_inflight(IdeLspManager *mgr);
void               ide_lsp_send_inflight_request(IdeLspManager    *mgr,
                                                 DocState         *doc,
                                                 RequestCtx       *ctx,
                                                 const char       *method,
                                                 JsonNode         *params,
                                                 IdeLspResponseCb  cb);
gboolean           ide_lsp_take_popover_response(RequestCtx  *ctx,
                                                 gboolean     require_anchor,
                                                 DocState   **doc_out,
                                                 GtkWidget  **anchor_out);

char              *ide_lsp_buffer_line_text(GtkTextBuffer *tbuf, guint line0);
void               ide_lsp_buffer_cursor_position(IdeBuffer *buf, guint *line0, guint *utf16);
void               ide_lsp_buffer_iter_at_pos(GtkTextBuffer *tbuf, guint line0, guint utf16, GtkTextIter *out);
guint              ide_lsp_buffer_char_col_1based(GtkTextBuffer *tbuf, guint line0, guint utf16);
void               ide_lsp_file_to_line_col(const char *path, guint line0, guint utf16,
                                            guint *line1, guint *col1);
char              *ide_lsp_read_disk_line_and_col(const char *path, guint line0, guint utf16, guint *col1_out);
void               ide_lsp_resolve_request_position(IdeBuffer *buf, guint line_1based, guint col_1based,
                                                    guint *line0, guint *utf16);
char              *ide_lsp_extract_word_at_cursor(IdeBuffer *buf, char **out_prefix);

void               ide_lsp_report_message(IdeLspManager *mgr, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
JsonNode          *ide_lsp_text_document_params(const char *uri);
void               ide_lsp_doc_send_change_now(DocState *doc);
DocState          *ide_lsp_require_doc(IdeLspManager *lsp,
                                       IdeBuffer     *buf,
                                       const char    *capability,
                                       const char    *feature_name,
                                       const char    *cap_label);

/* ide_lsp_popovers.c */
void               ide_lsp_set_ui_state(IdeLspManager *lsp, IdeLspUiState state);
GtkWidget         *ide_lsp_new_popover_for(IdeLspManager *lsp, GtkWidget *anchor_view, IdeBuffer *buf);
void               ide_lsp_show_hover_card(IdeLspManager *lsp,
                                           GtkWidget     *anchor_view,
                                           IdeBuffer     *buf,
                                           guint          line_1based,
                                           guint          col_1based,
                                           const char    *title,
                                           const char    *code,
                                           const char    *doc_text,
                                           gboolean       is_fallback);
void               ide_lsp_present_completion_popover(IdeLspManager *lsp,
                                                      GtkWidget     *anchor_view,
                                                      IdeBuffer     *buf);

/* ide_lsp_actions.c */
void               ide_lsp_apply_edits_to_buffer(IdeLspManager *mgr, IdeBuffer *buf, const GPtrArray *edits);
void               ide_lsp_apply_workspace_edit(IdeLspManager *mgr, JsonNode *edit,
                                                guint *edits_out, guint *files_out);
void               ide_lsp_run_code_action(IdeLspManager *lsp, IdeBuffer *buf, guint index);
void               ide_lsp_legacy_hover(IdeLspManager *lsp,
                                        GtkWidget     *anchor_view,
                                        IdeBuffer     *buf,
                                        guint          line_1based,
                                        guint          col_1based);
void               ide_lsp_collect_buffer_identifiers(IdeLspManager *lsp,
                                                      IdeBuffer     *buf,
                                                      const char    *prefix);

G_END_DECLS

#endif /* KAJO_IDE_LSP_PRIVATE_H */
