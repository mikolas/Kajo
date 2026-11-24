#include "web_storage.h"
#include <sqlite3.h>
#include <json-glib/json-glib.h>
#include <string.h>

struct _KajoWebStorage {
    gchar   *dir;
    gchar   *db_path;
    gchar   *session_path;
    sqlite3 *db;
};

void
web_history_entry_free(gpointer ptr)
{
    KajoWebHistoryEntry *e = ptr;
    if (!e)
        return;
    g_free(e->uri);
    g_free(e->title);
    g_free(e);
}

static void
init_schema(sqlite3 *db)
{
    static const char *sql =
        "PRAGMA busy_timeout=3000;"
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS history ("
        "  uri TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  visit_count INTEGER NOT NULL DEFAULT 1,"
        "  last_visit_ms INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_history_visit ON history(last_visit_ms DESC);"
        "CREATE TABLE IF NOT EXISTS bookmarks ("
        "  uuid TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  uri TEXT NOT NULL DEFAULT '',"
        "  folder TEXT NOT NULL DEFAULT '',"
        "  updated_at INTEGER NOT NULL,"
        "  deleted INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_bookmarks_uri ON bookmarks(uri, deleted);";
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

KajoWebStorage *
web_storage_new(const gchar *profile_data_dir)
{
    if (!profile_data_dir || !*profile_data_dir)
        return NULL;
    g_mkdir_with_parents(profile_data_dir, 0700);
    KajoWebStorage *st = g_new0(KajoWebStorage, 1);
    st->dir = g_strdup(profile_data_dir);
    st->db_path = g_build_filename(profile_data_dir, "history.db", NULL);
    st->session_path = g_build_filename(profile_data_dir, "session.json", NULL);

    if (sqlite3_open(st->db_path, &st->db) == SQLITE_OK) {
        init_schema(st->db);
    } else {
        g_warning("kajo-web: failed to open %s", st->db_path);
        g_clear_pointer(&st->db, sqlite3_close);
    }
    return st;
}

void
web_storage_free(KajoWebStorage *st)
{
    if (!st)
        return;
    g_clear_pointer(&st->db, sqlite3_close);
    g_free(st->session_path);
    g_free(st->db_path);
    g_free(st->dir);
    g_free(st);
}

void
web_storage_record_visit(KajoWebStorage *st, const gchar *uri, const gchar *title)
{
    if (!st || !st->db || !uri || !*uri || g_str_has_prefix(uri, "kajo://") || g_str_has_prefix(uri, "about:"))
        return;
    const char *sql =
        "INSERT INTO history(uri, title, visit_count, last_visit_ms) VALUES(?1, ?2, 1, ?3) "
        "ON CONFLICT(uri) DO UPDATE SET "
        "  title = CASE WHEN length(?2) > 0 THEN ?2 ELSE history.title END, "
        "  visit_count = history.visit_count + 1, "
        "  last_visit_ms = ?3;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, uri, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, g_get_real_time() / 1000);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

GPtrArray *
web_storage_search_history(KajoWebStorage *st, const gchar *query, gint limit)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(web_history_entry_free);
    if (!st || !st->db)
        return out;
    if (limit <= 0)
        limit = 30;

    sqlite3_stmt *stmt = NULL;
    if (query && *query) {
        const char *sql =
            "SELECT uri, title, visit_count, last_visit_ms FROM history "
            "WHERE uri LIKE ?1 OR title LIKE ?1 "
            "ORDER BY last_visit_ms DESC LIMIT ?2;";
        g_autofree gchar *pat = g_strdup_printf("%%%s%%", query);
        if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, pat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, limit);
        }
    } else {
        const char *sql =
            "SELECT uri, title, visit_count, last_visit_ms FROM history "
            "ORDER BY last_visit_ms DESC LIMIT ?1;";
        if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, limit);
        }
    }
    if (!stmt)
        return out;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        KajoWebHistoryEntry *e = g_new0(KajoWebHistoryEntry, 1);
        const unsigned char *u = sqlite3_column_text(stmt, 0);
        const unsigned char *t = sqlite3_column_text(stmt, 1);
        e->uri = g_strdup(u ? (const char *)u : "");
        e->title = g_strdup(t ? (const char *)t : "");
        e->visit_count = sqlite3_column_int(stmt, 2);
        e->last_visit_ms = sqlite3_column_int64(stmt, 3);
        g_ptr_array_add(out, e);
    }
    sqlite3_finalize(stmt);
    return out;
}

