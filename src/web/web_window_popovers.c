#include "web_window_private.h"
#include "../common/spawn_detached.h"
#include "../compositor/compositor.h"

static GtkWidget *
make_popover_3zone(const gchar *title, const gchar *badge_txt, const gchar *badge_cls,
                   gint min_width, GtkWidget **out_body, GtkWidget **out_footer)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(root, min_width, -1);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(hdr, "shell-popover-header");

    GtkWidget *tl = gtk_label_new(title);
    gtk_widget_set_hexpand(tl, TRUE);
    gtk_widget_set_halign(tl, GTK_ALIGN_START);
    gtk_widget_set_valign(tl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(tl, "shell-popover-title");
    gtk_box_append(GTK_BOX(hdr), tl);

    if (badge_txt && *badge_txt) {
        GtkWidget *bg = gtk_label_new(badge_txt);
        gtk_widget_set_valign(bg, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(bg, "shell-popover-badge");
        if (badge_cls && *badge_cls)
            gtk_widget_add_css_class(bg, badge_cls);
        gtk_box_append(GTK_BOX(hdr), bg);
        g_object_set_data(G_OBJECT(root), "hdr-badge", bg);
    }
    gtk_box_append(GTK_BOX(root), hdr);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(body, "shell-popover-body");
    gtk_box_append(GTK_BOX(root), body);
    if (out_body)
        *out_body = body;

    if (out_footer) {
        GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(foot, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(foot, "shell-popover-footer");
        gtk_box_append(GTK_BOX(root), foot);
        *out_footer = foot;
    }

    return root;
}

static GtkWidget *
make_toggle_card(const gchar *title, const gchar *subtitle, gboolean active,
                 GCallback on_toggled, gpointer ud)
{
    GtkWidget *tb = gtk_toggle_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb), active);
    gtk_widget_add_css_class(tb, "toggle-card");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_widget_set_valign(t, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(t, "tile-title");

    GtkWidget *s = gtk_label_new(subtitle);
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    gtk_widget_set_valign(s, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(s, "tile-subtitle");

    gtk_box_append(GTK_BOX(vbox), t);
    gtk_box_append(GTK_BOX(vbox), s);
    gtk_button_set_child(GTK_BUTTON(tb), vbox);

    g_object_set_data(G_OBJECT(tb), "sub-lbl", s);
    g_signal_connect(tb, "toggled", on_toggled, ud);
    return tb;
}

static void
mutate_set_adblock(KajoWebCfg *draft, gpointer ud)
{
    draft->adblock_enabled = GPOINTER_TO_INT(ud) != 0;
}

static void
mutate_set_userscripts(KajoWebCfg *draft, gpointer ud)
{
    draft->user_scripts_enabled = GPOINTER_TO_INT(ud) != 0;
}

static void
on_adblock_card_toggled(GtkToggleButton *tb, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    gboolean on = gtk_toggle_button_get_active(tb);
    kajo_config_store_update_web(NULL, mutate_set_adblock, GINT_TO_POINTER(on ? 1 : 0), NULL);
    GtkLabel *sub = g_object_get_data(G_OBJECT(tb), "sub-lbl");
    if (sub) {
        gtk_label_set_text(sub, on ? "EasyList + EasyPrivacy + Finland + Nordic + Cookies"
                                   : "Disabled globally — click to re-enable");
    }
    GtkWidget *root = gtk_popover_get_child(GTK_POPOVER(w->pop_shield));
    GtkLabel *bg = root ? g_object_get_data(G_OBJECT(root), "hdr-badge") : NULL;
    if (bg)
        gtk_label_set_text(bg, on ? web_adblock_get_status_text(w->adblock) : "DISABLED");
    kajo_web_win_sync_chrome(w);
}

static void
on_uscripts_card_toggled(GtkToggleButton *tb, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    gboolean on = gtk_toggle_button_get_active(tb);
    kajo_config_store_update_web(NULL, mutate_set_userscripts, GINT_TO_POINTER(on ? 1 : 0), NULL);
    kajo_web_win_sync_chrome(w);
}

static void
on_open_settings_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkPopover *pop = user_data;
    if (pop)
        gtk_popover_popdown(pop);
    kajo_web_win_open_settings();
}

static void
on_update_lists_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    web_adblock_update_lists_async(w->adblock);
    gtk_popover_popdown(GTK_POPOVER(w->pop_shield));
}

