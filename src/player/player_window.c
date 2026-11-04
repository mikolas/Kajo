#include "player_window.h"
#include "player_queue.h"
#include "player_mpris.h"
#include "../compositor/compositor.h"
#include <math.h>
#include <string.h>

struct _KajoPlayerWindow {
    GtkApplicationWindow parent_instance;

    /* Blueprint Template Children */
    GtkWidget *main_vbox;
    GtkWidget *header_handle;
    GtkWidget *title_label;
    GtkWidget *stream_badge;
    GtkWidget *btn_open;
    GtkWidget *btn_queue;
    GtkWidget *btn_float;
    GtkWidget *btn_close;

    GtkOverlay *video_overlay;
    GtkStack *canvas_stack;
    GtkWidget *btn_empty_open;
    GtkWidget *video_picture;
    GtkWidget *audio_title_label;
    GtkWidget *audio_meta_label;

    GtkWidget *queue_vbox;
    GtkWidget *queue_count_badge;
    GtkListBox *queue_listbox;

    GtkWidget *transport_deck;
    GtkWidget *time_elapsed_lbl;
    GtkWidget *seek_scale;
    GtkWidget *time_total_lbl;

    GtkWidget *btn_prev;
    GtkWidget *btn_seek_back;
    GtkWidget *btn_play_pause;
    GtkWidget *btn_seek_fwd;
    GtkWidget *btn_next;
    GtkWidget *btn_loop;
    GtkWidget *transport_status_lbl;
    GtkWidget *btn_mute;
    GtkWidget *volume_scale;
    GtkWidget *volume_pct_lbl;
    GtkWidget *btn_fullscreen;

    /* Playback State */
    GtkMediaStream *media_stream;
    KajoPlayerQueue *queue;
    KajoPlayerMpris *mpris;

    double volume;
    gboolean muted;
    gboolean loop_enabled;
    gboolean queue_visible;
    gboolean is_fullscreen;

    gboolean suppress_seek_signal;
    gboolean suppress_vol_signal;

    guint hud_hide_timer_id;
    double last_motion_x;
    double last_motion_y;
};

G_DEFINE_FINAL_TYPE(KajoPlayerWindow, kajo_player_window, GTK_TYPE_APPLICATION_WINDOW)

static void update_stream_badge(KajoPlayerWindow *self);
static void update_transport_ui(KajoPlayerWindow *self);
static void sync_mpris_state(KajoPlayerWindow *self);

/* --- Fullscreen Auto-Hide Chrome --- */

static void
show_fullscreen_chrome(KajoPlayerWindow *self)
{
    gtk_widget_set_visible(self->header_handle, TRUE);
    gtk_widget_set_visible(self->transport_deck, TRUE);
}

