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
    GtkWindow                 *parent_win;
    gchar                     *default_dir;
    gboolean                   ask_location;
    GCancellable              *alive;
};

typedef struct {
    KajoWebDownloads *dl;
    guint64           job_id;
    GCancellable     *alive;
} DlAsyncRef;

static DlAsyncRef *
dl_async_ref_new(KajoWebDownloads *dl, guint64 job_id, GCancellable *alive)
{
    DlAsyncRef *r = g_new0(DlAsyncRef, 1);
    r->dl = dl;
    r->job_id = job_id;
    r->alive = alive ? g_object_ref(alive) : NULL;
    return r;
}

static void
dl_async_ref_free(DlAsyncRef *r)
{
    if (!r)
        return;
    g_clear_object(&r->alive);
    g_free(r);
}

static DlJob *
find_job_by_id(KajoWebDownloads *dl, guint64 job_id)
{
    if (!dl || !dl->jobs)
        return NULL;
    for (guint i = 0; i < dl->jobs->len; i++) {
        DlJob *j = g_ptr_array_index(dl->jobs, i);
        if (j && j->id == job_id)
            return j;
    }
    return NULL;
}

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
    if (j->wk_dl)
        g_signal_handlers_disconnect_by_data(j->wk_dl, j);
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

gchar *
web_downloads_resolve_dir(const KajoWebCfg *cfg)
{
    if (cfg && cfg->download_dir && *cfg->download_dir) {
        const gchar *raw = cfg->download_dir;
        gchar *dir = NULL;
        if (g_strcmp0(raw, "~") == 0)
            dir = g_strdup(g_get_home_dir());
        else if (g_str_has_prefix(raw, "~/"))
            dir = g_build_filename(g_get_home_dir(), raw + 2, NULL);
        else
            dir = g_strdup(raw);
        g_mkdir_with_parents(dir, 0755);
        return dir;
    }
    const gchar *xdg = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    gchar *dir = g_strdup(xdg ? xdg : g_get_home_dir());
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

static gchar *
resolve_owner_download_dir(const KajoWebDownloads *dl, const KajoWebCfg *cfg)
{
    if (cfg && cfg->download_dir && *cfg->download_dir)
        return web_downloads_resolve_dir(cfg);
    if (dl && dl->default_dir && *dl->default_dir) {
        g_mkdir_with_parents(dl->default_dir, 0755);
        return g_strdup(dl->default_dir);
    }
    return web_downloads_resolve_dir(NULL);
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
    dl->alive = g_cancellable_new();
    return dl;
}

void
web_downloads_free(KajoWebDownloads *dl)
{
    if (!dl)
        return;
    if (dl->alive)
        g_cancellable_cancel(dl->alive);
    g_clear_object(&dl->alive);
    if (dl->parent_win) {
        g_object_remove_weak_pointer(G_OBJECT(dl->parent_win), (gpointer *)&dl->parent_win);
        dl->parent_win = NULL;
    }
    g_free(dl->default_dir);
    g_clear_pointer(&dl->jobs, g_ptr_array_unref);
    g_free(dl);
}

void
web_downloads_set_parent(KajoWebDownloads *dl, GtkWindow *parent)
{
    if (!dl)
        return;
    if (dl->parent_win) {
        g_object_remove_weak_pointer(G_OBJECT(dl->parent_win), (gpointer *)&dl->parent_win);
        dl->parent_win = NULL;
    }
    if (parent) {
        dl->parent_win = parent;
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&dl->parent_win);
    }
}

void
web_downloads_apply_config(KajoWebDownloads *dl, const KajoWebCfg *cfg)
{
    if (!dl || !cfg)
        return;
    g_free(dl->default_dir);
    dl->default_dir = web_downloads_resolve_dir(cfg);
    dl->ask_location = cfg->ask_download_location;
}

void
web_downloads_cancel_job(KajoWebDownloads *dl, guint64 job_id)
{
    DlJob *j = find_job_by_id(dl, job_id);
    if (!j || j->state != DL_STATE_RUNNING)
        return;
    j->state = DL_STATE_CANCELLED;
    g_free(j->status_detail);
    j->status_detail = g_strdup("CANCELLED");
    if (j->cancellable)
        g_cancellable_cancel(j->cancellable);
    if (j->proc) {
        g_subprocess_force_exit(j->proc);
        g_clear_object(&j->proc);
    }
    if (j->wk_dl)
        webkit_download_cancel(j->wk_dl);
    notify_changed(dl);
}

