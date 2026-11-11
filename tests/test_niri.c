/* Tests for the Niri IPC layer (src/compositor/niri.c) against a mock Niri
 * socket: the exact JSON shape of the actions we send, and the EventStream
 * parser. Regression coverage for the FLOAT / TILE bug, where
 * {"Action":"ToggleWindowFloating"} was sent and silently rejected by Niri
 * (the action is a struct variant and must be {"Action":{"ToggleWindowFloating":{}}}). */

#include "../src/compositor/compositor.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

static GAsyncQueue *g_requests = NULL; /* char* lines received on non-EventStream connections */
static gint g_server_done = 0;         /* atomic: lets the EventStream handler return */

static const char *const kEvents[] = {
    "{\"WorkspacesChanged\":{\"workspaces\":["
    "{\"id\":2,\"idx\":2,\"name\":null,\"output\":\"eDP-1\",\"is_active\":false,"
    "\"is_focused\":false,\"is_urgent\":false,\"active_window_id\":null},"
    "{\"id\":1,\"idx\":1,\"name\":\"main\",\"output\":\"eDP-1\",\"is_active\":true,"
    "\"is_focused\":true,\"is_urgent\":false,\"active_window_id\":10}]}}\n",

    "{\"WindowsChanged\":{\"windows\":["
    "{\"id\":10,\"title\":\"Terminal\",\"app_id\":\"foot\",\"workspace_id\":1,\"is_focused\":true},"
    "{\"id\":11,\"title\":\"Docs\",\"app_id\":\"firefox\",\"workspace_id\":2,\"is_focused\":false}]}}\n",

    "{\"WindowFocusChanged\":{\"id\":11}}\n",

    "{\"KeyboardLayoutsChanged\":{\"keyboard_layouts\":"
    "{\"names\":[\"English (US)\",\"Finnish\"],\"current_idx\":1}}}\n",
};

static const char *const kKajoCompEvents[] = {
    "{\"event\":\"WorkspacesChanged\",\"workspaces\":["
    "{\"id\":2,\"idx\":2,\"name\":null,\"output\":\"eDP-1\",\"is_active\":false,"
    "\"is_focused\":false,\"is_urgent\":false,\"active_window_id\":null},"
    "{\"id\":1,\"idx\":1,\"name\":\"main\",\"output\":\"eDP-1\",\"is_active\":true,"
    "\"is_focused\":true,\"is_urgent\":false,\"active_window_id\":10}]}\n",

    "{\"event\":\"WindowsChanged\",\"windows\":["
    "{\"id\":10,\"title\":\"Terminal\",\"app_id\":\"foot\",\"workspace_id\":1,\"is_focused\":true},"
    "{\"id\":11,\"title\":\"Docs\",\"app_id\":\"firefox\",\"workspace_id\":2,\"is_focused\":false}]}\n",

    "{\"event\":\"WindowFocusChanged\",\"id\":11}\n",

    "{\"event\":\"KeyboardLayoutsChanged\",\"names\":[\"English (US)\",\"Finnish\"],\"current_idx\":1}\n",

    "{\"event\":\"TouchBezelSwipe\",\"edge\":\"bottom\"}\n",
};

static gpointer
handle_mock_connection(gpointer data)
{
    g_autoptr(GSocketConnection) conn = data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_autoptr(GDataInputStream) lines = g_data_input_stream_new(in);

    gsize len = 0;
    g_autofree char *line = g_data_input_stream_read_line(lines, &len, NULL, NULL);
    if (!line)
        return NULL;

    if (g_strcmp0(line, "\"EventStream\"") == 0) {
        const char *ack = "{\"Ok\":\"Handled\"}\n";
        g_output_stream_write_all(out, ack, strlen(ack), NULL, NULL, NULL);
        for (gsize i = 0; i < G_N_ELEMENTS(kEvents); i++)
            g_output_stream_write_all(out, kEvents[i], strlen(kEvents[i]), NULL, NULL, NULL);
        /* Keep the stream open until the test is finished, like the real compositor. */
        for (int i = 0; i < 250 && !g_atomic_int_get(&g_server_done); i++)
            g_usleep(20 * 1000);
        return NULL;
    }

    if (strstr(line, "\"cmd\":\"subscribe\"") != NULL) {
        const char *ack = "{\"ok\":true}\n";
        g_output_stream_write_all(out, ack, strlen(ack), NULL, NULL, NULL);
        for (gsize i = 0; i < G_N_ELEMENTS(kKajoCompEvents); i++)
            g_output_stream_write_all(out, kKajoCompEvents[i], strlen(kKajoCompEvents[i]), NULL, NULL, NULL);
        for (int i = 0; i < 250 && !g_atomic_int_get(&g_server_done); i++)
            g_usleep(20 * 1000);
        return NULL;
    }

    g_async_queue_push(g_requests, g_strdup(line));

    if (strstr(line, "\"cmd\":\"action\"") != NULL) {
        const char *reply = "{\"ok\":true}\n";
        g_output_stream_write_all(out, reply, strlen(reply), NULL, NULL, NULL);
        return NULL;
    }

    /* Mimic Niri's validation: an Action must be a JSON object variant. */
    const char *reply = strstr(line, "\"Action\":{") ? "{\"Ok\":\"Handled\"}\n"
                                                      : "{\"Err\":\"error parsing request\"}\n";
    g_output_stream_write_all(out, reply, strlen(reply), NULL, NULL, NULL);
    return NULL;
}