static gboolean
on_hud_hide_timeout(gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    self->hud_hide_timer_id = 0;

    if (self->is_fullscreen && !self->queue_visible &&
        self->media_stream && gtk_media_stream_get_playing(self->media_stream)) {
        gtk_widget_set_visible(self->header_handle, FALSE);
        gtk_widget_set_visible(self->transport_deck, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_hud_hide(KajoPlayerWindow *self)
{
    if (self->hud_hide_timer_id > 0) {
        g_source_remove(self->hud_hide_timer_id);
        self->hud_hide_timer_id = 0;
    }
    if (self->is_fullscreen) {
        self->hud_hide_timer_id = g_timeout_add_seconds(3, on_hud_hide_timeout, self);
    }
}

static void
on_pointer_motion(GtkEventControllerMotion *ctrl G_GNUC_UNUSED,
                  double x,
                  double y,
                  gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (fabs(x - self->last_motion_x) < 2.0 && fabs(y - self->last_motion_y) < 2.0)
        return;

    self->last_motion_x = x;
    self->last_motion_y = y;

    if (self->is_fullscreen) {
        show_fullscreen_chrome(self);
        schedule_hud_hide(self);
    }
}

static void
toggle_fullscreen(KajoPlayerWindow *self)
{
    if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
        gtk_window_unfullscreen(GTK_WINDOW(self));
    else
        gtk_window_fullscreen(GTK_WINDOW(self));
}

static void
on_window_fullscreened_notify(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    self->is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(obj));

    if (self->is_fullscreen) {
        gtk_button_set_label(GTK_BUTTON(self->btn_fullscreen), "⛶ EXIT");
        gtk_widget_add_css_class(self->btn_fullscreen, "active");
        schedule_hud_hide(self);
    } else {
        if (self->hud_hide_timer_id > 0) {
            g_source_remove(self->hud_hide_timer_id);
            self->hud_hide_timer_id = 0;
        }
        show_fullscreen_chrome(self);
        gtk_button_set_label(GTK_BUTTON(self->btn_fullscreen), "⛶ FULL");
        gtk_widget_remove_css_class(self->btn_fullscreen, "active");
    }
}

/* --- Media Stream Signals & State Updates --- */

static void
update_stream_badge(KajoPlayerWindow *self)
{
    const KajoMediaTrack *track = kajo_player_queue_get_current(self->queue);
    if (!track) {
        gtk_label_set_text(GTK_LABEL(self->stream_badge), "[ IDLE ]");
        return;
    }

    g_autofree char *size_str = kajo_player_format_size(track->size);
    const char *mime = track->content_type ? track->content_type : "media/stream";

    int w = 0, h = 0;
    if (self->media_stream && !track->is_audio_only) {
        w = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->media_stream));
        h = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->media_stream));
    }

    if (w > 0 && h > 0) {
        g_autofree char *badge = g_strdup_printf("[ %s | %d×%d | %s ]", mime, w, h, size_str);
        gtk_label_set_text(GTK_LABEL(self->stream_badge), badge);
    } else {
        g_autofree char *badge = g_strdup_printf("[ %s | %s ]", mime, size_str);
        gtk_label_set_text(GTK_LABEL(self->stream_badge), badge);
    }
}

static void
update_transport_ui(KajoPlayerWindow *self)
{
    if (!self->media_stream) {
        gtk_button_set_label(GTK_BUTTON(self->btn_play_pause), "▶ PLAY");
        gtk_label_set_text(GTK_LABEL(self->time_elapsed_lbl), "00:00");
        gtk_label_set_text(GTK_LABEL(self->time_total_lbl), "00:00");
        gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), "READY");
        self->suppress_seek_signal = TRUE;
        gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, 100.0);
        gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);
        self->suppress_seek_signal = FALSE;
        return;
    }

    gboolean playing = gtk_media_stream_get_playing(self->media_stream);
    gtk_button_set_label(GTK_BUTTON(self->btn_play_pause), playing ? "⏸ PAUSE" : "▶ PLAY");

    gint64 pos_us = gtk_media_stream_get_timestamp(self->media_stream);
    gint64 dur_us = gtk_media_stream_get_duration(self->media_stream);

    g_autofree char *pos_str = kajo_player_format_time(pos_us);
    g_autofree char *dur_str = kajo_player_format_time(dur_us);

    gtk_label_set_text(GTK_LABEL(self->time_elapsed_lbl), pos_str);
    gtk_label_set_text(GTK_LABEL(self->time_total_lbl), dur_str);

    self->suppress_seek_signal = TRUE;
    if (dur_us > 0) {
        double dur_sec = (double)dur_us / 1000000.0;
        double pos_sec = (double)pos_us / 1000000.0;
        gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, dur_sec);
        gtk_range_set_value(GTK_RANGE(self->seek_scale), CLAMP(pos_sec, 0.0, dur_sec));
    } else {
        gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, 100.0);
        gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);
    }
    self->suppress_seek_signal = FALSE;

    const GError *err = gtk_media_stream_get_error(self->media_stream);
    if (err) {
        g_autofree char *msg = g_strdup_printf("⚠ %s", err->message);
        gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), msg);
    } else {
        int idx = kajo_player_queue_get_current_index(self->queue);
        guint total = kajo_player_queue_get_length(self->queue);
        const char *state_str = playing ? "PLAYING" : "PAUSED";
        if (total > 1 && idx >= 0) {
            g_autofree char *status = g_strdup_printf("%s  •  TRACK %d / %u", state_str, idx + 1, total);
            gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), status);
        } else {
            gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), state_str);
        }
    }
}