void
web_downloads_clear_finished(KajoWebDownloads *dl)
{
    if (!dl || !dl->jobs)
        return;
    for (gint i = (gint)dl->jobs->len - 1; i >= 0; i--) {
        DlJob *j = g_ptr_array_index(dl->jobs, (guint)i);
        if (j && j->state != DL_STATE_RUNNING)
            g_ptr_array_remove_index(dl->jobs, (guint)i);
    }
    notify_changed(dl);
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
    g_autofree gchar *dir = g_file_test(path, G_FILE_TEST_IS_DIR)
        ? g_strdup(path)
        : g_path_get_dirname(path);
    g_autofree gchar *files = g_find_program_in_path("kajo-files");
    const gchar *bin = files ? files : "kajo-files";
    const gchar *argv[] = { bin, dir, NULL };
    kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
}

/* ─── Tier 1: Direct WebKitDownload ─── */

typedef struct {
    KajoWebDownloads *dl;
    guint64           job_id;
    WebKitDownload   *wk_dl;
    GCancellable     *alive;
} DlSaveCtx;

static void
dl_save_ctx_free(DlSaveCtx *c)
{
    if (!c)
        return;
    g_clear_object(&c->wk_dl);
    g_clear_object(&c->alive);
    g_free(c);
}

static void
on_file_dialog_save_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DlSaveCtx *c = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);

    if (!c->alive || g_cancellable_is_cancelled(c->alive)) {
        dl_save_ctx_free(c);
        return;
    }

    DlJob *j = find_job_by_id(c->dl, c->job_id);
    if (!file) {
        if (j) {
            j->state = DL_STATE_CANCELLED;
            g_free(j->status_detail);
            j->status_detail = g_strdup("CANCELLED");
        }
        webkit_download_cancel(c->wk_dl);
        notify_changed(c->dl);
        dl_save_ctx_free(c);
        return;
    }

    g_autofree gchar *chosen = g_file_get_path(file);
    if (!chosen || !*chosen) {
        webkit_download_cancel(c->wk_dl);
        dl_save_ctx_free(c);
        return;
    }
    if (j) {
        g_free(j->dest_path);
        j->dest_path = g_strdup(chosen);
        g_free(j->name);
        j->name = g_path_get_basename(chosen);
    }
    webkit_download_set_allow_overwrite(c->wk_dl, TRUE);
    webkit_download_set_destination(c->wk_dl, chosen);
    notify_changed(c->dl);
    dl_save_ctx_free(c);
}

static gboolean
on_wk_decide_dest(WebKitDownload *wk, gchar *suggested, gpointer user_data)
{
    DlJob *j = user_data;
    KajoWebDownloads *dl = j->owner;
    g_autofree gchar *dir = resolve_owner_download_dir(dl, NULL);
    g_autofree gchar *clean = sanitize_filename(suggested, NULL);
    g_free(j->name);
    j->name = g_strdup(clean);
    g_free(j->dest_path);
    j->dest_path = g_build_filename(dir, clean, NULL);

    if (dl && dl->ask_location && dl->parent_win && GTK_IS_WINDOW(dl->parent_win)) {
        g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
        g_autoptr(GFile) init_dir = g_file_new_for_path(dir);
        gtk_file_dialog_set_title(dlg, "Save Download As");
        gtk_file_dialog_set_initial_folder(dlg, init_dir);
        gtk_file_dialog_set_initial_name(dlg, clean);

        DlSaveCtx *c = g_new0(DlSaveCtx, 1);
        c->dl = dl;
        c->job_id = j->id;
        c->wk_dl = g_object_ref(wk);
        c->alive = dl->alive ? g_object_ref(dl->alive) : NULL;
        gtk_file_dialog_save(dlg, dl->parent_win, dl->alive, on_file_dialog_save_done, c);
        notify_changed(dl);
        return TRUE;
    }

    webkit_download_set_destination(wk, j->dest_path);
    notify_changed(dl);
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
    if (j->state == DL_STATE_CANCELLED ||
        g_error_matches(err, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) {
        j->state = DL_STATE_CANCELLED;
        g_free(j->status_detail);
        j->status_detail = g_strdup("CANCELLED");
    } else {
        j->state = DL_STATE_FAILED;
        g_free(j->status_detail);
        j->status_detail = g_strdup(err ? err->message : "FAILED");
    }
    notify_changed(j->owner);
}

void
web_downloads_track_webkit(KajoWebDownloads *dl, WebKitDownload *wk_dl, const KajoWebCfg *cfg)
{
    if (!dl || !wk_dl)
        return;
    if (cfg)
        web_downloads_apply_config(dl, cfg);
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
    DlAsyncRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        !ref->alive || g_cancellable_is_cancelled(ref->alive)) {
        dl_async_ref_free(ref);
        return;
    }

    DlJob *j = find_job_by_id(ref->dl, ref->job_id);
    if (j) {
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
        notify_changed(ref->dl);
    }
    dl_async_ref_free(ref);
}

