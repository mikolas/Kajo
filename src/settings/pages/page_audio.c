#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../audio/pa_conn.h"
#include <math.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <stdint.h>
#include <string.h>

/* ─── Programmatic Bento Page Subclass: 04 / 12 Audio ─── */

typedef struct _SettingsPageAudio {
    GtkBox parent_instance;

    GtkWidget *card_out;
    GtkWidget *combo_sink;
    GtkWidget *scale_vol;
    GtkWidget *lbl_vol_val;
    GtkWidget *sw_out_mute;

    GtkWidget *card_in;
    GtkWidget *combo_source;
    GtkWidget *scale_mic;
    GtkWidget *lbl_mic_val;
    GtkWidget *sw_in_mute;

    GtkWidget *card_streams;
    GtkWidget *list_streams;

    ShellPaConn    pa;
    pa_stream     *test_stream;
    gchar         *server_label;
    gchar         *default_sink_name;
    gchar         *default_sink_short;
    gchar         *default_source_name;
    guint8         sink_channels;
    guint8         source_channels;
    guint32        out_vol_pct;
    guint32        in_vol_pct;
    gboolean       out_muted;
    gboolean       in_muted;

    GPtrArray     *sink_names;
    GPtrArray     *source_names;
    GtkStringList *sink_string_list;
    GtkStringList *source_string_list;
    GPtrArray     *pending_tags;
    guint          sink_query_seq;
    guint          source_query_seq;
    guint          stream_query_seq;
    guint          active_stream_count;
    gboolean       updating_sinks;
    gboolean       updating_sources;
    gboolean       updating_ui;
} SettingsPageAudio;

typedef struct {
    GWeakRef self_ref;
    guint    query_seq;
} AudioQueryTag;

typedef struct _SettingsPageAudioClass {
    GtkBoxClass parent_class;
} SettingsPageAudioClass;

G_DEFINE_TYPE(SettingsPageAudio, settings_page_audio, GTK_TYPE_BOX)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SettingsPageAudio, g_object_unref)

static void audio_query_server_info(SettingsPageAudio *self);
static void audio_query_sinks(SettingsPageAudio *self);
static void audio_query_sources(SettingsPageAudio *self);
static void audio_query_streams(SettingsPageAudio *self);
static void audio_sync_sink_selection(SettingsPageAudio *self);
static void audio_sync_source_selection(SettingsPageAudio *self);

static inline void
unref_pa_op(pa_operation *op)
{
    if (op)
        pa_operation_unref(op);
}

static void
audio_query_tag_free(AudioQueryTag *tag)
{
    if (!tag)
        return;
    g_weak_ref_clear(&tag->self_ref);
    g_free(tag);
}

static void
clear_audio_box(GtkWidget *box)
{
    if (!box)
        return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), child);
}

static guint32
audio_cvolume_to_percent(const pa_cvolume *cv)
{
    if (!cv || cv->channels == 0)
        return 0;
    pa_volume_t avg = pa_cvolume_avg(cv);
    double pct = ((double)avg / (double)PA_VOLUME_NORM) * 100.0;
    if (pct < 0.0)
        pct = 0.0;
    if (pct > 150.0)
        pct = 150.0;
    return (guint32)(pct + 0.5);
}

static void
update_audio_telemetry(SettingsPageAudio *self)
{
    gboolean ready = (self->pa.ctx && pa_context_get_state(self->pa.ctx) == PA_CONTEXT_READY);

    g_autofree gchar *out_ch_badge = g_strdup_printf("%u %s",
        self->sink_channels, self->sink_channels == 1 ? "CHANNEL" : "CHANNELS");
    section_card_set_badge(self->card_out, out_ch_badge, "accent");

    g_autofree gchar *in_ch_badge = g_strdup_printf("%u %s",
        self->source_channels, self->source_channels == 1 ? "CHANNEL" : "CHANNELS");
    section_card_set_badge(self->card_in, in_ch_badge, NULL);

    g_autofree gchar *out_val = self->out_muted
        ? g_strdup("MUTED")
        : g_strdup_printf("%u%% (%uCH)", self->out_vol_pct, self->sink_channels);
    g_autofree gchar *in_val = self->in_muted
        ? g_strdup("MUTED")
        : g_strdup_printf("%u%% (%uCH)", self->in_vol_pct, self->source_channels);

    settings_page_set_counter(GTK_WIDGET(self), 0, "SERVER",
                              ready ? (self->server_label ? self->server_label : "PipeWire-Pulse")
                                    : "RECONNECTING",
                              ready ? "success" : "warn");
    settings_page_set_counter(GTK_WIDGET(self), 1, "SINK",
                              (self->default_sink_short && self->default_sink_short[0])
                                  ? self->default_sink_short
                                  : "--",
                              "accent");
    settings_page_set_counter(GTK_WIDGET(self), 2, "OUT VOL", out_val,
                              self->out_muted ? "warn" : "accent");
    settings_page_set_counter(GTK_WIDGET(self), 3, "MIC GAIN", in_val,
                              self->in_muted ? "warn" : NULL);

    g_autofree gchar *nav_txt = self->out_muted
        ? g_strdup("MUTE")
        : g_strdup_printf("%u%%", self->out_vol_pct);
    settings_page_set_nav_badge(GTK_WIDGET(self), "audio", nav_txt,
                                self->out_muted ? "warn" : NULL);
}

