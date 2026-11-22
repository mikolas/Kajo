#include "player_window_private.h"

gboolean
player_popovers_any_open(KajoPlayerWindow *self)
{
    return (self->speed_popover && gtk_widget_get_visible(GTK_WIDGET(self->speed_popover))) ||
           (self->subs_popover && gtk_widget_get_visible(GTK_WIDGET(self->subs_popover))) ||
           (self->audio_popover && gtk_widget_get_visible(GTK_WIDGET(self->audio_popover)));
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

void
player_popovers_rebuild_tracks_ui(KajoPlayerWindow *self)
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

void
player_popovers_rebuild_chapters_ui(KajoPlayerWindow *self)
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

void
player_popovers_update_stats_osd(KajoPlayerWindow *self)
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

void
player_popovers_toggle_stats_osd(KajoPlayerWindow *self)
{
    self->stats_visible = !self->stats_visible;
    gtk_widget_set_visible(self->stats_osd_box, self->stats_visible);
    if (self->stats_visible) {
        gtk_widget_add_css_class(self->btn_stats, "active");
        player_popovers_update_stats_osd(self);
    } else {
        gtk_widget_remove_css_class(self->btn_stats, "active");
    }
}

/* --- External Subtitle Open Dialog --- */

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
            player_window_show_status_toast(self, msg);
        }
    }
    g_object_unref(self); /* ref taken in open_subtitle_chooser() */
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

    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_sub_file_dialog_opened, g_object_ref(self));
}

/* --- Popover & Stats Signal Handlers --- */

static void
on_btn_stats_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    player_popovers_toggle_stats_osd(KAJO_PLAYER_WINDOW(user_data));
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
    player_window_show_status_toast(self, "🔁 A–B LOOP CYCLED");
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

void
player_popovers_connect_signals(KajoPlayerWindow *self)
{
    g_signal_connect(self->btn_stats, "clicked", G_CALLBACK(on_btn_stats_clicked), self);

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
}
