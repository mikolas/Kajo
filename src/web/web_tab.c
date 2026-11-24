#include "web_tab.h"
#include "web_downloads.h"
#include <string.h>

struct _KajoWebTab {
    guint64                   id;
    WebKitWebContext         *web_ctx;
    WebKitNetworkSession     *session;
    KajoWebAdblock           *adblock;
    KajoWebTabCallbacks       cbs;
    gpointer                  ud;

    GtkWidget                *container;     /* GtkStack: "web", "crashed", "suspended" */
    WebKitWebView            *view;
    WebKitUserContentManager *ucm;           /* ref to shared or paused UCM from adblock */
    gboolean                  using_paused_ucm;
    WebKitWebViewSessionState *saved_state;
    GdkTexture               *favicon;
    gchar                    *last_uri;
    gchar                    *last_title;
    gboolean                  loading;
    gboolean                  suspended;
    gboolean                  muted;
    GPtrArray                *media_streams; /* KajoWebMediaStream* */
    GCancellable             *cancellable;
    guint                     swap_idle_id;
    gchar                    *pending_swap_uri;
    gboolean                  pending_swap_paused;
    guint                     https_idle_id;
    gchar                    *pending_https_uri;
    gchar                    *last_https_upgrade;
    gchar                    *last_https_orig;
    GHashTable               *https_fallback_hosts;
    KajoWebAutoplayPolicy     autoplay_policy;
    gboolean                  https_only;
    gint                      applied_default_zoom_pct;
};

static const gchar *
resolve_ua_string(const KajoWebCfg *cfg)
{
    if (!cfg)
        return NULL;
    switch (cfg->user_agent_preset) {
    case KAJO_WEB_UA_SAFARI:
        return "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15";
    case KAJO_WEB_UA_CHROME_LINUX:
        return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    case KAJO_WEB_UA_CUSTOM:
        return (cfg->user_agent_custom && *cfg->user_agent_custom) ? cfg->user_agent_custom : NULL;
    case KAJO_WEB_UA_DEFAULT:
    default:
        return NULL;
    }
}

static void
configure_webkit_settings(KajoWebTab *tab, WebKitWebView *view, const KajoWebCfg *cfg)
{
    if (!tab || !view || !cfg)
        return;
    WebKitSettings *s = webkit_web_view_get_settings(view);
    if (!s)
        return;

    WebKitHardwareAccelerationPolicy hw = WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS;
    if (cfg->hardware_acceleration == KAJO_WEB_HW_NEVER)
        hw = WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER;

    webkit_settings_set_hardware_acceleration_policy(s, hw);
    webkit_settings_set_enable_webgl(s, cfg->enable_webgl);
    webkit_settings_set_enable_smooth_scrolling(s, cfg->smooth_scrolling);
    webkit_settings_set_enable_webrtc(s, cfg->enable_webrtc);
    webkit_settings_set_enable_developer_extras(s, cfg->developer_extras);
    webkit_settings_set_enable_media_stream(s, cfg->enable_webrtc);
    webkit_settings_set_enable_mediasource(s, TRUE);
    webkit_settings_set_default_font_size(s, (guint32)CLAMP(cfg->default_font_size_px, 10, 32));
    webkit_settings_set_default_monospace_font_size(s, (guint32)CLAMP(cfg->mono_font_size_px, 10, 28));
    webkit_settings_set_minimum_font_size(s, (guint32)CLAMP(cfg->min_font_size_px, 0, 24));

    g_autoptr(WebKitFeatureList) feats = webkit_settings_get_all_features();
    if (feats) {
        WebKitFeature *wgl_gpu = webkit_feature_list_find(feats, "UseGPUProcessForWebGL");
        if (wgl_gpu)
            webkit_settings_set_feature_enabled(
                s, wgl_gpu,
                cfg->enable_webgl && cfg->gpu_process_webgl &&
                cfg->hardware_acceleration != KAJO_WEB_HW_NEVER);
    }

    const gchar *ua = resolve_ua_string(cfg);
    webkit_settings_set_user_agent(s, ua);

    tab->autoplay_policy = cfg->autoplay_policy;
    tab->https_only = cfg->https_only;

    if (tab->applied_default_zoom_pct != cfg->default_zoom_pct) {
        tab->applied_default_zoom_pct = cfg->default_zoom_pct;
        webkit_web_view_set_zoom_level(view, (gdouble)CLAMP(cfg->default_zoom_pct, 50, 200) / 100.0);
    }
}

static void
notify_tab_changed(KajoWebTab *tab)
{
    if (tab && tab->cbs.on_changed)
        tab->cbs.on_changed(tab, tab->ud);
}

static void
on_notify_progress(WebKitWebView *view, GParamSpec *ps G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (!tab)
        return;
    gboolean now_loading = webkit_web_view_is_loading(view);
    if (now_loading != tab->loading) {
        tab->loading = now_loading;
        notify_tab_changed(tab);
    }
    if (tab->cbs.on_progress) {
        tab->cbs.on_progress(tab,
                             webkit_web_view_get_estimated_load_progress(view),
                             now_loading,
                             tab->ud);
    }
}

