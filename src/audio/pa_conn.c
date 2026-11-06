#include "pa_conn.h"

static void shell_pa_conn_connect(ShellPaConn *conn);

static void
pa_conn_cancel_reconnect(ShellPaConn *conn)
{
    if (conn->reconnect_id != 0) {
        g_source_remove(conn->reconnect_id);
        conn->reconnect_id = 0;
    }
}

static gboolean
pa_conn_reconnect_cb(gpointer user_data)
{
    ShellPaConn *conn = user_data;
    conn->reconnect_id = 0;
    shell_pa_conn_connect(conn);
    return G_SOURCE_REMOVE;
}

static void
pa_conn_schedule_reconnect(ShellPaConn *conn)
{
    if (!conn->ml || conn->reconnect_id != 0)
        return;
    conn->reconnect_id = g_timeout_add(SHELL_PA_RECONNECT_MS, pa_conn_reconnect_cb, conn);
}

static void
pa_conn_drop_context(ShellPaConn *conn)
{
    if (conn->drop_cb)
        conn->drop_cb(conn->user_data);

    if (conn->ctx) {
        pa_context_set_state_callback(conn->ctx, NULL, NULL);
        pa_context_set_subscribe_callback(conn->ctx, NULL, NULL);
        pa_context_disconnect(conn->ctx);
        pa_context_unref(conn->ctx);
        conn->ctx = NULL;
    }
}

static void
pa_conn_state_cb(pa_context *ctx, void *user_data)
{
    ShellPaConn *conn = user_data;
    pa_context_state_t state = pa_context_get_state(ctx);

    if (state == PA_CONTEXT_READY)
        pa_conn_cancel_reconnect(conn);
    else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
        pa_conn_schedule_reconnect(conn);

    if (conn->state_cb)
        conn->state_cb(ctx, state, conn->user_data);
}

static void
shell_pa_conn_connect(ShellPaConn *conn)
{
    if (!conn->ml)
        return;

    pa_conn_cancel_reconnect(conn);
    pa_conn_drop_context(conn);

    pa_mainloop_api *api = pa_glib_mainloop_get_api(conn->ml);
    conn->ctx = pa_context_new(api, conn->client_name);
    if (!conn->ctx) {
        pa_conn_schedule_reconnect(conn);
        return;
    }

    pa_context_set_state_callback(conn->ctx, pa_conn_state_cb, conn);
    if (pa_context_connect(conn->ctx, NULL, PA_CONTEXT_NOFAIL, NULL) < 0)
        pa_conn_schedule_reconnect(conn);
}

gboolean
shell_pa_conn_open(ShellPaConn *conn,
                   const gchar *client_name,
                   ShellPaStateFunc state_cb,
                   ShellPaDropFunc drop_cb,
                   gpointer user_data)
{
    g_return_val_if_fail(conn != NULL, FALSE);

    conn->client_name = g_strdup(client_name ? client_name : "kajo");
    conn->state_cb = state_cb;
    conn->drop_cb = drop_cb;
    conn->user_data = user_data;

    conn->ml = pa_glib_mainloop_new(NULL);
    if (!conn->ml)
        return FALSE;

    shell_pa_conn_connect(conn);
    return TRUE;
}

void
shell_pa_conn_close(ShellPaConn *conn)
{
    if (!conn)
        return;

    pa_conn_cancel_reconnect(conn);
    pa_conn_drop_context(conn);

    if (conn->ml) {
        pa_glib_mainloop_free(conn->ml);
        conn->ml = NULL;
    }
    g_clear_pointer(&conn->client_name, g_free);
    conn->state_cb = NULL;
    conn->drop_cb = NULL;
}
