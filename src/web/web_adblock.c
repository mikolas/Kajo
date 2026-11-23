#include "web_adblock.h"
#include "../config/kajo_config_schema.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Lightweight YouTube in-stream ad skipper (active ONLY when adblock_enabled == TRUE,
 * scoped strictly to youtube.com via WebKitUserScript allow_list).
 * WebKit Content Blocker blocks third-party & /pagead/ requests, while this handles
 * YouTube's server-side stitched googlevideo.com ad segments. */
static const char k_yt_adskip_js[] =
    "(function(){"
    "if(window.__kajoYtAdSkip)return;window.__kajoYtAdSkip=true;"
    "function tick(){"
    "  var p=document.querySelector('.html5-video-player');"
    "  if(!p)return;"
    "  var ad=p.classList.contains('ad-showing')||p.classList.contains('ad-interrupting');"
    "  var v=p.querySelector('video');"
    "  if(ad&&v){"
    "    v.muted=true;"
    "    if(isFinite(v.duration)&&v.duration>0){try{v.currentTime=v.duration;}catch(e){}}"
    "    var b=document.querySelector('.ytp-ad-skip-button,.ytp-ad-skip-button-modern,.ytp-skip-ad-button');"
    "    if(b)b.click();"
    "  }"
    "}"
    "var mo=new MutationObserver(tick);"
    "function boot(){"
    "  if(document.body)mo.observe(document.body,{subtree:true,childList:true,attributes:true,attributeFilter:['class']});"
    "  setInterval(tick,350);"
    "}"
    "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',boot);else boot();"
    "})();";

struct _KajoWebAdblock {
    gchar                        *cache_dir;
    gchar                        *community_json_path;
    WebKitUserContentFilterStore *store;
    WebKitUserContentFilter      *filter;
    WebKitUserScript             *yt_adskip_script;
    SoupSession                  *soup;
    GPtrArray                    *managers;     /* borrowed WebKitUserContentManager* */
    GPtrArray                    *user_scripts; /* WebKitUserScript* */
    GPtrArray                    *user_styles;  /* WebKitUserStyleSheet* */
    GCancellable                 *cancellable;
    gboolean                      enabled;
    gboolean                      user_scripts_enabled;
    gboolean                      updating;
    gchar                        *custom_rules_path;
    gchar                        *pending_filter_id;
    guint                         rule_count;
    gchar                        *status_text;
    KajoWebAdblockReadyCb         on_ready;
    gpointer                      on_ready_ud;
};

static guint
count_json_array_rules(const gchar *json_data, gsize len)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_data, (gssize)len, NULL))
        return 0;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root))
        return 0;
    return json_array_get_length(json_node_get_array(root));
}

static void
apply_to_manager(KajoWebAdblock *ab, WebKitUserContentManager *ucm)
{
    if (!ab || !ucm)
        return;
    webkit_user_content_manager_remove_all_filters(ucm);
    if (ab->enabled && ab->filter)
        webkit_user_content_manager_add_filter(ucm, ab->filter);

    webkit_user_content_manager_remove_all_scripts(ucm);
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    if (ab->enabled && ab->yt_adskip_script)
        webkit_user_content_manager_add_script(ucm, ab->yt_adskip_script);

    if (ab->user_scripts_enabled) {
        for (guint i = 0; i < ab->user_scripts->len; i++)
            webkit_user_content_manager_add_script(ucm, g_ptr_array_index(ab->user_scripts, i));
        for (guint i = 0; i < ab->user_styles->len; i++)
            webkit_user_content_manager_add_style_sheet(ucm, g_ptr_array_index(ab->user_styles, i));
    }
}

static void
sync_all_managers(KajoWebAdblock *ab)
{
    for (guint i = 0; i < ab->managers->len; i++) {
        apply_to_manager(ab, WEBKIT_USER_CONTENT_MANAGER(g_ptr_array_index(ab->managers, i)));
    }
}

