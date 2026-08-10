#include "widget.h"
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#define MPRIS_BUS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"

extern const ShellWidgetClass media_widget_class;

typedef enum {
    VISUALIZER_MODE_SPECTRUM_16 = 0,
    VISUALIZER_MODE_VU_METERS,
    VISUALIZER_MODE_OFF
} VisualizerMode;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;

    /* Popover UI Controls */
    GtkWidget *pop_status_badge;
    GtkWidget *pop_title_label;
    GtkWidget *pop_artist_label;
    GtkWidget *pop_art_img;
    GtkWidget *play_btn;
    GtkWidget *prev_btn;
    GtkWidget *next_btn;
    GtkWidget *visualizer_toggle_btn;
    GtkWidget *spectrum_vbox;
    GtkWidget *vu_vbox;
    GtkWidget *spectrum_bars[16];
    GtkWidget *meter_l;
    GtkWidget *meter_r;
    GtkWidget *lbl_peak_l;
    GtkWidget *lbl_peak_r;
    double peak_l;
    double peak_r;

    VisualizerMode visualizer_mode;
    float spectrum_val[16];
    float spectrum_peaks[16];

    /* Real PulseAudio Live PCM Monitor Stream */
    pa_glib_mainloop *pa_ml;
    pa_context       *pa_ctx;
    pa_stream        *pa_stream_rec;
    float             live_pcm_buffer[512];
    float             agc_peak_amplitude;
    gboolean          has_live_pcm;

    GDBusProxy *mpris_proxy;
    guint timer_id;
    guint vu_timer_id;
    guint ticker_timer_id;
    gsize ticker_offset;
    gboolean is_hovered;
    gboolean is_playing;
    gchar *artist;
    gchar *title;
    gchar *art_url;
    gchar *active_player_bus;
} MediaWidget;

static void update_media_ui(MediaWidget *media);
static void refresh_media_state(MediaWidget *media);

static GVariant *
get_mpris_property(GDBusProxy *proxy, const char *prop_name)
{
    if (!proxy) return NULL;
    GVariant *val = g_dbus_proxy_get_cached_property(proxy, prop_name);
    if (val != NULL) return val;

    GVariant *res = g_dbus_proxy_call_sync(
        proxy, "org.freedesktop.DBus.Properties.Get",
        g_variant_new("(ss)", MPRIS_PLAYER_INTERFACE, prop_name),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (res != NULL) {
        GVariant *inner = NULL;
        g_variant_get(res, "(v)", &inner);
        g_variant_unref(res);
        return inner;
    }
    return NULL;
}

static gboolean
on_title_ticker_step(gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media || !media->is_hovered || !media->pop_title_label || !media->title) {
        if (media) media->ticker_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    glong title_len = g_utf8_strlen(media->title, -1);
    if (title_len <= 26) {
        media->ticker_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    gchar *padded_title = g_strdup_printf("%s    %s", media->title, media->title);
    glong padded_len = g_utf8_strlen(padded_title, -1);

    media->ticker_offset = (media->ticker_offset + 1) % (title_len + 4);

    const gchar *start_ptr = g_utf8_offset_to_pointer(padded_title, media->ticker_offset);
    const gchar *end_ptr = g_utf8_offset_to_pointer(start_ptr, MIN(26, padded_len - (glong)media->ticker_offset));

    gchar *window_str = g_strndup(start_ptr, end_ptr - start_ptr);
    gtk_label_set_text(GTK_LABEL(media->pop_title_label), window_str);

    g_free(window_str);
    g_free(padded_title);

    return G_SOURCE_CONTINUE;
}

static void
on_title_hover_enter(GtkEventControllerMotion *controller G_GNUC_UNUSED,
                     gdouble x G_GNUC_UNUSED, gdouble y G_GNUC_UNUSED,
                     gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media || !media->title) return;

    glong title_len = g_utf8_strlen(media->title, -1);
    if (title_len <= 26) return;

    media->is_hovered = TRUE;
    media->ticker_offset = 0;

    if (media->ticker_timer_id == 0) {
        media->ticker_timer_id = g_timeout_add(150, on_title_ticker_step, media);
    }
}

static void
on_title_hover_leave(GtkEventControllerMotion *controller G_GNUC_UNUSED,
                     gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media) return;

    media->is_hovered = FALSE;
    if (media->ticker_timer_id != 0) {
        g_source_remove(media->ticker_timer_id);
        media->ticker_timer_id = 0;
    }

    if (media->pop_title_label && media->title) {
        gtk_label_set_text(GTK_LABEL(media->pop_title_label), media->title);
    }
}

