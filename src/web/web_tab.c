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
    WebKitUserContentManager *ucm;
    WebKitWebViewSessionState *saved_state;
    gchar                    *last_uri;
    gchar                    *last_title;
    gboolean                  suspended;
    gboolean                  muted;
    GPtrArray                *media_streams; /* KajoWebMediaStream* */
    GCancellable             *cancellable;
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
configure_webkit_settings(WebKitWebView *view, const KajoWebCfg *cfg)
{
    if (!view || !cfg)
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
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    webkit_settings_set_enable_dns_prefetching(s, cfg->dns_prefetch);
    G_GNUC_END_IGNORE_DEPRECATIONS
    webkit_settings_set_enable_developer_extras(s, cfg->developer_extras);
    webkit_settings_set_enable_media_stream(s, cfg->enable_webrtc);
    webkit_settings_set_enable_mediasource(s, TRUE);
    webkit_settings_set_default_font_size(s, (guint32)CLAMP(cfg->default_font_size_px, 10, 32));
    webkit_settings_set_default_monospace_font_size(s, (guint32)CLAMP(cfg->mono_font_size_px, 10, 28));
    webkit_settings_set_minimum_font_size(s, (guint32)CLAMP(cfg->min_font_size_px, 0, 24));

    const gchar *ua = resolve_ua_string(cfg);
    webkit_settings_set_user_agent(s, ua);

    /* Optional WebGL GPU process feature flag when available */
    WebKitFeatureList *fl = webkit_settings_get_all_features();
    if (fl) {
        gsize n = webkit_feature_list_get_length(fl);
        for (gsize i = 0; i < n; i++) {
            WebKitFeature *f = webkit_feature_list_get(fl, i);
            if (f && g_strcmp0(webkit_feature_get_identifier(f), "UseGPUProcessForWebGL") == 0) {
                webkit_settings_set_feature_enabled(s, f, cfg->gpu_process_webgl);
                break;
            }
        }
        webkit_feature_list_unref(fl);
    }

    webkit_web_view_set_zoom_level(view, (gdouble)CLAMP(cfg->default_zoom_pct, 50, 200) / 100.0);
}

static void
notify_tab_changed(KajoWebTab *tab)
{
    if (tab && tab->cbs.on_changed)
        tab->cbs.on_changed(tab, tab->ud);
}

static void
on_notify_prop(WebKitWebView *view G_GNUC_UNUSED, GParamSpec *ps G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebTab *tab = user_data;
    const gchar *u = webkit_web_view_get_uri(tab->view);
    const gchar *t = webkit_web_view_get_title(tab->view);
    if (u && *u) {
        g_free(tab->last_uri);
        tab->last_uri = g_strdup(u);
    }
    if (t && *t) {
        g_free(tab->last_title);
        tab->last_title = g_strdup(t);
    }
    notify_tab_changed(tab);
}

static void
on_load_changed(WebKitWebView *view, WebKitLoadEvent ev, gpointer user_data)
{
    KajoWebTab *tab = user_data;
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
        tab->cbs.on_new_tab_requested(uri, FALSE, tab->ud);
    return NULL;
}

