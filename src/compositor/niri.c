#include "compositor.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Callback entry */
typedef struct {
    gpointer callback;
    gpointer userdata;
} CallbackEntry;

struct _ShellCompositor {
    GSocketConnection *conn;
    GDataInputStream  *input_stream;
    GCancellable      *cancellable;
    guint              reconnect_timer_id;

    GArray *workspaces;           /* CompositorWorkspace */
    CompositorWindow focused_window;
    CompositorKeyboardLayouts keyboard_layouts;
    GArray *windows;              /* CompositorWindow - all windows */

    GArray *workspaces_callbacks;       /* CallbackEntry */
    GArray *window_focus_callbacks;     /* CallbackEntry */
    GArray *keyboard_layout_callbacks;  /* CallbackEntry */
    GArray *windows_changed_callbacks;  /* CallbackEntry */

    JsonParser *parser;
    gboolean connected;
};

/* Forward declarations */
static void niri_connect(ShellCompositor *compositor);
static void niri_read_line_async(ShellCompositor *compositor);
static void niri_handle_event(ShellCompositor *compositor, const gchar *line);
static gboolean on_niri_reconnect_timer(gpointer user_data);
void niri_send_action(const gchar *json_str);

/* --- Helpers --- */

static void
workspace_clear(CompositorWorkspace *ws)
{
    g_free(ws->name);
    g_free(ws->output);
    ws->name = NULL;
    ws->output = NULL;
}

static void
window_clear(CompositorWindow *win)
{
    g_free(win->title);
    g_free(win->app_id);
    win->title = NULL;
    win->app_id = NULL;
    win->id = 0;
    win->workspace_id = 0;
    win->is_focused = FALSE;
}

static void
keyboard_layouts_clear(CompositorKeyboardLayouts *kl)
{
    g_strfreev(kl->names);
    kl->names = NULL;
    kl->current_idx = 0;
}

static void
emit_workspaces_callbacks(ShellCompositor *compositor)
{
    for (guint i = 0; i < compositor->workspaces_callbacks->len; i++) {
        CallbackEntry *entry = &g_array_index(compositor->workspaces_callbacks, CallbackEntry, i);
        ((CompositorWorkspacesCallback)entry->callback)(compositor, entry->userdata);
    }
}

static void
emit_window_focus_callbacks(ShellCompositor *compositor)
{
    for (guint i = 0; i < compositor->window_focus_callbacks->len; i++) {
        CallbackEntry *entry = &g_array_index(compositor->window_focus_callbacks, CallbackEntry, i);
        ((CompositorWindowFocusCallback)entry->callback)(compositor, entry->userdata);
    }
}

static void
emit_keyboard_layout_callbacks(ShellCompositor *compositor)
{
    for (guint i = 0; i < compositor->keyboard_layout_callbacks->len; i++) {
        CallbackEntry *entry = &g_array_index(compositor->keyboard_layout_callbacks, CallbackEntry, i);
        ((CompositorKeyboardLayoutCallback)entry->callback)(compositor, entry->userdata);
    }
}

static void
emit_windows_changed_callbacks(ShellCompositor *compositor)
{
    for (guint i = 0; i < compositor->windows_changed_callbacks->len; i++) {
        CallbackEntry *entry = &g_array_index(compositor->windows_changed_callbacks, CallbackEntry, i);
        ((CompositorWindowsChangedCallback)entry->callback)(compositor, entry->userdata);
    }
}

/* --- Event parsing --- */

static gint
compare_workspaces_by_idx(gconstpointer a, gconstpointer b)
{
    const CompositorWorkspace *ws1 = a;
    const CompositorWorkspace *ws2 = b;
    return (gint)ws1->idx - (gint)ws2->idx;
}

static void
handle_workspaces_changed(ShellCompositor *compositor, JsonObject *obj)
{
    JsonArray *arr = json_object_get_array_member(obj, "workspaces");
    if (!arr)
        return;

    /* Clear existing workspaces */
    for (guint i = 0; i < compositor->workspaces->len; i++)
        workspace_clear(&g_array_index(compositor->workspaces, CompositorWorkspace, i));
    g_array_set_size(compositor->workspaces, 0);

    guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; i++) {
        JsonObject *ws_obj = json_array_get_object_element(arr, i);
        CompositorWorkspace ws = {0};

        ws.id = (guint64)json_object_get_int_member(ws_obj, "id");
        ws.idx = (guint8)json_object_get_int_member(ws_obj, "idx");

        if (json_object_has_member(ws_obj, "name") &&
            !json_object_get_null_member(ws_obj, "name"))
            ws.name = g_strdup(json_object_get_string_member(ws_obj, "name"));

        if (json_object_has_member(ws_obj, "output") &&
            !json_object_get_null_member(ws_obj, "output"))
            ws.output = g_strdup(json_object_get_string_member(ws_obj, "output"));

        ws.is_active = json_object_get_boolean_member_with_default(ws_obj, "is_active", FALSE);
        ws.is_focused = json_object_get_boolean_member_with_default(ws_obj, "is_focused", FALSE);
        ws.is_urgent = json_object_get_boolean_member_with_default(ws_obj, "is_urgent", FALSE);

        if (json_object_has_member(ws_obj, "active_window_id") &&
            !json_object_get_null_member(ws_obj, "active_window_id"))
            ws.active_window_id = (guint64)json_object_get_int_member(ws_obj, "active_window_id");

        g_array_append_val(compositor->workspaces, ws);
    }

    g_array_sort(compositor->workspaces, compare_workspaces_by_idx);

    emit_workspaces_callbacks(compositor);
}

