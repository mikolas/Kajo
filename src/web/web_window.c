#include "web_window_private.h"
#include "../compositor/compositor.h"
#include "../common/spawn_detached.h"
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

void
kajo_web_win_sync_chrome(KajoWebWindow *w)
{
    if (!w)
        return;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);

    gtk_widget_set_sensitive(w->btn_back, t && web_tab_can_go_back(t));
    gtk_widget_set_sensitive(w->btn_fwd, t && web_tab_can_go_forward(t));
    gtk_button_set_label(GTK_BUTTON(w->btn_reload), (t && web_tab_is_loading(t)) ? "✕" : "↻");

    const gchar *uri = t ? web_tab_get_uri(t) : "kajo://newtab";
    if (!gtk_widget_has_focus(w->omni_entry)) {
        const gchar *cur_txt = gtk_editable_get_text(GTK_EDITABLE(w->omni_entry));
        if (g_strcmp0(cur_txt, uri) != 0) {
            w->updating_omni = TRUE;
            gtk_editable_set_text(GTK_EDITABLE(w->omni_entry), uri);
            w->updating_omni = FALSE;
        }
    }

    if (g_str_has_prefix(uri, "kajo://"))
        gtk_label_set_text(GTK_LABEL(w->omni_tls), "❖");
    else if (g_str_has_prefix(uri, "https://"))
        gtk_label_set_text(GTK_LABEL(w->omni_tls), "🔒");
    else
        gtk_label_set_text(GTK_LABEL(w->omni_tls), "⚠");

    gdouble prog = t ? web_tab_get_progress(t) : 0.0;
    gboolean loading = t && web_tab_is_loading(t) && prog > 0.0 && prog < 1.0;
    gtk_widget_set_visible(w->omni_progress, loading);
    if (loading)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->omni_progress), prog);

    gboolean ab_on = web_adblock_is_enabled(w->adblock);
    gtk_button_set_label(GTK_BUTTON(w->btn_shield), ab_on ? "🛡 ON" : "🛡 OFF");
    if (ab_on)
        gtk_widget_remove_css_class(w->btn_shield, "off");
    else
        gtk_widget_add_css_class(w->btn_shield, "off");

    gboolean is_sw = w->hw_disabled_override || (cfg && cfg->hardware_acceleration == KAJO_WEB_HW_NEVER);
    gtk_widget_set_visible(w->chip_sw, is_sw);
    gtk_widget_set_visible(w->chip_priv, w->is_private);

    const GPtrArray *streams = t ? web_tab_get_media_streams(t) : NULL;
    guint n_streams = streams ? streams->len : 0;
    if (n_streams > 0) {
        g_autofree gchar *ml = g_strdup_printf("⬇ %u", n_streams);
        gtk_button_set_label(GTK_BUTTON(w->btn_media), ml);
    } else {
        gtk_button_set_label(GTK_BUTTON(w->btn_media), "⬇");
    }

    if (t) {
        g_autofree gchar *wt = g_strdup_printf("%s — Kajo Web", web_tab_get_title(t));
        if (g_strcmp0(gtk_window_get_title(w->win), wt) != 0)
            gtk_window_set_title(w->win, wt);
    }
}

static void add_and_activate_tab(KajoWebWindow *w, const gchar *uri, gboolean background);

static void
on_tab_progress(KajoWebTab *tab, gdouble progress, gboolean loading, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (tab != kajo_web_win_active_tab(w))
        return;
    gboolean show = loading && progress > 0.0 && progress < 1.0;
    gtk_widget_set_visible(w->omni_progress, show);
    if (show)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->omni_progress), progress);
    gtk_button_set_label(GTK_BUTTON(w->btn_reload), loading ? "✕" : "↻");
    gtk_widget_set_sensitive(w->btn_back, web_tab_can_go_back(tab));
    gtk_widget_set_sensitive(w->btn_fwd, web_tab_can_go_forward(tab));
}

