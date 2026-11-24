#include "web_window_private.h"
#include "../compositor/compositor.h"
#include "../common/spawn_detached.h"
#include <math.h>
#include <string.h>

static guint64 *
u64_key(guint64 v)
{
    guint64 *p = g_new(guint64, 1);
    *p = v;
    return p;
}

KajoWebTab *
kajo_web_win_active_tab(KajoWebWindow *w)
{
    if (!w || !w->model || w->model->active_id == 0)
        return NULL;
    return g_hash_table_lookup(w->tabs, &w->model->active_id);
}

void
kajo_web_win_open_settings(void)
{
    g_autofree gchar *bin = g_find_program_in_path("kajo-settings");
    const gchar *argv[] = { bin ? bin : "kajo-settings", "--page", "web", NULL };
    kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
}

static void
apply_profile_border_accent(KajoWebWindow *w, const KajoWebCfg *cfg)
{
    if (w->border_css) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(w->border_css));
        g_clear_object(&w->border_css);
    }
    if (!cfg || !cfg->profile_border_accent || w->is_private)
        return;

    KajoWebProfileRegistry *reg = web_profile_manager_get_registry(w->pm);
    KajoWebProfileInfo *p = reg ? web_profile_registry_find(reg, w->profile_id) : NULL;
    if (!p || !p->accent_color || p->accent_color[0] != '#')
        return;

    if (!w->border_css_class) {
        w->border_css_class = g_strdup_printf("web-prof-frame-%p", (void *)w);
        gtk_widget_add_css_class(GTK_WIDGET(w->win), w->border_css_class);
    }

    w->border_css = gtk_css_provider_new();
    g_autofree gchar *css = g_strdup_printf(
        "window.%s { border-color: %s; }\n"
        "window.%s .web-profile-dot { background-color: %s; }\n",
        w->border_css_class, p->accent_color,
        w->border_css_class, p->accent_color);
    gtk_css_provider_load_from_string(w->border_css, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(w->border_css),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
}

gboolean
kajo_web_win_is_blank_uri(const gchar *uri)
{
    return !uri || !*uri ||
           g_strcmp0(uri, "kajo://newtab") == 0 ||
           g_strcmp0(uri, "about:blank") == 0;
}

void
kajo_web_win_focus_webview(KajoWebWindow *w)
{
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t && web_tab_get_view(t))
        gtk_widget_grab_focus(GTK_WIDGET(web_tab_get_view(t)));
}

void
kajo_web_win_cancel_deferred_load(KajoWebWindow *w)
{
    if (!w)
        return;
    w->deferred_initial_tab_id = 0;
    if (w->deferred_load_timer_id) {
        g_source_remove(w->deferred_load_timer_id);
        w->deferred_load_timer_id = 0;
    }
}

static gboolean
focus_omnibar_idle_cb(gpointer ud)
{
    KajoWebWindow *w = ud;
    w->focus_omni_idle_id = 0;
    if (w->omni_entry && gtk_widget_get_visible(w->header_handle)) {
        gtk_widget_grab_focus(w->omni_entry);
        gtk_editable_select_region(GTK_EDITABLE(w->omni_entry), 0, -1);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_focus_omnibar(KajoWebWindow *w)
{
    if (!w)
        return;
    if (w->top_autohide && w->header_handle)
        gtk_widget_set_visible(w->header_handle, TRUE);
    if (w->focus_omni_idle_id)
        g_source_remove(w->focus_omni_idle_id);
    w->focus_omni_idle_id = g_idle_add(focus_omnibar_idle_cb, w);
}

static void
set_css_class_if_changed(GtkWidget *w, const gchar *cls, gboolean enabled)
{
    if (!w)
        return;
    gboolean has = gtk_widget_has_css_class(w, cls);
    if (enabled && !has)
        gtk_widget_add_css_class(w, cls);
    else if (!enabled && has)
        gtk_widget_remove_css_class(w, cls);
}

void
kajo_web_win_invalidate_bookmark_cache(KajoWebWindow *w)
{
    if (!w)
        return;
    g_clear_pointer(&w->cached_bm_uri, g_free);
    w->cached_bm_starred = FALSE;
    w->cached_bm_valid = FALSE;
}

void
kajo_web_win_sync_chrome(KajoWebWindow *w)
{
    if (!w || !w->btn_back || !w->btn_shield || !w->omni_entry)
        return;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    const KajoWebCfg *cfg = w->cfg;

    gboolean can_back = t && web_tab_can_go_back(t);
    gboolean can_fwd  = t && web_tab_can_go_forward(t);
    gboolean tab_loading = t && web_tab_is_loading(t);
    if (gtk_widget_get_sensitive(w->btn_back) != can_back)
        gtk_widget_set_sensitive(w->btn_back, can_back);
    if (gtk_widget_get_sensitive(w->btn_fwd) != can_fwd)
        gtk_widget_set_sensitive(w->btn_fwd, can_fwd);
    const gchar *rel_lbl = tab_loading ? "✕" : "↻";
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(w->btn_reload)), rel_lbl) != 0)
        gtk_button_set_label(GTK_BUTTON(w->btn_reload), rel_lbl);

    const gchar *uri = t ? web_tab_get_uri(t) : "kajo://newtab";
    if (!gtk_widget_has_focus(w->omni_entry)) {
        const gchar *disp_uri = kajo_web_win_is_blank_uri(uri) ? "" : uri;
        const gchar *cur_txt = gtk_editable_get_text(GTK_EDITABLE(w->omni_entry));
        if (g_strcmp0(cur_txt, disp_uri) != 0) {
            w->updating_omni = TRUE;
            gtk_editable_set_text(GTK_EDITABLE(w->omni_entry), disp_uri);
            w->updating_omni = FALSE;
        }
    }

    gint tls_kind = 2;
    if (g_str_has_prefix(uri, "kajo://") || g_str_has_prefix(uri, "about:"))
        tls_kind = 0;
    else if (g_str_has_prefix(uri, "https://"))
        tls_kind = 1;
    if (w->last_tls_kind != tls_kind) {
        w->last_tls_kind = tls_kind;
        gtk_label_set_text(GTK_LABEL(w->omni_tls),
                           tls_kind == 0 ? "❖" : (tls_kind == 1 ? "🔒" : "⚠"));
        set_css_class_if_changed(w->omni_tls, "tls-kajo", tls_kind == 0);
        set_css_class_if_changed(w->omni_tls, "tls-secure", tls_kind == 1);
        set_css_class_if_changed(w->omni_tls, "tls-warn", tls_kind == 2);
    }

    set_css_class_if_changed(w->btn_pin_rail, "active", cfg && cfg->rail_mode == KAJO_WEB_RAIL_PINNED);

    gdouble prog = t ? web_tab_get_progress(t) : 0.0;
    gboolean loading = tab_loading && prog > 0.0 && prog < 1.0;
    if (gtk_widget_get_visible(w->omni_progress) != loading)
        gtk_widget_set_visible(w->omni_progress, loading);
    if (loading) {
        if (w->last_progress_frac < 0.0 || fabs(prog - w->last_progress_frac) >= 0.03 || prog >= 0.99) {
            w->last_progress_frac = prog;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->omni_progress), prog);
        }
    } else {
        w->last_progress_frac = -1.0;
    }

    gboolean ab_on = web_adblock_is_enabled(w->adblock);
    gboolean ab_ready = web_adblock_is_ready(w->adblock);
    gboolean ab_err = web_adblock_has_error(w->adblock);
    g_autofree gchar *cur_host = (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))
        ? web_model_extract_host(uri) : NULL;
    gboolean site_paused = cur_host && web_adblock_is_host_paused(w->adblock, cur_host);

    const gchar *shield_lbl = "🛡 ON";
    gboolean shield_off = FALSE;
    if (!ab_on) {
        shield_lbl = "🛡 OFF";
        shield_off = TRUE;
    } else if (site_paused) {
        shield_lbl = "🛡 PAUSED";
        shield_off = TRUE;
    } else if (ab_err) {
        shield_lbl = "🛡 ERR";
        shield_off = TRUE;
    } else if (!ab_ready) {
        shield_lbl = "🛡 ...";
    }
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(w->btn_shield)), shield_lbl) != 0)
        gtk_button_set_label(GTK_BUTTON(w->btn_shield), shield_lbl);
    set_css_class_if_changed(w->btn_shield, "off", shield_off);
    if (site_paused) {
        g_autofree gchar *tip = g_strdup_printf("Paused on %s — click to resume", cur_host);
        if (g_strcmp0(gtk_widget_get_tooltip_text(w->btn_shield), tip) != 0)
            gtk_widget_set_tooltip_text(w->btn_shield, tip);
    } else {
        const gchar *ab_tip = web_adblock_get_status_text(w->adblock);
        if (g_strcmp0(gtk_widget_get_tooltip_text(w->btn_shield), ab_tip) != 0)
            gtk_widget_set_tooltip_text(w->btn_shield, ab_tip);
    }

    gboolean is_sw = w->hw_disabled_override || (cfg && cfg->hardware_acceleration == KAJO_WEB_HW_NEVER);
    if (gtk_widget_get_visible(w->chip_sw) != is_sw)
        gtk_widget_set_visible(w->chip_sw, is_sw);
    if (gtk_widget_get_visible(w->chip_priv) != w->is_private)
        gtk_widget_set_visible(w->chip_priv, w->is_private);

    if (t) {
        gint zpct = (gint)(web_tab_get_zoom(t) * 100.0 + 0.5);
        gboolean zcustom = (zpct != 100);
        if (gtk_widget_get_visible(w->btn_zoom) != zcustom)
            gtk_widget_set_visible(w->btn_zoom, zcustom);
        if (zcustom) {
            g_autofree gchar *zl = g_strdup_printf("%d%%", zpct);
            if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(w->btn_zoom)), zl) != 0)
                gtk_button_set_label(GTK_BUTTON(w->btn_zoom), zl);
        }
    } else if (w->btn_zoom && gtk_widget_get_visible(w->btn_zoom)) {
        gtk_widget_set_visible(w->btn_zoom, FALSE);
    }

    if (w->btn_bookmark) {
        gboolean bm_show = !w->is_private && w->storage && uri &&
                           !g_str_has_prefix(uri, "kajo://") && !g_str_has_prefix(uri, "about:");
        if (gtk_widget_get_visible(w->btn_bookmark) != bm_show)
            gtk_widget_set_visible(w->btn_bookmark, bm_show);
        if (bm_show) {
            if (!w->cached_bm_valid || g_strcmp0(w->cached_bm_uri, uri) != 0) {
                g_free(w->cached_bm_uri);
                w->cached_bm_uri = g_strdup(uri);
                w->cached_bm_starred = web_storage_is_bookmarked(w->storage, uri, NULL);
                w->cached_bm_valid = TRUE;
            }
            const gchar *bm_lbl = w->cached_bm_starred ? "★" : "☆";
            if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(w->btn_bookmark)), bm_lbl) != 0)
                gtk_button_set_label(GTK_BUTTON(w->btn_bookmark), bm_lbl);
            set_css_class_if_changed(w->btn_bookmark, "starred", w->cached_bm_starred);
        }
    }

    const GPtrArray *streams = t ? web_tab_get_media_streams(t) : NULL;
    guint n_streams = streams ? streams->len : 0;
    if (n_streams > 0) {
        g_autofree gchar *ml = g_strdup_printf("🎬 %u", n_streams);
        if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(w->btn_media)), ml) != 0)
            gtk_button_set_label(GTK_BUTTON(w->btn_media), ml);
    } else if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(w->btn_media)), "🎬") != 0) {
        gtk_button_set_label(GTK_BUTTON(w->btn_media), "🎬");
    }

    if (t) {
        g_autofree gchar *wt = g_strdup_printf("%s — Kajo Web", web_tab_get_title(t));
        if (g_strcmp0(gtk_window_get_title(w->win), wt) != 0)
            gtk_window_set_title(w->win, wt);
    }
}