/* The mock runs its own accept loop on a dedicated thread: the code under test
 * does blocking socket I/O on the thread that would otherwise have to iterate
 * the main loop for a GSocketService to accept connections. */
typedef struct {
    GSocketListener *listener;
    GCancellable *cancel;
    GThread *thread;
    char *dir;
    char *path;
} MockNiri;

static gpointer
mock_accept_loop(gpointer data)
{
    MockNiri *m = data;
    GPtrArray *workers = g_ptr_array_new();
    while (!g_cancellable_is_cancelled(m->cancel)) {
        GSocketConnection *conn =
            g_socket_listener_accept(m->listener, NULL, m->cancel, NULL);
        if (!conn)
            break;
        g_ptr_array_add(workers, g_thread_new("mock-niri-conn", handle_mock_connection, conn));
    }
    g_atomic_int_set(&g_server_done, 1);
    for (guint i = 0; i < workers->len; i++)
        g_thread_join(g_ptr_array_index(workers, i));
    g_ptr_array_free(workers, TRUE);
    return NULL;
}

static MockNiri *
mock_niri_start(void)
{
    MockNiri *m = g_new0(MockNiri, 1);
    m->dir = g_dir_make_tmp("kajo-niri-XXXXXX", NULL);
    g_assert_nonnull(m->dir);
    m->path = g_build_filename(m->dir, "niri.sock", NULL);

    g_requests = g_async_queue_new_full(g_free);
    g_atomic_int_set(&g_server_done, 0);

    m->listener = g_socket_listener_new();
    m->cancel = g_cancellable_new();
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(m->path);
    g_autoptr(GError) err = NULL;
    g_assert_true(g_socket_listener_add_address(m->listener, addr, G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err));
    g_assert_no_error(err);
    g_unsetenv("KAJO_COMP_SOCKET");
    g_setenv("NIRI_SOCKET", m->path, TRUE);
    m->thread = g_thread_new("mock-niri-accept", mock_accept_loop, m);
    return m;
}

static MockNiri *
mock_kajo_comp_start(void)
{
    MockNiri *m = g_new0(MockNiri, 1);
    m->dir = g_dir_make_tmp("kajo-comp-XXXXXX", NULL);
    g_assert_nonnull(m->dir);
    m->path = g_build_filename(m->dir, "kajo-comp.sock", NULL);

    g_requests = g_async_queue_new_full(g_free);
    g_atomic_int_set(&g_server_done, 0);

    m->listener = g_socket_listener_new();
    m->cancel = g_cancellable_new();
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(m->path);
    g_autoptr(GError) err = NULL;
    g_assert_true(g_socket_listener_add_address(m->listener, addr, G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err));
    g_assert_no_error(err);
    g_unsetenv("NIRI_SOCKET");
    g_setenv("KAJO_COMP_SOCKET", m->path, TRUE);
    m->thread = g_thread_new("mock-kc-accept", mock_accept_loop, m);
    return m;
}

static void
mock_niri_stop(MockNiri *m)
{
    g_atomic_int_set(&g_server_done, 1);
    g_cancellable_cancel(m->cancel);
    g_socket_listener_close(m->listener);
    g_thread_join(m->thread);
    g_object_unref(m->listener);
    g_object_unref(m->cancel);
    unlink(m->path);
    rmdir(m->dir);
    g_unsetenv("NIRI_SOCKET");
    g_unsetenv("KAJO_COMP_SOCKET");
    g_async_queue_unref(g_requests);
    g_requests = NULL;
    g_free(m->path);
    g_free(m->dir);
    g_free(m);
}

/* The action object must be { "Action": { "<Name>": { ... } } }. */
static void
assert_action_object(const char *request, const char *action_name)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) err = NULL;
    g_assert_true(json_parser_load_from_data(parser, request, -1, &err));
    JsonNode *root = json_parser_get_root(parser);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
    JsonObject *obj = json_node_get_object(root);
    g_assert_true(json_object_has_member(obj, "Action"));
    JsonNode *action = json_object_get_member(obj, "Action");
    g_assert_true(JSON_NODE_HOLDS_OBJECT(action)); /* a bare string would be rejected by Niri */
    JsonObject *aobj = json_node_get_object(action);
    g_assert_true(json_object_has_member(aobj, action_name));
    g_assert_true(JSON_NODE_HOLDS_OBJECT(json_object_get_member(aobj, action_name)));
}