static void
on_site_shield_toggled(GtkToggleButton *tb, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (!t)
        return;
    g_autofree gchar *host = web_model_extract_host(web_tab_get_uri(t));
    if (!host || !*host)
        return;
    web_adblock_toggle_host_paused(w->adblock, host);
    gboolean active = gtk_toggle_button_get_active(tb);
    GtkLabel *sub = g_object_get_data(G_OBJECT(tb), "sub-lbl");
    if (sub) {
        gtk_label_set_text(sub, active ? "Active on this site — click to pause & reload"
                                       : "Paused on this site — click to resume blocking");
    }
    GtkWidget *root = gtk_popover_get_child(GTK_POPOVER(w->pop_shield));
    GtkLabel *bg = root ? g_object_get_data(G_OBJECT(root), "hdr-badge") : NULL;
    if (bg)
        gtk_label_set_text(bg, active ? web_adblock_get_status_text(w->adblock) : "PAUSED ON SITE");
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    web_tab_sync_adblock_pause(t, cfg);
    kajo_web_win_sync_chrome(w);
}

void
kajo_web_win_show_shield_popover(KajoWebWindow *w)
{
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    gboolean ab_on = cfg && cfg->adblock_enabled;
    gboolean us_on = cfg && cfg->user_scripts_enabled;

    KajoWebTab *t = kajo_web_win_active_tab(w);
    const gchar *uri = t ? web_tab_get_uri(t) : NULL;
    g_autofree gchar *host = (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
        ? web_model_extract_host(uri) : NULL;
    gboolean site_paused = host && web_adblock_is_host_paused(w->adblock, host);

    GtkWidget *body = NULL, *foot = NULL;
    GtkWidget *root = make_popover_3zone("CONTENT BLOCKER",
                                         site_paused ? "PAUSED ON SITE" : web_adblock_get_status_text(w->adblock),
                                         (ab_on && !site_paused) ? "success" : NULL,
                                         350, &body, &foot);

    if (host && *host && ab_on) {
        g_autofree gchar *stitle = g_strdup_printf("Shield on %s", host);
        gtk_box_append(GTK_BOX(body),
            make_toggle_card(stitle,
                             site_paused ? "Paused on this site — click to resume blocking"
                                         : "Active on this site — click to pause & reload",
                             !site_paused, G_CALLBACK(on_site_shield_toggled), w));
    }

    gtk_box_append(GTK_BOX(body),
        make_toggle_card("Ads, Trackers & Cookies",
                         ab_on ? "EasyList + EasyPrivacy + Finland + Nordic + Cookies"
                               : "Disabled globally — click to re-enable",
                         ab_on, G_CALLBACK(on_adblock_card_toggled), w));

    g_autofree gchar *us_sub = g_strdup_printf("%u user scripts & CSS loaded (~/.config/kajo/web/scripts)",
                                               web_adblock_get_user_script_count(w->adblock));
    gtk_box_append(GTK_BOX(body),
        make_toggle_card("User Scripts & CSS", us_sub,
                         us_on, G_CALLBACK(on_uscripts_card_toggled), w));

    GtkWidget *b_upd = gtk_button_new_with_label(
        web_adblock_is_updating(w->adblock) ? "↻ UPDATING..." : "↻ UPDATE LISTS");
    gtk_widget_set_hexpand(b_upd, TRUE);
    gtk_widget_set_sensitive(b_upd, !web_adblock_is_updating(w->adblock));
    g_signal_connect(b_upd, "clicked", G_CALLBACK(on_update_lists_clicked), w);

    GtkWidget *b_set = gtk_button_new_with_label("⚙ SETTINGS");
    g_signal_connect(b_set, "clicked", G_CALLBACK(on_open_settings_clicked), w->pop_shield);

    gtk_box_append(GTK_BOX(foot), b_upd);
    gtk_box_append(GTK_BOX(foot), b_set);

    gtk_popover_set_child(GTK_POPOVER(w->pop_shield), root);
    gtk_popover_popup(GTK_POPOVER(w->pop_shield));
}

/* ─── Media Stream Sniffer & Downloader Popover ─── */

typedef struct {
    KajoWebWindow      *w;
    KajoWebMediaStream *stream_copy;
} StreamDlCtx;

static void
stream_dl_ctx_free(gpointer ptr, GClosure *c G_GNUC_UNUSED)
{
    StreamDlCtx *ctx = ptr;
    if (!ctx)
        return;
    web_media_stream_free(ctx->stream_copy);
    g_free(ctx);
}

static void
on_sniffed_stream_dl(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    StreamDlCtx *ctx = user_data;
    KajoWebTab *t = kajo_web_win_active_tab(ctx->w);
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    web_downloads_start_ffmpeg_stream(ctx->w->downloads,
                                      ctx->stream_copy,
                                      t ? web_tab_get_title(t) : "kajo-video",
                                      t ? web_tab_get_uri(t) : "",
                                      ctx->w->session,
                                      cfg);
    gtk_popover_popdown(GTK_POPOVER(ctx->w->pop_media));
}

typedef struct {
    KajoWebWindow      *w;
    KajoWebDownloadFmt  fmt;
} YtdlpBtnCtx;

static void
free_ytdlp_ctx(gpointer ptr, GClosure *c G_GNUC_UNUSED)
{
    g_free(ptr);
}

static void
on_ytdlp_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    YtdlpBtnCtx *ctx = user_data;
    KajoWebTab *t = kajo_web_win_active_tab(ctx->w);
    if (t) {
        g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
        web_downloads_start_ytdlp(ctx->w->downloads, web_tab_get_uri(t), ctx->fmt, cfg);
    }
    gtk_popover_popdown(GTK_POPOVER(ctx->w->pop_media));
}

