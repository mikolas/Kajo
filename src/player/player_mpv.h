#ifndef KAJO_PLAYER_MPV_H
#define KAJO_PLAYER_MPV_H

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _KajoPlayerMpv KajoPlayerMpv;

typedef enum {
    KAJO_MPV_TRACK_AUDIO = 0,
    KAJO_MPV_TRACK_SUB,
} KajoMpvTrackType;

typedef struct {
    int id;
    KajoMpvTrackType type;
    gboolean selected;
    gboolean is_default;
    gboolean external;
    char *title;
    char *lang;
    char *codec;
    int channels;
    int samplerate;
} KajoMpvTrack;

typedef struct {
    int index;
    char *title;
    gint64 time_usec;
} KajoMpvChapter;

typedef struct {
    gboolean has_media;
    gboolean playing;
    gboolean eof_reached;
    gint64 position_usec;
    gint64 duration_usec;

    int video_width;
    int video_height;
    double fps;
    int64_t dropped_frames;
    const char *video_codec;
    const char *pixel_format;
    const char *colormatrix;
    const char *hwdec_current;

    const char *audio_codec;
    int audio_channels;
    int audio_samplerate;

    double speed;
    double sub_delay_sec;
    double sub_scale;
    gboolean sub_visible;
    double audio_delay_sec;
    int current_chapter;

    const GPtrArray *tracks;   /* ElementType: KajoMpvTrack* */
    const GPtrArray *chapters; /* ElementType: KajoMpvChapter* */

    const char *error_message;
} KajoMpvState;

typedef void (*KajoMpvStateChangedFunc)(const KajoMpvState *state, gpointer user_data);

/* Pure helper for formatting human-readable track labels */
char *kajo_mpv_track_format_label(const KajoMpvTrack *track);

KajoPlayerMpv *kajo_player_mpv_new(GtkGLArea *gl_area,
                                   KajoMpvStateChangedFunc state_cb,
                                   gpointer user_data);

void kajo_player_mpv_free(KajoPlayerMpv *self);

/* Core Playback */
void     kajo_player_mpv_load_file(KajoPlayerMpv *self, GFile *file);
void     kajo_player_mpv_stop(KajoPlayerMpv *self);
void     kajo_player_mpv_set_playing(KajoPlayerMpv *self, gboolean playing);
gboolean kajo_player_mpv_get_playing(const KajoPlayerMpv *self);
gboolean kajo_player_mpv_get_eof_reached(const KajoPlayerMpv *self);

/* Seeking, Frame-Stepping & Chapters */
void   kajo_player_mpv_seek_absolute_usec(KajoPlayerMpv *self, gint64 target_usec);
void   kajo_player_mpv_seek_relative_sec(KajoPlayerMpv *self, double delta_sec);
gint64 kajo_player_mpv_get_position_usec(const KajoPlayerMpv *self);
gint64 kajo_player_mpv_get_duration_usec(const KajoPlayerMpv *self);
void   kajo_player_mpv_frame_step(KajoPlayerMpv *self, gboolean backward);
void   kajo_player_mpv_set_chapter(KajoPlayerMpv *self, int chapter_idx);
void   kajo_player_mpv_cycle_ab_loop(KajoPlayerMpv *self);

/* Volume, Speed & Loop */
void kajo_player_mpv_set_volume(KajoPlayerMpv *self, double volume_0_to_1);
void kajo_player_mpv_set_muted(KajoPlayerMpv *self, gboolean muted);
void kajo_player_mpv_set_loop(KajoPlayerMpv *self, gboolean loop_file);
void kajo_player_mpv_set_speed(KajoPlayerMpv *self, double speed);
void kajo_player_mpv_adjust_speed(KajoPlayerMpv *self, double delta);

/* Subtitle Track, External Subtitles, Delay & Scale */
void kajo_player_mpv_set_sid(KajoPlayerMpv *self, int sid); /* sid <= 0 disables subtitles */
void kajo_player_mpv_cycle_sub(KajoPlayerMpv *self);
void kajo_player_mpv_toggle_sub_visibility(KajoPlayerMpv *self);
void kajo_player_mpv_add_subtitle_file(KajoPlayerMpv *self, const char *path);
void kajo_player_mpv_set_sub_delay(KajoPlayerMpv *self, double delay_sec);
void kajo_player_mpv_adjust_sub_delay(KajoPlayerMpv *self, double delta_sec);
void kajo_player_mpv_adjust_sub_scale(KajoPlayerMpv *self, double delta);

/* Audio Track & Sync Delay */
void kajo_player_mpv_set_aid(KajoPlayerMpv *self, int aid);
void kajo_player_mpv_cycle_audio(KajoPlayerMpv *self);
void kajo_player_mpv_set_audio_delay(KajoPlayerMpv *self, double delay_sec);
void kajo_player_mpv_adjust_audio_delay(KajoPlayerMpv *self, double delta_sec);

/* Frame Screenshot */
gboolean kajo_player_mpv_take_screenshot(KajoPlayerMpv *self, char **out_path);

const KajoMpvState *kajo_player_mpv_get_state(const KajoPlayerMpv *self);

G_END_DECLS

#endif /* KAJO_PLAYER_MPV_H */
