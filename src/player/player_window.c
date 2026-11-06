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
    GtkWidget *btn_stats;
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

    GtkWidget *stats_osd_box;
    GtkWidget *stats_osd_label;

    GtkWidget *queue_vbox;
    GtkWidget *chapters_card;
    GtkWidget *chapters_count_badge;
    GtkListBox *chapters_listbox;
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

    /* Speed & Frame Step Popover */
    GtkWidget *btn_speed;
    GtkPopover *speed_popover;
    GtkWidget *speed_badge_lbl;
    GtkWidget *btn_spd_05;
    GtkWidget *btn_spd_075;
    GtkWidget *btn_spd_10;
    GtkWidget *btn_spd_125;
    GtkWidget *btn_spd_15;
    GtkWidget *btn_spd_20;
    GtkWidget *btn_frame_prev;
    GtkWidget *btn_frame_next;
    GtkWidget *btn_ab_loop;

    /* Subtitles Popover */
    GtkWidget *btn_subs;
    GtkPopover *subs_popover;
    GtkWidget *subs_badge_lbl;
    GtkListBox *subs_listbox;
    GtkWidget *btn_load_sub;
    GtkWidget *btn_sub_delay_dec;
    GtkWidget *sub_delay_lbl;
    GtkWidget *btn_sub_delay_inc;
    GtkWidget *btn_sub_delay_reset;
    GtkWidget *btn_sub_scale_dec;
    GtkWidget *sub_scale_lbl;
    GtkWidget *btn_sub_scale_inc;

    /* Audio Streams Popover */
    GtkWidget *btn_audio;
    GtkPopover *audio_popover;
    GtkWidget *audio_badge_lbl;
    GtkListBox *audio_listbox;
    GtkWidget *btn_audio_delay_dec;
    GtkWidget *audio_delay_lbl;
    GtkWidget *btn_audio_delay_inc;
    GtkWidget *btn_audio_delay_reset;

    GtkWidget *btn_screenshot;
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
    gboolean stats_visible;
    gboolean is_fullscreen;
    gboolean syncing_fullscreen;
    gboolean last_eof_reached;
    gint64 last_mpris_duration_usec;

    guint last_tracks_hash;
    guint last_chapters_count;
    int last_chapter_idx;

    char *toast_message;
    guint toast_timer_id;

    gboolean suppress_seek_signal;
    gboolean suppress_vol_signal;
    gboolean suppress_track_select;
    gboolean suppress_chapter_select;

    guint hud_hide_timer_id;
    double last_motion_x;
    double last_motion_y;
};

G_DEFINE_FINAL_TYPE(KajoPlayerWindow, kajo_player_window, GTK_TYPE_APPLICATION_WINDOW)

static void update_stream_badge(KajoPlayerWindow *self);
static void update_transport_ui(KajoPlayerWindow *self);
static void update_stats_osd(KajoPlayerWindow *self);
static void rebuild_tracks_ui(KajoPlayerWindow *self);
static void rebuild_chapters_ui(KajoPlayerWindow *self);
static void sync_mpris_state(KajoPlayerWindow *self);

/* --- Toast Status Banner Helper --- */

static gboolean
on_toast_timeout(gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    self->toast_timer_id = 0;
    g_clear_pointer(&self->toast_message, g_free);
    update_transport_ui(self);
    return G_SOURCE_REMOVE;
}

static void
show_status_toast(KajoPlayerWindow *self, const char *msg)
{
    if (self->toast_timer_id > 0) {
        g_source_remove(self->toast_timer_id);
        self->toast_timer_id = 0;
    }
    g_free(self->toast_message);
    self->toast_message = g_strdup(msg);
    gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), msg);
    self->toast_timer_id = g_timeout_add(2500, on_toast_timeout, self);
}

/* --- Fullscreen & Bottom-Edge Auto-Hide Chrome --- */

