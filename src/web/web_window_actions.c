#include "web_window_private.h"

/* ─── Rail & Palette Action Handlers ─── */

static void on_rail_select(guint64 id, gpointer ud) { kajo_web_win_select_tab(ud, id); }
static void on_rail_close(guint64 id, gpointer ud)  { kajo_web_win_close_tab(ud, id); }
static void on_rail_new(gpointer ud)                { kajo_web_win_new_tab(ud, NULL, FALSE); }

static GdkTexture *
on_rail_get_favicon(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &id);
    return t ? web_tab_get_favicon(t) : NULL;
}

static void
on_rail_mute(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &id);
    if (t) {
        web_tab_set_muted(t, !web_tab_is_muted(t));
        kajo_web_win_on_tab_changed(t, w);
    }
}

static void
on_rail_pin(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTabMeta *m = web_model_find_tab(w->model, id);
    if (!m)
        return;
    web_model_set_pinned(w->model, id, !m->pinned);
    web_tab_rail_rebuild(w->rail, w->model);
}

static void
on_rail_reload(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &id);
    if (t) {
        if (web_tab_is_suspended(t))
            web_tab_wake(t, w->cfg);
        else
            web_tab_reload(t, FALSE);
    }
}

static void
on_rail_duplicate(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    const KajoWebTabMeta *m = web_model_find_tab(w->model, id);
    if (m && m->uri)
        kajo_web_win_add_tab_full(w, m->uri, m->title, m->group, FALSE, FALSE, FALSE, FALSE, FALSE);
}

static void
on_rail_copy_url(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    const KajoWebTabMeta *m = web_model_find_tab(w->model, id);
    if (m && m->uri) {
        GdkDisplay *dpy = gdk_display_get_default();
        GdkClipboard *cb = dpy ? gdk_display_get_clipboard(dpy) : NULL;
        if (cb)
            gdk_clipboard_set_text(cb, m->uri);
    }
}

static void
on_rail_suspend(guint64 id, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (id == w->model->active_id)
        return;
    KajoWebTabMeta *m = web_model_find_tab(w->model, id);
    KajoWebTab *t = g_hash_table_lookup(w->tabs, &id);
    if (t && m && !web_tab_is_suspended(t)) {
        web_tab_suspend(t);
        m->suspended = TRUE;
        web_tab_rail_update_tab(w->rail, w->model, id);
    }
}

static void
on_rail_close_others(guint64 keep_id, gpointer ud)
{
    KajoWebWindow *w = ud;
    g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    for (guint i = 0; i < w->model->tabs->len; i++) {
        const KajoWebTabMeta *m = g_ptr_array_index(w->model->tabs, i);
        if (m->id != keep_id && !m->pinned)
            g_array_append_val(ids, m->id);
    }
    for (guint i = 0; i < ids->len; i++)
        kajo_web_win_close_tab(w, g_array_index(ids, guint64, i));
}

static void
on_rail_close_below(guint64 pivot_id, gpointer ud)
{
    KajoWebWindow *w = ud;
    gboolean found = FALSE;
    g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    for (guint i = 0; i < w->model->tabs->len; i++) {
        const KajoWebTabMeta *m = g_ptr_array_index(w->model->tabs, i);
        if (found && !m->pinned)
            g_array_append_val(ids, m->id);
        else if (m->id == pivot_id)
            found = TRUE;
    }
    for (guint i = 0; i < ids->len; i++)
        kajo_web_win_close_tab(w, g_array_index(ids, guint64, i));
}

static void
on_rail_toggle_group(const gchar *group_name, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!group_name || !*group_name)
        return;
    gboolean cur = web_model_is_group_collapsed(w->model, group_name);
    web_model_set_group_collapsed(w->model, group_name, !cur);
    web_tab_rail_rebuild(w->rail, w->model);
}

static void
on_rail_reorder(guint64 tab_id, guint target_index, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (web_model_move_tab(w->model, tab_id, target_index))
        web_tab_rail_rebuild(w->rail, w->model);
}

