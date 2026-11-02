#define _GNU_SOURCE
#include "greetd_ipc.h"
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <string.h>

void
greetd_response_free(GreetdResponse *resp)
{
    if (!resp)
        return;
    g_free(resp->auth_message);
    g_free(resp->error_description);
    g_free(resp);
}

static GSocketConnection *
connect_greetd_socket(const gchar  *socket_path,
                      GCancellable *cancellable,
                      GError      **error)
{
    const gchar *path = socket_path;
    if (!path || path[0] == '\0')
        path = g_getenv("GREETD_SOCK");

    if (!path || path[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "GREETD_SOCK environment variable is not set");
        return NULL;
    }

    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(path);
    g_autoptr(GSocketClient) client = g_socket_client_new();

    return g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), cancellable, error);
}

static GreetdResponse *
parse_greetd_response_json(const gchar *json_str, gssize len, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, len, error))
        return NULL;

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "greetd response is not a JSON object");
        return NULL;
    }

    JsonObject *obj = json_node_get_object(root);
    const gchar *type_str = json_object_get_string_member_with_default(obj, "type", "");

    GreetdResponse *resp = g_new0(GreetdResponse, 1);

    if (g_strcmp0(type_str, "success") == 0) {
        resp->type = GREETD_RESPONSE_SUCCESS;
    } else if (g_strcmp0(type_str, "error") == 0) {
        resp->type = GREETD_RESPONSE_ERROR;
        const gchar *err_type = json_object_get_string_member_with_default(obj, "error_type", "error");
        resp->error_type = (g_strcmp0(err_type, "auth_error") == 0)
                               ? GREETD_ERROR_AUTH
                               : GREETD_ERROR_OTHER;
        resp->error_description = g_strdup(
            json_object_get_string_member_with_default(obj, "description", "Authentication failed"));
    } else if (g_strcmp0(type_str, "auth_message") == 0) {
        resp->type = GREETD_RESPONSE_AUTH_MESSAGE;
        const gchar *msg_type = json_object_get_string_member_with_default(obj, "auth_message_type", "secret");
        if (g_strcmp0(msg_type, "visible") == 0)
            resp->auth_message_type = GREETD_AUTH_MESSAGE_VISIBLE;
        else if (g_strcmp0(msg_type, "info") == 0)
            resp->auth_message_type = GREETD_AUTH_MESSAGE_INFO;
        else if (g_strcmp0(msg_type, "error") == 0)
            resp->auth_message_type = GREETD_AUTH_MESSAGE_ERROR;
        else
            resp->auth_message_type = GREETD_AUTH_MESSAGE_SECRET;

        resp->auth_message = g_strdup(
            json_object_get_string_member_with_default(obj, "auth_message", "Password:"));
    } else {
        greetd_response_free(resp);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Unknown greetd response type: %s", type_str);
        return NULL;
    }

    return resp;
}

static GreetdResponse *
send_and_receive_on_connection(GSocketConnection *conn,
                               const gchar       *request_json,
                               GCancellable      *cancellable,
                               GError           **error)
{
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GInputStream  *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));

    uint32_t req_len = (uint32_t)strlen(request_json);
    gsize bytes_written = 0;

    if (!g_output_stream_write_all(out, &req_len, sizeof(req_len), &bytes_written, cancellable, error))
        return NULL;
    if (!g_output_stream_write_all(out, request_json, req_len, &bytes_written, cancellable, error))
        return NULL;
    if (!g_output_stream_flush(out, cancellable, error))
        return NULL;

    uint32_t resp_len = 0;
    gsize bytes_read = 0;
    if (!g_input_stream_read_all(in, &resp_len, sizeof(resp_len), &bytes_read, cancellable, error))
        return NULL;

    if (bytes_read != sizeof(resp_len) || resp_len == 0 || resp_len > 65536) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Invalid frame length from greetd socket");
        return NULL;
    }

    g_autofree gchar *buf = g_malloc(resp_len + 1);
    if (!g_input_stream_read_all(in, buf, resp_len, &bytes_read, cancellable, error))
        return NULL;
    buf[resp_len] = '\0';

    return parse_greetd_response_json(buf, (gssize)resp_len, error);
}