static void
on_tab_changed(KajoWebTab *tab, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTabMeta *meta = web_model_find_tab(w->model, web_tab_get_id(tab));
    gboolean rail_dirty = FALSE;
    if (meta) {
        const gchar *nt = web_tab_get_title(tab);
        const gchar *nu = web_tab_get_uri(tab);
        gboolean na = web_tab_is_playing_audio(tab);
        gboolean nm = web_tab_is_muted(tab);
        gboolean ns = web_tab_is_suspended(tab);
        if (g_strcmp0(meta->title, nt) != 0) {
            g_free(meta->title);
            meta->title = g_strdup(nt);
            rail_dirty = TRUE;
        }
        if (g_strcmp0(meta->uri, nu) != 0) {
            g_free(meta->uri);
            meta->uri = g_strdup(nu);
            rail_dirty = TRUE;
        }
        if (meta->audible != na || meta->muted != nm || meta->suspended != ns) {
            meta->audible = na;
            meta->muted = nm;
            meta->suspended = ns;
            rail_dirty = TRUE;
        }
    }
    if (tab == kajo_web_win_active_tab(w))
        kajo_web_win_sync_chrome(w);
    if (rail_dirty)
        web_tab_rail_rebuild(w->rail, w->model);
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

static void
select_tab_by_id(KajoWebWindow *w, guint64 tab_id)
{
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &tab_id);
    if (!t)
        return;
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    if (web_tab_is_suspended(t))
        web_tab_wake(t, cfg);
    web_model_activate(w->model, tab_id, g_get_monotonic_time());
    g_autofree gchar *name = g_strdup_printf("tab-%" G_GUINT64_FORMAT, tab_id);
    gtk_stack_set_visible_child_name(GTK_STACK(w->web_stack), name);
    kajo_web_win_sync_chrome(w);
    web_tab_rail_rebuild(w->rail, w->model);
}

static void
add_and_activate_tab(KajoWebWindow *w, const gchar *uri, gboolean background)
{
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    const gchar *target = uri;
    if (!target || !*target) {
        if (cfg && cfg->new_tab_page == KAJO_WEB_NEWTAB_BLANK)
            target = "about:blank";
        else if (cfg && cfg->new_tab_page == KAJO_WEB_NEWTAB_CUSTOM &&
                 cfg->new_tab_custom_url && *cfg->new_tab_custom_url)
            target = cfg->new_tab_custom_url;
        else
            target = "kajo://newtab";
    }

    guint64 prev_active = w->model->active_id;
    KajoWebTabMeta *meta = web_model_add_tab(w->model, target, target, NULL, FALSE, g_get_monotonic_time());
    if (background && prev_active != 0)
        w->model->active_id = prev_active;
    KajoWebTabCallbacks cbs = {
        .on_changed = on_tab_changed,
        .on_progress = on_tab_progress,
        .on_new_tab_requested = on_tab_new_requested,
        .on_permission_requested = on_tab_permission_requested,
        .on_media_detected = on_tab_media_detected,
        .on_visit_committed = on_tab_visit_committed,
    };
    KajoWebTab *tab = web_tab_new(meta->id, w->web_ctx, w->session, w->adblock, cfg, &cbs, w);
    g_hash_table_insert(w->tabs, u64_key(meta->id), tab);

    g_autofree gchar *sname = g_strdup_printf("tab-%" G_GUINT64_FORMAT, meta->id);
    gtk_stack_add_named(GTK_STACK(w->web_stack), web_tab_get_widget(tab), sname);
    web_tab_load_uri(tab, target);

    if (!background) {
        select_tab_by_id(w, meta->id);
    } else {
        kajo_web_win_sync_chrome(w);
        web_tab_rail_rebuild(w->rail, w->model);
    }
}

static void
close_tab_by_id(KajoWebWindow *w, guint64 tab_id)
{
    KajoWebTab *tab = g_hash_table_lookup(w->tabs, &tab_id);
    if (!tab)
        return;
    gtk_stack_remove(GTK_STACK(w->web_stack), web_tab_get_widget(tab));
    g_hash_table_remove(w->tabs, &tab_id);
    web_model_close_tab(w->model, tab_id);

    if (w->model->tabs->len == 0) {
        add_and_activate_tab(w, NULL, FALSE);
    } else if (w->model->active_id) {
        select_tab_by_id(w, w->model->active_id);
    }
}

/* ─── Rail & Palette Callbacks ─── */

static void on_rail_select(guint64 id, gpointer ud) { select_tab_by_id(ud, id); }
static void on_rail_close(guint64 id, gpointer ud)  { close_tab_by_id(ud, id); }
static void on_rail_new(gpointer ud)                { add_and_activate_tab(ud, NULL, FALSE); }

