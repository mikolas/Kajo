#include "web_adblock.h"
#include "web_abp_compiler.h"
#include "../config/kajo_config_schema.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define AB_COMPILER_VERSION 3

/* Lightweight YouTube in-stream ad skipper (active ONLY when adblock_enabled == TRUE,
 * scoped strictly to youtube.com via WebKitUserScript allow_list).
 * Never seeks v.currentTime = v.duration (which breaks YouTube MSE state);
 * accelerates ready ads via playbackRate=16 + mute and clicks skip buttons. */
static const char k_yt_adskip_js[] =
    "(function(){"
    "if(window.__kajoYtAdSkip)return;window.__kajoYtAdSkip=true;"
    "var skipTimer=0,obs=null,bootObs=null,bootStop=0,curPlayer=null,savedMute=null,inAd=false;"
    "function restorePlayer(){"
    "  var p=curPlayer&&curPlayer.isConnected?curPlayer:(document.getElementById('movie_player')||document.querySelector('.html5-video-player'));"
    "  var v=p?p.querySelector('video'):null;"
    "  if(v){"
    "    if(v.playbackRate!==1)v.playbackRate=1;"
    "    if(savedMute!==null){v.muted=savedMute;savedMute=null;}"
    "  }else{savedMute=null;}"
    "  inAd=false;"
    "  if(skipTimer){clearInterval(skipTimer);skipTimer=0;}"
    "}"
    "function trySkip(){"
    "  var p=curPlayer&&curPlayer.isConnected?curPlayer:(document.getElementById('movie_player')||document.querySelector('.html5-video-player'));"
    "  if(!p){restorePlayer();return;}"
    "  var ad=p.classList.contains('ad-showing')||p.classList.contains('ad-interrupting');"
    "  if(!ad){if(inAd||skipTimer)restorePlayer();return;}"
    "  var v=p.querySelector('video');"
    "  if(!inAd){inAd=true;if(v&&savedMute===null)savedMute=!!v.muted;}"
    "  if(v){"
    "    v.muted=true;"
    "    if(v.readyState>=2&&isFinite(v.duration)&&v.duration>0&&v.duration<=180){"
    "      try{v.playbackRate=16;}catch(e){}"
    "    }"
    "  }"
    "  var b=p.querySelector('.ytp-ad-skip-button,.ytp-ad-skip-button-modern,.ytp-skip-ad-button');"
    "  if(b)b.click();"
    "}"
    "function stopBoot(){"
    "  if(bootObs){bootObs.disconnect();bootObs=null;}"
    "  if(bootStop){clearTimeout(bootStop);bootStop=0;}"
    "}"
    "function onClass(){"
    "  if(!curPlayer)return;"
    "  var ad=curPlayer.classList.contains('ad-showing')||curPlayer.classList.contains('ad-interrupting');"
    "  if(ad){trySkip();if(!skipTimer)skipTimer=setInterval(trySkip,250);}"
    "  else if(inAd||skipTimer){restorePlayer();}"
    "}"
    "function attach(){"
    "  var p=document.getElementById('movie_player')||document.querySelector('.html5-video-player');"
    "  if(!p)return false;"
    "  if(p!==curPlayer){"
    "    if(obs)obs.disconnect();"
    "    if(inAd||skipTimer)restorePlayer();"
    "    curPlayer=p;"
    "    obs=new MutationObserver(onClass);"
    "    obs.observe(p,{attributes:true,attributeFilter:['class'],subtree:false});"
    "    onClass();"
    "  }"
    "  stopBoot();"
    "  return true;"
    "}"
    "function boot(){"
    "  if(attach())return;"
    "  if(location.pathname.indexOf('/watch')===0||location.pathname.indexOf('/shorts')===0){"
    "    if(document.body&&!bootObs){"
    "      bootObs=new MutationObserver(function(){attach();});"
    "      bootObs.observe(document.body,{childList:true,subtree:true});"
    "      bootStop=setTimeout(stopBoot,3000);"
    "    }"
    "  }"
    "}"
    "document.addEventListener('yt-navigate-finish',function(){setTimeout(boot,0);});"
    "document.addEventListener('yt-page-data-updated',function(){attach();});"
    "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',boot);else boot();"
    "})();";

typedef enum {
    AB_SLOT_BASE = 0,
    AB_SLOT_EASYLIST,
    AB_SLOT_EASYPRIVACY,
    AB_SLOT_REGIONAL_FI,
    AB_SLOT_NORDIC,
    AB_SLOT_COOKIES,
    AB_SLOT_COUNT
} AdblockSlotId;

static const struct {
    const char *slug;
    const char *url;
    guint       max_rules;
} k_slot_defs[AB_SLOT_COUNT] = {
    [AB_SLOT_BASE]        = { "base",        NULL,                                                                                                                               5000  },
    [AB_SLOT_EASYLIST]    = { "easylist",    "https://easylist.to/easylist/easylist.txt",                                                                                        28000 },
    [AB_SLOT_EASYPRIVACY] = { "easyprivacy", "https://easylist.to/easylist/easyprivacy.txt",                                                                                     22000 },
    [AB_SLOT_REGIONAL_FI] = { "regional-fi", "https://raw.githubusercontent.com/finnish-easylist-addition/finnish-easylist-addition/gh-pages/Finland_adb.txt",                   4000  },
    [AB_SLOT_NORDIC]      = { "nordic",      "https://raw.githubusercontent.com/DandelionSprout/adfilt/master/NorwegianExperimentalList%20alternate%20versions/NordicFiltersABP-Inclusion.txt", 8000 },
    [AB_SLOT_COOKIES]     = { "cookies",     "https://secure.fanboy.co.nz/fanboy-cookiemonster.txt",                                                                             15000 },
};

typedef struct {
    AdblockSlotId            id;
    WebKitUserContentFilter *filter;
    gchar                   *active_filter_id;
    guint                    compiled_rules;
    gboolean                 failed;
    gchar                   *json_cache_path;
    /* Manifest metadata & validation */
    gchar                   *manifest_fid;
    guint                    manifest_rules;
    gchar                   *manifest_custom_path;
    gint64                   manifest_mtime;
} FilterSlot;

