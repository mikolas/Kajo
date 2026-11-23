#include "web_adblock.h"
#include "../config/kajo_config_schema.h"
#include <json-glib/json-glib.h>

struct _KajoWebAdblock {
    WebKitUserContentFilterStore *store;
    WebKitUserContentFilter      *filter;
    GPtrArray                    *managers;     /* borrowed WebKitUserContentManager* */
    GPtrArray                    *user_scripts; /* WebKitUserScript* */
    GPtrArray                    *user_styles;  /* WebKitUserStyleSheet* */
    GCancellable                 *cancellable;
    gboolean                      enabled;
    gboolean                      user_scripts_enabled;
    gchar                        *custom_rules_path;
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
    if (err || !compiled) {
        g_warning("kajo-web: content filter compile failed: %s", err ? err->message : "unknown");
        g_free(ab->status_text);
        ab->status_text = g_strdup("FILTER COMPILE ERROR");
        return;
    }

    g_clear_pointer(&ab->filter, webkit_user_content_filter_unref);
    ab->filter = compiled;
    g_free(ab->status_text);
    ab->status_text = g_strdup_printf("ACTIVE (%u RULES)", ab->rule_count);
    sync_all_managers(ab);
    if (ab->on_ready)
        ab->on_ready(ab, ab->on_ready_ud);
}

static void
compile_rules_async(KajoWebAdblock *ab)
{
    g_autofree gchar *rules_path = NULL;
    if (ab->custom_rules_path && *ab->custom_rules_path &&
        g_file_test(ab->custom_rules_path, G_FILE_TEST_EXISTS)) {
        rules_path = g_strdup(ab->custom_rules_path);
    } else {
        rules_path = kajo_config_find_default_file("web-adblock.json");
        if ((!rules_path || !g_file_test(rules_path, G_FILE_TEST_EXISTS))) {
            g_free(rules_path);
            rules_path = g_build_filename(KAJO_SOURCE_DIR, "data", "defaults", "web-adblock.json", NULL);
        }
    }

    g_autofree gchar *json_data = NULL;
    gsize len = 0;
    if (!rules_path || !g_file_get_contents(rules_path, &json_data, &len, NULL) || len == 0) {
        static const char k_fallback[] =
            "[{\"trigger\":{\"url-filter\":\"doubleclick\\\\.net\",\"load-type\":[\"third-party\"]},"
            "\"action\":{\"type\":\"block\"}}]";
        json_data = g_strdup(k_fallback);
        len = strlen(k_fallback);
    }

    ab->rule_count = count_json_array_rules(json_data, len);
    g_autoptr(GBytes) bytes = g_bytes_new(json_data, len);
    webkit_user_content_filter_store_save(ab->store, "kajo-adblock-v1",
                                          bytes, ab->cancellable,
                                          on_filter_compiled, ab);
}

KajoWebAdblock *
web_adblock_new(const gchar           *cache_dir,
                KajoWebAdblockReadyCb  on_ready,
                gpointer               user_data)
{
    KajoWebAdblock *ab = g_new0(KajoWebAdblock, 1);
    g_autofree gchar *store_dir = g_build_filename(
        cache_dir ? cache_dir : g_get_user_cache_dir(), "content-filters", NULL);
    g_mkdir_with_parents(store_dir, 0700);

    ab->store = webkit_user_content_filter_store_new(store_dir);
    ab->managers = g_ptr_array_new();
    ab->user_scripts = g_ptr_array_new_with_free_func((GDestroyNotify)webkit_user_script_unref);
    ab->user_styles = g_ptr_array_new_with_free_func((GDestroyNotify)webkit_user_style_sheet_unref);
    ab->cancellable = g_cancellable_new();
    ab->enabled = TRUE;
    ab->status_text = g_strdup("COMPILING...");
    ab->on_ready = on_ready;
    ab->on_ready_ud = user_data;

    compile_rules_async(ab);
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
    g_clear_pointer(&ab->filter, webkit_user_content_filter_unref);
    g_clear_object(&ab->store);
    g_clear_pointer(&ab->managers, g_ptr_array_unref);
    g_clear_pointer(&ab->user_scripts, g_ptr_array_unref);
    g_clear_pointer(&ab->user_styles, g_ptr_array_unref);
    g_free(ab->custom_rules_path);
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
        compile_rules_async(ab);
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
