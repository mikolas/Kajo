#define _GNU_SOURCE
#include "web_sync.h"
#include "web_sync_merge.h"
#include "../config/kajo_config_store.h"
#include <libsoup/soup.h>
#include <libsecret/secret.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <string.h>

static const SecretSchema k_sync_token_schema = {
    .name = "org.kajo.Web.DriveSync",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
        { "profile_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { NULL, 0 }
    }
};

struct _KajoWebSync {
    gchar                *profile_id;
    KajoWebStorage       *storage;
    SoupSession          *soup;
    GSocketService       *oauth_srv;
    guint16               oauth_port;
    gchar                *pkce_verifier;
    gchar                *oauth_state;
    gchar                *client_id;
    gchar                *client_secret;
    gchar                *access_token;
    gchar                *refresh_token;
    gchar                *status_text;
    gchar                *email;
    gchar                *e2ee_passphrase;
    gboolean              connected;
    gboolean              syncing;
    guint                 debounce_id;
    GCancellable         *cancellable;
    KajoWebSyncStatusCb   on_status;
    KajoWebSyncOpenUrlCb  on_open_url;
    gpointer              user_data;
};

static void
emit_status(KajoWebSync *sync, const gchar *text)
{
    g_free(sync->status_text);
    sync->status_text = g_strdup(text ? text : "");
    if (sync->on_status)
        sync->on_status(sync->status_text, sync->email ? sync->email : "", sync->connected, sync->user_data);
}

static void
scrub_and_free(gchar **s)
{
    if (!s || !*s)
        return;
    explicit_bzero(*s, strlen(*s));
    g_free(*s);
    *s = NULL;
}

static void
store_refresh_token_async(KajoWebSync *sync, const gchar *token)
{
    if (!token || !*token)
        return;
    g_autofree gchar *label = g_strdup_printf("Kajo Web Drive Sync (%s)", sync->profile_id);
    secret_password_store(&k_sync_token_schema, SECRET_COLLECTION_DEFAULT,
                          label, token, sync->cancellable, NULL, NULL,
                          "profile_id", sync->profile_id, NULL);
}

static void
on_lookup_token_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    g_autoptr(GError) err = NULL;
    gchar *tok = secret_password_lookup_finish(res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (tok)
            secret_password_free(tok);
        return;
    }
    KajoWebSync *sync = user_data;
    if (tok && *tok) {
        scrub_and_free(&sync->refresh_token);
        sync->refresh_token = g_strdup(tok);
        secret_password_free(tok);
        sync->connected = TRUE;
        emit_status(sync, "CLOUD READY");
    } else {
        if (tok)
            secret_password_free(tok);
        sync->connected = FALSE;
        emit_status(sync, "LOCAL ONLY");
    }
}

KajoWebSync *
web_sync_new(const gchar         *profile_id,
             KajoWebStorage      *storage,
             KajoWebSyncStatusCb  on_status,
             KajoWebSyncOpenUrlCb on_open_url,
             gpointer             user_data)
{
    KajoWebSync *sync = g_new0(KajoWebSync, 1);
    sync->profile_id = g_strdup(profile_id && *profile_id ? profile_id : "default");
    sync->storage = storage;
    sync->soup = soup_session_new();
    sync->cancellable = g_cancellable_new();
    sync->status_text = g_strdup("LOCAL ONLY");
    sync->email = g_strdup("");
    sync->on_status = on_status;
    sync->on_open_url = on_open_url;
    sync->user_data = user_data;

    secret_password_lookup(&k_sync_token_schema, sync->cancellable,
                           on_lookup_token_done, sync,
                           "profile_id", sync->profile_id, NULL);
    return sync;
}

void
web_sync_free(KajoWebSync *sync)
{
    if (!sync)
        return;
    if (sync->debounce_id) {
        g_source_remove(sync->debounce_id);
        sync->debounce_id = 0;
    }
    if (sync->cancellable)
        g_cancellable_cancel(sync->cancellable);
    g_clear_object(&sync->cancellable);
    if (sync->oauth_srv) {
        g_socket_service_stop(sync->oauth_srv);
        g_clear_object(&sync->oauth_srv);
    }
    g_clear_object(&sync->soup);
    scrub_and_free(&sync->pkce_verifier);
    scrub_and_free(&sync->oauth_state);
    scrub_and_free(&sync->client_secret);
    scrub_and_free(&sync->access_token);
    scrub_and_free(&sync->refresh_token);
    scrub_and_free(&sync->e2ee_passphrase);
    g_free(sync->client_id);
    g_free(sync->profile_id);
    g_free(sync->status_text);
    g_free(sync->email);
    g_free(sync);
}

