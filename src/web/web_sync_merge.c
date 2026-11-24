#define _GNU_SOURCE
#include "web_sync_merge.h"
#include <json-glib/json-glib.h>
#include <gcrypt.h>
#include <string.h>

KajoWebBookmark *
web_bookmark_new(const gchar *uuid,
                 const gchar *title,
                 const gchar *uri,
                 const gchar *folder,
                 gint64       updated_at,
                 gboolean     deleted)
{
    KajoWebBookmark *b = g_new0(KajoWebBookmark, 1);
    b->uuid = (uuid && *uuid) ? g_strdup(uuid) : g_uuid_string_random();
    b->title = g_strdup(title ? title : "");
    b->uri = g_strdup(uri ? uri : "");
    b->folder = g_strdup(folder ? folder : "");
    b->updated_at = updated_at > 0 ? updated_at : (g_get_real_time() / 1000);
    b->deleted = deleted;
    return b;
}

KajoWebBookmark *
web_bookmark_copy(const KajoWebBookmark *src)
{
    if (!src)
        return NULL;
    return web_bookmark_new(src->uuid, src->title, src->uri, src->folder,
                            src->updated_at, src->deleted);
}

void
web_bookmark_free(gpointer ptr)
{
    KajoWebBookmark *b = ptr;
    if (!b)
        return;
    g_free(b->uuid);
    g_free(b->title);
    g_free(b->uri);
    g_free(b->folder);
    g_free(b);
}

GPtrArray *
web_sync_merge_bookmarks(const GPtrArray *local_list,
                         const GPtrArray *remote_list)
{
    g_autoptr(GHashTable) map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      NULL, web_bookmark_free);
    const GPtrArray *inputs[2] = { local_list, remote_list };
    for (int k = 0; k < 2; k++) {
        if (!inputs[k])
            continue;
        for (guint i = 0; i < inputs[k]->len; i++) {
            const KajoWebBookmark *cand = g_ptr_array_index(inputs[k], i);
            if (!cand || !cand->uuid || !*cand->uuid)
                continue;
            const KajoWebBookmark *prev = g_hash_table_lookup(map, cand->uuid);
            if (!prev ||
                cand->updated_at > prev->updated_at ||
                (cand->updated_at == prev->updated_at && cand->deleted && !prev->deleted)) {
                KajoWebBookmark *cp = web_bookmark_copy(cand);
                g_hash_table_replace(map, cp->uuid, cp);
            }
        }
    }

    GPtrArray *merged = g_ptr_array_new_with_free_func(web_bookmark_free);
    GHashTableIter iter;
    gpointer val = NULL;
    g_hash_table_iter_init(&iter, map);
    while (g_hash_table_iter_next(&iter, NULL, &val)) {
        g_ptr_array_add(merged, web_bookmark_copy((const KajoWebBookmark *)val));
    }
    return merged;
}

gchar *
web_sync_bookmarks_to_json(const GPtrArray *list)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "schema");
    json_builder_add_int_value(b, 1);
    json_builder_set_member_name(b, "bookmarks");
    json_builder_begin_array(b);
    if (list) {
        for (guint i = 0; i < list->len; i++) {
            const KajoWebBookmark *bm = g_ptr_array_index(list, i);
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "uuid");
            json_builder_add_string_value(b, bm->uuid ? bm->uuid : "");
            json_builder_set_member_name(b, "title");
            json_builder_add_string_value(b, bm->title ? bm->title : "");
            json_builder_set_member_name(b, "uri");
            json_builder_add_string_value(b, bm->uri ? bm->uri : "");
            json_builder_set_member_name(b, "folder");
            json_builder_add_string_value(b, bm->folder ? bm->folder : "");
            json_builder_set_member_name(b, "updated_at");
            json_builder_add_int_value(b, bm->updated_at);
            json_builder_set_member_name(b, "deleted");
            json_builder_add_boolean_value(b, bm->deleted);
            json_builder_end_object(b);
        }
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    return json_generator_to_data(gen, NULL);
}

