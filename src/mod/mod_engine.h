#ifndef KAJO_MOD_ENGINE_H
#define KAJO_MOD_ENGINE_H

#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KAJO_MOD_ENGINE_KIND_DEFINED
#define KAJO_MOD_ENGINE_KIND_DEFINED

#define KAJO_MOD_SCOPE_SAMPLES 256
#define KAJO_MOD_FFT_BANDS     16

typedef enum {
    KAJO_MOD_ENGINE_UADE = 0,
    KAJO_MOD_ENGINE_SIDPLAYFP,
    KAJO_MOD_ENGINE_GME,
    KAJO_MOD_ENGINE_OPENMPT,
    KAJO_MOD_ENGINE_XMP,
    KAJO_MOD_ENGINE_NONE
} KajoModEngineKind;

#endif /* KAJO_MOD_ENGINE_KIND_DEFINED */

#define KAJO_MOD_SAMPLE_RATE 48000

#ifndef KAJO_MOD_DSP_ENUMS_DEFINED
#define KAJO_MOD_DSP_ENUMS_DEFINED

typedef enum {
    KAJO_MOD_FILTER_A500   = 0,
    KAJO_MOD_FILTER_A1200  = 1,
    KAJO_MOD_FILTER_NEAR   = 2,
    KAJO_MOD_FILTER_LINEAR = 3,
    KAJO_MOD_FILTER_SINC   = 4
} KajoModFilterMode;

typedef enum {
    KAJO_MOD_LED_AUTO = 0,
    KAJO_MOD_LED_ON   = 1,
    KAJO_MOD_LED_OFF  = 2
} KajoModLedMode;

typedef enum {
    KAJO_MOD_SID_AUTO = 0,
    KAJO_MOD_SID_6581 = 1,
    KAJO_MOD_SID_8580 = 2
} KajoModSidModel;

typedef enum {
    KAJO_MOD_END_NEXT_SUB = 0,
    KAJO_MOD_END_LOOP     = 1,
    KAJO_MOD_END_NEXT_MOD = 2
} KajoModEndAction;

#endif /* KAJO_MOD_DSP_ENUMS_DEFINED */

typedef struct {
    KajoModFilterMode filter_mode;
    KajoModLedMode    led_mode;
    KajoModSidModel   sid_model;
    KajoModEndAction  end_action;
    double            stereo_sep_pct; /* 0.0 .. 100.0 (70.0 = default Amiga 70/30) */
    double            tempo_pct;      /* 50.0 .. 200.0 (100.0 = nominal) */
    gboolean          ch_muted[4];
} KajoModDspConfig;

typedef struct {
    gchar idx_hex[8];
    gchar ascii_name[48];
    gchar length[24];
    gchar loop[24];
    gchar vol[24];
} KajoModSampleRow;

typedef struct {
    KajoModEngineKind engine;
    gchar            *file_path;
    gchar            *title;
    gchar            *subtitle;
    gchar            *pl_badge;
    gchar            *pl_badge_css;
    gchar            *hdr_badge;
    gchar            *bdg_engine;
    gchar            *bdg_format;
    gchar            *bdg_chip;
    int               subsong_cur;
    int               subsong_min;
    int               subsong_def;
    int               subsong_max;
    double            duration_sec;
    gchar            *cnt_voices;
    gchar            *cnt_timing;
    gchar            *cnt_clock;
    gchar            *ch_names[4];
    gboolean          filter_enabled;
    gboolean          led_enabled;
    gboolean          sid_enabled;
    gboolean          tempo_enabled;
    gchar            *sample_hdr_left;
    gchar            *sample_hdr_right;
    gchar            *stil_hdr_right;
    gchar            *status_engine;
    gchar            *stil_text;
    GArray           *samples; /* Element type: KajoModSampleRow */
} KajoModMetadata;

typedef struct {
    double           pos_sec;
    double           duration_sec;
    double           ch_vu[4];
    gboolean         song_ended;
    gchar            cnt_timing[24];
    gchar            cnt_clock[24];
    gchar            bdg_chip[32];
    gboolean         has_live_rows;
    int              n_live_rows;
    KajoModSampleRow live_rows[16];
} KajoModTelemetry;

typedef struct _KajoModEngine KajoModEngine;