/* ─── Google Drive API v3 (appDataFolder) Bookmarks Sync ─── */

static void
on_drive_upload_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    KajoWebSync *sync = user_data;
    sync->syncing = FALSE;
    if (err) {
        emit_status(sync, "SYNC UPLOAD FAILED");
        return;
    }
    emit_status(sync, "☁ SYNCED JUST NOW");
}

static void
upload_bookmarks_snapshot(KajoWebSync *sync, const gchar *existing_file_id)
{
    if (!sync->storage || !sync->access_token) {
        sync->syncing = FALSE;
        return;
    }
    g_autoptr(GPtrArray) all_bm = web_storage_list_bookmarks(sync->storage, TRUE);
    g_autofree gchar *plain = web_sync_bookmarks_to_json(all_bm);
    g_autofree gchar *payload = NULL;
    if (sync->e2ee_passphrase && *sync->e2ee_passphrase) {
        payload = web_sync_encrypt_payload(plain, sync->e2ee_passphrase, NULL);
    }
    if (!payload)
        payload = g_strdup(plain);

    g_autofree gchar *auth_hdr = g_strdup_printf("Bearer %s", sync->access_token);
    g_autoptr(SoupMessage) msg = NULL;

    if (existing_file_id && *existing_file_id) {
        g_autofree gchar *url = g_strdup_printf(
            "https://www.googleapis.com/upload/drive/v3/files/%s?uploadType=media",
            existing_file_id);
        msg = soup_message_new("PATCH", url);
        g_autoptr(GBytes) b = g_bytes_new(payload, strlen(payload));
        soup_message_set_request_body_from_bytes(msg, "application/json", b);
    } else {
        const char *boundary = "kajo_web_sync_boundary_7f3a";
        g_autofree gchar *multipart = g_strdup_printf(
            "--%s\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"
            "{\"name\":\"kajo-web-bookmarks.json\",\"parents\":[\"appDataFolder\"]}\r\n"
            "--%s\r\nContent-Type: application/json\r\n\r\n"
            "%s\r\n--%s--\r\n",
            boundary, boundary, payload, boundary);
        g_autofree gchar *ct = g_strdup_printf("multipart/related; boundary=%s", boundary);
        msg = soup_message_new("POST", "https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart");
        g_autoptr(GBytes) b = g_bytes_new(multipart, strlen(multipart));
        soup_message_set_request_body_from_bytes(msg, ct, b);
    }

    soup_message_headers_replace(soup_message_get_request_headers(msg), "Authorization", auth_hdr);
    soup_session_send_and_read_async(sync->soup, msg, G_PRIORITY_DEFAULT,
                                     sync->cancellable, on_drive_upload_done, sync);
}

typedef struct {
    KajoWebSync *sync;
    gchar       *file_id;
} DriveDownloadCtx;

static void
on_drive_download_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DriveDownloadCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_free(ctx->file_id);
        g_free(ctx);
        return;
    }

    KajoWebSync *sync = ctx->sync;
    g_autofree gchar *file_id = ctx->file_id;
    g_free(ctx);

    if (!err && body && sync->storage) {
        gsize len = 0;
        const char *raw = g_bytes_get_data(body, &len);
        g_autofree gchar *text = g_strndup(raw, len);
        g_autofree gchar *dec = NULL;
        if (strstr(text, "\"aes-256-gcm-v1\"") && sync->e2ee_passphrase && *sync->e2ee_passphrase)
            dec = web_sync_decrypt_payload(text, sync->e2ee_passphrase, NULL);
        const gchar *json_src = dec ? dec : text;
        g_autoptr(GPtrArray) remote = web_sync_bookmarks_from_json(json_src, NULL);
        if (remote) {
            g_autoptr(GPtrArray) local = web_storage_list_bookmarks(sync->storage, TRUE);
            g_autoptr(GPtrArray) merged = web_sync_merge_bookmarks(local, remote);
            web_storage_replace_bookmarks(sync->storage, merged);
        }
    }
    upload_bookmarks_snapshot(sync, file_id);
}