static void
on_open_kajo_player_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t) {
        const GPtrArray *streams = web_tab_get_media_streams(t);
        if (streams && streams->len > 0) {
            const KajoWebMediaStream *s0 = g_ptr_array_index(streams, 0);
            web_downloads_open_in_kajo_player(s0->uri);
        } else {
            web_downloads_open_in_kajo_player(web_tab_get_uri(t));
        }
    }
    gtk_popover_popdown(GTK_POPOVER(w->pop_media));
}

void
kajo_web_win_show_media_popover(KajoWebWindow *w)
{
    KajoWebTab *t = kajo_web_win_active_tab(w);
    const GPtrArray *streams = t ? web_tab_get_media_streams(t) : NULL;
    gboolean has_ffmpeg = web_downloads_has_ffmpeg();
    gboolean has_ytdlp  = web_downloads_has_ytdlp();

    g_autofree gchar *badge = g_strdup_printf("%u STREAMS", streams ? streams->len : 0u);
    GtkWidget *body = NULL, *foot = NULL;
    GtkWidget *root = make_popover_3zone("MEDIA DOWNLOADER", badge,
                                         (streams && streams->len > 0) ? "accent" : NULL,
                                         350, &body, &foot);

    if (streams && streams->len > 0) {
        g_autofree gchar *stxt = g_strdup_printf("SNIFFED STREAMS (FFMPEG %s)",
                                                 has_ffmpeg ? "READY" : "MISSING");
        GtkWidget *sl = gtk_label_new(stxt);
        gtk_widget_set_halign(sl, GTK_ALIGN_START);
        gtk_widget_add_css_class(sl, "shell-popover-subtitle");
        gtk_box_append(GTK_BOX(body), sl);

        GtkWidget *ssc = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ssc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(ssc), 180);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(ssc), TRUE);
        GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        for (guint i = 0; i < streams->len; i++) {
            const KajoWebMediaStream *s = g_ptr_array_index(streams, i);
            g_autofree gchar *blbl = g_strdup_printf("⬇ %s", s->label);
            GtkWidget *b = gtk_button_new_with_label(blbl);
            gtk_widget_set_sensitive(b, has_ffmpeg);
            StreamDlCtx *sc = g_new0(StreamDlCtx, 1);
            sc->w = w;
            sc->stream_copy = web_media_stream_new(s->kind, s->uri, s->label, s->mime_type,
                                                   s->width, s->height, s->bandwidth, s->size_bytes);
            g_signal_connect_data(b, "clicked", G_CALLBACK(on_sniffed_stream_dl), sc, stream_dl_ctx_free, 0);
            gtk_box_append(GTK_BOX(sbox), b);
        }
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ssc), sbox);
        gtk_box_append(GTK_BOX(body), ssc);
    } else {
        GtkWidget *none = gtk_label_new("No .m3u8/.mpd/.mp4 streams sniffed on this page yet.");
        gtk_widget_set_halign(none, GTK_ALIGN_START);
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(body), none);
    }

    GtkWidget *yt_hdr = gtk_label_new(has_ytdlp ? "YT-DLP + FFMPEG (YOUTUBE / SOCIAL)"
                                                : "YT-DLP NOT INSTALLED (pacman -S yt-dlp)");
    gtk_widget_set_halign(yt_hdr, GTK_ALIGN_START);
    gtk_widget_add_css_class(yt_hdr, "shell-popover-subtitle");
    gtk_box_append(GTK_BOX(body), yt_hdr);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    KajoWebDownloadFmt def_fmt = w->cfg ? w->cfg->media_download_format : KAJO_WEB_DL_BEST_MP4;
    static const struct { const char *lbl; KajoWebDownloadFmt fmt; int c; int r; } k_fmts[] = {
        { "⬇ BEST MP4",    KAJO_WEB_DL_BEST_MP4,   0, 0 },
        { "⬇ 1080P MP4",   KAJO_WEB_DL_1080P,      1, 0 },
        { "⬇ 720P MP4",    KAJO_WEB_DL_720P,       0, 1 },
        { "🎵 AUDIO OPUS", KAJO_WEB_DL_AUDIO_OPUS, 1, 1 },
        { "🎵 AUDIO MP3",  KAJO_WEB_DL_AUDIO_MP3,  0, 2 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_fmts); i++) {
        GtkWidget *b = gtk_button_new_with_label(k_fmts[i].lbl);
        if (k_fmts[i].fmt == def_fmt)
            gtk_widget_add_css_class(b, "action-btn-primary");
        gtk_widget_set_sensitive(b, has_ytdlp);
        YtdlpBtnCtx *yc = g_new0(YtdlpBtnCtx, 1);
        yc->w = w;
        yc->fmt = k_fmts[i].fmt;
        g_signal_connect_data(b, "clicked", G_CALLBACK(on_ytdlp_clicked), yc, free_ytdlp_ctx, 0);
        gtk_grid_attach(GTK_GRID(grid), b, k_fmts[i].c, k_fmts[i].r, 1, 1);
    }
    gtk_box_append(GTK_BOX(body), grid);

    GtkWidget *b_pl = gtk_button_new_with_label("▶ OPEN IN KAJO PLAYER (libmpv)");
    gtk_widget_set_hexpand(b_pl, TRUE);
    g_signal_connect(b_pl, "clicked", G_CALLBACK(on_open_kajo_player_clicked), w);
    gtk_box_append(GTK_BOX(foot), b_pl);

    gtk_popover_set_child(GTK_POPOVER(w->pop_media), root);
    gtk_popover_popup(GTK_POPOVER(w->pop_media));
}