struct _KajoWebAdblock {
    gchar                        *cache_dir;
    gchar                        *lists_dir;
    gchar                        *store_dir;
    gchar                        *manifest_path;
    gchar                        *allowlist_path;
    WebKitUserContentFilterStore *store;
    FilterSlot                    slots[AB_SLOT_COUNT];
    WebKitUserContentManager     *shared_ucm;
    WebKitUserContentManager     *paused_ucm;
    WebKitUserScript             *yt_adskip_script;
    SoupSession                  *soup;
    GHashTable                   *paused_hosts;
    GPtrArray                    *user_scripts;
    GPtrArray                    *user_styles;
    GCancellable                 *cancellable;
    gboolean                      enabled;
    gboolean                      user_scripts_enabled;
    gboolean                      slots_bootstrapped;
    gboolean                      initial_barrier_done;
    gint                          manifest_version;
    guint                         pending_initial_loads;
    guint                         pending_downloads;
    gchar                        *custom_rules_path;
    gchar                        *status_text;
    KajoWebAdblockReadyCb         on_ready;
    gpointer                      on_ready_ud;
};

static void fetch_slot_async(KajoWebAdblock *ab, AdblockSlotId id);
static void start_cached_json_task(KajoWebAdblock *ab, AdblockSlotId id, gboolean strip_global_css, gboolean is_initial);

static gchar *
expand_tilde_path(const gchar *path)
{
    if (!path || !*path)
        return g_strdup("");
    if (g_strcmp0(path, "~") == 0)
        return g_strdup(g_get_home_dir());
    if (g_str_has_prefix(path, "~/"))
        return g_build_filename(g_get_home_dir(), path + 2, NULL);
    return g_strdup(path);
}

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

static gchar *
normalize_host(const gchar *host)
{
    if (!host || !*host)
        return NULL;
    gchar *low = g_ascii_strdown(host, -1);
    g_strstrip(low);
    const gchar *src = g_str_has_prefix(low, "www.") ? low + 4 : low;
    if (!*src || !strchr(src, '.')) {
        g_free(low);
        return NULL;
    }
    gchar *res = g_strdup(src);
    g_free(low);
    return res;
}

static void
load_allowlist(KajoWebAdblock *ab)
{
    if (!ab->allowlist_path)
        return;
    g_autofree gchar *txt = NULL;
    if (!g_file_get_contents(ab->allowlist_path, &txt, NULL, NULL) || !txt)
        return;
    g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
    for (gint i = 0; lines && lines[i]; i++) {
        g_autofree gchar *h = normalize_host(lines[i]);
        if (h)
            g_hash_table_insert(ab->paused_hosts, g_strdup(h), GINT_TO_POINTER(1));
    }
}

static void
save_allowlist(KajoWebAdblock *ab)
{
    if (!ab->allowlist_path)
        return;
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer k = NULL;
    g_hash_table_iter_init(&iter, ab->paused_hosts);
    while (g_hash_table_iter_next(&iter, &k, NULL)) {
        g_string_append(out, (const gchar *)k);
        g_string_append_c(out, '\n');
    }
    g_file_set_contents(ab->allowlist_path, out->str, (gssize)out->len, NULL);
    g_string_free(out, TRUE);
}

static void
load_manifest(KajoWebAdblock *ab)
{
    ab->manifest_version = 0;
    if (!ab->manifest_path || !g_file_test(ab->manifest_path, G_FILE_TEST_EXISTS))
        return;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, ab->manifest_path, NULL))
        return;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return;
    JsonObject *obj = json_node_get_object(root);
    ab->manifest_version = (gint)json_object_get_int_member_with_default(obj, "compiler_version", 0);
    if (!json_object_has_member(obj, "slots"))
        return;
    JsonObject *sobj = json_object_get_object_member(obj, "slots");
    if (!sobj)
        return;
    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        if (!json_object_has_member(sobj, k_slot_defs[i].slug))
            continue;
        JsonObject *ent = json_object_get_object_member(sobj, k_slot_defs[i].slug);
        if (!ent)
            continue;
        g_free(ab->slots[i].manifest_fid);
        ab->slots[i].manifest_fid = g_strdup(json_object_get_string_member_with_default(ent, "filter_id", ""));
        ab->slots[i].manifest_rules = (guint)MAX(0, json_object_get_int_member_with_default(ent, "rules", 0));
        g_free(ab->slots[i].manifest_custom_path);
        ab->slots[i].manifest_custom_path = g_strdup(json_object_get_string_member_with_default(ent, "custom_path", ""));
        ab->slots[i].manifest_mtime = json_object_get_int_member_with_default(ent, "src_mtime", 0);
    }
}

static void
save_manifest(KajoWebAdblock *ab)
{
    if (!ab || !ab->manifest_path)
        return;
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "compiler_version");
    json_builder_add_int_value(b, AB_COMPILER_VERSION);
    json_builder_set_member_name(b, "slots");
    json_builder_begin_object(b);

    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        const FilterSlot *s = &ab->slots[i];
        const gchar *fid = s->active_filter_id ? s->active_filter_id : s->manifest_fid;
        guint rules = s->filter ? s->compiled_rules : s->manifest_rules;
        if (!fid || !*fid || rules == 0)
            continue;
        json_builder_set_member_name(b, k_slot_defs[i].slug);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "filter_id");
        json_builder_add_string_value(b, fid);
        json_builder_set_member_name(b, "rules");
        json_builder_add_int_value(b, (gint64)rules);
        if (i == AB_SLOT_BASE) {
            json_builder_set_member_name(b, "custom_path");
            json_builder_add_string_value(b, s->manifest_custom_path ? s->manifest_custom_path : "");
            json_builder_set_member_name(b, "src_mtime");
            json_builder_add_int_value(b, s->manifest_mtime);
        }
        json_builder_end_object(b);
    }

    json_builder_end_object(b);
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    g_autofree gchar *data = json_generator_to_data(gen, NULL);
    if (data)
        g_file_set_contents(ab->manifest_path, data, -1, NULL);
    ab->manifest_version = AB_COMPILER_VERSION;
}

static void
refresh_status_text(KajoWebAdblock *ab)
{
    guint total_rules = 0;
    guint loaded_lists = 0;
    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        if (ab->slots[i].filter) {
            total_rules += ab->slots[i].compiled_rules;
            loaded_lists++;
        }
    }

    g_free(ab->status_text);
    if (!ab->enabled) {
        ab->status_text = g_strdup("DISABLED");
    } else if (ab->pending_downloads > 0) {
        if (loaded_lists > 0)
            ab->status_text = g_strdup_printf("ACTIVE (%u RULES • UPDATING...)", total_rules);
        else
            ab->status_text = g_strdup("DOWNLOADING LISTS...");
    } else if (loaded_lists > 0) {
        ab->status_text = g_strdup_printf("ACTIVE (%u RULES • %u %s)",
                                          total_rules, loaded_lists,
                                          loaded_lists == 1 ? "LIST" : "LISTS");
    } else if (ab->slots[AB_SLOT_BASE].failed) {
        ab->status_text = g_strdup("FILTER COMPILE ERROR");
    } else {
        ab->status_text = g_strdup("LOADING RULES...");
    }
}