static gboolean
any_transport_popover_open(KajoPlayerWindow *self)
{
    return (self->speed_popover && gtk_widget_get_visible(GTK_WIDGET(self->speed_popover))) ||
           (self->subs_popover && gtk_widget_get_visible(GTK_WIDGET(self->subs_popover))) ||
           (self->audio_popover && gtk_widget_get_visible(GTK_WIDGET(self->audio_popover)));
}

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
    if (!self->is_fullscreen || any_transport_popover_open(self))
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

    gtk_widget_set_cursor(GTK_WIDGET(self->video_overlay), NULL);

    if (any_transport_popover_open(self)) {
        gtk_widget_set_visible(self->transport_deck, TRUE);
        return;
    }

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

/* --- Subtitle & Audio Track Popover Builders --- */

static guint
compute_tracks_hash(const KajoMpvState *st)
{
    if (!st || !st->tracks)
        return 0;

    guint h = st->sub_visible ? 0x9e3779b9u : 0x12345678u;
    for (guint i = 0; i < st->tracks->len; i++) {
        const KajoMpvTrack *t = g_ptr_array_index(st->tracks, i);
        h ^= (guint)(t->id * 31 + (int)t->type * 97 + (t->selected ? 7 : 0));
    }
    return h;
}

static void
clear_listbox(GtkListBox *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL) {
        gtk_list_box_remove(box, child);
    }
}

static GtkWidget *
create_track_row(const char *label_text, gboolean selected, int track_id, KajoMpvTrackType type)
{
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "track-id", GINT_TO_POINTER(track_id + 1000));
    g_object_set_data(G_OBJECT(row), "track-type", GINT_TO_POINTER((int)type));

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(hbox, 8);
    gtk_widget_set_margin_end(hbox, 8);
    gtk_widget_set_margin_top(hbox, 6);
    gtk_widget_set_margin_bottom(hbox, 6);

    GtkWidget *chk = gtk_label_new(selected ? "✔" : " ");
    gtk_widget_set_valign(chk, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(chk, "shell-popover-badge");

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hbox), chk);
    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    return row;
}