/* ─── Non-Blocking Synthesized Stereo Test Chime ─── */

static void
audio_drop_test_stream(SettingsPageAudio *self)
{
    if (!self || !self->test_stream)
        return;
    pa_stream *s = self->test_stream;
    self->test_stream = NULL;
    pa_stream_set_state_callback(s, NULL, NULL);
    pa_stream_disconnect(s);
    pa_stream_unref(s);
}

static void
on_test_stream_drained_cb(pa_stream *s G_GNUC_UNUSED, int success G_GNUC_UNUSED, void *userdata)
{
    SettingsPageAudio *self = userdata;
    audio_drop_test_stream(self);
    if (self) {
        settings_page_set_live_status(GTK_WIDGET(self), "✓ PIPEWIRE LIVE",
                                      "Multi-channel routing & volume apply immediately");
    }
}

static void
write_synthesized_stereo_chime(SettingsPageAudio *self, pa_stream *s)
{
    const guint32 rate = 48000;
    const guint32 frames_note1 = (rate * 16) / 100; /* 160 ms: C5 (523.25 Hz) */
    const guint32 frames_note2 = (rate * 20) / 100; /* 200 ms: E5 (659.25 Hz) */
    const guint32 total_frames = frames_note1 + frames_note2;
    const gsize pcm_bytes = (gsize)total_frames * 2 * sizeof(int16_t);

    int16_t *pcm = g_malloc0(pcm_bytes);
    const double two_pi = 6.28318530717958647692;

    for (guint32 i = 0; i < total_frames; i++) {
        gboolean second = (i >= frames_note1);
        guint32 local_i = second ? (i - frames_note1) : i;
        guint32 local_n = second ? frames_note2 : frames_note1;
        double freq = second ? 659.255 : 523.251;

        /* Smooth raised-cosine attack (12ms) + exponential-cosine decay */
        double env = 1.0;
        guint32 attack = (rate * 12) / 1000;
        if (local_i < attack)
            env = 0.5 * (1.0 - cos(G_PI * (double)local_i / (double)attack));
        double progress = (double)local_i / (double)local_n;
        env *= (1.0 - 0.72 * progress) * cos(0.5 * G_PI * progress);

        double phase = two_pi * freq * ((double)i / (double)rate);
        double wave = sin(phase) + 0.22 * sin(2.0 * phase);
        double gain_l = second ? 0.14 : 0.22;
        double gain_r = second ? 0.22 : 0.14;

        pcm[i * 2 + 0] = (int16_t)CLAMP(wave * env * gain_l * 32767.0, -32767.0, 32767.0);
        pcm[i * 2 + 1] = (int16_t)CLAMP(wave * env * gain_r * 32767.0, -32767.0, 32767.0);
    }

    if (pa_stream_write(s, pcm, pcm_bytes, g_free, 0, PA_SEEK_RELATIVE) < 0) {
        g_free(pcm);
        audio_drop_test_stream(self);
        return;
    }

    unref_pa_op(pa_stream_drain(s, on_test_stream_drained_cb, self));
}

static void
on_test_stream_state_cb(pa_stream *s, void *userdata)
{
    SettingsPageAudio *self = userdata;
    if (!self || s != self->test_stream)
        return;

    pa_stream_state_t st = pa_stream_get_state(s);
    if (st == PA_STREAM_READY) {
        write_synthesized_stereo_chime(self, s);
    } else if (st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) {
        audio_drop_test_stream(self);
    }
}

static void
on_audio_test_sound(gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (!self || !self->pa.ctx || pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    audio_drop_test_stream(self);

    static const pa_sample_spec spec = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = 48000,
        .channels = 2
    };

    pa_proplist *pl = pa_proplist_new();
    pa_proplist_sets(pl, PA_PROP_MEDIA_ROLE, "event");
    pa_proplist_sets(pl, PA_PROP_MEDIA_NAME, "Kajo Stereo Output Verification");
    pa_stream *s = pa_stream_new_with_proplist(self->pa.ctx, "kajo-settings-chime", &spec, NULL, pl);
    pa_proplist_free(pl);
    if (!s)
        return;

    self->test_stream = s;
    pa_stream_set_state_callback(s, on_test_stream_state_cb, self);
    settings_page_set_live_status(GTK_WIDGET(self), "🔊 PLAYING CHIME",
                                  "Playing stereo C5→E5 verification chime on default sink…");

    if (pa_stream_connect_playback(s, self->default_sink_name, NULL,
                                   PA_STREAM_NOFLAGS, NULL, NULL) < 0) {
        audio_drop_test_stream(self);
    }
}