static void
sync_shared_ucm(KajoWebAdblock *ab)
{
    if (!ab || !ab->shared_ucm)
        return;
    webkit_user_content_manager_remove_all_filters(ab->shared_ucm);
    if (ab->enabled) {
        for (guint i = 0; i < AB_SLOT_COUNT; i++) {
            if (ab->slots[i].filter)
                webkit_user_content_manager_add_filter(ab->shared_ucm, ab->slots[i].filter);
        }
    }

    webkit_user_content_manager_remove_all_scripts(ab->shared_ucm);
    webkit_user_content_manager_remove_all_style_sheets(ab->shared_ucm);
    if (ab->enabled && ab->yt_adskip_script)
        webkit_user_content_manager_add_script(ab->shared_ucm, ab->yt_adskip_script);

    if (ab->user_scripts_enabled) {
        for (guint i = 0; i < ab->user_scripts->len; i++)
            webkit_user_content_manager_add_script(ab->shared_ucm, g_ptr_array_index(ab->user_scripts, i));
        for (guint i = 0; i < ab->user_styles->len; i++)
            webkit_user_content_manager_add_style_sheet(ab->shared_ucm, g_ptr_array_index(ab->user_styles, i));
    }
}

static void
notify_ready(KajoWebAdblock *ab)
{
    refresh_status_text(ab);
    if (ab->on_ready)
        ab->on_ready(ab, ab->on_ready_ud);
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
cleanup_orphan_temp_filters(const gchar *store_dir)
{
    g_autoptr(GDir) dir = g_dir_open(store_dir, 0, NULL);
    if (!dir)
        return;
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(name, "ContentRuleList-") &&
            !g_str_has_prefix(name, "ContentRuleList-kajo-ab-")) {
            g_autofree gchar *full = g_build_filename(store_dir, name, NULL);
            g_unlink(full);
        }
    }
}

static void
on_old_filter_removed(GObject *src, GAsyncResult *res, gpointer ud G_GNUC_UNUSED)
{
    webkit_user_content_filter_store_remove_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(src), res, NULL);
}

static void
evict_superseded_slot_filters(KajoWebAdblock *ab, AdblockSlotId id, const gchar *keep_fid)
{
    if (!ab || !ab->store_dir || !keep_fid)
        return;
    g_autofree gchar *prefix = g_strdup_printf("ContentRuleList-kajo-ab-%s-", k_slot_defs[id].slug);
    g_autofree gchar *keep_name = g_strdup_printf("ContentRuleList-%s", keep_fid);

    g_autoptr(GDir) dir = g_dir_open(ab->store_dir, 0, NULL);
    if (!dir)
        return;
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(name, prefix) && g_strcmp0(name, keep_name) != 0) {
            const gchar *old_id = name + strlen("ContentRuleList-");
            webkit_user_content_filter_store_remove(ab->store, old_id, ab->cancellable,
                                                    on_old_filter_removed, NULL);
        }
    }
}

static gchar *
load_base_rules_json(KajoWebAdblock *ab, gsize *out_len)
{
    gchar *json_data = NULL;
    gsize len = 0;

    if (ab->custom_rules_path && *ab->custom_rules_path &&
        g_file_test(ab->custom_rules_path, G_FILE_TEST_EXISTS)) {
        if (g_file_get_contents(ab->custom_rules_path, &json_data, &len, NULL) && len > 0) {
            if (out_len) *out_len = len;
            return json_data;
        }
        g_clear_pointer(&json_data, g_free);
    }

    g_autoptr(GBytes) res = g_resources_lookup_data("/org/kajo/shell/defaults/web-adblock.json",
                                                    G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (res) {
        gsize rlen = 0;
        const gchar *rdata = g_bytes_get_data(res, &rlen);
        if (rdata && rlen > 0) {
            json_data = g_strndup(rdata, rlen);
            if (out_len) *out_len = rlen;
            return json_data;
        }
    }

    g_autofree gchar *rules_path = g_build_filename(KAJO_SOURCE_DIR, "data", "defaults", "web-adblock.json", NULL);
    if (!g_file_test(rules_path, G_FILE_TEST_EXISTS)) {
        g_free(rules_path);
        rules_path = kajo_config_find_default_file("web-adblock.json");
    }
    if (rules_path && g_file_get_contents(rules_path, &json_data, &len, NULL) && len > 0) {
        if (out_len) *out_len = len;
        return json_data;
    }
    g_free(json_data);

    static const char k_fallback[] =
        "[{\"trigger\":{\"url-filter\":\"^https?://([^/]+\\\\.)?doubleclick\\\\.net/\"},"
        "\"action\":{\"type\":\"block\"}}]";
    if (out_len) *out_len = strlen(k_fallback);
    return g_strdup(k_fallback);
}

static gchar *
compute_slot_fid(AdblockSlotId id, const gchar *data, gsize len)
{
    g_autofree gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                        (const guchar *)data, len);
    return g_strdup_printf("kajo-ab-%s-%.12s", k_slot_defs[id].slug, sha ? sha : "v2");
}

typedef struct {
    KajoWebAdblock *ab;
    GCancellable   *alive;
    AdblockSlotId   slot_id;
    GBytes         *json_bytes;
    gchar          *filter_id;
    guint           expected_rules;
    gboolean        is_initial;
} SlotCompileCtx;

static void
slot_compile_ctx_free(SlotCompileCtx *ctx)
{
    if (!ctx)
        return;
    g_clear_object(&ctx->alive);
    g_clear_pointer(&ctx->json_bytes, g_bytes_unref);
    g_free(ctx->filter_id);
    g_free(ctx);
}

