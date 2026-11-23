#include "web_window_private.h"
#include "../common/spawn_detached.h"

static void
mutate_toggle_adblock(KajoWebCfg *draft, gpointer ud G_GNUC_UNUSED)
{
    draft->adblock_enabled = !draft->adblock_enabled;
}

static void
mutate_toggle_userscripts(KajoWebCfg *draft, gpointer ud G_GNUC_UNUSED)
{
    draft->user_scripts_enabled = !draft->user_scripts_enabled;
}

static void
on_toggle_adblock_btn(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    kajo_config_store_update_web(NULL, mutate_toggle_adblock, NULL, NULL);
    gtk_popover_popdown(GTK_POPOVER(w->pop_shield));
}

static void
on_toggle_uscripts_btn(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebWindow *w = user_data;
    kajo_config_store_update_web(NULL, mutate_toggle_userscripts, NULL, NULL);
    gtk_popover_popdown(GTK_POPOVER(w->pop_shield));
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

void
kajo_web_win_show_shield_popover(KajoWebWindow *w)
{
    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(box, 310, -1);
    gtk_widget_add_css_class(box, "web-popover-box");

    g_autofree gchar *hdr_txt = g_strdup_printf("🛡 CONTENT BLOCKER • %s",
                                                web_adblock_get_status_text(w->adblock));
    GtkWidget *hdr = gtk_label_new(hdr_txt);
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_widget_add_css_class(hdr, "web-popover-title");
    gtk_box_append(GTK_BOX(box), hdr);

    GtkWidget *b_ab = gtk_button_new_with_label(
        (cfg && cfg->adblock_enabled) ? "✓ ADS & TRACKERS: ON (CLICK TO DISABLE)"
                                      : "✕ ADS & TRACKERS: OFF (CLICK TO ENABLE)");
    gtk_widget_add_css_class(b_ab, (cfg && cfg->adblock_enabled) ? "action-btn-primary" : "files-nav-btn");
    g_signal_connect(b_ab, "clicked", G_CALLBACK(on_toggle_adblock_btn), w);
    gtk_box_append(GTK_BOX(box), b_ab);

    GtkWidget *b_upd = gtk_button_new_with_label(
        web_adblock_is_updating(w->adblock) ? "↻ UPDATING COMMUNITY LISTS..."
                                            : "↻ UPDATE FILTER LISTS (~3,700+ DOMAINS)");
    gtk_widget_add_css_class(b_upd, "files-nav-btn");
    gtk_widget_set_sensitive(b_upd, !web_adblock_is_updating(w->adblock));
    g_signal_connect(b_upd, "clicked", G_CALLBACK(on_update_lists_clicked), w);
    gtk_box_append(GTK_BOX(box), b_upd);

    g_autofree gchar *us_lbl = g_strdup_printf("USER SCRIPTS & CSS (%u): %s",
                                               web_adblock_get_user_script_count(w->adblock),
                                               (cfg && cfg->user_scripts_enabled) ? "ON" : "OFF");
    GtkWidget *b_us = gtk_button_new_with_label(us_lbl);
    gtk_widget_add_css_class(b_us, "files-nav-btn");
    g_signal_connect(b_us, "clicked", G_CALLBACK(on_toggle_uscripts_btn), w);
    gtk_box_append(GTK_BOX(box), b_us);

    GtkWidget *b_set = gtk_button_new_with_label("⚙ CONFIGURE IN KAJO SETTINGS");
    gtk_widget_add_css_class(b_set, "files-nav-btn");
    g_signal_connect(b_set, "clicked", G_CALLBACK(on_open_settings_clicked), w->pop_shield);
    gtk_box_append(GTK_BOX(box), b_set);

    gtk_popover_set_child(GTK_POPOVER(w->pop_shield), box);
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

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(box, 330, -1);
    gtk_widget_add_css_class(box, "web-popover-box");

    GtkWidget *hdr = gtk_label_new("⬇ MEDIA SNIFFER & DOWNLOADER");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_widget_add_css_class(hdr, "web-popover-title");
    gtk_box_append(GTK_BOX(box), hdr);

    if (streams && streams->len > 0) {
        g_autofree gchar *stxt = g_strdup_printf("SNIFFED STREAMS (%u) — FFMPEG %s",
                                                 streams->len, has_ffmpeg ? "READY" : "MISSING");
        GtkWidget *sl = gtk_label_new(stxt);
        gtk_widget_set_halign(sl, GTK_ALIGN_START);
        gtk_widget_add_css_class(sl, "web-badge-telemetry");
        gtk_box_append(GTK_BOX(box), sl);

        guint lim = MIN(streams->len, 5);
        for (guint i = 0; i < lim; i++) {
            const KajoWebMediaStream *s = g_ptr_array_index(streams, i);
            g_autofree gchar *blbl = g_strdup_printf("⬇ %s", s->label);
            GtkWidget *b = gtk_button_new_with_label(blbl);
            gtk_widget_add_css_class(b, "files-nav-btn");
            gtk_widget_set_sensitive(b, has_ffmpeg);
            StreamDlCtx *sc = g_new0(StreamDlCtx, 1);
            sc->w = w;
            sc->stream_copy = web_media_stream_new(s->kind, s->uri, s->label, s->mime_type,
                                                   s->width, s->height, s->bandwidth, s->size_bytes);
            g_signal_connect_data(b, "clicked", G_CALLBACK(on_sniffed_stream_dl), sc, stream_dl_ctx_free, 0);
            gtk_box_append(GTK_BOX(box), b);
        }
    } else {
        GtkWidget *none = gtk_label_new("No .m3u8/.mpd/.mp4 streams sniffed on this page yet.");
        gtk_widget_set_halign(none, GTK_ALIGN_START);
        gtk_widget_add_css_class(none, "web-dim-label");
        gtk_box_append(GTK_BOX(box), none);
    }

    GtkWidget *yt_hdr = gtk_label_new(has_ytdlp ? "YT-DLP + FFMPEG (YOUTUBE / SOCIAL)"
                                                : "YT-DLP NOT INSTALLED (pacman -S yt-dlp)");
    gtk_widget_set_halign(yt_hdr, GTK_ALIGN_START);
    gtk_widget_add_css_class(yt_hdr, has_ytdlp ? "web-badge-telemetry" : "web-dim-label");
    gtk_box_append(GTK_BOX(box), yt_hdr);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    static const struct { const char *lbl; KajoWebDownloadFmt fmt; int c; int r; } k_fmts[] = {
        { "⬇ BEST MP4",   KAJO_WEB_DL_BEST_MP4,   0, 0 },
        { "⬇ 1080P MP4",  KAJO_WEB_DL_1080P,      1, 0 },
        { "⬇ 720P MP4",   KAJO_WEB_DL_720P,       0, 1 },
        { "🎵 AUDIO OPUS", KAJO_WEB_DL_AUDIO_OPUS, 1, 1 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_fmts); i++) {
        GtkWidget *b = gtk_button_new_with_label(k_fmts[i].lbl);
        gtk_widget_add_css_class(b, i == 0 ? "action-btn-primary" : "files-nav-btn");
        gtk_widget_set_sensitive(b, has_ytdlp);
        YtdlpBtnCtx *yc = g_new0(YtdlpBtnCtx, 1);
        yc->w = w;
        yc->fmt = k_fmts[i].fmt;
        g_signal_connect_data(b, "clicked", G_CALLBACK(on_ytdlp_clicked), yc, free_ytdlp_ctx, 0);
        gtk_grid_attach(GTK_GRID(grid), b, k_fmts[i].c, k_fmts[i].r, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);

    GtkWidget *b_pl = gtk_button_new_with_label("▶ OPEN IN KAJO PLAYER (libmpv)");
    gtk_widget_add_css_class(b_pl, "files-nav-btn");
    g_signal_connect(b_pl, "clicked", G_CALLBACK(on_open_kajo_player_clicked), w);
    gtk_box_append(GTK_BOX(box), b_pl);

    gtk_popover_set_child(GTK_POPOVER(w->pop_media), box);
    gtk_popover_popup(GTK_POPOVER(w->pop_media));
}

void
kajo_web_win_show_downloads_popover(KajoWebWindow *w)
{
    GtkWidget *content = web_downloads_build_popover_content(w->downloads);
    gtk_popover_set_child(GTK_POPOVER(w->pop_downloads), content);
    gtk_popover_popup(GTK_POPOVER(w->pop_downloads));
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
    if (web_sync_is_connected(w->sync)) {
        web_sync_trigger_now(w->sync, cfg, NULL);
    } else {
        g_autoptr(GError) err = NULL;
        if (!web_sync_begin_oauth(w->sync, cfg, &err))
            g_warning("kajo-web sync: %s", err ? err->message : "oauth config missing");
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
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(box, 320, -1);
    gtk_widget_add_css_class(box, "web-popover-box");

    KajoWebProfileRegistry *reg = web_profile_manager_get_registry(w->pm);
    KajoWebProfileInfo *cur = reg ? web_profile_registry_find(reg, w->profile_id) : NULL;

    g_autofree gchar *hdr_txt = g_strdup_printf("● %s PROFILE",
                                                w->is_private ? "PRIVATE" : (cur ? cur->name : "DEFAULT"));
    GtkWidget *hdr = gtk_label_new(hdr_txt);
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_widget_add_css_class(hdr, "web-popover-title");
    gtk_box_append(GTK_BOX(box), hdr);

    if (!w->is_private && w->sync) {
        GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(sbox, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(sbox, "web-dl-card");
        GtkWidget *sl = gtk_label_new(web_sync_get_status(w->sync));
        gtk_widget_set_hexpand(sl, TRUE);
        gtk_widget_set_halign(sl, GTK_ALIGN_START);
        gtk_widget_set_valign(sl, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(sl, "web-badge-telemetry");
        GtkWidget *sb = gtk_button_new_with_label(web_sync_is_connected(w->sync) ? "↻ SYNC" : "SIGN IN");
        gtk_widget_add_css_class(sb, "action-btn-primary");
        g_signal_connect(sb, "clicked", G_CALLBACK(on_sync_btn_clicked), w);
        gtk_box_append(GTK_BOX(sbox), sl);
        gtk_box_append(GTK_BOX(sbox), sb);
        gtk_box_append(GTK_BOX(box), sbox);
    }

    if (reg) {
        for (guint i = 0; i < reg->profiles->len; i++) {
            KajoWebProfileInfo *p = g_ptr_array_index(reg->profiles, i);
            g_autofree gchar *plbl = g_strdup_printf("%s  %s%s", p->icon ? p->icon : "🌐",
                                                     p->name,
                                                     (g_strcmp0(p->id, w->profile_id) == 0 && !w->is_private) ? "  (ACTIVE)" : "");
            GtkWidget *pb = gtk_button_new_with_label(plbl);
            gtk_widget_add_css_class(pb, "files-nav-btn");
            ProfSwitchCtx *pc = g_new0(ProfSwitchCtx, 1);
            pc->w = w;
            pc->profile_id = g_strdup(p->id);
            g_signal_connect_data(pb, "clicked", G_CALLBACK(on_switch_profile_clicked), pc, prof_switch_ctx_free, 0);
            gtk_box_append(GTK_BOX(box), pb);
        }
    }

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_pick = gtk_button_new_with_label("⊞ PROFILE PICKER");
    gtk_widget_set_hexpand(b_pick, TRUE);
    gtk_widget_add_css_class(b_pick, "files-nav-btn");
    g_signal_connect(b_pick, "clicked", G_CALLBACK(on_open_picker_clicked), w);

    GtkWidget *b_priv = gtk_button_new_with_label("🕶 PRIVATE");
    gtk_widget_set_hexpand(b_priv, TRUE);
    gtk_widget_add_css_class(b_priv, "files-nav-btn");
    g_signal_connect(b_priv, "clicked", G_CALLBACK(on_private_win_clicked), w);

    gtk_box_append(GTK_BOX(foot), b_pick);
    gtk_box_append(GTK_BOX(foot), b_priv);
    gtk_box_append(GTK_BOX(box), foot);

    gtk_popover_set_child(GTK_POPOVER(w->pop_profile), box);
    gtk_popover_popup(GTK_POPOVER(w->pop_profile));
}

void
kajo_web_win_setup_popovers(KajoWebWindow *w)
{
    w->pop_shield = gtk_popover_new();
    gtk_widget_set_parent(w->pop_shield, w->btn_shield);
    gtk_widget_add_css_class(w->pop_shield, "web-popover");

    w->pop_media = gtk_popover_new();
    gtk_widget_set_parent(w->pop_media, w->btn_media);
    gtk_widget_add_css_class(w->pop_media, "web-popover");

    w->pop_downloads = gtk_popover_new();
    gtk_widget_set_parent(w->pop_downloads, w->btn_downloads);
    gtk_widget_add_css_class(w->pop_downloads, "web-popover");

    w->pop_profile = gtk_popover_new();
    gtk_widget_set_parent(w->pop_profile, w->btn_profile);
    gtk_widget_add_css_class(w->pop_profile, "web-popover");
}
