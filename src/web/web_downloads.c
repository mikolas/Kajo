#include "web_downloads.h"
#include "../common/spawn_detached.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

typedef enum {
    DL_STATE_RUNNING = 0,
    DL_STATE_DONE,
    DL_STATE_FAILED,
    DL_STATE_CANCELLED,
} DlState;

typedef struct {
    guint64        id;
    gchar         *name;
    gchar         *dest_path;
    gchar         *status_detail;
    gdouble        progress; /* 0.0..1.0, <0 indeterminate */
    DlState        state;
    WebKitDownload *wk_dl;
    GSubprocess    *proc;
    GCancellable   *cancellable;
    gchar          *temp_hdr_path;
    KajoWebDownloads *owner;
} DlJob;

struct _KajoWebDownloads {
    GPtrArray                 *jobs; /* DlJob* */
    guint64                    next_id;
    KajoWebDownloadsChangedCb  on_changed;
    gpointer                   user_data;
};

static void
dl_job_free(gpointer ptr)
{
    DlJob *j = ptr;
    if (!j)
        return;
    if (j->cancellable)
        g_cancellable_cancel(j->cancellable);
    g_clear_object(&j->cancellable);
    if (j->proc) {
        g_subprocess_force_exit(j->proc);
        g_clear_object(&j->proc);
    }
    g_clear_object(&j->wk_dl);
    if (j->temp_hdr_path) {
        g_unlink(j->temp_hdr_path);
        g_free(j->temp_hdr_path);
    }
    g_free(j->name);
    g_free(j->dest_path);
    g_free(j->status_detail);
    g_free(j);
}

static void
notify_changed(KajoWebDownloads *dl)
{
    if (!dl || !dl->on_changed)
        return;
    guint active = 0;
    for (guint i = 0; i < dl->jobs->len; i++) {
        DlJob *j = g_ptr_array_index(dl->jobs, i);
        if (j->state == DL_STATE_RUNNING)
            active++;
    }
    dl->on_changed(dl, active, dl->jobs->len, dl->user_data);
}