static gboolean
on_dl_popover_refresh_tick(gpointer user_data)
{
    KajoWebWindow *w = user_data;
    if (!w->pop_downloads || !gtk_widget_get_visible(w->pop_downloads)) {
        w->dl_refresh_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (w->dl_popover_dirty) {
        w->dl_popover_dirty = FALSE;
        gtk_popover_set_child(GTK_POPOVER(w->pop_downloads),
                              web_downloads_build_popover_content(w->downloads));
    }
    return G_SOURCE_CONTINUE;
}

void
kajo_web_win_show_downloads_popover(KajoWebWindow *w)
{
    w->dl_popover_dirty = FALSE;
    GtkWidget *content = web_downloads_build_popover_content(w->downloads);
    gtk_popover_set_child(GTK_POPOVER(w->pop_downloads), content);
    gtk_popover_popup(GTK_POPOVER(w->pop_downloads));
    if (!w->dl_refresh_timer_id)
        w->dl_refresh_timer_id = g_timeout_add(150, on_dl_popover_refresh_tick, w);
}

/* ─── Profile Switcher & Google Drive AppData Sync Popover ─── */

typedef struct {
    KajoWebWindow *w;
    gchar         *profile_id;
} ProfSwitchCtx;

static void
prof_switch_ctx_free(gpointer ptr, GClosure *c G_GNUC_UNUSED)
{
    ProfSwitchCtx *ctx = ptr;
    if (!ctx)
        return;
    g_free(ctx->profile_id);
    g_free(ctx);
}

static void
on_switch_profile_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ProfSwitchCtx *ctx = user_data;
    gtk_popover_popdown(GTK_POPOVER(ctx->w->pop_profile));
    if (ctx->w->app_cbs.on_open_profile)
        ctx->w->app_cbs.on_open_profile(ctx->profile_id, FALSE, NULL, ctx->w->app_ud);
}