static void
handle_workspace_activated(ShellCompositor *compositor, JsonObject *obj)
{
    guint64 id = (guint64)json_object_get_int_member(obj, "id");
    gboolean focused = json_object_get_boolean_member_with_default(obj, "focused", FALSE);

    /* Find the activated workspace's output first */
    const gchar *activated_output = NULL;
    for (guint i = 0; i < compositor->workspaces->len; i++) {
        CompositorWorkspace *ws = &g_array_index(compositor->workspaces, CompositorWorkspace, i);
        if (ws->id == id) {
            activated_output = ws->output;
            break;
        }
    }

    /* Update all workspaces: clear is_active for same output (or when output is NULL), set for target */
    for (guint i = 0; i < compositor->workspaces->len; i++) {
        CompositorWorkspace *ws = &g_array_index(compositor->workspaces, CompositorWorkspace, i);

        if (ws->id == id) {
            ws->is_active = TRUE;
            ws->is_focused = focused;
        } else {
            if (!activated_output || !ws->output || g_strcmp0(ws->output, activated_output) == 0) {
                ws->is_active = FALSE;
            }
            if (focused) {
                ws->is_focused = FALSE;
            }
        }
    }

    emit_workspaces_callbacks(compositor);
}

static void
handle_window_focus_changed(ShellCompositor *compositor, JsonObject *obj)
{
    if (json_object_has_member(obj, "id") && !json_object_get_null_member(obj, "id")) {
        guint64 id = (guint64)json_object_get_int_member(obj, "id");
        window_clear(&compositor->focused_window);
        compositor->focused_window.id = id;
        compositor->focused_window.is_focused = TRUE;

        /* Look up title, app_id, workspace_id for the newly focused window */
        for (guint i = 0; i < compositor->windows->len; i++) {
            CompositorWindow *w = &g_array_index(compositor->windows, CompositorWindow, i);
            if (w->id == id) {
                compositor->focused_window.title = g_strdup(w->title);
                compositor->focused_window.app_id = g_strdup(w->app_id);
                compositor->focused_window.workspace_id = w->workspace_id;
                w->is_focused = TRUE;
            } else {
                w->is_focused = FALSE;
            }
        }
    } else {
        /* No window focused */
        window_clear(&compositor->focused_window);
        for (guint i = 0; i < compositor->windows->len; i++) {
            CompositorWindow *w = &g_array_index(compositor->windows, CompositorWindow, i);
            w->is_focused = FALSE;
        }
    }

    emit_window_focus_callbacks(compositor);
}

static void
handle_window_opened_or_changed(ShellCompositor *compositor, JsonObject *obj)
{
    JsonObject *win_obj = json_object_get_object_member(obj, "window");
    if (!win_obj)
        return;

    guint64 id = (guint64)json_object_get_int_member(win_obj, "id");
    gboolean is_focused = json_object_get_boolean_member_with_default(win_obj, "is_focused", FALSE);

    /* Extract window properties */
    gchar *title = NULL;
    gchar *app_id = NULL;
    guint64 workspace_id = 0;

    if (json_object_has_member(win_obj, "title") &&
        !json_object_get_null_member(win_obj, "title"))
        title = g_strdup(json_object_get_string_member(win_obj, "title"));

    if (json_object_has_member(win_obj, "app_id") &&
        !json_object_get_null_member(win_obj, "app_id"))
        app_id = g_strdup(json_object_get_string_member(win_obj, "app_id"));

    if (json_object_has_member(win_obj, "workspace_id") &&
        !json_object_get_null_member(win_obj, "workspace_id"))
        workspace_id = (guint64)json_object_get_int_member(win_obj, "workspace_id");

    /* Update windows array: find existing or add new */
    gboolean found = FALSE;
    for (guint i = 0; i < compositor->windows->len; i++) {
        CompositorWindow *w = &g_array_index(compositor->windows, CompositorWindow, i);
        if (w->id == id) {
            g_free(w->title);
            g_free(w->app_id);
            w->title = g_strdup(title);
            w->app_id = g_strdup(app_id);
            w->workspace_id = workspace_id;
            w->is_focused = is_focused;
            found = TRUE;
        } else if (is_focused) {
            w->is_focused = FALSE;
        }
    }
    if (!found) {
        CompositorWindow new_win = {0};
        new_win.id = id;
        new_win.title = g_strdup(title);
        new_win.app_id = g_strdup(app_id);
        new_win.workspace_id = workspace_id;
        new_win.is_focused = is_focused;
        g_array_append_val(compositor->windows, new_win);
    }

    emit_windows_changed_callbacks(compositor);

    /* Update focused window tracking */
    if (is_focused) {
        window_clear(&compositor->focused_window);
        compositor->focused_window.id = id;
        compositor->focused_window.is_focused = TRUE;
        compositor->focused_window.title = g_strdup(title);
        compositor->focused_window.app_id = g_strdup(app_id);
        compositor->focused_window.workspace_id = workspace_id;

        emit_window_focus_callbacks(compositor);
    } else if (compositor->focused_window.id == id) {
        /* Update info for the currently focused window */
        g_free(compositor->focused_window.title);
        g_free(compositor->focused_window.app_id);
        compositor->focused_window.title = g_strdup(title);
        compositor->focused_window.app_id = g_strdup(app_id);
        compositor->focused_window.workspace_id = workspace_id;

        emit_window_focus_callbacks(compositor);
    }

    g_free(title);
    g_free(app_id);
}