static gchar *
resolve_download_dir(const KajoWebCfg *cfg)
{
    if (cfg && cfg->download_dir && *cfg->download_dir) {
        g_mkdir_with_parents(cfg->download_dir, 0755);
        return g_strdup(cfg->download_dir);
    }
    const gchar *xdg = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    gchar *dir = g_strdup(xdg ? xdg : g_get_home_dir());
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

static gchar *
sanitize_filename(const gchar *raw, const gchar *fallback_ext)
{
    GString *s = g_string_new(NULL);
    for (const char *p = raw ? raw : ""; *p && s->len < 80; p++) {
        unsigned char c = (unsigned char)*p;
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.' || c == ' ')
            g_string_append_c(s, (char)c);
    }
    g_strstrip(s->str);
    if (s->len == 0)
        g_string_printf(s, "kajo-media-%" G_GINT64_FORMAT, g_get_real_time() / 1000000);
    if (fallback_ext && !g_str_has_suffix(s->str, fallback_ext))
        g_string_append(s, fallback_ext);
    return g_string_free(s, FALSE);
}

KajoWebDownloads *
web_downloads_new(KajoWebDownloadsChangedCb on_changed, gpointer user_data)
{
    KajoWebDownloads *dl = g_new0(KajoWebDownloads, 1);
    dl->jobs = g_ptr_array_new_with_free_func(dl_job_free);
    dl->next_id = 1;
    dl->on_changed = on_changed;
    dl->user_data = user_data;
    return dl;
}

void
web_downloads_free(KajoWebDownloads *dl)
{
    if (!dl)
        return;
    g_ptr_array_unref(dl->jobs);
    g_free(dl);
}

gboolean
web_downloads_has_ffmpeg(void)
{
    g_autofree gchar *p = g_find_program_in_path("ffmpeg");
    return p != NULL;
}

gboolean
web_downloads_has_ytdlp(void)
{
    g_autofree gchar *p = g_find_program_in_path("yt-dlp");
    return p != NULL;
}

void
web_downloads_open_in_kajo_player(const gchar *uri_or_path)
{
    if (!uri_or_path || !*uri_or_path)
        return;
    g_autofree gchar *player = g_find_program_in_path("kajo-player");
    const gchar *bin = player ? player : "kajo-player";
    const gchar *argv[] = { bin, uri_or_path, NULL };
    kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
}

static void
open_in_kajo_files(const gchar *path)
{
    if (!path || !*path)
        return;
    g_autofree gchar *dir = g_path_get_dirname(path);
    g_autofree gchar *files = g_find_program_in_path("kajo-files");
    const gchar *bin = files ? files : "kajo-files";
    const gchar *argv[] = { bin, dir, NULL };
    kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
}

/* ─── Tier 1: Direct WebKitDownload ─── */

static gboolean
on_wk_decide_dest(WebKitDownload *wk, gchar *suggested, gpointer user_data)
{
    DlJob *j = user_data;
    g_autofree gchar *dir = resolve_download_dir(NULL);
    g_autofree gchar *clean = sanitize_filename(suggested, NULL);
    g_free(j->name);
    j->name = g_strdup(clean);
    g_free(j->dest_path);
    j->dest_path = g_build_filename(dir, clean, NULL);
    webkit_download_set_destination(wk, j->dest_path);
    notify_changed(j->owner);
    return TRUE;
}

static void
on_wk_progress(WebKitDownload *wk, GParamSpec *ps G_GNUC_UNUSED, gpointer user_data)
{
    DlJob *j = user_data;
    j->progress = webkit_download_get_estimated_progress(wk);
    g_free(j->status_detail);
    j->status_detail = g_strdup_printf("%d%%", (int)(j->progress * 100.0));
    notify_changed(j->owner);
}

static void
on_wk_finished(WebKitDownload *wk G_GNUC_UNUSED, gpointer user_data)
{
    DlJob *j = user_data;
    if (j->state == DL_STATE_RUNNING) {
        j->state = DL_STATE_DONE;
        j->progress = 1.0;
        g_free(j->status_detail);
        j->status_detail = g_strdup("DONE");
        notify_changed(j->owner);
    }
}

static void
on_wk_failed(WebKitDownload *wk G_GNUC_UNUSED, GError *err, gpointer user_data)
{
    DlJob *j = user_data;
    j->state = DL_STATE_FAILED;
    g_free(j->status_detail);
    j->status_detail = g_strdup(err ? err->message : "FAILED");
    notify_changed(j->owner);
}

void
web_downloads_track_webkit(KajoWebDownloads *dl, WebKitDownload *wk_dl, const KajoWebCfg *cfg G_GNUC_UNUSED)
{
    if (!dl || !wk_dl)
        return;
    DlJob *j = g_new0(DlJob, 1);
    j->id = dl->next_id++;
    j->name = g_strdup("Download...");
    j->status_detail = g_strdup("0%");
    j->state = DL_STATE_RUNNING;
    j->wk_dl = g_object_ref(wk_dl);
    j->owner = dl;
    g_ptr_array_add(dl->jobs, j);

    g_signal_connect(wk_dl, "decide-destination", G_CALLBACK(on_wk_decide_dest), j);
    g_signal_connect(wk_dl, "notify::estimated-progress", G_CALLBACK(on_wk_progress), j);
    g_signal_connect(wk_dl, "finished", G_CALLBACK(on_wk_finished), j);
    g_signal_connect(wk_dl, "failed", G_CALLBACK(on_wk_failed), j);
    notify_changed(dl);
}

/* ─── Subprocess Async Finish Helper (ffmpeg / yt-dlp) ─── */

static void
on_subproc_wait_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    g_autoptr(GError) err = NULL;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    DlJob *j = user_data;
    if (j->temp_hdr_path) {
        g_unlink(j->temp_hdr_path);
        g_clear_pointer(&j->temp_hdr_path, g_free);
    }
    if (ok) {
        j->state = DL_STATE_DONE;
        j->progress = 1.0;
        g_free(j->status_detail);
        j->status_detail = g_strdup("DONE");
    } else {
        j->state = DL_STATE_FAILED;
        g_free(j->status_detail);
        j->status_detail = g_strdup(err ? err->message : "FAILED");
    }
    g_clear_object(&j->proc);
    notify_changed(j->owner);
}