static gchar *
build_create_session_json(const gchar *username)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "create_session");
    json_builder_set_member_name(b, "username");
    json_builder_add_string_value(b, username ? username : "");
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    return json_generator_to_data(gen, NULL);
}

static gchar *
build_post_auth_json(const gchar *response_str, gsize *out_alloc_len)
{
    if (response_str == NULL) {
        const gchar *literal = "{\"type\":\"post_auth_message_response\"}";
        if (out_alloc_len)
            *out_alloc_len = strlen(literal);
        return g_strdup(literal);
    }

    const gchar *prefix = "{\"type\":\"post_auth_message_response\",\"response\":\"";
    const gchar *suffix = "\"}";
    gsize prefix_len = strlen(prefix);
    gsize suffix_len = strlen(suffix);
    gsize raw_len    = strlen(response_str);

    /* Worst-case JSON escape expands each byte to 6 bytes (\u00XX) */
    gsize cap = prefix_len + (raw_len * 6) + suffix_len + 1;
    gchar *buf = g_malloc0(cap);
    memcpy(buf, prefix, prefix_len);
    gsize pos = prefix_len;

    for (gsize i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)response_str[i];
        if (c == '"') {
            buf[pos++] = '\\';
            buf[pos++] = '"';
        } else if (c == '\\') {
            buf[pos++] = '\\';
            buf[pos++] = '\\';
        } else if (c == '\b') {
            buf[pos++] = '\\';
            buf[pos++] = 'b';
        } else if (c == '\f') {
            buf[pos++] = '\\';
            buf[pos++] = 'f';
        } else if (c == '\n') {
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == '\r') {
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (c == '\t') {
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            buf[pos++] = '\\';
            buf[pos++] = 'u';
            buf[pos++] = '0';
            buf[pos++] = '0';
            buf[pos++] = hex[(c >> 4) & 0x0f];
            buf[pos++] = hex[c & 0x0f];
        } else {
            buf[pos++] = (gchar)c;
        }
    }

    memcpy(buf + pos, suffix, suffix_len);
    pos += suffix_len;
    buf[pos] = '\0';

    if (out_alloc_len)
        *out_alloc_len = cap;
    return buf;
}

static gchar *
build_start_session_json(const gchar *const *cmd, const gchar *const *env)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "start_session");

    json_builder_set_member_name(b, "cmd");
    json_builder_begin_array(b);
    if (cmd) {
        for (gsize i = 0; cmd[i] != NULL; i++)
            json_builder_add_string_value(b, cmd[i]);
    }
    json_builder_end_array(b);

    json_builder_set_member_name(b, "env");
    json_builder_begin_array(b);
    if (env) {
        for (gsize i = 0; env[i] != NULL; i++)
            json_builder_add_string_value(b, env[i]);
    }
    json_builder_end_array(b);

    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    return json_generator_to_data(gen, NULL);
}

typedef struct {
    gchar  *socket_path;
    gchar  *username;
    gchar  *password;
    gchar **cmd;
    gchar **env;
} GreetdAuthJob;

static void
greetd_auth_job_free(gpointer data)
{
    GreetdAuthJob *job = data;
    if (!job)
        return;

    if (job->password) {
        explicit_bzero(job->password, strlen(job->password));
        g_free(job->password);
    }
    g_free(job->socket_path);
    g_free(job->username);
    g_strfreev(job->cmd);
    g_strfreev(job->env);
    g_free(job);
}

static void
cancel_greetd_session_quiet(GSocketConnection *conn)
{
    if (!conn)
        return;
    g_autoptr(GreetdResponse) resp = send_and_receive_on_connection(
        conn, "{\"type\":\"cancel_session\"}", NULL, NULL);
}

