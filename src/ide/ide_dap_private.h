#ifndef KAJO_IDE_DAP_PRIVATE_H
#define KAJO_IDE_DAP_PRIVATE_H

#include "ide_dap.h"
#include "ide_dap_client.h"
#include "ide_dap_launch.h"
#include "ide_dap_java.h"

G_BEGIN_DECLS

typedef enum {
  DAP_IDLE = 0,
  DAP_STARTING,
  DAP_RUNNING,   /* debuggee running */
  DAP_STOPPED,   /* paused: stack and variables are valid */
  DAP_ENDED,
} DapState;

typedef struct {
  gint64  id;
  char   *name;
  char   *path;
  guint   line;
} DapFrame;

typedef struct _DapVarNode DapVarNode;
struct _DapVarNode {
  guint       id;
  char       *name;
  char       *type;
  char       *value;
  gint64      var_ref;          /* > 0 means expandable */
  guint       depth;
  gboolean    expanded;
  gboolean    loading;
  gboolean    is_scope;
  gboolean    children_loaded;
  GPtrArray  *children;         /* DapVarNode* (lazy-fetched on expand) */
};

typedef struct {
  guint      id;
  char      *expression;
  char      *value;
  char      *type;
  gint64     var_ref;
  gboolean   expanded;
  gboolean   loading;
  gboolean   children_loaded;
  gboolean   has_error;
  GPtrArray *children;          /* DapVarNode* */
} DapWatchItem;

struct _IdeDapView {
  IdeDapActionCb action_cb;
  gpointer       cb_data;

  GtkWidget     *root_box;
  GtkWidget     *status_badge;
  GtkWidget     *adapter_badge;
  GtkWidget     *stack_list_box;
  GtkWidget     *locals_list_box;
  GtkWidget     *watch_list_box;
  GtkWidget     *watch_entry;
  GtkWidget     *console_view;
  GtkTextBuffer *console_buf;
  GtkWidget     *repl_entry;

  /* session */
  char                 *workspace_root;
  IdeDapBreakpointsFn   breakpoints_fn;
  IdeDapFrameCb         frame_cb;
  IdeDapStateCb         state_cb;
  gpointer              ctx_data;

  IdeDapClient *client;
  DapState      state;
  gint64        thread_id;
  gint64        frame_id;
  GPtrArray    *frames;       /* DapFrame* */
  GPtrArray    *var_roots;    /* DapVarNode* */
  GPtrArray    *watches;      /* DapWatchItem* */
  guint         generation;   /* invalidates responses of a previous session */
  guint         scope_gen;    /* invalidates variable/watch responses across frames */
  guint         next_node_id;
  guint         next_watch_id;
  gboolean      seen_initialized; /* adapter banner output precedes this */
  IdeDapJavaExecFn java_exec;
  gpointer      java_exec_data;
  gpointer      java_host_cache;   /* JavaHost of the last Java session */
  GCancellable *java_cancel;
};

typedef struct {
  IdeDapView *dap;
  guint       generation;
} DapToken;

DapToken *ide_dap_token_new(IdeDapView *dap);
gboolean  ide_dap_token_valid(DapToken *t);
JsonNode *ide_dap_args_object(const char *k1, gint64 v1, const char *k2, gint64 v2);

void      ide_dap_frame_free(gpointer p);
void      ide_dap_var_node_free(gpointer p);
void      ide_dap_watch_item_free(gpointer p);
void      ide_dap_ui_clear_stopped(IdeDapView *dap);
void      ide_dap_ui_request_stack(IdeDapView *dap);
void      ide_dap_ui_build_panels(IdeDapView *dap, GtkWidget *cols);
void      ide_dap_ui_rebuild_watch_list(IdeDapView *dap);
void      ide_dap_ui_evaluate_watch(IdeDapView *dap, DapWatchItem *w);

G_END_DECLS

#endif /* KAJO_IDE_DAP_PRIVATE_H */
