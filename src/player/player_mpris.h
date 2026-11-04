#ifndef KAJO_PLAYER_MPRIS_H
#define KAJO_PLAYER_MPRIS_H

#include <gio/gio.h>
#include "player_queue.h"

G_BEGIN_DECLS

typedef enum {
    KAJO_MPRIS_CMD_PLAY = 0,
    KAJO_MPRIS_CMD_PAUSE,
    KAJO_MPRIS_CMD_PLAY_PAUSE,
    KAJO_MPRIS_CMD_STOP,
    KAJO_MPRIS_CMD_NEXT,
    KAJO_MPRIS_CMD_PREV,
    KAJO_MPRIS_CMD_RAISE,
    KAJO_MPRIS_CMD_QUIT,
} KajoMprisCommand;

typedef void (*KajoMprisCommandFunc)(KajoMprisCommand cmd, gint64 arg_usec, gpointer user_data);

typedef struct _KajoPlayerMpris KajoPlayerMpris;

KajoPlayerMpris *kajo_player_mpris_new(KajoMprisCommandFunc cmd_cb, gpointer user_data);
void             kajo_player_mpris_free(KajoPlayerMpris *self);

void kajo_player_mpris_update_state(KajoPlayerMpris *self,
                                    gboolean has_media,
                                    gboolean playing,
                                    gint64 position_usec,
                                    gint64 duration_usec,
                                    double volume);

void kajo_player_mpris_update_track(KajoPlayerMpris *self,
                                    const KajoMediaTrack *track,
                                    gint64 duration_usec);

G_END_DECLS

#endif /* KAJO_PLAYER_MPRIS_H */
