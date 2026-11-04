#ifndef KAJO_PLAYER_QUEUE_H
#define KAJO_PLAYER_QUEUE_H

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
    GFile *file;
    char *uri;
    char *display_name;
    char *content_type;
    guint64 size;
    gboolean is_audio_only;
} KajoMediaTrack;

KajoMediaTrack *kajo_media_track_new(GFile *file,
                                     const char *display_name,
                                     const char *content_type,
                                     guint64 size);
void            kajo_media_track_free(KajoMediaTrack *track);

/* Formatting & Media Detection Helpers */
char    *kajo_player_format_time(gint64 time_usec);
char    *kajo_player_format_size(guint64 bytes);
gboolean kajo_player_is_supported_media(const char *content_type, const char *filename);
gboolean kajo_player_is_audio_only(const char *content_type, const char *filename);

/* Queue Controller */
typedef void (*KajoQueueTrackSelectedFunc)(const KajoMediaTrack *track,
                                           int index,
                                           int total,
                                           gpointer user_data);

typedef struct _KajoPlayerQueue KajoPlayerQueue;

KajoPlayerQueue *kajo_player_queue_new(GtkListBox *list_box,
                                       GtkWidget *count_badge_lbl,
                                       KajoQueueTrackSelectedFunc select_cb,
                                       gpointer user_data);

void kajo_player_queue_free(KajoPlayerQueue *self);

void                  kajo_player_queue_load_file_and_siblings(KajoPlayerQueue *self, GFile *target_file);
void                  kajo_player_queue_set_files(KajoPlayerQueue *self, GFile **files, int n_files);
const KajoMediaTrack *kajo_player_queue_get_current(const KajoPlayerQueue *self);
int                   kajo_player_queue_get_current_index(const KajoPlayerQueue *self);
guint                 kajo_player_queue_get_length(const KajoPlayerQueue *self);
gboolean              kajo_player_queue_select_index(KajoPlayerQueue *self, int index);
gboolean              kajo_player_queue_next(KajoPlayerQueue *self);
gboolean              kajo_player_queue_prev(KajoPlayerQueue *self);

G_END_DECLS

#endif /* KAJO_PLAYER_QUEUE_H */
