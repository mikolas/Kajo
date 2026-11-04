#include "player_window.h"
#include "player_mpv.h"
#include "player_queue.h"
#include "player_mpris.h"
#include "../compositor/compositor.h"
#include <math.h>
#include <string.h>

struct _KajoPlayerWindow {
    GtkApplicationWindow parent_instance;

    /* Blueprint Template Children */
    GtkOverlay *root_overlay;
    GtkWidget *body_hbox;
    GtkWidget *header_handle;
    GtkWidget *title_label;
    GtkWidget *stream_badge;
    GtkWidget *btn_open;
    GtkWidget *btn_queue;
    GtkWidget *btn_float;
    GtkWidget *btn_close;

    GtkOverlay *video_overlay;
    GtkGLArea *video_gl_area;
    GtkWidget *empty_state_box;
    GtkWidget *btn_empty_open;
    GtkWidget *audio_state_box;
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

    /* Playback Engine & Controllers */
    KajoPlayerMpv *mpv;
    KajoPlayerQueue *queue;
    KajoPlayerMpris *mpris;

    double volume;
    gboolean muted;
    gboolean loop_enabled;
    gboolean queue_visible;
    gboolean is_fullscreen;
    gboolean syncing_fullscreen;
    gboolean last_eof_reached;
    gint64 last_mpris_duration_usec;

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

/* --- Fullscreen & Bottom-Edge Auto-Hide Chrome --- */

static void
update_windowed_margins(KajoPlayerWindow *self)
{
    if (self->is_fullscreen) {
        gtk_widget_set_margin_top(self->body_hbox, 0);
        gtk_widget_set_margin_bottom(self->body_hbox, 0);
        return;
    }

    int hdr_h = 0, deck_h = 0;
    gtk_widget_measure(self->header_handle, GTK_ORIENTATION_VERTICAL, -1, NULL, &hdr_h, NULL, NULL);
    gtk_widget_measure(self->transport_deck, GTK_ORIENTATION_VERTICAL, -1, NULL, &deck_h, NULL, NULL);

    gtk_widget_set_margin_top(self->body_hbox, hdr_h > 0 ? hdr_h : 41);
    gtk_widget_set_margin_bottom(self->body_hbox, deck_h > 0 ? deck_h : 78);
}

static void
hide_fullscreen_chrome(KajoPlayerWindow *self)
{
    if (!self->is_fullscreen)
        return;

    gtk_widget_set_visible(self->header_handle, FALSE);
    gtk_widget_set_visible(self->transport_deck, FALSE);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self->video_overlay), "none");
}

static gboolean
on_hud_hide_timeout(gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    self->hud_hide_timer_id = 0;

    if (self->is_fullscreen) {
        hide_fullscreen_chrome(self);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_hud_hide(KajoPlayerWindow *self, guint delay_ms)
{
    if (self->hud_hide_timer_id > 0) {
        g_source_remove(self->hud_hide_timer_id);
        self->hud_hide_timer_id = 0;
    }
    if (self->is_fullscreen) {
        self->hud_hide_timer_id = g_timeout_add(delay_ms, on_hud_hide_timeout, self);
    }
}

static void
on_pointer_motion(GtkEventControllerMotion *ctrl G_GNUC_UNUSED,
                  double x,
                  double y,
                  gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (!self->is_fullscreen)
        return;

    if (fabs(x - self->last_motion_x) < 1.5 && fabs(y - self->last_motion_y) < 1.5)
        return;

    self->last_motion_x = x;
    self->last_motion_y = y;

    /* Always restore pointer cursor when moving the mouse */
    gtk_widget_set_cursor(GTK_WIDGET(self->video_overlay), NULL);

    int win_h = gtk_widget_get_height(GTK_WIDGET(self));
    gboolean in_bottom_zone = (win_h > 0 && y >= (double)(win_h - 115));
    gboolean in_top_zone = (y <= 52.0);

    if (in_bottom_zone) {
        gtk_widget_set_visible(self->transport_deck, TRUE);
        gtk_widget_set_visible(self->header_handle, FALSE);
        schedule_hud_hide(self, 2500);
    } else if (in_top_zone) {
        gtk_widget_set_visible(self->header_handle, TRUE);
        gtk_widget_set_visible(self->transport_deck, FALSE);
        schedule_hud_hide(self, 2500);
    } else {
        /* Pointer is over the middle of the video canvas: hide bars and start cursor hide timer */
        gtk_widget_set_visible(self->header_handle, FALSE);
        gtk_widget_set_visible(self->transport_deck, FALSE);
        schedule_hud_hide(self, 1800);
    }
}

static void
set_fullscreen_mode(KajoPlayerWindow *self, gboolean fullscreen)
{
    if (self->is_fullscreen == fullscreen)
        return;

    self->is_fullscreen = fullscreen;

    if (self->hud_hide_timer_id > 0) {
        g_source_remove(self->hud_hide_timer_id);
        self->hud_hide_timer_id = 0;
    }

    self->syncing_fullscreen = TRUE;
    if (fullscreen) {
        if (!gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_fullscreen(GTK_WINDOW(self));
    } else {
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
    }
    self->syncing_fullscreen = FALSE;

    if (self->is_fullscreen) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "player-fullscreen");
        gtk_button_set_label(GTK_BUTTON(self->btn_fullscreen), "⛶ EXIT");
        gtk_widget_add_css_class(self->btn_fullscreen, "active");

        /* Expand video to 100% of window and immediately hide all chrome & queue */
        update_windowed_margins(self);
        gtk_widget_set_visible(self->queue_vbox, FALSE);
        hide_fullscreen_chrome(self);
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "player-fullscreen");
        gtk_button_set_label(GTK_BUTTON(self->btn_fullscreen), "⛶ FULL");
        gtk_widget_remove_css_class(self->btn_fullscreen, "active");

        gtk_widget_set_cursor(GTK_WIDGET(self->video_overlay), NULL);
        gtk_widget_set_visible(self->header_handle, TRUE);
        gtk_widget_set_visible(self->transport_deck, TRUE);
        gtk_widget_set_visible(self->queue_vbox, self->queue_visible);
        update_windowed_margins(self);
    }
}