static void
test_toggle_floating_sends_object_variant(void)
{
    MockNiri *m = mock_niri_start();

    niri_toggle_window_floating();

    g_autofree char *req = g_async_queue_timeout_pop(g_requests, 2 * G_USEC_PER_SEC);
    g_assert_nonnull(req);
    assert_action_object(req, "ToggleWindowFloating");

    mock_niri_stop(m);
}

static void
test_reload_config_sends_load_config_file(void)
{
    MockNiri *m = mock_niri_start();

    niri_reload_config();

    g_autofree char *req = g_async_queue_timeout_pop(g_requests, 2 * G_USEC_PER_SEC);
    g_assert_nonnull(req);
    assert_action_object(req, "LoadConfigFile");
    g_assert_null(strstr(req, "ReloadConfig")); /* not a real Niri action */

    mock_niri_stop(m);
}

static void
test_send_action_logs_niri_rejection(void)
{
    MockNiri *m = mock_niri_start();

    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*niri: action rejected*");
    niri_send_action("{\"Action\":\"ToggleWindowFloating\"}"); /* the old, invalid form */
    g_test_assert_expected_messages();

    g_autofree char *req = g_async_queue_timeout_pop(g_requests, 2 * G_USEC_PER_SEC);
    g_assert_nonnull(req);

    mock_niri_stop(m);
}

typedef struct {
    ShellCompositor *comp;
    GMainLoop *loop;
    guint ticks;
} PollCtx;

