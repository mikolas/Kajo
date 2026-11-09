#include "mod_audio.h"
#include <pulse/pulseaudio.h>
#include <math.h>
#include <string.h>

#define FFT_SIZE 512

struct _KajoModAudio {
    GMutex               lock;
    GCond                cond;
    GThread             *thread;
    gboolean             quit;

    KajoModEngine       *eng;
    gboolean             playing;
    gboolean             paused;
    gboolean             vis_mapped;
    double               volume;

    KajoModAudioSnapshot snap;
    float                hann[FFT_SIZE];
};

static void
fft_radix2_512(float re[FFT_SIZE], float im[FFT_SIZE])
{
    int j = 0;
    for (int i = 0; i < FFT_SIZE; i++) {
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
        int m = FFT_SIZE >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    for (int len = 2; len <= FFT_SIZE; len <<= 1) {
        int half = len >> 1;
        double ang = -2.0 * G_PI / (double)len;
        float wlen_r = (float)cos(ang);
        float wlen_i = (float)sin(ang);
        for (int i = 0; i < FFT_SIZE; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int k = 0; k < half; k++) {
                int u = i + k;
                int v = i + k + half;
                float tr = wr * re[v] - wi * im[v];
                float ti = wr * im[v] + wi * re[v];
                re[v] = re[u] - tr;
                im[v] = im[u] - ti;
                re[u] += tr;
                im[u] += ti;
                float nwr = wr * wlen_r - wi * wlen_i;
                wi = wr * wlen_i + wi * wlen_r;
                wr = nwr;
            }
        }
    }
}

static void
update_scope_and_fft(KajoModAudio  *audio,
                     const int16_t *stereo_frames,
                     int            n_frames)
{
    if (!audio->vis_mapped || n_frames <= 0) return;

    int scope_n = MIN(n_frames, KAJO_MOD_SCOPE_SAMPLES);
    for (int i = 0; i < scope_n; i++) {
        audio->snap.scope_l[i] = (float)stereo_frames[i * 2] / 32768.0f;
        audio->snap.scope_r[i] = (float)stereo_frames[i * 2 + 1] / 32768.0f;
    }
    for (int i = scope_n; i < KAJO_MOD_SCOPE_SAMPLES; i++) {
        audio->snap.scope_l[i] = 0.0f;
        audio->snap.scope_r[i] = 0.0f;
    }

    float re[FFT_SIZE];
    float im[FFT_SIZE];
    for (int i = 0; i < FFT_SIZE; i++) {
        if (i < n_frames) {
            float mono = ((float)stereo_frames[i * 2] + (float)stereo_frames[i * 2 + 1]) / 65536.0f;
            re[i] = mono * audio->hann[i];
        } else {
            re[i] = 0.0f;
        }
        im[i] = 0.0f;
    }

    fft_radix2_512(re, im);

    /* 16 logarithmic bin ranges across 0..255 (93.75 Hz per bin at 48 kHz) */
    static const int k_bin_edges[17] = {
        1, 2, 3, 4, 6, 8, 11, 15, 21, 29, 40, 55, 76, 105, 145, 195, 255
    };

    for (int b = 0; b < KAJO_MOD_FFT_BANDS; b++) {
        int b0 = k_bin_edges[b];
        int b1 = MAX(b0, k_bin_edges[b + 1]);
        float sum = 0.0f;
        for (int k = b0; k <= b1 && k < (FFT_SIZE / 2); k++) {
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]) / (float)(FFT_SIZE / 4);
            if (mag > sum) sum = mag;
        }
        /* Convert amplitude to dB scale (-56 dB .. 0 dB) */
        float db = 20.0f * log10f(sum + 1e-5f);
        float norm = (db + 52.0f) / 48.0f;
        norm = CLAMP(norm, 0.03f, 1.0f);

        float prev = audio->snap.fft_bands[b];
        float cur  = (norm > prev) ? (prev * 0.35f + norm * 0.65f)
                                   : (prev * 0.72f + norm * 0.28f);
        audio->snap.fft_bands[b] = cur;

        if (cur >= audio->snap.fft_peaks[b]) {
            audio->snap.fft_peaks[b] = cur;
        } else {
            audio->snap.fft_peaks[b] = MAX(cur, audio->snap.fft_peaks[b] - 0.015f);
        }
    }
}