static void
handle_window_closed(ShellCompositor *compositor, JsonObject *obj)
{
    guint64 id = (guint64)json_object_get_int_member(obj, "id");

    /* Remove from windows array */
    for (guint i = 0; i < compositor->windows->len; i++) {
        CompositorWindow *w = &g_array_index(compositor->windows, CompositorWindow, i);
        if (w->id == id) {
            window_clear(w);
            g_array_remove_index(compositor->windows, i);
            break;
        }
    }

    emit_windows_changed_callbacks(compositor);

    if (compositor->focused_window.id == id) {
        window_clear(&compositor->focused_window);
        emit_window_focus_callbacks(compositor);
    }
}

static void
handle_keyboard_layouts_changed(ShellCompositor *compositor, JsonObject *obj)
{
    JsonObject *kl_obj = json_object_get_object_member(obj, "keyboard_layouts");
    if (!kl_obj)
        return;

    keyboard_layouts_clear(&compositor->keyboard_layouts);

    if (json_object_has_member(kl_obj, "names")) {
        JsonArray *names_arr = json_object_get_array_member(kl_obj, "names");
        guint len = json_array_get_length(names_arr);
        gchar **names = g_new0(gchar *, len + 1);
        for (guint i = 0; i < len; i++)
            names[i] = g_strdup(json_array_get_string_element(names_arr, i));
        names[len] = NULL;
        compositor->keyboard_layouts.names = names;
    }

    compositor->keyboard_layouts.current_idx =
        (guint8)json_object_get_int_member_with_default(kl_obj, "current_idx", 0);

    emit_keyboard_layout_callbacks(compositor);
}

static void
handle_keyboard_layout_switched(ShellCompositor *compositor, JsonObject *obj)
{
    compositor->keyboard_layouts.current_idx =
        (guint8)json_object_get_int_member(obj, "idx");

    emit_keyboard_layout_callbacks(compositor);
}

static void
handle_windows_changed(ShellCompositor *compositor, JsonObject *obj)
{
    JsonArray *arr = json_object_get_array_member(obj, "windows");
    if (!arr)
        return;

    /* Clear existing windows */
    for (guint i = 0; i < compositor->windows->len; i++)
        window_clear(&g_array_index(compositor->windows, CompositorWindow, i));
    g_array_set_size(compositor->windows, 0);

    guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; i++) {
        JsonObject *win_obj = json_array_get_object_element(arr, i);
        CompositorWindow win = {0};

        win.id = (guint64)json_object_get_int_member(win_obj, "id");

        if (json_object_has_member(win_obj, "title") &&
            !json_object_get_null_member(win_obj, "title"))
            win.title = g_strdup(json_object_get_string_member(win_obj, "title"));

        if (json_object_has_member(win_obj, "app_id") &&
            !json_object_get_null_member(win_obj, "app_id"))
            win.app_id = g_strdup(json_object_get_string_member(win_obj, "app_id"));

        if (json_object_has_member(win_obj, "workspace_id") &&
            !json_object_get_null_member(win_obj, "workspace_id"))
            win.workspace_id = (guint64)json_object_get_int_member(win_obj, "workspace_id");

        win.is_focused = json_object_get_boolean_member_with_default(win_obj, "is_focused", FALSE);

        g_array_append_val(compositor->windows, win);
    }

    emit_windows_changed_callbacks(compositor);
}