/* ─── Tier 2: VDH-Style Sniffed HLS/DASH Remux via System ffmpeg ─── */

typedef struct {
    KajoWebDownloads *dl;
    DlJob            *job;
    gchar            *stream_uri;
    gchar            *referer;
} FfmpegLaunchCtx;

static void
launch_ffmpeg_with_cookies(FfmpegLaunchCtx *ctx, const gchar *cookie_hdr)
{
    DlJob *j = ctx->job;
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("ffmpeg"));
    g_ptr_array_add(argv, g_strdup("-y"));
    g_ptr_array_add(argv, g_strdup("-loglevel"));
    g_ptr_array_add(argv, g_strdup("error"));

    if ((ctx->referer && *ctx->referer) || (cookie_hdr && *cookie_hdr)) {
        GString *hdrs = g_string_new(NULL);
        if (ctx->referer && *ctx->referer)
            g_string_append_printf(hdrs, "Referer: %s\r\n", ctx->referer);
        if (cookie_hdr && *cookie_hdr)
            g_string_append_printf(hdrs, "Cookie: %s\r\n", cookie_hdr);
        g_ptr_array_add(argv, g_strdup("-headers"));
        g_ptr_array_add(argv, g_string_free(hdrs, FALSE));
    }

    g_ptr_array_add(argv, g_strdup("-i"));
    g_ptr_array_add(argv, g_strdup(ctx->stream_uri));
    g_ptr_array_add(argv, g_strdup("-c"));
    g_ptr_array_add(argv, g_strdup("copy"));
    g_ptr_array_add(argv, g_strdup("-movflags"));
    g_ptr_array_add(argv, g_strdup("+faststart"));
    g_ptr_array_add(argv, g_strdup(j->dest_path));
    g_ptr_array_add(argv, NULL);

    g_autoptr(GError) err = NULL;
    j->proc = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &err);
    g_ptr_array_unref(argv);

    if (!j->proc) {
        j->state = DL_STATE_FAILED;
        g_free(j->status_detail);
        j->status_detail = g_strdup(err ? err->message : "ffmpeg launch failed");
        notify_changed(ctx->dl);
    } else {
        g_subprocess_wait_check_async(j->proc, j->cancellable, on_subproc_wait_done, j);
    }

    g_free(ctx->stream_uri);
    g_free(ctx->referer);
    g_free(ctx);
}

static void
on_cookies_for_ffmpeg(GObject *src, GAsyncResult *res, gpointer user_data)
{
    FfmpegLaunchCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    GList *cookies = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
        g_free(ctx->stream_uri);
        g_free(ctx->referer);
        g_free(ctx);
        return;
    }

    GString *hdr = g_string_new(NULL);
    for (GList *l = cookies; l; l = l->next) {
        SoupCookie *c = l->data;
        if (hdr->len > 0)
            g_string_append(hdr, "; ");
        g_string_append_printf(hdr, "%s=%s", soup_cookie_get_name(c), soup_cookie_get_value(c));
    }
    g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);

    launch_ffmpeg_with_cookies(ctx, hdr->len > 0 ? hdr->str : NULL);
    g_string_free(hdr, TRUE);
}