static void
apply_stereo_sep_and_volume(int16_t *stereo_frames,
                            int      n_frames,
                            double   sep_pct,
                            double   vol)
{
    double sep = CLAMP(sep_pct / 100.0, 0.0, 1.0);
    double gain = CLAMP(vol, 0.0, 1.0);
    /* sep = 1.0 -> full stereo; sep = 0.0 -> 50/50 mono; sep = 0.7 -> 85/15 Amiga crossfeed */
    double same_w  = 0.5 * (1.0 + sep) * gain;
    double cross_w = 0.5 * (1.0 - sep) * gain;

    for (int i = 0; i < n_frames; i++) {
        double l = (double)stereo_frames[i * 2];
        double r = (double)stereo_frames[i * 2 + 1];
        int32_t out_l = (int32_t)(l * same_w + r * cross_w);
        int32_t out_r = (int32_t)(r * same_w + l * cross_w);
        stereo_frames[i * 2]     = (int16_t)CLAMP(out_l, -32768, 32767);
        stereo_frames[i * 2 + 1] = (int16_t)CLAMP(out_r, -32768, 32767);
    }
}

static gpointer
audio_worker_thread(gpointer user_data)
{
    KajoModAudio *audio = (KajoModAudio *)user_data;

    pa_mainloop *ml = pa_mainloop_new();
    pa_context  *ctx = NULL;
    pa_stream   *stream = NULL;
    gboolean     pa_ready = FALSE;

    if (ml) {
        pa_mainloop_api *api = pa_mainloop_get_api(ml);
        ctx = pa_context_new(api, "Kajo Mod Player");
        if (ctx && pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) >= 0) {
            for (int tries = 0; tries < 50; tries++) {
                pa_mainloop_iterate(ml, 0, NULL);
                pa_context_state_t st = pa_context_get_state(ctx);
                if (st == PA_CONTEXT_READY) {
                    pa_ready = TRUE;
                    break;
                }
                if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) break;
                g_usleep(4000);
            }
        }
        if (pa_ready) {
            pa_sample_spec ss = {
                .format   = PA_SAMPLE_S16LE,
                .rate     = KAJO_MOD_SAMPLE_RATE,
                .channels = 2
            };
            stream = pa_stream_new(ctx, "Module Synthesis", &ss, NULL);
            if (stream) {
                pa_buffer_attr attr = {
                    .maxlength = (uint32_t)-1,
                    .tlength   = 4800 * 4, /* ~100ms target latency */
                    .prebuf    = (uint32_t)-1,
                    .minreq    = (uint32_t)-1,
                    .fragsize  = (uint32_t)-1
                };
                pa_stream_connect_playback(stream, NULL, &attr,
                                           PA_STREAM_ADJUST_LATENCY, NULL, NULL);
                for (int tries = 0; tries < 50; tries++) {
                    pa_mainloop_iterate(ml, 0, NULL);
                    pa_stream_state_t sst = pa_stream_get_state(stream);
                    if (sst == PA_STREAM_READY) break;
                    if (sst == PA_STREAM_FAILED || sst == PA_STREAM_TERMINATED) {
                        pa_stream_unref(stream);
                        stream = NULL;
                        break;
                    }
                    g_usleep(4000);
                }
            }
        }
    }

    int16_t pcm_buf[512 * 2];

    while (TRUE) {
        g_mutex_lock(&audio->lock);
        while (!audio->quit && (!audio->eng || !audio->playing || audio->paused)) {
            g_cond_wait(&audio->cond, &audio->lock);
        }
        if (audio->quit) {
            g_mutex_unlock(&audio->lock);
            break;
        }

        gboolean should_render = TRUE;
        if (stream) {
            pa_mainloop_iterate(ml, 0, NULL);
            size_t writable = pa_stream_writable_size(stream);
            if (writable < sizeof(pcm_buf)) {
                should_render = FALSE;
            }
        }

        if (should_render) {
            gboolean ended = FALSE;
            int got = kajo_mod_engine_render(audio->eng, pcm_buf, 512, &ended);
            if (got > 0) {
                double eff_vol = audio->volume;
                if (audio->eng->dsp.end_action == KAJO_MOD_END_NEXT_MOD &&
                    audio->eng->meta.duration_sec > 4.0) {
                    double rem = audio->eng->meta.duration_sec - audio->eng->pos_sec;
                    if (rem < 4.0) {
                        eff_vol *= CLAMP(rem / 4.0, 0.0, 1.0);
                    }
                }
                apply_stereo_sep_and_volume(pcm_buf, got,
                                            audio->eng->dsp.stereo_sep_pct,
                                            eff_vol);
                update_scope_and_fft(audio, pcm_buf, got);
            }
            kajo_mod_engine_poll_telemetry(audio->eng, &audio->snap.telemetry);
            audio->snap.pos_sec      = audio->eng->pos_sec;
            audio->snap.duration_sec = audio->eng->meta.duration_sec;
            audio->snap.playing      = audio->playing && !audio->paused;
            audio->snap.paused       = audio->paused;
            if (ended) {
                audio->snap.song_ended = TRUE;
                audio->playing         = FALSE;
            }
            g_mutex_unlock(&audio->lock);

            if (got > 0 && stream) {
                pa_stream_write(stream, pcm_buf, (size_t)got * 4, NULL, 0, PA_SEEK_RELATIVE);
                pa_mainloop_iterate(ml, 0, NULL);
            } else if (!stream) {
                g_usleep(10666); /* 512 frames @ 48 kHz */
            }
        } else {
            g_mutex_unlock(&audio->lock);
            g_usleep(4000);
        }
    }

    if (stream) {
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }
    if (ctx) {
        pa_context_disconnect(ctx);
        pa_context_unref(ctx);
    }
    if (ml) {
        pa_mainloop_free(ml);
    }
    return NULL;
}