static void
finish_slot_ready(KajoWebAdblock *ab, AdblockSlotId id,
                  WebKitUserContentFilter *compiled,
                  const gchar *fid, guint rule_count, gboolean is_initial)
{
    FilterSlot *slot = &ab->slots[id];
    g_clear_pointer(&slot->filter, webkit_user_content_filter_unref);
    slot->filter = compiled;
    slot->compiled_rules = compiled ? rule_count : 0;
    slot->failed = (compiled == NULL);
    if (compiled && fid) {
        g_free(slot->active_filter_id);
        slot->active_filter_id = g_strdup(fid);
        g_free(slot->manifest_fid);
        slot->manifest_fid = g_strdup(fid);
        slot->manifest_rules = rule_count;
        evict_superseded_slot_filters(ab, id, fid);
    } else {
        g_clear_pointer(&slot->active_filter_id, g_free);
        g_clear_pointer(&slot->manifest_fid, g_free);
        slot->manifest_rules = 0;
    }

    if (is_initial && !ab->initial_barrier_done) {
        if (ab->pending_initial_loads > 0)
            ab->pending_initial_loads--;
        if (ab->pending_initial_loads > 0) {
            refresh_status_text(ab);
            return;
        }
        ab->initial_barrier_done = TRUE;
        save_manifest(ab);
        sync_shared_ucm(ab);
        notify_ready(ab);
        return;
    }

    if (ab->initial_barrier_done)
        save_manifest(ab);
    sync_shared_ucm(ab);
    notify_ready(ab);
}

static void
on_slot_saved(GObject *source, GAsyncResult *res, gpointer user_data)
{
    SlotCompileCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    WebKitUserContentFilter *compiled =
        webkit_user_content_filter_store_save_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err);
    if (g_cancellable_is_cancelled(ctx->alive) ||
        g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (compiled)
            webkit_user_content_filter_unref(compiled);
        slot_compile_ctx_free(ctx);
        return;
    }

    KajoWebAdblock *ab = ctx->ab;
    AdblockSlotId id = ctx->slot_id;
    guint rules = ctx->expected_rules;
    gboolean is_init = ctx->is_initial;
    g_autofree gchar *fid = g_strdup(ctx->filter_id);
    slot_compile_ctx_free(ctx);

    if (err || !compiled) {
        g_warning("kajo-web: adblock slot '%s' compile failed: %s",
                  k_slot_defs[id].slug, err ? err->message : "unknown");
        finish_slot_ready(ab, id, NULL, NULL, 0, is_init);
        return;
    }
    finish_slot_ready(ab, id, compiled, fid, rules, is_init);
}

static void load_base_slot_async(KajoWebAdblock *ab, gboolean is_initial);

static void
on_slot_loaded(GObject *source, GAsyncResult *res, gpointer user_data)
{
    SlotCompileCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    WebKitUserContentFilter *loaded =
        webkit_user_content_filter_store_load_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err);
    if (g_cancellable_is_cancelled(ctx->alive) ||
        g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (loaded)
            webkit_user_content_filter_unref(loaded);
        slot_compile_ctx_free(ctx);
        return;
    }

    KajoWebAdblock *ab = ctx->ab;
    if (loaded) {
        AdblockSlotId id = ctx->slot_id;
        guint rules = ctx->expected_rules;
        gboolean is_init = ctx->is_initial;
        g_autofree gchar *fid = g_strdup(ctx->filter_id);
        slot_compile_ctx_free(ctx);
        finish_slot_ready(ab, id, loaded, fid, rules, is_init);
        return;
    }

    if (ctx->json_bytes) {
        webkit_user_content_filter_store_save(ab->store, ctx->filter_id,
                                              ctx->json_bytes, ab->cancellable,
                                              on_slot_saved, ctx);
        return;
    }

    /* Manifest pointed to missing DFA binary: fall back cleanly */
    AdblockSlotId id = ctx->slot_id;
    gboolean is_init = ctx->is_initial;
    slot_compile_ctx_free(ctx);

    if (id == AB_SLOT_BASE) {
        load_base_slot_async(ab, is_init);
    } else if (ab->slots[id].json_cache_path &&
               g_file_test(ab->slots[id].json_cache_path, G_FILE_TEST_EXISTS)) {
        start_cached_json_task(ab, id, TRUE, is_init);
    } else {
        if (ab->enabled)
            fetch_slot_async(ab, id);
        finish_slot_ready(ab, id, NULL, NULL, 0, is_init);
    }
}

static void
compile_slot_bytes_async(KajoWebAdblock *ab, AdblockSlotId id,
                         GBytes *json_bytes, const gchar *fid,
                         guint nrules, gboolean is_initial)
{
    if (!ab || !json_bytes || !fid || nrules == 0)
        return;

    SlotCompileCtx *ctx = g_new0(SlotCompileCtx, 1);
    ctx->ab = ab;
    ctx->alive = g_object_ref(ab->cancellable);
    ctx->slot_id = id;
    ctx->json_bytes = g_bytes_ref(json_bytes);
    ctx->filter_id = g_strdup(fid);
    ctx->expected_rules = nrules;
    ctx->is_initial = is_initial;

    webkit_user_content_filter_store_load(ab->store, fid, ab->cancellable,
                                          on_slot_loaded, ctx);
}

static void
compile_slot_json_async(KajoWebAdblock *ab, AdblockSlotId id,
                        const gchar *json_data, gsize len, gboolean is_initial)
{
    if (!ab || !json_data || len == 0) {
        if (is_initial)
            finish_slot_ready(ab, id, NULL, NULL, 0, TRUE);
        return;
    }
    guint nrules = count_json_array_rules(json_data, len);
    if (nrules == 0) {
        if (is_initial)
            finish_slot_ready(ab, id, NULL, NULL, 0, TRUE);
        return;
    }

    g_autofree gchar *fid = compute_slot_fid(id, json_data, len);
    g_autoptr(GBytes) bytes = g_bytes_new(json_data, len);
    compile_slot_bytes_async(ab, id, bytes, fid, nrules, is_initial);
}

static void
load_base_slot_async(KajoWebAdblock *ab, gboolean is_initial)
{
    gsize len = 0;
    g_autofree gchar *json_data = load_base_rules_json(ab, &len);
    g_free(ab->slots[AB_SLOT_BASE].manifest_custom_path);
    ab->slots[AB_SLOT_BASE].manifest_custom_path = g_strdup(ab->custom_rules_path ? ab->custom_rules_path : "");
    ab->slots[AB_SLOT_BASE].manifest_mtime = 0;
    if (ab->custom_rules_path && *ab->custom_rules_path &&
        g_file_test(ab->custom_rules_path, G_FILE_TEST_EXISTS)) {
        GStatBuf st;
        if (g_stat(ab->custom_rules_path, &st) == 0)
            ab->slots[AB_SLOT_BASE].manifest_mtime = (gint64)st.st_mtime;
    }
    compile_slot_json_async(ab, AB_SLOT_BASE, json_data, len, is_initial);
}