static void
sync_mpris_state(KajoPlayerWindow *self)
{
    if (!self->mpris)
        return;

    gboolean has_media = (self->media_stream != NULL);
    gboolean playing = has_media && gtk_media_stream_get_playing(self->media_stream);
    gint64 pos_us = has_media ? gtk_media_stream_get_timestamp(self->media_stream) : 0;
    gint64 dur_us = has_media ? gtk_media_stream_get_duration(self->media_stream) : 0;

    kajo_player_mpris_update_state(self->mpris, has_media, playing, pos_us, dur_us, self->volume);
}

static void
on_media_timestamp_notify(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    update_transport_ui(self);
    sync_mpris_state(self);
}

static void
on_media_duration_notify(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    update_transport_ui(self);
    update_stream_badge(self);

    const KajoMediaTrack *track = kajo_player_queue_get_current(self->queue);
    gint64 dur_us = self->media_stream ? gtk_media_stream_get_duration(self->media_stream) : 0;
    kajo_player_mpris_update_track(self->mpris, track, dur_us);
    sync_mpris_state(self);
}

static void
on_media_playing_notify(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    update_transport_ui(self);
    sync_mpris_state(self);

    if (self->is_fullscreen) {
        if (self->media_stream && gtk_media_stream_get_playing(self->media_stream))
            schedule_hud_hide(self);
        else
            show_fullscreen_chrome(self);
    }
}

static void
on_media_prepared_notify(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    update_stream_badge(self);
    update_transport_ui(self);
}

static void
on_paintable_invalidate_size(GdkPaintable *paintable G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    update_stream_badge(self);
}

static void
on_media_ended_notify(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (!self->media_stream)
        return;

    if (gtk_media_stream_get_ended(self->media_stream) && !self->loop_enabled) {
        guint total = kajo_player_queue_get_length(self->queue);
        int idx = kajo_player_queue_get_current_index(self->queue);
        if (total > 1 && idx + 1 < (int)total) {
            kajo_player_queue_next(self->queue);
            return;
        }
    }
    update_transport_ui(self);
    sync_mpris_state(self);
}

static void
on_media_error_notify(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    update_transport_ui(self);
}

static void
clear_media_stream(KajoPlayerWindow *self)
{
    if (self->media_stream) {
        g_signal_handlers_disconnect_by_data(self->media_stream, self);
        gtk_media_stream_pause(self->media_stream);
    }
    if (self->video_picture)
        gtk_picture_set_paintable(GTK_PICTURE(self->video_picture), NULL);
    g_clear_object(&self->media_stream);
}

/* --- Track Selection Callback from KajoPlayerQueue --- */