static gboolean
sync_chrome_idle_cb(gpointer ud)
{
    KajoWebWindow *w = ud;
    w->sync_chrome_idle_id = 0;
    kajo_web_win_sync_chrome(w);
    return G_SOURCE_REMOVE;
}

void
kajo_web_win_schedule_sync_chrome(KajoWebWindow *w)
{
    if (w && !w->sync_chrome_idle_id)
        w->sync_chrome_idle_id = g_idle_add(sync_chrome_idle_cb, w);
}

static void add_and_activate_tab(KajoWebWindow *w, const gchar *uri, gboolean background);

static void
on_tab_progress(KajoWebTab *tab, gdouble progress, gboolean loading, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (tab != kajo_web_win_active_tab(w))
        return;
    gboolean show = loading && progress > 0.0 && progress < 1.0;
    if (gtk_widget_get_visible(w->omni_progress) != show)
        gtk_widget_set_visible(w->omni_progress, show);
    if (show) {
        if (w->last_progress_frac < 0.0 || fabs(progress - w->last_progress_frac) >= 0.03 || progress >= 0.99) {
            w->last_progress_frac = progress;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->omni_progress), progress);
        }
    } else {
        w->last_progress_frac = -1.0;
    }
    const gchar *rel_lbl = loading ? "✕" : "↻";
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(w->btn_reload)), rel_lbl) != 0)
        gtk_button_set_label(GTK_BUTTON(w->btn_reload), rel_lbl);
    gboolean can_back = web_tab_can_go_back(tab);
    gboolean can_fwd  = web_tab_can_go_forward(tab);
    if (gtk_widget_get_sensitive(w->btn_back) != can_back)
        gtk_widget_set_sensitive(w->btn_back, can_back);
    if (gtk_widget_get_sensitive(w->btn_fwd) != can_fwd)
        gtk_widget_set_sensitive(w->btn_fwd, can_fwd);
}

void
kajo_web_win_on_tab_changed(KajoWebTab *tab, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTabMeta *meta = web_model_find_tab(w->model, web_tab_get_id(tab));
    if (meta) {
        const gchar *nt = web_tab_get_title(tab);
        const gchar *nu = web_tab_get_uri(tab);
        gboolean na = web_tab_is_playing_audio(tab);
        gboolean nm = web_tab_is_muted(tab);
        gboolean nl = web_tab_is_loading(tab);
        gboolean ns = web_tab_is_suspended(tab);
        if (g_strcmp0(meta->title, nt) != 0) {
            g_free(meta->title);
            meta->title = g_strdup(nt);
        }
        if (g_strcmp0(meta->uri, nu) != 0) {
            g_free(meta->uri);
            meta->uri = g_strdup(nu);
        }
        meta->audible = na;
        meta->muted = nm;
        meta->loading = nl;
        meta->suspended = ns;
        web_tab_rail_update_tab(w->rail, w->model, meta->id);
    }
    if (tab == kajo_web_win_active_tab(w))
        kajo_web_win_schedule_sync_chrome(w);
}

static void
on_tab_new_requested(const gchar *uri, gboolean background, gpointer ud)
{
    add_and_activate_tab((KajoWebWindow *)ud, uri, background);
}

static void
on_tab_permission_requested(KajoWebTab *tab G_GNUC_UNUSED, WebKitPermissionRequest *req, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->pending_perm) {
        webkit_permission_request_deny(w->pending_perm);
        g_clear_object(&w->pending_perm);
    }
    w->pending_perm = g_object_ref(req);

    const gchar *kind = "Site Permission";
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req))
        kind = "Camera / Microphone (XDG Portal)";
    else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req))
        kind = "Desktop Notifications";
    else if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req))
        kind = "Geolocation";
    else if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(req))
        kind = "Clipboard Read";

    g_autofree gchar *msg = g_strdup_printf("⚠ Page requests access to: %s", kind);
    gtk_label_set_text(GTK_LABEL(w->perm_lbl), msg);
    gtk_widget_set_visible(w->perm_bar, TRUE);
}