static void
on_rail_undo(gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTabMeta *restored = web_model_reopen_closed(w->model, g_get_monotonic_time());
    if (!restored)
        return;
    g_autofree gchar *uri = g_strdup(restored->uri);
    g_autofree gchar *title = g_strdup(restored->title);
    g_autofree gchar *group = g_strdup(restored->group);
    gboolean pinned = restored->pinned;
    web_model_close_tab(w->model, restored->id);
    if (w->model->closed_stack->len > 0)
        g_ptr_array_remove_index(w->model->closed_stack, w->model->closed_stack->len - 1);
    kajo_web_win_add_tab_full(w, uri, title, group, pinned, FALSE, FALSE, FALSE, FALSE);
}

static void
on_pal_open_uri(const gchar *uri, gpointer ud)
{
    KajoWebWindow *w = ud;
    kajo_web_win_cancel_deferred_load(w);
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t) {
        const KajoWebCfg *cfg = w->cfg;
        g_autofree gchar *resolved = web_model_resolve_input(
            uri,
            cfg ? cfg->search_engine : KAJO_WEB_SEARCH_DUCKDUCKGO,
            cfg ? cfg->search_custom_url : "");
        web_tab_load_uri(t, resolved);
    }
}

static void
on_pal_cmd(const gchar *cmd, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (g_strcmp0(cmd, "toggle-rail") == 0)
        web_tab_rail_toggle_reveal(w->rail);
    else if (g_strcmp0(cmd, "toggle-zen") == 0)
        kajo_web_win_toggle_zen_topbar(w);
    else if (g_strcmp0(cmd, "toggle-fullscreen") == 0)
        kajo_web_win_toggle_fullscreen(w);
    else if (g_strcmp0(cmd, "open-find") == 0)
        kajo_web_win_open_find(w);
    else if (g_strcmp0(cmd, "print-page") == 0) {
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t)
            web_tab_print(t, w->win);
    } else if (g_strcmp0(cmd, "open-settings") == 0)
        kajo_web_win_open_settings();
    else if (g_strcmp0(cmd, "open-picker") == 0 && w->app_cbs.on_open_picker)
        w->app_cbs.on_open_picker(w->app_ud);
    else if (g_strcmp0(cmd, "toggle-inspector") == 0) {
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t)
            web_tab_toggle_inspector(t);
    }
}

/* ─── Omnibar, Bookmark & Find Bar Handlers ─── */

static void
toggle_bookmark_active(KajoWebWindow *w)
{
    if (w->is_private || !w->storage)
        return;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (!t)
        return;
    const gchar *uri = web_tab_get_uri(t);
    if (!uri || g_str_has_prefix(uri, "kajo://") || g_str_has_prefix(uri, "about:"))
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
    kajo_web_win_invalidate_bookmark_cache(w);
    kajo_web_win_sync_chrome(w);
}

static void
on_omni_click_pressed(GtkGestureClick *gest G_GNUC_UNUSED, gint n G_GNUC_UNUSED,
                      double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    w->omni_had_focus_on_press = gtk_widget_has_focus(w->omni_entry);
}

static void
on_omni_click_released(GtkGestureClick *gest G_GNUC_UNUSED, gint n G_GNUC_UNUSED,
                       double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (!w->omni_had_focus_on_press && gtk_widget_has_focus(w->omni_entry)) {
        gint s = 0, e = 0;
        if (!gtk_editable_get_selection_bounds(GTK_EDITABLE(w->omni_entry), &s, &e))
            gtk_editable_select_region(GTK_EDITABLE(w->omni_entry), 0, -1);
    }
}

static void
on_omni_changed(GtkEditable *ed, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->updating_omni || !w->omni_ac)
        return;
    web_omnibar_ac_schedule(w->omni_ac, gtk_editable_get_text(ed));
}