/* ─── Sink, Source & Application Playback Stream Queries ─── */

static void
audio_sink_info_cb(pa_context *ctx G_GNUC_UNUSED, const pa_sink_info *info, int eol, void *userdata)
{
    SettingsPageAudio *self = userdata;
    if (eol > 0 || !info || !self)
        return;

    if (self->default_sink_name && g_strcmp0(info->name, self->default_sink_name) == 0) {
        guint8 ch = info->channel_map.channels ? info->channel_map.channels : info->volume.channels;
        self->sink_channels = ch ? ch : 2;
        self->out_vol_pct = audio_cvolume_to_percent(&info->volume);
        self->out_muted = (info->mute != 0);

        const char *desc = (info->description && info->description[0]) ? info->description : info->name;
        g_free(self->default_sink_short);
        self->default_sink_short = g_strdup_printf("%uCH %s", self->sink_channels, desc);

        self->updating_ui = TRUE;
        gtk_range_set_value(GTK_RANGE(self->scale_vol), (double)self->out_vol_pct);
        gtk_switch_set_active(GTK_SWITCH(self->sw_out_mute), self->out_muted);
        g_autofree gchar *str = g_strdup_printf("%u%%", self->out_vol_pct);
        gtk_label_set_text(GTK_LABEL(self->lbl_vol_val), str);
        self->updating_ui = FALSE;

        update_audio_telemetry(self);
    }
}

static void
audio_source_info_cb(pa_context *ctx G_GNUC_UNUSED, const pa_source_info *info, int eol, void *userdata)
{
    SettingsPageAudio *self = userdata;
    if (eol > 0 || !info || !self)
        return;

    if (self->default_source_name && g_strcmp0(info->name, self->default_source_name) == 0) {
        guint8 ch = info->channel_map.channels ? info->channel_map.channels : info->volume.channels;
        self->source_channels = ch ? ch : 2;
        self->in_vol_pct = audio_cvolume_to_percent(&info->volume);
        self->in_muted = (info->mute != 0);

        self->updating_ui = TRUE;
        gtk_range_set_value(GTK_RANGE(self->scale_mic), (double)self->in_vol_pct);
        gtk_switch_set_active(GTK_SWITCH(self->sw_in_mute), self->in_muted);
        g_autofree gchar *str = g_strdup_printf("%u%%", self->in_vol_pct);
        gtk_label_set_text(GTK_LABEL(self->lbl_mic_val), str);
        self->updating_ui = FALSE;

        update_audio_telemetry(self);
    }
}

static void
audio_sync_sink_selection(SettingsPageAudio *self)
{
    if (!self || !self->combo_sink || !self->sink_names || !self->default_sink_name)
        return;

    for (guint i = 0; i < self->sink_names->len; i++) {
        const char *name = g_ptr_array_index(self->sink_names, i);
        if (g_strcmp0(name, self->default_sink_name) == 0) {
            gboolean prev = self->updating_ui;
            self->updating_ui = TRUE;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_sink), i);
            self->updating_ui = prev;
            break;
        }
    }
}

static void
audio_sync_source_selection(SettingsPageAudio *self)
{
    if (!self || !self->combo_source || !self->source_names || !self->default_source_name)
        return;

    for (guint i = 0; i < self->source_names->len; i++) {
        const char *name = g_ptr_array_index(self->source_names, i);
        if (g_strcmp0(name, self->default_source_name) == 0) {
            gboolean prev = self->updating_ui;
            self->updating_ui = TRUE;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_source), i);
            self->updating_ui = prev;
            break;
        }
    }
}

static void
finish_audio_tag(SettingsPageAudio *self, AudioQueryTag *tag)
{
    if (self && self->pending_tags)
        g_ptr_array_remove_fast(self->pending_tags, tag);
    else
        audio_query_tag_free(tag);
}