static void
on_tab_media_detected(KajoWebTab *tab G_GNUC_UNUSED, gpointer ud)
{
    kajo_web_win_sync_chrome((KajoWebWindow *)ud);
}

static void
on_tab_visit_committed(KajoWebTab *tab G_GNUC_UNUSED, const gchar *uri, const gchar *title, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!w->is_private && w->storage)
        web_storage_record_visit(w->storage, uri, title);
}

static gboolean
on_tab_scheme_action(KajoWebTab *tab, const gchar *uri, gpointer ud)
{
    return kajo_web_win_handle_scheme_action(ud, tab, uri);
}

static void
on_tab_hover_target(KajoWebTab *tab, const gchar *uri, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!w || !w->hover_hud || tab != kajo_web_win_active_tab(w))
        return;
    if (uri && *uri) {
        gtk_label_set_text(GTK_LABEL(w->hover_hud), uri);
        gtk_widget_set_visible(w->hover_hud, TRUE);
    } else {
        gtk_widget_set_visible(w->hover_hud, FALSE);
    }
}

static gboolean
on_tab_fullscreen_changed(KajoWebTab *tab, gboolean enter, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w && tab == kajo_web_win_active_tab(w))
        kajo_web_win_set_html5_fullscreen(w, enter);
    return FALSE;
}

static void
on_tab_find_state(KajoWebTab *tab, gint total, gboolean found, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!w || !w->find_count_lbl || !w->find_bar || tab != kajo_web_win_active_tab(w))
        return;
    w->find_total = MAX(0, total);
    if (total < 0) {
        w->find_pos = 0;
        gtk_label_set_text(GTK_LABEL(w->find_count_lbl), "");
        gtk_widget_remove_css_class(w->find_bar, "not-found");
    } else if (!found || total == 0) {
        w->find_pos = 0;
        gtk_label_set_text(GTK_LABEL(w->find_count_lbl), "0 / 0");
        gtk_widget_add_css_class(w->find_bar, "not-found");
    } else {
        if (w->find_pos < 1 || w->find_pos > total)
            w->find_pos = 1;
        g_autofree gchar *cnt = g_strdup_printf("%d / %d", w->find_pos, total);
        gtk_label_set_text(GTK_LABEL(w->find_count_lbl), cnt);
        gtk_widget_remove_css_class(w->find_bar, "not-found");
    }
}

void
kajo_web_win_select_tab(KajoWebWindow *w, guint64 tab_id)
{
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &tab_id);
    if (!t)
        return;
    if (w->omni_ac)
        web_omnibar_ac_dismiss(w->omni_ac);
    if (w->hover_hud)
        gtk_widget_set_visible(w->hover_hud, FALSE);
    if (w->html5_fullscreen)
        kajo_web_win_set_html5_fullscreen(w, FALSE);
    if (web_tab_is_suspended(t))
        web_tab_wake(t, w->cfg);
    web_model_activate(w->model, tab_id, g_get_monotonic_time());
    g_autofree gchar *name = g_strdup_printf("tab-%" G_GUINT64_FORMAT, tab_id);
    gtk_stack_set_visible_child_name(GTK_STACK(w->web_stack), name);
    kajo_web_win_sync_chrome(w);
    web_tab_rail_rebuild(w->rail, w->model);
}

guint64
kajo_web_win_add_tab_full(KajoWebWindow *w,
                          const gchar   *uri,
                          const gchar   *title,
                          const gchar   *group,
                          gboolean       pinned,
                          gboolean       suspended,
                          gboolean       defer_load,
                          gboolean       background,
                          gboolean       skip_rail_rebuild)
{
    const KajoWebCfg *cfg = w->cfg;
    const gchar *target = uri;
    gboolean explicit_uri = (uri && *uri);
    if (!explicit_uri) {
        if (cfg && cfg->new_tab_page == KAJO_WEB_NEWTAB_BLANK)
            target = "about:blank";
        else if (cfg && cfg->new_tab_page == KAJO_WEB_NEWTAB_CUSTOM &&
                 cfg->new_tab_custom_url && *cfg->new_tab_custom_url)
            target = cfg->new_tab_custom_url;
        else
            target = "kajo://newtab";
    }

    guint64 prev_active = w->model->active_id;
    KajoWebTabMeta *meta = web_model_add_tab(w->model, target,
                                             (title && *title) ? title : target,
                                             group, pinned, g_get_monotonic_time());
    if (background && prev_active != 0)
        w->model->active_id = prev_active;

    KajoWebTabCallbacks cbs = {
        .on_changed = kajo_web_win_on_tab_changed,
        .on_progress = on_tab_progress,
        .on_new_tab_requested = on_tab_new_requested,
        .on_permission_requested = on_tab_permission_requested,
        .on_media_detected = on_tab_media_detected,
        .on_visit_committed = on_tab_visit_committed,
        .on_scheme_action = on_tab_scheme_action,
        .on_hover_target = on_tab_hover_target,
        .on_fullscreen_changed = on_tab_fullscreen_changed,
        .on_find_state = on_tab_find_state,
    };

    KajoWebTab *tab = NULL;
    if (suspended) {
        meta->suspended = TRUE;
        tab = web_tab_new_suspended(meta->id, w->web_ctx, w->session, w->adblock,
                                    target, (title && *title) ? title : target, &cbs, w);
    } else {
        tab = web_tab_new(meta->id, w->web_ctx, w->session, w->adblock, cfg, &cbs, w);
    }
    g_hash_table_insert(w->tabs, u64_key(meta->id), tab);

    g_autofree gchar *sname = g_strdup_printf("tab-%" G_GUINT64_FORMAT, meta->id);
    gtk_stack_add_named(GTK_STACK(w->web_stack), web_tab_get_widget(tab), sname);

    if (!suspended) {
        gboolean is_http = g_str_has_prefix(target, "http://") || g_str_has_prefix(target, "https://");
        if (defer_load && is_http) {
            /* Deferred until initial adblock barrier finishes (~16ms) */
        } else {
            if (is_http && web_adblock_is_enabled(w->adblock) && !web_adblock_is_initial_load_done(w->adblock))
                w->initial_load_unfiltered = TRUE;
            web_tab_load_uri(tab, target);
        }
    }

    if (!background) {
        if (!skip_rail_rebuild)
            kajo_web_win_select_tab(w, meta->id);
        else {
            web_model_activate(w->model, meta->id, g_get_monotonic_time());
            gtk_stack_set_visible_child_name(GTK_STACK(w->web_stack), sname);
            kajo_web_win_sync_chrome(w);
        }
        if (!explicit_uri && kajo_web_win_is_blank_uri(target)) {
            w->updating_omni = TRUE;
            gtk_editable_set_text(GTK_EDITABLE(w->omni_entry), "");
            w->updating_omni = FALSE;
            schedule_focus_omnibar(w);
        }
    } else if (!skip_rail_rebuild) {
        kajo_web_win_sync_chrome(w);
        web_tab_rail_rebuild(w->rail, w->model);
    }

    return meta->id;
}

static void
add_and_activate_tab(KajoWebWindow *w, const gchar *uri, gboolean background)
{
    kajo_web_win_add_tab_full(w, uri, NULL, NULL, FALSE, FALSE, FALSE, background, FALSE);
}

void
kajo_web_win_new_tab(KajoWebWindow *w, const gchar *uri, gboolean background)
{
    if (w)
        add_and_activate_tab(w, uri, background);
}