static void
on_omni_focus_leave(GtkEventControllerFocus *c G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->omni_ac)
        web_omnibar_ac_dismiss(w->omni_ac);
}

static gboolean
on_omni_key_pressed(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
                    guint kc G_GNUC_UNUSED, GdkModifierType st G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    return web_omnibar_ac_handle_key(w->omni_ac, keyval);
}

static void
on_omni_ac_select(const gchar *uri, gpointer ud)
{
    KajoWebWindow *w = ud;
    on_pal_open_uri(uri, w);
    kajo_web_win_focus_webview(w);
}

static void
on_omni_activate(GtkEntry *entry, gpointer ud)
{
    KajoWebWindow *w = ud;
    if (w->omni_ac)
        web_omnibar_ac_dismiss(w->omni_ac);
    kajo_web_win_cancel_deferred_load(w);
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (!t)
        return;
    const KajoWebCfg *cfg = w->cfg;
    g_autofree gchar *resolved = web_model_resolve_input(
        gtk_editable_get_text(GTK_EDITABLE(entry)),
        cfg ? cfg->search_engine : KAJO_WEB_SEARCH_DUCKDUCKGO,
        cfg ? cfg->search_custom_url : "");
    web_tab_load_uri(t, resolved);
    if (web_tab_get_view(t))
        gtk_widget_grab_focus(GTK_WIDGET(web_tab_get_view(t)));
}

static void
on_btn_zoom_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t) {
        web_tab_set_zoom(t, 1.0);
        kajo_web_win_sync_chrome(w);
    }
}

static void
on_btn_bookmark_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    toggle_bookmark_active(ud);
}

static void
on_find_changed(GtkEditable *ed, gpointer ud)
{
    KajoWebWindow *w = ud;
    const gchar *txt = gtk_editable_get_text(ed);
    w->find_pos = (txt && *txt) ? 1 : 0;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t)
        web_tab_find_search(t, txt);
}

static void
on_find_next(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (!t)
        return;
    if (w->find_total > 0) {
        w->find_pos = (w->find_pos % w->find_total) + 1;
        g_autofree gchar *cnt = g_strdup_printf("%d / %d", w->find_pos, w->find_total);
        gtk_label_set_text(GTK_LABEL(w->find_count_lbl), cnt);
    }
    web_tab_find_next(t);
}

static void
on_find_prev(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (!t)
        return;
    if (w->find_total > 0) {
        w->find_pos = (w->find_pos <= 1) ? w->find_total : (w->find_pos - 1);
        g_autofree gchar *cnt = g_strdup_printf("%d / %d", w->find_pos, w->find_total);
        gtk_label_set_text(GTK_LABEL(w->find_count_lbl), cnt);
    }
    web_tab_find_prev(t);
}

static gboolean
on_find_entry_key(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
                  guint kc G_GNUC_UNUSED, GdkModifierType st, gpointer ud)
{
    gboolean shift = (st & GDK_SHIFT_MASK) != 0;
    gboolean ctrl  = (st & GDK_CONTROL_MASK) != 0;
    guint k = gdk_keyval_to_lower(keyval);
    if (shift && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
        on_find_prev(NULL, ud);
        return TRUE;
    }
    if (keyval == GDK_KEY_F3 || (ctrl && k == GDK_KEY_g)) {
        if (shift)
            on_find_prev(NULL, ud);
        else
            on_find_next(NULL, ud);
        return TRUE;
    }
    return FALSE;
}

static void
on_find_close(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_widget_set_visible(w->find_bar, FALSE);
    w->find_total = 0;
    w->find_pos = 0;
    if (w->find_count_lbl)
        gtk_label_set_text(GTK_LABEL(w->find_count_lbl), "");
    gtk_widget_remove_css_class(w->find_bar, "not-found");
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t)
        web_tab_find_clear(t);
    kajo_web_win_focus_webview(w);
}

/* ─── Global Keyboard Shortcuts ─── */