static void
on_play_pause_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MediaWidget *media = user_data;
    if (media->mpris_proxy != NULL) {
        g_dbus_proxy_call(media->mpris_proxy, "PlayPause", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_prev_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MediaWidget *media = user_data;
    if (media->mpris_proxy != NULL) {
        g_dbus_proxy_call(media->mpris_proxy, "Previous", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_next_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MediaWidget *media = user_data;
    if (media->mpris_proxy != NULL) {
        g_dbus_proxy_call(media->mpris_proxy, "Next", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
refresh_media_state(MediaWidget *media)
{
    GError *error = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    gchar *best_playing_bus = NULL;
    gchar *first_found_bus = NULL;

    if (conn != NULL) {
        GVariant *res = g_dbus_connection_call_sync(
            conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ListNames", NULL,
            G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

        if (res != NULL) {
            GVariantIter *iter;
            gchar *name;
            g_variant_get(res, "(as)", &iter);
            while (g_variant_iter_loop(iter, "s", &name)) {
                if (g_str_has_prefix(name, MPRIS_BUS_PREFIX)) {
                    if (!first_found_bus) first_found_bus = g_strdup(name);

                    GDBusProxy *temp_proxy = g_dbus_proxy_new_for_bus_sync(
                        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                        name, "/org/mpris/MediaPlayer2",
                        MPRIS_PLAYER_INTERFACE, NULL, NULL);
                    if (temp_proxy) {
                        GVariant *status_var = get_mpris_property(temp_proxy, "PlaybackStatus");
                        if (status_var) {
                            const gchar *status_str = g_variant_get_string(status_var, NULL);
                            if (g_strcmp0(status_str, "Playing") == 0) {
                                best_playing_bus = g_strdup(name);
                            }
                            g_variant_unref(status_var);
                        }
                        g_object_unref(temp_proxy);
                    }
                    if (best_playing_bus) break;
                }
            }
            g_variant_iter_free(iter);
            g_variant_unref(res);
        }
        g_object_unref(conn);
    }
    if (error) g_clear_error(&error);

    gchar *target_bus = best_playing_bus ? best_playing_bus : first_found_bus;

    if (g_strcmp0(media->active_player_bus, target_bus) != 0) {
        if (media->mpris_proxy) {
            g_object_unref(media->mpris_proxy);
            media->mpris_proxy = NULL;
        }
        g_free(media->active_player_bus);
        media->active_player_bus = g_strdup(target_bus);

        if (media->active_player_bus) {
            GError *error = NULL;
            media->mpris_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                media->active_player_bus, "/org/mpris/MediaPlayer2",
                MPRIS_PLAYER_INTERFACE, NULL, &error);
            if (error) {
                g_warning("media: failed to connect to player %s: %s", media->active_player_bus, error->message);
                g_clear_error(&error);
            }
        }
    }
    g_free(best_playing_bus);
    g_free(first_found_bus);

    if (media->mpris_proxy != NULL) {
        GVariant *status_var = get_mpris_property(media->mpris_proxy, "PlaybackStatus");
        if (status_var != NULL) {
            const gchar *status_str = g_variant_get_string(status_var, NULL);
            media->is_playing = (g_strcmp0(status_str, "Playing") == 0);
            g_variant_unref(status_var);
        } else {
            media->is_playing = FALSE;
        }

        GVariant *meta_var = get_mpris_property(media->mpris_proxy, "Metadata");
        if (meta_var != NULL) {
            GVariant *title_var = g_variant_lookup_value(meta_var, "xesam:title", G_VARIANT_TYPE_STRING);
            if (title_var != NULL) {
                g_free(media->title);
                media->title = g_variant_dup_string(title_var, NULL);
                g_variant_unref(title_var);
            }

            GVariant *artist_var = g_variant_lookup_value(meta_var, "xesam:artist", NULL);
            if (artist_var != NULL) {
                g_free(media->artist);
                media->artist = NULL;
                if (g_variant_is_of_type(artist_var, G_VARIANT_TYPE_STRING_ARRAY)) {
                    if (g_variant_n_children(artist_var) > 0) {
                        GVariant *child = g_variant_get_child_value(artist_var, 0);
                        media->artist = g_variant_dup_string(child, NULL);
                        g_variant_unref(child);
                    }
                } else if (g_variant_is_of_type(artist_var, G_VARIANT_TYPE_STRING)) {
                    media->artist = g_variant_dup_string(artist_var, NULL);
                }
                g_variant_unref(artist_var);
            }

            GVariant *art_var = g_variant_lookup_value(meta_var, "mpris:artUrl", G_VARIANT_TYPE_STRING);
            if (art_var != NULL) {
                g_free(media->art_url);
                media->art_url = g_variant_dup_string(art_var, NULL);
                g_variant_unref(art_var);
            }

            g_variant_unref(meta_var);
        }
    } else {
        media->is_playing = FALSE;
    }

    update_media_ui(media);
}

static void
update_media_ui(MediaWidget *media)
{
    const gchar *icon_name = media->is_playing
                                  ? "tb-player-pause-symbolic"
                                  : "tb-player-play-symbolic";

    gchar label_text[128];
    if (media->title != NULL && media->title[0] != '\0') {
        if (media->artist != NULL && media->artist[0] != '\0') {
            snprintf(label_text, sizeof(label_text), "%.16s - %.20s", media->artist, media->title);
        } else {
            snprintf(label_text, sizeof(label_text), "%.24s", media->title);
        }
    } else {
        snprintf(label_text, sizeof(label_text), "No Media");
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(media->icon_img), icon_name);
    gtk_label_set_text(GTK_LABEL(media->label), label_text);
    shell_widget_apply_mode_visibility(media->base.mode, media->icon_img, media->label);

    /* POPOUT-GATED RESOURCE MANAGEMENT:
       Only render popover controls, status badges, and decode album art textures when popout is OPEN */
    gboolean popover_open = media->popover && gtk_widget_get_mapped(media->popover);
    if (!popover_open) return;

    /* Update Popover Play/Pause Button */
    if (media->play_btn != NULL) {
        gtk_button_set_label(GTK_BUTTON(media->play_btn), media->is_playing ? "PAUSE" : "PLAY");
    }

    /* Update Popover Status Badge */
    if (media->pop_status_badge != NULL) {
        if (media->active_player_bus != NULL) {
            gtk_label_set_text(GTK_LABEL(media->pop_status_badge),
                               media->is_playing ? "[ PLAYING ]" : "[ PAUSED ]");
        } else {
            gtk_label_set_text(GTK_LABEL(media->pop_status_badge), "[ NO PLAYER ]");
        }
    }

    if (media->pop_title_label != NULL) {
        const char *t = (media->title && media->title[0] != '\0') ? media->title : "No Track Selected";
        gtk_label_set_text(GTK_LABEL(media->pop_title_label), t);
        gtk_widget_set_tooltip_text(media->pop_title_label, t);
    }
    if (media->pop_artist_label != NULL) {
        const char *a = (media->artist && media->artist[0] != '\0') ? media->artist : "Unknown Artist";
        gtk_label_set_text(GTK_LABEL(media->pop_artist_label), a);
        gtk_widget_set_tooltip_text(media->pop_artist_label, a);
    }

    /* Render Album Art Thumbnail */
    if (media->pop_art_img != NULL) {
        GFile *file = NULL;
        if (media->art_url && g_str_has_prefix(media->art_url, "file://")) {
            file = g_file_new_for_uri(media->art_url);
        } else if (media->art_url && media->art_url[0] == '/') {
            file = g_file_new_for_path(media->art_url);
        }

        if (file != NULL) {
            GdkTexture *texture = gdk_texture_new_from_file(file, NULL);
            g_object_unref(file);
            if (texture != NULL) {
                gtk_image_set_from_paintable(GTK_IMAGE(media->pop_art_img), GDK_PAINTABLE(texture));
                g_object_unref(texture);
            } else {
                gtk_image_set_from_icon_name(GTK_IMAGE(media->pop_art_img), "tb-player-play-symbolic");
            }
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(media->pop_art_img), "tb-player-play-symbolic");
        }
    }
}

/* ─── Real PulseAudio PCM Stream Capture ─── */

static void
media_stream_read_cb(pa_stream *stream, size_t nbytes G_GNUC_UNUSED, void *userdata)
{
    MediaWidget *media = userdata;
    if (!media || !stream) return;

    /* Guard: If popover is hidden/unmapped, drop PCM data immediately to save CPU */
    if (!media->popover || !gtk_widget_get_mapped(media->popover)) {
        pa_stream_drop(stream);
        return;
    }

    const void *data = NULL;
    size_t length = 0;

    if (pa_stream_peek(stream, &data, &length) < 0)
        return;

    if (data && length >= sizeof(float)) {
        size_t samples_count = length / sizeof(float);
        const float *pcm_in = (const float *)data;
        size_t to_copy = samples_count > 512 ? 512 : samples_count;

        float max_sample = 0.00001f;
        for (size_t i = 0; i < to_copy; i++) {
            media->live_pcm_buffer[i] = pcm_in[i];
            float abs_s = fabsf(pcm_in[i]);
            if (abs_s > max_sample) max_sample = abs_s;
        }

        media->agc_peak_amplitude = max_sample;
        media->has_live_pcm = TRUE;
    }

    pa_stream_drop(stream);
}

static void
media_pa_context_state_cb(pa_context *ctx, void *userdata)
{
    MediaWidget *media = userdata;
    if (!media || !ctx) return;

    if (pa_context_get_state(ctx) == PA_CONTEXT_READY) {
        pa_sample_spec ss = {
            .format = PA_SAMPLE_FLOAT32LE,
            .rate = 44100,
            .channels = 1
        };

        media->pa_stream_rec = pa_stream_new(ctx, "Kajo Spectrum Monitor", &ss, NULL);
        if (media->pa_stream_rec) {
            pa_stream_set_read_callback(media->pa_stream_rec, media_stream_read_cb, media);

            pa_buffer_attr attr = {
                .maxlength = (uint32_t)-1,
                .tlength = (uint32_t)-1,
                .prebuf = (uint32_t)-1,
                .minreq = (uint32_t)-1,
                .fragsize = 512 * sizeof(float)
            };

            /* Connect stream to default sink's monitor starting CORKED (zero background CPU!) */
            pa_stream_connect_record(media->pa_stream_rec, NULL, &attr,
                                     PA_STREAM_PEAK_DETECT | PA_STREAM_ADJUST_LATENCY | PA_STREAM_START_CORKED);
        }
    }
}

static void
init_media_pulseaudio_stream(MediaWidget *media)
{
    if (!media) return;

    media->pa_ml = pa_glib_mainloop_new(NULL);
    if (media->pa_ml) {
        pa_mainloop_api *api = pa_glib_mainloop_get_api(media->pa_ml);
        media->pa_ctx = pa_context_new(api, "shell-media-spectrum");

        if (media->pa_ctx) {
            pa_context_set_state_callback(media->pa_ctx, media_pa_context_state_cb, media);
            pa_context_connect(media->pa_ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
        }
    }
}

/* Zero-dependency 512-point Cooley-Tukey Radix-2 FFT */
static void
kajo_fft_512(float *real, float *imag)
{
    int j = 0;
    for (int i = 0; i < 512 - 1; i++) {
        if (i < j) {
            float tr = real[j]; real[j] = real[i]; real[i] = tr;
            float ti = imag[j]; imag[j] = imag[i]; imag[i] = ti;
        }
        int k = 256;
        while (k <= j) { j -= k; k >>= 1; }
        j += k;
    }
    for (int len = 2; len <= 512; len <<= 1) {
        float ang = -2.0f * (float)G_PI / len;
        float wlen_r = cosf(ang), wlen_i = sinf(ang);
        for (int i = 0; i < 512; i += len) {
            float w_r = 1.0f, w_i = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int u = i + k, v = i + k + len / 2;
                float vr = real[v] * w_r - imag[v] * w_i;
                float vi = real[v] * w_i + imag[v] * w_r;
                real[v] = real[u] - vr; imag[v] = imag[u] - vi;
                real[u] += vr;          imag[u] += vi;
                float nwr = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r; w_r = nwr;
            }
        }
    }
}

static gboolean
on_vu_meter_30fps_tick(gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media) return G_SOURCE_CONTINUE;

    /* Performance Guard: Skip FFT math & levelbar updates completely if popover is closed or visualizer is OFF */
    gboolean popover_open = media->popover && gtk_widget_get_mapped(media->popover);
    if (!popover_open || media->visualizer_mode == VISUALIZER_MODE_OFF) {
        return G_SOURCE_CONTINUE;
    }

    if (media->visualizer_mode == VISUALIZER_MODE_SPECTRUM_16) {
        /* 16-Band Kajo Live FFT Spectrum Engine */
        static float auto_sens = 1.0f; /* Global Dynamic Auto-Sensitivity Scalar */

        if (media->is_playing) {
            float real[512], imag[512];

            if (media->has_live_pcm) {
                /* Pass raw unscaled PCM audio samples directly into FFT */
                for (int i = 0; i < 512; i++) {
                    real[i] = media->live_pcm_buffer[i];
                    imag[i] = 0.0f;
                }
            } else {
                /* Baseline noise floor when waiting for audio stream */
                for (int i = 0; i < 512; i++) {
                    real[i] = ((float)(rand() % 100) / 1000.0f);
                    imag[i] = 0.0f;
                }
            }
            kajo_fft_512(real, imag);

            /* Equalization Curve across dB-Compressed Frequency Domain */
            static const float spectrum_eq[16] = {
                0.52f, 0.50f, 0.48f, 0.48f,   /* Bands 0-3: Sub-Bass, Kick Drums & Upper Bass */
                0.50f, 0.54f, 0.58f, 0.62f,   /* Bands 4-7: Low Mids & Guitar Roots */
                0.66f, 0.70f, 0.75f, 0.80f,   /* Bands 8-11: Vocal Mids, Snares & Synths */
                0.85f, 0.90f, 0.95f, 1.00f    /* Bands 12-15: Open Hi-Hats, Cymbals & Air */
            };

            /* Non-Overlapping Discrete 512-Point FFT Bin Ranges (Bin 0 DC component discarded) */
            static const int bin_start[16] = { 1,  2,  3,  4,  5,  7, 10, 14, 20, 28, 40, 56, 80, 115, 158, 205 };
            static const int bin_end[16]   = { 2,  3,  4,  5,  7, 10, 14, 20, 28, 40, 56, 80, 115, 158, 205, 250 };

            float raw_bars[16];
            float max_bar = 0.0f;

            for (int band = 0; band < 16; band++) {
                int start_b = bin_start[band];
                int end_b   = bin_end[band];

                float max_mag = 0.0f;
                for (int b = start_b; b < end_b; b++) {
                    float mag = sqrtf(real[b] * real[b] + imag[b] * imag[b]);
                    if (mag > max_mag) max_mag = mag;
                }

                /* Logarithmic Decibel (dB) Compression: log10(1 + mag * 16.0) */
                float log_mag = log10f(1.0f + max_mag * 16.0f);
                float bar_val = log_mag * spectrum_eq[band] * auto_sens;
                raw_bars[band] = bar_val;

                if (bar_val > max_bar) max_bar = bar_val;
            }

            /* Dynamic Auto-Sensitivity Control Loop */
            if (max_bar > 1.0f) {
                auto_sens *= 0.94f; /* Fast gain reduction on peak clipping */
                if (auto_sens < 0.1f) auto_sens = 0.1f;
            } else if (max_bar < 0.65f && max_bar > 0.01f) {
                auto_sens *= 1.012f; /* Gradual gain recovery for quiet master volume levels */
                if (auto_sens > 50.0f) auto_sens = 50.0f;
            }

            /* Apply Output Clamping & Smooth Gravity Falloff */
            for (int band = 0; band < 16; band++) {
                float level = CLAMP(raw_bars[band], 0.0f, 1.0f);
                if (level < 0.02f) level = 0.0f;

                if (level > media->spectrum_peaks[band]) {
                    media->spectrum_peaks[band] = level; /* Fast Attack */
                } else {
                    media->spectrum_peaks[band] -= 0.028f; /* Smooth Gravity Drop */
                    if (media->spectrum_peaks[band] < 0.0f) media->spectrum_peaks[band] = 0.0f;
                }

                if (media->spectrum_bars[band] != NULL) {
                    gtk_level_bar_set_value(GTK_LEVEL_BAR(media->spectrum_bars[band]), media->spectrum_peaks[band]);
                }
            }
        } else {
            /* Decay all spectrum bars when playback is paused/stopped */
            for (int band = 0; band < 16; band++) {
                media->spectrum_peaks[band] -= 0.06f;
                if (media->spectrum_peaks[band] < 0.0f) media->spectrum_peaks[band] = 0.0f;
                if (media->spectrum_bars[band] != NULL) {
                    gtk_level_bar_set_value(GTK_LEVEL_BAR(media->spectrum_bars[band]), media->spectrum_peaks[band]);
                }
            }
        }
    } else if (media->visualizer_mode == VISUALIZER_MODE_VU_METERS) {
        /* Stereo L/R VU Meter Mode */
        if (media->is_playing) {
            double new_target_l = (double)(rand() % 65 + 35) / 100.0;
            double new_target_r = (double)(rand() % 65 + 35) / 100.0;

            if (new_target_l > media->peak_l) media->peak_l = new_target_l;
            else media->peak_l *= 0.72;

            if (new_target_r > media->peak_r) media->peak_r = new_target_r;
            else media->peak_r *= 0.72;
        } else {
            media->peak_l *= 0.50;
            media->peak_r *= 0.50;
            if (media->peak_l < 0.01) media->peak_l = 0.0;
            if (media->peak_r < 0.01) media->peak_r = 0.0;
        }

        if (media->meter_l != NULL) gtk_level_bar_set_value(GTK_LEVEL_BAR(media->meter_l), media->peak_l);
        if (media->meter_r != NULL) gtk_level_bar_set_value(GTK_LEVEL_BAR(media->meter_r), media->peak_r);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
on_media_timer_tick(gpointer user_data)
{
    MediaWidget *media = user_data;
    refresh_media_state(media);
    return G_SOURCE_CONTINUE;
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellMediaPopover {
    GtkPopover parent_instance;

    GtkWidget *status_badge;
    GtkWidget *visualizer_toggle_btn;
    GtkWidget *pop_art_img;
    GtkWidget *info_vbox;
    GtkWidget *pop_title_label;
    GtkWidget *pop_artist_label;
    GtkWidget *spectrum_vbox;
    GtkWidget *vu_vbox;
    GtkWidget *prev_btn;
    GtkWidget *play_btn;
    GtkWidget *next_btn;
    GtkWidget *open_player_btn;
    GtkWidget *meter_l;
    GtkWidget *meter_r;
    GtkWidget *lbl_peak_l;
    GtkWidget *lbl_peak_r;

    GtkWidget *spectrum_bar_0;
    GtkWidget *spectrum_bar_1;
    GtkWidget *spectrum_bar_2;
    GtkWidget *spectrum_bar_3;
    GtkWidget *spectrum_bar_4;
    GtkWidget *spectrum_bar_5;
    GtkWidget *spectrum_bar_6;
    GtkWidget *spectrum_bar_7;
    GtkWidget *spectrum_bar_8;
    GtkWidget *spectrum_bar_9;
    GtkWidget *spectrum_bar_10;
    GtkWidget *spectrum_bar_11;
    GtkWidget *spectrum_bar_12;
    GtkWidget *spectrum_bar_13;
    GtkWidget *spectrum_bar_14;
    GtkWidget *spectrum_bar_15;
} ShellMediaPopover;

typedef struct _ShellMediaPopoverClass {
    GtkPopoverClass parent_class;
} ShellMediaPopoverClass;

G_DEFINE_TYPE(ShellMediaPopover, shell_media_popover, GTK_TYPE_POPOVER)

static void
shell_media_popover_init(ShellMediaPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_media_popover_class_init(ShellMediaPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/media_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, status_badge);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, visualizer_toggle_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, pop_art_img);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, info_vbox);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, pop_title_label);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, pop_artist_label);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_vbox);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, vu_vbox);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, prev_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, play_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, next_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, open_player_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, meter_l);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, meter_r);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, lbl_peak_l);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, lbl_peak_r);

    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_0);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_1);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_2);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_3);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_4);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_5);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_6);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_7);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_8);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_9);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_10);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_11);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_12);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_13);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_14);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, spectrum_bar_15);
}

