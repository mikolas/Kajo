#ifndef KAJO_MOD_AUDIO_H
#define KAJO_MOD_AUDIO_H

#include "mod_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _KajoModAudio KajoModAudio;

typedef struct {
    gboolean          playing;
    gboolean          paused;
    gboolean          song_ended;
    double            pos_sec;
    double            duration_sec;
    KajoModTelemetry  telemetry;
    float             scope_l[KAJO_MOD_SCOPE_SAMPLES];
    float             scope_r[KAJO_MOD_SCOPE_SAMPLES];
    float             fft_bands[KAJO_MOD_FFT_BANDS];
    float             fft_peaks[KAJO_MOD_FFT_BANDS];
} KajoModAudioSnapshot;

KajoModAudio *kajo_mod_audio_new(void);
void          kajo_mod_audio_free(KajoModAudio *audio);

/* Transfers ownership of @eng into the audio player and starts playback */
void          kajo_mod_audio_set_engine(KajoModAudio *audio, KajoModEngine *eng, gboolean start_playing);
void          kajo_mod_audio_stop(KajoModAudio *audio);
void          kajo_mod_audio_set_paused(KajoModAudio *audio, gboolean paused);
gboolean      kajo_mod_audio_get_paused(KajoModAudio *audio);
gboolean      kajo_mod_audio_is_active(KajoModAudio *audio);

void          kajo_mod_audio_seek(KajoModAudio *audio, double target_sec);
gboolean      kajo_mod_audio_set_subsong(KajoModAudio *audio, int subsong_1based, KajoModMetadata *out_meta);
void          kajo_mod_audio_apply_dsp(KajoModAudio *audio, const KajoModDspConfig *dsp);
void          kajo_mod_audio_set_volume(KajoModAudio *audio, double volume_0_to_1);
void          kajo_mod_audio_set_vis_mapped(KajoModAudio *audio, gboolean mapped);

/* Thread-safe snapshot readout for the 60 Hz UI tick */
gboolean      kajo_mod_audio_get_snapshot(KajoModAudio *audio, KajoModAudioSnapshot *out_snap);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoModAudio, kajo_mod_audio_free)

#ifdef __cplusplus
}
#endif

#endif /* KAJO_MOD_AUDIO_H */
