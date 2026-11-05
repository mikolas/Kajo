#ifndef KAJO_IDE_AGENT_IPC_H
#define KAJO_IDE_AGENT_IPC_H

#include "ide_buffer.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  char *path;
  char *old_content;
  char *new_content;
  char *unified_diff;
  guint added_lines;
  guint removed_lines;
} IdeAgentTxItem;

typedef struct _IdeAgentIpc IdeAgentIpc;

typedef void (*IdeAgentOpenCb)(const char *path,
                               guint       line_1based,
                               guint       col_1based,
                               gpointer    user_data);

typedef void (*IdeAgentReloadCb)(const char *path,
                                 const char *old_snapshot_opt,
                                 gpointer    user_data);

typedef void (*IdeAgentStatusCb)(const char *badge_text,
                                 const char *toast_msg,
                                 guint       pending_tx_count,
                                 gpointer    user_data);

IdeAgentIpc *ide_agent_ipc_new(IdeAgentOpenCb   open_cb,
                               IdeAgentReloadCb reload_cb,
                               IdeAgentStatusCb status_cb,
                               gpointer         user_data);
void         ide_agent_ipc_free(IdeAgentIpc *ipc);

/* Starts Unix domain socket server on default $XDG_RUNTIME_DIR paths or custom path */
gboolean     ide_agent_ipc_start_default(IdeAgentIpc *ipc);
gboolean     ide_agent_ipc_start_on_path(IdeAgentIpc *ipc,
                                         const char  *socket_path,
                                         GError     **error);

/* Pure JSON command dispatcher (used by socket handler and unit tests) */
char        *ide_agent_ipc_dispatch_json(IdeAgentIpc *ipc, const char *json_request);

/* Batch transaction management (Space d s, Space d a, Space d r) */
void         ide_agent_ipc_record_edit(IdeAgentIpc *ipc,
                                       const char  *path,
                                       const char  *old_content,
                                       const char  *new_content);
guint        ide_agent_ipc_get_pending_count(const IdeAgentIpc *ipc);
void         ide_agent_ipc_accept_all(IdeAgentIpc *ipc);
void         ide_agent_ipc_reject_all(IdeAgentIpc *ipc);

/* Agent Transaction Review Drawer widget */
GtkWidget   *ide_agent_ipc_get_drawer_widget(IdeAgentIpc *ipc);

G_END_DECLS

#endif /* KAJO_IDE_AGENT_IPC_H */