static void
on_rail_mute(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &id);
    if (t) {
        web_tab_set_muted(t, !web_tab_is_muted(t));
        on_tab_changed(t, w);
    }
}

static void
on_rail_undo(gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTabMeta *restored = web_model_reopen_closed(w->model, g_get_monotonic_time());
    if (!restored)
        return;
    g_autofree gchar *uri = g_strdup(restored->uri);
    web_model_close_tab(w->model, restored->id);
    if (w->model->closed_stack->len > 0)
        g_ptr_array_remove_index(w->model->closed_stack, w->model->closed_stack->len - 1);
    add_and_activate_tab(w, uri, FALSE);
}

static void
on_pal_open_uri(const gchar *uri, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t)
        web_tab_load_uri(t, uri);
}

static void
toggle_zen_topbar(KajoWebWindow *w)
{
    w->top_autohide = !w->top_autohide;
    gtk_widget_set_visible(w->header_handle, !w->top_autohide);
    gtk_widget_set_visible(w->top_zen_trigger, w->top_autohide);
}

static void
on_pal_cmd(const gchar *cmd, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (g_strcmp0(cmd, "toggle-rail") == 0)
        web_tab_rail_toggle_reveal(w->rail);
    else if (g_strcmp0(cmd, "toggle-zen") == 0)
        toggle_zen_topbar(w);
    else if (g_strcmp0(cmd, "open-settings") == 0)
        kajo_web_win_open_settings();
    else if (g_strcmp0(cmd, "open-picker") == 0 && w->app_cbs.on_open_picker)
        w->app_cbs.on_open_picker(w->app_ud);
    else if (g_strcmp0(cmd, "toggle-inspector") == 0) {
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t) web_tab_toggle_inspector(t);
    }
}

/* ─── Top Chrome Signals ─── */

static void on_btn_back(GtkButton *b G_GNUC_UNUSED, gpointer ud)    { KajoWebTab *t = kajo_web_win_active_tab(ud); if (t) web_tab_go_back(t); }
static void on_btn_fwd(GtkButton *b G_GNUC_UNUSED, gpointer ud)     { KajoWebTab *t = kajo_web_win_active_tab(ud); if (t) web_tab_go_forward(t); }
static void on_btn_pin(GtkButton *b G_GNUC_UNUSED, gpointer ud)     { web_tab_rail_toggle_reveal(((KajoWebWindow *)ud)->rail); }
static void on_btn_shield(GtkButton *b G_GNUC_UNUSED, gpointer ud)  { kajo_web_win_show_shield_popover(ud); }
static void on_btn_media(GtkButton *b G_GNUC_UNUSED, gpointer ud)   { kajo_web_win_show_media_popover(ud); }
static void on_btn_dl(GtkButton *b G_GNUC_UNUSED, gpointer ud)      { kajo_web_win_show_downloads_popover(ud); }
static void on_btn_prof(GtkButton *b G_GNUC_UNUSED, gpointer ud)    { kajo_web_win_show_profile_popover(ud); }
static void on_btn_pal(GtkButton *b G_GNUC_UNUSED, gpointer ud)     { KajoWebWindow *w = ud; web_palette_open(w->palette, w->model, w->storage); }
static void on_btn_set(GtkButton *b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) { kajo_web_win_open_settings(); }
static void on_btn_float(GtkButton *b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) { shell_compositor_toggle_floating(); }
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
on_omni_activate(GtkEntry *entry, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (!t)
        return;
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    g_autofree gchar *resolved = web_model_resolve_input(
        gtk_editable_get_text(GTK_EDITABLE(entry)),
        cfg ? cfg->search_engine : KAJO_WEB_SEARCH_DUCKDUCKGO,
        cfg ? cfg->search_custom_url : "");
    web_tab_load_uri(t, resolved);
    if (web_tab_get_view(t))
        gtk_widget_grab_focus(GTK_WIDGET(web_tab_get_view(t)));
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

static void on_find_changed(GtkEditable *ed, gpointer ud) { KajoWebTab *t = kajo_web_win_active_tab(ud); if (t) web_tab_find_search(t, gtk_editable_get_text(ed)); }
static void on_find_next(GtkButton *b G_GNUC_UNUSED, gpointer ud) { KajoWebTab *t = kajo_web_win_active_tab(ud); if (t) web_tab_find_next(t); }
static void on_find_prev(GtkButton *b G_GNUC_UNUSED, gpointer ud) { KajoWebTab *t = kajo_web_win_active_tab(ud); if (t) web_tab_find_prev(t); }
static void on_find_close(GtkButton *b G_GNUC_UNUSED, gpointer ud) { KajoWebWindow *w = ud; gtk_widget_set_visible(w->find_bar, FALSE); KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_find_clear(t); }

static void
on_top_zen_enter(GtkEventControllerMotion *c G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->top_autohide)
        gtk_widget_set_visible(w->header_handle, TRUE);
}