static void
audio_sink_list_cb(pa_context *ctx G_GNUC_UNUSED, const pa_sink_info *info, int eol, void *userdata)
{
    AudioQueryTag *tag = userdata;
    if (!tag)
        return;

    g_autoptr(SettingsPageAudio) self = g_weak_ref_get(&tag->self_ref);
    if (!self || tag->query_seq != self->sink_query_seq) {
        if (eol != 0)
            finish_audio_tag(self, tag);
        return;
    }

    if (eol != 0) {
        self->updating_sinks = FALSE;
        audio_sync_sink_selection(self);
        finish_audio_tag(self, tag);
        return;
    }
    if (!info)
        return;

    for (guint i = 0; i < self->sink_names->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->sink_names, i), info->name) == 0)
            return;
    }

    guint8 ch = info->channel_map.channels ? info->channel_map.channels : info->volume.channels;
    if (ch == 0)
        ch = 2;
    const char *desc = (info->description && info->description[0]) ? info->description : info->name;
    g_autofree gchar *label = g_strdup_printf("%s (%uch)", desc, ch);

    g_ptr_array_add(self->sink_names, g_strdup(info->name));
    gtk_string_list_append(self->sink_string_list, label);

    if (self->default_sink_name && g_strcmp0(info->name, self->default_sink_name) == 0) {
        self->sink_channels = ch;
        audio_sync_sink_selection(self);
        update_audio_telemetry(self);
    }
}

static void
audio_source_list_cb(pa_context *ctx G_GNUC_UNUSED, const pa_source_info *info, int eol, void *userdata)
{
    AudioQueryTag *tag = userdata;
    if (!tag)
        return;

    g_autoptr(SettingsPageAudio) self = g_weak_ref_get(&tag->self_ref);
    if (!self || tag->query_seq != self->source_query_seq) {
        if (eol != 0)
            finish_audio_tag(self, tag);
        return;
    }

    if (eol != 0) {
        self->updating_sources = FALSE;
        audio_sync_source_selection(self);
        finish_audio_tag(self, tag);
        return;
    }
    if (!info || info->monitor_of_sink != PA_INVALID_INDEX)
        return;

    for (guint i = 0; i < self->source_names->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->source_names, i), info->name) == 0)
            return;
    }

    guint8 ch = info->channel_map.channels ? info->channel_map.channels : info->volume.channels;
    if (ch == 0)
        ch = 2;
    const char *desc = (info->description && info->description[0]) ? info->description : info->name;
    g_autofree gchar *label = g_strdup_printf("%s (%uch)", desc, ch);

    g_ptr_array_add(self->source_names, g_strdup(info->name));
    gtk_string_list_append(self->source_string_list, label);

    if (self->default_source_name && g_strcmp0(info->name, self->default_source_name) == 0) {
        self->source_channels = ch;
        audio_sync_source_selection(self);
        update_audio_telemetry(self);
    }
}

static void
on_stream_vol_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (!self || self->updating_ui || !self->pa.ctx)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    uint32_t idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(range), "stream-idx"));
    guint8 ch = (guint8)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(range), "stream-ch"));
    if (ch == 0)
        ch = 2;

    double val = gtk_range_get_value(range);
    pa_volume_t vol = (pa_volume_t)(val * PA_VOLUME_NORM / 100.0);
    pa_cvolume cv;
    pa_cvolume_set(&cv, ch, vol);
    unref_pa_op(pa_context_set_sink_input_volume(self->pa.ctx, idx, &cv, NULL, NULL));
}

static void
on_stream_mute_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (!self || self->updating_ui || !self->pa.ctx)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    uint32_t idx = GPOINTER_TO_UINT(g_object_get_data(obj, "stream-idx"));
    gboolean muted = gtk_switch_get_active(GTK_SWITCH(obj));
    unref_pa_op(pa_context_set_sink_input_mute(self->pa.ctx, idx, muted ? 1 : 0, NULL, NULL));
}