static void
niri_handle_event(ShellCompositor *compositor, const gchar *line)
{
    GError *error = NULL;

    if (!json_parser_load_from_data(compositor->parser, line, -1, &error)) {
        g_warning("niri: failed to parse event JSON: %s", error->message);
        g_error_free(error);
        return;
    }

    JsonNode *root = json_parser_get_root(compositor->parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
        return;

    JsonObject *root_obj = json_node_get_object(root);
    GList *members = json_object_get_members(root_obj);
    if (!members)
        return;

    const gchar *event_name = (const gchar *)members->data;
    JsonNode *event_node = json_object_get_member(root_obj, event_name);
    if (!event_node || !JSON_NODE_HOLDS_OBJECT(event_node)) {
        g_list_free(members);
        return;
    }

    JsonObject *event_obj = json_node_get_object(event_node);

    if (g_strcmp0(event_name, "WorkspacesChanged") == 0) {
        handle_workspaces_changed(compositor, event_obj);
    } else if (g_strcmp0(event_name, "WorkspaceActivated") == 0) {
        handle_workspace_activated(compositor, event_obj);
    } else if (g_strcmp0(event_name, "WindowFocusChanged") == 0) {
        handle_window_focus_changed(compositor, event_obj);
    } else if (g_strcmp0(event_name, "WindowOpenedOrChanged") == 0) {
        handle_window_opened_or_changed(compositor, event_obj);
    } else if (g_strcmp0(event_name, "WindowClosed") == 0) {
        handle_window_closed(compositor, event_obj);
    } else if (g_strcmp0(event_name, "WindowsChanged") == 0) {
        handle_windows_changed(compositor, event_obj);
    } else if (g_strcmp0(event_name, "KeyboardLayoutsChanged") == 0) {
        handle_keyboard_layouts_changed(compositor, event_obj);
    } else if (g_strcmp0(event_name, "KeyboardLayoutSwitched") == 0) {
        handle_keyboard_layout_switched(compositor, event_obj);
    }

    g_list_free(members);
}

/* --- Async line reading --- */

static void
on_line_read(GObject *source, GAsyncResult *result, gpointer userdata)
{
    ShellCompositor *compositor = (ShellCompositor *)userdata;
    GError *error = NULL;
    gsize length = 0;

    gchar *line = g_data_input_stream_read_line_finish_utf8(
        G_DATA_INPUT_STREAM(source), result, &length, &error);

    if (!compositor || (compositor->cancellable && g_cancellable_is_cancelled(compositor->cancellable)) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (error) g_clear_error(&error);
        if (line) g_free(line);
        return;
    }

    if (!line) {
        if (error) {
            g_warning("niri: error reading from socket: %s", error->message);
            g_error_free(error);
        } else {
            g_warning("niri: event stream connection closed, scheduling auto-reconnect...");
        }
        compositor->connected = FALSE;
        if (compositor->input_stream) {
            g_input_stream_close(G_INPUT_STREAM(compositor->input_stream), NULL, NULL);
            g_clear_object(&compositor->input_stream);
        }
        if (compositor->conn) {
            g_io_stream_close(G_IO_STREAM(compositor->conn), NULL, NULL);
            g_clear_object(&compositor->conn);
        }
        if (compositor->reconnect_timer_id == 0) {
            compositor->reconnect_timer_id = g_timeout_add_seconds(2, on_niri_reconnect_timer, compositor);
        }
        return;
    }

    niri_handle_event(compositor, line);
    g_free(line);

    /* Continue reading */
    niri_read_line_async(compositor);
}

static void
niri_read_line_async(ShellCompositor *compositor)
{
    if (!compositor || !compositor->input_stream || !compositor->cancellable)
        return;

    g_data_input_stream_read_line_async(
        compositor->input_stream,
        G_PRIORITY_DEFAULT,
        compositor->cancellable,
        on_line_read,
        compositor);
}

/* --- Connection --- */

static void
niri_connect(ShellCompositor *compositor)
{
    const gchar *socket_path = g_getenv("NIRI_SOCKET");
    if (!socket_path) {
        g_warning("niri: NIRI_SOCKET environment variable not set");
        return;
    }

    GError *error = NULL;
    GSocket *socket = g_socket_new(G_SOCKET_FAMILY_UNIX,
                                   G_SOCKET_TYPE_STREAM,
                                   G_SOCKET_PROTOCOL_DEFAULT,
                                   &error);
    if (!socket) {
        g_warning("niri: failed to create socket: %s", error->message);
        g_error_free(error);
        return;
    }

    GSocketAddress *address = g_unix_socket_address_new(socket_path);

    if (!g_socket_connect(socket, address, NULL, &error)) {
        g_warning("niri: failed to connect to %s: %s", socket_path, error->message);
        g_error_free(error);
        g_object_unref(socket);
        g_object_unref(address);
        return;
    }

    g_socket_set_blocking(socket, FALSE);

    compositor->conn = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    g_object_unref(address);

    if (!compositor->conn) {
        g_warning("niri: failed to create socket connection");
        return;
    }

    compositor->connected = TRUE;

    /* Send the EventStream request */
    GOutputStream *ostream = g_io_stream_get_output_stream(G_IO_STREAM(compositor->conn));
    const gchar *request = "\"EventStream\"\n";
    gsize bytes_written = 0;

    if (!g_output_stream_write_all(ostream, request, strlen(request),
                                   &bytes_written, NULL, &error)) {
        g_warning("niri: failed to send EventStream request: %s", error->message);
        g_error_free(error);
        g_clear_object(&compositor->conn);
        compositor->connected = FALSE;
        return;
    }

    /* Set up async line reading */
    GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(compositor->conn));
    compositor->input_stream = g_data_input_stream_new(istream);
    g_data_input_stream_set_newline_type(compositor->input_stream,
                                         G_DATA_STREAM_NEWLINE_TYPE_LF);

    /* Start reading events asynchronously */
    niri_read_line_async(compositor);
}