static void
on_sync_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    if (!w->sync)
        return;
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    g_clear_pointer(&w->sync_err_msg, g_free);
    if (web_sync_is_connected(w->sync)) {
        web_sync_trigger_now(w->sync, cfg, NULL);
        gtk_popover_popdown(GTK_POPOVER(w->pop_profile));
        return;
    }
    g_autoptr(GError) err = NULL;
    if (!web_sync_begin_oauth(w->sync, cfg, &err)) {
        w->sync_err_msg = g_strdup(err ? err->message : "OAuth configuration error");
        if (w->sync_err_lbl) {
            gtk_label_set_text(GTK_LABEL(w->sync_err_lbl), w->sync_err_msg);
            gtk_widget_set_visible(w->sync_err_lbl, TRUE);
        }
        return;
    }
    gtk_popover_popdown(GTK_POPOVER(w->pop_profile));
}

static void
on_open_picker_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    gtk_popover_popdown(GTK_POPOVER(w->pop_profile));
    if (w->app_cbs.on_open_picker)
        w->app_cbs.on_open_picker(w->app_ud);
}

static void
on_private_win_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    gtk_popover_popdown(GTK_POPOVER(w->pop_profile));
    if (w->app_cbs.on_open_profile)
        w->app_cbs.on_open_profile(w->profile_id, TRUE, NULL, w->app_ud);
}

