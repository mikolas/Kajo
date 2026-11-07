#ifndef KAJO_IDE_DAP_JAVA_H
#define KAJO_IDE_DAP_JAVA_H

/* Java debugging through jdtls + Microsoft's java-debug bundle (GTK-free).
 *
 * jdtls (with the bundle loaded via initializationOptions.bundles) exposes the
 * debugger through workspace/executeCommand: we resolve the main class and
 * classpath of the active file, ask jdtls to open a debug server
 * (`vscode.java.startDebugSession` -> TCP port) and hand back the connection and
 * the DAP `launch` arguments. */

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Result of one jdtls command: `result` is borrowed; on failure NULL + `error`. */
typedef void (*IdeJavaExecCb)(JsonNode *result, const char *error, gpointer user_data);

/* Host hook that runs `workspace/executeCommand` on the jdtls attached to the
 * file. Returns FALSE when there is no ready server (callback not invoked).
 * `arguments` ownership is transferred. */
typedef gboolean (*IdeJavaExecFn)(const char    *command,
                                  JsonNode      *arguments,
                                  IdeJavaExecCb  cb,
                                  gpointer       cb_data,
                                  gpointer       host_data);

/* `conn` (transferred) is the connected debug-server socket; `launch_args`
 * (transferred) is the body of the DAP `launch` request. On failure both are
 * NULL and `error` describes why. */
typedef void (*IdeJavaDebugReadyCb)(GIOStream *conn,
                                    JsonNode  *launch_args,
                                    const char *error,
                                    gpointer   user_data);

void ide_java_debug_prepare(const char          *file_path,
                            const char          *workspace_root,
                            IdeJavaExecFn        exec,
                            gpointer             host_data,
                            IdeJavaDebugReadyCb  ready_cb,
                            gpointer             user_data);

void ide_java_debug_prepare_cancellable(const char          *file_path,
                                        const char          *workspace_root,
                                        GCancellable        *cancellable,
                                        IdeJavaExecFn        exec,
                                        gpointer             host_data,
                                        IdeJavaDebugReadyCb  ready_cb,
                                        gpointer             user_data);

G_END_DECLS

#endif