static void
toggle_fullscreen(KajoPlayerWindow *self)
{
    set_fullscreen_mode(self, !self->is_fullscreen);
}

static void
on_window_fullscreened_notify(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (self->syncing_fullscreen)
        return;

    gboolean compositor_fs = gtk_window_is_fullscreen(GTK_WINDOW(obj));
    set_fullscreen_mode(self, compositor_fs);
}

/* --- libmpv State Updates & Stream Badge --- */

static void
update_stream_badge(KajoPlayerWindow *self)
{
    const KajoMediaTrack *track = kajo_player_queue_get_current(self->queue);
    if (!track) {
        gtk_label_set_text(GTK_LABEL(self->stream_badge), "[ IDLE ]");
        return;
    }

    g_autofree char *size_str = kajo_player_format_size(track->size);
    const KajoMpvState *st = kajo_player_mpv_get_state(self->mpv);

    int w = st ? st->video_width : 0;
    int h = st ? st->video_height : 0;
    const char *vcodec = (st && st->video_codec && st->video_codec[0]) ? st->video_codec : NULL;
    const char *hwdec = (st && st->hwdec_current && st->hwdec_current[0]) ? st->hwdec_current : NULL;
    const char *acodec = (st && st->audio_codec && st->audio_codec[0]) ? st->audio_codec : NULL;
    const char *mime = track->content_type ? track->content_type : "media/stream";

    if (w > 0 && h > 0) {
        g_autofree char *codec_tag = NULL;
        if (vcodec && hwdec) {
            g_autofree char *v_up = g_ascii_strup(vcodec, -1);
            codec_tag = g_strdup_printf("%s•%s", v_up, hwdec);
        } else if (vcodec) {
            codec_tag = g_ascii_strup(vcodec, -1);
        } else {
            codec_tag = g_strdup(mime);
        }
        g_autofree char *badge = g_strdup_printf("[ %s | %d×%d | %s ]", codec_tag, w, h, size_str);
        gtk_label_set_text(GTK_LABEL(self->stream_badge), badge);
    } else if (acodec) {
        g_autofree char *a_up = g_ascii_strup(acodec, -1);
        g_autofree char *badge = g_strdup_printf("[ %s | %s ]", a_up, size_str);
        gtk_label_set_text(GTK_LABEL(self->stream_badge), badge);
    } else {
        g_autofree char *badge = g_strdup_printf("[ %s | %s ]", mime, size_str);
        gtk_label_set_text(GTK_LABEL(self->stream_badge), badge);
    }
}