static void
rebuild_tracks_ui(KajoPlayerWindow *self)
{
    const KajoMpvState *st = kajo_player_mpv_get_state(self->mpv);
    guint new_hash = compute_tracks_hash(st);

    /* Update Subtitle Delay & Scale Labels */
    if (st) {
        int sub_ms = (int)round(st->sub_delay_sec * 1000.0);
        g_autofree char *s_del = g_strdup_printf("%+d ms", sub_ms);
        gtk_label_set_text(GTK_LABEL(self->sub_delay_lbl), sub_ms == 0 ? "0 ms" : s_del);

        int scale_pct = (int)round(st->sub_scale * 100.0);
        g_autofree char *s_scl = g_strdup_printf("%d%%", scale_pct);
        gtk_label_set_text(GTK_LABEL(self->sub_scale_lbl), s_scl);

        int aud_ms = (int)round(st->audio_delay_sec * 1000.0);
        g_autofree char *a_del = g_strdup_printf("%+d ms", aud_ms);
        gtk_label_set_text(GTK_LABEL(self->audio_delay_lbl), aud_ms == 0 ? "0 ms" : a_del);

        g_autofree char *spd_btn = g_strdup_printf("%.2g×", st->speed);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(self->btn_speed), spd_btn);
        g_autofree char *spd_bdg = g_strdup_printf("[ %.2f× ]", st->speed);
        gtk_label_set_text(GTK_LABEL(self->speed_badge_lbl), spd_bdg);
    }

    if (new_hash == self->last_tracks_hash)
        return;
    self->last_tracks_hash = new_hash;

    self->suppress_track_select = TRUE;
    clear_listbox(self->subs_listbox);
    clear_listbox(self->audio_listbox);

    const KajoMpvTrack *active_sub = NULL;
    const KajoMpvTrack *active_audio = NULL;
    guint n_subs = 0;
    guint n_audio = 0;

    if (st && st->tracks) {
        for (guint i = 0; i < st->tracks->len; i++) {
            const KajoMpvTrack *t = g_ptr_array_index(st->tracks, i);
            if (t->type == KAJO_MPV_TRACK_SUB) {
                n_subs++;
                if (t->selected && st->sub_visible)
                    active_sub = t;
            } else if (t->type == KAJO_MPV_TRACK_AUDIO) {
                n_audio++;
                if (t->selected)
                    active_audio = t;
            }
        }
    }

    /* Row 0 in Subtitles: "Subtitles Off" */
    GtkWidget *off_row = create_track_row("Subtitles Disabled (Off)", active_sub == NULL, 0, KAJO_MPV_TRACK_SUB);
    gtk_list_box_append(self->subs_listbox, off_row);
    if (active_sub == NULL)
        gtk_list_box_select_row(self->subs_listbox, GTK_LIST_BOX_ROW(off_row));

    if (st && st->tracks) {
        for (guint i = 0; i < st->tracks->len; i++) {
            const KajoMpvTrack *t = g_ptr_array_index(st->tracks, i);
            g_autofree char *lbl = kajo_mpv_track_format_label(t);
            if (t->type == KAJO_MPV_TRACK_SUB) {
                gboolean sel = (active_sub == t);
                GtkWidget *row = create_track_row(lbl, sel, t->id, KAJO_MPV_TRACK_SUB);
                gtk_list_box_append(self->subs_listbox, row);
                if (sel)
                    gtk_list_box_select_row(self->subs_listbox, GTK_LIST_BOX_ROW(row));
            } else if (t->type == KAJO_MPV_TRACK_AUDIO) {
                gboolean sel = (active_audio == t);
                GtkWidget *row = create_track_row(lbl, sel, t->id, KAJO_MPV_TRACK_AUDIO);
                gtk_list_box_append(self->audio_listbox, row);
                if (sel)
                    gtk_list_box_select_row(self->audio_listbox, GTK_LIST_BOX_ROW(row));
            }
        }
    }

    if (active_sub) {
        g_autofree char *b = g_strdup_printf("[ #%d • %u ]", active_sub->id, n_subs);
        gtk_label_set_text(GTK_LABEL(self->subs_badge_lbl), b);
    } else {
        g_autofree char *b = g_strdup_printf("[ OFF • %u ]", n_subs);
        gtk_label_set_text(GTK_LABEL(self->subs_badge_lbl), b);
    }

    if (active_audio) {
        g_autofree char *b = g_strdup_printf("[ #%d • %u ]", active_audio->id, n_audio);
        gtk_label_set_text(GTK_LABEL(self->audio_badge_lbl), b);
    } else {
        g_autofree char *b = g_strdup_printf("[ %u ]", n_audio);
        gtk_label_set_text(GTK_LABEL(self->audio_badge_lbl), b);
    }

    self->suppress_track_select = FALSE;
}

static void
on_subs_row_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (self->suppress_track_select || !row)
        return;

    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "track-id")) - 1000;
    kajo_player_mpv_set_sid(self->mpv, id);
    self->last_tracks_hash = 0;
}

static void
on_audio_row_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (self->suppress_track_select || !row)
        return;

    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "track-id")) - 1000;
    kajo_player_mpv_set_aid(self->mpv, id);
    self->last_tracks_hash = 0;
}

/* --- Chapters List Builder --- */