static void
on_queue_track_selected(const KajoMediaTrack *track,
                        int index G_GNUC_UNUSED,
                        int total,
                        gpointer user_data)
{
    KajoPlayerWindow *self = user_data;

    clear_media_stream(self);

    if (!track || !track->file) {
        gtk_label_set_text(GTK_LABEL(self->title_label), "🎬 KAJO PLAYER");
        gtk_window_set_title(GTK_WINDOW(self), "KAJO PLAYER");
        gtk_stack_set_visible_child_name(self->canvas_stack, "empty");
        update_stream_badge(self);
        update_transport_ui(self);
        kajo_player_mpris_update_track(self->mpris, NULL, 0);
        sync_mpris_state(self);
        return;
    }

    g_autofree char *hdr_title = g_strdup_printf("🎬 KAJO PLAYER // %s", track->display_name);
    gtk_label_set_text(GTK_LABEL(self->title_label), hdr_title);
    gtk_window_set_title(GTK_WINDOW(self), hdr_title);

    gtk_widget_set_sensitive(self->btn_prev, total > 1);
    gtk_widget_set_sensitive(self->btn_next, total > 1);

    self->media_stream = gtk_media_file_new_for_file(track->file);
    gtk_media_stream_set_loop(self->media_stream, self->loop_enabled);
    gtk_media_stream_set_volume(self->media_stream, self->volume);
    gtk_media_stream_set_muted(self->media_stream, self->muted);

    g_signal_connect(self->media_stream, "notify::timestamp", G_CALLBACK(on_media_timestamp_notify), self);
    g_signal_connect(self->media_stream, "notify::duration", G_CALLBACK(on_media_duration_notify), self);
    g_signal_connect(self->media_stream, "notify::playing", G_CALLBACK(on_media_playing_notify), self);
    g_signal_connect(self->media_stream, "notify::prepared", G_CALLBACK(on_media_prepared_notify), self);
    g_signal_connect(self->media_stream, "notify::ended", G_CALLBACK(on_media_ended_notify), self);
    g_signal_connect(self->media_stream, "notify::error", G_CALLBACK(on_media_error_notify), self);
    g_signal_connect(self->media_stream, "invalidate-size", G_CALLBACK(on_paintable_invalidate_size), self);

    if (track->is_audio_only) {
        gtk_label_set_text(GTK_LABEL(self->audio_title_label), track->display_name);
        g_autofree char *size_str = kajo_player_format_size(track->size);
        g_autofree char *meta = g_strdup_printf("[ %s  •  %s ]",
            track->content_type ? track->content_type : "audio/stream", size_str);
        gtk_label_set_text(GTK_LABEL(self->audio_meta_label), meta);
        gtk_stack_set_visible_child_name(self->canvas_stack, "audio");
    } else {
        gtk_picture_set_paintable(GTK_PICTURE(self->video_picture), GDK_PAINTABLE(self->media_stream));
        gtk_stack_set_visible_child_name(self->canvas_stack, "video");
    }

    gtk_media_stream_play(self->media_stream);

    update_stream_badge(self);
    update_transport_ui(self);
    kajo_player_mpris_update_track(self->mpris, track, 0);
    sync_mpris_state(self);
}

/* --- Transport Controls & Actions --- */

static void
toggle_play_pause(KajoPlayerWindow *self)
{
    if (!self->media_stream)
        return;

    if (gtk_media_stream_get_playing(self->media_stream)) {
        gtk_media_stream_pause(self->media_stream);
    } else {
        if (gtk_media_stream_get_ended(self->media_stream))
            gtk_media_stream_seek(self->media_stream, 0);
        gtk_media_stream_play(self->media_stream);
    }
}

static void
seek_relative_seconds(KajoPlayerWindow *self, double delta_sec)
{
    if (!self->media_stream)
        return;

    gint64 pos = gtk_media_stream_get_timestamp(self->media_stream);
    gint64 dur = gtk_media_stream_get_duration(self->media_stream);
    gint64 target = pos + (gint64)(delta_sec * 1000000.0);
    if (target < 0)
        target = 0;
    if (dur > 0 && target > dur)
        target = dur;

    gtk_media_stream_seek(self->media_stream, target);
}