typedef struct {
    KajoModEngineKind kind;
    const gchar      *name;
    gboolean (*open)(KajoModEngine          *eng,
                     const gchar            *path,
                     int                     subsong_1based,
                     const KajoModDspConfig *dsp,
                     KajoModMetadata        *meta,
                     GError                **error);
    void     (*close)(KajoModEngine *eng);
    int      (*render)(KajoModEngine *eng,
                       int16_t       *stereo_frames,
                       int            max_frames,
                       gboolean      *out_song_ended);
    gboolean (*seek)(KajoModEngine *eng, double target_sec);
    gboolean (*set_subsong)(KajoModEngine *eng, int subsong_1based, KajoModMetadata *meta);
    void     (*apply_dsp)(KajoModEngine *eng, const KajoModDspConfig *dsp);
    void     (*poll_telemetry)(KajoModEngine *eng, KajoModTelemetry *out_tel);
} KajoModEngineVTable;

struct _KajoModEngine {
    const KajoModEngineVTable *vt;
    gpointer                   priv;
    KajoModMetadata            meta;
    KajoModDspConfig           dsp;
    double                     pos_sec;
    gchar                     *archive_nfo_name;
    gchar                     *archive_nfo_text;
};

/* Metadata helpers */
void kajo_mod_metadata_init(KajoModMetadata *meta);
void kajo_mod_metadata_clear(KajoModMetadata *meta);
void kajo_mod_metadata_copy(KajoModMetadata *dst, const KajoModMetadata *src);
void kajo_mod_dsp_config_init(KajoModDspConfig *dsp);

/* Thread-safe global runtime preferences synced from ~/.config/kajo/config.json */
void     kajo_mod_set_runtime_prefs(double       fallback_duration_sec,
                                    const gchar *hvsc_path,
                                    gboolean     auto_extract_archives);
double   kajo_mod_get_fallback_duration_sec(void);
gchar   *kajo_mod_dup_hvsc_path(void);
gboolean kajo_mod_get_auto_extract_archives(void);

/* Strict Priority Format Router (1. UADE -> 2. libsidplayfp -> 3. libgme -> 4. libopenmpt -> 5. libxmp) */
gboolean          kajo_mod_is_c64_sid_magic(const guint8 *hdr, gsize len);
gboolean          kajo_mod_is_amiga_mod_magic(const guint8 *hdr, gsize len, const gchar *basename, guint *out_channels);
void              kajo_mod_parse_protracker_samples(const guint8 *data, gsize len, GArray *out_samples, gchar **out_song_title);
KajoModEngineKind kajo_mod_probe_file(const gchar *path, gchar **out_format_hint);

/* Engine lifecycle & dispatch */
KajoModEngine *kajo_mod_engine_open(const gchar            *path,
                                    int                     subsong_1based,
                                    const KajoModDspConfig *dsp,
                                    const gchar            *archive_nfo_name,
                                    const gchar            *archive_nfo_text,
                                    GError                **error);
void           kajo_mod_engine_free(KajoModEngine *eng);
int            kajo_mod_engine_render(KajoModEngine *eng, int16_t *stereo_frames, int max_frames, gboolean *out_song_ended);
gboolean       kajo_mod_engine_seek(KajoModEngine *eng, double target_sec);
gboolean       kajo_mod_engine_set_subsong(KajoModEngine *eng, int subsong_1based);
void           kajo_mod_engine_apply_dsp(KajoModEngine *eng, const KajoModDspConfig *dsp);
void           kajo_mod_engine_poll_telemetry(KajoModEngine *eng, KajoModTelemetry *out_tel);

/* Backend VTable getters implemented by each backend module */
const KajoModEngineVTable *kajo_mod_backend_uade_vtable(void);
const KajoModEngineVTable *kajo_mod_backend_sid_vtable(void);
const KajoModEngineVTable *kajo_mod_backend_gme_vtable(void);
const KajoModEngineVTable *kajo_mod_backend_openmpt_vtable(void);
const KajoModEngineVTable *kajo_mod_backend_xmp_vtable(void);

/* Direct UADE probe helper (thread-safe via dedicated probe lock) */
gboolean kajo_mod_uade_probe_file(const gchar *path, gchar **out_ext);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoModEngine, kajo_mod_engine_free)

#ifdef __cplusplus
}
#endif

#endif /* KAJO_MOD_ENGINE_H */