static gboolean
on_niri_reconnect_timer(gpointer user_data)
{
    ShellCompositor *compositor = user_data;
    if (!compositor) return G_SOURCE_REMOVE;

    if (compositor->connected) {
        compositor->reconnect_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    niri_connect(compositor);
    if (compositor->connected) {
        g_info("niri: successfully reconnected to Niri compositor EventStream!");
        compositor->reconnect_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

/* --- Actions (separate connection per action) --- */

void
niri_send_action(const gchar *json_str)
{
    const gchar *socket_path = g_getenv("NIRI_SOCKET");
    if (!socket_path) {
        g_warning("niri: NIRI_SOCKET not set, cannot send action");
        return;
    }

    GError *error = NULL;
    GSocket *socket = g_socket_new(G_SOCKET_FAMILY_UNIX,
                                   G_SOCKET_TYPE_STREAM,
                                   G_SOCKET_PROTOCOL_DEFAULT,
                                   &error);
    if (!socket) {
        g_warning("niri: action socket creation failed: %s", error->message);
        g_error_free(error);
        return;
    }

    GSocketAddress *address = g_unix_socket_address_new(socket_path);
    if (!g_socket_connect(socket, address, NULL, &error)) {
        g_warning("niri: action connect failed: %s", error->message);
        g_error_free(error);
        g_object_unref(socket);
        g_object_unref(address);
        return;
    }

    GSocketConnection *conn = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    g_object_unref(address);

    if (!conn) {
        g_warning("niri: failed to create action connection");
        return;
    }

    GOutputStream *ostream = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gchar *request = g_strdup_printf("%s\n", json_str);
    gsize bytes_written = 0;

    if (!g_output_stream_write_all(ostream, request, strlen(request),
                                   &bytes_written, NULL, &error)) {
        g_warning("niri: failed to send action: %s", error->message);
        g_error_free(error);
    } else {
        /* Read response (one line) */
        GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
        GDataInputStream *data_input = g_data_input_stream_new(istream);
        gchar *response = g_data_input_stream_read_line_utf8(data_input, NULL, NULL, &error);
        if (error) {
            g_warning("niri: failed to read action response: %s", error->message);
            g_error_free(error);
        }
        g_free(response);
        g_object_unref(data_input);
    }

    g_free(request);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_object_unref(conn);
}

gchar *
niri_send_ipc_request(const char *json_req)
{
    const gchar *socket_path = g_getenv("NIRI_SOCKET");
    if (!socket_path) return NULL;

    GError *error = NULL;
    GSocket *socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (!socket) {
        if (error) g_error_free(error);
        return NULL;
    }

    GSocketAddress *address = g_unix_socket_address_new(socket_path);
    if (!g_socket_connect(socket, address, NULL, &error)) {
        if (error) g_error_free(error);
        g_object_unref(socket);
        g_object_unref(address);
        return NULL;
    }

    GSocketConnection *conn = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    g_object_unref(address);

    if (!conn) return NULL;

    GOutputStream *ostream = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gchar *request = g_str_has_suffix(json_req, "\n") ? g_strdup(json_req) : g_strdup_printf("%s\n", json_req);
    gsize bytes_written = 0;
    gchar *response = NULL;

    if (g_output_stream_write_all(ostream, request, strlen(request), &bytes_written, NULL, &error)) {
        GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
        GDataInputStream *data_input = g_data_input_stream_new(istream);
        response = g_data_input_stream_read_line_utf8(data_input, NULL, NULL, &error);
        if (error) {
            g_error_free(error);
        }
        g_object_unref(data_input);
    } else {
        if (error) g_error_free(error);
    }

    g_free(request);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_object_unref(conn);
    return response;
}

/* --- Public API --- */

ShellCompositor *
shell_compositor_new(void)
{
    ShellCompositor *compositor = g_new0(ShellCompositor, 1);

    compositor->cancellable = g_cancellable_new();
    compositor->workspaces = g_array_new(FALSE, TRUE, sizeof(CompositorWorkspace));
    compositor->windows = g_array_new(FALSE, TRUE, sizeof(CompositorWindow));
    compositor->workspaces_callbacks = g_array_new(FALSE, FALSE, sizeof(CallbackEntry));
    compositor->window_focus_callbacks = g_array_new(FALSE, FALSE, sizeof(CallbackEntry));
    compositor->keyboard_layout_callbacks = g_array_new(FALSE, FALSE, sizeof(CallbackEntry));
    compositor->windows_changed_callbacks = g_array_new(FALSE, FALSE, sizeof(CallbackEntry));
    compositor->parser = json_parser_new();

    memset(&compositor->focused_window, 0, sizeof(CompositorWindow));
    memset(&compositor->keyboard_layouts, 0, sizeof(CompositorKeyboardLayouts));

    niri_connect(compositor);

    return compositor;
}

void
shell_compositor_destroy(ShellCompositor *compositor)
{
    if (!compositor)
        return;

    if (compositor->reconnect_timer_id != 0) {
        g_source_remove(compositor->reconnect_timer_id);
        compositor->reconnect_timer_id = 0;
    }

    if (compositor->cancellable) {
        g_cancellable_cancel(compositor->cancellable);
    }

    if (compositor->input_stream) {
        g_input_stream_close(G_INPUT_STREAM(compositor->input_stream), NULL, NULL);
        g_clear_object(&compositor->input_stream);
    }
    if (compositor->conn) {
        g_io_stream_close(G_IO_STREAM(compositor->conn), NULL, NULL);
        g_clear_object(&compositor->conn);
    }
    g_clear_object(&compositor->cancellable);

    /* Clean up workspaces */
    for (guint i = 0; i < compositor->workspaces->len; i++)
        workspace_clear(&g_array_index(compositor->workspaces, CompositorWorkspace, i));
    g_array_free(compositor->workspaces, TRUE);

    /* Clean up windows */
    for (guint i = 0; i < compositor->windows->len; i++)
        window_clear(&g_array_index(compositor->windows, CompositorWindow, i));
    g_array_free(compositor->windows, TRUE);

    /* Clean up focused window */
    window_clear(&compositor->focused_window);

    /* Clean up keyboard layouts */
    keyboard_layouts_clear(&compositor->keyboard_layouts);

    /* Clean up callbacks */
    g_array_free(compositor->workspaces_callbacks, TRUE);
    g_array_free(compositor->window_focus_callbacks, TRUE);
    g_array_free(compositor->keyboard_layout_callbacks, TRUE);
    g_array_free(compositor->windows_changed_callbacks, TRUE);

    g_clear_object(&compositor->parser);

    g_free(compositor);
}

const GArray *
shell_compositor_get_workspaces(ShellCompositor *compositor)
{
    g_return_val_if_fail(compositor != NULL, NULL);
    return compositor->workspaces;
}

const CompositorWindow *
shell_compositor_get_focused_window(ShellCompositor *compositor)
{
    g_return_val_if_fail(compositor != NULL, NULL);
    if (compositor->focused_window.id == 0)
        return NULL;
    return &compositor->focused_window;
}

const CompositorKeyboardLayouts *
shell_compositor_get_keyboard_layouts(ShellCompositor *compositor)
{
    g_return_val_if_fail(compositor != NULL, NULL);
    if (!compositor->keyboard_layouts.names)
        return NULL;
    return &compositor->keyboard_layouts;
}

void
shell_compositor_on_workspaces_changed(ShellCompositor *compositor,
                                       CompositorWorkspacesCallback cb,
                                       gpointer userdata)
{
    g_return_if_fail(compositor != NULL);
    g_return_if_fail(cb != NULL);
    CallbackEntry entry = { .callback = (gpointer)cb, .userdata = userdata };
    g_array_append_val(compositor->workspaces_callbacks, entry);

    if (compositor->workspaces && compositor->workspaces->len > 0) {
        cb(compositor, userdata);
    }
}

void
shell_compositor_on_window_focus_changed(ShellCompositor *compositor,
                                         CompositorWindowFocusCallback cb,
                                         gpointer userdata)
{
    g_return_if_fail(compositor != NULL);
    g_return_if_fail(cb != NULL);
    CallbackEntry entry = { .callback = (gpointer)cb, .userdata = userdata };
    g_array_append_val(compositor->window_focus_callbacks, entry);
}

void
shell_compositor_on_keyboard_layout_changed(ShellCompositor *compositor,
                                            CompositorKeyboardLayoutCallback cb,
                                            gpointer userdata)
{
    g_return_if_fail(compositor != NULL);
    g_return_if_fail(cb != NULL);
    CallbackEntry entry = { .callback = (gpointer)cb, .userdata = userdata };
    g_array_append_val(compositor->keyboard_layout_callbacks, entry);
}

void
shell_compositor_on_windows_changed(ShellCompositor *compositor,
                                    CompositorWindowsChangedCallback cb,
                                    gpointer userdata)
{
    g_return_if_fail(compositor != NULL);
    g_return_if_fail(cb != NULL);
    CallbackEntry entry = { .callback = (gpointer)cb, .userdata = userdata };
    g_array_append_val(compositor->windows_changed_callbacks, entry);

    if (compositor->windows && compositor->windows->len > 0) {
        cb(compositor, userdata);
    }
}

static void
remove_callback_from_array(GArray *arr, gpointer userdata)
{
    if (!arr) return;
    for (guint i = 0; i < arr->len; ) {
        CallbackEntry *entry = &g_array_index(arr, CallbackEntry, i);
        if (entry->userdata == userdata) {
            g_array_remove_index(arr, i);
        } else {
            i++;
        }
    }
}

void
shell_compositor_remove_callbacks(ShellCompositor *compositor, gpointer userdata)
{
    if (!compositor || !userdata) return;
    remove_callback_from_array(compositor->workspaces_callbacks, userdata);
    remove_callback_from_array(compositor->window_focus_callbacks, userdata);
    remove_callback_from_array(compositor->keyboard_layout_callbacks, userdata);
    remove_callback_from_array(compositor->windows_changed_callbacks, userdata);
}

void
shell_compositor_switch_workspace(ShellCompositor *compositor, guint64 workspace_id)
{
    g_return_if_fail(compositor != NULL);

    gchar *json = g_strdup_printf(
        "{\"Action\":{\"FocusWorkspace\":{\"reference\":{\"Id\":%" G_GUINT64_FORMAT "}}}}",
        workspace_id);
    niri_send_action(json);
    g_free(json);
}

void
shell_compositor_switch_keyboard_layout_idx(ShellCompositor *compositor, guint8 idx)
{
    g_return_if_fail(compositor != NULL);

    gchar *json = g_strdup_printf(
        "{\"Action\":{\"SwitchLayout\":{\"layout\":{\"Index\":%u}}}}", (guint)idx);
    niri_send_action(json);
    g_free(json);
}

const GArray *
shell_compositor_get_windows(ShellCompositor *compositor)
{
    g_return_val_if_fail(compositor != NULL, NULL);
    return compositor->windows;
}

static void
compositor_window_element_clear(gpointer data)
{
    CompositorWindow *w = data;
    if (w) {
        g_free(w->title);
        g_free(w->app_id);
    }
}

GArray *
shell_compositor_get_windows_for_workspace(ShellCompositor *compositor, guint64 workspace_id)
{
    g_return_val_if_fail(compositor != NULL, NULL);

    GArray *result = g_array_new(FALSE, TRUE, sizeof(CompositorWindow));
    g_array_set_clear_func(result, compositor_window_element_clear);

    for (guint i = 0; i < compositor->windows->len; i++) {
        CompositorWindow *w = &g_array_index(compositor->windows, CompositorWindow, i);
        if (w->workspace_id == workspace_id) {
            CompositorWindow copy = {0};
            copy.id = w->id;
            copy.title = g_strdup(w->title);
            copy.app_id = g_strdup(w->app_id);
            copy.workspace_id = w->workspace_id;
            copy.is_focused = w->is_focused;
            g_array_append_val(result, copy);
        }
    }

    return result;
}

void
shell_compositor_focus_window(ShellCompositor *compositor, guint64 window_id)
{
    g_return_if_fail(compositor != NULL);

    gchar *json = g_strdup_printf(
        "{\"Action\":{\"FocusWindow\":{\"id\":%" G_GUINT64_FORMAT "}}}", window_id);
    niri_send_action(json);
    g_free(json);
}

void
shell_compositor_focus_app(ShellCompositor *compositor, const gchar *app_name, const gchar *desktop_entry, const gchar *app_icon)
{
    if (!compositor) return;
    const GArray *windows = shell_compositor_get_windows(compositor);
    if (!windows || windows->len == 0) return;

    gchar *de_fold = (desktop_entry && *desktop_entry) ? g_utf8_casefold(desktop_entry, -1) : NULL;
    gchar *app_fold = (app_name && *app_name) ? g_utf8_casefold(app_name, -1) : NULL;
    gchar *icon_fold = (app_icon && *app_icon) ? g_utf8_casefold(app_icon, -1) : NULL;

    /* Stripping suffix .desktop if present */
    if (de_fold && g_str_has_suffix(de_fold, ".desktop")) {
        de_fold[strlen(de_fold) - 8] = '\0';
    }

    /* Pass 0: PWA & Web App Specific Window Title Priority Match */
    if (app_fold && *app_fold) {
        for (guint i = 0; i < windows->len; i++) {
            const CompositorWindow *win = &g_array_index(windows, CompositorWindow, i);
            if (!win->title) continue;
            gchar *title_fold = g_utf8_casefold(win->title, -1);
            if (title_fold && *title_fold && strstr(title_fold, app_fold)) {
                shell_compositor_focus_window(compositor, win->id);
                g_free(title_fold);
                if (de_fold) g_free(de_fold);
                if (app_fold) g_free(app_fold);
                if (icon_fold) g_free(icon_fold);
                return;
            }
            if (title_fold) g_free(title_fold);
        }
    }

    /* Pass 1: Match desktop_entry against win->app_id (Skip generic browser fallback if app_fold is set) */
    gboolean is_generic_browser = (de_fold && (strstr(de_fold, "chrome") || strstr(de_fold, "chromium") || strstr(de_fold, "firefox") || strstr(de_fold, "brave")));
    if (de_fold && *de_fold && (!is_generic_browser || !app_fold)) {
        for (guint i = 0; i < windows->len; i++) {
            const CompositorWindow *win = &g_array_index(windows, CompositorWindow, i);
            if (win->app_id && *win->app_id) {
                gchar *win_app = g_utf8_casefold(win->app_id, -1);
                if (*win_app && (strstr(win_app, de_fold) || strstr(de_fold, win_app))) {
                    shell_compositor_focus_window(compositor, win->id);
                    g_free(win_app);
                    if (de_fold) g_free(de_fold);
                    if (app_fold) g_free(app_fold);
                    if (icon_fold) g_free(icon_fold);
                    return;
                }
                g_free(win_app);
            }
        }
    }

    /* Pass 2: Substring match on app_name & app_icon against win->app_id or win->title (PWAs & Web Apps) */
    for (guint i = 0; i < windows->len; i++) {
        const CompositorWindow *win = &g_array_index(windows, CompositorWindow, i);
        if (!win->title && !win->app_id) continue;

        gchar *title_fold = (win->title && *win->title) ? g_utf8_casefold(win->title, -1) : NULL;
        gchar *win_app_fold = (win->app_id && *win->app_id) ? g_utf8_casefold(win->app_id, -1) : NULL;

        gboolean match = FALSE;
        if (app_fold && *app_fold) {
            if (win_app_fold && *win_app_fold && strstr(win_app_fold, app_fold)) match = TRUE;
            if (!match && title_fold && *title_fold && strstr(title_fold, app_fold)) match = TRUE;
        }
        if (!match && icon_fold && *icon_fold) {
            if (win_app_fold && *win_app_fold && strstr(win_app_fold, icon_fold)) match = TRUE;
            if (!match && title_fold && *title_fold && strstr(title_fold, icon_fold)) match = TRUE;
        }

        if (title_fold) g_free(title_fold);
        if (win_app_fold) g_free(win_app_fold);

        if (match) {
            shell_compositor_focus_window(compositor, win->id);
            if (de_fold) g_free(de_fold);
            if (app_fold) g_free(app_fold);
            if (icon_fold) g_free(icon_fold);
            return;
        }
    }

    /* Pass 3: Tokenized word matching (PWAs like Teams, Discord, Spotify) */
    if (app_fold && *app_fold) {
        gchar **tokens = g_strsplit_set(app_fold, " -._", -1);
        for (guint t = 0; tokens[t] != NULL; t++) {
            g_strstrip(tokens[t]);
            if (strlen(tokens[t]) < 3) continue;

            for (guint i = 0; i < windows->len; i++) {
                const CompositorWindow *win = &g_array_index(windows, CompositorWindow, i);
                gchar *win_app = (win->app_id && *win->app_id) ? g_utf8_casefold(win->app_id, -1) : NULL;
                gchar *title_fold = (win->title && *win->title) ? g_utf8_casefold(win->title, -1) : NULL;

                gboolean match = FALSE;
                if (win_app && *win_app && strstr(win_app, tokens[t])) match = TRUE;
                if (!match && title_fold && *title_fold && strstr(title_fold, tokens[t])) match = TRUE;

                if (win_app) g_free(win_app);
                if (title_fold) g_free(title_fold);

                if (match) {
                    shell_compositor_focus_window(compositor, win->id);
                    g_strfreev(tokens);
                    if (de_fold) g_free(de_fold);
                    if (app_fold) g_free(app_fold);
                    if (icon_fold) g_free(icon_fold);
                    return;
                }
            }
        }
        g_strfreev(tokens);
    }

    if (de_fold) g_free(de_fold);
    if (app_fold) g_free(app_fold);
    if (icon_fold) g_free(icon_fold);
}

void
shell_compositor_close_window(ShellCompositor *compositor, guint64 window_id)
{
    g_return_if_fail(compositor != NULL);

    gchar *json = g_strdup_printf(
        "{\"Action\":{\"CloseWindow\":{\"id\":%" G_GUINT64_FORMAT "}}}", window_id);
    niri_send_action(json);
    g_free(json);
}