static void
rebuild_chapters_ui(KajoPlayerWindow *self)
{
    const KajoMpvState *st = kajo_player_mpv_get_state(self->mpv);
    guint count = (st && st->chapters) ? st->chapters->len : 0;
    int cur_ch = st ? st->current_chapter : -1;

    if (count == self->last_chapters_count && cur_ch == self->last_chapter_idx)
        return;

    self->last_chapters_count = count;
    self->last_chapter_idx = cur_ch;

    gtk_widget_set_visible(self->chapters_card, count > 0);
    if (count == 0)
        return;

    g_autofree char *badge = g_strdup_printf("[ %u ]", count);
    gtk_label_set_text(GTK_LABEL(self->chapters_count_badge), badge);

    self->suppress_chapter_select = TRUE;
    clear_listbox(self->chapters_listbox);

    for (guint i = 0; i < count; i++) {
        const KajoMpvChapter *ch = g_ptr_array_index(st->chapters, i);
        g_autofree char *t_str = kajo_player_format_time(ch->time_usec);
        g_autofree char *lbl = g_strdup_printf("%02d  [%s]  %s", ch->index + 1, t_str, ch->title);

        GtkWidget *row = create_track_row(lbl, (int)i == cur_ch, ch->index, KAJO_MPV_TRACK_SUB);
        gtk_list_box_append(self->chapters_listbox, row);
        if ((int)i == cur_ch)
            gtk_list_box_select_row(self->chapters_listbox, GTK_LIST_BOX_ROW(row));
    }
    self->suppress_chapter_select = FALSE;
}

static void
on_chapter_row_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoPlayerWindow *self = user_data;
    if (self->suppress_chapter_select || !row)
        return;

    int ch_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "track-id")) - 1000;
    if (ch_idx >= 0)
        kajo_player_mpv_set_chapter(self->mpv, ch_idx);
}

/* --- Technical Telemetry OSD (`I` / `Ctrl+J` / `ℹ STATS`) --- */

static void
update_stats_osd(KajoPlayerWindow *self)
{
    if (!self->stats_visible)
        return;

    const KajoMediaTrack *track = kajo_player_queue_get_current(self->queue);
    const KajoMpvState *st = kajo_player_mpv_get_state(self->mpv);

    if (!track || !st || !st->has_media) {
        gtk_label_set_text(GTK_LABEL(self->stats_osd_label), "STATE      IDLE (No media loaded)");
        return;
    }

    g_autofree char *size_str = kajo_player_format_size(track->size);
    const char *vcodec = (st->video_codec && st->video_codec[0]) ? st->video_codec : "none";
    const char *hwdec = (st->hwdec_current && st->hwdec_current[0]) ? st->hwdec_current : "software (cpu)";
    const char *pixfmt = (st->pixel_format && st->pixel_format[0]) ? st->pixel_format : "n/a";
    const char *cmatrix = (st->colormatrix && st->colormatrix[0]) ? st->colormatrix : "bt.709";
    const char *acodec = (st->audio_codec && st->audio_codec[0]) ? st->audio_codec : "none";

    g_autofree char *text = g_strdup_printf(
        "FILE       %s (%s)\n"
        "VIDEO      %s  •  %d×%d @ %.3f fps\n"
        "DECODER    %s  •  %s (%s)  •  Dropped: %" G_GINT64_FORMAT "\n"
        "AUDIO      %s  •  %d ch @ %d Hz  •  Delay: %+d ms\n"
        "SUBTITLES  %s  •  Delay: %+d ms  •  Scale: %d%%\n"
        "SPEED      %.2f×",
        track->display_name, size_str,
        vcodec, st->video_width, st->video_height, st->fps,
        hwdec, pixfmt, cmatrix, st->dropped_frames,
        acodec, st->audio_channels, st->audio_samplerate, (int)round(st->audio_delay_sec * 1000.0),
        st->sub_visible ? "VISIBLE" : "HIDDEN",
        (int)round(st->sub_delay_sec * 1000.0), (int)round(st->sub_scale * 100.0),
        st->speed
    );

    gtk_label_set_text(GTK_LABEL(self->stats_osd_label), text);
}