void
kajo_web_win_close_tab(KajoWebWindow *w, guint64 tab_id)
{
    KajoWebTab *tab = g_hash_table_lookup(w->tabs, &tab_id);
    if (!tab)
        return;
    if (w->deferred_initial_tab_id == tab_id)
        w->deferred_initial_tab_id = 0;
    gtk_stack_remove(GTK_STACK(w->web_stack), web_tab_get_widget(tab));
    g_hash_table_remove(w->tabs, &tab_id);
    web_model_close_tab(w->model, tab_id);

    if (w->model->tabs->len == 0) {
        add_and_activate_tab(w, NULL, FALSE);
    } else if (w->model->active_id) {
        kajo_web_win_select_tab(w, w->model->active_id);
    }
}

/* ─── Top Chrome Signals ─── */

void
kajo_web_win_toggle_zen_topbar(KajoWebWindow *w)
{
    w->top_autohide = !w->top_autohide;
    gtk_widget_set_visible(w->header_handle, !w->top_autohide);
    gtk_widget_set_visible(w->top_zen_trigger, w->top_autohide);
}

static void
mutate_set_rail_mode(KajoWebCfg *draft, gpointer ud)
{
    draft->rail_mode = (KajoWebRailMode)GPOINTER_TO_INT(ud);
}

static void on_btn_back(GtkButton *b G_GNUC_UNUSED, gpointer ud)    { KajoWebTab *t = kajo_web_win_active_tab(ud); if (t) web_tab_go_back(t); }
static void on_btn_fwd(GtkButton *b G_GNUC_UNUSED, gpointer ud)     { KajoWebTab *t = kajo_web_win_active_tab(ud); if (t) web_tab_go_forward(t); }

void
kajo_web_win_toggle_pin_rail(KajoWebWindow *w)
{
    KajoWebRailMode cur = w->cfg ? w->cfg->rail_mode : KAJO_WEB_RAIL_AUTOHIDE;
    KajoWebRailMode next = (cur == KAJO_WEB_RAIL_PINNED)
        ? ((w->last_unpinned_rail_mode != KAJO_WEB_RAIL_PINNED)
               ? w->last_unpinned_rail_mode
               : KAJO_WEB_RAIL_COMPACT)
        : KAJO_WEB_RAIL_PINNED;
    kajo_config_store_update_web(NULL, mutate_set_rail_mode, GINT_TO_POINTER((gint)next), NULL);
}

static void on_btn_pin(GtkButton *b G_GNUC_UNUSED, gpointer ud)     { kajo_web_win_toggle_pin_rail(ud); }
static void on_btn_shield(GtkButton *b G_GNUC_UNUSED, gpointer ud)  { kajo_web_win_show_shield_popover(ud); }
static void on_btn_media(GtkButton *b G_GNUC_UNUSED, gpointer ud)   { kajo_web_win_show_media_popover(ud); }
static void on_btn_dl(GtkButton *b G_GNUC_UNUSED, gpointer ud)      { kajo_web_win_show_downloads_popover(ud); }
static void on_btn_prof(GtkButton *b G_GNUC_UNUSED, gpointer ud)    { kajo_web_win_show_profile_popover(ud); }
static void on_btn_menu(GtkButton *b G_GNUC_UNUSED, gpointer ud)    { kajo_web_win_show_menu_popover(ud); }
static void on_btn_close(GtkButton *b G_GNUC_UNUSED, gpointer ud)   { gtk_window_close(((KajoWebWindow *)ud)->win); }

static void
on_btn_reload(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTab *t = kajo_web_win_active_tab(ud);
    if (!t)
        return;
    if (web_tab_is_loading(t))
        web_tab_stop(t);
    else
        web_tab_reload(t, FALSE);
}

static void
on_perm_allow(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->pending_perm) {
        webkit_permission_request_allow(w->pending_perm);
        g_clear_object(&w->pending_perm);
    }
    gtk_widget_set_visible(w->perm_bar, FALSE);
}

static void
on_perm_deny(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->pending_perm) {
        webkit_permission_request_deny(w->pending_perm);
        g_clear_object(&w->pending_perm);
    }
    gtk_widget_set_visible(w->perm_bar, FALSE);
}

static gboolean
any_popover_visible(KajoWebWindow *w)
{
    return (w->pop_shield    && gtk_widget_get_visible(w->pop_shield)) ||
           (w->pop_media     && gtk_widget_get_visible(w->pop_media)) ||
           (w->pop_downloads && gtk_widget_get_visible(w->pop_downloads)) ||
           (w->pop_profile   && gtk_widget_get_visible(w->pop_profile)) ||
           (w->pop_menu      && gtk_widget_get_visible(w->pop_menu));
}

static gboolean
on_zen_hide_timeout(gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!w->top_autohide || w->window_fullscreen || w->html5_fullscreen) {
        w->zen_hide_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (any_popover_visible(w) || (w->omni_entry && gtk_widget_has_focus(w->omni_entry)))
        return G_SOURCE_CONTINUE;
    w->zen_hide_timer_id = 0;
    gtk_widget_set_visible(w->header_handle, FALSE);
    return G_SOURCE_REMOVE;
}

static void
on_top_zen_enter(GtkEventControllerMotion *c G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->zen_hide_timer_id) {
        g_source_remove(w->zen_hide_timer_id);
        w->zen_hide_timer_id = 0;
    }
    if (w->top_autohide && !w->window_fullscreen && !w->html5_fullscreen)
        gtk_widget_set_visible(w->header_handle, TRUE);
}

static void
on_top_bar_leave(GtkEventControllerMotion *c G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!w->top_autohide || w->window_fullscreen || w->html5_fullscreen)
        return;
    if (w->zen_hide_timer_id)
        g_source_remove(w->zen_hide_timer_id);
    w->zen_hide_timer_id = g_timeout_add(350, on_zen_hide_timeout, w);
}

static void
on_downloads_changed(KajoWebDownloads *dl G_GNUC_UNUSED, guint active, guint total, gpointer ud)
{
    KajoWebWindow *w = ud;
    w->dl_popover_dirty = TRUE;
    if (total > 0) {
        g_autofree gchar *lbl = g_strdup_printf("↓ %u", active > 0 ? active : total);
        gtk_button_set_label(GTK_BUTTON(w->btn_downloads), lbl);
    } else {
        gtk_button_set_label(GTK_BUTTON(w->btn_downloads), "↓");
    }
    if (active > 0)
        gtk_widget_add_css_class(w->btn_downloads, "has-active");
    else
        gtk_widget_remove_css_class(w->btn_downloads, "has-active");
}

static void
on_session_download_started(WebKitNetworkSession *s G_GNUC_UNUSED, WebKitDownload *dl, gpointer ud)
{
    KajoWebWindow *w = ud;
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    web_downloads_track_webkit(w->downloads, dl, cfg);
}

static void
on_sync_status(const gchar *status G_GNUC_UNUSED, const gchar *email, gboolean connected, gpointer ud)
{
    KajoWebWindow *w = ud;
    w->scheme_ctx.sync_enabled = connected;
    if (email && *email) {
        KajoWebProfileRegistry *reg = web_profile_manager_get_registry(w->pm);
        KajoWebProfileInfo *p = reg ? web_profile_registry_find(reg, w->profile_id) : NULL;
        if (p) {
            g_free(p->google_email);
            p->google_email = g_strdup(email);
            p->sync_enabled = connected;
            web_profile_manager_save(w->pm, NULL);
        }
    }
}

static void
on_sync_open_url(const gchar *url, gpointer ud)
{
    add_and_activate_tab((KajoWebWindow *)ud, url, FALSE);
}

/* ─── Config Hot-Reload & Idle Tab Suspension ─── */