/* ─── Worker-Thread JSON Migration / ABP Compilation (GTask) ─── */

typedef struct {
    KajoWebAdblock *ab;
    GCancellable   *alive;
    AdblockSlotId   slot_id;
    gchar          *json_cache_path;
    GBytes         *raw_body;
    guint           max_rules;
    gboolean        strip_global_css;
    gboolean        is_initial;
    /* Outputs filled on worker thread */
    GBytes         *out_json_bytes;
    gchar          *out_filter_id;
    guint           out_rules;
} BgSlotTask;

static void
bg_slot_task_free(gpointer ptr)
{
    BgSlotTask *t = ptr;
    if (!t)
        return;
    g_clear_object(&t->alive);
    g_free(t->json_cache_path);
    g_clear_pointer(&t->raw_body, g_bytes_unref);
    g_clear_pointer(&t->out_json_bytes, g_bytes_unref);
    g_free(t->out_filter_id);
    g_free(t);
}

static gboolean
strip_has_clauses_from_selector(const gchar *sel, gchar **out_clean_sel)
{
    if (!sel || !*sel || !strstr(sel, ":has("))
        return FALSE;

    GString *rebuilt = g_string_new(NULL);
    const gchar *seg = sel;
    gint paren = 0, brack = 0;
    gchar quote = 0;
    gboolean dropped_any = FALSE;

    for (const gchar *p = sel; ; p++) {
        char c = *p;
        gboolean at_end = (c == '\0');
        gboolean at_comma = (!at_end && quote == 0 && paren == 0 && brack == 0 && c == ',');

        if (at_end || at_comma) {
            g_autofree gchar *clause = g_strstrip(g_strndup(seg, (gsize)(p - seg)));
            if (*clause) {
                if (strstr(clause, ":has(")) {
                    dropped_any = TRUE;
                } else {
                    if (rebuilt->len > 0)
                        g_string_append(rebuilt, ", ");
                    g_string_append(rebuilt, clause);
                }
            }
            if (at_end)
                break;
            seg = p + 1;
            continue;
        }

        if (quote) {
            if (c == '\\' && p[1]) { p++; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') quote = c;
        else if (c == '(') paren++;
        else if (c == ')' && paren > 0) paren--;
        else if (c == '[') brack++;
        else if (c == ']' && brack > 0) brack--;
    }

    if (!dropped_any) {
        g_string_free(rebuilt, TRUE);
        return FALSE;
    }

    *out_clean_sel = (rebuilt->len > 0) ? g_string_free(rebuilt, FALSE) : NULL;
    if (!*out_clean_sel)
        g_string_free(rebuilt, TRUE);
    return TRUE;
}

/* Returns: 0 = keep as-is, 1 = drop entire rule, 2 = replace with *out_modified. */
static gint
sanitize_v3_rule(JsonNode *elem, JsonNode **out_modified)
{
    if (out_modified)
        *out_modified = NULL;
    if (!elem || !JSON_NODE_HOLDS_OBJECT(elem))
        return 0;

    JsonObject *obj = json_node_get_object(elem);
    JsonObject *act = json_object_has_member(obj, "action")
        ? json_object_get_object_member(obj, "action") : NULL;
    JsonObject *trig = json_object_has_member(obj, "trigger")
        ? json_object_get_object_member(obj, "trigger") : NULL;
    if (!act)
        return 0;

    const gchar *atype = json_object_get_string_member_with_default(act, "type", "");
    if (g_strcmp0(atype, "css-display-none") == 0) {
        if (!trig || !json_object_has_member(trig, "if-domain"))
            return 1;
        const gchar *sel = json_object_get_string_member_with_default(act, "selector", "");
        g_autofree gchar *clean_sel = NULL;
        if (strip_has_clauses_from_selector(sel, &clean_sel)) {
            if (!clean_sel || !*clean_sel)
                return 1;
            JsonNode *copy = json_node_copy(elem);
            JsonObject *cobj = json_node_get_object(copy);
            JsonObject *cact = json_object_get_object_member(cobj, "action");
            json_object_set_string_member(cact, "selector", clean_sel);
            if (out_modified)
                *out_modified = copy;
            else
                json_node_unref(copy);
            return 2;
        }
        return 0;
    }

    if (g_strcmp0(atype, "block") == 0 && trig) {
        const gchar *uf = json_object_get_string_member_with_default(trig, "url-filter", "");
        if (strstr(uf, "api/stats/qoe") || strstr(uf, "api/stats/atr") ||
            strstr(uf, "ptracking") || strstr(uf, "youtubei/v1/log_event")) {
            return 1;
        }
    }
    return 0;
}

static void
cached_json_worker_thread(GTask        *task,
                          gpointer      source_object G_GNUC_UNUSED,
                          gpointer      task_data,
                          GCancellable *cancellable G_GNUC_UNUSED)
{
    BgSlotTask *td = task_data;
    g_autofree gchar *jdata = NULL;
    gsize jlen = 0;
    if (!td->json_cache_path || !g_file_get_contents(td->json_cache_path, &jdata, &jlen, NULL) || jlen == 0) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    if (td->strip_global_css) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (!json_parser_load_from_data(parser, jdata, (gssize)jlen, NULL)) {
            g_task_return_boolean(task, FALSE);
            return;
        }
        JsonNode *root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
            g_task_return_boolean(task, FALSE);
            return;
        }
        JsonArray *arr = json_node_get_array(root);
        guint n = json_array_get_length(arr);
        gboolean removed_any = FALSE;

        g_autoptr(JsonBuilder) b = json_builder_new();
        json_builder_begin_array(b);
        guint kept = 0;
        for (guint i = 0; i < n; i++) {
            JsonNode *elem = json_array_get_element(arr, i);
            JsonNode *mod = NULL;
            gint verdict = sanitize_v3_rule(elem, &mod);
            if (verdict == 1) {
                removed_any = TRUE;
                continue;
            }
            if (verdict == 2 && mod) {
                removed_any = TRUE;
                json_builder_add_value(b, mod);
            } else {
                json_builder_add_value(b, json_node_copy(elem));
            }
            kept++;
        }
        json_builder_end_array(b);

        if (kept == 0) {
            g_task_return_boolean(task, FALSE);
            return;
        }
        if (removed_any) {
            g_autoptr(JsonGenerator) gen = json_generator_new();
            g_autoptr(JsonNode) nroot = json_builder_get_root(b);
            json_generator_set_root(gen, nroot);
            gsize out_len = 0;
            gchar *out_str = json_generator_to_data(gen, &out_len);
            if (out_str && out_len > 0) {
                g_file_set_contents(td->json_cache_path, out_str, (gssize)out_len, NULL);
                td->out_filter_id = compute_slot_fid(td->slot_id, out_str, out_len);
                td->out_json_bytes = g_bytes_new_take(out_str, out_len);
                td->out_rules = kept;
                g_task_return_boolean(task, TRUE);
                return;
            }
            g_free(out_str);
        }
        td->out_rules = kept;
    } else {
        td->out_rules = count_json_array_rules(jdata, jlen);
    }

    if (td->out_rules == 0) {
        g_task_return_boolean(task, FALSE);
        return;
    }
    td->out_filter_id = compute_slot_fid(td->slot_id, jdata, jlen);
    td->out_json_bytes = g_bytes_new_take(g_steal_pointer(&jdata), jlen);
    g_task_return_boolean(task, TRUE);
}