static void
toggle_stats_osd(KajoPlayerWindow *self)
{
    self->stats_visible = !self->stats_visible;
    gtk_widget_set_visible(self->stats_osd_box, self->stats_visible);
    if (self->stats_visible) {
        gtk_widget_add_css_class(self->btn_stats, "active");
        update_stats_osd(self);
    } else {
        gtk_widget_remove_css_class(self->btn_stats, "active");
    }
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
        gtk_label_set_text(GTK_LABEL(self->transport_status_lbl),
                           self->toast_message ? self->toast_message : "READY");
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

    if (self->toast_message) {
        gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), self->toast_message);
    } else if (st->error_message) {
        g_autofree char *msg = g_strdup_printf("⚠ %s", st->error_message);
        gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), msg);
    } else {
        const char *state_str = st->playing ? "PLAYING" : "PAUSED";
        if (st->chapters && st->chapters->len > 0 &&
            st->current_chapter >= 0 && (guint)st->current_chapter < st->chapters->len) {
            const KajoMpvChapter *ch = g_ptr_array_index(st->chapters, st->current_chapter);
            g_autofree char *status = g_strdup_printf("%s  •  CH %d/%u: %s",
                state_str, st->current_chapter + 1, st->chapters->len, ch->title);
            gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), status);
        } else {
            int idx = kajo_player_queue_get_current_index(self->queue);
            guint total = kajo_player_queue_get_length(self->queue);
            if (total > 1 && idx >= 0) {
                g_autofree char *status = g_strdup_printf("%s  •  TRACK %d / %u", state_str, idx + 1, total);
                gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), status);
            } else {
                gtk_label_set_text(GTK_LABEL(self->transport_status_lbl), state_str);
            }
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
    update_stats_osd(self);
    rebuild_tracks_ui(self);
    rebuild_chapters_ui(self);
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
    self->last_tracks_hash = 0;
    self->last_chapters_count = 0;
    self->last_chapter_idx = -1;

    if (!track || !track->file) {
        kajo_player_mpv_stop(self->mpv);
        gtk_label_set_text(GTK_LABEL(self->title_label), "🎬 KAJO PLAYER");
        gtk_window_set_title(GTK_WINDOW(self), "KAJO PLAYER");
        gtk_widget_set_visible(self->empty_state_box, TRUE);
        gtk_widget_set_visible(self->audio_state_box, FALSE);
        update_stream_badge(self);
        update_transport_ui(self);
        update_stats_osd(self);
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
    update_stats_osd(self);
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

static void
take_frame_screenshot(KajoPlayerWindow *self)
{
    g_autofree char *saved_path = NULL;
    if (kajo_player_mpv_take_screenshot(self->mpv, &saved_path) && saved_path) {
        g_autofree char *base = g_path_get_basename(saved_path);
        g_autofree char *msg = g_strdup_printf("📷 SAVED: %s", base);
        show_status_toast(self, msg);
    }
}

/* --- File & External Subtitle Open Dialogs --- */

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

static void
on_sub_file_dialog_opened(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    KajoPlayerWindow *self = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, res, &err);
    if (file) {
        g_autofree char *path = g_file_get_path(file);
        if (path) {
            kajo_player_mpv_add_subtitle_file(self->mpv, path);
            g_autofree char *base = g_path_get_basename(path);
            g_autofree char *msg = g_strdup_printf("💬 SUBTITLE LOADED: %s", base);
            show_status_toast(self, msg);
        }
    }
}

static void
open_subtitle_chooser(KajoPlayerWindow *self)
{
    if (self->subs_popover)
        gtk_popover_popdown(self->subs_popover);

    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Load External Subtitle File — Kajo Player");

    g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Subtitle Files (.srt, .ass, .ssa, .vtt, .sub, .sup)");
    gtk_file_filter_add_pattern(filter, "*.srt");
    gtk_file_filter_add_pattern(filter, "*.ass");
    gtk_file_filter_add_pattern(filter, "*.ssa");
    gtk_file_filter_add_pattern(filter, "*.vtt");
    gtk_file_filter_add_pattern(filter, "*.sub");
    gtk_file_filter_add_pattern(filter, "*.sup");

    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));

    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_sub_file_dialog_opened, self);
}