/* ─── Tier 2: VDH-Style Sniffed HLS/DASH Remux via System ffmpeg ─── */

typedef struct {
    KajoWebDownloads *dl;
    guint64           job_id;
    GCancellable     *alive;
    gchar            *stream_uri;
    gchar            *referer;
} FfmpegLaunchCtx;

static void
ffmpeg_launch_ctx_free(FfmpegLaunchCtx *ctx)
{
    if (!ctx)
        return;
    g_clear_object(&ctx->alive);
    g_free(ctx->stream_uri);
    g_free(ctx->referer);
    g_free(ctx);
}

static void
launch_ffmpeg_with_cookies(FfmpegLaunchCtx *ctx, const gchar *cookie_hdr)
{
    if (!ctx->alive || g_cancellable_is_cancelled(ctx->alive)) {
        ffmpeg_launch_ctx_free(ctx);
        return;
    }
    DlJob *j = find_job_by_id(ctx->dl, ctx->job_id);
    if (!j) {
        ffmpeg_launch_ctx_free(ctx);
        return;
    }

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
        g_subprocess_wait_check_async(j->proc, j->cancellable, on_subproc_wait_done,
                                      dl_async_ref_new(ctx->dl, j->id, j->cancellable));
    }

    ffmpeg_launch_ctx_free(ctx);
}

static void
on_cookies_for_ffmpeg(GObject *src, GAsyncResult *res, gpointer user_data)
{
    FfmpegLaunchCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    GList *cookies = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(src), res, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        !ctx->alive || g_cancellable_is_cancelled(ctx->alive)) {
        g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
        ffmpeg_launch_ctx_free(ctx);
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
    g_autofree gchar *dir = resolve_owner_download_dir(dl, cfg);
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
    ctx->job_id = j->id;
    ctx->alive = g_object_ref(j->cancellable);
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

static void
on_ytdlp_comm_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DlAsyncRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autofree gchar *stdout_buf = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &stdout_buf, NULL, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        !ref->alive || g_cancellable_is_cancelled(ref->alive)) {
        dl_async_ref_free(ref);
        return;
    }

    DlJob *j = find_job_by_id(ref->dl, ref->job_id);
    if (j) {
        gboolean exited_ok = ok && g_subprocess_get_successful(G_SUBPROCESS(src));
        if (exited_ok) {
            if (stdout_buf && *stdout_buf) {
                g_strstrip(stdout_buf);
                /* Multiple lines possible (e.g. formats); take last non-empty line */
                g_auto(GStrv) lines = g_strsplit(stdout_buf, "\n", -1);
                const gchar *last = NULL;
                for (int k = 0; lines && lines[k]; k++) {
                    g_strstrip(lines[k]);
                    if (*lines[k])
                        last = lines[k];
                }
                if (last && g_file_test(last, G_FILE_TEST_EXISTS)) {
                    g_free(j->dest_path);
                    j->dest_path = g_strdup(last);
                    g_free(j->name);
                    j->name = g_path_get_basename(last);
                }
            }
            j->state = DL_STATE_DONE;
            j->progress = 1.0;
            g_free(j->status_detail);
            j->status_detail = g_strdup("DONE");
        } else {
            j->state = DL_STATE_FAILED;
            g_free(j->status_detail);
            j->status_detail = g_strdup(err ? err->message : "yt-dlp failed");
        }
        g_clear_object(&j->proc);
        notify_changed(ref->dl);
    }
    dl_async_ref_free(ref);
}