static void
update_transport_ui(KajoPlayerWindow *self)
{
    const KajoMpvState *st = kajo_player_mpv_get_state(self->mpv);
    if (!st || !st->has_media) {
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

    gtk_button_set_label(GTK_BUTTON(self->btn_play_pause), st->playing ? "⏸ PAUSE" : "▶ PLAY");

    g_autofree char *pos_str = kajo_player_format_time(st->position_usec);
    g_autofree char *dur_str = kajo_player_format_time(st->duration_usec);

    gtk_label_set_text(GTK_LABEL(self->time_elapsed_lbl), pos_str);
    gtk_label_set_text(GTK_LABEL(self->time_total_lbl), dur_str);

    self->suppress_seek_signal = TRUE;
    if (st->duration_usec > 0) {
        double dur_sec = (double)st->duration_usec / 1000000.0;
        double pos_sec = (double)st->position_usec / 1000000.0;
        gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, dur_sec);
        gtk_range_set_value(GTK_RANGE(self->seek_scale), CLAMP(pos_sec, 0.0, dur_sec));
    } else {
        gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, 100.0);
        gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);
    }
    self->suppress_seek_signal = FALSE;

    if (st->error_message) {
        g_autofree char *msg = g_strdup_printf("⚠ %s", st->error_message);
        gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), msg);
    } else {
        int idx = kajo_player_queue_get_current_index(self->queue);
        guint total = kajo_player_queue_get_length(self->queue);
        const char *state_str = st->playing ? "PLAYING" : "PAUSED";
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

    const KajoMpvState *st = kajo_player_mpv_get_state(self->mpv);
    gboolean has_media = st && st->has_media;
    gboolean playing = has_media && st->playing;
    gint64 pos_us = has_media ? st->position_usec : 0;
    gint64 dur_us = has_media ? st->duration_usec : 0;

    if (has_media && dur_us != self->last_mpris_duration_usec) {
        self->last_mpris_duration_usec = dur_us;
        const KajoMediaTrack *track = kajo_player_queue_get_current(self->queue);
        kajo_player_mpris_update_track(self->mpris, track, dur_us);
    }

    kajo_player_mpris_update_state(self->mpris, has_media, playing, pos_us, dur_us, self->volume);
}

static void
on_mpv_state_changed(const KajoMpvState *state, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;

    if (state && state->eof_reached && !self->last_eof_reached && !self->loop_enabled) {
        self->last_eof_reached = TRUE;
        guint total = kajo_player_queue_get_length(self->queue);
        int idx = kajo_player_queue_get_current_index(self->queue);
        if (total > 1 && idx + 1 < (int)total) {
            kajo_player_queue_next(self->queue);
            return;
        }
    } else if (state && !state->eof_reached) {
        self->last_eof_reached = FALSE;
    }

    update_stream_badge(self);
    update_transport_ui(self);
    sync_mpris_state(self);
}

/* --- Track Selection Callback from KajoPlayerQueue --- */

