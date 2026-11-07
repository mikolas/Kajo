#ifndef KAJO_FILES_DISKIMAGE_H
#define KAJO_FILES_DISKIMAGE_H

/* Loop-mounting of ISO / raw disk images through UDisks2 (system bus).
 *
 * Flow: reuse a loop device already set up for the same file by this user, otherwise
 * LoopSetup (read-only) -> wait for a filesystem (the loop itself or its first partition)
 * -> Mount -> SetAutoclear(TRUE) so the loop device disappears again when the filesystem is
 * unmounted. A loop device created here is deleted again if mounting fails. */

#include <gio/gio.h>

G_BEGIN_DECLS

/* `mount_path` is the mount point (valid only for the duration of the call); `error` is set on
 * failure (G_IO_ERROR_CANCELLED when cancelled). */
typedef void (*KajoDiskImageCallback)(const char *mount_path, GError *error, gpointer user_data);

void kajo_files_diskimage_mount_async(GFile *image,
                                      GCancellable *cancellable,
                                      KajoDiskImageCallback callback,
                                      gpointer user_data);

/* ── Pure helpers over the UDisks2 ObjectManager reply (type a{oa{sa{sv}}}); exposed for tests ── */

/* Object path of a loop device backed by `backing_path` that was set up by `uid`, or NULL. */
char *kajo_diskimage_find_loop(GVariant *managed, const char *backing_path, guint32 uid);
/* Object path of the block device carrying the filesystem for `loop_path`: the loop itself, or
 * its lowest-numbered partition that has one. NULL while none is visible yet. */
char *kajo_diskimage_find_filesystem(GVariant *managed, const char *loop_path);
/* First mount point of `fs_path`, or NULL when it is not mounted. */
char *kajo_diskimage_first_mount_point(GVariant *managed, const char *fs_path);

G_END_DECLS

#endif