static void
on_cached_json_task_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data G_GNUC_UNUSED)
{
    GTask *task = G_TASK(res);
    BgSlotTask *td = g_task_get_task_data(task);
    if (!td || g_cancellable_is_cancelled(td->alive))
        return;

    gboolean ok = g_task_propagate_boolean(task, NULL);
    if (ok && td->out_json_bytes && td->out_filter_id && td->out_rules > 0) {
        compile_slot_bytes_async(td->ab, td->slot_id,
                                 td->out_json_bytes, td->out_filter_id,
                                 td->out_rules, td->is_initial);
        return;
    }

    if (td->ab->enabled)
        fetch_slot_async(td->ab, td->slot_id);
    if (td->is_initial)
        finish_slot_ready(td->ab, td->slot_id, NULL, NULL, 0, TRUE);
}

static void
start_cached_json_task(KajoWebAdblock *ab, AdblockSlotId id, gboolean strip_global_css, gboolean is_initial)
{
    BgSlotTask *td = g_new0(BgSlotTask, 1);
    td->ab = ab;
    td->alive = g_object_ref(ab->cancellable);
    td->slot_id = id;
    td->json_cache_path = g_strdup(ab->slots[id].json_cache_path);
    td->strip_global_css = strip_global_css;
    td->is_initial = is_initial;

    GTask *task = g_task_new(NULL, ab->cancellable, on_cached_json_task_done, NULL);
    g_task_set_task_data(task, td, bg_slot_task_free);
    g_task_run_in_thread(task, cached_json_worker_thread);
    g_object_unref(task);
}

/* ─── Online Multi-List Subscription Updater (GTask-offloaded compile) ─── */

static void
remote_compile_worker_thread(GTask        *task,
                             gpointer      source_object G_GNUC_UNUSED,
                             gpointer      task_data,
                             GCancellable *cancellable G_GNUC_UNUSED)
{
    BgSlotTask *td = task_data;
    gsize rlen = 0;
    const gchar *rdata = td->raw_body ? g_bytes_get_data(td->raw_body, &rlen) : NULL;
    g_autofree gchar *text = g_strndup(rdata ? rdata : "", rlen);

    guint count = 0;
    gchar *compiled_json = web_abp_compile_text_to_json(text, td->max_rules, &count);
    if (count > 0 && compiled_json && td->json_cache_path) {
        gsize clen = strlen(compiled_json);
        g_file_set_contents(td->json_cache_path, compiled_json, (gssize)clen, NULL);
        td->out_filter_id = compute_slot_fid(td->slot_id, compiled_json, clen);
        td->out_json_bytes = g_bytes_new_take(compiled_json, clen);
        td->out_rules = count;
        g_task_return_boolean(task, TRUE);
        return;
    }
    g_free(compiled_json);
    g_task_return_boolean(task, FALSE);
}

static void
on_remote_compile_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data G_GNUC_UNUSED)
{
    GTask *task = G_TASK(res);
    BgSlotTask *td = g_task_get_task_data(task);
    if (!td || g_cancellable_is_cancelled(td->alive))
        return;

    KajoWebAdblock *ab = td->ab;
    if (ab->pending_downloads > 0)
        ab->pending_downloads--;

    gboolean ok = g_task_propagate_boolean(task, NULL);
    if (ok && td->out_json_bytes && td->out_filter_id && td->out_rules > 0) {
        compile_slot_bytes_async(ab, td->slot_id,
                                 td->out_json_bytes, td->out_filter_id,
                                 td->out_rules, FALSE);
    } else {
        notify_ready(ab);
    }
}

typedef struct {
    KajoWebAdblock *ab;
    GCancellable   *alive;
    AdblockSlotId   slot_id;
} ListFetchCtx;

static void
list_fetch_ctx_free(ListFetchCtx *ctx)
{
    if (!ctx)
        return;
    g_clear_object(&ctx->alive);
    g_free(ctx);
}

static void
on_remote_list_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    ListFetchCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (g_cancellable_is_cancelled(ctx->alive) ||
        g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        list_fetch_ctx_free(ctx);
        return;
    }

    KajoWebAdblock *ab = ctx->ab;
    AdblockSlotId id = ctx->slot_id;
    list_fetch_ctx_free(ctx);

    if (err || !body) {
        if (ab->pending_downloads > 0)
            ab->pending_downloads--;
        g_warning("kajo-web: failed to fetch filter list '%s': %s",
                  k_slot_defs[id].slug, err ? err->message : "empty body");
        notify_ready(ab);
        return;
    }

    BgSlotTask *td = g_new0(BgSlotTask, 1);
    td->ab = ab;
    td->alive = g_object_ref(ab->cancellable);
    td->slot_id = id;
    td->json_cache_path = g_strdup(ab->slots[id].json_cache_path);
    td->raw_body = g_bytes_ref(body);
    td->max_rules = k_slot_defs[id].max_rules;

    GTask *task = g_task_new(NULL, ab->cancellable, on_remote_compile_done, NULL);
    g_task_set_task_data(task, td, bg_slot_task_free);
    g_task_run_in_thread(task, remote_compile_worker_thread);
    g_object_unref(task);
}

static void
fetch_slot_async(KajoWebAdblock *ab, AdblockSlotId id)
{
    if (!ab || !k_slot_defs[id].url)
        return;
    ab->pending_downloads++;
    ListFetchCtx *ctx = g_new0(ListFetchCtx, 1);
    ctx->ab = ab;
    ctx->alive = g_object_ref(ab->cancellable);
    ctx->slot_id = id;
    g_autoptr(SoupMessage) msg = soup_message_new("GET", k_slot_defs[id].url);
    soup_session_send_and_read_async(ab->soup, msg, G_PRIORITY_DEFAULT,
                                     ab->cancellable, on_remote_list_fetched, ctx);
}