void
web_downloads_start_ffmpeg_stream(KajoWebDownloads         *dl,
                                  const KajoWebMediaStream *stream,
                                  const gchar              *page_title,
                                  const gchar              *referer_uri,
                                  WebKitNetworkSession     *session,
                                  const KajoWebCfg         *cfg)
{
    if (!dl || !stream || !stream->uri || !*stream->uri)
        return;
    g_autofree gchar *dir = resolve_download_dir(cfg);
    g_autofree gchar *fname = sanitize_filename(page_title && *page_title ? page_title : stream->label, ".mp4");

    DlJob *j = g_new0(DlJob, 1);
    j->id = dl->next_id++;
    j->name = g_strdup(fname);
    j->dest_path = g_build_filename(dir, fname, NULL);
    j->status_detail = g_strdup("REMUXING (ffmpeg)...");
    j->progress = -1.0;
    j->state = DL_STATE_RUNNING;
    j->cancellable = g_cancellable_new();
    j->owner = dl;
    g_ptr_array_add(dl->jobs, j);
    notify_changed(dl);

    FfmpegLaunchCtx *ctx = g_new0(FfmpegLaunchCtx, 1);
    ctx->dl = dl;
    ctx->job = j;
    ctx->stream_uri = g_strdup(stream->uri);
    ctx->referer = g_strdup(referer_uri ? referer_uri : "");

    WebKitCookieManager *cm = session ? webkit_network_session_get_cookie_manager(session) : NULL;
    if (cm && referer_uri && *referer_uri) {
        webkit_cookie_manager_get_cookies(cm, referer_uri, j->cancellable, on_cookies_for_ffmpeg, ctx);
    } else {
        launch_ffmpeg_with_cookies(ctx, NULL);
    }
}

/* ─── Tier 3: Optional yt-dlp + ffmpeg ─── */

void
web_downloads_start_ytdlp(KajoWebDownloads   *dl,
                          const gchar        *page_uri,
                          KajoWebDownloadFmt  fmt,
                          const KajoWebCfg   *cfg)
{
    if (!dl || !page_uri || !*page_uri || !web_downloads_has_ytdlp())
        return;

    g_autofree gchar *dir = resolve_download_dir(cfg);
    g_autofree gchar *out_tmpl = g_build_filename(dir, "%(title).80s.%(ext)s", NULL);

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("yt-dlp"));
    g_ptr_array_add(argv, g_strdup("--no-playlist"));
    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup(out_tmpl));

    switch (fmt) {
    case KAJO_WEB_DL_1080P:
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("bestvideo[height<=1080]+bestaudio/best[height<=1080]"));
        g_ptr_array_add(argv, g_strdup("--merge-output-format"));
        g_ptr_array_add(argv, g_strdup("mp4"));
        break;
    case KAJO_WEB_DL_720P:
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("bestvideo[height<=720]+bestaudio/best[height<=720]"));
        g_ptr_array_add(argv, g_strdup("--merge-output-format"));
        g_ptr_array_add(argv, g_strdup("mp4"));
        break;
    case KAJO_WEB_DL_AUDIO_OPUS:
        g_ptr_array_add(argv, g_strdup("-x"));
        g_ptr_array_add(argv, g_strdup("--audio-format"));
        g_ptr_array_add(argv, g_strdup("opus"));
        break;
    case KAJO_WEB_DL_AUDIO_MP3:
        g_ptr_array_add(argv, g_strdup("-x"));
        g_ptr_array_add(argv, g_strdup("--audio-format"));
        g_ptr_array_add(argv, g_strdup("mp3"));
        break;
    case KAJO_WEB_DL_BEST_MP4:
    default:
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("bestvideo+bestaudio/best"));
        g_ptr_array_add(argv, g_strdup("--merge-output-format"));
        g_ptr_array_add(argv, g_strdup("mp4"));
        break;
    }
    g_ptr_array_add(argv, g_strdup(page_uri));
    g_ptr_array_add(argv, NULL);

    DlJob *j = g_new0(DlJob, 1);
    j->id = dl->next_id++;
    j->name = g_strdup_printf("yt-dlp: %s", page_uri);
    j->dest_path = g_strdup(dir);
    j->status_detail = g_strdup("DOWNLOADING (yt-dlp)...");
    j->progress = -1.0;
    j->state = DL_STATE_RUNNING;
    j->cancellable = g_cancellable_new();
    j->owner = dl;
    g_ptr_array_add(dl->jobs, j);

    g_autoptr(GError) err = NULL;
    j->proc = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &err);
    g_ptr_array_unref(argv);

    if (!j->proc) {
        j->state = DL_STATE_FAILED;
        g_free(j->status_detail);
        j->status_detail = g_strdup(err ? err->message : "yt-dlp launch failed");
    } else {
        g_subprocess_wait_check_async(j->proc, j->cancellable, on_subproc_wait_done, j);
    }
    notify_changed(dl);
}