static void
set_volume_level(KajoPlayerWindow *self, double vol)
{
    self->volume = CLAMP(vol, 0.0, 1.0);
    if (self->media_stream)
        gtk_media_stream_set_volume(self->media_stream, self->volume);

    self->suppress_vol_signal = TRUE;
    gtk_range_set_value(GTK_RANGE(self->volume_scale), self->volume * 100.0);
    self->suppress_vol_signal = FALSE;

    g_autofree char *pct = g_strdup_printf("%d%%", (int)round(self->volume * 100.0));
    gtk_label_set_text(GTK_LABEL(self->volume_pct_lbl), pct);

    if (self->muted || self->volume <= 0.001)
        gtk_button_set_label(GTK_BUTTON(self->btn_mute), "🔇");
    else
        gtk_button_set_label(GTK_BUTTON(self->btn_mute), "🔊");

    sync_mpris_state(self);
}

static void
toggle_mute(KajoPlayerWindow *self)
{
    self->muted = !self->muted;
    if (self->media_stream)
        gtk_media_stream_set_muted(self->media_stream, self->muted);

    if (self->muted) {
        gtk_button_set_label(GTK_BUTTON(self->btn_mute), "🔇");
        gtk_widget_add_css_class(self->btn_mute, "active");
    } else {
        gtk_button_set_label(GTK_BUTTON(self->btn_mute), "🔊");
        gtk_widget_remove_css_class(self->btn_mute, "active");
    }
}

static void
toggle_loop(KajoPlayerWindow *self)
{
    self->loop_enabled = !self->loop_enabled;
    if (self->media_stream)
        gtk_media_stream_set_loop(self->media_stream, self->loop_enabled);

    if (self->loop_enabled)
        gtk_widget_add_css_class(self->btn_loop, "active");
    else
        gtk_widget_remove_css_class(self->btn_loop, "active");
}

static void
toggle_queue_drawer(KajoPlayerWindow *self)
{
    self->queue_visible = !self->queue_visible;
    gtk_widget_set_visible(self->queue_vbox, self->queue_visible);
    if (self->queue_visible)
        gtk_widget_add_css_class(self->btn_queue, "active");
    else
        gtk_widget_remove_css_class(self->btn_queue, "active");
}

/* --- File Open Dialog --- */

static void
on_file_dialog_opened(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    KajoPlayerWindow *self = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, res, &err);
    if (file) {
        kajo_player_window_open_file(self, file);
    }
}

static void
open_file_chooser(KajoPlayerWindow *self)
{
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Media File — Kajo Player");

    g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Media Files (Video & Audio)");
    gtk_file_filter_add_mime_type(filter, "video/*");
    gtk_file_filter_add_mime_type(filter, "audio/*");

    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));

    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_file_dialog_opened, self);
}

/* --- Button & Scale Signal Handlers --- */

static void
on_btn_open_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    open_file_chooser(KAJO_PLAYER_WINDOW(user_data));
}

static void
on_btn_queue_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    toggle_queue_drawer(KAJO_PLAYER_WINDOW(user_data));
}

static void
on_btn_float_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    niri_send_action("{\"Action\":\"ToggleWindowFloating\"}");
}

static void
on_btn_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    gtk_window_close(GTK_WINDOW(user_data));
}

static void
on_btn_play_pause_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    toggle_play_pause(KAJO_PLAYER_WINDOW(user_data));
}

static void
on_btn_seek_back_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    seek_relative_seconds(KAJO_PLAYER_WINDOW(user_data), -10.0);
}

static void
on_btn_seek_fwd_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    seek_relative_seconds(KAJO_PLAYER_WINDOW(user_data), 10.0);
}

static void
on_btn_prev_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    kajo_player_queue_prev(self->queue);
}

static void
on_btn_next_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    kajo_player_queue_next(self->queue);
}

static void
on_btn_loop_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    toggle_loop(KAJO_PLAYER_WINDOW(user_data));
}

static void
on_btn_mute_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    toggle_mute(KAJO_PLAYER_WINDOW(user_data));
}

static void
on_btn_fullscreen_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    toggle_fullscreen(KAJO_PLAYER_WINDOW(user_data));
}