static void
on_notify_favicon(WebKitWebView *view, GParamSpec *ps G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (!tab)
        return;
    GdkTexture *tex = webkit_web_view_get_favicon(view);
    if (g_set_object(&tab->favicon, tex))
        notify_tab_changed(tab);
}

static void
on_notify_prop(WebKitWebView *view G_GNUC_UNUSED, GParamSpec *ps G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    const gchar *u = webkit_web_view_get_uri(tab->view);
    const gchar *t = webkit_web_view_get_title(tab->view);
    gboolean changed = FALSE;
    if (u && *u && g_strcmp0(u, tab->last_uri) != 0) {
        g_free(tab->last_uri);
        tab->last_uri = g_strdup(u);
        if (g_str_has_prefix(u, "kajo://") || g_str_has_prefix(u, "about:"))
            g_clear_object(&tab->favicon);
        changed = TRUE;
    }
    if (t && *t && g_strcmp0(t, tab->last_title) != 0) {
        g_free(tab->last_title);
        tab->last_title = g_strdup(t);
        changed = TRUE;
    }
    if (ps && g_strcmp0(ps->name, "is-playing-audio") == 0)
        changed = TRUE;
    if (changed)
        notify_tab_changed(tab);
}

static void
on_load_changed(WebKitWebView *view, WebKitLoadEvent ev, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    tab->loading = (ev != WEBKIT_LOAD_FINISHED);
    if (ev == WEBKIT_LOAD_COMMITTED) {
        g_ptr_array_set_size(tab->media_streams, 0);
        if (tab->cbs.on_media_detected)
            tab->cbs.on_media_detected(tab, tab->ud);
        gtk_stack_set_visible_child_name(GTK_STACK(tab->container), "web");
    } else if (ev == WEBKIT_LOAD_FINISHED) {
        const gchar *uri = webkit_web_view_get_uri(view);
        const gchar *title = webkit_web_view_get_title(view);
        if (uri && *uri) {
            g_free(tab->last_uri);
            tab->last_uri = g_strdup(uri);
        }
        if (title && *title) {
            g_free(tab->last_title);
            tab->last_title = g_strdup(title);
        }
        if (tab->cbs.on_visit_committed && uri && *uri)
            tab->cbs.on_visit_committed(tab, uri, title ? title : uri, tab->ud);
    }
    notify_tab_changed(tab);
}

static gboolean
nav_action_wants_background(WebKitNavigationAction *act)
{
    if (!act)
        return FALSE;
    guint btn = webkit_navigation_action_get_mouse_button(act);
    guint mods = webkit_navigation_action_get_modifiers(act);
    return (btn == 2 || (mods & GDK_CONTROL_MASK) != 0) && (mods & GDK_SHIFT_MASK) == 0;
}