gboolean
kajo_web_win_on_key_pressed(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
                            guint kc G_GNUC_UNUSED, GdkModifierType st, gpointer ud)
{
    KajoWebWindow *w = ud;
    gboolean ctrl = (st & GDK_CONTROL_MASK) != 0;
    gboolean shift = (st & GDK_SHIFT_MASK) != 0;
    gboolean alt = (st & GDK_ALT_MASK) != 0;
    guint k = gdk_keyval_to_lower(keyval);

    if (keyval == GDK_KEY_Escape) {
        if (web_palette_is_visible(w->palette)) {
            web_palette_close(w->palette);
            kajo_web_win_focus_webview(w);
            return TRUE;
        }
        if (gtk_widget_get_visible(w->find_bar)) {
            on_find_close(NULL, w);
            return TRUE;
        }
        if (web_tab_rail_escape(w->rail))
            return TRUE;
        if (gtk_widget_has_focus(w->omni_entry)) {
            if (web_omnibar_ac_is_visible(w->omni_ac)) {
                web_omnibar_ac_dismiss(w->omni_ac);
                return TRUE;
            }
            KajoWebTab *t = kajo_web_win_active_tab(w);
            const gchar *u = t ? web_tab_get_uri(t) : "";
            w->updating_omni = TRUE;
            gtk_editable_set_text(GTK_EDITABLE(w->omni_entry), kajo_web_win_is_blank_uri(u) ? "" : u);
            w->updating_omni = FALSE;
            kajo_web_win_focus_webview(w);
            return TRUE;
        }
        if (w->html5_fullscreen)
            return FALSE;
        if (w->window_fullscreen) {
            kajo_web_win_toggle_fullscreen(w);
            return TRUE;
        }
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t && web_tab_is_loading(t)) {
            web_tab_stop(t);
            return TRUE;
        }
        return FALSE;
    }
    if (keyval == GDK_KEY_F12 || (ctrl && shift && k == GDK_KEY_i)) {
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t) web_tab_toggle_inspector(t);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        kajo_web_win_toggle_fullscreen(w);
        return TRUE;
    }
    if (keyval == GDK_KEY_F5) {
        KajoWebTab *t = kajo_web_win_active_tab(w);
        if (t) web_tab_reload(t, ctrl || shift);
        return TRUE;
    }
    if (ctrl && shift && k == GDK_KEY_u) {
        kajo_web_win_toggle_zen_topbar(w);
        return TRUE;
    }
    if (ctrl && !shift && !alt) {
        switch (k) {
        case GDK_KEY_t: kajo_web_win_new_tab(w, NULL, FALSE); return TRUE;
        case GDK_KEY_n:
            if (w->app_cbs.on_new_window)
                w->app_cbs.on_new_window(w->profile_id, w->is_private, NULL, w->app_ud);
            else if (w->app_cbs.on_open_profile)
                w->app_cbs.on_open_profile(w->profile_id, w->is_private, NULL, w->app_ud);
            return TRUE;
        case GDK_KEY_w: if (w->model->active_id) kajo_web_win_close_tab(w, w->model->active_id); return TRUE;
        case GDK_KEY_b:
            if (w->cfg && w->cfg->rail_mode == KAJO_WEB_RAIL_PINNED)
                kajo_web_win_toggle_pin_rail(w);
            else
                web_tab_rail_toggle_reveal(w->rail);
            return TRUE;
        case GDK_KEY_l:
            if (w->top_autohide && !w->window_fullscreen && !w->html5_fullscreen)
                gtk_widget_set_visible(w->header_handle, TRUE);
            gtk_widget_grab_focus(w->omni_entry);
            gtk_editable_select_region(GTK_EDITABLE(w->omni_entry), 0, -1);
            return TRUE;
        case GDK_KEY_k: web_palette_open(w->palette, w->model, w->storage); return TRUE;
        case GDK_KEY_r: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_reload(t, FALSE); return TRUE; }
        case GDK_KEY_p: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_print(t, w->win); return TRUE; }
        case GDK_KEY_h: kajo_web_win_new_tab(w, "kajo://history", FALSE); return TRUE;
        case GDK_KEY_f: kajo_web_win_open_find(w); return TRUE;
        case GDK_KEY_j: kajo_web_win_show_downloads_popover(w); return TRUE;
        case GDK_KEY_d: toggle_bookmark_active(w); return TRUE;
        case GDK_KEY_m: if (w->model->active_id) on_rail_mute(w->model->active_id, w); return TRUE;
        case GDK_KEY_comma: kajo_web_win_open_settings(); return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
            { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) { web_tab_set_zoom(t, web_tab_get_zoom(t) + 0.1); kajo_web_win_sync_chrome(w); } return TRUE; }
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
            { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) { web_tab_set_zoom(t, web_tab_get_zoom(t) - 0.1); kajo_web_win_sync_chrome(w); } return TRUE; }
        case GDK_KEY_0: case GDK_KEY_KP_0:
            { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) { web_tab_set_zoom(t, 1.0); kajo_web_win_sync_chrome(w); } return TRUE; }
        case GDK_KEY_Tab: case GDK_KEY_Page_Down:
            { guint64 nxt = web_model_cycle_visual(w->model, TRUE); if (nxt) kajo_web_win_select_tab(w, nxt); return TRUE; }
        case GDK_KEY_Page_Up:
            { guint64 nxt = web_model_cycle_visual(w->model, FALSE); if (nxt) kajo_web_win_select_tab(w, nxt); return TRUE; }
        default: break;
        }
    }
    if (ctrl && shift) {
        if (keyval == GDK_KEY_plus) {
            KajoWebTab *t = kajo_web_win_active_tab(w);
            if (t) { web_tab_set_zoom(t, web_tab_get_zoom(t) + 0.1); kajo_web_win_sync_chrome(w); }
            return TRUE;
        }
        switch (k) {
        case GDK_KEY_t: on_rail_undo(w); return TRUE;
        case GDK_KEY_r: { KajoWebTab *t = kajo_web_win_active_tab(w); if (t) web_tab_reload(t, TRUE); return TRUE; }
        case GDK_KEY_n: if (w->app_cbs.on_open_profile) w->app_cbs.on_open_profile(w->profile_id, TRUE, NULL, w->app_ud); return TRUE;
        case GDK_KEY_b: case GDK_KEY_o: kajo_web_win_new_tab(w, "kajo://bookmarks", FALSE); return TRUE;
        case GDK_KEY_m: kajo_web_win_show_profile_popover(w); return TRUE;
        case GDK_KEY_ISO_Left_Tab: case GDK_KEY_Tab:
            { guint64 nxt = web_model_cycle_visual(w->model, FALSE); if (nxt) kajo_web_win_select_tab(w, nxt); return TRUE; }
        default: break;
        }
    }
    if (alt && !ctrl) {
        if (keyval == GDK_KEY_Left) {
            KajoWebTab *t = kajo_web_win_active_tab(w);
            if (t) web_tab_go_back(t);
            return TRUE;
        }
        if (keyval == GDK_KEY_Right) {
            KajoWebTab *t = kajo_web_win_active_tab(w);
            if (t) web_tab_go_forward(t);
            return TRUE;
        }
        if (k == GDK_KEY_d) {
            if (w->top_autohide && !w->window_fullscreen && !w->html5_fullscreen)
                gtk_widget_set_visible(w->header_handle, TRUE);
            gtk_widget_grab_focus(w->omni_entry);
            gtk_editable_select_region(GTK_EDITABLE(w->omni_entry), 0, -1);
            return TRUE;
        }
        if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
            guint idx = keyval - GDK_KEY_1;
            if (idx < w->model->tabs->len) {
                const KajoWebTabMeta *t = g_ptr_array_index(w->model->tabs, idx);
                kajo_web_win_select_tab(w, t->id);
                return TRUE;
            }
        }
    }
    return FALSE;
}