static void
on_drive_list_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    KajoWebSync *sync = user_data;
    if (err || !body) {
        sync->syncing = FALSE;
        emit_status(sync, "SYNC NETWORK ERROR");
        return;
    }

    gsize len = 0;
    const char *raw = g_bytes_get_data(body, &len);
    g_autoptr(JsonParser) p = json_parser_new();
    g_autofree gchar *file_id = NULL;
    if (json_parser_load_from_data(p, raw, (gssize)len, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "files")) {
                JsonArray *arr = json_object_get_array_member(obj, "files");
                if (arr && json_array_get_length(arr) > 0) {
                    JsonObject *f0 = json_array_get_object_element(arr, 0);
                    const gchar *fid = f0 ? json_object_get_string_member_with_default(f0, "id", "") : "";
                    if (*fid)
                        file_id = g_strdup(fid);
                }
            }
        }
    }

    if (file_id) {
        g_autofree gchar *url = g_strdup_printf(
            "https://www.googleapis.com/drive/v3/files/%s?alt=media", file_id);
        g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
        g_autofree gchar *auth_hdr = g_strdup_printf("Bearer %s", sync->access_token);
        soup_message_headers_replace(soup_message_get_request_headers(msg), "Authorization", auth_hdr);
        DriveDownloadCtx *ctx = g_new0(DriveDownloadCtx, 1);
        ctx->sync = sync;
        ctx->file_id = g_steal_pointer(&file_id);
        soup_session_send_and_read_async(sync->soup, msg, G_PRIORITY_DEFAULT,
                                         sync->cancellable, on_drive_download_done, ctx);
    } else {
        upload_bookmarks_snapshot(sync, NULL);
    }
}

static void
start_drive_list(KajoWebSync *sync)
{
    g_autoptr(SoupMessage) msg = soup_message_new(
        "GET",
        "https://www.googleapis.com/drive/v3/files?spaces=appDataFolder"
        "&q=name%3D%27kajo-web-bookmarks.json%27&fields=files(id,modifiedTime)");
    g_autofree gchar *auth_hdr = g_strdup_printf("Bearer %s", sync->access_token);
    soup_message_headers_replace(soup_message_get_request_headers(msg), "Authorization", auth_hdr);
    soup_session_send_and_read_async(sync->soup, msg, G_PRIORITY_DEFAULT,
                                     sync->cancellable, on_drive_list_done, sync);
}

static void
on_token_exchange_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    KajoWebSync *sync = user_data;
    if (err || !body) {
        sync->syncing = FALSE;
        emit_status(sync, "AUTH FAILED");
        return;
    }

    gsize len = 0;
    const char *raw = g_bytes_get_data(body, &len);
    g_autoptr(JsonParser) p = json_parser_new();
    if (!json_parser_load_from_data(p, raw, (gssize)len, NULL)) {
        sync->syncing = FALSE;
        emit_status(sync, "INVALID TOKEN JSON");
        return;
    }
    JsonNode *root = json_parser_get_root(p);
    JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root)) ? json_node_get_object(root) : NULL;
    const gchar *acc = obj ? json_object_get_string_member_with_default(obj, "access_token", "") : "";
    const gchar *ref = obj ? json_object_get_string_member_with_default(obj, "refresh_token", "") : "";
    if (!*acc) {
        sync->syncing = FALSE;
        emit_status(sync, "TOKEN EXPIRED / DENIED");
        return;
    }

    scrub_and_free(&sync->access_token);
    sync->access_token = g_strdup(acc);
    if (*ref) {
        scrub_and_free(&sync->refresh_token);
        sync->refresh_token = g_strdup(ref);
        store_refresh_token_async(sync, ref);
    }
    sync->connected = TRUE;
    emit_status(sync, "↻ SYNCING…");
    start_drive_list(sync);
}

static void
exchange_oauth_code(KajoWebSync *sync, const gchar *code)
{
    g_autofree gchar *redir = g_strdup_printf("http://127.0.0.1:%u/callback", sync->oauth_port);
    g_autofree gchar *form = soup_form_encode(
        "code", code,
        "client_id", sync->client_id ? sync->client_id : "",
        "client_secret", sync->client_secret ? sync->client_secret : "",
        "redirect_uri", redir,
        "code_verifier", sync->pkce_verifier ? sync->pkce_verifier : "",
        "grant_type", "authorization_code",
        NULL);
    g_autoptr(SoupMessage) msg = soup_message_new("POST", "https://oauth2.googleapis.com/token");
    g_autoptr(GBytes) b = g_bytes_new(form, strlen(form));
    soup_message_set_request_body_from_bytes(msg, "application/x-www-form-urlencoded", b);
    sync->syncing = TRUE;
    emit_status(sync, "EXCHANGING TOKEN…");
    soup_session_send_and_read_async(sync->soup, msg, G_PRIORITY_DEFAULT,
                                     sync->cancellable, on_token_exchange_done, sync);
}