static void
on_seek_scale_value_changed(GtkRange *range, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (self->suppress_seek_signal || !self->media_stream)
        return;

    double sec = gtk_range_get_value(range);
    gint64 target_us = (gint64)(sec * 1000000.0);
    gtk_media_stream_seek(self->media_stream, target_us);

    g_autofree char *pos_str = kajo_player_format_time(target_us);
    gtk_label_set_text(GTK_LABEL(self->time_elapsed_lbl), pos_str);
}

static void
on_volume_scale_value_changed(GtkRange *range, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (self->suppress_vol_signal)
        return;

    double pct = gtk_range_get_value(range);
    set_volume_level(self, pct / 100.0);
}

static void
on_video_canvas_clicked(GtkGestureClick *gesture G_GNUC_UNUSED,
                        int n_press,
                        double x G_GNUC_UNUSED,
                        double y G_GNUC_UNUSED,
                        gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (n_press == 2) {
        toggle_fullscreen(self);
    }
}

/* --- Drag-and-Drop Media Support --- */

static gboolean
on_drop_file(GtkDropTarget *target G_GNUC_UNUSED,
             const GValue *value,
             double x G_GNUC_UNUSED,
             double y G_GNUC_UNUSED,
             gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
        GFile *file = g_value_get_object(value);
        if (file) {
            kajo_player_window_open_file(self, file);
            return TRUE;
        }
    }
    return FALSE;
}

/* --- MPRIS D-Bus Command Callback --- */

static void
on_mpris_command(KajoMprisCommand cmd, gint64 arg_usec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    switch (cmd) {
    case KAJO_MPRIS_CMD_PLAY:
        if (self->media_stream && !gtk_media_stream_get_playing(self->media_stream))
            gtk_media_stream_play(self->media_stream);
        break;
    case KAJO_MPRIS_CMD_PAUSE:
        if (self->media_stream && gtk_media_stream_get_playing(self->media_stream))
            gtk_media_stream_pause(self->media_stream);
        break;
    case KAJO_MPRIS_CMD_PLAY_PAUSE:
        toggle_play_pause(self);
        break;
    case KAJO_MPRIS_CMD_STOP:
        if (self->media_stream) {
            gtk_media_stream_pause(self->media_stream);
            gtk_media_stream_seek(self->media_stream, 0);
        }
        break;
    case KAJO_MPRIS_CMD_NEXT:
        kajo_player_queue_next(self->queue);
        break;
    case KAJO_MPRIS_CMD_PREV:
        kajo_player_queue_prev(self->queue);
        break;
    case KAJO_MPRIS_CMD_RAISE:
        gtk_window_present(GTK_WINDOW(self));
        break;
    case KAJO_MPRIS_CMD_QUIT:
        gtk_window_close(GTK_WINDOW(self));
        break;
    }
}

/* --- Keyboard Shortcuts --- */

