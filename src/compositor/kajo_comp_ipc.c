/* kajo_comp_ipc.c — Client-side IPC helper for $XDG_RUNTIME_DIR/kajo-comp.sock (Phase 2d). */
#include "kajo_comp_ipc.h"
#include "../comp/core/comp_schema_keys.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

gchar *
kajo_comp_ipc_socket_path(void)
{
    const gchar *env_sock = g_getenv(COMP_IPC_ENV_SOCKET);
    if (env_sock && *env_sock)
        return g_strdup(env_sock);

    const gchar *niri_sock = g_getenv("NIRI_SOCKET");
    if (niri_sock && *niri_sock)
        return NULL;

    const gchar *xdg_run = g_getenv("XDG_RUNTIME_DIR");
    if (xdg_run && *xdg_run) {
        gchar *cand = g_build_filename(xdg_run, COMP_IPC_SOCK_NAME, NULL);
        if (g_file_test(cand, G_FILE_TEST_EXISTS))
            return cand;
        g_free(cand);
    }
    return NULL;
}

gboolean
kajo_comp_ipc_available(void)
{
    g_autofree gchar *path = kajo_comp_ipc_socket_path();
    return path != NULL && g_file_test(path, G_FILE_TEST_EXISTS);
}

gchar *
kajo_comp_ipc_send_request(const gchar *json_req)
{
    if (!json_req || !*json_req)
        return NULL;

    g_autofree gchar *socket_path = kajo_comp_ipc_socket_path();
    if (!socket_path)
        return NULL;

    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_UNIX,
                                             G_SOCKET_TYPE_STREAM,
                                             G_SOCKET_PROTOCOL_DEFAULT,
                                             &error);
    if (!socket)
        return NULL;

    g_socket_set_timeout(socket, 2);

    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    if (!g_socket_connect(socket, address, NULL, &error))
        return NULL;

    g_autoptr(GSocketConnection) conn = g_socket_connection_factory_create_connection(socket);
    if (!conn)
        return NULL;

    GOutputStream *ostream = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_autofree gchar *request = g_str_has_suffix(json_req, "\n")
                                    ? g_strdup(json_req)
                                    : g_strdup_printf("%s\n", json_req);
    gsize bytes_written = 0;
    gchar *response = NULL;

    if (g_output_stream_write_all(ostream, request, strlen(request), &bytes_written, NULL, &error)) {
        GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
        g_autoptr(GDataInputStream) data_input = g_data_input_stream_new(istream);
        response = g_data_input_stream_read_line_utf8(data_input, NULL, NULL, &error);
    }

    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    return response;
}

gboolean
kajo_comp_ipc_send_action(const gchar *action_name)
{
    if (!action_name || !*action_name)
        return FALSE;

    g_autofree gchar *req = g_strdup_printf("{\"" COMP_IPC_KEY_CMD "\":\"" COMP_IPC_CMD_ACTION
                                            "\",\"" COMP_IPC_KEY_NAME "\":\"%s\"}", action_name);
    g_autofree gchar *resp = kajo_comp_ipc_send_request(req);
    if (!resp)
        return FALSE;
    if (g_str_has_prefix(resp, "{\"error\"")) {
        g_warning("kajo-comp: action '%s' rejected: %s", action_name, resp);
        return FALSE;
    }
    return g_str_has_prefix(resp, "{\"ok\"");
}

gboolean
kajo_comp_ipc_toggle_floating(void)
{
    return kajo_comp_ipc_send_action(COMP_IPC_ACT_TOGGLE_WINDOW_FLOATING);
}

gboolean
kajo_comp_ipc_reload_config(void)
{
    return kajo_comp_ipc_send_action(COMP_IPC_ACT_RELOAD_CONFIG);
}