static gboolean
on_permission_request(WebKitWebView *view G_GNUC_UNUSED, WebKitPermissionRequest *req, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (tab->cbs.on_permission_requested) {
        tab->cbs.on_permission_requested(tab, req, tab->ud);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *
on_create_new_view(WebKitWebView *view G_GNUC_UNUSED, WebKitNavigationAction *action, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    if (uri && *uri && tab->cbs.on_new_tab_requested)
        tab->cbs.on_new_tab_requested(uri, nav_action_wants_background(action), tab->ud);
    return NULL;
}

static void create_backing_view(KajoWebTab *tab, const KajoWebCfg *cfg, gboolean use_paused_ucm);

static gboolean
uri_is_adblock_paused(const KajoWebTab *tab, const gchar *uri)
{
    if (!tab || !tab->adblock || !uri)
        return FALSE;
    if (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://"))
        return FALSE;
    g_autofree gchar *host = web_model_extract_host(uri);
    return host ? web_adblock_is_host_paused(tab->adblock, host) : FALSE;
}

static void
swap_backing_view_ucm(KajoWebTab *tab, const KajoWebCfg *cfg,
                      gboolean use_paused_ucm, const gchar *load_uri)
{
    if (!tab || tab->suspended)
        return;
    if (tab->view) {
        g_clear_pointer(&tab->saved_state, webkit_web_view_session_state_unref);
        tab->saved_state = webkit_web_view_get_session_state(tab->view);
        if (tab->https_idle_id) {
            g_source_remove(tab->https_idle_id);
            tab->https_idle_id = 0;
        }
        g_clear_pointer(&tab->pending_https_uri, g_free);
        WebKitFindController *fc = webkit_web_view_get_find_controller(tab->view);
        if (fc)
            g_signal_handlers_disconnect_by_data(fc, tab);
        g_signal_handlers_disconnect_by_data(tab->view, tab);
        GtkWidget *old = gtk_stack_get_child_by_name(GTK_STACK(tab->container), "web");
        if (old)
            gtk_stack_remove(GTK_STACK(tab->container), old);
        tab->view = NULL;
    }
    g_clear_object(&tab->ucm);

    create_backing_view(tab, cfg, use_paused_ucm);
    if (tab->saved_state) {
        webkit_web_view_restore_session_state(tab->view, tab->saved_state);
        g_clear_pointer(&tab->saved_state, webkit_web_view_session_state_unref);
    }
    if (load_uri && *load_uri)
        webkit_web_view_load_uri(tab->view, load_uri);
}

static gboolean
on_deferred_ucm_swap(gpointer user_data)
{
    KajoWebTab *tab = user_data;
    tab->swap_idle_id = 0;
    g_autofree gchar *dest = g_steal_pointer(&tab->pending_swap_uri);
    if (dest && *dest)
        swap_backing_view_ucm(tab, NULL, tab->pending_swap_paused, dest);
    return G_SOURCE_REMOVE;
}

static gboolean
host_is_local_plain(const gchar *host)
{
    if (!host || !*host)
        return TRUE;
    return g_ascii_strcasecmp(host, "localhost") == 0 ||
           g_strcmp0(host, "127.0.0.1") == 0 ||
           g_strcmp0(host, "::1") == 0 ||
           g_strcmp0(host, "[::1]") == 0 ||
           g_str_has_suffix(host, ".local") ||
           strchr(host, '.') == NULL;
}

static gboolean
on_deferred_https_upgrade(gpointer user_data)
{
    KajoWebTab *tab = user_data;
    tab->https_idle_id = 0;
    g_autofree gchar *dest = g_steal_pointer(&tab->pending_https_uri);
    if (dest && *dest && tab->view && !tab->suspended)
        webkit_web_view_load_uri(tab->view, dest);
    return G_SOURCE_REMOVE;
}

static gboolean
on_load_failed(WebKitWebView  *view G_GNUC_UNUSED,
               WebKitLoadEvent ev G_GNUC_UNUSED,
               const gchar    *failing_uri,
               GError         *error G_GNUC_UNUSED,
               gpointer        user_data)
{
    KajoWebTab *tab = user_data;
    if (tab->last_https_upgrade && tab->last_https_orig && failing_uri &&
        g_strcmp0(failing_uri, tab->last_https_upgrade) == 0) {
        g_autofree gchar *host = web_model_extract_host(failing_uri);
        if (host && *host && tab->https_fallback_hosts)
            g_hash_table_add(tab->https_fallback_hosts, g_strdup(host));
        g_clear_pointer(&tab->last_https_upgrade, g_free);
        g_free(tab->pending_https_uri);
        tab->pending_https_uri = g_steal_pointer(&tab->last_https_orig);
        if (!tab->https_idle_id)
            tab->https_idle_id = g_idle_add(on_deferred_https_upgrade, tab);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_decide_policy(WebKitWebView            *view G_GNUC_UNUSED,
                 WebKitPolicyDecision     *decision,
                 WebKitPolicyDecisionType  type,
                 gpointer                  user_data)
{
    KajoWebTab *tab = user_data;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *act = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = act ? webkit_navigation_action_get_request(act) : NULL;
        const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
        const gchar *frame = act ? webkit_navigation_action_get_frame_name(act) : NULL;

        if (uri && (!frame || !*frame) &&
            (g_str_has_prefix(uri, "kajo://search") || g_str_has_prefix(uri, "kajo://action"))) {
            if (tab->cbs.on_scheme_action && tab->cbs.on_scheme_action(tab, uri, tab->ud)) {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
        }

        if (act && webkit_navigation_action_get_navigation_type(act) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
            uri && *uri && tab->cbs.on_new_tab_requested) {
            guint btn = webkit_navigation_action_get_mouse_button(act);
            guint mods = webkit_navigation_action_get_modifiers(act);
            if (btn == 2 || (mods & GDK_CONTROL_MASK) != 0) {
                gboolean bg = (mods & GDK_SHIFT_MASK) == 0;
                tab->cbs.on_new_tab_requested(uri, bg, tab->ud);
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
        }

        if (tab->https_only && (!frame || !*frame) && uri && g_str_has_prefix(uri, "http://")) {
            g_autofree gchar *host = web_model_extract_host(uri);
            if (host && !host_is_local_plain(host) &&
                (!tab->https_fallback_hosts || !g_hash_table_contains(tab->https_fallback_hosts, host))) {
                g_autofree gchar *upgraded = g_strconcat("https://", uri + strlen("http://"), NULL);
                g_free(tab->last_https_upgrade);
                tab->last_https_upgrade = g_strdup(upgraded);
                g_free(tab->last_https_orig);
                tab->last_https_orig = g_strdup(uri);
                g_free(tab->pending_https_uri);
                tab->pending_https_uri = g_strdup(upgraded);
                if (!tab->https_idle_id)
                    tab->https_idle_id = g_idle_add(on_deferred_https_upgrade, tab);
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
        }

        if (act && webkit_navigation_action_is_user_gesture(act) &&
            (!frame || !*frame) && uri &&
            (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
            gboolean want_paused = uri_is_adblock_paused(tab, uri);
            if (want_paused != tab->using_paused_ucm) {
                webkit_policy_decision_ignore(decision);
                g_free(tab->pending_swap_uri);
                tab->pending_swap_uri = g_strdup(uri);
                tab->pending_swap_paused = want_paused;
                if (!tab->swap_idle_id)
                    tab->swap_idle_id = g_idle_add(on_deferred_ucm_swap, tab);
                return TRUE;
            }
        }

        if (!frame || !*frame) {
            WebKitAutoplayPolicy ap = WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND;
            if (tab->autoplay_policy == KAJO_WEB_AUTOPLAY_ALLOW)
                ap = WEBKIT_AUTOPLAY_ALLOW;
            else if (tab->autoplay_policy == KAJO_WEB_AUTOPLAY_DENY)
                ap = WEBKIT_AUTOPLAY_DENY;
            g_autoptr(WebKitWebsitePolicies) pol =
                webkit_website_policies_new_with_policies("autoplay", ap, NULL);
            webkit_policy_decision_use_with_policies(decision, pol);
            return TRUE;
        }
        return FALSE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *act = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = act ? webkit_navigation_action_get_request(act) : NULL;
        const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
        if (uri && *uri && tab->cbs.on_new_tab_requested)
            tab->cbs.on_new_tab_requested(uri, nav_action_wants_background(act), tab->ud);
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *rd = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!webkit_response_policy_decision_is_mime_type_supported(rd)) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
    }
    return FALSE;
}

/* ─── VDH-Style Passive Resource Sniffer (resource-load-started) ─── */

static void
on_resource_response(WebKitWebResource *res, GParamSpec *ps G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    WebKitURIResponse *resp = webkit_web_resource_get_response(res);
    if (!resp)
        return;
    const gchar *uri = webkit_uri_response_get_uri(resp);
    const gchar *mime = webkit_uri_response_get_mime_type(resp);
    gint64 len = (gint64)webkit_uri_response_get_content_length(resp);

    KajoWebMediaKind kind = web_media_classify_resource(uri, mime, len);
    if (kind == KAJO_MEDIA_NONE)
        return;

    const gchar *kstr = (kind == KAJO_MEDIA_HLS) ? "HLS (.m3u8)"
                      : (kind == KAJO_MEDIA_DASH) ? "DASH (.mpd)" : "Video Stream";
    g_autofree gchar *lbl = (len > 0)
        ? g_strdup_printf("%s (%.1f MB)", kstr, (double)len / (1024.0 * 1024.0))
        : g_strdup(kstr);

    if (web_media_list_add_unique(tab->media_streams,
            web_media_stream_new(kind, uri, lbl, mime, 0, 0, 0, len))) {
        if (tab->cbs.on_media_detected)
            tab->cbs.on_media_detected(tab, tab->ud);
    }
}

static void
on_resource_resp_bridge(WebKitWebResource *res, GParamSpec *ps, gpointer user_data G_GNUC_UNUSED)
{
    KajoWebTab *tab = g_object_get_data(G_OBJECT(res), "kajo-tab");
    if (tab)
        on_resource_response(res, ps, tab);
}

static gboolean
uri_obviously_non_media(const gchar *uri)
{
    if (!uri || (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://")))
        return TRUE;
    if (strstr(uri, "googlevideo.com/videoplayback") ||
        strstr(uri, "/generate_204") ||
        strstr(uri, "/api/stats/") ||
        strstr(uri, "/ptracking") ||
        strstr(uri, "/log_event") ||
        strstr(uri, "/youtubei/v1/")) {
        return TRUE;
    }
    const gchar *q = strchr(uri, '?');
    const gchar *end = q ? q : uri + strlen(uri);
    const gchar *dot = NULL;
    for (const gchar *p = end; p > uri && *p != '/'; p--) {
        if (*p == '.') {
            dot = p + 1;
            break;
        }
    }
    if (!dot || dot >= end)
        return FALSE;
    gsize ext_len = (gsize)(end - dot);
    if (ext_len > 6)
        return FALSE;
    static const char *const k_skip[] = {
        "js", "mjs", "css", "png", "jpg", "jpeg", "webp", "gif", "svg",
        "ico", "woff", "woff2", "ttf", "otf", "eot", "json", "xml", "txt",
        "html", "htm", "avif", "map", "wasm"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_skip); i++) {
        if (strlen(k_skip[i]) == ext_len && g_ascii_strncasecmp(dot, k_skip[i], ext_len) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
on_resource_load_started_safe(WebKitWebView     *view G_GNUC_UNUSED,
                              WebKitWebResource *res,
                              WebKitURIRequest  *req,
                              gpointer           user_data)
{
    KajoWebTab *tab = user_data;
    const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    if (uri_obviously_non_media(uri))
        return;

    KajoWebMediaKind early = web_media_classify_resource(uri, NULL, 0);
    if (early == KAJO_MEDIA_HLS || early == KAJO_MEDIA_DASH) {
        const gchar *lbl = (early == KAJO_MEDIA_HLS) ? "HLS Playlist (.m3u8)" : "DASH Manifest (.mpd)";
        const gchar *mime = (early == KAJO_MEDIA_HLS) ? "application/vnd.apple.mpegurl" : "application/dash+xml";
        if (web_media_list_add_unique(tab->media_streams,
                web_media_stream_new(early, uri, lbl, mime, 0, 0, 0, 0))) {
            if (tab->cbs.on_media_detected)
                tab->cbs.on_media_detected(tab, tab->ud);
        }
        return;
    }
    /* Only watch response headers for plausible video/stream endpoints */
    if (strstr(uri, ".mp4") || strstr(uri, ".webm") || strstr(uri, ".m4v") ||
        strstr(uri, "video") || strstr(uri, "stream") || strstr(uri, "media") ||
        strstr(uri, "playlist") || strstr(uri, "manifest")) {
        g_object_set_data(G_OBJECT(res), "kajo-tab", tab);
        g_signal_connect_object(res, "notify::response", G_CALLBACK(on_resource_resp_bridge), tab->container, 0);
    }
}

static void
on_ctx_open_player(GSimpleAction *act G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data)
{
    const gchar *uri = user_data;
    web_downloads_open_in_kajo_player(uri);
}

static void
free_closure_ptr(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
    g_free(data);
}

static gboolean
on_context_menu(WebKitWebView       *view G_GNUC_UNUSED,
                WebKitContextMenu   *menu,
                WebKitHitTestResult *hit,
                gpointer             user_data G_GNUC_UNUSED)
{
    if (webkit_hit_test_result_context_is_media(hit)) {
        const gchar *muri = webkit_hit_test_result_get_media_uri(hit);
        if (muri && *muri) {
            GSimpleAction *act = g_simple_action_new("open-in-kajo-player", NULL);
            g_signal_connect_data(act, "activate", G_CALLBACK(on_ctx_open_player),
                                  g_strdup(muri), free_closure_ptr, 0);
            WebKitContextMenuItem *item = webkit_context_menu_item_new_from_gaction(
                G_ACTION(act), "▶ Open Media in Kajo Player", NULL);
            webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
            webkit_context_menu_append(menu, item);
            g_object_unref(act);
        }
    }
    return FALSE;
}

static void
on_web_process_terminated(WebKitWebView *view G_GNUC_UNUSED,
                          WebKitWebProcessTerminationReason reason G_GNUC_UNUSED,
                          gpointer user_data)
{
    KajoWebTab *tab = user_data;
    gtk_stack_set_visible_child_name(GTK_STACK(tab->container), "crashed");
    notify_tab_changed(tab);
}

static void
on_crash_reload_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (tab->view) {
        gtk_stack_set_visible_child_name(GTK_STACK(tab->container), "web");
        webkit_web_view_reload(tab->view);
    }
}

static void
on_mouse_target_changed(WebKitWebView       *view G_GNUC_UNUSED,
                        WebKitHitTestResult *hit,
                        guint                modifiers G_GNUC_UNUSED,
                        gpointer             user_data)
{
    KajoWebTab *tab = user_data;
    if (!tab->cbs.on_hover_target)
        return;
    const gchar *link = (hit && webkit_hit_test_result_context_is_link(hit))
        ? webkit_hit_test_result_get_link_uri(hit)
        : NULL;
    tab->cbs.on_hover_target(tab, (link && *link) ? link : NULL, tab->ud);
}

static gboolean
on_enter_fullscreen(WebKitWebView *view G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (tab->cbs.on_fullscreen_changed)
        return tab->cbs.on_fullscreen_changed(tab, TRUE, tab->ud);
    return FALSE;
}

static gboolean
on_leave_fullscreen(WebKitWebView *view G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (tab->cbs.on_fullscreen_changed)
        return tab->cbs.on_fullscreen_changed(tab, FALSE, tab->ud);
    return FALSE;
}

static void
on_find_counted_matches(WebKitFindController *fc G_GNUC_UNUSED, guint match_count, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (tab->cbs.on_find_state)
        tab->cbs.on_find_state(tab, (gint)match_count, match_count > 0, tab->ud);
}

static void
on_find_failed(WebKitFindController *fc G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    if (tab->cbs.on_find_state)
        tab->cbs.on_find_state(tab, 0, FALSE, tab->ud);
}

static void
create_backing_view(KajoWebTab *tab, const KajoWebCfg *cfg, gboolean use_paused_ucm)
{
    if (tab->view)
        return;

    tab->using_paused_ucm = use_paused_ucm;
    if (tab->adblock) {
        WebKitUserContentManager *shared = use_paused_ucm
            ? web_adblock_get_paused_ucm(tab->adblock)
            : web_adblock_get_shared_ucm(tab->adblock);
        tab->ucm = shared ? g_object_ref(shared) : webkit_user_content_manager_new();
    } else {
        tab->ucm = webkit_user_content_manager_new();
    }

    tab->view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "web-context", tab->web_ctx,
        "network-session", tab->session,
        "user-content-manager", tab->ucm,
        NULL));

    configure_webkit_settings(tab, tab->view, cfg);
    static const GdkRGBA k_dark_bg = { 0.063f, 0.063f, 0.063f, 1.0f };
    webkit_web_view_set_background_color(tab->view, &k_dark_bg);
    webkit_web_view_set_is_muted(tab->view, tab->muted);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->view), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(tab->view), TRUE);

    g_signal_connect(tab->view, "notify::title", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "notify::uri", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "notify::favicon", G_CALLBACK(on_notify_favicon), tab);
    g_signal_connect(tab->view, "notify::estimated-load-progress", G_CALLBACK(on_notify_progress), tab);
    g_signal_connect(tab->view, "notify::is-loading", G_CALLBACK(on_notify_progress), tab);
    g_signal_connect(tab->view, "notify::is-playing-audio", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->view, "load-failed", G_CALLBACK(on_load_failed), tab);
    g_signal_connect(tab->view, "permission-request", G_CALLBACK(on_permission_request), tab);
    g_signal_connect(tab->view, "create", G_CALLBACK(on_create_new_view), tab);
    g_signal_connect(tab->view, "decide-policy", G_CALLBACK(on_decide_policy), tab);
    g_signal_connect(tab->view, "resource-load-started", G_CALLBACK(on_resource_load_started_safe), tab);
    g_signal_connect(tab->view, "context-menu", G_CALLBACK(on_context_menu), tab);
    g_signal_connect(tab->view, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), tab);
    g_signal_connect(tab->view, "enter-fullscreen", G_CALLBACK(on_enter_fullscreen), tab);
    g_signal_connect(tab->view, "leave-fullscreen", G_CALLBACK(on_leave_fullscreen), tab);
    g_signal_connect(tab->view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), tab);

    WebKitFindController *fc = webkit_web_view_get_find_controller(tab->view);
    if (fc) {
        g_signal_connect(fc, "counted-matches", G_CALLBACK(on_find_counted_matches), tab);
        g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(on_find_failed), tab);
    }

    GtkWidget *old = gtk_stack_get_child_by_name(GTK_STACK(tab->container), "web");
    if (old)
        gtk_stack_remove(GTK_STACK(tab->container), old);
    gtk_stack_add_named(GTK_STACK(tab->container), GTK_WIDGET(tab->view), "web");
    gtk_stack_set_visible_child_name(GTK_STACK(tab->container), "web");
}

static GtkWidget *
build_status_card(const gchar *title, const gchar *subtitle, gboolean with_reload, KajoWebTab *tab)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(box, "web-status-card");

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "shell-popover-title");
    GtkWidget *s = gtk_label_new(subtitle);
    gtk_widget_add_css_class(s, "web-dim-label");
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), s);

    if (with_reload) {
        GtkWidget *btn = gtk_button_new_with_label("↻ RELOAD TAB");
        gtk_widget_add_css_class(btn, "web-btn-primary");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_crash_reload_clicked), tab);
        gtk_box_append(GTK_BOX(box), btn);
    }
    return box;
}