KajoModAudio *
kajo_mod_audio_new(void)
{
    KajoModAudio *audio = g_new0(KajoModAudio, 1);
    g_mutex_init(&audio->lock);
    g_cond_init(&audio->cond);
    audio->volume     = 0.85;
    audio->vis_mapped = TRUE;

    for (int i = 0; i < FFT_SIZE; i++) {
        audio->hann[i] = (float)(0.5 * (1.0 - cos(2.0 * G_PI * (double)i / (double)(FFT_SIZE - 1))));
    }

    audio->thread = g_thread_new("kajo-mod-audio", audio_worker_thread, audio);
    return audio;
}

void
kajo_mod_audio_free(KajoModAudio *audio)
{
    if (!audio) return;
    g_mutex_lock(&audio->lock);
    audio->quit = TRUE;
    g_cond_broadcast(&audio->cond);
    g_mutex_unlock(&audio->lock);

    if (audio->thread) {
        g_thread_join(audio->thread);
        audio->thread = NULL;
    }

    if (audio->eng) {
        kajo_mod_engine_free(audio->eng);
        audio->eng = NULL;
    }
    g_mutex_clear(&audio->lock);
    g_cond_clear(&audio->cond);
    g_free(audio);
}

void
kajo_mod_audio_set_engine(KajoModAudio *audio, KajoModEngine *eng, gboolean start_playing)
{
    if (!audio) return;
    g_mutex_lock(&audio->lock);
    if (audio->eng) {
        kajo_mod_engine_free(audio->eng);
    }
    audio->eng     = eng;
    audio->playing = (eng != NULL) && start_playing;
    audio->paused  = FALSE;
    memset(&audio->snap, 0, sizeof(audio->snap));
    if (eng) {
        audio->snap.playing      = audio->playing;
        audio->snap.duration_sec = eng->meta.duration_sec;
        kajo_mod_engine_poll_telemetry(eng, &audio->snap.telemetry);
    }
    g_cond_broadcast(&audio->cond);
    g_mutex_unlock(&audio->lock);
}

