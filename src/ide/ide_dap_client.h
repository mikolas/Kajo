#ifndef KAJO_IDE_DAP_CLIENT_H
#define KAJO_IDE_DAP_CLIENT_H

/* Debug Adapter Protocol client (GTK-free). Talks to a debug adapter over the
 * adapter's stdin/stdout (gdb -i=dap, lldb-dap, dlv dap, ...) or over any
 * GIO stream pair (e.g. a TCP socket for the Java debug server). */

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "ide_jsonrpc.h"

G_BEGIN_DECLS

typedef struct _IdeDapClient IdeDapClient;

/* `body` is NULL when the response carried none. On failure `success` is FALSE
 * and `message` describes the error. Borrowed: valid during the callback. */
typedef void (*IdeDapResponseCb)(IdeDapClient *client,
                                 gboolean      success,
                                 JsonObject   *body,
                                 const char   *message,
                                 gpointer      user_data);

typedef void (*IdeDapEventCb)(IdeDapClient *client,
                              const char   *event,
                              JsonObject   *body,
                              gpointer      user_data);

/* The adapter closed its output / exited. Do not free the client inside. */
typedef void (*IdeDapExitCb)(IdeDapClient *client, gpointer user_data);

IdeDapClient *ide_dap_client_new_process(const char *const *argv, const char *cwd);
IdeDapClient *ide_dap_client_new_streams(GInputStream *in, GOutputStream *out);
void          ide_dap_client_free(IdeDapClient *client);

void          ide_dap_client_set_callbacks(IdeDapClient  *client,
                                           IdeDapEventCb  event_cb,
                                           IdeDapExitCb   exit_cb,
                                           gpointer       user_data);

/* Spawns the process (no-op for stream clients) and starts reading. */
gboolean      ide_dap_client_start(IdeDapClient *client, GError **error);
/* Kills the adapter process / closes the streams. Idempotent. */
void          ide_dap_client_stop(IdeDapClient *client);
gboolean      ide_dap_client_is_alive(const IdeDapClient *client);

/* `arguments` ownership is transferred (may be NULL). Returns the request seq. */
guint         ide_dap_client_request(IdeDapClient    *client,
                                     const char      *command,
                                     JsonNode        *arguments,
                                     IdeDapResponseCb cb,
                                     gpointer         user_data);

/* Pumps the default main context until the process exited or the timeout hit. */
gboolean      ide_dap_client_wait_exit(IdeDapClient *client, guint timeout_ms);

G_END_DECLS

#endif