GPtrArray *
web_sync_bookmarks_from_json(const gchar *json_str, GError **error)
{
    if (!json_str || !*json_str)
        return g_ptr_array_new_with_free_func(web_bookmark_free);

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, -1, error))
        return NULL;

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid bookmarks JSON root");
        return NULL;
    }
    JsonObject *obj = json_node_get_object(root);
    GPtrArray *out = g_ptr_array_new_with_free_func(web_bookmark_free);
    if (!json_object_has_member(obj, "bookmarks"))
        return out;

    JsonArray *arr = json_object_get_array_member(obj, "bookmarks");
    guint n = arr ? json_array_get_length(arr) : 0;
    for (guint i = 0; i < n; i++) {
        JsonObject *it = json_array_get_object_element(arr, i);
        if (!it)
            continue;
        const gchar *uuid = json_object_get_string_member_with_default(it, "uuid", "");
        const gchar *title = json_object_get_string_member_with_default(it, "title", "");
        const gchar *uri = json_object_get_string_member_with_default(it, "uri", "");
        const gchar *folder = json_object_get_string_member_with_default(it, "folder", "");
        gint64 ts = json_object_get_int_member_with_default(it, "updated_at", 0);
        gboolean del = json_object_get_boolean_member_with_default(it, "deleted", FALSE);
        if (*uuid)
            g_ptr_array_add(out, web_bookmark_new(uuid, title, uri, folder, ts, del));
    }
    return out;
}

/* ─── Optional AES-256-GCM + PBKDF2-HMAC-SHA256 E2EE Envelope ─── */

static void
ensure_gcrypt_init(void)
{
    static gsize once = 0;
    if (g_once_init_enter(&once)) {
        if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) {
            gcry_check_version(NULL);
            gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
            gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
        }
        g_once_init_leave(&once, 1);
    }
}

#define KAJO_E2EE_ITERATIONS 100000

gchar *
web_sync_encrypt_payload(const gchar *plaintext,
                         const gchar *passphrase,
                         GError     **error)
{
    if (!plaintext || !passphrase || !*passphrase) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing plaintext or passphrase");
        return NULL;
    }
    ensure_gcrypt_init();

    unsigned char salt[16];
    unsigned char iv[12];
    unsigned char key[32];
    unsigned char tag[16];
    gcry_randomize(salt, sizeof(salt), GCRY_STRONG_RANDOM);
    gcry_create_nonce(iv, sizeof(iv));

    gcry_error_t err = gcry_kdf_derive(passphrase, strlen(passphrase),
                                       GCRY_KDF_PBKDF2, GCRY_MD_SHA256,
                                       salt, sizeof(salt),
                                       KAJO_E2EE_ITERATIONS, sizeof(key), key);
    if (err != 0) {
        explicit_bzero(key, sizeof(key));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "KDF failed: %s", gcry_strerror(err));
        return NULL;
    }

    gcry_cipher_hd_t hd = NULL;
    err = gcry_cipher_open(&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
    if (err == 0)
        err = gcry_cipher_setkey(hd, key, sizeof(key));
    if (err == 0)
        err = gcry_cipher_setiv(hd, iv, sizeof(iv));

    gsize pt_len = strlen(plaintext);
    g_autofree unsigned char *ct = g_malloc(pt_len > 0 ? pt_len : 1);
    if (err == 0 && pt_len > 0)
        err = gcry_cipher_encrypt(hd, ct, pt_len, plaintext, pt_len);
    if (err == 0)
        err = gcry_cipher_gettag(hd, tag, sizeof(tag));

    explicit_bzero(key, sizeof(key));
    if (hd)
        gcry_cipher_close(hd);

    if (err != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "AES-GCM encrypt failed: %s", gcry_strerror(err));
        return NULL;
    }

    g_autofree gchar *salt_b64 = g_base64_encode(salt, sizeof(salt));
    g_autofree gchar *iv_b64 = g_base64_encode(iv, sizeof(iv));
    g_autofree gchar *tag_b64 = g_base64_encode(tag, sizeof(tag));
    g_autofree gchar *ct_b64 = g_base64_encode(ct, pt_len);

    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "e2ee");
    json_builder_add_string_value(b, "aes-256-gcm-v1");
    json_builder_set_member_name(b, "iter");
    json_builder_add_int_value(b, KAJO_E2EE_ITERATIONS);
    json_builder_set_member_name(b, "salt");
    json_builder_add_string_value(b, salt_b64);
    json_builder_set_member_name(b, "iv");
    json_builder_add_string_value(b, iv_b64);
    json_builder_set_member_name(b, "tag");
    json_builder_add_string_value(b, tag_b64);
    json_builder_set_member_name(b, "ct");
    json_builder_add_string_value(b, ct_b64);
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    return json_generator_to_data(gen, NULL);
}