void
kajo_web_win_fill_rail_callbacks(KajoWebTabRailCallbacks *out)
{
    if (!out)
        return;
    *out = (KajoWebTabRailCallbacks){
        .on_select_tab = on_rail_select,
        .on_close_tab = on_rail_close,
        .on_toggle_mute = on_rail_mute,
        .on_toggle_pin = on_rail_pin,
        .on_reload_tab = on_rail_reload,
        .on_duplicate_tab = on_rail_duplicate,
        .on_copy_tab_url = on_rail_copy_url,
        .on_suspend_tab = on_rail_suspend,
        .on_close_other_tabs = on_rail_close_others,
        .on_close_tabs_below = on_rail_close_below,
        .on_toggle_group = on_rail_toggle_group,
        .on_new_tab = on_rail_new,
        .on_undo_close = on_rail_undo,
        .on_reorder_tab = on_rail_reorder,
        .get_favicon = on_rail_get_favicon,
    };
}

void
kajo_web_win_fill_palette_callbacks(KajoWebPaletteCallbacks *out)
{
    if (!out)
        return;
    *out = (KajoWebPaletteCallbacks){
        .on_select_tab = on_rail_select,
        .on_open_uri = on_pal_open_uri,
        .on_run_command = on_pal_cmd,
    };
}