static KajoWebTab *
web_tab_alloc_common(guint64                    tab_id,
                     WebKitWebContext          *web_ctx,
                     WebKitNetworkSession      *session,
                     KajoWebAdblock            *adblock,
                     const gchar               *uri,
                     const gchar               *title,
                     const KajoWebTabCallbacks *cbs,
                     gpointer                   user_data)
{
    KajoWebTab *tab = g_new0(KajoWebTab, 1);
    tab->id = tab_id;
    tab->web_ctx = g_object_ref(web_ctx);
    tab->session = g_object_ref(session);
    tab->adblock = adblock;
    if (cbs)
        tab->cbs = *cbs;
    tab->ud = user_data;
    tab->media_streams = g_ptr_array_new_with_free_func(web_media_stream_free);
    tab->https_fallback_hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    tab->applied_default_zoom_pct = -1;
    tab->cancellable = g_cancellable_new();
    tab->last_uri = g_strdup((uri && *uri) ? uri : "kajo://newtab");
    tab->last_title = g_strdup((title && *title) ? title : tab->last_uri);

    tab->container = gtk_stack_new();
    gtk_widget_set_hexpand(tab->container, TRUE);
    gtk_widget_set_vexpand(tab->container, TRUE);
    g_object_ref_sink(tab->container);

    gtk_stack_add_named(GTK_STACK(tab->container),
                        build_status_card("⚠ WEB PROCESS TERMINATED",
                                          "Tab process crashed or exceeded memory limit.", TRUE, tab),
                        "crashed");
    gtk_stack_add_named(GTK_STACK(tab->container),
                        build_status_card("💤 TAB SUSPENDED",
                                          "Unloaded to save memory. Restoring on focus...", FALSE, tab),
                        "suspended");
    return tab;
}