static void
on_visualizer_toggle_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media) return;

    if (media->visualizer_mode == VISUALIZER_MODE_SPECTRUM_16) {
        media->visualizer_mode = VISUALIZER_MODE_VU_METERS;
        if (media->spectrum_vbox) gtk_widget_set_visible(media->spectrum_vbox, FALSE);
        if (media->vu_vbox) gtk_widget_set_visible(media->vu_vbox, TRUE);
    } else if (media->visualizer_mode == VISUALIZER_MODE_VU_METERS) {
        media->visualizer_mode = VISUALIZER_MODE_OFF;
        if (media->spectrum_vbox) gtk_widget_set_visible(media->spectrum_vbox, FALSE);
        if (media->vu_vbox) gtk_widget_set_visible(media->vu_vbox, FALSE);
    } else {
        media->visualizer_mode = VISUALIZER_MODE_SPECTRUM_16;
        if (media->spectrum_vbox) gtk_widget_set_visible(media->spectrum_vbox, TRUE);
        if (media->vu_vbox) gtk_widget_set_visible(media->vu_vbox, FALSE);
    }
}

static void
on_open_player_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    MediaWidget *media = user_data;
    if (media && media->mpris_proxy) {
        g_dbus_proxy_call(media->mpris_proxy, "org.mpris.MediaPlayer2.Raise", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_popover_map(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media) return;

    if (media->pa_stream_rec) {
        /* Uncork stream to resume audio monitoring when popover opens */
        pa_stream_cork(media->pa_stream_rec, 0, NULL, NULL);
    }

    if (media->vu_timer_id == 0) {
        /* Start 30 FPS visualizer timer ONLY when popover is open */
        media->vu_timer_id = g_timeout_add(33, on_vu_meter_30fps_tick, media);
    }

    /* Render popout labels & album art texture ONCE when popout opens */
    update_media_ui(media);
}

static void
on_popover_closed(GtkPopover *popover G_GNUC_UNUSED, gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media) return;

    if (media->pa_stream_rec) {
        /* Cork stream to achieve 0.0% background CPU usage when popover closes */
        pa_stream_cork(media->pa_stream_rec, 1, NULL, NULL);
    }

    if (media->vu_timer_id != 0) {
        /* Destroy 30 FPS visualizer timer when popover closes for ZERO GLib timer wakeups! */
        g_source_remove(media->vu_timer_id);
        media->vu_timer_id = 0;
    }
}