static void
on_queue_track_selected(const KajoMediaTrack *track,
                        int index G_GNUC_UNUSED,
                        int total,
                        gpointer user_data)
{
    KajoPlayerWindow *self = user_data;

    self->last_eof_reached = FALSE;
    self->last_mpris_duration_usec = -1;

    if (!track || !track->file) {
        kajo_player_mpv_stop(self->mpv);
        gtk_label_set_text(GTK_LABEL(self->title_label), "🎬 KAJO PLAYER");
        gtk_window_set_title(GTK_WINDOW(self), "KAJO PLAYER");
        gtk_widget_set_visible(self->empty_state_box, TRUE);
        gtk_widget_set_visible(self->audio_state_box, FALSE);
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

    gtk_widget_set_visible(self->empty_state_box, FALSE);

    if (track->is_audio_only) {
        gtk_label_set_text(GTK_LABEL(self->audio_title_label), track->display_name);
        g_autofree char *size_str = kajo_player_format_size(track->size);
        g_autofree char *meta = g_strdup_printf("[ %s  •  %s ]",
            track->content_type ? track->content_type : "audio/stream", size_str);
        gtk_label_set_text(GTK_LABEL(self->audio_meta_label), meta);
        gtk_widget_set_visible(self->audio_state_box, TRUE);
    } else {
        gtk_widget_set_visible(self->audio_state_box, FALSE);
    }

    kajo_player_mpv_set_loop(self->mpv, self->loop_enabled);
    kajo_player_mpv_set_volume(self->mpv, self->volume);
    kajo_player_mpv_set_muted(self->mpv, self->muted);
    kajo_player_mpv_load_file(self->mpv, track->file);

    update_stream_badge(self);
    update_transport_ui(self);
    kajo_player_mpris_update_track(self->mpris, track, 0);
    sync_mpris_state(self);
}

/* --- Transport Controls & Actions --- */

static void
toggle_play_pause(KajoPlayerWindow *self)
{
    gboolean playing = kajo_player_mpv_get_playing(self->mpv);
    kajo_player_mpv_set_playing(self->mpv, !playing);
}

static void
seek_relative_seconds(KajoPlayerWindow *self, double delta_sec)
{
    kajo_player_mpv_seek_relative_sec(self->mpv, delta_sec);

    /* Briefly flash the seek bar when seeking in fullscreen */
    if (self->is_fullscreen) {
        gtk_widget_set_visible(self->transport_deck, TRUE);
        schedule_hud_hide(self, 1500);
    }
}

static void
set_volume_level(KajoPlayerWindow *self, double vol)
{
    self->volume = CLAMP(vol, 0.0, 1.0);
    kajo_player_mpv_set_volume(self->mpv, self->volume);

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
    kajo_player_mpv_set_muted(self->mpv, self->muted);

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
    kajo_player_mpv_set_loop(self->mpv, self->loop_enabled);

    if (self->loop_enabled)
        gtk_widget_add_css_class(self->btn_loop, "active");
    else
        gtk_widget_remove_css_class(self->btn_loop, "active");
}

static void
toggle_queue_drawer(KajoPlayerWindow *self)
{
    self->queue_visible = !self->queue_visible;
    gtk_widget_set_visible(self->queue_vbox, self->queue_visible && !self->is_fullscreen);
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
    if (self->suppress_seek_signal)
        return;

    double sec = gtk_range_get_value(range);
    gint64 target_us = (gint64)(sec * 1000000.0);
    kajo_player_mpv_seek_absolute_usec(self->mpv, target_us);

    g_autofree char *pos_str = kajo_player_format_time(target_us);
    gtk_label_set_text(GTK_LABEL(self->time_elapsed_lbl), pos_str);

    if (self->is_fullscreen)
        schedule_hud_hide(self, 2500);
}

static void
on_volume_scale_value_changed(GtkRange *range, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (self->suppress_vol_signal)
        return;

    double pct = gtk_range_get_value(range);
    set_volume_level(self, pct / 100.0);

    if (self->is_fullscreen)
        schedule_hud_hide(self, 2500);
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
        kajo_player_mpv_set_playing(self->mpv, TRUE);
        break;
    case KAJO_MPRIS_CMD_PAUSE:
        kajo_player_mpv_set_playing(self->mpv, FALSE);
        break;
    case KAJO_MPRIS_CMD_PLAY_PAUSE:
        toggle_play_pause(self);
        break;
    case KAJO_MPRIS_CMD_STOP:
        kajo_player_mpv_set_playing(self->mpv, FALSE);
        kajo_player_mpv_seek_absolute_usec(self->mpv, 0);
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
        if (self->is_fullscreen) {
            set_fullscreen_mode(self, FALSE);
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

    g_clear_pointer(&self->mpv, kajo_player_mpv_free);
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

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, root_overlay);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, body_hbox);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, header_handle);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, title_label);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, stream_badge);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_open);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_queue);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_float);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_close);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, video_overlay);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, video_gl_area);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, empty_state_box);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_empty_open);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, audio_state_box);
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
    self->last_mpris_duration_usec = -1;

    self->mpv = kajo_player_mpv_new(self->video_gl_area, on_mpv_state_changed, self);

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

    update_windowed_margins(self);

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

    /* Double-Click on Video Overlay for Fullscreen */
    GtkGesture *dbl_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dbl_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(dbl_click, "pressed", G_CALLBACK(on_video_canvas_clicked), self);
    gtk_widget_add_controller(GTK_WIDGET(self->video_overlay), GTK_EVENT_CONTROLLER(dbl_click));

    /* Pointer Motion on Root Overlay for Fullscreen Bottom-Edge Auto-Show / Auto-Hide */
    GtkEventController *motion_ctrl = gtk_event_controller_motion_new();
    g_signal_connect(motion_ctrl, "motion", G_CALLBACK(on_pointer_motion), self);
    gtk_widget_add_controller(GTK_WIDGET(self->root_overlay), motion_ctrl);

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
