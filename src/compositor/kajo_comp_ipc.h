/* kajo_comp_ipc.h — Client-side IPC helper for $XDG_RUNTIME_DIR/kajo-comp.sock (Phase 2d).
 *
 * Used by Kajo suite clients (`kajo`, `kajo-settings`, etc.) to send JSON requests,
 * execute compositor actions, and parse live subscription events from `kajo-comp`.
 */
#ifndef KAJO_COMP_IPC_H
#define KAJO_COMP_IPC_H

#include "compositor.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the resolved kajo-comp socket path (caller frees with g_free), or NULL. */
gchar   *kajo_comp_ipc_socket_path(void);

/* Returns TRUE if $KAJO_COMP_SOCKET (or $XDG_RUNTIME_DIR/kajo-comp.sock) exists. */
gboolean kajo_comp_ipc_available(void);

/* Sends a single newline-delimited JSON request and returns the raw response line
 * (caller frees with g_free), or NULL on socket error. */
gchar   *kajo_comp_ipc_send_request(const gchar *json_req);

/* Sends {"cmd":"action","name":<action_name>} asynchronously (or synchronously when short). */
gboolean kajo_comp_ipc_send_action(const gchar *action_name);

/* Convenience helpers matching niri_toggle_window_floating() / niri_reload_config(). */
gboolean kajo_comp_ipc_toggle_floating(void);
gboolean kajo_comp_ipc_reload_config(void);
gboolean kajo_comp_ipc_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* KAJO_COMP_IPC_H */