static void
audio_sink_input_list_cb(pa_context *ctx G_GNUC_UNUSED,
                         const pa_sink_input_info *info,
                         int eol,
                         void *userdata)
{
    AudioQueryTag *tag = userdata;
    if (!tag)
        return;

    g_autoptr(SettingsPageAudio) self = g_weak_ref_get(&tag->self_ref);
    if (!self || tag->query_seq != self->stream_query_seq) {
        if (eol != 0)
            finish_audio_tag(self, tag);
        return;
    }

    if (eol != 0) {
        if (self->active_stream_count == 0) {
            gtk_box_append(GTK_BOX(self->list_streams),
                           create_empty_state_label("No active application playback streams."));
        } else {
            int pw = gtk_widget_get_width(GTK_WIDGET(self));
            settings_update_descendant_scales(self->list_streams, pw > 0 && pw < 520);
        }
        g_autofree gchar *btxt = g_strdup_printf("%u ACTIVE", self->active_stream_count);
        section_card_set_badge(self->card_streams, btxt,
                               self->active_stream_count > 0 ? "success" : NULL);
        finish_audio_tag(self, tag);
        return;
    }
    if (!info)
        return;

    if (g_strcmp0(info->name, "kajo-settings-chime") == 0)
        return;

    const char *app_name = info->proplist
                               ? pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME)
                               : NULL;
    const char *media_name = info->proplist
                                 ? pa_proplist_gets(info->proplist, PA_PROP_MEDIA_NAME)
                                 : NULL;
    const char *title = (app_name && app_name[0]) ? app_name : (info->name ? info->name : "Audio Stream");

    guint8 ch = info->channel_map.channels ? info->channel_map.channels : info->volume.channels;
    if (ch == 0)
        ch = 2;
    guint32 pct = audio_cvolume_to_percent(&info->volume);

    g_autofree gchar *sub = (media_name && media_name[0] && g_strcmp0(media_name, title) != 0)
                                ? g_strdup_printf("%s • %uch stream", media_name, ch)
                                : g_strdup_printf("Sink Input #%u • %uch stream", info->index, ch);

    GtkWidget *st_scale = NULL;
    GtkWidget *st_badge = NULL;
    GtkWidget *scale_wrap = create_scale_with_badge(0.0, 150.0, 1.0, (double)pct,
                                                    "%.0f%%", &st_scale, &st_badge);
    g_object_set_data(G_OBJECT(st_scale), "stream-idx", GUINT_TO_POINTER(info->index));
    g_object_set_data(G_OBJECT(st_scale), "stream-ch", GUINT_TO_POINTER((guint)ch));
    g_signal_connect(st_scale, "value-changed", G_CALLBACK(on_stream_vol_changed), self);

    GtkWidget *st_mute = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(st_mute), info->mute != 0);
    gtk_widget_set_valign(st_mute, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(st_mute), "stream-idx", GUINT_TO_POINTER(info->index));
    g_signal_connect(st_mute, "notify::active", G_CALLBACK(on_stream_mute_changed), self);

    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(right_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(right_box), scale_wrap);
    gtk_box_append(GTK_BOX(right_box), st_mute);

    gtk_box_append(GTK_BOX(self->list_streams),
                   create_boxed_row("🎵", title, sub, right_box));
    self->active_stream_count++;
}

static void
audio_server_info_cb(pa_context *ctx, const pa_server_info *info, void *userdata)
{
    SettingsPageAudio *self = userdata;
    if (!info || !self)
        return;

    g_free(self->server_label);
    if (info->server_name && strstr(info->server_name, "PipeWire"))
        self->server_label = g_strdup("PipeWire-Pulse");
    else if (info->server_name && info->server_name[0])
        self->server_label = g_strdup(info->server_name);
    else
        self->server_label = g_strdup("PulseAudio");

    if (info->default_sink_name) {
        gchar *dup = g_strdup(info->default_sink_name);
        g_free(self->default_sink_name);
        self->default_sink_name = dup;
        audio_sync_sink_selection(self);
        unref_pa_op(pa_context_get_sink_info_by_name(ctx, self->default_sink_name,
                                                     audio_sink_info_cb, self));
    }
    if (info->default_source_name) {
        gchar *dup = g_strdup(info->default_source_name);
        g_free(self->default_source_name);
        self->default_source_name = dup;
        audio_sync_source_selection(self);
        unref_pa_op(pa_context_get_source_info_by_name(ctx, self->default_source_name,
                                                       audio_source_info_cb, self));
    }
    update_audio_telemetry(self);
}

static void
audio_query_server_info(SettingsPageAudio *self)
{
    if (!self->pa.ctx || pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;
    unref_pa_op(pa_context_get_server_info(self->pa.ctx, audio_server_info_cb, self));
}

static AudioQueryTag *
create_tracked_tag(SettingsPageAudio *self, guint seq)
{
    AudioQueryTag *tag = g_new0(AudioQueryTag, 1);
    g_weak_ref_init(&tag->self_ref, self);
    tag->query_seq = seq;
    if (self->pending_tags)
        g_ptr_array_add(self->pending_tags, tag);
    return tag;
}

static void
audio_query_sinks(SettingsPageAudio *self)
{
    if (!self->pa.ctx || pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    self->sink_query_seq++;
    self->updating_sinks = TRUE;
    g_ptr_array_set_size(self->sink_names, 0);

    GtkStringList *new_list = gtk_string_list_new(NULL);
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_sink), G_LIST_MODEL(new_list));
    g_clear_object(&self->sink_string_list);
    self->sink_string_list = new_list;

    AudioQueryTag *tag = create_tracked_tag(self, self->sink_query_seq);
    pa_operation *op = pa_context_get_sink_info_list(self->pa.ctx, audio_sink_list_cb, tag);
    if (!op) {
        self->updating_sinks = FALSE;
        finish_audio_tag(self, tag);
    } else {
        pa_operation_unref(op);
    }
}