static gboolean
on_oauth_incoming(GSocketService    *service G_GNUC_UNUSED,
                  GSocketConnection *conn,
                  GObject           *src G_GNUC_UNUSED,
                  gpointer           user_data)
{
    KajoWebSync *sync = user_data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

    char buf[2048];
    gssize n = g_input_stream_read(in, buf, sizeof(buf) - 1, sync->cancellable, NULL);
    if (n <= 0)
        return FALSE;
    buf[n] = '\0';

    g_autofree gchar *code = NULL;
    g_autofree gchar *state = NULL;
    char *q = strstr(buf, "GET /callback?");
    if (q) {
        q += strlen("GET /callback?");
        char *sp = strchr(q, ' ');
        if (sp)
            *sp = '\0';
        g_auto(GStrv) pairs = g_strsplit(q, "&", -1);
        for (gsize i = 0; pairs && pairs[i]; i++) {
            if (g_str_has_prefix(pairs[i], "code="))
                code = g_uri_unescape_string(pairs[i] + 5, NULL);
            else if (g_str_has_prefix(pairs[i], "state="))
                state = g_uri_unescape_string(pairs[i] + 6, NULL);
        }
    }

    const char *resp = (code && state && g_strcmp0(state, sync->oauth_state) == 0)
        ? "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
          "<html><body style='background:#101010;color:#fff;font-family:monospace;padding:40px;'>"
          "<h2>✓ KAJO WEB — GOOGLE DRIVE APPDATA CONNECTED</h2><p>You may close this tab.</p></body></html>"
        : "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOAuth state mismatch.";
    g_output_stream_write_all(out, resp, strlen(resp), NULL, NULL, NULL);

    if (sync->oauth_srv) {
        g_socket_service_stop(sync->oauth_srv);
        g_clear_object(&sync->oauth_srv);
    }

    if (code && state && g_strcmp0(state, sync->oauth_state) == 0)
        exchange_oauth_code(sync, code);
    else
        emit_status(sync, "OAUTH CANCELLED");
    return FALSE;
}

static gchar *
base64url_no_pad(const guchar *data, gsize len)
{
    gchar *b64 = g_base64_encode(data, len);
    for (char *p = b64; *p; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') { *p = '\0'; break; }
    }
    return b64;
}

gboolean
web_sync_begin_oauth(KajoWebSync *sync, const KajoWebCfg *cfg, GError **error)
{
    if (!sync || !cfg || !cfg->sync_oauth_client_id || !*cfg->sync_oauth_client_id) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Set sync_oauth_client_id in kajo-settings -> Kajo Web first");
        return FALSE;
    }

    if (sync->oauth_srv) {
        g_socket_service_stop(sync->oauth_srv);
        g_clear_object(&sync->oauth_srv);
    }

    sync->oauth_srv = g_socket_service_new();
    g_autoptr(GInetAddress) lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    g_autoptr(GSocketAddress) req_addr = g_inet_socket_address_new(lo, 0);
    g_autoptr(GSocketAddress) bound = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(sync->oauth_srv), req_addr,
                                       G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                       NULL, &bound, error)) {
        g_clear_object(&sync->oauth_srv);
        return FALSE;
    }
    sync->oauth_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_signal_connect(sync->oauth_srv, "incoming", G_CALLBACK(on_oauth_incoming), sync);
    g_socket_service_start(sync->oauth_srv);

    g_free(sync->client_id);
    sync->client_id = g_strdup(cfg->sync_oauth_client_id);
    scrub_and_free(&sync->client_secret);
    sync->client_secret = g_strdup(cfg->sync_oauth_client_secret ? cfg->sync_oauth_client_secret : "");

    g_autofree gchar *u1 = g_uuid_string_random();
    g_autofree gchar *u2 = g_uuid_string_random();
    scrub_and_free(&sync->pkce_verifier);
    sync->pkce_verifier = g_strdup_printf("%s%s", u1, u2);
    scrub_and_free(&sync->oauth_state);
    sync->oauth_state = g_uuid_string_random();

    g_autoptr(GChecksum) cs = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cs, (const guchar *)sync->pkce_verifier, (gssize)strlen(sync->pkce_verifier));
    guchar digest[32];
    gsize dlen = sizeof(digest);
    g_checksum_get_digest(cs, digest, &dlen);
    g_autofree gchar *challenge = base64url_no_pad(digest, dlen);

    g_autofree gchar *redir = g_strdup_printf("http://127.0.0.1:%u/callback", sync->oauth_port);
    g_autofree gchar *esc_id = g_uri_escape_string(sync->client_id, NULL, TRUE);
    g_autofree gchar *esc_redir = g_uri_escape_string(redir, NULL, TRUE);
    g_autofree gchar *auth_url = g_strdup_printf(
        "https://accounts.google.com/o/oauth2/v2/auth?"
        "client_id=%s&redirect_uri=%s&response_type=code&"
        "scope=https%%3A%%2F%%2Fwww.googleapis.com%%2Fauth%%2Fdrive.appdata+openid+email&"
        "access_type=offline&prompt=consent&code_challenge=%s&code_challenge_method=S256&state=%s",
        esc_id, esc_redir, challenge, sync->oauth_state);

    emit_status(sync, "WAITING FOR GOOGLE OAUTH…");
    if (sync->on_open_url)
        sync->on_open_url(auth_url, sync->user_data);
    return TRUE;
}

