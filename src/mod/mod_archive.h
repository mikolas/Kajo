#ifndef KAJO_MOD_ARCHIVE_H
#define KAJO_MOD_ARCHIVE_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gchar     *archive_path;     /* Original archive path on disk */
    gchar     *extract_dir;      /* Private temporary extraction directory */
    GPtrArray *module_paths;     /* Playable primary module file paths (gchar *) */
    GPtrArray *companion_paths;  /* Companion sample/instrument files (smpl.*, smp.*, ins.*) */
    gchar     *nfo_name;         /* Basename of extracted .NFO / .DIZ / README */
    gchar     *nfo_text;         /* UTF-8 converted text of .NFO / .DIZ / README */
} KajoModArchive;

/* Returns TRUE if path has a supported archive extension or magic signature
 * (.lha, .lzh, .lzx, .zip, .7z, .tar, .tgz, .gz). Note: .vgz is handled
 * natively by libgme and is NOT treated as a multi-file container. */
gboolean        kajo_mod_path_is_archive(const gchar *path);

/* Returns TRUE if basename is a secondary companion sample/instrument file
 * (e.g. smpl.turrican2, smp.song, ins.song, *.smpl, *.smp, *.str) that belongs
 * to a primary module in the same directory rather than a standalone track. */
gboolean        kajo_mod_filename_is_companion(const gchar *filename);

/* Returns TRUE if basename is an archive info/readme file (*.nfo, *.diz,
 * *.txt, *.readme, readme*). */
gboolean        kajo_mod_filename_is_nfo(const gchar *filename);

/* Convert raw Amiga ISO-8859-1 / CP437 text buffer to valid UTF-8. */
gchar          *kajo_mod_text_to_utf8(const guint8 *data, gsize len);

/* Extract archive to a private temporary directory and classify its contents. */
KajoModArchive *kajo_mod_archive_extract(const gchar *archive_path, GError **error);

/* Remove temporary extraction directory and free all resources. */
void            kajo_mod_archive_free(KajoModArchive *arch);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoModArchive, kajo_mod_archive_free)

#ifdef __cplusplus
}
#endif

#endif /* KAJO_MOD_ARCHIVE_H */
