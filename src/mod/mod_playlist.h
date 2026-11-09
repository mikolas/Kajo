#ifndef KAJO_MOD_PLAYLIST_H
#define KAJO_MOD_PLAYLIST_H

#include "mod_engine.h"
#include "mod_archive.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gchar            *file_path;
    gchar            *display_title;
    gchar            *badge;
    gchar            *badge_css;
    gchar            *sub_text;
    gchar            *dur_text;
    gchar            *archive_nfo_name;
    gchar            *archive_nfo_text;
    KajoModEngineKind engine;
} KajoModPlaylistItem;

typedef struct {
    GPtrArray *items;    /* Element: KajoModPlaylistItem * */
    GPtrArray *archives; /* Element: KajoModArchive * (owned temp dirs) */
} KajoModPlaylist;

KajoModPlaylist     *kajo_mod_playlist_new(void);
void                 kajo_mod_playlist_free(KajoModPlaylist *pl);
void                 kajo_mod_playlist_clear(KajoModPlaylist *pl);
void                 kajo_mod_playlist_remove_at(KajoModPlaylist *pl, guint index);

/* Adds a file, archive (.lha/.zip/.7z/...), or directory recursively.
 * Returns number of playable items added. */
int                  kajo_mod_playlist_add_path(KajoModPlaylist *pl,
                                                const gchar     *path,
                                                GError         **error);

/* Update a playlist item's display fields once opened by its engine */
void                 kajo_mod_playlist_item_update_from_meta(KajoModPlaylistItem   *item,
                                                             const KajoModMetadata *meta);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoModPlaylist, kajo_mod_playlist_free)

#ifdef __cplusplus
}
#endif

#endif /* KAJO_MOD_PLAYLIST_H */