static void
audio_query_sources(SettingsPageAudio *self)
{
    if (!self->pa.ctx || pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    self->source_query_seq++;
    self->updating_sources = TRUE;
    g_ptr_array_set_size(self->source_names, 0);

    GtkStringList *new_list = gtk_string_list_new(NULL);
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_source), G_LIST_MODEL(new_list));
    g_clear_object(&self->source_string_list);
    self->source_string_list = new_list;

    AudioQueryTag *tag = create_tracked_tag(self, self->source_query_seq);
    pa_operation *op = pa_context_get_source_info_list(self->pa.ctx, audio_source_list_cb, tag);
    if (!op) {
        self->updating_sources = FALSE;
        finish_audio_tag(self, tag);
    } else {
        pa_operation_unref(op);
    }
}

static void
audio_query_streams(SettingsPageAudio *self)
{
    if (!self->pa.ctx || pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    self->stream_query_seq++;
    self->active_stream_count = 0;
    clear_audio_box(self->list_streams);

    AudioQueryTag *tag = create_tracked_tag(self, self->stream_query_seq);
    pa_operation *op = pa_context_get_sink_input_info_list(self->pa.ctx,
                                                           audio_sink_input_list_cb, tag);
    if (!op)
        finish_audio_tag(self, tag);
    else
        pa_operation_unref(op);
}

static void
audio_subscription_cb(pa_context *ctx G_GNUC_UNUSED,
                      pa_subscription_event_type_t t,
                      uint32_t idx G_GNUC_UNUSED,
                      void *userdata)
{
    SettingsPageAudio *self = userdata;
    pa_subscription_event_type_t facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_subscription_event_type_t ev_type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    if (facility == PA_SUBSCRIPTION_EVENT_SERVER) {
        audio_query_server_info(self);
        audio_query_sinks(self);
        audio_query_sources(self);
    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        audio_query_server_info(self);
        if (ev_type == PA_SUBSCRIPTION_EVENT_NEW || ev_type == PA_SUBSCRIPTION_EVENT_REMOVE)
            audio_query_sinks(self);
    } else if (facility == PA_SUBSCRIPTION_EVENT_SOURCE) {
        audio_query_server_info(self);
        if (ev_type == PA_SUBSCRIPTION_EVENT_NEW || ev_type == PA_SUBSCRIPTION_EVENT_REMOVE)
            audio_query_sources(self);
    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        audio_query_streams(self);
    }
}

static void
audio_context_drop_cb(gpointer userdata)
{
    SettingsPageAudio *self = userdata;
    audio_drop_test_stream(self);
}

static void
audio_context_state_cb(pa_context *ctx, pa_context_state_t state, gpointer userdata)
{
    SettingsPageAudio *self = userdata;

    if (state == PA_CONTEXT_READY) {
        /* AGENTS.md §4.C: Always include PA_SUBSCRIPTION_MASK_SERVER */
        pa_context_set_subscribe_callback(ctx, audio_subscription_cb, self);
        unref_pa_op(pa_context_subscribe(
            ctx,
            PA_SUBSCRIPTION_MASK_SINK |
            PA_SUBSCRIPTION_MASK_SOURCE |
            PA_SUBSCRIPTION_MASK_SINK_INPUT |
            PA_SUBSCRIPTION_MASK_SERVER,
            NULL, NULL));

        audio_query_server_info(self);
        audio_query_sinks(self);
        audio_query_sources(self);
        audio_query_streams(self);
        settings_page_set_live_status(GTK_WIDGET(self), "✓ PIPEWIRE LIVE",
                                      "Multi-channel routing & volume apply immediately");
    } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
        update_audio_telemetry(self);
        settings_page_set_live_status(GTK_WIDGET(self), "⟳ RECONNECTING",
                                      "PipeWire/PulseAudio disconnected • retrying in 2s…");
    }
}

/* ─── Interactive UI Callbacks ─── */

static void
on_audio_vol_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    double val = gtk_range_get_value(range);
    self->out_vol_pct = (guint32)(val + 0.5);

    if (self->updating_ui || !self->pa.ctx || !self->default_sink_name)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    /* AGENTS.md §4.C: Never hardcode 2 channels; pass sink_channels */
    pa_volume_t vol = (pa_volume_t)(val * PA_VOLUME_NORM / 100.0);
    pa_cvolume cv;
    pa_cvolume_set(&cv, self->sink_channels ? self->sink_channels : 2, vol);
    unref_pa_op(pa_context_set_sink_volume_by_name(
        self->pa.ctx, self->default_sink_name, &cv, NULL, NULL));
    update_audio_telemetry(self);
}

