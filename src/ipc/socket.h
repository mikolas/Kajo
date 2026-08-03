#ifndef SHELL_IPC_SOCKET_H
#define SHELL_IPC_SOCKET_H

#include <gio/gio.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellApp ShellApp;
typedef struct _ShellIPCSocket ShellIPCSocket;

ShellIPCSocket *shell_ipc_socket_new(ShellApp *app);
gboolean        shell_ipc_send_command(const gchar *command);
void            shell_ipc_socket_destroy(ShellIPCSocket *ipc);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_IPC_SOCKET_H */
