#include "ipc_client.h"

#include <gio/gio.h>
#include <string.h>

gboolean
shell_ipc_client_send(const gchar *command)
{
    if (!command)
        return FALSE;

    const gchar *runtime_dir = g_getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir)
        runtime_dir = "/tmp";
    g_autofree gchar *path = g_build_filename(runtime_dir, "kajo.sock", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return FALSE;

    g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_UNIX,
                                             G_SOCKET_TYPE_STREAM,
                                             G_SOCKET_PROTOCOL_DEFAULT,
                                             NULL);
    if (!socket)
        return FALSE;

    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
    if (!g_socket_connect(socket, address, NULL, NULL))
        return FALSE;

    g_autoptr(GSocketConnection) conn = g_socket_connection_factory_create_connection(socket);
    if (!conn)
        return FALSE;

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_autofree gchar *msg = g_strconcat(command, "\n", NULL);
    gboolean ok = g_output_stream_write_all(out, msg, strlen(msg), NULL, NULL, NULL);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    return ok;
}