static void
reload_user_scripts(KajoWebAdblock *ab)
{
    g_ptr_array_set_size(ab->user_scripts, 0);
    g_ptr_array_set_size(ab->user_styles, 0);
    if (!ab->user_scripts_enabled)
        return;

    g_autofree gchar *cfg_dir = kajo_config_resolve_user_dir();
    g_autofree gchar *scripts_dir = g_build_filename(cfg_dir, "web", "scripts", NULL);
    g_autoptr(GDir) dir = g_dir_open(scripts_dir, 0, NULL);
    if (!dir)
        return;

    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *full = g_build_filename(scripts_dir, name, NULL);
        g_autofree gchar *content = NULL;
        gsize len = 0;
        if (!g_file_get_contents(full, &content, &len, NULL) || len == 0)
            continue;
        if (g_str_has_suffix(name, ".js")) {
            WebKitUserScript *s = webkit_user_script_new(
                content, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
            g_ptr_array_add(ab->user_scripts, s);
        } else if (g_str_has_suffix(name, ".css")) {
            WebKitUserStyleSheet *css = webkit_user_style_sheet_new(
                content, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
            g_ptr_array_add(ab->user_styles, css);
        }
    }
}

static gchar *
load_active_rules_json(KajoWebAdblock *ab, gsize *out_len)
{
    g_autofree gchar *rules_path = NULL;
    if (ab->custom_rules_path && *ab->custom_rules_path &&
        g_file_test(ab->custom_rules_path, G_FILE_TEST_EXISTS)) {
        rules_path = g_strdup(ab->custom_rules_path);
    } else if (ab->community_json_path && g_file_test(ab->community_json_path, G_FILE_TEST_EXISTS)) {
        rules_path = g_strdup(ab->community_json_path);
    } else {
        rules_path = kajo_config_find_default_file("web-adblock.json");
        if (!rules_path || !g_file_test(rules_path, G_FILE_TEST_EXISTS)) {
            g_free(rules_path);
            rules_path = g_build_filename(KAJO_SOURCE_DIR, "data", "defaults", "web-adblock.json", NULL);
        }
    }

    gchar *json_data = NULL;
    gsize len = 0;
    if (!rules_path || !g_file_get_contents(rules_path, &json_data, &len, NULL) || len == 0) {
        g_free(json_data);
        static const char k_fallback[] =
            "[{\"trigger\":{\"url-filter\":\"^https?://([^/]+\\\\.)?doubleclick\\\\.net/\"},"
            "\"action\":{\"type\":\"block\"}}]";
        json_data = g_strdup(k_fallback);
        len = strlen(k_fallback);
    }
    if (out_len)
        *out_len = len;
    return json_data;
}

static void
finish_filter_ready(KajoWebAdblock *ab, WebKitUserContentFilter *compiled)
{
    g_clear_pointer(&ab->filter, webkit_user_content_filter_unref);
    ab->filter = compiled;
    g_free(ab->status_text);
    ab->status_text = g_strdup_printf("ACTIVE (%u RULES)", ab->rule_count);
    sync_all_managers(ab);
    if (ab->on_ready)
        ab->on_ready(ab, ab->on_ready_ud);
}

static void
on_filter_compiled(GObject *source, GAsyncResult *res, gpointer user_data)
{
    g_autoptr(GError) err = NULL;
    WebKitUserContentFilter *compiled =
        webkit_user_content_filter_store_save_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (compiled)
            webkit_user_content_filter_unref(compiled);
        return;
    }

    KajoWebAdblock *ab = user_data;
    ab->updating = FALSE;
    if (err || !compiled) {
        g_warning("kajo-web: content filter compile failed: %s", err ? err->message : "unknown");
        g_free(ab->status_text);
        ab->status_text = g_strdup("FILTER COMPILE ERROR");
        if (ab->on_ready)
            ab->on_ready(ab, ab->on_ready_ud);
        return;
    }
    finish_filter_ready(ab, compiled);
}

typedef struct {
    KajoWebAdblock *ab;
    GBytes         *json_bytes;
    gchar          *filter_id;
} LoadOrCompileCtx;

static void
load_or_compile_ctx_free(LoadOrCompileCtx *ctx)
{
    if (!ctx)
        return;
    g_clear_pointer(&ctx->json_bytes, g_bytes_unref);
    g_free(ctx->filter_id);
    g_free(ctx);
}