KajoWebTab *
web_tab_new(guint64                    tab_id,
            WebKitWebContext          *web_ctx,
            WebKitNetworkSession      *session,
            KajoWebAdblock            *adblock,
            const KajoWebCfg          *cfg,
            const KajoWebTabCallbacks *cbs,
            gpointer                   user_data)
{
    KajoWebTab *tab = web_tab_alloc_common(tab_id, web_ctx, session, adblock,
                                           "kajo://newtab", "New Tab", cbs, user_data);
    create_backing_view(tab, cfg, uri_is_adblock_paused(tab, tab->last_uri));
    return tab;
}

KajoWebTab *
web_tab_new_suspended(guint64                    tab_id,
                      WebKitWebContext          *web_ctx,
                      WebKitNetworkSession      *session,
                      KajoWebAdblock            *adblock,
                      const gchar               *uri,
                      const gchar               *title,
                      const KajoWebTabCallbacks *cbs,
                      gpointer                   user_data)
{
    KajoWebTab *tab = web_tab_alloc_common(tab_id, web_ctx, session, adblock,
                                           uri, title, cbs, user_data);
    tab->suspended = TRUE;
    gtk_stack_set_visible_child_name(GTK_STACK(tab->container), "suspended");
    return tab;
}

void
web_tab_free(KajoWebTab *tab)
{
    if (!tab)
        return;
    if (tab->swap_idle_id) {
        g_source_remove(tab->swap_idle_id);
        tab->swap_idle_id = 0;
    }
    if (tab->https_idle_id) {
        g_source_remove(tab->https_idle_id);
        tab->https_idle_id = 0;
    }
    g_clear_pointer(&tab->pending_swap_uri, g_free);
    g_clear_pointer(&tab->pending_https_uri, g_free);
    g_clear_pointer(&tab->last_https_upgrade, g_free);
    g_clear_pointer(&tab->last_https_orig, g_free);
    g_clear_pointer(&tab->https_fallback_hosts, g_hash_table_unref);
    if (tab->cancellable)
        g_cancellable_cancel(tab->cancellable);
    g_clear_object(&tab->cancellable);
    if (tab->view) {
        WebKitFindController *fc = webkit_web_view_get_find_controller(tab->view);
        if (fc)
            g_signal_handlers_disconnect_by_data(fc, tab);
        g_signal_handlers_disconnect_by_data(tab->view, tab);
    }
    g_clear_pointer(&tab->saved_state, webkit_web_view_session_state_unref);
    g_clear_object(&tab->favicon);
    g_clear_object(&tab->ucm);
    g_clear_object(&tab->container);
    tab->view = NULL;
    g_clear_object(&tab->session);
    g_clear_object(&tab->web_ctx);
    g_clear_pointer(&tab->media_streams, g_ptr_array_unref);
    g_free(tab->last_uri);
    g_free(tab->last_title);
    g_free(tab);
}