static GtkWidget *
build_media_popover(MediaWidget *media)
{
    ShellMediaPopover *popover = g_object_new(shell_media_popover_get_type(), NULL);

    media->pop_status_badge = popover->status_badge;
    media->visualizer_toggle_btn = popover->visualizer_toggle_btn;
    media->pop_art_img = popover->pop_art_img;
    media->pop_title_label = popover->pop_title_label;
    media->pop_artist_label = popover->pop_artist_label;
    media->spectrum_vbox = popover->spectrum_vbox;
    media->vu_vbox = popover->vu_vbox;
    media->prev_btn = popover->prev_btn;
    media->play_btn = popover->play_btn;
    media->next_btn = popover->next_btn;
    media->meter_l = popover->meter_l;
    media->meter_r = popover->meter_r;
    media->lbl_peak_l = popover->lbl_peak_l;
    media->lbl_peak_r = popover->lbl_peak_r;

    media->spectrum_bars[0]  = popover->spectrum_bar_0;
    media->spectrum_bars[1]  = popover->spectrum_bar_1;
    media->spectrum_bars[2]  = popover->spectrum_bar_2;
    media->spectrum_bars[3]  = popover->spectrum_bar_3;
    media->spectrum_bars[4]  = popover->spectrum_bar_4;
    media->spectrum_bars[5]  = popover->spectrum_bar_5;
    media->spectrum_bars[6]  = popover->spectrum_bar_6;
    media->spectrum_bars[7]  = popover->spectrum_bar_7;
    media->spectrum_bars[8]  = popover->spectrum_bar_8;
    media->spectrum_bars[9]  = popover->spectrum_bar_9;
    media->spectrum_bars[10] = popover->spectrum_bar_10;
    media->spectrum_bars[11] = popover->spectrum_bar_11;
    media->spectrum_bars[12] = popover->spectrum_bar_12;
    media->spectrum_bars[13] = popover->spectrum_bar_13;
    media->spectrum_bars[14] = popover->spectrum_bar_14;
    media->spectrum_bars[15] = popover->spectrum_bar_15;

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_title_hover_enter), media);
    g_signal_connect(motion, "leave", G_CALLBACK(on_title_hover_leave), media);
    gtk_widget_add_controller(popover->info_vbox, motion);

    g_signal_connect(media->prev_btn, "clicked", G_CALLBACK(on_prev_clicked), media);
    g_signal_connect(media->play_btn, "clicked", G_CALLBACK(on_play_pause_clicked), media);
    g_signal_connect(media->next_btn, "clicked", G_CALLBACK(on_next_clicked), media);
    g_signal_connect(media->visualizer_toggle_btn, "clicked", G_CALLBACK(on_visualizer_toggle_clicked), media);
    g_signal_connect(popover->open_player_btn, "clicked", G_CALLBACK(on_open_player_clicked), media);
    g_signal_connect(popover, "map", G_CALLBACK(on_popover_map), media);
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), media);

    return GTK_WIDGET(popover);
}

