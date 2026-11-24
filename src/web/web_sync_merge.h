#ifndef KAJO_WEB_SYNC_MERGE_H
#define KAJO_WEB_SYNC_MERGE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    gchar   *uuid;
    gchar   *title;
    gchar   *uri;
    gchar   *folder;
    gint64   updated_at;  /* UTC epoch milliseconds */
    gboolean deleted;     /* tombstone for CRDT delete sync */
} KajoWebBookmark;

KajoWebBookmark *web_bookmark_new(const gchar *uuid,
                                  const gchar *title,
                                  const gchar *uri,
                                  const gchar *folder,
                                  gint64       updated_at,
                                  gboolean     deleted);
KajoWebBookmark *web_bookmark_copy(const KajoWebBookmark *src);
void             web_bookmark_free(gpointer ptr);

/* Deterministic UUID + timestamp + tombstone CRDT merge (newer updated_at wins) */
GPtrArray *web_sync_merge_bookmarks(const GPtrArray *local_list,
                                    const GPtrArray *remote_list);

gchar     *web_sync_bookmarks_to_json(const GPtrArray *list);
GPtrArray *web_sync_bookmarks_from_json(const gchar *json_str, GError **error);

/* Optional client-side E2EE (PBKDF2-HMAC-SHA256 + AES-256-GCM via libgcrypt) */
gchar *web_sync_encrypt_payload(const gchar *plaintext,
                                const gchar *passphrase,
                                GError     **error);
gchar *web_sync_decrypt_payload(const gchar *envelope_json,
                                const gchar *passphrase,
                                GError     **error);

G_END_DECLS

#endif /* KAJO_WEB_SYNC_MERGE_H */