gchar *
web_sync_decrypt_payload(const gchar *envelope_json,
                         const gchar *passphrase,
                         GError     **error)
{
    if (!envelope_json || !passphrase || !*passphrase) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing envelope or passphrase");
        return NULL;
    }
    ensure_gcrypt_init();

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, envelope_json, -1, error))
        return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid E2EE envelope");
        return NULL;
    }
    JsonObject *obj = json_node_get_object(root);
    const gchar *alg = json_object_get_string_member_with_default(obj, "e2ee", "");
    if (g_strcmp0(alg, "aes-256-gcm-v1") != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported E2EE scheme");
        return NULL;
    }
    gint64 iter = json_object_get_int_member_with_default(obj, "iter", KAJO_E2EE_ITERATIONS);
    if (iter < 10000 || iter > 2000000) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid KDF iteration count");
        return NULL;
    }

    gsize salt_len = 0, iv_len = 0, tag_len = 0, ct_len = 0;
    g_autofree guchar *salt = g_base64_decode(json_object_get_string_member_with_default(obj, "salt", ""), &salt_len);
    g_autofree guchar *iv   = g_base64_decode(json_object_get_string_member_with_default(obj, "iv", ""), &iv_len);
    g_autofree guchar *tag  = g_base64_decode(json_object_get_string_member_with_default(obj, "tag", ""), &tag_len);
    g_autofree guchar *ct   = g_base64_decode(json_object_get_string_member_with_default(obj, "ct", ""), &ct_len);
    if (salt_len != 16 || iv_len != 12 || tag_len != 16) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Malformed E2EE parameters");
        return NULL;
    }

    unsigned char key[32];
    gcry_error_t err = gcry_kdf_derive(passphrase, strlen(passphrase),
                                       GCRY_KDF_PBKDF2, GCRY_MD_SHA256,
                                       salt, salt_len,
                                       (unsigned long)iter, sizeof(key), key);
    if (err != 0) {
        explicit_bzero(key, sizeof(key));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "KDF failed: %s", gcry_strerror(err));
        return NULL;
    }

    gcry_cipher_hd_t hd = NULL;
    err = gcry_cipher_open(&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
    if (err == 0)
        err = gcry_cipher_setkey(hd, key, sizeof(key));
    if (err == 0)
        err = gcry_cipher_setiv(hd, iv, iv_len);

    g_autofree gchar *pt = g_malloc0(ct_len + 1);
    if (err == 0 && ct_len > 0)
        err = gcry_cipher_decrypt(hd, pt, ct_len, ct, ct_len);
    if (err == 0)
        err = gcry_cipher_checktag(hd, tag, tag_len);

    explicit_bzero(key, sizeof(key));
    if (hd)
        gcry_cipher_close(hd);

    if (err != 0) {
        explicit_bzero(pt, ct_len);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Wrong passphrase or tampered payload");
        return NULL;
    }
    return g_steal_pointer(&pt);
}