static void
start_initial_slot_loads(KajoWebAdblock *ab)
{
    ab->slots_bootstrapped = TRUE;
    ab->initial_barrier_done = FALSE;
    ab->pending_initial_loads = 0;

    if (!ab->enabled) {
        ab->initial_barrier_done = TRUE;
        refresh_status_text(ab);
        return;
    }

    /* 1. Base slot: check manifest match against custom mtime or resource bytes */
    ab->pending_initial_loads++;
    const FilterSlot *mb = &ab->slots[AB_SLOT_BASE];
    gboolean use_base_manifest = FALSE;
    g_autofree gchar *cur_custom = g_strdup(ab->custom_rules_path ? ab->custom_rules_path : "");
    gint64 cur_mtime = 0;
    gboolean has_custom = (*cur_custom && g_file_test(cur_custom, G_FILE_TEST_EXISTS));
    if (has_custom) {
        GStatBuf st;
        if (g_stat(cur_custom, &st) == 0)
            cur_mtime = (gint64)st.st_mtime;
    }

    if (ab->manifest_version == AB_COMPILER_VERSION &&
        mb->manifest_fid && *mb->manifest_fid && mb->manifest_rules > 0 &&
        g_strcmp0(mb->manifest_custom_path ? mb->manifest_custom_path : "", cur_custom) == 0 &&
        cur_mtime == mb->manifest_mtime) {
        if (has_custom) {
            use_base_manifest = TRUE;
        } else {
            gsize blen = 0;
            g_autofree gchar *bjson = load_base_rules_json(ab, &blen);
            g_autofree gchar *bfid = compute_slot_fid(AB_SLOT_BASE, bjson, blen);
            use_base_manifest = (g_strcmp0(bfid, mb->manifest_fid) == 0);
        }
    }

    if (use_base_manifest) {
        SlotCompileCtx *bctx = g_new0(SlotCompileCtx, 1);
        bctx->ab = ab;
        bctx->alive = g_object_ref(ab->cancellable);
        bctx->slot_id = AB_SLOT_BASE;
        bctx->filter_id = g_strdup(mb->manifest_fid);
        bctx->expected_rules = mb->manifest_rules;
        bctx->is_initial = TRUE;
        webkit_user_content_filter_store_load(ab->store, bctx->filter_id, ab->cancellable,
                                              on_slot_loaded, bctx);
    } else {
        load_base_slot_async(ab, TRUE);
    }

    /* 2. Remote slots: Case A (hot manifest DFA mmap), Case B (v1->v2 worker task), Case C (bg fetch) */
    for (guint i = AB_SLOT_EASYLIST; i < AB_SLOT_COUNT; i++) {
        const FilterSlot *ms = &ab->slots[i];
        if (ab->manifest_version == AB_COMPILER_VERSION &&
            ms->manifest_fid && *ms->manifest_fid && ms->manifest_rules > 0) {
            ab->pending_initial_loads++;
            SlotCompileCtx *sctx = g_new0(SlotCompileCtx, 1);
            sctx->ab = ab;
            sctx->alive = g_object_ref(ab->cancellable);
            sctx->slot_id = (AdblockSlotId)i;
            sctx->filter_id = g_strdup(ms->manifest_fid);
            sctx->expected_rules = ms->manifest_rules;
            sctx->is_initial = TRUE;
            webkit_user_content_filter_store_load(ab->store, sctx->filter_id, ab->cancellable,
                                                  on_slot_loaded, sctx);
        } else if (ms->json_cache_path && g_file_test(ms->json_cache_path, G_FILE_TEST_EXISTS)) {
            ab->pending_initial_loads++;
            start_cached_json_task(ab, (AdblockSlotId)i,
                                   ab->manifest_version < AB_COMPILER_VERSION, TRUE);
        } else {
            fetch_slot_async(ab, (AdblockSlotId)i);
        }
    }

    if (ab->pending_initial_loads == 0) {
        ab->initial_barrier_done = TRUE;
        sync_shared_ucm(ab);
        notify_ready(ab);
    } else {
        refresh_status_text(ab);
    }
}

void
web_adblock_update_lists_async(KajoWebAdblock *ab)
{
    if (!ab || ab->pending_downloads > 0)
        return;

    for (guint i = AB_SLOT_EASYLIST; i < AB_SLOT_COUNT; i++)
        fetch_slot_async(ab, (AdblockSlotId)i);
    notify_ready(ab);
}

gboolean
web_adblock_is_updating(const KajoWebAdblock *ab)
{
    return ab ? (ab->pending_downloads > 0) : FALSE;
}