static void
on_downloads_changed(KajoWebDownloads *dl G_GNUC_UNUSED, guint active, guint total, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_widget_set_visible(w->btn_downloads, total > 0);
    g_autofree gchar *lbl = g_strdup_printf("↓ %u", active > 0 ? active : total);
    gtk_button_set_label(GTK_BUTTON(w->btn_downloads), lbl);
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

static void
toggle_bookmark_active(KajoWebWindow *w)
{
    if (w->is_private || !w->storage)
        return;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (!t)
        return;
    const gchar *uri = web_tab_get_uri(t);
    if (!uri || g_str_has_prefix(uri, "kajo://"))
        return;
    g_autofree gchar *existing_uuid = NULL;
    if (web_storage_is_bookmarked(w->storage, uri, &existing_uuid)) {
        web_storage_delete_bookmark(w->storage, existing_uuid);
    } else {
        KajoWebBookmark *bm = web_bookmark_new(NULL, web_tab_get_title(t), uri, "", 0, FALSE);
        web_storage_upsert_bookmark(w->storage, bm);
        web_bookmark_free(bm);
    }
    if (w->sync) {
        g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
        web_sync_schedule_debounced(w->sync, cfg);
    }
}

/* ─── Keyboard Shortcuts ─── */

static gboolean
on_key_pressed(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
               guint kc G_GNUC_UNUSED, GdkModifierType st, gpointer ud)
{
    KajoWebWindow *w = ud;
    gboolean ctrl = (st & GDK_CONTROL_MASK) != 0;
    gboolean shift = (st & GDK_SHIFT_MASK) != 0;
    gboolean alt = (st & GDK_ALT_MASK) != 0;

    if (keyval == GDK_KEY_Escape) {
        web_palette_close(w->palette);
        web_tab_rail_escape(w->rail);
        if (gtk_widget_get_visible(w->find_bar))
            on_find_close(NULL, w);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12 || (ctrl && shift && (keyval == GDK_KEY_I || keyval == GDK_KEY_i))) {
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t) web_tab_toggle_inspector(t);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11 || (ctrl && shift && (keyval == GDK_KEY_U || keyval == GDK_KEY_u))) {
        toggle_zen_topbar(w);
        return TRUE;
    }
    if (ctrl && !shift && !alt) {
        switch (keyval) {
        case GDK_KEY_t: add_and_activate_tab(w, NULL, FALSE); return TRUE;
        case GDK_KEY_w: if (w->model->active_id) close_tab_by_id(w, w->model->active_id); return TRUE;
        case GDK_KEY_b: web_tab_rail_toggle_reveal(w->rail); return TRUE;
        case GDK_KEY_l: gtk_widget_grab_focus(w->omni_entry); gtk_editable_select_region(GTK_EDITABLE(w->omni_entry), 0, -1); return TRUE;
        case GDK_KEY_k: web_palette_open(w->palette, w->model, w->storage); return TRUE;
        case GDK_KEY_r: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_reload(t, FALSE); return TRUE; }
        case GDK_KEY_f: gtk_widget_set_visible(w->find_bar, TRUE); gtk_widget_grab_focus(w->find_entry); return TRUE;
        case GDK_KEY_j: kajo_web_win_show_downloads_popover(w); return TRUE;
        case GDK_KEY_d: toggle_bookmark_active(w); return TRUE;
        case GDK_KEY_m: if (w->model->active_id) on_rail_mute(w->model->active_id, w); return TRUE;
        case GDK_KEY_comma: kajo_web_win_open_settings(); return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_set_zoom(t, web_tab_get_zoom(t) + 0.1); return TRUE; }
        case GDK_KEY_minus: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_set_zoom(t, web_tab_get_zoom(t) - 0.1); return TRUE; }
        case GDK_KEY_0: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_set_zoom(t, 1.0); return TRUE; }
        case GDK_KEY_Tab: { guint64 nxt = web_model_cycle_mru(w->model, TRUE); if (nxt) select_tab_by_id(w, nxt); return TRUE; }
        default: break;
        }
    }
    if (ctrl && shift) {
        switch (keyval) {
        case GDK_KEY_T: case GDK_KEY_t: on_rail_undo(w); return TRUE;
        case GDK_KEY_R: case GDK_KEY_r: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_reload(t, TRUE); return TRUE; }
        case GDK_KEY_N: case GDK_KEY_n: if (w->app_cbs.on_open_profile) w->app_cbs.on_open_profile(w->profile_id, TRUE, NULL, w->app_ud); return TRUE;
        case GDK_KEY_M: case GDK_KEY_m: kajo_web_win_show_profile_popover(w); return TRUE;
        case GDK_KEY_ISO_Left_Tab: case GDK_KEY_Tab: { guint64 nxt = web_model_cycle_mru(w->model, FALSE); if (nxt) select_tab_by_id(w, nxt); return TRUE; }
        default: break;
        }
    }
    if (alt && !ctrl) {
        if (keyval == GDK_KEY_Left) { on_btn_back(NULL, w); return TRUE; }
        if (keyval == GDK_KEY_Right) { on_btn_fwd(NULL, w); return TRUE; }
        if (keyval == GDK_KEY_d) { gtk_widget_grab_focus(w->omni_entry); gtk_editable_select_region(GTK_EDITABLE(w->omni_entry), 0, -1); return TRUE; }
        if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
            guint idx = keyval - GDK_KEY_1;
            if (idx < w->model->tabs->len) {
                const KajoWebTabMeta *t = g_ptr_array_index(w->model->tabs, idx);
                select_tab_by_id(w, t->id);
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* ─── Config Hot-Reload & Idle Tab Suspension ─── */

static void
apply_web_config_live(KajoWebWindow *w, const KajoWebCfg *cfg)
{
    if (!w || !cfg)
        return;
    web_profile_manager_apply_config(w->pm, cfg);
    web_adblock_apply_config(w->adblock, cfg);
    web_tab_rail_apply_config(w->rail, cfg);
    apply_profile_border_accent(w, cfg);

    /* Docked pinned mode vs overlay mode + compact spacer side placement */
    GtkWidget *spacer_w = web_tab_rail_get_compact_spacer(w->rail);
    if (spacer_w && gtk_widget_get_parent(spacer_w) == w->body_hbox) {
        gtk_box_reorder_child_after(GTK_BOX(w->body_hbox), spacer_w,
                                    (cfg->rail_side == KAJO_WEB_SIDE_RIGHT) ? w->viewport_overlay : NULL);
    }

    GtkWidget *rail_w = web_tab_rail_get_widget(w->rail);
    GtkWidget *parent = gtk_widget_get_parent(rail_w);
    if (cfg->rail_mode == KAJO_WEB_RAIL_PINNED) {
        if (parent == w->viewport_overlay) {
            gtk_overlay_remove_overlay(GTK_OVERLAY(w->viewport_overlay), rail_w);
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
            gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), rail_w);
        }
    }

    w->top_autohide = (cfg->top_bar_mode == KAJO_WEB_TOPBAR_AUTOHIDE);
    gtk_widget_set_visible(w->header_handle, !w->top_autohide);
    gtk_widget_set_visible(w->top_zen_trigger, w->top_autohide);

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
                web_tab_rail_rebuild(w->rail, w->model);
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
    if (!w->is_private && w->storage && w->model) {
        g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
        if (!cfg || cfg->restore_session)
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

    g_clear_pointer(&w->tabs, g_hash_table_unref);
    g_clear_pointer(&w->rail, web_tab_rail_free);
    g_clear_pointer(&w->downloads, web_downloads_free);
    g_clear_pointer(&w->sync, web_sync_free);
    g_clear_pointer(&w->adblock, web_adblock_free);
    g_clear_pointer(&w->storage, web_storage_free);
    g_clear_pointer(&w->model, web_model_free);
    g_clear_object(&w->session);
    g_clear_object(&w->web_ctx);
    g_free(w->profile_id);
    g_free(w->border_css_class);
    g_free(w);
}

static void
on_adblock_ready(KajoWebAdblock *ab G_GNUC_UNUSED, gpointer ud)
{
    kajo_web_win_sync_chrome((KajoWebWindow *)ud);
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
    w->profile_id = web_profile_sanitize_id(profile_id ? profile_id : "default");
    w->is_private = is_private;
    w->hw_disabled_override = hw_disabled_override;
    if (app_cbs)
        w->app_cbs = *app_cbs;
    w->app_ud = app_ud;

    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);

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
    w->session = web_profile_manager_get_session(pm, w->profile_id, is_private, cfg);
    g_signal_connect(w->session, "download-started", G_CALLBACK(on_session_download_started), w);

    if (!is_private) {
        w->storage = web_storage_new(data_dir);
        w->sync = web_sync_new(w->profile_id, w->storage, on_sync_status, on_sync_open_url, w);
    }
    w->adblock = web_adblock_new(cache_dir, on_adblock_ready, w);
    web_adblock_apply_config(w->adblock, cfg);
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
    web_schemes_register(w->web_ctx, &w->scheme_ctx);

    GtkWidget *win = gtk_application_window_new(app);
    w->win = GTK_WINDOW(win);
    gtk_window_set_decorated(w->win, FALSE);
    gtk_window_set_default_size(w->win, 1280, 820);
    gtk_widget_add_css_class(win, "shell-settings-window");
    gtk_widget_add_css_class(win, "shell-web-window");
    g_object_set_data(G_OBJECT(win), "kajo-web-win", w);
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), w);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(on_window_is_active), w);

    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), w);
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

    /* 1. Minimalist 34px Top Control Bar */
    w->header_handle = gtk_window_handle_new();
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(hdr, "settings-header-bar");
    gtk_widget_add_css_class(hdr, "web-header-bar");

    GtkWidget *brand = gtk_label_new("🌐 KAJO WEB");
    gtk_widget_set_valign(brand, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(brand, "web-brand-mark");
    gtk_box_append(GTK_BOX(hdr), brand);

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
    g_signal_connect(w->btn_back, "clicked", G_CALLBACK(on_btn_back), w);
    g_signal_connect(w->btn_fwd, "clicked", G_CALLBACK(on_btn_fwd), w);
    g_signal_connect(w->btn_reload, "clicked", G_CALLBACK(on_btn_reload), w);
    g_signal_connect(w->btn_pin_rail, "clicked", G_CALLBACK(on_btn_pin), w);

    /* Center Unified Omnibar */
    GtkWidget *omni_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(omni_box, TRUE);
    gtk_widget_set_valign(omni_box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(omni_box, "web-omnibar-box");

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
    g_signal_connect(w->omni_entry, "activate", G_CALLBACK(on_omni_activate), w);

    w->btn_shield = gtk_button_new_with_label("🛡 ON");
    gtk_widget_set_valign(w->btn_shield, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->btn_shield, "web-omni-shield");
    g_signal_connect(w->btn_shield, "clicked", G_CALLBACK(on_btn_shield), w);

    gtk_box_append(GTK_BOX(omni_row), w->omni_tls);
    gtk_box_append(GTK_BOX(omni_row), w->omni_entry);
    gtk_box_append(GTK_BOX(omni_row), w->btn_shield);
    gtk_box_append(GTK_BOX(omni_box), omni_row);

    w->omni_progress = gtk_progress_bar_new();
    gtk_widget_add_css_class(w->omni_progress, "web-omni-progress");
    gtk_widget_set_visible(w->omni_progress, FALSE);
    gtk_box_append(GTK_BOX(omni_box), w->omni_progress);
    gtk_box_append(GTK_BOX(hdr), omni_box);

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

    w->btn_media = gtk_button_new_with_label("⬇");
    gtk_widget_set_valign(w->btn_media, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(w->btn_media, "Media Stream Sniffer & Downloader / Open in Kajo Player");
    gtk_widget_add_css_class(w->btn_media, "web-chrome-btn");
    g_signal_connect(w->btn_media, "clicked", G_CALLBACK(on_btn_media), w);
    gtk_box_append(GTK_BOX(hdr), w->btn_media);

    w->btn_downloads = gtk_button_new_with_label("↓ 0");
    gtk_widget_set_valign(w->btn_downloads, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->btn_downloads, "web-chip-dl");
    gtk_widget_set_visible(w->btn_downloads, FALSE);
    g_signal_connect(w->btn_downloads, "clicked", G_CALLBACK(on_btn_dl), w);
    gtk_box_append(GTK_BOX(hdr), w->btn_downloads);

    w->btn_profile = gtk_button_new();
    gtk_widget_set_valign(w->btn_profile, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->btn_profile, "web-profile-chip");
    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_valign(pbox, GTK_ALIGN_CENTER);
    w->profile_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(w->profile_dot, 7, 7);
    gtk_widget_set_valign(w->profile_dot, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(w->profile_dot, "web-profile-dot");
    w->profile_lbl = gtk_label_new(is_private ? "PRIVATE" : (pinfo ? pinfo->name : "DEFAULT"));
    gtk_widget_set_valign(w->profile_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pbox), w->profile_dot);
    gtk_box_append(GTK_BOX(pbox), w->profile_lbl);
    gtk_button_set_child(GTK_BUTTON(w->btn_profile), pbox);
    g_signal_connect(w->btn_profile, "clicked", G_CALLBACK(on_btn_prof), w);
    gtk_box_append(GTK_BOX(hdr), w->btn_profile);

    GtkWidget *b_pal = gtk_button_new_with_label("⌘");
    gtk_widget_set_valign(b_pal, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b_pal, "Command & Tab Switcher (Ctrl+K)");
    gtk_widget_add_css_class(b_pal, "web-chrome-btn");
    g_signal_connect(b_pal, "clicked", G_CALLBACK(on_btn_pal), w);
    gtk_box_append(GTK_BOX(hdr), b_pal);

    GtkWidget *b_set = gtk_button_new_with_label("⚙");
    gtk_widget_set_valign(b_set, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b_set, "Kajo Settings → Kajo Web (Ctrl+,)");
    gtk_widget_add_css_class(b_set, "web-chrome-btn");
    g_signal_connect(b_set, "clicked", G_CALLBACK(on_btn_set), w);
    gtk_box_append(GTK_BOX(hdr), b_set);

    GtkWidget *b_flt = gtk_button_new_with_label("🗗");
    gtk_widget_set_valign(b_flt, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b_flt, "Float / Tile Window");
    gtk_widget_add_css_class(b_flt, "web-chrome-btn");
    g_signal_connect(b_flt, "clicked", G_CALLBACK(on_btn_float), w);
    gtk_box_append(GTK_BOX(hdr), b_flt);

    GtkWidget *b_cls = gtk_button_new_with_label("✕");
    gtk_widget_set_valign(b_cls, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b_cls, "settings-close-btn");
    g_signal_connect(b_cls, "clicked", G_CALLBACK(on_btn_close), w);
    gtk_box_append(GTK_BOX(hdr), b_cls);

    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(w->header_handle), hdr);
    gtk_box_append(GTK_BOX(w->main_vbox), w->header_handle);

    /* 2. Body: Vertical Autohide Rail + Viewport Overlay */
    w->body_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(w->body_hbox, TRUE);
    gtk_box_append(GTK_BOX(w->main_vbox), w->body_hbox);

    KajoWebTabRailCallbacks rcbs = {
        .on_select_tab = on_rail_select,
        .on_close_tab = on_rail_close,
        .on_toggle_mute = on_rail_mute,
        .on_new_tab = on_rail_new,
        .on_undo_close = on_rail_undo,
    };
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
    GtkWidget *b_pa = gtk_button_new_with_label("ALLOW");
    gtk_widget_add_css_class(b_pa, "action-btn-primary");
    g_signal_connect(b_pa, "clicked", G_CALLBACK(on_perm_allow), w);
    GtkWidget *b_pd = gtk_button_new_with_label("DENY");
    gtk_widget_add_css_class(b_pd, "files-nav-btn");
    g_signal_connect(b_pd, "clicked", G_CALLBACK(on_perm_deny), w);
    gtk_box_append(GTK_BOX(w->perm_bar), w->perm_lbl);
    gtk_box_append(GTK_BOX(w->perm_bar), b_pa);
    gtk_box_append(GTK_BOX(w->perm_bar), b_pd);
    gtk_box_append(GTK_BOX(w->viewport_vbox), w->perm_bar);

    w->web_stack = gtk_stack_new();
    gtk_widget_set_hexpand(w->web_stack, TRUE);
    gtk_widget_set_vexpand(w->web_stack, TRUE);
    gtk_box_append(GTK_BOX(w->viewport_vbox), w->web_stack);

    /* Overlay children: Trigger Strip + Vertical Rail + Find Bar + Ctrl+K Palette */
    gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), web_tab_rail_get_trigger(w->rail));
    gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), web_tab_rail_get_widget(w->rail));

    w->find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(w->find_bar, GTK_ALIGN_END);
    gtk_widget_set_valign(w->find_bar, GTK_ALIGN_END);
    gtk_widget_set_margin_end(w->find_bar, 14);
    gtk_widget_set_margin_bottom(w->find_bar, 14);
    gtk_widget_add_css_class(w->find_bar, "web-find-bar");
    gtk_widget_set_visible(w->find_bar, FALSE);
    w->find_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->find_entry), "Find in page...");
    g_signal_connect(w->find_entry, "changed", G_CALLBACK(on_find_changed), w);
    GtkWidget *bf_p = gtk_button_new_with_label("▲");
    GtkWidget *bf_n = gtk_button_new_with_label("▼");
    GtkWidget *bf_c = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(bf_p, "web-chrome-btn");
    gtk_widget_add_css_class(bf_n, "web-chrome-btn");
    gtk_widget_add_css_class(bf_c, "web-chrome-btn");
    g_signal_connect(bf_p, "clicked", G_CALLBACK(on_find_prev), w);
    g_signal_connect(bf_n, "clicked", G_CALLBACK(on_find_next), w);
    g_signal_connect(bf_c, "clicked", G_CALLBACK(on_find_close), w);
    gtk_box_append(GTK_BOX(w->find_bar), w->find_entry);
    gtk_box_append(GTK_BOX(w->find_bar), bf_p);
    gtk_box_append(GTK_BOX(w->find_bar), bf_n);
    gtk_box_append(GTK_BOX(w->find_bar), bf_c);
    gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), w->find_bar);

    KajoWebPaletteCallbacks pcbs = {
        .on_select_tab = on_rail_select,
        .on_open_uri = on_pal_open_uri,
        .on_run_command = on_pal_cmd,
    };
    w->palette = web_palette_new(&pcbs, w);
    gtk_overlay_add_overlay(GTK_OVERLAY(w->viewport_overlay), w->palette);

    kajo_web_win_setup_popovers(w);
    apply_web_config_live(w, cfg);

    /* Open initial URLs or restore saved session */
    gboolean opened_any = FALSE;
    if (initial_urls && n_urls > 0) {
        for (gint i = 0; i < n_urls; i++) {
            g_autofree gchar *u = web_model_resolve_input(
                initial_urls[i],
                cfg ? cfg->search_engine : KAJO_WEB_SEARCH_DUCKDUCKGO,
                cfg ? cfg->search_custom_url : "");
            add_and_activate_tab(w, u, i > 0);
            opened_any = TRUE;
        }
    } else if (!is_private && w->storage && (!cfg || cfg->restore_session)) {
        g_autoptr(KajoWebModel) tmp = web_model_new();
        if (web_storage_load_session(w->storage, tmp)) {
            guint64 act_idx = 0;
            for (guint i = 0; i < tmp->tabs->len; i++) {
                const KajoWebTabMeta *t = g_ptr_array_index(tmp->tabs, i);
                if (t->id == tmp->active_id)
                    act_idx = i;
                add_and_activate_tab(w, t->uri, TRUE);
                KajoWebTabMeta *added = g_ptr_array_index(w->model->tabs, w->model->tabs->len - 1);
                if (t->pinned)
                    web_model_set_pinned(w->model, added->id, TRUE);
            }
            if (act_idx < w->model->tabs->len) {
                const KajoWebTabMeta *act = g_ptr_array_index(w->model->tabs, (guint)act_idx);
                select_tab_by_id(w, act->id);
            }
            opened_any = (w->model->tabs->len > 0);
        }
    }
    if (!opened_any)
        add_and_activate_tab(w, NULL, FALSE);

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