void
kajo_web_win_show_profile_popover(KajoWebWindow *w)
{
    KajoWebProfileRegistry *reg = web_profile_manager_get_registry(w->pm);
    KajoWebProfileInfo *cur = reg ? web_profile_registry_find(reg, w->profile_id) : NULL;

    const gchar *pname = w->is_private ? "PRIVATE" : (cur ? cur->name : "DEFAULT");
    GtkWidget *body = NULL, *foot = NULL;
    GtkWidget *root = make_popover_3zone("PROFILES & SYNC", pname,
                                         w->is_private ? NULL : "accent",
                                         330, &body, &foot);

    if (!w->is_private && w->sync) {
        GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(sbox, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(sbox, "web-dl-card");
        GtkWidget *sl = gtk_label_new(web_sync_get_status(w->sync));
        gtk_widget_set_hexpand(sl, TRUE);
        gtk_widget_set_halign(sl, GTK_ALIGN_START);
        gtk_widget_set_valign(sl, GTK_ALIGN_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(sl), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(sl, "web-badge-telemetry");
        GtkWidget *sb = gtk_button_new_with_label(web_sync_is_connected(w->sync) ? "↻ SYNC" : "SIGN IN");
        gtk_widget_set_valign(sb, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(sb, "action-btn-primary");
        g_signal_connect(sb, "clicked", G_CALLBACK(on_sync_btn_clicked), w);
        gtk_box_append(GTK_BOX(sbox), sl);
        gtk_box_append(GTK_BOX(sbox), sb);
        gtk_box_append(GTK_BOX(body), sbox);

        w->sync_err_lbl = gtk_label_new(w->sync_err_msg ? w->sync_err_msg : "");
        gtk_widget_set_halign(w->sync_err_lbl, GTK_ALIGN_START);
        gtk_label_set_wrap(GTK_LABEL(w->sync_err_lbl), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(w->sync_err_lbl), 38);
        gtk_widget_add_css_class(w->sync_err_lbl, "web-sync-err");
        gtk_widget_set_visible(w->sync_err_lbl, w->sync_err_msg && *w->sync_err_msg);
        gtk_box_append(GTK_BOX(body), w->sync_err_lbl);
    } else {
        w->sync_err_lbl = NULL;
    }

    if (reg) {
        for (guint i = 0; i < reg->profiles->len; i++) {
            KajoWebProfileInfo *p = g_ptr_array_index(reg->profiles, i);
            gboolean is_cur = (g_strcmp0(p->id, w->profile_id) == 0 && !w->is_private);
            g_autofree gchar *plbl = g_strdup_printf("%s  %s%s", p->icon ? p->icon : "🌐",
                                                     p->name, is_cur ? "  (ACTIVE)" : "");
            GtkWidget *pb = gtk_button_new_with_label(plbl);
            GtkWidget *pc_lbl = gtk_button_get_child(GTK_BUTTON(pb));
            if (GTK_IS_LABEL(pc_lbl))
                gtk_widget_set_halign(pc_lbl, GTK_ALIGN_START);
            if (is_cur)
                gtk_widget_add_css_class(pb, "active");
            ProfSwitchCtx *pc = g_new0(ProfSwitchCtx, 1);
            pc->w = w;
            pc->profile_id = g_strdup(p->id);
            g_signal_connect_data(pb, "clicked", G_CALLBACK(on_switch_profile_clicked), pc, prof_switch_ctx_free, 0);
            gtk_box_append(GTK_BOX(body), pb);
        }
    }

    GtkWidget *b_pick = gtk_button_new_with_label("⊞ PICKER");
    gtk_widget_set_hexpand(b_pick, TRUE);
    g_signal_connect(b_pick, "clicked", G_CALLBACK(on_open_picker_clicked), w);

    GtkWidget *b_priv = gtk_button_new_with_label("🕶 PRIVATE");
    gtk_widget_set_hexpand(b_priv, TRUE);
    g_signal_connect(b_priv, "clicked", G_CALLBACK(on_private_win_clicked), w);

    gtk_box_append(GTK_BOX(foot), b_pick);
    gtk_box_append(GTK_BOX(foot), b_priv);

    gtk_popover_set_child(GTK_POPOVER(w->pop_profile), root);
    gtk_popover_popup(GTK_POPOVER(w->pop_profile));
}

/* ─── Overflow Browser Menu (☰) ─── */

static GtkWidget *
make_menu_action_btn(const gchar *lbl, GCallback cb, gpointer ud)
{
    GtkWidget *b = gtk_button_new_with_label(lbl);
    gtk_widget_set_halign(b, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(b, "web-ctx-row");
    GtkWidget *c = gtk_button_get_child(GTK_BUTTON(b));
    if (GTK_IS_LABEL(c))
        gtk_widget_set_halign(c, GTK_ALIGN_START);
    g_signal_connect(b, "clicked", cb, ud);
    return b;
}

static void
on_menu_pal_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    web_palette_open(w->palette, w->model, w->storage);
}

static void
on_menu_new_tab_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    kajo_web_win_new_tab(w, NULL, FALSE);
}

static void
on_menu_new_priv_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    if (w->app_cbs.on_open_profile)
        w->app_cbs.on_open_profile(w->profile_id, TRUE, NULL, w->app_ud);
}

static void
on_menu_new_win_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    if (w->app_cbs.on_new_window)
        w->app_cbs.on_new_window(w->profile_id, w->is_private, NULL, w->app_ud);
    else if (w->app_cbs.on_open_profile)
        w->app_cbs.on_open_profile(w->profile_id, w->is_private, NULL, w->app_ud);
}

static void
on_menu_find_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    kajo_web_win_open_find(w);
}

static void
on_menu_print_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t)
        web_tab_print(t, w->win);
}

static void
on_menu_fs_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    kajo_web_win_toggle_fullscreen(w);
}

static void
on_menu_insp_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    KajoWebTab *t = kajo_web_win_active_tab(w);
    if (t)
        web_tab_toggle_inspector(t);
}

static void
on_menu_float_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    shell_compositor_toggle_floating();
}

static void
on_menu_set_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebWindow *w = ud;
    gtk_popover_popdown(GTK_POPOVER(w->pop_menu));
    kajo_web_win_open_settings();
}