/* ─── Aero + Metro Downloads Popover Content ─── */

static void
on_btn_show_files(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const gchar *p = g_object_get_data(G_OBJECT(btn), "dl-path");
    open_in_kajo_files(p);
}

static void
on_btn_open_player(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    const gchar *p = g_object_get_data(G_OBJECT(btn), "dl-path");
    web_downloads_open_in_kajo_player(p);
}

GtkWidget *
web_downloads_build_popover_content(KajoWebDownloads *dl)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, 320, -1);
    gtk_widget_add_css_class(box, "web-popover-box");

    GtkWidget *hdr = gtk_label_new("↓ DOWNLOADS");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_widget_add_css_class(hdr, "web-popover-title");
    gtk_box_append(GTK_BOX(box), hdr);

    if (!dl || dl->jobs->len == 0) {
        GtkWidget *empty = gtk_label_new("No downloads in this session.");
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_widget_add_css_class(empty, "web-dim-label");
        gtk_box_append(GTK_BOX(box), empty);
        return box;
    }

    for (gint i = (gint)dl->jobs->len - 1; i >= 0; i--) {
        DlJob *j = g_ptr_array_index(dl->jobs, (guint)i);
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_add_css_class(card, "web-dl-card");

        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(top, GTK_ALIGN_CENTER);
        GtkWidget *nm = gtk_label_new(j->name);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(nm, TRUE);
        gtk_widget_set_halign(nm, GTK_ALIGN_START);
        gtk_widget_set_valign(nm, GTK_ALIGN_CENTER);
        GtkWidget *st = gtk_label_new(j->status_detail);
        gtk_widget_add_css_class(st, "web-badge-telemetry");
        gtk_widget_set_valign(st, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(top), nm);
        gtk_box_append(GTK_BOX(top), st);
        gtk_box_append(GTK_BOX(card), top);

        if (j->state == DL_STATE_DONE && j->dest_path) {
            GtkWidget *acts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *b_files = gtk_button_new_with_label("📁 KAJO FILES");
            gtk_widget_add_css_class(b_files, "web-mini-btn");
            g_object_set_data_full(G_OBJECT(b_files), "dl-path", g_strdup(j->dest_path), g_free);
            g_signal_connect(b_files, "clicked", G_CALLBACK(on_btn_show_files), NULL);
            gtk_box_append(GTK_BOX(acts), b_files);

            if (g_str_has_suffix(j->dest_path, ".mp4") || g_str_has_suffix(j->dest_path, ".mkv") ||
                g_str_has_suffix(j->dest_path, ".webm") || g_str_has_suffix(j->dest_path, ".opus") ||
                g_str_has_suffix(j->dest_path, ".mp3")) {
                GtkWidget *b_play = gtk_button_new_with_label("▶ KAJO PLAYER");
                gtk_widget_add_css_class(b_play, "web-mini-btn");
                g_object_set_data_full(G_OBJECT(b_play), "dl-path", g_strdup(j->dest_path), g_free);
                g_signal_connect(b_play, "clicked", G_CALLBACK(on_btn_open_player), NULL);
                gtk_box_append(GTK_BOX(acts), b_play);
            }
            gtk_box_append(GTK_BOX(card), acts);
        }
        gtk_box_append(GTK_BOX(box), card);
    }
    return box;
}