/* --- Button & Scale Signal Handlers --- */

static void
on_btn_stats_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    toggle_stats_osd(KAJO_PLAYER_WINDOW(user_data));
}

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
    niri_toggle_window_floating();
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
on_btn_screenshot_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    take_frame_screenshot(KAJO_PLAYER_WINDOW(user_data));
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

/* Speed & Frame Popover Handlers */
static void on_btn_spd_05(GtkButton *b G_GNUC_UNUSED, gpointer d)  { kajo_player_mpv_set_speed(KAJO_PLAYER_WINDOW(d)->mpv, 0.5); }
static void on_btn_spd_075(GtkButton *b G_GNUC_UNUSED, gpointer d) { kajo_player_mpv_set_speed(KAJO_PLAYER_WINDOW(d)->mpv, 0.75); }
static void on_btn_spd_10(GtkButton *b G_GNUC_UNUSED, gpointer d)  { kajo_player_mpv_set_speed(KAJO_PLAYER_WINDOW(d)->mpv, 1.0); }
static void on_btn_spd_125(GtkButton *b G_GNUC_UNUSED, gpointer d) { kajo_player_mpv_set_speed(KAJO_PLAYER_WINDOW(d)->mpv, 1.25); }
static void on_btn_spd_15(GtkButton *b G_GNUC_UNUSED, gpointer d)  { kajo_player_mpv_set_speed(KAJO_PLAYER_WINDOW(d)->mpv, 1.5); }
static void on_btn_spd_20(GtkButton *b G_GNUC_UNUSED, gpointer d)  { kajo_player_mpv_set_speed(KAJO_PLAYER_WINDOW(d)->mpv, 2.0); }

static void
on_btn_frame_prev_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_frame_step(KAJO_PLAYER_WINDOW(d)->mpv, TRUE);
}

static void
on_btn_frame_next_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_frame_step(KAJO_PLAYER_WINDOW(d)->mpv, FALSE);
}

static void
on_btn_ab_loop_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoPlayerWindow *self = KAJO_PLAYER_WINDOW(d);
    kajo_player_mpv_cycle_ab_loop(self->mpv);
    show_status_toast(self, "🔁 A–B LOOP CYCLED");
}

/* Subtitle & Audio Delay / Scale Handlers */
static void
on_btn_load_sub_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    open_subtitle_chooser(KAJO_PLAYER_WINDOW(d));
}

static void
on_btn_sub_delay_dec(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_adjust_sub_delay(KAJO_PLAYER_WINDOW(d)->mpv, -0.100);
}

static void
on_btn_sub_delay_inc(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_adjust_sub_delay(KAJO_PLAYER_WINDOW(d)->mpv, 0.100);
}

static void
on_btn_sub_delay_reset(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_set_sub_delay(KAJO_PLAYER_WINDOW(d)->mpv, 0.0);
}

static void
on_btn_sub_scale_dec(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_adjust_sub_scale(KAJO_PLAYER_WINDOW(d)->mpv, -0.10);
}

static void
on_btn_sub_scale_inc(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_adjust_sub_scale(KAJO_PLAYER_WINDOW(d)->mpv, 0.10);
}

static void
on_btn_audio_delay_dec(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_adjust_audio_delay(KAJO_PLAYER_WINDOW(d)->mpv, -0.100);
}

static void
on_btn_audio_delay_inc(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_adjust_audio_delay(KAJO_PLAYER_WINDOW(d)->mpv, 0.100);
}