void
kajo_mod_audio_stop(KajoModAudio *audio)
{
    if (!audio) return;
    g_mutex_lock(&audio->lock);
    audio->playing = FALSE;
    audio->paused  = FALSE;
    if (audio->eng) {
        kajo_mod_engine_seek(audio->eng, 0.0);
        audio->snap.pos_sec = 0.0;
    }
    audio->snap.playing = FALSE;
    audio->snap.paused  = FALSE;
    g_mutex_unlock(&audio->lock);
}

void
kajo_mod_audio_set_paused(KajoModAudio *audio, gboolean paused)
{
    if (!audio) return;
    g_mutex_lock(&audio->lock);
    audio->paused = paused;
    if (!paused && audio->eng) {
        audio->playing = TRUE;
    }
    audio->snap.paused  = audio->paused;
    audio->snap.playing = audio->playing && !audio->paused;
    g_cond_broadcast(&audio->cond);
    g_mutex_unlock(&audio->lock);
}

gboolean
kajo_mod_audio_get_paused(KajoModAudio *audio)
{
    if (!audio) return FALSE;
    g_mutex_lock(&audio->lock);
    gboolean p = audio->paused;
    g_mutex_unlock(&audio->lock);
    return p;
}

gboolean
kajo_mod_audio_is_active(KajoModAudio *audio)
{
    if (!audio) return FALSE;
    g_mutex_lock(&audio->lock);
    gboolean act = (audio->eng != NULL);
    g_mutex_unlock(&audio->lock);
    return act;
}

void
kajo_mod_audio_seek(KajoModAudio *audio, double target_sec)
{
    if (!audio) return;
    g_mutex_lock(&audio->lock);
    if (audio->eng) {
        kajo_mod_engine_seek(audio->eng, target_sec);
        audio->snap.pos_sec    = audio->eng->pos_sec;
        audio->snap.song_ended = FALSE;
    }
    g_mutex_unlock(&audio->lock);
}

gboolean
kajo_mod_audio_set_subsong(KajoModAudio *audio, int subsong_1based, KajoModMetadata *out_meta)
{
    if (!audio) return FALSE;
    g_mutex_lock(&audio->lock);
    gboolean ok = FALSE;
    if (audio->eng) {
        ok = kajo_mod_engine_set_subsong(audio->eng, subsong_1based);
        if (ok) {
            audio->snap.pos_sec      = 0.0;
            audio->snap.duration_sec = audio->eng->meta.duration_sec;
            audio->snap.song_ended   = FALSE;
            audio->playing           = TRUE;
            audio->paused            = FALSE;
            if (out_meta) {
                kajo_mod_metadata_copy(out_meta, &audio->eng->meta);
            }
            g_cond_broadcast(&audio->cond);
        }
    }
    g_mutex_unlock(&audio->lock);
    return ok;
}

void
kajo_mod_audio_apply_dsp(KajoModAudio *audio, const KajoModDspConfig *dsp)
{
    if (!audio || !dsp) return;
    g_mutex_lock(&audio->lock);
    if (audio->eng) {
        kajo_mod_engine_apply_dsp(audio->eng, dsp);
        kajo_mod_engine_poll_telemetry(audio->eng, &audio->snap.telemetry);
    }
    g_mutex_unlock(&audio->lock);
}

void
kajo_mod_audio_set_volume(KajoModAudio *audio, double volume_0_to_1)
{
    if (!audio) return;
    g_mutex_lock(&audio->lock);
    audio->volume = CLAMP(volume_0_to_1, 0.0, 1.0);
    g_mutex_unlock(&audio->lock);
}

void
kajo_mod_audio_set_vis_mapped(KajoModAudio *audio, gboolean mapped)
{
    if (!audio) return;
    g_mutex_lock(&audio->lock);
    audio->vis_mapped = mapped;
    g_mutex_unlock(&audio->lock);
}

gboolean
kajo_mod_audio_get_snapshot(KajoModAudio *audio, KajoModAudioSnapshot *out_snap)
{
    if (!audio || !out_snap) return FALSE;
    g_mutex_lock(&audio->lock);
    gboolean has_eng = (audio->eng != NULL);
    *out_snap = audio->snap;
    /* Clear song_ended one-shot flag once consumed by UI */
    audio->snap.song_ended = FALSE;
    g_mutex_unlock(&audio->lock);
    return has_eng;
}