void
web_sync_sign_out(KajoWebSync *sync)
{
    if (!sync)
        return;
    scrub_and_free(&sync->access_token);
    scrub_and_free(&sync->refresh_token);
    sync->connected = FALSE;
    secret_password_clear(&k_sync_token_schema, sync->cancellable, NULL, NULL,
                          "profile_id", sync->profile_id, NULL);
    emit_status(sync, "LOCAL ONLY");
}

void
web_sync_trigger_now(KajoWebSync *sync, const KajoWebCfg *cfg, const gchar *passphrase)
{
    if (!sync || sync->syncing || !sync->refresh_token || !cfg ||
        !cfg->sync_oauth_client_id || !*cfg->sync_oauth_client_id) {
        return;
    }
    if (!cfg->sync_bookmarks) {
        emit_status(sync, "BOOKMARK SYNC DISABLED");
        return;
    }
    scrub_and_free(&sync->e2ee_passphrase);
    if (cfg->sync_e2ee_enabled && passphrase && *passphrase)
        sync->e2ee_passphrase = g_strdup(passphrase);

    g_autofree gchar *form = soup_form_encode(
        "refresh_token", sync->refresh_token,
        "client_id", cfg->sync_oauth_client_id,
        "client_secret", cfg->sync_oauth_client_secret ? cfg->sync_oauth_client_secret : "",
        "grant_type", "refresh_token",
        NULL);
    g_autoptr(SoupMessage) msg = soup_message_new("POST", "https://oauth2.googleapis.com/token");
    g_autoptr(GBytes) b = g_bytes_new(form, strlen(form));
    soup_message_set_request_body_from_bytes(msg, "application/x-www-form-urlencoded", b);
    sync->syncing = TRUE;
    emit_status(sync, "↻ SYNCING…");
    soup_session_send_and_read_async(sync->soup, msg, G_PRIORITY_DEFAULT,
                                     sync->cancellable, on_token_exchange_done, sync);
}

static gboolean
on_debounce_sync(gpointer user_data)
{
    KajoWebSync *sync = user_data;
    sync->debounce_id = 0;
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    web_sync_trigger_now(sync, cfg, sync->e2ee_passphrase);
    return G_SOURCE_REMOVE;
}

void
web_sync_schedule_debounced(KajoWebSync *sync, const KajoWebCfg *cfg G_GNUC_UNUSED)
{
    if (!sync || !sync->connected)
        return;
    if (sync->debounce_id)
        g_source_remove(sync->debounce_id);
    sync->debounce_id = g_timeout_add_seconds(30, on_debounce_sync, sync);
}

gboolean
web_sync_is_connected(const KajoWebSync *sync)
{
    return sync ? sync->connected : FALSE;
}

const gchar *
web_sync_get_status(const KajoWebSync *sync)
{
    return (sync && sync->status_text) ? sync->status_text : "LOCAL ONLY";
}