static void
apply_web_config_live(KajoWebWindow *w, const KajoWebCfg *cfg)
{
    if (!w || !cfg)
        return;
    const KajoWebCfg *new_ref = kajo_web_cfg_ref(cfg);
    kajo_web_cfg_unref(w->cfg);
    w->cfg = new_ref;
    if (cfg->rail_mode != KAJO_WEB_RAIL_PINNED)
        w->last_unpinned_rail_mode = cfg->rail_mode;

    web_profile_manager_apply_config(w->pm, cfg);
    web_adblock_apply_config(w->adblock, cfg);
    web_tab_rail_apply_config(w->rail, cfg);
    web_downloads_apply_config(w->downloads, cfg);
    apply_profile_border_accent(w, cfg);

    GtkSettings *gs = gtk_settings_get_default();
    if (gs)
        g_object_set(gs, "gtk-application-prefer-dark-theme", cfg->color_scheme != KAJO_WEB_SCHEME_LIGHT, NULL);

    /* Docked pinned mode vs body_overlay mode + compact spacer side placement.
     * In COMPACT mode, rail->root sits on body_overlay at x=0 (left) or x=W (right),
     * directly covering compact_spacer (0..44px) and expanding over viewport_overlay
     * with ZERO dead void and ZERO page overlap when collapsed. */
    GtkWidget *spacer_w = web_tab_rail_get_compact_spacer(w->rail);
    if (spacer_w && gtk_widget_get_parent(spacer_w) == w->body_hbox) {
        gtk_box_reorder_child_after(GTK_BOX(w->body_hbox), spacer_w,
                                    (cfg->rail_side == KAJO_WEB_SIDE_RIGHT) ? w->viewport_overlay : NULL);
    }

    GtkWidget *rail_w = web_tab_rail_get_widget(w->rail);
    GtkWidget *parent = gtk_widget_get_parent(rail_w);
    if (cfg->rail_mode == KAJO_WEB_RAIL_PINNED) {
        if (parent == w->body_overlay) {
            gtk_overlay_remove_overlay(GTK_OVERLAY(w->body_overlay), rail_w);
            if (cfg->rail_side == KAJO_WEB_SIDE_RIGHT)
                gtk_box_append(GTK_BOX(w->body_hbox), rail_w);
            else
                gtk_box_prepend(GTK_BOX(w->body_hbox), rail_w);
        } else if (parent == w->body_hbox) {
            gtk_box_reorder_child_after(GTK_BOX(w->body_hbox), rail_w,
                                        (cfg->rail_side == KAJO_WEB_SIDE_RIGHT) ? w->viewport_overlay : NULL);
        }
    } else {
        if (parent == w->body_hbox) {
            gtk_box_remove(GTK_BOX(w->body_hbox), rail_w);
            gtk_overlay_add_overlay(GTK_OVERLAY(w->body_overlay), rail_w);
        }
    }

    w->top_autohide = (cfg->top_bar_mode == KAJO_WEB_TOPBAR_AUTOHIDE);
    kajo_web_win_sync_fullscreen_chrome(w);

    GHashTableIter iter;
    gpointer val = NULL;
    g_hash_table_iter_init(&iter, w->tabs);
    while (g_hash_table_iter_next(&iter, NULL, &val))
        web_tab_apply_config(val, cfg);

    kajo_web_win_sync_chrome(w);
    web_tab_rail_rebuild(w->rail, w->model);
}

static void
on_config_changed(KajoConfigStore *store G_GNUC_UNUSED, KajoConfigDomain dom G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    apply_web_config_live(w, cfg);
}

static gboolean
on_idle_suspend_tick(gpointer ud)
{
    KajoWebWindow *w = ud;
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    if (!cfg || cfg->suspend_idle_min <= 0 || w->model->tabs->len <= 1)
        return G_SOURCE_CONTINUE;

    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < w->model->tabs->len; i++) {
        KajoWebTabMeta *m = g_ptr_array_index(w->model->tabs, i);
        if (web_model_can_suspend(w->model, m, cfg->suspend_idle_min, now)) {
            KajoWebTab *t = g_hash_table_lookup(w->tabs, &m->id);
            if (t && !web_tab_is_suspended(t)) {
                web_tab_suspend(t);
                m->suspended = TRUE;
                web_tab_rail_update_tab(w->rail, w->model, m->id);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static void
on_window_is_active(GtkWindow *win, GParamSpec *ps G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!gtk_window_is_active(win))
        web_tab_rail_focus_lost(w->rail);
}

static void
on_window_destroy(GtkWidget *widget G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->cfg_sub_id) {
        kajo_config_store_unsubscribe(NULL, w->cfg_sub_id);
        w->cfg_sub_id = 0;
    }
    if (w->suspend_timer_id) {
        g_source_remove(w->suspend_timer_id);
        w->suspend_timer_id = 0;
    }
    if (w->sync_chrome_idle_id) {
        g_source_remove(w->sync_chrome_idle_id);
        w->sync_chrome_idle_id = 0;
    }
    if (w->focus_omni_idle_id) {
        g_source_remove(w->focus_omni_idle_id);
        w->focus_omni_idle_id = 0;
    }
    if (w->deferred_load_timer_id) {
        g_source_remove(w->deferred_load_timer_id);
        w->deferred_load_timer_id = 0;
    }
    if (w->zen_hide_timer_id) {
        g_source_remove(w->zen_hide_timer_id);
        w->zen_hide_timer_id = 0;
    }
    if (w->dl_refresh_timer_id) {
        g_source_remove(w->dl_refresh_timer_id);
        w->dl_refresh_timer_id = 0;
    }
    if (!w->is_private && w->storage && w->model) {
        if (!w->cfg || w->cfg->restore_session)
            web_storage_save_session(w->storage, w->model);
    }
    if (w->pending_perm) {
        webkit_permission_request_deny(w->pending_perm);
        g_clear_object(&w->pending_perm);
    }
    if (w->border_css) {
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(w->border_css));
        g_clear_object(&w->border_css);
    }
    if (w->session)
        g_signal_handlers_disconnect_by_data(w->session, w);

    g_clear_pointer(&w->pop_shield, gtk_widget_unparent);
    g_clear_pointer(&w->pop_media, gtk_widget_unparent);
    g_clear_pointer(&w->pop_downloads, gtk_widget_unparent);
    g_clear_pointer(&w->pop_profile, gtk_widget_unparent);
    g_clear_pointer(&w->pop_menu, gtk_widget_unparent);

    g_clear_pointer(&w->omni_ac, web_omnibar_ac_free);
    g_clear_pointer(&w->tabs, g_hash_table_unref);
    g_clear_pointer(&w->rail, web_tab_rail_free);
    g_clear_pointer(&w->downloads, web_downloads_free);
    g_clear_pointer(&w->sync, web_sync_free);
    g_clear_pointer(&w->adblock, web_adblock_free);
    g_clear_pointer(&w->storage, web_storage_free);
    g_clear_pointer(&w->model, web_model_free);
    g_clear_pointer(&w->cfg, kajo_web_cfg_unref);
    g_clear_object(&w->session);
    g_clear_object(&w->web_ctx);
    g_free(w->sync_err_msg);
    g_free(w->cached_bm_uri);
    g_free(w->profile_id);
    g_free(w->border_css_class);
    g_free(w);
}

static void
fire_deferred_initial_load(KajoWebWindow *w)
{
    if (w->deferred_load_timer_id) {
        g_source_remove(w->deferred_load_timer_id);
        w->deferred_load_timer_id = 0;
    }
    if (!w->deferred_initial_tab_id)
        return;
    guint64 tid = w->deferred_initial_tab_id;
    w->deferred_initial_tab_id = 0;
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &tid);
    KajoWebTabMeta *m = web_model_find_tab(w->model, tid);
    if (t && m && !web_tab_is_suspended(t) && m->uri && *m->uri)
        web_tab_load_uri(t, m->uri);
}

static gboolean
on_deferred_initial_load_timeout(gpointer ud)
{
    KajoWebWindow *w = ud;
    w->deferred_load_timer_id = 0;
    fire_deferred_initial_load(w);
    return G_SOURCE_REMOVE;
}

