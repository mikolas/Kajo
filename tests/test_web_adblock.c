#include "../src/web/web_abp_compiler.h"
#include <webkit/webkit.h>
#include <json-glib/json-glib.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    GMainLoop *loop;
    gboolean   ok;
    gchar     *err_msg;
} CompileWait;

static void
on_save_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    CompileWait *w = ud;
    g_autoptr(GError) err = NULL;
    WebKitUserContentFilter *flt =
        webkit_user_content_filter_store_save_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(src), res, &err);
    if (flt) {
        w->ok = TRUE;
        webkit_user_content_filter_unref(flt);
    } else {
        w->ok = FALSE;
        w->err_msg = g_strdup(err ? err->message : "unknown compile failure");
    }
    g_main_loop_quit(w->loop);
}

static void
assert_webkit_compiles(WebKitUserContentFilterStore *store,
                       const gchar                  *id,
                       const gchar                  *json_str)
{
    CompileWait w = { .loop = g_main_loop_new(NULL, FALSE) };
    g_autoptr(GBytes) bytes = g_bytes_new(json_str, strlen(json_str));
    webkit_user_content_filter_store_save(store, id, bytes, NULL, on_save_done, &w);
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    if (!w.ok)
        g_test_message("WebKit filter '%s' failed to compile: %s", id, w.err_msg ? w.err_msg : "");
    g_assert_true(w.ok);
    g_free(w.err_msg);
}

/* 1. Real WebKitGTK DFA compilation gate for data/defaults/web-adblock.json */
static void
test_shipped_adblock_json_compiles_in_webkit(void)
{
    const gchar *root = g_getenv("KAJO_SOURCE_ROOT");
    if (!root || !*root)
        root = KAJO_SOURCE_DIR;
    g_autofree gchar *path = g_build_filename(root, "data", "defaults", "web-adblock.json", NULL);
    g_autofree gchar *json = NULL;
    gsize len = 0;
    g_autoptr(GError) err = NULL;
    g_assert_true(g_file_get_contents(path, &json, &len, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(len, >, 2000);
    g_assert_null(strstr(json, "ptracking"));
    g_assert_null(strstr(json, "log_event"));
    g_assert_null(strstr(json, "api/stats/atr"));

    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, json, (gssize)len, &err));
    g_assert_no_error(err);
    JsonNode *jroot = json_parser_get_root(parser);
    g_assert_nonnull(jroot);
    g_assert_true(JSON_NODE_HOLDS_ARRAY(jroot));
    JsonArray *arr = json_node_get_array(jroot);
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        JsonObject *obj = json_array_get_object_element(arr, i);
        JsonObject *act = obj ? json_object_get_object_member(obj, "action") : NULL;
        JsonObject *trig = obj ? json_object_get_object_member(obj, "trigger") : NULL;
        const gchar *atype = act ? json_object_get_string_member_with_default(act, "type", "") : "";
        if (g_strcmp0(atype, "css-display-none") == 0) {
            g_assert_nonnull(trig);
            g_assert_true(json_object_has_member(trig, "if-domain"));
        }
    }

    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-wk-ab-XXXXXX", NULL);
    g_autoptr(WebKitUserContentFilterStore) store = webkit_user_content_filter_store_new(tmpdir);
    assert_webkit_compiles(store, "shipped-base", json);

    /* Clean temp store */
    g_autoptr(GDir) d = g_dir_open(tmpdir, 0, NULL);
    const gchar *nm;
    while (d && (nm = g_dir_read_name(d)) != NULL) {
        g_autofree gchar *fp = g_build_filename(tmpdir, nm, NULL);
        g_unlink(fp);
    }
    g_rmdir(tmpdir);
}

/* 2. Real WebKitGTK DFA compilation gate for ABP/EasyList/Finland compiler output */
static void
test_abp_compiler_output_compiles_in_webkit(void)
{
    static const char k_sample_abp[] =
        "[Adblock Plus 2.0]\n"
        "! EasyList + EasyList Finland + Hosts mix\n"
        "||doubleclick.net^\n"
        "||assets.adobedtm.com^$third-party,script\n"
        "||aas.snmdt.fi^$third-party,xmlhttprequest\n"
        "||adnxs.com^$domain=hs.fi|is.fi\n"
        "@@||doubleclick.net/allowed.js$domain=example.com\n"
        "0.0.0.0 tracker.example.org # comment\n"
        "127.0.0.1 ads.example.net\n"
        "##.ad-banner\n"
        "##[data-ad-position]\n"
        "hs.fi,is.fi##.teaser-ad__background\n"
        "hs.fi##[data-testid|=\"laneitem\"]:has(> .list a[href^=\"/mainos/\"])\n"
        "youtube.com##ytd-rich-item-renderer:has(> ytd-ad-slot-renderer)\n"
        "hs.fi##a.block[href*=\"/mainos\"]\n"
        "! YouTube player telemetry endpoints MUST be skipped to prevent Fetch error storms:\n"
        "||youtube.com/api/stats/qoe^\n"
        "||youtube.com/ptracking^\n"
        "! Unsupported procedural rules MUST be skipped cleanly:\n"
        "hs.fi#?#div:has-text(Mainos)\n"
        "hs.fi##+js(abort-on-property-read, foo)\n"
        "||bad.example^$redirect=noopjs\n"
        "/regex|with|disjunction/\n";

    guint count = 0;
    g_autofree gchar *json = web_abp_compile_text_to_json(k_sample_abp, 1000, &count);
    g_assert_nonnull(json);
    g_assert_cmpuint(count, >=, 8);
    g_assert_nonnull(strstr(json, "adobedtm"));
    g_assert_nonnull(strstr(json, "snmdt"));
    g_assert_nonnull(strstr(json, "ignore-previous-rules"));
    g_assert_nonnull(strstr(json, "css-display-none"));
    /* Global generic and :has() selectors + YouTube QoE telemetry MUST be stripped */
    g_assert_null(strstr(json, ".ad-banner"));
    g_assert_null(strstr(json, "data-ad-position"));
    g_assert_null(strstr(json, ":has("));
    g_assert_null(strstr(json, "stats/qoe"));
    g_assert_null(strstr(json, "ptracking"));
    g_assert_nonnull(strstr(json, "\"if-domain\""));
    g_assert_null(strstr(json, "has-text"));
    g_assert_null(strstr(json, "abort-on-property-read"));

    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-wk-abp-XXXXXX", NULL);
    g_autoptr(WebKitUserContentFilterStore) store = webkit_user_content_filter_store_new(tmpdir);
    assert_webkit_compiles(store, "abp-sample", json);

    g_autoptr(GDir) d = g_dir_open(tmpdir, 0, NULL);
    const gchar *nm;
    while (d && (nm = g_dir_read_name(d)) != NULL) {
        g_autofree gchar *fp = g_build_filename(tmpdir, nm, NULL);
        g_unlink(fp);
    }
    g_rmdir(tmpdir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web/adblock/shipped-json-compiles-in-webkit", test_shipped_adblock_json_compiles_in_webkit);
    g_test_add_func("/web/adblock/abp-compiler-compiles-in-webkit", test_abp_compiler_output_compiles_in_webkit);
    return g_test_run();
}
