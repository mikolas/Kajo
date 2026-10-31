#include "../src/greeter/greetd_ipc.h"
#include "../src/greeter/greeter_users.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    gchar          *sock_path;
    GSocketService *service;
    gboolean        cancelled_called;
    gboolean        started_called;
    gchar          *last_user;
} MockGreetdServer;

static gboolean
write_greetd_frame(GOutputStream *out, const gchar *json_str)
{
    uint32_t len = (uint32_t)strlen(json_str);
    gsize written = 0;
    if (!g_output_stream_write_all(out, &len, sizeof(len), &written, NULL, NULL))
        return FALSE;
    if (!g_output_stream_write_all(out, json_str, len, &written, NULL, NULL))
        return FALSE;
    return g_output_stream_flush(out, NULL, NULL);
}

static gchar *
read_greetd_frame(GInputStream *in)
{
    uint32_t len = 0;
    gsize nread = 0;
    if (!g_input_stream_read_all(in, &len, sizeof(len), &nread, NULL, NULL))
        return NULL;
    if (nread != sizeof(len) || len == 0 || len > 65536)
        return NULL;

    gchar *buf = g_malloc(len + 1);
    if (!g_input_stream_read_all(in, buf, len, &nread, NULL, NULL)) {
        g_free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

static gboolean
on_mock_incoming(GSocketService    *service G_GNUC_UNUSED,
                 GSocketConnection *connection,
                 GObject           *source_object G_GNUC_UNUSED,
                 gpointer           user_data)
{
    MockGreetdServer *srv = user_data;
    GInputStream  *in  = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

    while (TRUE) {
        g_autofree gchar *req_str = read_greetd_frame(in);
        if (!req_str)
            break;

        g_autoptr(JsonParser) parser = json_parser_new();
        g_assert_true(json_parser_load_from_data(parser, req_str, -1, NULL));
        JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
        const gchar *type = json_object_get_string_member(obj, "type");

        if (g_strcmp0(type, "create_session") == 0) {
            g_free(srv->last_user);
            srv->last_user = g_strdup(json_object_get_string_member(obj, "username"));
            write_greetd_frame(
                out,
                "{\"type\":\"auth_message\",\"auth_message_type\":\"secret\",\"auth_message\":\"Password:\"}");
        } else if (g_strcmp0(type, "post_auth_message_response") == 0) {
            const gchar *pw = json_object_get_string_member_with_default(obj, "response", "");
            if (g_strcmp0(pw, "correct-\"horse\"\\battery") == 0) {
                write_greetd_frame(out, "{\"type\":\"success\"}");
            } else {
                write_greetd_frame(
                    out,
                    "{\"type\":\"error\",\"error_type\":\"auth_error\",\"description\":\"PAM authentication failed\"}");
            }
        } else if (g_strcmp0(type, "start_session") == 0) {
            srv->started_called = TRUE;
            write_greetd_frame(out, "{\"type\":\"success\"}");
            break;
        } else if (g_strcmp0(type, "cancel_session") == 0) {
            srv->cancelled_called = TRUE;
            write_greetd_frame(out, "{\"type\":\"success\"}");
            break;
        }
    }

    return TRUE;
}

static MockGreetdServer *
mock_server_start(void)
{
    MockGreetdServer *srv = g_new0(MockGreetdServer, 1);
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-greetd-test-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    srv->sock_path = g_build_filename(tmpdir, "greetd.sock", NULL);

    srv->service = g_threaded_socket_service_new(4);
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(srv->sock_path);
    GError *err = NULL;
    gboolean ok = g_socket_listener_add_address(
        G_SOCKET_LISTENER(srv->service), addr,
        G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
        NULL, NULL, &err);
    g_assert_no_error(err);
    g_assert_true(ok);

    g_signal_connect(srv->service, "run", G_CALLBACK(on_mock_incoming), srv);
    g_socket_service_start(srv->service);
    return srv;
}

static void
mock_server_stop(MockGreetdServer *srv)
{
    if (!srv)
        return;
    g_socket_service_stop(srv->service);
    g_socket_listener_close(G_SOCKET_LISTENER(srv->service));
    g_object_unref(srv->service);
    g_unlink(srv->sock_path);
    g_autofree gchar *dir = g_path_get_dirname(srv->sock_path);
    g_rmdir(dir);
    g_free(srv->sock_path);
    g_free(srv->last_user);
    g_free(srv);
}

typedef struct {
    GMainLoop *loop;
    gboolean   ok;
    GError    *error;
} AsyncWaitResult;

static void
on_auth_done_cb(GObject      *source G_GNUC_UNUSED,
                GAsyncResult *res,
                gpointer      user_data)
{
    AsyncWaitResult *wait = user_data;
    wait->ok = greetd_authenticate_and_start_finish(res, &wait->error);
    g_main_loop_quit(wait->loop);
}

static void
test_greetd_auth_success_and_failure(void)
{
    MockGreetdServer *srv = mock_server_start();
    const gchar *cmd[] = { "niri-session", NULL };
    const gchar *env[] = { "XDG_SESSION_TYPE=wayland", NULL };

    /* 1. Test wrong password -> should fail with auth error and call cancel_session */
    {
        AsyncWaitResult wait = { .loop = g_main_loop_new(NULL, FALSE) };
        greetd_authenticate_and_start_async(
            srv->sock_path, "mikolas", "wrong-password", cmd, env,
            NULL, on_auth_done_cb, &wait);
        g_main_loop_run(wait.loop);

        g_assert_false(wait.ok);
        g_assert_nonnull(wait.error);
        g_assert_true(srv->cancelled_called);
        g_assert_false(srv->started_called);

        g_clear_error(&wait.error);
        g_main_loop_unref(wait.loop);
    }

    /* 2. Test valid password with quotes/backslashes -> should escape cleanly and call start_session */
    {
        srv->cancelled_called = FALSE;
        AsyncWaitResult wait = { .loop = g_main_loop_new(NULL, FALSE) };
        greetd_authenticate_and_start_async(
            srv->sock_path, "mikolas", "correct-\"horse\"\\battery", cmd, env,
            NULL, on_auth_done_cb, &wait);
        g_main_loop_run(wait.loop);

        g_assert_no_error(wait.error);
        g_assert_true(wait.ok);
        g_assert_true(srv->started_called);
        g_assert_cmpstr(srv->last_user, ==, "mikolas");

        g_main_loop_unref(wait.loop);
    }

    mock_server_stop(srv);
}

static void
test_user_and_session_discovery(void)
{
    /* Real POSIX user discovery */
    g_autoptr(GPtrArray) users = greeter_discover_users();
    g_assert_nonnull(users);
    g_assert_cmpuint(users->len, >=, 1);
    GreeterUser *u0 = g_ptr_array_index(users, 0);
    g_assert_nonnull(u0->username);
    g_assert_cmpstr(u0->username, !=, "");

    /* Custom .desktop session parsing */
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-sessions-XXXXXX", NULL);
    g_autofree gchar *niri_dt = g_build_filename(tmpdir, "niri.desktop", NULL);
    g_autofree gchar *hidden_dt = g_build_filename(tmpdir, "hidden.desktop", NULL);

    g_assert_true(g_file_set_contents(
        niri_dt,
        "[Desktop Entry]\nName=Niri\nComment=Scrollable tiling\nExec=niri-session %U\nType=Application\nDesktopNames=niri\n",
        -1, NULL));
    g_assert_true(g_file_set_contents(
        hidden_dt,
        "[Desktop Entry]\nName=HiddenDE\nExec=hidden-session\nHidden=true\n",
        -1, NULL));

    const gchar *dirs[] = { tmpdir, NULL };
    g_autoptr(GPtrArray) sessions = greeter_discover_sessions_from_dirs(dirs, NULL);
    g_assert_nonnull(sessions);
    g_assert_cmpuint(sessions->len, ==, 1);

    GreeterSession *s0 = g_ptr_array_index(sessions, 0);
    g_assert_cmpstr(s0->id, ==, "niri");
    g_assert_cmpstr(s0->name, ==, "Niri");
    g_assert_cmpstr(s0->exec, ==, "niri-session %U");
    g_assert_nonnull(s0->cmd_argv);
    g_assert_cmpstr(s0->cmd_argv[0], ==, "niri-session");
    g_assert_null(s0->cmd_argv[1]);

    /* State persistence roundtrip */
    g_autofree gchar *state_file = g_build_filename(tmpdir, "state.json", NULL);
    g_assert_true(greeter_state_save(state_file, "mikolas", "niri"));

    g_autofree gchar *loaded_user = NULL;
    g_autofree gchar *loaded_sess = NULL;
    greeter_state_load(state_file, &loaded_user, &loaded_sess);
    g_assert_cmpstr(loaded_user, ==, "mikolas");
    g_assert_cmpstr(loaded_sess, ==, "niri");

    g_unlink(niri_dt);
    g_unlink(hidden_dt);
    g_unlink(state_file);
    g_rmdir(tmpdir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/greeter/ipc-auth-success-and-failure", test_greetd_auth_success_and_failure);
    g_test_add_func("/greeter/user-and-session-discovery", test_user_and_session_discovery);
    return g_test_run();
}