static void
on_adblock_ready(KajoWebAdblock *ab G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (web_adblock_is_initial_load_done(w->adblock)) {
        fire_deferred_initial_load(w);
        if (w->initial_load_unfiltered && web_adblock_is_ready(w->adblock)) {
            w->initial_load_unfiltered = FALSE;
            GHashTableIter iter;
            gpointer val = NULL;
            g_hash_table_iter_init(&iter, w->tabs);
            while (g_hash_table_iter_next(&iter, NULL, &val)) {
                KajoWebTab *t = val;
                if (web_tab_is_suspended(t))
                    continue;
                const gchar *u = web_tab_get_uri(t);
                if (u && (g_str_has_prefix(u, "http://") || g_str_has_prefix(u, "https://")))
                    web_tab_reload(t, FALSE);
            }
        }
    }
    kajo_web_win_sync_chrome(w);
}

GtkWindow *
kajo_web_window_new(GtkApplication                  *app,
                    KajoWebProfileManager           *pm,
                    const gchar                     *profile_id,
                    gboolean                         is_private,
                    gboolean                         hw_disabled_override,
                    const gchar *const              *initial_urls,
                    gint                             n_urls,
                    const KajoWebWindowAppCallbacks *app_cbs,
                    gpointer                         app_ud)
{
    KajoWebWindow *w = g_new0(KajoWebWindow, 1);
    w->pm = pm;
    w->last_progress_frac = -1.0;
    w->last_tls_kind = -1;
    w->profile_id = web_profile_sanitize_id(profile_id ? profile_id : "default");
    w->is_private = is_private;
    w->hw_disabled_override = hw_disabled_override;
    if (app_cbs)
        w->app_cbs = *app_cbs;
    w->app_ud = app_ud;

    w->cfg = kajo_config_store_acquire_web(NULL);
    w->last_unpinned_rail_mode = (w->cfg && w->cfg->rail_mode != KAJO_WEB_RAIL_PINNED)
        ? w->cfg->rail_mode : KAJO_WEB_RAIL_COMPACT;

    KajoWebProfileRegistry *reg = web_profile_manager_get_registry(pm);
    KajoWebProfileInfo *pinfo = reg ? web_profile_registry_find(reg, w->profile_id) : NULL;
    if (!pinfo && reg && reg->profiles->len > 0) {
        pinfo = g_ptr_array_index(reg->profiles, 0);
        g_free(w->profile_id);
        w->profile_id = g_strdup(pinfo->id);
    }

    g_autofree gchar *data_dir  = web_profile_manager_get_data_dir(pm, w->profile_id);
    g_autofree gchar *cache_dir = web_profile_manager_get_cache_dir(pm, w->profile_id);

    w->web_ctx = webkit_web_context_new();
    webkit_web_context_set_cache_model(w->web_ctx, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    w->session = web_profile_manager_get_session(pm, w->profile_id, is_private, w->cfg);
    g_signal_connect(w->session, "download-started", G_CALLBACK(on_session_download_started), w);

    if (!is_private) {
        w->storage = web_storage_new(data_dir);
        w->sync = web_sync_new(w->profile_id, w->storage, on_sync_status, on_sync_open_url, w);
    }
    w->adblock = web_adblock_new(cache_dir, is_private ? NULL : data_dir, w->cfg, on_adblock_ready, w);
    w->downloads = web_downloads_new(on_downloads_changed, w);
    w->model = web_model_new();
    w->tabs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)web_tab_free);

    w->scheme_ctx.profile_name = pinfo ? pinfo->name : "DEFAULT";
    w->scheme_ctx.profile_email = pinfo ? pinfo->google_email : "";
    w->scheme_ctx.profile_accent = pinfo ? pinfo->accent_color : "#0078d4";
    w->scheme_ctx.sync_enabled = pinfo ? pinfo->sync_enabled : FALSE;
    w->scheme_ctx.is_private = is_private;
    w->scheme_ctx.hw_disabled_override = hw_disabled_override;
    w->scheme_ctx.storage = w->storage;
    w->scheme_ctx.adblock = w->adblock;
    g_autofree gchar *act_tok = g_uuid_string_random();
    g_strlcpy(w->scheme_ctx.action_token, act_tok, sizeof(w->scheme_ctx.action_token));
    web_schemes_register(w->web_ctx, &w->scheme_ctx);

    GtkWidget *win = gtk_application_window_new(app);
    w->win = GTK_WINDOW(win);
    web_downloads_set_parent(w->downloads, w->win);
    gtk_window_set_decorated(w->win, FALSE);
    gtk_window_set_default_size(w->win, 1280, 820);
    gtk_widget_add_css_class(win, "shell-web-window");
    if (is_private)
        gtk_widget_add_css_class(win, "web-private-window");
    g_object_set_data(G_OBJECT(win), "kajo-web-win", w);
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), w);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(on_window_is_active), w);

    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(kajo_web_win_on_key_pressed), w);
    gtk_widget_add_controller(win, kc);

    w->root_overlay = gtk_overlay_new();
    gtk_window_set_child(w->win, w->root_overlay);

    w->main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_overlay_set_child(GTK_OVERLAY(w->root_overlay), w->main_vbox);

    /* Zen Top Bar Hover Trigger */
    w->top_zen_trigger = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(w->top_zen_trigger, -1, 4);
    gtk_widget_set_valign(w->top_zen_trigger, GTK_ALIGN_START);
    gtk_widget_add_css_class(w->top_zen_trigger, "web-top-zen-trigger");
    gtk_widget_set_visible(w->top_zen_trigger, FALSE);
    GtkEventController *zm = gtk_event_controller_motion_new();
    g_signal_connect(zm, "enter", G_CALLBACK(on_top_zen_enter), w);
    gtk_widget_add_controller(w->top_zen_trigger, zm);
    gtk_overlay_add_overlay(GTK_OVERLAY(w->root_overlay), w->top_zen_trigger);

    /* 1. 42px Top Control Bar */
    w->header_handle = gtk_window_handle_new();
    GtkEventController *hm = gtk_event_controller_motion_new();
    g_signal_connect(hm, "enter", G_CALLBACK(on_top_zen_enter), w);
    g_signal_connect(hm, "leave", G_CALLBACK(on_top_bar_leave), w);
    gtk_widget_add_controller(w->header_handle, hm);
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(hdr, "web-header-bar");

    w->btn_back = gtk_button_new_with_label("◀");
    w->btn_fwd = gtk_button_new_with_label("▶");
    w->btn_reload = gtk_button_new_with_label("↻");
    w->btn_pin_rail = gtk_button_new_with_label("◧");
    GtkWidget *nav_btns[] = { w->btn_back, w->btn_fwd, w->btn_reload, w->btn_pin_rail };
    for (gsize i = 0; i < G_N_ELEMENTS(nav_btns); i++) {
        gtk_widget_set_valign(nav_btns[i], GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(nav_btns[i], "web-chrome-btn");
        gtk_box_append(GTK_BOX(hdr), nav_btns[i]);
    }
    gtk_widget_set_tooltip_text(w->btn_back, "Back (Alt+Left)");
    gtk_widget_set_tooltip_text(w->btn_fwd, "Forward (Alt+Right)");
    gtk_widget_set_tooltip_text(w->btn_reload, "Reload / Stop (Ctrl+R / Esc)");
    gtk_widget_set_tooltip_text(w->btn_pin_rail, "Pin / Unpin Vertical Tab Rail");
    g_signal_connect(w->btn_back, "clicked", G_CALLBACK(on_btn_back), w);
    g_signal_connect(w->btn_fwd, "clicked", G_CALLBACK(on_btn_fwd), w);
    g_signal_connect(w->btn_reload, "clicked", G_CALLBACK(on_btn_reload), w);
    g_signal_connect(w->btn_pin_rail, "clicked", G_CALLBACK(on_btn_pin), w);

    /* Center Unified Omnibar (progress bar overlays bottom edge -> zero 2px bounce) */
    GtkWidget *omni_overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(omni_overlay, TRUE);
    gtk_widget_set_valign(omni_overlay, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(omni_overlay, "web-omnibar-box");

    GtkWidget *omni_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(omni_row, GTK_ALIGN_CENTER);
    w->omni_tls = gtk_label_new("❖");
    gtk_widget_set_valign(w->omni_tls, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->omni_tls, "web-omni-tls");

    w->omni_entry = gtk_entry_new();
    gtk_widget_set_hexpand(w->omni_entry, TRUE);
    gtk_widget_set_valign(w->omni_entry, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->omni_entry, "web-omni-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->omni_entry), "Search or enter URL (!arch, !gh, !mdn)...");

    w->btn_zoom = gtk_button_new_with_label("100%");
    gtk_widget_set_valign(w->btn_zoom, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(w->btn_zoom, "Reset Page Zoom (Ctrl+0)");
    gtk_widget_add_css_class(w->btn_zoom, "web-omni-zoom");
    gtk_widget_set_visible(w->btn_zoom, FALSE);

    w->btn_bookmark = gtk_button_new_with_label("☆");
    gtk_widget_set_valign(w->btn_bookmark, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(w->btn_bookmark, "Bookmark This Page (Ctrl+D)");
    gtk_widget_add_css_class(w->btn_bookmark, "web-omni-star");

    w->btn_shield = gtk_button_new_with_label("🛡 ON");
    gtk_widget_set_valign(w->btn_shield, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->btn_shield, "web-omni-shield");
    g_signal_connect(w->btn_shield, "clicked", G_CALLBACK(on_btn_shield), w);

    gtk_box_append(GTK_BOX(omni_row), w->omni_tls);
    gtk_box_append(GTK_BOX(omni_row), w->omni_entry);
    gtk_box_append(GTK_BOX(omni_row), w->btn_zoom);
    gtk_box_append(GTK_BOX(omni_row), w->btn_bookmark);
    gtk_box_append(GTK_BOX(omni_row), w->btn_shield);
    gtk_overlay_set_child(GTK_OVERLAY(omni_overlay), omni_row);

    w->omni_progress = gtk_progress_bar_new();
    gtk_widget_set_valign(w->omni_progress, GTK_ALIGN_END);
    gtk_widget_set_can_target(w->omni_progress, FALSE);
    gtk_widget_add_css_class(w->omni_progress, "web-omni-progress");
    gtk_widget_set_visible(w->omni_progress, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(omni_overlay), w->omni_progress);
    gtk_box_append(GTK_BOX(hdr), omni_overlay);

    /* Right Minimalist Cluster */
    w->chip_sw = gtk_label_new("⚠ SW RENDER");
    gtk_widget_set_valign(w->chip_sw, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->chip_sw, "web-chip-warn");
    gtk_widget_set_visible(w->chip_sw, FALSE);
    gtk_box_append(GTK_BOX(hdr), w->chip_sw);

    w->chip_priv = gtk_label_new("🕶 PRIVATE");
    gtk_widget_set_valign(w->chip_priv, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->chip_priv, "web-chip-priv");
    gtk_widget_set_visible(w->chip_priv, is_private);
    gtk_box_append(GTK_BOX(hdr), w->chip_priv);

    w->btn_media = gtk_button_new_with_label("🎬");
    gtk_widget_set_valign(w->btn_media, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(w->btn_media, "Media Stream Sniffer & Downloader / Open in Kajo Player");
    gtk_widget_add_css_class(w->btn_media, "web-chrome-btn");
    g_signal_connect(w->btn_media, "clicked", G_CALLBACK(on_btn_media), w);
    gtk_box_append(GTK_BOX(hdr), w->btn_media);

    w->btn_downloads = gtk_button_new_with_label("↓");
    gtk_widget_set_valign(w->btn_downloads, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(w->btn_downloads, "Downloads (Ctrl+J)");
    gtk_widget_add_css_class(w->btn_downloads, "web-chip-dl");
    g_signal_connect(w->btn_downloads, "clicked", G_CALLBACK(on_btn_dl), w);
    gtk_box_append(GTK_BOX(hdr), w->btn_downloads);

    w->btn_profile = gtk_button_new();
    gtk_widget_set_valign(w->btn_profile, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(w->btn_profile, "Profile Switcher & Drive Sync (Ctrl+Shift+M)");
    gtk_widget_add_css_class(w->btn_profile, "web-profile-chip");
    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_valign(pbox, GTK_ALIGN_CENTER);
    w->profile_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(w->profile_dot, 8, 8);
    gtk_widget_set_valign(w->profile_dot, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->profile_dot, "web-profile-dot");
    w->profile_lbl = gtk_label_new(is_private ? "PRIVATE" : (pinfo ? pinfo->name : "DEFAULT"));
    gtk_widget_set_valign(w->profile_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pbox), w->profile_dot);
    gtk_box_append(GTK_BOX(pbox), w->profile_lbl);
    gtk_button_set_child(GTK_BUTTON(w->btn_profile), pbox);
    g_signal_connect(w->btn_profile, "clicked", G_CALLBACK(on_btn_prof), w);
    gtk_box_append(GTK_BOX(hdr), w->btn_profile);

    w->btn_menu = gtk_button_new_with_label("☰");
    gtk_widget_set_valign(w->btn_menu, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(w->btn_menu, "Menu (Command Palette, DevTools, Settings, Float)");
    gtk_widget_add_css_class(w->btn_menu, "web-chrome-btn");
    g_signal_connect(w->btn_menu, "clicked", G_CALLBACK(on_btn_menu), w);
    gtk_box_append(GTK_BOX(hdr), w->btn_menu);

    GtkWidget *b_cls = gtk_button_new_with_label("✕");
    gtk_widget_set_valign(b_cls, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b_cls, "Close Window");
    gtk_widget_add_css_class(b_cls, "web-close-btn");
    g_signal_connect(b_cls, "clicked", G_CALLBACK(on_btn_close), w);
    gtk_box_append(GTK_BOX(hdr), b_cls);

    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(w->header_handle), hdr);
    gtk_box_append(GTK_BOX(w->main_vbox), w->header_handle);

    /* 2. Body: body_overlay wraps body_hbox so rail->trigger & rail->root anchor at
     * x=0 (left) or x=W (right), sitting directly over compact_spacer (0..44px) in
     * COMPACT mode and floating cleanly over the page when expanded. */
    w->body_overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(w->body_overlay, TRUE);
    gtk_widget_set_vexpand(w->body_overlay, TRUE);
    gtk_box_append(GTK_BOX(w->main_vbox), w->body_overlay);

    w->body_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(w->body_hbox, TRUE);
    gtk_widget_set_vexpand(w->body_hbox, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(w->body_overlay), w->body_hbox);

    KajoWebTabRailCallbacks rcbs = { 0 };
    kajo_web_win_fill_rail_callbacks(&rcbs);
    w->rail = web_tab_rail_new(&rcbs, w);
    gtk_box_append(GTK_BOX(w->body_hbox), web_tab_rail_get_compact_spacer(w->rail));

    w->viewport_overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(w->viewport_overlay, TRUE);
    gtk_widget_set_vexpand(w->viewport_overlay, TRUE);
    gtk_box_append(GTK_BOX(w->body_hbox), w->viewport_overlay);

    w->viewport_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_overlay_set_child(GTK_OVERLAY(w->viewport_overlay), w->viewport_vbox);

    /* Non-Modal Permission Infobar */
    w->perm_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(w->perm_bar, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->perm_bar, "web-perm-bar");
    gtk_widget_set_visible(w->perm_bar, FALSE);
    w->perm_lbl = gtk_label_new("");
    gtk_widget_set_hexpand(w->perm_lbl, TRUE);
    gtk_widget_set_halign(w->perm_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(w->perm_lbl, GTK_ALIGN_CENTER);
    GtkWidget *b_pa = gtk_button_new_with_label("ALLOW");
    gtk_widget_set_valign(b_pa, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b_pa, "web-btn-primary");
    g_signal_connect(b_pa, "clicked", G_CALLBACK(on_perm_allow), w);
    GtkWidget *b_pd = gtk_button_new_with_label("DENY");
    gtk_widget_set_valign(b_pd, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b_pd, "web-btn-secondary");
    g_signal_connect(b_pd, "clicked", G_CALLBACK(on_perm_deny), w);
    gtk_box_append(GTK_BOX(w->perm_bar), w->perm_lbl);
    gtk_box_append(GTK_BOX(w->perm_bar), b_pa);
    gtk_box_append(GTK_BOX(w->perm_bar), b_pd);
    gtk_box_append(GTK_BOX(w->viewport_vbox), w->perm_bar);

    w->web_stack = gtk_stack_new();
    gtk_widget_set_hexpand(w->web_stack, TRUE);
    gtk_widget_set_vexpand(w->web_stack, TRUE);
    gtk_box_append(GTK_BOX(w->viewport_vbox), w->web_stack);

    /* Body Overlay: Trigger Strip + Vertical Rail (aligned to full window edge) */
    gtk_overlay_add_overlay(GTK_OVERLAY(w->body_overlay), web_tab_rail_get_trigger(w->rail));
    gtk_overlay_add_overlay(GTK_OVERLAY(w->body_overlay), web_tab_rail_get_widget(w->rail));

    /* Viewport Overlay: Bottom-Left Hover URL HUD + Top-Right Find Bar + Ctrl+K Palette */
    w->hover_hud = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(w->hover_hud), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(w->hover_hud), 72);
    gtk_widget_set_halign(w->hover_hud, GTK_ALIGN_START);
    gtk_widget_set_valign(w->hover_hud, GTK_ALIGN_END);
    gtk_widget_set_margin_start(w->hover_hud, 6);
    gtk_widget_set_margin_bottom(w->hover_hud, 6);
    gtk_widget_set_can_target(w->hover_hud, FALSE);
    gtk_widget_add_css_class(w->hover_hud, "web-hover-hud");
    gtk_widget_set_visible(w->hover_hud, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), w->hover_hud);

    w->find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(w->find_bar, GTK_ALIGN_END);
    gtk_widget_set_valign(w->find_bar, GTK_ALIGN_START);
    gtk_widget_set_margin_end(w->find_bar, 14);
    gtk_widget_set_margin_top(w->find_bar, 10);
    gtk_widget_add_css_class(w->find_bar, "web-find-bar");
    gtk_widget_set_visible(w->find_bar, FALSE);
    w->find_entry = gtk_entry_new();
    gtk_widget_set_valign(w->find_entry, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->find_entry, "web-find-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->find_entry), "Find in page...");
    w->find_count_lbl = gtk_label_new("");
    gtk_widget_set_valign(w->find_count_lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->find_count_lbl, "web-find-count");
    GtkWidget *bf_p = gtk_button_new_with_label("▲");
    GtkWidget *bf_n = gtk_button_new_with_label("▼");
    GtkWidget *bf_c = gtk_button_new_with_label("✕");
    gtk_widget_set_valign(bf_p, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(bf_n, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(bf_c, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(bf_p, "web-chrome-btn");
    gtk_widget_add_css_class(bf_n, "web-chrome-btn");
    gtk_widget_add_css_class(bf_c, "web-close-btn");
    kajo_web_win_wire_omnibar_and_find(w, bf_p, bf_n, bf_c);
    gtk_box_append(GTK_BOX(w->find_bar), w->find_entry);
    gtk_box_append(GTK_BOX(w->find_bar), w->find_count_lbl);
    gtk_box_append(GTK_BOX(w->find_bar), bf_p);
    gtk_box_append(GTK_BOX(w->find_bar), bf_n);
    gtk_box_append(GTK_BOX(w->find_bar), bf_c);
    gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), w->find_bar);

    KajoWebPaletteCallbacks pcbs = { 0 };
    kajo_web_win_fill_palette_callbacks(&pcbs);
    w->palette = web_palette_new(&pcbs, w);
    gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), w->palette);

    kajo_web_win_setup_popovers(w);
    apply_web_config_live(w, w->cfg);

    /* Open initial URLs or restore saved session (non-active session tabs suspended) */
    gboolean delay_net_load = web_adblock_is_enabled(w->adblock) && !web_adblock_is_initial_load_done(w->adblock);
    gboolean opened_any = FALSE;
    if (initial_urls && n_urls > 0) {
        for (gint i = 0; i < n_urls; i++) {
            g_autofree gchar *u = web_model_resolve_input(
                initial_urls[i],
                w->cfg ? w->cfg->search_engine : KAJO_WEB_SEARCH_DUCKDUCKGO,
                w->cfg ? w->cfg->search_custom_url : "");
            gboolean defer_this = (i == 0) && delay_net_load &&
                (g_str_has_prefix(u, "http://") || g_str_has_prefix(u, "https://"));
            guint64 tid = kajo_web_win_add_tab_full(w, u, NULL, NULL, FALSE, FALSE, defer_this, i > 0, TRUE);
            if (defer_this)
                w->deferred_initial_tab_id = tid;
            opened_any = TRUE;
        }
        web_tab_rail_rebuild(w->rail, w->model);
    } else if (!is_private && w->storage && (!w->cfg || w->cfg->restore_session)) {
        g_autoptr(KajoWebModel) tmp = web_model_new();
        if (web_storage_load_session(w->storage, tmp)) {
            guint act_idx = 0;
            for (guint i = 0; i < tmp->tabs->len; i++) {
                const KajoWebTabMeta *t = g_ptr_array_index(tmp->tabs, i);
                if (t->id == tmp->active_id) {
                    act_idx = i;
                    break;
                }
            }
            guint64 act_id = 0;
            for (guint i = 0; i < tmp->tabs->len; i++) {
                const KajoWebTabMeta *t = g_ptr_array_index(tmp->tabs, i);
                gboolean is_active = (i == act_idx);
                gboolean defer_this = is_active && delay_net_load && t->uri &&
                    (g_str_has_prefix(t->uri, "http://") || g_str_has_prefix(t->uri, "https://"));
                guint64 added_id = kajo_web_win_add_tab_full(w, t->uri, t->title, t->group, t->pinned,
                                                             !is_active, defer_this, !is_active, TRUE);
                if (is_active) {
                    act_id = added_id;
                    if (defer_this)
                        w->deferred_initial_tab_id = added_id;
                }
            }
            if (act_id)
                kajo_web_win_select_tab(w, act_id);
            else
                web_tab_rail_rebuild(w->rail, w->model);
            opened_any = (w->model->tabs->len > 0);
        }
    }
    if (!opened_any)
        add_and_activate_tab(w, NULL, FALSE);

    if (w->deferred_initial_tab_id)
        w->deferred_load_timer_id = g_timeout_add(250, on_deferred_initial_load_timeout, w);

    w->cfg_sub_id = kajo_config_store_subscribe(NULL, KAJO_CONFIG_DOMAIN_WEB, on_config_changed, w, NULL);
    w->suspend_timer_id = g_timeout_add_seconds(60, on_idle_suspend_tick, w);
    return w->win;
}

void
kajo_web_window_open_uri(GtkWindow *win, const gchar *uri, gboolean new_tab)
{
    KajoWebWindow *w = win ? g_object_get_data(G_OBJECT(win), "kajo-web-win") : NULL;
    if (!w || !uri || !*uri)
        return;
    if (new_tab) {
        add_and_activate_tab(w, uri, FALSE);
    } else {
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t) web_tab_load_uri(t, uri);
    }
}

const gchar *
kajo_web_window_get_profile_id(GtkWindow *win)
{
    KajoWebWindow *w = win ? g_object_get_data(G_OBJECT(win), "kajo-web-win") : NULL;
    return w ? w->profile_id : NULL;
}

gboolean
kajo_web_window_is_private(GtkWindow *win)
{
    KajoWebWindow *w = win ? g_object_get_data(G_OBJECT(win), "kajo-web-win") : NULL;
    return w ? w->is_private : FALSE;
}