static void
on_btn_audio_delay_reset(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    kajo_player_mpv_set_audio_delay(KAJO_PLAYER_WINDOW(d)->mpv, 0.0);
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

/* --- Drag-and-Drop Media & External Subtitle Support --- */

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
            g_autofree char *path = g_file_get_path(file);
            if (path && (g_str_has_suffix(path, ".srt") ||
                         g_str_has_suffix(path, ".ass") ||
                         g_str_has_suffix(path, ".ssa") ||
                         g_str_has_suffix(path, ".vtt"))) {
                kajo_player_mpv_add_subtitle_file(self->mpv, path);
                g_autofree char *base = g_path_get_basename(path);
                g_autofree char *msg = g_strdup_printf("💬 SUBTITLE LOADED: %s", base);
                show_status_toast(self, msg);
            } else {
                kajo_player_window_open_file(self, file);
            }
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
        if (keyval == GDK_KEY_p || keyval == GDK_KEY_P) {
            toggle_queue_drawer(self);
            return TRUE;
        }
        if (keyval == GDK_KEY_j || keyval == GDK_KEY_J ||
            keyval == GDK_KEY_i || keyval == GDK_KEY_I) {
            toggle_stats_osd(self);
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
        if (shift_held) {
            kajo_player_mpv_cycle_ab_loop(self->mpv);
            show_status_toast(self, "🔁 A–B LOOP CYCLED");
        } else {
            toggle_loop(self);
        }
        return TRUE;

    case GDK_KEY_j:
    case GDK_KEY_J:
        kajo_player_mpv_cycle_sub(self->mpv);
        show_status_toast(self, "💬 CYCLED SUBTITLE TRACK");
        return TRUE;

    case GDK_KEY_v:
    case GDK_KEY_V:
        kajo_player_mpv_toggle_sub_visibility(self->mpv);
        return TRUE;

    case GDK_KEY_z:
    case GDK_KEY_Z:
        kajo_player_mpv_adjust_sub_delay(self->mpv, -0.100);
        return TRUE;

    case GDK_KEY_x:
    case GDK_KEY_X:
        kajo_player_mpv_adjust_sub_delay(self->mpv, 0.100);
        return TRUE;

    case GDK_KEY_numbersign:
    case GDK_KEY_a:
    case GDK_KEY_A:
        kajo_player_mpv_cycle_audio(self->mpv);
        show_status_toast(self, "🎧 CYCLED AUDIO STREAM");
        return TRUE;

    case GDK_KEY_bracketleft:
        kajo_player_mpv_adjust_speed(self->mpv, -0.10);
        return TRUE;

    case GDK_KEY_bracketright:
        kajo_player_mpv_adjust_speed(self->mpv, 0.10);
        return TRUE;

    case GDK_KEY_BackSpace:
        kajo_player_mpv_set_speed(self->mpv, 1.0);
        return TRUE;

    case GDK_KEY_period:
        kajo_player_mpv_frame_step(self->mpv, FALSE);
        return TRUE;

    case GDK_KEY_comma:
        kajo_player_mpv_frame_step(self->mpv, TRUE);
        return TRUE;

    case GDK_KEY_s:
    case GDK_KEY_S:
        take_frame_screenshot(self);
        return TRUE;

    case GDK_KEY_i:
    case GDK_KEY_I:
        toggle_stats_osd(self);
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
    if (self->toast_timer_id > 0) {
        g_source_remove(self->toast_timer_id);
        self->toast_timer_id = 0;
    }
    g_clear_pointer(&self->toast_message, g_free);

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
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_stats);
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

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, stats_osd_box);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, stats_osd_label);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, queue_vbox);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, chapters_card);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, chapters_count_badge);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, chapters_listbox);
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

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_speed);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, speed_popover);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, speed_badge_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_spd_05);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_spd_075);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_spd_10);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_spd_125);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_spd_15);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_spd_20);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_frame_prev);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_frame_next);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_ab_loop);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_subs);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, subs_popover);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, subs_badge_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, subs_listbox);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_load_sub);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_sub_delay_dec);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, sub_delay_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_sub_delay_inc);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_sub_delay_reset);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_sub_scale_dec);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, sub_scale_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_sub_scale_inc);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_audio);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, audio_popover);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, audio_badge_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, audio_listbox);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_audio_delay_dec);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, audio_delay_lbl);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_audio_delay_inc);
    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_audio_delay_reset);

    gtk_widget_class_bind_template_child(widget_class, KajoPlayerWindow, btn_screenshot);
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
    self->stats_visible = FALSE;
    self->last_mpris_duration_usec = -1;
    self->last_chapter_idx = -1;

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
    rebuild_tracks_ui(self);

    /* Connect Header & Transport Buttons */
    g_signal_connect(self->btn_stats, "clicked", G_CALLBACK(on_btn_stats_clicked), self);
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
    g_signal_connect(self->btn_screenshot, "clicked", G_CALLBACK(on_btn_screenshot_clicked), self);
    g_signal_connect(self->btn_mute, "clicked", G_CALLBACK(on_btn_mute_clicked), self);
    g_signal_connect(self->btn_fullscreen, "clicked", G_CALLBACK(on_btn_fullscreen_clicked), self);

    /* Speed & Frame Popover Signals */
    g_signal_connect(self->btn_spd_05, "clicked", G_CALLBACK(on_btn_spd_05), self);
    g_signal_connect(self->btn_spd_075, "clicked", G_CALLBACK(on_btn_spd_075), self);
    g_signal_connect(self->btn_spd_10, "clicked", G_CALLBACK(on_btn_spd_10), self);
    g_signal_connect(self->btn_spd_125, "clicked", G_CALLBACK(on_btn_spd_125), self);
    g_signal_connect(self->btn_spd_15, "clicked", G_CALLBACK(on_btn_spd_15), self);
    g_signal_connect(self->btn_spd_20, "clicked", G_CALLBACK(on_btn_spd_20), self);
    g_signal_connect(self->btn_frame_prev, "clicked", G_CALLBACK(on_btn_frame_prev_clicked), self);
    g_signal_connect(self->btn_frame_next, "clicked", G_CALLBACK(on_btn_frame_next_clicked), self);
    g_signal_connect(self->btn_ab_loop, "clicked", G_CALLBACK(on_btn_ab_loop_clicked), self);

    /* Subtitles & Audio Popover Signals */
    g_signal_connect(self->subs_listbox, "row-activated", G_CALLBACK(on_subs_row_activated), self);
    g_signal_connect(self->btn_load_sub, "clicked", G_CALLBACK(on_btn_load_sub_clicked), self);
    g_signal_connect(self->btn_sub_delay_dec, "clicked", G_CALLBACK(on_btn_sub_delay_dec), self);
    g_signal_connect(self->btn_sub_delay_inc, "clicked", G_CALLBACK(on_btn_sub_delay_inc), self);
    g_signal_connect(self->btn_sub_delay_reset, "clicked", G_CALLBACK(on_btn_sub_delay_reset), self);
    g_signal_connect(self->btn_sub_scale_dec, "clicked", G_CALLBACK(on_btn_sub_scale_dec), self);
    g_signal_connect(self->btn_sub_scale_inc, "clicked", G_CALLBACK(on_btn_sub_scale_inc), self);

    g_signal_connect(self->audio_listbox, "row-activated", G_CALLBACK(on_audio_row_activated), self);
    g_signal_connect(self->btn_audio_delay_dec, "clicked", G_CALLBACK(on_btn_audio_delay_dec), self);
    g_signal_connect(self->btn_audio_delay_inc, "clicked", G_CALLBACK(on_btn_audio_delay_inc), self);
    g_signal_connect(self->btn_audio_delay_reset, "clicked", G_CALLBACK(on_btn_audio_delay_reset), self);

    g_signal_connect(self->chapters_listbox, "row-activated", G_CALLBACK(on_chapter_row_activated), self);

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

    /* Drag-and-Drop File & External Subtitle Support */
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