KajoWebAdblock *
web_adblock_new(const gchar           *cache_dir,
                const gchar           *data_dir,
                const KajoWebCfg      *cfg,
                KajoWebAdblockReadyCb  on_ready,
                gpointer               user_data)
{
    KajoWebAdblock *ab = g_new0(KajoWebAdblock, 1);
    ab->cache_dir = g_strdup(cache_dir ? cache_dir : g_get_user_cache_dir());
    ab->lists_dir = g_build_filename(ab->cache_dir, "adblock-lists", NULL);
    ab->store_dir = g_build_filename(ab->cache_dir, "content-filters", NULL);
    ab->manifest_path = g_build_filename(ab->cache_dir, "slots-manifest.json", NULL);
    g_mkdir_with_parents(ab->cache_dir, 0700);
    g_mkdir_with_parents(ab->lists_dir, 0700);
    g_mkdir_with_parents(ab->store_dir, 0700);
    cleanup_orphan_temp_filters(ab->store_dir);

    if (data_dir && *data_dir) {
        g_mkdir_with_parents(data_dir, 0700);
        ab->allowlist_path = g_build_filename(data_dir, "adblock-allowlist.txt", NULL);
    }

    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        ab->slots[i].id = (AdblockSlotId)i;
        if (i != AB_SLOT_BASE) {
            g_autofree gchar *fname = g_strdup_printf("%s.json", k_slot_defs[i].slug);
            ab->slots[i].json_cache_path = g_build_filename(ab->lists_dir, fname, NULL);
        }
    }

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

    ab->store = webkit_user_content_filter_store_new(ab->store_dir);
    ab->shared_ucm = webkit_user_content_manager_new();
    ab->paused_ucm = webkit_user_content_manager_new();
    ab->soup = soup_session_new();
    soup_session_set_timeout(ab->soup, 20);
    ab->paused_hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ab->user_scripts = g_ptr_array_new_with_free_func((GDestroyNotify)webkit_user_script_unref);
    ab->user_styles = g_ptr_array_new_with_free_func((GDestroyNotify)webkit_user_style_sheet_unref);
    ab->cancellable = g_cancellable_new();
    ab->enabled = cfg ? cfg->adblock_enabled : TRUE;
    ab->user_scripts_enabled = cfg ? cfg->user_scripts_enabled : FALSE;
    ab->custom_rules_path = expand_tilde_path(cfg ? cfg->adblock_custom_rules_path : NULL);
    ab->status_text = g_strdup(ab->enabled ? "LOADING RULES..." : "DISABLED");
    ab->on_ready = on_ready;
    ab->on_ready_ud = user_data;

    load_allowlist(ab);
    load_manifest(ab);
    reload_user_scripts(ab);
    sync_shared_ucm(ab);

    if (ab->enabled)
        start_initial_slot_loads(ab);
    else
        ab->initial_barrier_done = TRUE;

    refresh_status_text(ab);
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
    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        g_clear_pointer(&ab->slots[i].filter, webkit_user_content_filter_unref);
        g_free(ab->slots[i].active_filter_id);
        g_free(ab->slots[i].json_cache_path);
        g_free(ab->slots[i].manifest_fid);
        g_free(ab->slots[i].manifest_custom_path);
    }
    g_clear_object(&ab->shared_ucm);
    g_clear_object(&ab->paused_ucm);
    g_clear_object(&ab->store);
    g_clear_pointer(&ab->paused_hosts, g_hash_table_unref);
    g_clear_pointer(&ab->user_scripts, g_ptr_array_unref);
    g_clear_pointer(&ab->user_styles, g_ptr_array_unref);
    g_free(ab->custom_rules_path);
    g_free(ab->allowlist_path);
    g_free(ab->manifest_path);
    g_free(ab->store_dir);
    g_free(ab->lists_dir);
    g_free(ab->cache_dir);
    g_free(ab->status_text);
    g_free(ab);
}

void
web_adblock_apply_config(KajoWebAdblock *ab, const KajoWebCfg *cfg)
{
    if (!ab || !cfg)
        return;

    g_autofree gchar *new_custom = expand_tilde_path(cfg->adblock_custom_rules_path);
    gboolean enabled_changed = (ab->enabled != cfg->adblock_enabled);
    gboolean us_changed = (ab->user_scripts_enabled != cfg->user_scripts_enabled);
    gboolean path_changed = (g_strcmp0(ab->custom_rules_path ? ab->custom_rules_path : "", new_custom) != 0);

    if (!enabled_changed && !us_changed && !path_changed)
        return;

    ab->enabled = cfg->adblock_enabled;
    ab->user_scripts_enabled = cfg->user_scripts_enabled;
    if (path_changed) {
        g_free(ab->custom_rules_path);
        ab->custom_rules_path = g_steal_pointer(&new_custom);
        if (ab->enabled && ab->slots_bootstrapped)
            load_base_slot_async(ab, FALSE);
    }
    if (enabled_changed && ab->enabled && !ab->slots_bootstrapped)
        start_initial_slot_loads(ab);
    if (us_changed || (ab->user_scripts_enabled && enabled_changed))
        reload_user_scripts(ab);
    sync_shared_ucm(ab);
    refresh_status_text(ab);
}

WebKitUserContentManager *
web_adblock_get_shared_ucm(KajoWebAdblock *ab)
{
    return ab ? ab->shared_ucm : NULL;
}

WebKitUserContentManager *
web_adblock_get_paused_ucm(KajoWebAdblock *ab)
{
    return ab ? ab->paused_ucm : NULL;
}

gboolean
web_adblock_is_host_paused(const KajoWebAdblock *ab, const gchar *host)
{
    if (!ab || !ab->paused_hosts || !host || !*host)
        return FALSE;
    g_autofree gchar *norm = normalize_host(host);
    if (!norm)
        return FALSE;
    return g_hash_table_contains(ab->paused_hosts, norm);
}

gboolean
web_adblock_toggle_host_paused(KajoWebAdblock *ab, const gchar *host)
{
    if (!ab || !ab->paused_hosts || !host || !*host)
        return FALSE;
    g_autofree gchar *norm = normalize_host(host);
    if (!norm)
        return FALSE;
    gboolean now_paused = FALSE;
    if (g_hash_table_contains(ab->paused_hosts, norm)) {
        g_hash_table_remove(ab->paused_hosts, norm);
        now_paused = FALSE;
    } else {
        g_hash_table_insert(ab->paused_hosts, g_strdup(norm), GINT_TO_POINTER(1));
        now_paused = TRUE;
    }
    save_allowlist(ab);
    return now_paused;
}

guint
web_adblock_get_paused_host_count(const KajoWebAdblock *ab)
{
    return (ab && ab->paused_hosts) ? g_hash_table_size(ab->paused_hosts) : 0;
}

gboolean
web_adblock_is_enabled(const KajoWebAdblock *ab)
{
    return ab ? ab->enabled : FALSE;
}

gboolean
web_adblock_is_initial_load_done(const KajoWebAdblock *ab)
{
    return ab ? ab->initial_barrier_done : TRUE;
}

gboolean
web_adblock_is_ready(const KajoWebAdblock *ab)
{
    if (!ab || !ab->initial_barrier_done)
        return FALSE;
    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        if (ab->slots[i].filter != NULL)
            return TRUE;
    }
    return FALSE;
}

gboolean
web_adblock_has_error(const KajoWebAdblock *ab)
{
    return ab && ab->slots[AB_SLOT_BASE].failed && !web_adblock_is_ready(ab);
}

guint
web_adblock_get_rule_count(const KajoWebAdblock *ab)
{
    if (!ab)
        return 0;
    guint total = 0;
    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        if (ab->slots[i].filter)
            total += ab->slots[i].compiled_rules;
    }
    return total;
}

guint
web_adblock_get_loaded_list_count(const KajoWebAdblock *ab)
{
    if (!ab)
        return 0;
    guint n = 0;
    for (guint i = 0; i < AB_SLOT_COUNT; i++) {
        if (ab->slots[i].filter)
            n++;
    }
    return n;
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
