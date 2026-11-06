#ifndef SHELL_IPC_CLIENT_H
#define SHELL_IPC_CLIENT_H

#include <glib.h>

G_BEGIN_DECLS

/* Sends one line-delimited command to the running kajo shell over
 * $XDG_RUNTIME_DIR/kajo.sock. Returns FALSE if the shell is not running or the
 * connection fails. Shared by the `kajo` CLI and kajo-settings. */
gboolean shell_ipc_client_send(const gchar *command);

G_END_DECLS

#endif /* SHELL_IPC_CLIENT_H */
