#ifndef KAJO_PLAYER_MPV_H
#define KAJO_PLAYER_MPV_H

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _KajoPlayerMpv KajoPlayerMpv;

typedef struct {
    gboolean has_media;
    gboolean playing;
    gboolean eof_reached;
    gint64 position_usec;
    gint64 duration_usec;
    int video_width;
    int video_height;
    const char *video_codec;
    const char *audio_codec;
    const char *hwdec_current;
    const char *error_message;
} KajoMpvState;

typedef void (*KajoMpvStateChangedFunc)(const KajoMpvState *state, gpointer user_data);

KajoPlayerMpv *kajo_player_mpv_new(GtkGLArea *gl_area,
                                   KajoMpvStateChangedFunc state_cb,
                                   gpointer user_data);

void kajo_player_mpv_free(KajoPlayerMpv *self);

void     kajo_player_mpv_load_file(KajoPlayerMpv *self, GFile *file);
void     kajo_player_mpv_stop(KajoPlayerMpv *self);
void     kajo_player_mpv_set_playing(KajoPlayerMpv *self, gboolean playing);
gboolean kajo_player_mpv_get_playing(const KajoPlayerMpv *self);
gboolean kajo_player_mpv_get_eof_reached(const KajoPlayerMpv *self);

void   kajo_player_mpv_seek_absolute_usec(KajoPlayerMpv *self, gint64 target_usec);
void   kajo_player_mpv_seek_relative_sec(KajoPlayerMpv *self, double delta_sec);
gint64 kajo_player_mpv_get_position_usec(const KajoPlayerMpv *self);
gint64 kajo_player_mpv_get_duration_usec(const KajoPlayerMpv *self);

void kajo_player_mpv_set_volume(KajoPlayerMpv *self, double volume_0_to_1);
void kajo_player_mpv_set_muted(KajoPlayerMpv *self, gboolean muted);
void kajo_player_mpv_set_loop(KajoPlayerMpv *self, gboolean loop_file);

const KajoMpvState *kajo_player_mpv_get_state(const KajoPlayerMpv *self);

G_END_DECLS

#endif /* KAJO_PLAYER_MPV_H */