guint64
web_tab_get_id(const KajoWebTab *tab)
{
    return tab ? tab->id : 0;
}

GtkWidget *
web_tab_get_widget(KajoWebTab *tab)
{
    return tab ? tab->container : NULL;
}

WebKitWebView *
web_tab_get_view(KajoWebTab *tab)
{
    return tab ? tab->view : NULL;
}

GdkTexture *
web_tab_get_favicon(const KajoWebTab *tab)
{
    return tab ? tab->favicon : NULL;
}

void
web_tab_load_uri(KajoWebTab *tab, const gchar *uri)
{
    if (!tab || !uri || !*uri)
        return;
    g_free(tab->last_uri);
    tab->last_uri = g_strdup(uri);
    gboolean want_paused = uri_is_adblock_paused(tab, uri);
    if (tab->view && want_paused != tab->using_paused_ucm) {
        swap_backing_view_ucm(tab, NULL, want_paused, uri);
        return;
    }
    if (tab->view)
        webkit_web_view_load_uri(tab->view, uri);
}

void
web_tab_reload(KajoWebTab *tab, gboolean bypass_cache)
{
    if (!tab || !tab->view)
        return;
    if (bypass_cache)
        webkit_web_view_reload_bypass_cache(tab->view);
    else
        webkit_web_view_reload(tab->view);
}

