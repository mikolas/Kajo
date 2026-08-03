#include "socket.h"
#include "../shell.h"
#include "../launcher/launcher_surface.h"
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _ShellIPCSocket {
    ShellApp *app;
    GSocketService *service;
    gchar *socket_path;
    GArray *pending_sources;
};

static gchar *
get_ipc_socket_path(void)
{
    const gchar *runtime_dir = g_getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        runtime_dir = "/tmp";
    }
    return g_build_filename(runtime_dir, "kajo.sock", NULL);
}

static gboolean
toggle_launcher_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app && app->launcher) {
        shell_launcher_surface_toggle(app->launcher);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
show_launcher_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app && app->launcher) {
        shell_launcher_surface_show(app->launcher);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
hide_launcher_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app && app->launcher) {
        shell_launcher_surface_hide(app->launcher);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
brightness_up_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app) {
        shell_brightness_change(app, 5);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
brightness_down_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app) {
        shell_brightness_change(app, -5);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
volume_up_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app) {
        shell_volume_change(app, 5);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
volume_down_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app) {
        shell_volume_change(app, -5);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
volume_mute_idle_cb(gpointer user_data)
{
    ShellApp *app = user_data;
    if (app) {
        shell_volume_toggle_mute(app);
    }
    return G_SOURCE_REMOVE;
}

static void
add_idle_command(ShellIPCSocket *ipc, GSourceFunc func)
{
    if (!ipc || !ipc->app) return;
    guint id = g_idle_add(func, ipc->app);
    if (ipc->pending_sources) {
        for (guint i = 0; i < ipc->pending_sources->len; ) {
            guint pending_id = g_array_index(ipc->pending_sources, guint, i);
            GSource *src = g_main_context_find_source_by_id(NULL, pending_id);
            if (!src || g_source_is_destroyed(src)) {
                g_array_remove_index_fast(ipc->pending_sources, i);
            } else {
                i++;
            }
        }
        g_array_append_val(ipc->pending_sources, id);
    }
}

static gboolean
on_incoming_connection(GSocketService *service,
                       GSocketConnection *connection,
                       GObject *source_object,
                       gpointer user_data)
{
    ShellIPCSocket *ipc = user_data;
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GDataInputStream *data_input = g_data_input_stream_new(input);

    GError *error = NULL;
    gchar *line = g_data_input_stream_read_line(data_input, NULL, NULL, &error);

    if (error) {
        g_clear_error(&error);
    }

    if (line != NULL) {
        g_strstrip(line);
        if (g_strcmp0(line, "launcher-toggle") == 0 || g_strcmp0(line, "toggle-launcher") == 0) {
            add_idle_command(ipc, toggle_launcher_idle_cb);
        } else if (g_strcmp0(line, "launcher-open") == 0) {
            add_idle_command(ipc, show_launcher_idle_cb);
        } else if (g_strcmp0(line, "launcher-close") == 0) {
            add_idle_command(ipc, hide_launcher_idle_cb);
        } else if (g_strcmp0(line, "brightness-up") == 0 || g_strcmp0(line, "brightness+") == 0) {
            add_idle_command(ipc, brightness_up_idle_cb);
        } else if (g_strcmp0(line, "brightness-down") == 0 || g_strcmp0(line, "brightness-") == 0) {
            add_idle_command(ipc, brightness_down_idle_cb);
        } else if (g_strcmp0(line, "volume-up") == 0 || g_strcmp0(line, "volume+") == 0) {
            add_idle_command(ipc, volume_up_idle_cb);
        } else if (g_strcmp0(line, "volume-down") == 0 || g_strcmp0(line, "volume-") == 0) {
            add_idle_command(ipc, volume_down_idle_cb);
        } else if (g_strcmp0(line, "volume-mute") == 0 || g_strcmp0(line, "volume-toggle") == 0) {
            add_idle_command(ipc, volume_mute_idle_cb);
        }
        g_free(line);
    }

    g_object_unref(data_input);
    if (connection) {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    }
    return FALSE;
}

gboolean
shell_ipc_send_command(const gchar *command)
{
    gchar *path = get_ipc_socket_path();
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return FALSE;
    }

    GError *error = NULL;
    GSocket *socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (!socket) {
        if (error) g_clear_error(&error);
        g_free(path);
        return FALSE;
    }

    GSocketAddress *address = g_unix_socket_address_new(path);
    if (g_socket_connect(socket, address, NULL, NULL)) {
        GSocketConnection *conn = g_socket_connection_factory_create_connection(socket);
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

        gchar *msg = g_strconcat(command, "\n", NULL);
        g_output_stream_write_all(out, msg, strlen(msg), NULL, NULL, NULL);
        g_free(msg);

        g_object_unref(conn);
        g_object_unref(address);
        g_object_unref(socket);
        g_free(path);
        return TRUE;
    }

    g_object_unref(address);
    g_object_unref(socket);
    g_free(path);
    return FALSE;
}

ShellIPCSocket *
shell_ipc_socket_new(ShellApp *app)
{
    ShellIPCSocket *ipc = g_new0(ShellIPCSocket, 1);
    ipc->app = app;
    ipc->socket_path = get_ipc_socket_path();
    ipc->pending_sources = g_array_new(FALSE, FALSE, sizeof(guint));

    /* Unlink old socket file if left over */
    g_unlink(ipc->socket_path);

    GError *error = NULL;
    GSocketAddress *address = g_unix_socket_address_new(ipc->socket_path);
    ipc->service = g_socket_service_new();

    if (g_socket_listener_add_address(G_SOCKET_LISTENER(ipc->service), address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        g_signal_connect(ipc->service, "incoming", G_CALLBACK(on_incoming_connection), ipc);
        g_socket_service_start(ipc->service);
    } else {
        if (error != NULL)
            g_clear_error(&error);
    }

    g_object_unref(address);
    return ipc;
}

void
shell_ipc_socket_destroy(ShellIPCSocket *ipc)
{
    if (!ipc)
        return;

    if (ipc->pending_sources) {
        for (guint i = 0; i < ipc->pending_sources->len; i++) {
            guint id = g_array_index(ipc->pending_sources, guint, i);
            if (id > 0) {
                GSource *src = g_main_context_find_source_by_id(NULL, id);
                if (src && !g_source_is_destroyed(src)) {
                    g_source_destroy(src);
                }
            }
        }
        g_array_free(ipc->pending_sources, TRUE);
        ipc->pending_sources = NULL;
    }

    if (ipc->service) {
        g_socket_service_stop(ipc->service);
        g_object_unref(ipc->service);
    }

    if (ipc->socket_path) {
        g_unlink(ipc->socket_path);
        g_free(ipc->socket_path);
    }

    g_free(ipc);
}