static void
on_audio_mic_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    double val = gtk_range_get_value(range);
    self->in_vol_pct = (guint32)(val + 0.5);

    if (self->updating_ui || !self->pa.ctx || !self->default_source_name)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    pa_volume_t vol = (pa_volume_t)(val * PA_VOLUME_NORM / 100.0);
    pa_cvolume cv;
    pa_cvolume_set(&cv, self->source_channels ? self->source_channels : 2, vol);
    unref_pa_op(pa_context_set_source_volume_by_name(
        self->pa.ctx, self->default_source_name, &cv, NULL, NULL));
    update_audio_telemetry(self);
}

static void
on_audio_out_mute_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (self->updating_ui || !self->pa.ctx || !self->default_sink_name)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    self->out_muted = gtk_switch_get_active(GTK_SWITCH(obj));
    unref_pa_op(pa_context_set_sink_mute_by_name(
        self->pa.ctx, self->default_sink_name, self->out_muted ? 1 : 0, NULL, NULL));
    update_audio_telemetry(self);
}

static void
on_audio_in_mute_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (self->updating_ui || !self->pa.ctx || !self->default_source_name)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    self->in_muted = gtk_switch_get_active(GTK_SWITCH(obj));
    unref_pa_op(pa_context_set_source_mute_by_name(
        self->pa.ctx, self->default_source_name, self->in_muted ? 1 : 0, NULL, NULL));
    update_audio_telemetry(self);
}

static void
on_sink_selected(GObject *gobject, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (self->updating_ui || self->updating_sinks || !self->pa.ctx || !self->sink_names)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (sel < self->sink_names->len) {
        const char *name = g_ptr_array_index(self->sink_names, sel);
        if (name && g_strcmp0(name, self->default_sink_name) != 0) {
            gchar *dup = g_strdup(name);
            g_free(self->default_sink_name);
            self->default_sink_name = dup;
            unref_pa_op(pa_context_set_default_sink(self->pa.ctx, dup, NULL, NULL));
            audio_query_server_info(self);
        }
    }
}

static void
on_source_selected(GObject *gobject, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (self->updating_ui || self->updating_sources || !self->pa.ctx || !self->source_names)
        return;
    if (pa_context_get_state(self->pa.ctx) != PA_CONTEXT_READY)
        return;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (sel < self->source_names->len) {
        const char *name = g_ptr_array_index(self->source_names, sel);
        if (name && g_strcmp0(name, self->default_source_name) != 0) {
            gchar *dup = g_strdup(name);
            g_free(self->default_source_name);
            self->default_source_name = dup;
            unref_pa_op(pa_context_set_default_source(self->pa.ctx, dup, NULL, NULL));
            audio_query_server_info(self);
        }
    }
}

/* ─── Object Lifecycle & Page Builder ─── */

static void
settings_page_audio_dispose(GObject *object)
{
    SettingsPageAudio *self = (SettingsPageAudio *)object;

    self->sink_query_seq++;
    self->source_query_seq++;
    self->stream_query_seq++;

    audio_drop_test_stream(self);
    shell_pa_conn_close(&self->pa);

    if (self->pending_tags) {
        g_ptr_array_free(self->pending_tags, TRUE);
        self->pending_tags = NULL;
    }

    g_clear_pointer(&self->server_label, g_free);
    g_clear_pointer(&self->default_sink_name, g_free);
    g_clear_pointer(&self->default_sink_short, g_free);
    g_clear_pointer(&self->default_source_name, g_free);

    if (self->sink_names) {
        g_ptr_array_free(self->sink_names, TRUE);
        self->sink_names = NULL;
    }
    if (self->source_names) {
        g_ptr_array_free(self->source_names, TRUE);
        self->source_names = NULL;
    }

    g_clear_object(&self->sink_string_list);
    g_clear_object(&self->source_string_list);

    G_OBJECT_CLASS(settings_page_audio_parent_class)->dispose(object);
}

static void
settings_page_audio_init(SettingsPageAudio *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));

    self->sink_channels = 2;
    self->source_channels = 2;
    self->out_vol_pct = 65;
    self->in_vol_pct = 80;
    self->sink_names = g_ptr_array_new_with_free_func(g_free);
    self->source_names = g_ptr_array_new_with_free_func(g_free);
    self->pending_tags = g_ptr_array_new_with_free_func((GDestroyNotify)audio_query_tag_free);
}

static void
settings_page_audio_class_init(SettingsPageAudioClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_audio_dispose;
}