static gboolean
poll_state(gpointer data)
{
    PollCtx *p = data;
    p->ticks++;
    const GArray *ws = shell_compositor_get_workspaces(p->comp);
    const GArray *wins = shell_compositor_get_windows(p->comp);
    const CompositorWindow *fw = shell_compositor_get_focused_window(p->comp);
    const CompositorKeyboardLayouts *kl = shell_compositor_get_keyboard_layouts(p->comp);
    gboolean ready = ws->len == 2 && wins->len == 2 && fw && fw->id == 11 &&
                     kl && kl->names && g_strv_length(kl->names) == 2;
    if (ready || p->ticks > 80) { /* 4 s */
        g_main_loop_quit(p->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void
test_event_stream_parsing(void)
{
    MockNiri *m = mock_niri_start();

    ShellCompositor *comp = shell_compositor_new();
    g_assert_nonnull(comp);

    PollCtx p = { comp, g_main_loop_new(NULL, FALSE), 0 };
    g_timeout_add(50, poll_state, &p);
    g_main_loop_run(p.loop);
    g_main_loop_unref(p.loop);

    const GArray *ws = shell_compositor_get_workspaces(comp);
    g_assert_cmpuint(ws->len, ==, 2);
    /* Sorted by idx regardless of the order Niri sent them in. */
    const CompositorWorkspace *w0 = &g_array_index(ws, CompositorWorkspace, 0);
    const CompositorWorkspace *w1 = &g_array_index(ws, CompositorWorkspace, 1);
    g_assert_cmpuint(w0->idx, ==, 1);
    g_assert_cmpstr(w0->name, ==, "main");
    g_assert_cmpstr(w0->output, ==, "eDP-1");
    g_assert_true(w0->is_active);
    g_assert_true(w0->is_focused);
    g_assert_cmpuint(w0->active_window_id, ==, 10);
    g_assert_cmpuint(w1->idx, ==, 2);
    g_assert_null(w1->name);
    g_assert_false(w1->is_active);

    const GArray *wins = shell_compositor_get_windows(comp);
    g_assert_cmpuint(wins->len, ==, 2);

    const CompositorWindow *fw = shell_compositor_get_focused_window(comp);
    g_assert_nonnull(fw);
    g_assert_cmpuint(fw->id, ==, 11);
    g_assert_cmpstr(fw->title, ==, "Docs");
    g_assert_cmpstr(fw->app_id, ==, "firefox");
    g_assert_cmpuint(fw->workspace_id, ==, 2);

    g_autoptr(GArray) ws1_windows = shell_compositor_get_windows_for_workspace(comp, 1);
    g_assert_cmpuint(ws1_windows->len, ==, 1);
    g_assert_cmpstr(g_array_index(ws1_windows, CompositorWindow, 0).app_id, ==, "foot");

    const CompositorKeyboardLayouts *kl = shell_compositor_get_keyboard_layouts(comp);
    g_assert_cmpstr(kl->names[0], ==, "English (US)");
    g_assert_cmpstr(kl->names[1], ==, "Finnish");
    g_assert_cmpuint(kl->current_idx, ==, 1);

    shell_compositor_destroy(comp);
    mock_niri_stop(m);
}

static void
on_bezel_swipe_cb(ShellCompositor *compositor G_GNUC_UNUSED,
                  const char *edge,
                  double x_frac G_GNUC_UNUSED,
                  gpointer user_data)
{
    char **out_edge = user_data;
    g_free(*out_edge);
    *out_edge = g_strdup(edge);
}

typedef struct {
    ShellCompositor *comp;
    GMainLoop *loop;
    char **bezel_edge;
    guint ticks;
} KcPollCtx;

static gboolean
poll_kc_state(gpointer data)
{
    KcPollCtx *p = data;
    p->ticks++;
    const GArray *ws = shell_compositor_get_workspaces(p->comp);
    const GArray *wins = shell_compositor_get_windows(p->comp);
    const CompositorWindow *fw = shell_compositor_get_focused_window(p->comp);
    const CompositorKeyboardLayouts *kl = shell_compositor_get_keyboard_layouts(p->comp);
    gboolean ready = ws->len == 2 && wins->len == 2 && fw && fw->id == 11 &&
                     kl && kl->names && g_strv_length(kl->names) == 2 &&
                     *p->bezel_edge != NULL;
    if (ready || p->ticks > 80) {
        g_main_loop_quit(p->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void
test_kajo_comp_actions_and_events(void)
{
    MockNiri *m = mock_kajo_comp_start();
    g_assert_true(shell_compositor_is_kajo_comp());

    /* Test compositor-neutral actions routing to kajo-comp IPC */
    shell_compositor_toggle_floating();
    g_autofree char *req1 = g_async_queue_timeout_pop(g_requests, 2 * G_USEC_PER_SEC);
    g_assert_nonnull(req1);
    g_assert_nonnull(strstr(req1, "\"toggle-window-floating\""));

    shell_compositor_reload_config();
    g_autofree char *req2 = g_async_queue_timeout_pop(g_requests, 2 * G_USEC_PER_SEC);
    g_assert_nonnull(req2);
    g_assert_nonnull(strstr(req2, "\"reload-config\""));

    shell_compositor_set_output_transform("eDP-1", "90");
    g_autofree char *req3 = g_async_queue_timeout_pop(g_requests, 2 * G_USEC_PER_SEC);
    g_assert_nonnull(req3);
    g_assert_nonnull(strstr(req3, "\"set-output-transform\""));
    g_assert_nonnull(strstr(req3, "\"90\""));

    /* Test Niri JSON translation via niri_send_action() when running on kajo-comp */
    niri_send_action("{\"Action\":{\"FocusWorkspaceDown\":{}}}");
    g_autofree char *req4 = g_async_queue_timeout_pop(g_requests, 2 * G_USEC_PER_SEC);
    g_assert_nonnull(req4);
    g_assert_nonnull(strstr(req4, "\"focus-workspace-down\""));

    /* Test subscribe + flat event stream + TouchBezelSwipe */
    g_autofree char *bezel_edge = NULL;
    ShellCompositor *comp = shell_compositor_new();
    g_assert_nonnull(comp);
    shell_compositor_on_touch_bezel_swipe(comp, on_bezel_swipe_cb, &bezel_edge);

    KcPollCtx p = { comp, g_main_loop_new(NULL, FALSE), &bezel_edge, 0 };
    g_timeout_add(50, poll_kc_state, &p);
    g_main_loop_run(p.loop);
    g_main_loop_unref(p.loop);

    const GArray *ws = shell_compositor_get_workspaces(comp);
    g_assert_cmpuint(ws->len, ==, 2);
    const CompositorWindow *fw = shell_compositor_get_focused_window(comp);
    g_assert_nonnull(fw);
    g_assert_cmpuint(fw->id, ==, 11);
    const CompositorKeyboardLayouts *kl = shell_compositor_get_keyboard_layouts(comp);
    g_assert_cmpuint(kl->current_idx, ==, 1);
    g_assert_cmpstr(bezel_edge, ==, "bottom");

    shell_compositor_destroy(comp);
    mock_niri_stop(m);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/niri/action/toggle-floating-object-variant",
                    test_toggle_floating_sends_object_variant);
    g_test_add_func("/niri/action/reload-config-load-config-file",
                    test_reload_config_sends_load_config_file);
    g_test_add_func("/niri/action/rejection-is-logged", test_send_action_logs_niri_rejection);
    g_test_add_func("/niri/events/stream-parsing", test_event_stream_parsing);
    g_test_add_func("/kajo-comp/ipc/actions-and-events", test_kajo_comp_actions_and_events);
    return g_test_run();
}