static void
on_filter_loaded_from_cache(GObject *source, GAsyncResult *res, gpointer user_data)
{
    LoadOrCompileCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    WebKitUserContentFilter *loaded =
        webkit_user_content_filter_store_load_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (loaded)
            webkit_user_content_filter_unref(loaded);
        load_or_compile_ctx_free(ctx);
        return;
    }

    KajoWebAdblock *ab = ctx->ab;
    if (loaded) {
        load_or_compile_ctx_free(ctx);
        finish_filter_ready(ab, loaded);
        return;
    }

    /* Cache miss -> compile once and persist under hash-keyed ID */
    webkit_user_content_filter_store_save(ab->store, ctx->filter_id,
                                          ctx->json_bytes, ab->cancellable,
                                          on_filter_compiled, ab);
    load_or_compile_ctx_free(ctx);
}

static void
load_or_compile_rules_async(KajoWebAdblock *ab)
{
    gsize len = 0;
    g_autofree gchar *json_data = load_active_rules_json(ab, &len);
    ab->rule_count = count_json_array_rules(json_data, len);

    g_autofree gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)json_data, len);
    g_autofree gchar *fid = g_strdup_printf("kajo-ab-%.12s", sha ? sha : "v2");
    g_free(ab->pending_filter_id);
    ab->pending_filter_id = g_strdup(fid);

    LoadOrCompileCtx *ctx = g_new0(LoadOrCompileCtx, 1);
    ctx->ab = ab;
    ctx->json_bytes = g_bytes_new(json_data, len);
    ctx->filter_id = g_strdup(fid);

    webkit_user_content_filter_store_load(ab->store, fid, ab->cancellable,
                                          on_filter_loaded_from_cache, ctx);
}

/* ─── Online Filter List Updater (Peter Lowe's Ad & Tracker Server List) ─── */

static gboolean
is_valid_ascii_hostname(const gchar *s)
{
    if (!s || !*s || !strchr(s, '.'))
        return FALSE;
    for (const gchar *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(g_ascii_isalnum(c) || c == '.' || c == '-' || c == '_'))
            return FALSE;
    }
    if (g_strcmp0(s, "localhost") == 0 || g_strcmp0(s, "local") == 0)
        return FALSE;
    return TRUE;
}

static gchar *
build_merged_community_json(const gchar *base_json, const gchar *hosts_text, guint *out_count)
{
    GString *out = g_string_new("[\n");
    guint count = 0;

    /* 1. Keep every rule from the built-in rulepack (YouTube, cosmetics, paths) */
    g_autoptr(JsonParser) parser = json_parser_new();
    if (base_json && json_parser_load_from_data(parser, base_json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *arr = json_node_get_array(root);
            guint n = json_array_get_length(arr);
            for (guint i = 0; i < n; i++) {
                g_autoptr(JsonGenerator) gen = json_generator_new();
                json_generator_set_root(gen, json_array_get_element(arr, i));
                g_autofree gchar *elem = json_generator_to_data(gen, NULL);
                if (elem && *elem) {
                    if (count > 0)
                        g_string_append(out, ",\n");
                    g_string_append(out, elem);
                    count++;
                }
            }
        }
    }

    /* 2. Append hosts-file domains from Peter Lowe's list as native WebKit block rules */
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_auto(GStrv) lines = g_strsplit(hosts_text ? hosts_text : "", "\n", -1);
    for (gint i = 0; lines && lines[i] && count < 12000; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line || line[0] == '#')
            continue;
        gchar *domain = line;
        if (g_str_has_prefix(line, "127.0.0.1") || g_str_has_prefix(line, "0.0.0.0")) {
            domain = line + (g_str_has_prefix(line, "127.0.0.1") ? 9 : 7);
            while (*domain == ' ' || *domain == '\t')
                domain++;
        }
        gchar *comment = strchr(domain, '#');
        if (comment)
            *comment = '\0';
        g_strstrip(domain);
        g_autofree gchar *low = g_ascii_strdown(domain, -1);
        if (!is_valid_ascii_hostname(low) || g_hash_table_contains(seen, low))
            continue;
        g_hash_table_add(seen, g_strdup(low));

        /* Escape dots for WebKit ASCII regex */
        GString *rx = g_string_new("^https?://([^/]+\\\\.)?");
        for (const gchar *p = low; *p; p++) {
            if (*p == '.')
                g_string_append(rx, "\\\\.");
            else
                g_string_append_c(rx, *p);
        }
        g_string_append(rx, "/");

        if (count > 0)
            g_string_append(out, ",\n");
        g_string_append_printf(out,
            "{\"trigger\":{\"url-filter\":\"%s\"},\"action\":{\"type\":\"block\"}}",
            rx->str);
        g_string_free(rx, TRUE);
        count++;
    }

    g_string_append(out, "\n]\n");
    if (out_count)
        *out_count = count;
    return g_string_free(out, FALSE);
}