GtkWidget *
build_page_audio(void)
{
    SettingsPageAudio *self = g_object_new(settings_page_audio_get_type(), NULL);

    /* ─── Card 1: Output Sound Devices & Routing ─── */
    self->card_out = create_section_card(
        SETTINGS_CARD_LEFT, "1. OUTPUT SOUND DEVICES & ROUTING",
        "2 CHANNELS", "accent", NULL);

    self->sink_string_list = gtk_string_list_new(NULL);
    self->combo_sink = gtk_drop_down_new(NULL, NULL);
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_sink), G_LIST_MODEL(self->sink_string_list));
    settings_make_dropdown_shrinkable(self->combo_sink);
    g_signal_connect(self->combo_sink, "notify::selected", G_CALLBACK(on_sink_selected), self);
    gtk_box_append(GTK_BOX(self->card_out),
                   create_boxed_row("🔊", "Default Output Sink",
                                    "Preserves sink_channels on surround sinks",
                                    self->combo_sink));

    GtkWidget *vol_scale_wrap = create_scale_with_badge(
        0.0, 150.0, 1.0, 65.0, "%.0f%%", &self->scale_vol, &self->lbl_vol_val);
    gtk_widget_add_css_class(self->lbl_vol_val, "accent");
    g_signal_connect(self->scale_vol, "value-changed", G_CALLBACK(on_audio_vol_changed), self);
    gtk_box_append(GTK_BOX(self->card_out),
                   create_boxed_row("🎚", "Master Output Volume",
                                    "pa_cvolume_set(&cv, sink_channels, vol)",
                                    vol_scale_wrap));

    self->sw_out_mute = gtk_switch_new();
    g_signal_connect(self->sw_out_mute, "notify::active", G_CALLBACK(on_audio_out_mute_changed), self);
    gtk_box_append(GTK_BOX(self->card_out),
                   create_boxed_row("🔇", "Mute Master Output",
                                    "Toggle sink mute without altering channel balance",
                                    self->sw_out_mute));

    /* ─── Card 2: Input Microphone & Capture ─── */
    self->card_in = create_section_card(
        SETTINGS_CARD_CENTER, "2. INPUT MICROPHONE & CAPTURE",
        "2 CHANNELS", NULL, NULL);

    self->source_string_list = gtk_string_list_new(NULL);
    self->combo_source = gtk_drop_down_new(NULL, NULL);
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_source), G_LIST_MODEL(self->source_string_list));
    settings_make_dropdown_shrinkable(self->combo_source);
    g_signal_connect(self->combo_source, "notify::selected", G_CALLBACK(on_source_selected), self);
    gtk_box_append(GTK_BOX(self->card_in),
                   create_boxed_row("🎙", "Default Input Source",
                                    "Monitor sources filtered automatically",
                                    self->combo_source));

    GtkWidget *mic_scale_wrap = create_scale_with_badge(
        0.0, 150.0, 1.0, 80.0, "%.0f%%", &self->scale_mic, &self->lbl_mic_val);
    gtk_widget_remove_css_class(self->lbl_mic_val, "accent");
    g_signal_connect(self->scale_mic, "value-changed", G_CALLBACK(on_audio_mic_changed), self);
    gtk_box_append(GTK_BOX(self->card_in),
                   create_boxed_row("🎚", "Microphone Gain Level",
                                    "pa_context_set_source_volume_by_name",
                                    mic_scale_wrap));

    self->sw_in_mute = gtk_switch_new();
    g_signal_connect(self->sw_in_mute, "notify::active", G_CALLBACK(on_audio_in_mute_changed), self);
    gtk_box_append(GTK_BOX(self->card_in),
                   create_boxed_row("🔇", "Mute Microphone Input",
                                    "Hardware privacy indicator synced on panel",
                                    self->sw_in_mute));

    settings_bento_add_pair(GTK_WIDGET(self), self->card_out, self->card_in);

    /* ─── Card 3: Active Application Playback Streams ─── */
    self->card_streams = create_section_card(
        SETTINGS_CARD_RIGHT, "3. ACTIVE APPLICATION PLAYBACK STREAMS",
        "0 ACTIVE", NULL, NULL);
    gtk_widget_set_vexpand(self->card_streams, TRUE);

    self->list_streams = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(self->list_streams),
                   create_empty_state_label("No active application playback streams."));
    gtk_box_append(GTK_BOX(self->card_streams), self->list_streams);

    settings_bento_add_full(GTK_WIDGET(self), self->card_streams);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "🔊 TEST SOUND",
        on_audio_test_sound,
        NULL,
        NULL,
        self);
    settings_page_set_live_status(GTK_WIDGET(self), "✓ PIPEWIRE LIVE",
                                  "Multi-channel routing & volume apply immediately");

    shell_pa_conn_open(&self->pa, "kajo-settings",
                       audio_context_state_cb, audio_context_drop_cb, self);

    return GTK_WIDGET(self);
}