static void
greetd_auth_worker_thread(GTask        *task,
                          gpointer      source_object G_GNUC_UNUSED,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
    GreetdAuthJob *job = task_data;
    GError *error = NULL;

    g_autoptr(GSocketConnection) conn = connect_greetd_socket(job->socket_path, cancellable, &error);
    if (!conn) {
        g_task_return_error(task, error);
        return;
    }

    /* Step 1: create_session */
    g_autofree gchar *create_json = build_create_session_json(job->username);
    g_autoptr(GreetdResponse) resp = send_and_receive_on_connection(conn, create_json, cancellable, &error);
    if (!resp) {
        g_task_return_error(task, error);
        return;
    }

    /* Step 2: Handle zero or more auth_message prompts */
    gboolean supplied_password = FALSE;
    for (int round = 0; round < 8 && resp->type == GREETD_RESPONSE_AUTH_MESSAGE; round++) {
        const gchar *reply_str = NULL;
        if (resp->auth_message_type == GREETD_AUTH_MESSAGE_SECRET ||
            resp->auth_message_type == GREETD_AUTH_MESSAGE_VISIBLE) {
            if (supplied_password) {
                cancel_greetd_session_quiet(conn);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                        "Additional authentication prompt required: %s",
                                        resp->auth_message ? resp->auth_message : "");
                return;
            }
            reply_str = job->password ? job->password : "";
            supplied_password = TRUE;
        }

        gsize send_cap = 0;
        gchar *send_json = build_post_auth_json(reply_str, &send_cap);
        /* Scrub job->password as soon as it has been serialized once */
        if (supplied_password && job->password) {
            explicit_bzero(job->password, strlen(job->password));
        }

        g_clear_pointer(&resp, greetd_response_free);
        resp = send_and_receive_on_connection(conn, send_json, cancellable, &error);
        if (send_json) {
            explicit_bzero(send_json, send_cap);
            g_free(send_json);
        }

        if (!resp) {
            cancel_greetd_session_quiet(conn);
            g_task_return_error(task, error);
            return;
        }
    }

    if (resp->type == GREETD_RESPONSE_ERROR) {
        gchar *desc = g_strdup(resp->error_description ? resp->error_description : "Authentication failed");
        cancel_greetd_session_quiet(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s", desc);
        g_free(desc);
        return;
    }

    if (resp->type != GREETD_RESPONSE_SUCCESS) {
        cancel_greetd_session_quiet(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Unexpected greetd state during authentication");
        return;
    }

    /* Step 3: start_session */
    g_autofree gchar *start_json = build_start_session_json(
        (const gchar *const *)job->cmd, (const gchar *const *)job->env);
    g_clear_pointer(&resp, greetd_response_free);
    resp = send_and_receive_on_connection(conn, start_json, cancellable, &error);
    if (!resp) {
        cancel_greetd_session_quiet(conn);
        g_task_return_error(task, error);
        return;
    }

    if (resp->type == GREETD_RESPONSE_ERROR) {
        gchar *desc = g_strdup(resp->error_description ? resp->error_description : "Failed to start session");
        cancel_greetd_session_quiet(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", desc);
        g_free(desc);
        return;
    }

    g_task_return_boolean(task, TRUE);
}

void
greetd_authenticate_and_start_async(const gchar        *socket_path,
                                    const gchar        *username,
                                    const gchar        *password,
                                    const gchar *const *cmd,
                                    const gchar *const *env,
                                    GCancellable       *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer            user_data)
{
    GreetdAuthJob *job = g_new0(GreetdAuthJob, 1);
    job->socket_path = g_strdup(socket_path);
    job->username    = g_strdup(username ? username : "");
    job->password    = g_strdup(password ? password : "");
    job->cmd         = g_strdupv((gchar **)cmd);
    job->env         = g_strdupv((gchar **)env);

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, job, greetd_auth_job_free);
    g_task_run_in_thread(task, greetd_auth_worker_thread);
    g_object_unref(task);
}

gboolean
greetd_authenticate_and_start_finish(GAsyncResult *result,
                                     GError      **error)
{
    g_return_val_if_fail(G_IS_TASK(result), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}