static gboolean
on_window_key_pressed(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                      guint keyval,
                      guint keycode G_GNUC_UNUSED,
                      GdkModifierType state,
                      gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    gboolean ctrl_held = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift_held = (state & GDK_SHIFT_MASK) != 0;

    if (ctrl_held) {
        if (keyval == GDK_KEY_o || keyval == GDK_KEY_O) {
            open_file_chooser(self);
            return TRUE;
        }
        if (keyval == GDK_KEY_p || keyval == GDK_KEY_P ||
            keyval == GDK_KEY_i || keyval == GDK_KEY_I) {
            toggle_queue_drawer(self);
            return TRUE;
        }
        if (keyval == GDK_KEY_q || keyval == GDK_KEY_Q ||
            keyval == GDK_KEY_w || keyval == GDK_KEY_W) {
            gtk_window_close(GTK_WINDOW(self));
            return TRUE;
        }
    }

    switch (keyval) {
    case GDK_KEY_space:
    case GDK_KEY_k:
    case GDK_KEY_K:
    case GDK_KEY_AudioPlay:
    case GDK_KEY_AudioPause:
        toggle_play_pause(self);
        return TRUE;

    case GDK_KEY_f:
    case GDK_KEY_F:
    case GDK_KEY_F11:
        toggle_fullscreen(self);
        return TRUE;

    case GDK_KEY_Escape:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self))) {
            gtk_window_unfullscreen(GTK_WINDOW(self));
            return TRUE;
        }
        break;

    case GDK_KEY_Left:
        seek_relative_seconds(self, shift_held ? -30.0 : -5.0);
        return TRUE;

    case GDK_KEY_Right:
        seek_relative_seconds(self, shift_held ? 30.0 : 5.0);
        return TRUE;

    case GDK_KEY_Up:
        set_volume_level(self, self->volume + 0.05);
        return TRUE;

    case GDK_KEY_Down:
        set_volume_level(self, self->volume - 0.05);
        return TRUE;

    case GDK_KEY_m:
    case GDK_KEY_M:
    case GDK_KEY_AudioMute:
        toggle_mute(self);
        return TRUE;

    case GDK_KEY_l:
    case GDK_KEY_L:
        toggle_loop(self);
        return TRUE;

    case GDK_KEY_Page_Up:
    case GDK_KEY_p:
    case GDK_KEY_P:
    case GDK_KEY_AudioPrev:
        kajo_player_queue_prev(self->queue);
        return TRUE;

    case GDK_KEY_Page_Down:
    case GDK_KEY_n:
    case GDK_KEY_N:
    case GDK_KEY_AudioNext:
        kajo_player_queue_next(self->queue);
        return TRUE;

    case GDK_KEY_q:
    case GDK_KEY_Q:
        gtk_window_close(GTK_WINDOW(self));
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

/* --- GObject Lifecycle --- */

static void
kajo_player_window_dispose(GObject *object)
{
    KajoPlayerWindow *self = KAJO_PLAYER_WINDOW(object);

    if (self->hud_hide_timer_id > 0) {
        g_source_remove(self->hud_hide_timer_id);
        self->hud_hide_timer_id = 0;
    }

    clear_media_stream(self);
    g_clear_pointer(&self->mpris, kajo_player_mpris_free);
    g_clear_pointer(&self->queue, kajo_player_queue_free);

    gtk_widget_dispose_template(GTK_WIDGET(self), KAJO_TYPE_PLAYER_WINDOW);

    G_OBJECT_CLASS(kajo_player_window_parent_class)->dispose(object);
}

static void
kajo_player_window_class_init(KajoPlayerWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = kajo_player_window_dispose;

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/player_window.ui");

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, main_vbox);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, header_handle);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, title_label);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, stream_badge);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_open);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_queue);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_float);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_close);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, video_overlay);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, canvas_stack);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_empty_open);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, video_picture);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, audio_title_label);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, audio_meta_label);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, queue_vbox);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, queue_count_badge);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, queue_listbox);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, transport_deck);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, time_elapsed_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, seek_scale);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, time_total_lbl);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_prev);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_seek_back);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_play_pause);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_seek_fwd);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_next);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_loop);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, transport_status_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_mute);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, volume_scale);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, volume_pct_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_fullscreen);
}