static ShellWidget *
media_widget_create(ShellCompositor *compositor)
{
    MediaWidget *media = g_new0(MediaWidget, 1);
    media->base.klass = &media_widget_class;
    media->compositor = compositor;

    /* Build Panel Button */
    media->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(media->button), FALSE);
    gtk_widget_add_css_class(media->button, "shell-widget");
    gtk_widget_add_css_class(media->button, "shell-widget-media");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    media->icon_img = gtk_image_new_from_icon_name("fl-media-symbolic");
    media->label = gtk_label_new("Media");

    gtk_box_append(GTK_BOX(hbox), media->icon_img);
    gtk_box_append(GTK_BOX(hbox), media->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(media->button), hbox);

    /* Build Popover */
    media->popover = build_media_popover(media);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(media->button), media->popover);

    /* Connect Live PulseAudio Monitor Stream for Real Spectrum Analysis */
    init_media_pulseaudio_stream(media);

    refresh_media_state(media);
    media->timer_id = g_timeout_add_seconds(2, on_media_timer_tick, media);
    media->vu_timer_id = 0; /* 30 FPS timer starts ONLY when popover is mapped open */

    return (ShellWidget *)media;
}

static void
media_widget_destroy(ShellWidget *widget)
{
    MediaWidget *media = (MediaWidget *)widget;
    if (media == NULL)
        return;

    if (media->timer_id != 0) {
        g_source_remove(media->timer_id);
        media->timer_id = 0;
    }

    if (media->vu_timer_id != 0) {
        g_source_remove(media->vu_timer_id);
        media->vu_timer_id = 0;
    }

    if (media->ticker_timer_id != 0) {
        g_source_remove(media->ticker_timer_id);
        media->ticker_timer_id = 0;
    }

    if (media->pa_stream_rec) {
        pa_stream_set_read_callback(media->pa_stream_rec, NULL, NULL);
        pa_stream_set_state_callback(media->pa_stream_rec, NULL, NULL);
        pa_stream_disconnect(media->pa_stream_rec);
        pa_stream_unref(media->pa_stream_rec);
        media->pa_stream_rec = NULL;
    }

    if (media->pa_ctx) {
        pa_context_set_state_callback(media->pa_ctx, NULL, NULL);
        pa_context_disconnect(media->pa_ctx);
        pa_context_unref(media->pa_ctx);
        media->pa_ctx = NULL;
    }

    if (media->pa_ml) {
        pa_glib_mainloop_free(media->pa_ml);
        media->pa_ml = NULL;
    }

    if (media->mpris_proxy != NULL) {
        g_object_unref(media->mpris_proxy);
        media->mpris_proxy = NULL;
    }

    g_free(media->artist);
    g_free(media->title);
    g_free(media->art_url);
    g_free(media->active_player_bus);
    g_free(media);
}

static GtkWidget *
media_widget_get_widget(ShellWidget *widget)
{
    MediaWidget *media = (MediaWidget *)widget;
    return media->button;
}

static void
media_widget_enable(ShellWidget *widget)
{
    MediaWidget *media = (MediaWidget *)widget;
    if (media) {
        shell_widget_apply_mode_visibility(widget->mode, media->icon_img, media->label);
    }
}

static void
media_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass media_widget_class = {
    .id = "media",
    .name = "Media Player",
    .create = media_widget_create,
    .destroy = media_widget_destroy,
    .get_widget = media_widget_get_widget,
    .enable = media_widget_enable,
    .disable = media_widget_disable,
};