static void
on_community_list_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    KajoWebAdblock *ab = user_data;
    if (err || !body) {
        ab->updating = FALSE;
        g_free(ab->status_text);
        ab->status_text = g_strdup("UPDATE FAILED (OFFLINE)");
        if (ab->on_ready)
            ab->on_ready(ab, ab->on_ready_ud);
        return;
    }

    gsize hlen = 0;
    const gchar *hdata = g_bytes_get_data(body, &hlen);
    g_autofree gchar *hosts_str = g_strndup(hdata ? hdata : "", hlen);

    /* Load shipped base rulepack */
    g_autofree gchar *base_path = kajo_config_find_default_file("web-adblock.json");
    if (!base_path || !g_file_test(base_path, G_FILE_TEST_EXISTS)) {
        g_free(base_path);
        base_path = g_build_filename(KAJO_SOURCE_DIR, "data", "defaults", "web-adblock.json", NULL);
    }
    g_autofree gchar *base_json = NULL;
    g_file_get_contents(base_path, &base_json, NULL, NULL);

    guint merged_count = 0;
    g_autofree gchar *merged = build_merged_community_json(base_json, hosts_str, &merged_count);
    if (merged_count > 100 && ab->community_json_path) {
        g_file_set_contents(ab->community_json_path, merged, -1, NULL);
        g_free(ab->status_text);
        ab->status_text = g_strdup_printf("COMPILING %u RULES...", merged_count);
        load_or_compile_rules_async(ab);
    } else {
        ab->updating = FALSE;
        g_free(ab->status_text);
        ab->status_text = g_strdup("UPDATE FAILED (EMPTY LIST)");
        if (ab->on_ready)
            ab->on_ready(ab, ab->on_ready_ud);
    }
}

void
web_adblock_update_lists_async(KajoWebAdblock *ab)
{
    if (!ab || ab->updating)
        return;
    ab->updating = TRUE;
    g_free(ab->status_text);
    ab->status_text = g_strdup("DOWNLOADING LISTS...");
    if (ab->on_ready)
        ab->on_ready(ab, ab->on_ready_ud);

    g_autoptr(SoupMessage) msg = soup_message_new(
        "GET",
        "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=hosts&showintro=0&mimetype=plaintext");
    soup_session_send_and_read_async(ab->soup, msg, G_PRIORITY_DEFAULT,
                                     ab->cancellable, on_community_list_fetched, ab);
}

gboolean
web_adblock_is_updating(const KajoWebAdblock *ab)
{
    return ab ? ab->updating : FALSE;
}