void
web_storage_delete_history_uri(KajoWebStorage *st, const gchar *uri)
{
    if (!st || !st->db || !uri || !*uri)
        return;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(st->db, "DELETE FROM history WHERE uri = ?1;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, uri, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void
web_storage_clear_history(KajoWebStorage *st)
{
    if (!st || !st->db)
        return;
    sqlite3_exec(st->db, "DELETE FROM history;", NULL, NULL, NULL);
}

gboolean
web_storage_upsert_bookmark(KajoWebStorage *st, const KajoWebBookmark *bm)
{
    if (!st || !st->db || !bm || !bm->uuid)
        return FALSE;
    const char *sql =
        "INSERT INTO bookmarks(uuid, title, uri, folder, updated_at, deleted) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(uuid) DO UPDATE SET "
        "  title=?2, uri=?3, folder=?4, updated_at=?5, deleted=?6;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(stmt, 1, bm->uuid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, bm->title ? bm->title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, bm->uri ? bm->uri : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, bm->folder ? bm->folder : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, bm->updated_at);
    sqlite3_bind_int(stmt, 6, bm->deleted ? 1 : 0);
    gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
web_storage_delete_bookmark(KajoWebStorage *st, const gchar *uuid)
{
    if (!st || !st->db || !uuid || !*uuid)
        return FALSE;
    const char *sql = "UPDATE bookmarks SET deleted = 1, updated_at = ?2 WHERE uuid = ?1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, g_get_real_time() / 1000);
    gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
web_storage_is_bookmarked(KajoWebStorage *st, const gchar *uri, gchar **out_uuid)
{
    if (out_uuid)
        *out_uuid = NULL;
    if (!st || !st->db || !uri || !*uri)
        return FALSE;
    const char *sql = "SELECT uuid FROM bookmarks WHERE uri = ?1 AND deleted = 0 LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(stmt, 1, uri, -1, SQLITE_TRANSIENT);
    gboolean found = FALSE;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = TRUE;
        const unsigned char *u = sqlite3_column_text(stmt, 0);
        if (out_uuid && u)
            *out_uuid = g_strdup((const char *)u);
    }
    sqlite3_finalize(stmt);
    return found;
}

GPtrArray *
web_storage_list_bookmarks(KajoWebStorage *st, gboolean include_deleted)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(web_bookmark_free);
    if (!st || !st->db)
        return out;
    const char *sql = include_deleted
        ? "SELECT uuid, title, uri, folder, updated_at, deleted FROM bookmarks ORDER BY updated_at DESC;"
        : "SELECT uuid, title, uri, folder, updated_at, deleted FROM bookmarks WHERE deleted = 0 ORDER BY updated_at DESC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *uuid   = sqlite3_column_text(stmt, 0);
        const unsigned char *title  = sqlite3_column_text(stmt, 1);
        const unsigned char *uri    = sqlite3_column_text(stmt, 2);
        const unsigned char *folder = sqlite3_column_text(stmt, 3);
        gint64 ts = sqlite3_column_int64(stmt, 4);
        gboolean del = sqlite3_column_int(stmt, 5) != 0;
        g_ptr_array_add(out, web_bookmark_new(
            uuid ? (const char *)uuid : "",
            title ? (const char *)title : "",
            uri ? (const char *)uri : "",
            folder ? (const char *)folder : "",
            ts, del));
    }
    sqlite3_finalize(stmt);
    return out;
}

gboolean
web_storage_replace_bookmarks(KajoWebStorage *st, const GPtrArray *merged)
{
    if (!st || !st->db || !merged)
        return FALSE;
    sqlite3_exec(st->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    for (guint i = 0; i < merged->len; i++) {
        web_storage_upsert_bookmark(st, g_ptr_array_index(merged, i));
    }
    return sqlite3_exec(st->db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK;
}

gboolean
web_storage_save_session(KajoWebStorage *st, const KajoWebModel *model)
{
    if (!st || !model)
        return FALSE;
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "active_index");
    gint active_idx = 0;
    for (guint i = 0; i < model->tabs->len; i++) {
        if (((KajoWebTabMeta *)g_ptr_array_index(model->tabs, i))->id == model->active_id) {
            active_idx = (gint)i;
            break;
        }
    }
    json_builder_add_int_value(b, active_idx);
    json_builder_set_member_name(b, "tabs");
    json_builder_begin_array(b);
    for (guint i = 0; i < model->tabs->len; i++) {
        const KajoWebTabMeta *t = g_ptr_array_index(model->tabs, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "uri");
        json_builder_add_string_value(b, t->uri ? t->uri : "kajo://newtab");
        json_builder_set_member_name(b, "title");
        json_builder_add_string_value(b, t->title ? t->title : "");
        json_builder_set_member_name(b, "group");
        json_builder_add_string_value(b, t->group ? t->group : "");
        json_builder_set_member_name(b, "pinned");
        json_builder_add_boolean_value(b, t->pinned);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    g_autofree gchar *data = json_generator_to_data(gen, NULL);
    return g_file_set_contents(st->session_path, data, -1, NULL);
}

gboolean
web_storage_load_session(KajoWebStorage *st, KajoWebModel *model)
{
    if (!st || !model || !g_file_test(st->session_path, G_FILE_TEST_EXISTS))
        return FALSE;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, st->session_path, NULL))
        return FALSE;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return FALSE;
    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "tabs"))
        return FALSE;

    JsonArray *tabs = json_object_get_array_member(obj, "tabs");
    guint n = tabs ? json_array_get_length(tabs) : 0;
    if (n == 0)
        return FALSE;

    gint64 now = g_get_monotonic_time();
    gint active_idx = (gint)json_object_get_int_member_with_default(obj, "active_index", 0);
    guint64 target_active_id = 0;

    for (guint i = 0; i < n; i++) {
        JsonObject *it = json_array_get_object_element(tabs, i);
        if (!it)
            continue;
        const gchar *uri = json_object_get_string_member_with_default(it, "uri", "kajo://newtab");
        const gchar *title = json_object_get_string_member_with_default(it, "title", "");
        const gchar *grp = json_object_get_string_member_with_default(it, "group", "");
        gboolean pinned = json_object_get_boolean_member_with_default(it, "pinned", FALSE);
        KajoWebTabMeta *t = web_model_add_tab(model, uri, title, *grp ? grp : NULL, pinned, now);
        if ((gint)i == active_idx && t)
            target_active_id = t->id;
    }
    if (target_active_id)
        web_model_activate(model, target_active_id, now);
    return model->tabs->len > 0;
}