void
web_tab_stop(KajoWebTab *tab)
{
    if (tab && tab->view)
        webkit_web_view_stop_loading(tab->view);
}

void
web_tab_go_back(KajoWebTab *tab)
{
    if (tab && tab->view && webkit_web_view_can_go_back(tab->view))
        webkit_web_view_go_back(tab->view);
}

void
web_tab_go_forward(KajoWebTab *tab)
{
    if (tab && tab->view && webkit_web_view_can_go_forward(tab->view))
        webkit_web_view_go_forward(tab->view);
}

gboolean
web_tab_can_go_back(const KajoWebTab *tab)
{
    return (tab && tab->view) ? webkit_web_view_can_go_back(tab->view) : FALSE;
}

gboolean
web_tab_can_go_forward(const KajoWebTab *tab)
{
    return (tab && tab->view) ? webkit_web_view_can_go_forward(tab->view) : FALSE;
}

gboolean
web_tab_is_loading(const KajoWebTab *tab)
{
    return (tab && tab->view) ? webkit_web_view_is_loading(tab->view) : FALSE;
}

gdouble
web_tab_get_progress(const KajoWebTab *tab)
{
    return (tab && tab->view) ? webkit_web_view_get_estimated_load_progress(tab->view) : 0.0;
}

const gchar *
web_tab_get_uri(const KajoWebTab *tab)
{
    if (!tab)
        return "";
    if (tab->view) {
        const gchar *u = webkit_web_view_get_uri(tab->view);
        if (u && *u)
            return u;
    }
    return tab->last_uri ? tab->last_uri : "";
}

const gchar *
web_tab_get_title(const KajoWebTab *tab)
{
    if (!tab)
        return "New Tab";
    if (tab->view) {
        const gchar *t = webkit_web_view_get_title(tab->view);
        if (t && *t)
            return t;
    }
    return (tab->last_title && *tab->last_title) ? tab->last_title : web_tab_get_uri(tab);
}

gboolean
web_tab_is_playing_audio(const KajoWebTab *tab)
{
    return (tab && tab->view) ? webkit_web_view_is_playing_audio(tab->view) : FALSE;
}

gboolean
web_tab_is_muted(const KajoWebTab *tab)
{
    return tab ? tab->muted : FALSE;
}