void
kajo_web_win_wire_omnibar_and_find(KajoWebWindow *w,
                                   GtkWidget     *bf_prev,
                                   GtkWidget     *bf_next,
                                   GtkWidget     *bf_close)
{
    w->omni_ac = web_omnibar_ac_new(GTK_ENTRY(w->omni_entry), w->storage, w->model, on_omni_ac_select, w);
    g_signal_connect(w->omni_entry, "changed", G_CALLBACK(on_omni_changed), w);
    g_signal_connect(w->omni_entry, "activate", G_CALLBACK(on_omni_activate), w);
    GtkEventController *omni_fc = gtk_event_controller_focus_new();
    g_signal_connect(omni_fc, "leave", G_CALLBACK(on_omni_focus_leave), w);
    gtk_widget_add_controller(w->omni_entry, omni_fc);
    GtkEventController *omni_kc = gtk_event_controller_key_new();
    g_signal_connect(omni_kc, "key-pressed", G_CALLBACK(on_omni_key_pressed), w);
    gtk_widget_add_controller(w->omni_entry, omni_kc);
    GtkGesture *omni_click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(omni_click), GTK_PHASE_CAPTURE);
    g_signal_connect(omni_click, "pressed", G_CALLBACK(on_omni_click_pressed), w);
    g_signal_connect(omni_click, "released", G_CALLBACK(on_omni_click_released), w);
    gtk_widget_add_controller(w->omni_entry, GTK_EVENT_CONTROLLER(omni_click));

    g_signal_connect(w->btn_zoom, "clicked", G_CALLBACK(on_btn_zoom_clicked), w);
    g_signal_connect(w->btn_bookmark, "clicked", G_CALLBACK(on_btn_bookmark_clicked), w);

    g_signal_connect(w->find_entry, "changed", G_CALLBACK(on_find_changed), w);
    g_signal_connect(w->find_entry, "activate", G_CALLBACK(on_find_next), w);
    GtkEventController *fkc = gtk_event_controller_key_new();
    g_signal_connect(fkc, "key-pressed", G_CALLBACK(on_find_entry_key), w);
    gtk_widget_add_controller(w->find_entry, fkc);
    g_signal_connect(bf_prev, "clicked", G_CALLBACK(on_find_prev), w);
    g_signal_connect(bf_next, "clicked", G_CALLBACK(on_find_next), w);
    g_signal_connect(bf_close, "clicked", G_CALLBACK(on_find_close), w);
}