static void
kajo_player_window_init(KajoPlayerWindow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->volume = 1.0;
    self->muted = FALSE;
    self->loop_enabled = FALSE;
    self->queue_visible = FALSE;

    self->queue = kajo_player_queue_new(
        self->queue_listbox,
        self->queue_count_badge,
        on_queue_track_selected,
        self
    );

    self->mpris = kajo_player_mpris_new(on_mpris_command, self);

    /* Configure Scrubber & Volume Scales */
    gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, 100.0);
    gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);

    self->suppress_vol_signal = TRUE;
    gtk_range_set_range(GTK_RANGE(self->volume_scale), 0.0, 100.0);
    gtk_range_set_value(GTK_RANGE(self->volume_scale), 100.0);
    self->suppress_vol_signal = FALSE;

    /* Connect Header & Transport Buttons */
    g_signal_connect(self->btn_open, "clicked", G_CALLBACK(on_btn_open_clicked), self);
    g_signal_connect(self->btn_empty_open, "clicked", G_CALLBACK(on_btn_open_clicked), self);
    g_signal_connect(self->btn_queue, "clicked", G_CALLBACK(on_btn_queue_clicked), self);
    g_signal_connect(self->btn_float, "clicked", G_CALLBACK(on_btn_float_clicked), self);
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_btn_close_clicked), self);

    g_signal_connect(self->btn_play_pause, "clicked", G_CALLBACK(on_btn_play_pause_clicked), self);
    g_signal_connect(self->btn_seek_back, "clicked", G_CALLBACK(on_btn_seek_back_clicked), self);
    g_signal_connect(self->btn_seek_fwd, "clicked", G_CALLBACK(on_btn_seek_fwd_clicked), self);
    g_signal_connect(self->btn_prev, "clicked", G_CALLBACK(on_btn_prev_clicked), self);
    g_signal_connect(self->btn_next, "clicked", G_CALLBACK(on_btn_next_clicked), self);
    g_signal_connect(self->btn_loop, "clicked", G_CALLBACK(on_btn_loop_clicked), self);
    g_signal_connect(self->btn_mute, "clicked", G_CALLBACK(on_btn_mute_clicked), self);
    g_signal_connect(self->btn_fullscreen, "clicked", G_CALLBACK(on_btn_fullscreen_clicked), self);

    g_signal_connect(self->seek_scale, "value-changed", G_CALLBACK(on_seek_scale_value_changed), self);
    g_signal_connect(self->volume_scale, "value-changed", G_CALLBACK(on_volume_scale_value_changed), self);

    g_signal_connect(self, "notify::fullscreened", G_CALLBACK(on_window_fullscreened_notify), self);

    /* Double-Click on Video Canvas for Fullscreen */
    GtkGesture *dbl_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dbl_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(dbl_click, "pressed", G_CALLBACK(on_video_canvas_clicked), self);
    gtk_widget_add_controller(GTK_WIDGET(self->video_picture), GTK_EVENT_CONTROLLER(dbl_click));

    /* Pointer Motion on Window for Fullscreen Chrome Auto-Hide */
    GtkEventController *motion_ctrl = gtk_event_controller_motion_new();
    g_signal_connect(motion_ctrl, "motion", G_CALLBACK(on_pointer_motion), self);
    gtk_widget_add_controller(self->main_vbox, motion_ctrl);

    /* Drag-and-Drop File Support */
    GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_drop_file), self);
    gtk_widget_add_controller(GTK_WIDGET(self->video_overlay), GTK_EVENT_CONTROLLER(drop_target));

    /* Global Window Key Controller */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);
}

KajoPlayerWindow *
kajo_player_window_new(GtkApplication *app)
{
    return g_object_new(KAJO_TYPE_PLAYER_WINDOW, "application", app, NULL);
}

void
kajo_player_window_open_file(KajoPlayerWindow *self, GFile *file)
{
    g_return_if_fail(KAJO_IS_PLAYER_WINDOW(self));
    g_return_if_fail(G_IS_FILE(file));

    kajo_player_queue_load_file_and_siblings(self->queue, file);
}

void
kajo_player_window_open_files(KajoPlayerWindow *self, GFile **files, int n_files)
{
    g_return_if_fail(KAJO_IS_PLAYER_WINDOW(self));
    if (!files || n_files <= 0)
        return;

    if (n_files == 1) {
        kajo_player_queue_load_file_and_siblings(self->queue, files[0]);
    } else {
        kajo_player_queue_set_files(self->queue, files, n_files);
    }
}
