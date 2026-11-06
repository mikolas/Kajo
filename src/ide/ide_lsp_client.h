#ifndef KAJO_IDE_LSP_CLIENT_H
#define KAJO_IDE_LSP_CLIENT_H

/* Asynchronous Language Server Protocol client (GTK-free).
 *
 * One IdeLspClient owns one language-server process and speaks JSON-RPC 2.0
 * with Content-Length framing over its stdin/stdout. Everything is driven by
 * the GLib main loop: reads, writes and process-exit watching are all async,
 * so the UI never blocks on a server. Requests and notifications issued before
 * the server finished `initialize` are queued and flushed, in order, right
 * after `initialized`. */

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "ide_jsonrpc.h"

G_BEGIN_DECLS

typedef struct _IdeLspClient IdeLspClient;

typedef enum {
  IDE_LSP_CLIENT_IDLE = 0,   /* created, not started */
  IDE_LSP_CLIENT_STARTING,   /* process spawned, waiting for initialize result */
  IDE_LSP_CLIENT_READY,
  IDE_LSP_CLIENT_STOPPED,    /* process exited or failed to start */
} IdeLspClientState;

/* `result` is NULL on error (then `error_message` is set). For a successful
 * response whose result is JSON null, `result` is a JSON null node. Borrowed:
 * valid only during the callback. */
typedef void (*IdeLspResponseCb)(IdeLspClient *client,
                                 JsonNode     *result,
                                 const char   *error_message,
                                 gpointer      user_data);

typedef void (*IdeLspNotificationCb)(IdeLspClient *client,
                                     const char   *method,
                                     JsonNode     *params,
                                     gpointer      user_data);

/* Server -> client requests (workspace/applyEdit, ...). Return the result node
 * (ownership transferred) or NULL for a JSON null result. */
typedef JsonNode *(*IdeLspServerRequestCb)(IdeLspClient *client,
                                           const char   *method,
                                           JsonNode     *params,
                                           gpointer      user_data);

typedef void (*IdeLspStateCb)(IdeLspClient     *client,
                              IdeLspClientState state,
                              gpointer          user_data);

/* `argv` is NULL-terminated and copied. `root_path` becomes the workspace
 * folder / rootUri. `init_options` (optional, ownership transferred) is sent as
 * initializationOptions. */
IdeLspClient *ide_lsp_client_new(const char  *server_id,
                                 const char *const *argv,
                                 const char  *root_path,
                                 JsonNode    *init_options);
void          ide_lsp_client_free(IdeLspClient *client);

void          ide_lsp_client_set_callbacks(IdeLspClient          *client,
                                           IdeLspNotificationCb   notification_cb,
                                           IdeLspServerRequestCb  server_request_cb,
                                           IdeLspStateCb          state_cb,
                                           gpointer               user_data);

gboolean      ide_lsp_client_start(IdeLspClient *client, GError **error);
/* Sends shutdown + exit, then force-kills the process after a short grace. */
void          ide_lsp_client_stop(IdeLspClient *client);

/* Pumps the default main context until the server process exited (or the
 * timeout elapsed). Used at shutdown so a JVM-based server is not orphaned. */
gboolean      ide_lsp_client_wait_exit(IdeLspClient *client, guint timeout_ms);

IdeLspClientState ide_lsp_client_get_state(const IdeLspClient *client);
const char       *ide_lsp_client_get_server_id(const IdeLspClient *client);
const char       *ide_lsp_client_get_root_path(const IdeLspClient *client);

/* `params` ownership is transferred. */
void          ide_lsp_client_notify(IdeLspClient *client,
                                    const char   *method,
                                    JsonNode     *params);
guint         ide_lsp_client_request(IdeLspClient     *client,
                                     const char       *method,
                                     JsonNode         *params,
                                     IdeLspResponseCb  cb,
                                     gpointer          user_data);
/* Drops the callback and tells the server to abandon the request. */
void          ide_lsp_client_cancel(IdeLspClient *client, guint request_id);

/* Capability lookup in the server's InitializeResult.capabilities, e.g.
 * "renameProvider" or "completionProvider.triggerCharacters". TRUE when present
 * and not `false`/null. */
gboolean      ide_lsp_client_has_capability(const IdeLspClient *client, const char *path);
JsonNode     *ide_lsp_client_get_capability(const IdeLspClient *client, const char *path);

/* JSON helpers */
JsonNode     *ide_lsp_position_params(const char *uri, guint line0, guint character_utf16);

G_END_DECLS

#endif /* KAJO_IDE_LSP_CLIENT_H */