void
kajo_web_win_show_menu_popover(KajoWebWindow *w)
{
    GtkWidget *body = NULL;
    GtkWidget *root = make_popover_3zone("KAJO WEB",
                                         w->is_private ? "PRIVATE" : "MENU",
                                         w->is_private ? NULL : "accent",
                                         280, &body, NULL);

    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("⌘  Command & Tab Switcher (Ctrl+K)",
                                        G_CALLBACK(on_menu_pal_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("+  New Tab (Ctrl+T)",
                                        G_CALLBACK(on_menu_new_tab_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("⊞  New Window (Ctrl+N)",
                                        G_CALLBACK(on_menu_new_win_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("🕶  New Private Window (Ctrl+Shift+N)",
                                        G_CALLBACK(on_menu_new_priv_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("🔎  Find in Page (Ctrl+F)",
                                        G_CALLBACK(on_menu_find_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("⎙  Print Page (Ctrl+P)",
                                        G_CALLBACK(on_menu_print_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("⛶  Toggle Fullscreen (F11)",
                                        G_CALLBACK(on_menu_fs_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("<> Web Inspector / DevTools (F12)",
                                        G_CALLBACK(on_menu_insp_clicked), w));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("🗗  Float / Tile Window",
                                        G_CALLBACK(on_menu_float_clicked), w));
    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(body),
                   make_menu_action_btn("⚙  Kajo Web Settings (Ctrl+,)",
                                        G_CALLBACK(on_menu_set_clicked), w));

    gtk_popover_set_child(GTK_POPOVER(w->pop_menu), root);
    gtk_popover_popup(GTK_POPOVER(w->pop_menu));
}

void
kajo_web_win_open_find(KajoWebWindow *w)
{
    if (!w || !w->find_bar || !w->find_entry)
        return;
    gtk_widget_set_visible(w->find_bar, TRUE);
    gtk_widget_grab_focus(w->find_entry);
    gtk_editable_select_region(GTK_EDITABLE(w->find_entry), 0, -1);
}

void
kajo_web_win_sync_fullscreen_chrome(KajoWebWindow *w)
{
    if (!w || !w->header_handle || !w->rail)
        return;
    gboolean fs = w->window_fullscreen || w->html5_fullscreen;
    if (fs) {
        if (w->zen_hide_timer_id) {
            g_source_remove(w->zen_hide_timer_id);
            w->zen_hide_timer_id = 0;
        }
        gtk_widget_set_visible(w->header_handle, FALSE);
        gtk_widget_set_visible(w->top_zen_trigger, FALSE);
        web_tab_rail_escape(w->rail);
        gtk_widget_set_visible(web_tab_rail_get_trigger(w->rail), FALSE);
        gtk_widget_set_visible(web_tab_rail_get_widget(w->rail), FALSE);
        gtk_widget_set_visible(web_tab_rail_get_compact_spacer(w->rail), FALSE);
    } else {
        gtk_widget_set_visible(w->header_handle, !w->top_autohide);
        gtk_widget_set_visible(w->top_zen_trigger, w->top_autohide);
        web_tab_rail_apply_config(w->rail, w->cfg);
    }
}

void
kajo_web_win_toggle_fullscreen(KajoWebWindow *w)
{
    if (!w || !w->win)
        return;
    w->window_fullscreen = !w->window_fullscreen;
    if (w->window_fullscreen)
        gtk_window_fullscreen(w->win);
    else
        gtk_window_unfullscreen(w->win);
    kajo_web_win_sync_fullscreen_chrome(w);
}

void
kajo_web_win_set_html5_fullscreen(KajoWebWindow *w, gboolean enter)
{
    if (!w)
        return;
    w->html5_fullscreen = enter;
    kajo_web_win_sync_fullscreen_chrome(w);
}

static gchar *
extract_scheme_query_param(const gchar *uri, const gchar *key)
{
    const gchar *q = uri ? strchr(uri, '?') : NULL;
    if (!q || !key)
        return NULL;
    gsize klen = strlen(key);
    g_auto(GStrv) parts = g_strsplit(q + 1, "&", -1);
    for (int i = 0; parts && parts[i]; i++) {
        if (strncmp(parts[i], key, klen) == 0 && parts[i][klen] == '=') {
            g_autofree gchar *raw = g_strdup(parts[i] + klen + 1);
            for (char *p = raw; *p; p++) {
                if (*p == '+')
                    *p = ' ';
            }
            return g_uri_unescape_string(raw, NULL);
        }
    }
    return NULL;
}

gboolean
kajo_web_win_handle_scheme_action(KajoWebWindow *w, KajoWebTab *tab, const gchar *uri)
{
    if (!w || !tab || !uri)
        return FALSE;

    if (g_str_has_prefix(uri, "kajo://search")) {
        g_autofree gchar *q = extract_scheme_query_param(uri, "q");
        if (q && *q) {
            const KajoWebCfg *cfg = w->cfg;
            g_autofree gchar *resolved = web_model_resolve_input(
                q,
                cfg ? cfg->search_engine : KAJO_WEB_SEARCH_DUCKDUCKGO,
                cfg ? cfg->search_custom_url : "");
            web_tab_load_uri(tab, resolved);
        }
        return TRUE;
    }

    if (g_str_has_prefix(uri, "kajo://action/")) {
        g_autofree gchar *tok = extract_scheme_query_param(uri, "tok");
        if (!tok || !w->scheme_ctx.action_token[0] ||
            g_strcmp0(tok, w->scheme_ctx.action_token) != 0) {
            return TRUE;
        }
        const gchar *sub = uri + strlen("kajo://action/");
        const gchar *q = strchr(sub, '?');
        g_autofree gchar *act = q ? g_strndup(sub, (gsize)(q - sub)) : g_strdup(sub);

        if (g_strcmp0(act, "clear-history") == 0) {
            if (w->storage && !w->is_private)
                web_storage_clear_history(w->storage);
            web_tab_load_uri(tab, "kajo://history");
        } else if (g_strcmp0(act, "delete-history") == 0) {
            g_autofree gchar *turi = extract_scheme_query_param(uri, "uri");
            if (w->storage && !w->is_private && turi && *turi)
                web_storage_delete_history_uri(w->storage, turi);
            web_tab_load_uri(tab, "kajo://history");
        } else if (g_strcmp0(act, "delete-bookmark") == 0) {
            g_autofree gchar *turi = extract_scheme_query_param(uri, "uri");
            if (w->storage && !w->is_private && turi && *turi) {
                g_autofree gchar *uuid = NULL;
                if (web_storage_is_bookmarked(w->storage, turi, &uuid))
                    web_storage_delete_bookmark(w->storage, uuid);
            }
            kajo_web_win_invalidate_bookmark_cache(w);
            kajo_web_win_sync_chrome(w);
            web_tab_load_uri(tab, "kajo://bookmarks");
        }
        return TRUE;
    }

    return FALSE;
}

static void
on_dl_popover_closed(GtkPopover *pop G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    if (w->dl_refresh_timer_id) {
        g_source_remove(w->dl_refresh_timer_id);
        w->dl_refresh_timer_id = 0;
    }
}

void
kajo_web_win_setup_popovers(KajoWebWindow *w)
{
    w->pop_shield = gtk_popover_new();
    gtk_widget_set_parent(w->pop_shield, w->btn_shield);
    gtk_widget_add_css_class(w->pop_shield, "shell-popover");

    w->pop_media = gtk_popover_new();
    gtk_widget_set_parent(w->pop_media, w->btn_media);
    gtk_widget_add_css_class(w->pop_media, "shell-popover");

    w->pop_downloads = gtk_popover_new();
    gtk_widget_set_parent(w->pop_downloads, w->btn_downloads);
    gtk_widget_add_css_class(w->pop_downloads, "shell-popover");
    g_signal_connect(w->pop_downloads, "closed", G_CALLBACK(on_dl_popover_closed), w);

    w->pop_profile = gtk_popover_new();
    gtk_widget_set_parent(w->pop_profile, w->btn_profile);
    gtk_widget_add_css_class(w->pop_profile, "shell-popover");

    w->pop_menu = gtk_popover_new();
    gtk_widget_set_parent(w->pop_menu, w->btn_menu);
    gtk_widget_add_css_class(w->pop_menu, "shell-popover");
}