void
web_tab_set_muted(KajoWebTab *tab, gboolean muted)
{
    if (!tab)
        return;
    tab->muted = muted;
    if (tab->view)
        webkit_web_view_set_is_muted(tab->view, muted);
}

void
web_tab_set_zoom(KajoWebTab *tab, gdouble zoom)
{
    if (tab && tab->view)
        webkit_web_view_set_zoom_level(tab->view, CLAMP(zoom, 0.5, 2.5));
}

gdouble
web_tab_get_zoom(const KajoWebTab *tab)
{
    return (tab && tab->view) ? webkit_web_view_get_zoom_level(tab->view) : 1.0;
}

void
web_tab_toggle_inspector(KajoWebTab *tab)
{
    if (!tab || !tab->view)
        return;
    WebKitWebInspector *insp = webkit_web_view_get_inspector(tab->view);
    if (!insp)
        return;
    if (webkit_web_inspector_is_attached(insp))
        webkit_web_inspector_close(insp);
    else
        webkit_web_inspector_show(insp);
}

void
web_tab_print(KajoWebTab *tab, GtkWindow *parent)
{
    if (!tab || !tab->view)
        return;
    g_autoptr(WebKitPrintOperation) op = webkit_print_operation_new(tab->view);
    webkit_print_operation_run_dialog(op, parent);
}

void
web_tab_apply_config(KajoWebTab *tab, const KajoWebCfg *cfg)
{
    if (tab && tab->view && cfg)
        configure_webkit_settings(tab, tab->view, cfg);
}

void
web_tab_sync_adblock_pause(KajoWebTab *tab, const KajoWebCfg *cfg)
{
    if (!tab || tab->suspended || !tab->view)
        return;
    const gchar *uri = web_tab_get_uri(tab);
    gboolean want_paused = uri_is_adblock_paused(tab, uri);
    if (want_paused != tab->using_paused_ucm) {
        g_autofree gchar *cur = g_strdup(uri);
        swap_backing_view_ucm(tab, cfg, want_paused, cur);
    } else {
        webkit_web_view_reload(tab->view);
    }
}

gboolean
web_tab_is_suspended(const KajoWebTab *tab)
{
    return tab ? tab->suspended : FALSE;
}

void
web_tab_suspend(KajoWebTab *tab)
{
    if (!tab || tab->suspended || !tab->view)
        return;
    g_clear_pointer(&tab->saved_state, webkit_web_view_session_state_unref);
    tab->saved_state = webkit_web_view_get_session_state(tab->view);

    if (tab->https_idle_id) {
        g_source_remove(tab->https_idle_id);
        tab->https_idle_id = 0;
    }
    g_clear_pointer(&tab->pending_https_uri, g_free);

    WebKitFindController *fc = webkit_web_view_get_find_controller(tab->view);
    if (fc)
        g_signal_handlers_disconnect_by_data(fc, tab);
    g_signal_handlers_disconnect_by_data(tab->view, tab);

    GtkWidget *old = gtk_stack_get_child_by_name(GTK_STACK(tab->container), "web");
    if (old)
        gtk_stack_remove(GTK_STACK(tab->container), old);
    tab->view = NULL;
    g_clear_object(&tab->ucm);

    tab->suspended = TRUE;
    gtk_stack_set_visible_child_name(GTK_STACK(tab->container), "suspended");
}

void
web_tab_wake(KajoWebTab *tab, const KajoWebCfg *cfg)
{
    if (!tab || !tab->suspended)
        return;
    tab->suspended = FALSE;
    create_backing_view(tab, cfg, uri_is_adblock_paused(tab, tab->last_uri));
    if (tab->saved_state) {
        webkit_web_view_restore_session_state(tab->view, tab->saved_state);
        g_clear_pointer(&tab->saved_state, webkit_web_view_session_state_unref);
    }
    if (tab->last_uri && *tab->last_uri)
        webkit_web_view_load_uri(tab->view, tab->last_uri);
}

const GPtrArray *
web_tab_get_media_streams(const KajoWebTab *tab)
{
    return tab ? tab->media_streams : NULL;
}

void
web_tab_find_search(KajoWebTab *tab, const gchar *text)
{
    if (!tab || !tab->view)
        return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(tab->view);
    if (!fc)
        return;
    if (!text || !*text) {
        webkit_find_controller_search_finish(fc);
        if (tab->cbs.on_find_state)
            tab->cbs.on_find_state(tab, -1, FALSE, tab->ud);
    } else {
        guint32 opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
        webkit_find_controller_search(fc, text, opts, 500);
        webkit_find_controller_count_matches(fc, text, opts, 500);
    }
}

void
web_tab_find_next(KajoWebTab *tab)
{
    if (tab && tab->view)
        webkit_find_controller_search_next(webkit_web_view_get_find_controller(tab->view));
}

void
web_tab_find_prev(KajoWebTab *tab)
{
    if (tab && tab->view)
        webkit_find_controller_search_previous(webkit_web_view_get_find_controller(tab->view));
}

void
web_tab_find_clear(KajoWebTab *tab)
{
    if (tab && tab->view)
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(tab->view));
}
