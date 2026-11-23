#ifndef KAJO_WEB_STORAGE_H
#define KAJO_WEB_STORAGE_H

#include <glib.h>
#include "web_model.h"
#include "web_sync_merge.h"

G_BEGIN_DECLS

typedef struct _KajoWebStorage KajoWebStorage;

typedef struct {
    gchar *uri;
    gchar *title;
    gint   visit_count;
    gint64 last_visit_ms;
} KajoWebHistoryEntry;

void            web_history_entry_free(gpointer ptr);

KajoWebStorage *web_storage_new(const gchar *profile_data_dir);
void            web_storage_free(KajoWebStorage *st);

/* History (SQLite WAL + FTS5) */
void            web_storage_record_visit(KajoWebStorage *st, const gchar *uri, const gchar *title);
GPtrArray      *web_storage_search_history(KajoWebStorage *st, const gchar *query, gint limit);
void            web_storage_clear_history(KajoWebStorage *st);

/* Syncable Bookmarks (UUID + updated_at + tombstone) */
gboolean        web_storage_upsert_bookmark(KajoWebStorage *st, const KajoWebBookmark *bm);
gboolean        web_storage_delete_bookmark(KajoWebStorage *st, const gchar *uuid);
gboolean        web_storage_is_bookmarked(KajoWebStorage *st, const gchar *uri, gchar **out_uuid);
GPtrArray      *web_storage_list_bookmarks(KajoWebStorage *st, gboolean include_deleted);
gboolean        web_storage_replace_bookmarks(KajoWebStorage *st, const GPtrArray *merged);

/* Session persistence (atomic session.json) */
gboolean        web_storage_save_session(KajoWebStorage *st, const KajoWebModel *model);
gboolean        web_storage_load_session(KajoWebStorage *st, KajoWebModel *model);

G_END_DECLS

#endif /* KAJO_WEB_STORAGE_H */