KajoWebAdblock *
web_adblock_new(const gchar           *cache_dir,
                KajoWebAdblockReadyCb  on_ready,
                gpointer               user_data)
{
    KajoWebAdblock *ab = g_new0(KajoWebAdblock, 1);
    ab->cache_dir = g_strdup(cache_dir ? cache_dir : g_get_user_cache_dir());
    g_mkdir_with_parents(ab->cache_dir, 0700);
    ab->community_json_path = g_build_filename(ab->cache_dir, "adblock-community.json", NULL);

    g_autofree gchar *store_dir = g_build_filename(ab->cache_dir, "content-filters", NULL);
    g_mkdir_with_parents(store_dir, 0700);

    static const char *const k_yt_allow[] = {
        "https://*.youtube.com/*",
        "https://youtube.com/*",
        NULL
    };
    ab->yt_adskip_script = webkit_user_script_new(
        k_yt_adskip_js,
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        k_yt_allow,
        NULL);

    ab->store = webkit_user_content_filter_store_new(store_dir);
    ab->soup = soup_session_new();
    ab->managers = g_ptr_array_new();
    ab->user_scripts = g_ptr_array_new_with_free_func((GDestroyNotify)webkit_user_script_unref);
    ab->user_styles = g_ptr_array_new_with_free_func((GDestroyNotify)webkit_user_style_sheet_unref);
    ab->cancellable = g_cancellable_new();
    ab->enabled = TRUE;
    ab->status_text = g_strdup("LOADING RULES...");
    ab->on_ready = on_ready;
    ab->on_ready_ud = user_data;

    load_or_compile_rules_async(ab);
    return ab;
}

void
web_adblock_free(KajoWebAdblock *ab)
{
    if (!ab)
        return;
    if (ab->cancellable)
        g_cancellable_cancel(ab->cancellable);
    g_clear_object(&ab->cancellable);
    g_clear_object(&ab->soup);
    g_clear_pointer(&ab->yt_adskip_script, webkit_user_script_unref);
    g_clear_pointer(&ab->filter, webkit_user_content_filter_unref);
    g_clear_object(&ab->store);
    g_clear_pointer(&ab->managers, g_ptr_array_unref);
    g_clear_pointer(&ab->user_scripts, g_ptr_array_unref);
    g_clear_pointer(&ab->user_styles, g_ptr_array_unref);
    g_free(ab->custom_rules_path);
    g_free(ab->pending_filter_id);
    g_free(ab->community_json_path);
    g_free(ab->cache_dir);
    g_free(ab->status_text);
    g_free(ab);
}

void
web_adblock_apply_config(KajoWebAdblock *ab, const KajoWebCfg *cfg)
{
    if (!ab || !cfg)
        return;
    gboolean path_changed = g_strcmp0(ab->custom_rules_path ? ab->custom_rules_path : "",
                                      cfg->adblock_custom_rules_path ? cfg->adblock_custom_rules_path : "") != 0;
    ab->enabled = cfg->adblock_enabled;
    ab->user_scripts_enabled = cfg->user_scripts_enabled;
    if (path_changed) {
        g_free(ab->custom_rules_path);
        ab->custom_rules_path = g_strdup(cfg->adblock_custom_rules_path ? cfg->adblock_custom_rules_path : "");
        load_or_compile_rules_async(ab);
    }
    reload_user_scripts(ab);
    sync_all_managers(ab);
}

void
web_adblock_attach_manager(KajoWebAdblock *ab, WebKitUserContentManager *ucm)
{
    if (!ab || !ucm)
        return;
    for (guint i = 0; i < ab->managers->len; i++) {
        if (g_ptr_array_index(ab->managers, i) == ucm)
            return;
    }
    g_ptr_array_add(ab->managers, ucm);
    apply_to_manager(ab, ucm);
}

void
web_adblock_detach_manager(KajoWebAdblock *ab, WebKitUserContentManager *ucm)
{
    if (!ab || !ucm)
        return;
    g_ptr_array_remove_fast(ab->managers, ucm);
}

gboolean
web_adblock_is_enabled(const KajoWebAdblock *ab)
{
    return ab ? ab->enabled : FALSE;
}

gboolean
web_adblock_is_ready(const KajoWebAdblock *ab)
{
    return ab && ab->filter != NULL;
}

guint
web_adblock_get_rule_count(const KajoWebAdblock *ab)
{
    return ab ? ab->rule_count : 0;
}

guint
web_adblock_get_user_script_count(const KajoWebAdblock *ab)
{
    return ab ? (ab->user_scripts->len + ab->user_styles->len) : 0;
}

const gchar *
web_adblock_get_status_text(const KajoWebAdblock *ab)
{
    return (ab && ab->status_text) ? ab->status_text : "IDLE";
}