void
web_downloads_start_ytdlp(KajoWebDownloads   *dl,
                          const gchar        *page_uri,
                          KajoWebDownloadFmt  fmt,
                          const KajoWebCfg   *cfg)
{
    if (!dl || !page_uri || !*page_uri || !web_downloads_has_ytdlp())
        return;

    g_autofree gchar *dir = resolve_owner_download_dir(dl, cfg);
    g_autofree gchar *out_tmpl = g_build_filename(dir, "%(title).80s.%(ext)s", NULL);

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("yt-dlp"));
    g_ptr_array_add(argv, g_strdup("--no-playlist"));
    g_ptr_array_add(argv, g_strdup("--print"));
    g_ptr_array_add(argv, g_strdup("after_move:filepath"));
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
                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &err);
    g_ptr_array_unref(argv);

    if (!j->proc) {
        j->state = DL_STATE_FAILED;
        g_free(j->status_detail);
        j->status_detail = g_strdup(err ? err->message : "yt-dlp launch failed");
    } else {
        g_subprocess_communicate_utf8_async(j->proc, NULL, j->cancellable, on_ytdlp_comm_done,
                                            dl_async_ref_new(dl, j->id, j->cancellable));
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

typedef struct {
    KajoWebDownloads *dl;
    guint64           job_id;
} DlCancelBtnCtx;

static void
dl_free_closure(gpointer data, GClosure *c G_GNUC_UNUSED)
{
    g_free(data);
}

static void
on_btn_cancel_job(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    DlCancelBtnCtx *c = user_data;
    web_downloads_cancel_job(c->dl, c->job_id);
}

static void
on_btn_clear_done(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebDownloads *dl = user_data;
    web_downloads_clear_finished(dl);
}

static void
on_btn_open_folder(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebDownloads *dl = user_data;
    g_autofree gchar *dir = resolve_owner_download_dir(dl, NULL);
    open_in_kajo_files(dir);
}

GtkWidget *
web_downloads_build_popover_content(KajoWebDownloads *dl)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(root, 380, -1);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(hdr, "shell-popover-header");

    GtkWidget *ht = gtk_label_new("DOWNLOADS");
    gtk_widget_set_hexpand(ht, TRUE);
    gtk_widget_set_halign(ht, GTK_ALIGN_START);
    gtk_widget_set_valign(ht, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(ht, "shell-popover-title");
    gtk_box_append(GTK_BOX(hdr), ht);

    g_autofree gchar *cnt = g_strdup_printf("%u", dl ? dl->jobs->len : 0u);
    GtkWidget *hb = gtk_label_new(cnt);
    gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(hb, "shell-popover-badge");
    gtk_box_append(GTK_BOX(hdr), hb);
    gtk_box_append(GTK_BOX(root), hdr);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(body, "shell-popover-body");
    gtk_box_append(GTK_BOX(root), body);

    gboolean has_finished = FALSE;

    if (!dl || dl->jobs->len == 0) {
        GtkWidget *empty = gtk_label_new("No downloads in this session.");
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_box_append(GTK_BOX(body), empty);
    } else {
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);

        GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        for (gint i = (gint)dl->jobs->len - 1; i >= 0; i--) {
            DlJob *j = g_ptr_array_index(dl->jobs, (guint)i);
            if (j->state != DL_STATE_RUNNING)
                has_finished = TRUE;

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

            if (j->state == DL_STATE_RUNNING) {
                GtkWidget *b_cancel = gtk_button_new_with_label("✕");
                gtk_widget_set_valign(b_cancel, GTK_ALIGN_CENTER);
                gtk_widget_set_tooltip_text(b_cancel, "Cancel Download");
                gtk_widget_add_css_class(b_cancel, "web-mini-btn");
                DlCancelBtnCtx *cc = g_new0(DlCancelBtnCtx, 1);
                cc->dl = dl;
                cc->job_id = j->id;
                g_signal_connect_data(b_cancel, "clicked", G_CALLBACK(on_btn_cancel_job),
                                      cc, dl_free_closure, 0);
                gtk_box_append(GTK_BOX(top), b_cancel);
            }
            gtk_box_append(GTK_BOX(card), top);

            if (j->state == DL_STATE_RUNNING) {
                GtkWidget *pb = gtk_progress_bar_new();
                gtk_widget_add_css_class(pb, "web-dl-progress");
                if (j->progress >= 0.0)
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pb), CLAMP(j->progress, 0.0, 1.0));
                else
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pb), 0.35);
                gtk_box_append(GTK_BOX(card), pb);
            }

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
            gtk_box_append(GTK_BOX(list), card);
        }
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
        gtk_box_append(GTK_BOX(body), scroll);
    }

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(foot, "shell-popover-footer");
    if (has_finished) {
        GtkWidget *b_clr = gtk_button_new_with_label("✓ CLEAR FINISHED");
        gtk_widget_add_css_class(b_clr, "web-mini-btn");
        g_signal_connect(b_clr, "clicked", G_CALLBACK(on_btn_clear_done), dl);
        gtk_box_append(GTK_BOX(foot), b_clr);
    }
    GtkWidget *b_dir = gtk_button_new_with_label("📁 OPEN FOLDER");
    gtk_widget_set_hexpand(b_dir, TRUE);
    gtk_widget_add_css_class(b_dir, "web-mini-btn");
    g_signal_connect(b_dir, "clicked", G_CALLBACK(on_btn_open_folder), dl);
    gtk_box_append(GTK_BOX(foot), b_dir);
    gtk_box_append(GTK_BOX(root), foot);
    return root;
}
