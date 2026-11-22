#ifndef KAJO_FILES_OPS_PRIVATE_H
#define KAJO_FILES_OPS_PRIVATE_H

/* Internal to src/files/: helpers shared between files_ops.c and
 * files_ops_archive.c. Not a public API. */

#include "files_ops.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

G_BEGIN_DECLS

typedef struct {
    gboolean regular_only;  /* previews: refuse links, devices and directories */
    gboolean strip_exec;    /* previews: never leave an executable behind */
} KajoArchiveExtractOpts;

/* Defined in files_ops.c */
gboolean   files_ops_delete_file_recursive_sync(GFile *file, GCancellable *cancellable, GError **error);
GFile     *files_ops_resolve_unique_dest_child(GFile *dest_dir, const char *requested_name);
GPtrArray *files_ops_clone_gfile_array(GPtrArray *files);

/* Defined in files_ops_archive.c */
char      *files_ops_strip_archive_stem_dup(const char *path);
gboolean   files_ops_extract_archive_subpath_sync(const char *archive_path,
                                                  const char *inner_path,
                                                  GFile *dest_target,
                                                  const KajoArchiveExtractOpts *opts,
                                                  GCancellable *cancellable,
                                                  GError **error);

G_END_DECLS

#endif /* KAJO_FILES_OPS_PRIVATE_H */