static gboolean
on_decide_policy(WebKitWebView            *view G_GNUC_UNUSED,
                 WebKitPolicyDecision     *decision,
                 WebKitPolicyDecisionType  type,
                 gpointer                  user_data)
{
    KajoWebTab *tab = user_data;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *act = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = act ? webkit_navigation_action_get_request(act) : NULL;
        const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
        if (uri && *uri && tab->cbs.on_new_tab_requested)
            tab->cbs.on_new_tab_requested(uri, FALSE, tab->ud);
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

static void
on_resource_load_started_safe(WebKitWebView     *view G_GNUC_UNUSED,
                              WebKitWebResource *res,
                              WebKitURIRequest  *req,
                              gpointer           user_data)
{
    KajoWebTab *tab = user_data;
    const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    KajoWebMediaKind early = web_media_classify_resource(uri, NULL, 0);
    if (early == KAJO_MEDIA_HLS || early == KAJO_MEDIA_DASH) {
        const gchar *lbl = (early == KAJO_MEDIA_HLS) ? "HLS Playlist (.m3u8)" : "DASH Manifest (.mpd)";
        const gchar *mime = (early == KAJO_MEDIA_HLS) ? "application/vnd.apple.mpegurl" : "application/dash+xml";
        if (web_media_list_add_unique(tab->media_streams,
                web_media_stream_new(early, uri, lbl, mime, 0, 0, 0, 0))) {
            if (tab->cbs.on_media_detected)
                tab->cbs.on_media_detected(tab, tab->ud);
        }
    }
    g_object_set_data(G_OBJECT(res), "kajo-tab", tab);
    g_signal_connect_object(res, "notify::response", G_CALLBACK(on_resource_resp_bridge), tab->container, 0);
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
create_backing_view(KajoWebTab *tab, const KajoWebCfg *cfg)
{
    if (tab->view)
        return;

    tab->ucm = webkit_user_content_manager_new();
    if (tab->adblock)
        web_adblock_attach_manager(tab->adblock, tab->ucm);

    tab->view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "web-context", tab->web_ctx,
        "network-session", tab->session,
        "user-content-manager", tab->ucm,
        NULL));

    configure_webkit_settings(tab->view, cfg);
    webkit_web_view_set_is_muted(tab->view, tab->muted);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->view), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(tab->view), TRUE);

    g_signal_connect(tab->view, "notify::title", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "notify::uri", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "notify::estimated-load-progress", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "notify::is-loading", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "notify::is-playing-audio", G_CALLBACK(on_notify_prop), tab);
    g_signal_connect(tab->view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->view, "permission-request", G_CALLBACK(on_permission_request), tab);
    g_signal_connect(tab->view, "create", G_CALLBACK(on_create_new_view), tab);
    g_signal_connect(tab->view, "decide-policy", G_CALLBACK(on_decide_policy), tab);
    g_signal_connect(tab->view, "resource-load-started", G_CALLBACK(on_resource_load_started_safe), tab);
    g_signal_connect(tab->view, "context-menu", G_CALLBACK(on_context_menu), tab);
    g_signal_connect(tab->view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), tab);

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
    gtk_widget_add_css_class(t, "web-popover-title");
    GtkWidget *s = gtk_label_new(subtitle);
    gtk_widget_add_css_class(s, "web-dim-label");
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), s);

    if (with_reload) {
        GtkWidget *btn = gtk_button_new_with_label("↻ RELOAD TAB");
        gtk_widget_add_css_class(btn, "action-btn-primary");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_crash_reload_clicked), tab);
        gtk_box_append(GTK_BOX(box), btn);
    }
    return box;
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
    KajoWebTab *tab = g_new0(KajoWebTab, 1);
    tab->id = tab_id;
    tab->web_ctx = g_object_ref(web_ctx);
    tab->session = g_object_ref(session);
    tab->adblock = adblock;
    if (cbs)
        tab->cbs = *cbs;
    tab->ud = user_data;
    tab->media_streams = g_ptr_array_new_with_free_func(web_media_stream_free);
    tab->cancellable = g_cancellable_new();
    tab->last_uri = g_strdup("kajo://newtab");
    tab->last_title = g_strdup("New Tab");

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

    create_backing_view(tab, cfg);
    return tab;
}

void
web_tab_free(KajoWebTab *tab)
{
    if (!tab)
        return;
    if (tab->cancellable)
        g_cancellable_cancel(tab->cancellable);
    g_clear_object(&tab->cancellable);
    if (tab->adblock && tab->ucm)
        web_adblock_detach_manager(tab->adblock, tab->ucm);
    if (tab->view)
        g_signal_handlers_disconnect_by_data(tab->view, tab);
    g_clear_pointer(&tab->saved_state, webkit_web_view_session_state_unref);
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

void
web_tab_load_uri(KajoWebTab *tab, const gchar *uri)
{
    if (!tab || !uri || !*uri)
        return;
    g_free(tab->last_uri);
    tab->last_uri = g_strdup(uri);
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
web_tab_apply_config(KajoWebTab *tab, const KajoWebCfg *cfg)
{
    if (tab && tab->view && cfg)
        configure_webkit_settings(tab->view, cfg);
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

    if (tab->adblock && tab->ucm)
        web_adblock_detach_manager(tab->adblock, tab->ucm);
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
    create_backing_view(tab, cfg);
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
    if (!text || !*text)
        webkit_find_controller_search_finish(fc);
    else
        webkit_find_controller_search(fc, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, 500);
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
