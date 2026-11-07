#define _DEFAULT_SOURCE
#include "files_ops.h"
#include <archive.h>
#include <archive_entry.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>

typedef struct {
    KajoFileOpCallback callback;
    gpointer user_data;
    char *success_msg;
} SimpleOpData;

static void
simple_op_data_free(SimpleOpData *d)
{
    if (!d) return;
    g_free(d->success_msg);
    g_free(d);
}

/* ─── Create Folder (mkdir) ─── */

static void
on_mkdir_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    SimpleOpData *d = user_data;
    GError *error = NULL;
    gboolean ok = g_file_make_directory_finish(G_FILE(source), res, &error);

    if (d->callback)
        d->callback(ok, ok ? d->success_msg : NULL, error, d->user_data);

    g_clear_error(&error);
    simple_op_data_free(d);
}

void
kajo_files_op_mkdir_async(GFile *parent_dir,
                          const char *folder_name,
                          GCancellable *cancellable,
                          KajoFileOpCallback callback,
                          gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(parent_dir));
    if (!folder_name || folder_name[0] == '\0') {
        if (callback) {
            g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Folder name cannot be empty");
            callback(FALSE, NULL, err, user_data);
        }
        return;
    }

    g_autoptr(GFile) child = g_file_get_child(parent_dir, folder_name);
    SimpleOpData *d = g_new0(SimpleOpData, 1);
    d->callback = callback;
    d->user_data = user_data;
    d->success_msg = g_strdup_printf("Created folder '%s'", folder_name);

    g_file_make_directory_async(child, G_PRIORITY_DEFAULT, cancellable, on_mkdir_ready, d);
}

/* ─── Create Empty File ─── */

static void
on_create_file_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    SimpleOpData *d = user_data;
    GError *error = NULL;
    g_autoptr(GFileOutputStream) stream = g_file_create_finish(G_FILE(source), res, &error);
    if (stream)
        g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, NULL);

    if (d->callback)
        d->callback(stream != NULL, stream ? d->success_msg : NULL, error, d->user_data);

    g_clear_error(&error);
    simple_op_data_free(d);
}

void
kajo_files_op_create_file_async(GFile *parent_dir,
                                const char *file_name,
                                GCancellable *cancellable,
                                KajoFileOpCallback callback,
                                gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(parent_dir));
    if (!file_name || file_name[0] == '\0') {
        if (callback) {
            g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "File name cannot be empty");
            callback(FALSE, NULL, err, user_data);
        }
        return;
    }

    g_autoptr(GFile) child = g_file_get_child(parent_dir, file_name);
    SimpleOpData *d = g_new0(SimpleOpData, 1);
    d->callback = callback;
    d->user_data = user_data;
    d->success_msg = g_strdup_printf("Created file '%s'", file_name);

    g_file_create_async(child, G_FILE_CREATE_NONE, G_PRIORITY_DEFAULT, cancellable, on_create_file_ready, d);
}

/* ─── Rename File / Folder ─── */

static void
on_rename_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    SimpleOpData *d = user_data;
    GError *error = NULL;
    g_autoptr(GFile) renamed = g_file_set_display_name_finish(G_FILE(source), res, &error);

    if (d->callback)
        d->callback(renamed != NULL, renamed ? d->success_msg : NULL, error, d->user_data);

    g_clear_error(&error);
    simple_op_data_free(d);
}

void
kajo_files_op_rename_async(GFile *file,
                           const char *new_name,
                           GCancellable *cancellable,
                           KajoFileOpCallback callback,
                           gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(file));
    if (!new_name || new_name[0] == '\0') {
        if (callback) {
            g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "New name cannot be empty");
            callback(FALSE, NULL, err, user_data);
        }
        return;
    }

    SimpleOpData *d = g_new0(SimpleOpData, 1);
    d->callback = callback;
    d->user_data = user_data;
    d->success_msg = g_strdup_printf("Renamed to '%s'", new_name);

    g_file_set_display_name_async(file, new_name, G_PRIORITY_DEFAULT, cancellable, on_rename_ready, d);
}

static GFile *resolve_unique_dest_child(GFile *dest_dir, const char *base_name);

static char *
strip_archive_stem(const char *basename)
{
    if (!basename || basename[0] == '\0')
        return g_strdup("extracted");

    static const char *const compound_exts[] = {
        ".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2",
        ".tgz", ".txz", ".tzst", ".zip", ".7z", ".rar",
        ".tar", ".deb", ".rpm", ".iso", ".jar", ".apk", NULL
    };

    g_autofree char *lower = g_ascii_strdown(basename, -1);
    for (int i = 0; compound_exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower, compound_exts[i])) {
            gsize stem_len = strlen(basename) - strlen(compound_exts[i]);
            if (stem_len > 0)
                return g_strndup(basename, stem_len);
        }
    }

    const char *dot = strrchr(basename, '.');
    if (dot && dot != basename)
        return g_strndup(basename, (gsize)(dot - basename));

    return g_strdup(basename);
}

/* Stem of the *basename* of a path ("/x/pack.tar.gz" -> "pack"); never contains '/'. */
static char *
strip_archive_stem_dup(const char *path)
{
    g_autofree char *base = g_path_get_basename(path ? path : "");
    return strip_archive_stem(base);
}

/* ─── Archive extraction engine ───
 *
 * One engine serves "extract all", copy-out of archive entries and the Quick Look/Open
 * preview extraction. Paths go through kajo_files_normalize_archive_path() (shared with the
 * virtual-folder listing), destinations are resolved to real paths first (libarchive's
 * SECURE_SYMLINKS refuses any path that merely *passes through* a symlink), permissions are
 * sanitized, and a single bad entry is skipped and reported instead of aborting the rest. */

static gboolean delete_file_recursive_sync(GFile *file, GCancellable *cancellable, GError **error);

/* Resolves symlinks in every existing ancestor of `path`; the tail that does not exist yet is
 * appended unchanged. */
char *
kajo_files_resolve_real_path(const char *path)
{
    if (!path || path[0] != '/')
        return NULL;

    g_autofree char *dir = g_strdup(path);
    GString *tail = g_string_new(NULL);
    while (TRUE) {
        char *rp = realpath(dir, NULL);
        if (rp) {
            char *out = tail->len > 0 ? g_build_filename(rp, tail->str, NULL) : g_strdup(rp);
            free(rp);
            g_string_free(tail, TRUE);
            return out;
        }
        if (g_strcmp0(dir, "/") == 0)
            break;
        g_autofree char *base = g_path_get_basename(dir);
        if (tail->len > 0)
            g_string_prepend_c(tail, '/');
        g_string_prepend(tail, base);
        g_autofree char *parent = g_path_get_dirname(dir);
        g_free(dir);
        dir = g_steal_pointer(&parent);
    }
    g_string_free(tail, TRUE);
    return g_strdup(path);
}

typedef struct {
    guint  extracted;
    guint  skipped;
    char  *first_skip_reason;
} ExtractStats;

static void
extract_stats_skip(ExtractStats *st, const char *reason)
{
    st->skipped++;
    if (!st->first_skip_reason && reason)
        st->first_skip_reason = g_strdup(reason);
}

typedef struct {
    gboolean regular_only;  /* previews: refuse links, devices and directories */
    gboolean strip_exec;    /* previews: never leave an executable behind */
} ExtractOpts;

/* Maps a normalized entry path to its destination, or NULL when the entry is not selected. */
typedef char *(*ArchiveMapFn)(const char *norm, gpointer data);

static struct archive *
open_archive_reader(const char *archive_path, GError **error)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, archive_path, 32768) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Archive open failed: %s", archive_error_string(a));
        archive_read_free(a);
        return NULL;
    }
    return a;
}

static const char *
entry_raw_path(struct archive_entry *entry)
{
    const char *raw = archive_entry_pathname_utf8(entry);
    if (!raw || raw[0] == '\0')
        raw = archive_entry_pathname(entry);
    return raw;
}

static gboolean
scan_archive_top_level(const char *archive_path,
                       GCancellable *cancellable,
                       guint *out_n_top,
                       char **out_single_top,
                       GError **error)
{
    if (out_n_top) *out_n_top = 0;
    if (out_single_top) *out_single_top = NULL;

    struct archive *a = open_archive_reader(archive_path, error);
    if (!a)
        return FALSE;

    g_autoptr(GHashTable) top_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autofree char *first_top = NULL;
    struct archive_entry *entry = NULL;

    while (TRUE) {
        if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
            archive_read_free(a);
            return FALSE;
        }
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_OK && r != ARCHIVE_WARN) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Archive read error: %s", archive_error_string(a));
            archive_read_free(a);
            return FALSE;
        }

        const char *raw_path = entry_raw_path(entry);
        g_autofree char *norm = raw_path ? kajo_files_normalize_archive_path(raw_path) : NULL;
        if (norm && norm[0] != '\0') {
            const char *slash = strchr(norm, '/');
            char *top_seg = slash ? g_strndup(norm, (gsize)(slash - norm)) : g_strdup(norm);
            if (!g_hash_table_contains(top_names, top_seg)) {
                if (!first_top)
                    first_top = g_strdup(top_seg);
                g_hash_table_add(top_names, top_seg);
            } else {
                g_free(top_seg);
            }
        }
        archive_read_data_skip(a);
    }

    archive_read_close(a);
    archive_read_free(a);

    guint n_top = g_hash_table_size(top_names);
    if (out_n_top)
        *out_n_top = n_top;
    if (out_single_top && n_top == 1 && first_top)
        *out_single_top = g_steal_pointer(&first_top);
    return TRUE;
}

/* Writes the current entry. Returns 1 = written, 0 = entry skipped (reason recorded),
 * -1 = fatal (cancelled / data error; *error set). */
static int
write_current_archive_entry(struct archive *a,
                            struct archive *ext,
                            struct archive_entry *entry,
                            const char *full_dest,
                            const char *full_hardlink,
                            const ExtractOpts *opts,
                            ExtractStats *stats,
                            GCancellable *cancellable,
                            GError **error)
{
    mode_t ftype = archive_entry_filetype(entry);
    if (opts->regular_only && (ftype != AE_IFREG || full_hardlink != NULL)) {
        extract_stats_skip(stats, "links, folders and special files are not extracted for preview");
        archive_read_data_skip(a);
        return 0;
    }

    g_autofree char *parent_dir = g_path_get_dirname(full_dest);
    if (parent_dir && parent_dir[0] != '\0' && g_strcmp0(parent_dir, ".") != 0)
        g_mkdir_with_parents(parent_dir, 0755);

    archive_entry_set_pathname(entry, full_dest);
    if (full_hardlink != NULL)
        archive_entry_set_hardlink(entry, full_hardlink);

    /* Never trust archive modes: drop group/other write and setuid/setgid/sticky; keep owner
     * rwx (directories must stay writable for the entries below them). */
    mode_t perm = archive_entry_perm(entry) & 0777 & ~(mode_t)0022;
    if (ftype == AE_IFDIR)
        perm |= 0700;
    else if (opts->strip_exec)
        perm = (perm & ~(mode_t)0111) | 0600;
    archive_entry_set_perm(entry, perm);

    int r = archive_write_header(ext, entry);
    if (r < ARCHIVE_WARN) {
        extract_stats_skip(stats, archive_error_string(ext));
        archive_read_data_skip(a);
        return 0;
    }

    if (archive_entry_size(entry) > 0 && ftype == AE_IFREG) {
        const void *buff = NULL;
        size_t size = 0;
        la_int64_t offset = 0;
        while (TRUE) {
            if (g_cancellable_set_error_if_cancelled(cancellable, error))
                return -1;
            int dr = archive_read_data_block(a, &buff, &size, &offset);
            if (dr == ARCHIVE_EOF)
                break;
            if (dr < ARCHIVE_OK && dr != ARCHIVE_WARN) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Archive data error: %s", archive_error_string(a));
                return -1;
            }
            if (archive_write_data_block(ext, buff, size, offset) < ARCHIVE_WARN) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Archive write error: %s", archive_error_string(ext));
                return -1;
            }
        }
    }

    if (archive_write_finish_entry(ext) < ARCHIVE_WARN) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Archive finish entry error: %s", archive_error_string(ext));
        return -1;
    }
    return 1;
}

/* Core loop: extracts every entry selected by `map`. `stop_after_first_file` ends the scan
 * after the first non-directory entry (single-file extraction). */
static gboolean
extract_selected_entries(const char *archive_path,
                         ArchiveMapFn map,
                         gpointer map_data,
                         const ExtractOpts *opts,
                         gboolean stop_after_first_file,
                         GCancellable *cancellable,
                         ExtractStats *stats,
                         GError **error)
{
    struct archive *a = open_archive_reader(archive_path, error);
    if (!a)
        return FALSE;

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(
        ext,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    archive_write_disk_set_standard_lookup(ext);

    struct archive_entry *entry = NULL;
    gboolean ok = TRUE;

    while (TRUE) {
        if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
            ok = FALSE;
            break;
        }
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_OK && r != ARCHIVE_WARN) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Archive read error: %s", archive_error_string(a));
            ok = FALSE;
            break;
        }

        const char *raw_path = entry_raw_path(entry);
        g_autofree char *norm = raw_path ? kajo_files_normalize_archive_path(raw_path) : NULL;
        if (!norm) { /* contains ".." */
            extract_stats_skip(stats, "entry path escapes the archive (..)");
            archive_read_data_skip(a);
            continue;
        }
        if (norm[0] == '\0') {
            archive_read_data_skip(a);
            continue;
        }

        g_autofree char *full_dest = map(norm, map_data);
        if (!full_dest) { /* not selected */
            archive_read_data_skip(a);
            continue;
        }

        /* Hardlink targets are archive-relative: map them like entry paths, or skip the link. */
        g_autofree char *full_hardlink = NULL;
        const char *hl = archive_entry_hardlink_utf8(entry);
        if (!hl || hl[0] == '\0')
            hl = archive_entry_hardlink(entry);
        if (hl && hl[0] != '\0') {
            g_autofree char *hl_norm = kajo_files_normalize_archive_path(hl);
            if (hl_norm && hl_norm[0] != '\0')
                full_hardlink = map(hl_norm, map_data);
            if (!full_hardlink) {
                extract_stats_skip(stats, "hard link target is outside the extracted entries");
                archive_read_data_skip(a);
                continue;
            }
        }

        mode_t ftype = archive_entry_filetype(entry);
        int w = write_current_archive_entry(a, ext, entry, full_dest, full_hardlink,
                                            opts, stats, cancellable, error);
        if (w < 0) {
            ok = FALSE;
            break;
        }
        if (w > 0) {
            stats->extracted++;
            if (stop_after_first_file && ftype != AE_IFDIR)
                break;
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return ok;
}

/* ── "Extract all": a single top-level entry goes directly into the parent, otherwise into a
 *    folder named after the archive ── */

typedef struct {
    const char *target_root;
    const char *single_top;   /* NULL when the archive has several top-level entries */
    gsize       single_top_len;
} FullExtractMap;

static char *
full_extract_map(const char *norm, gpointer data)
{
    const FullExtractMap *m = data;
    if (m->single_top) {
        if (strcmp(norm, m->single_top) == 0)
            return g_strdup(m->target_root);
        if (strncmp(norm, m->single_top, m->single_top_len) == 0 && norm[m->single_top_len] == '/')
            return g_build_filename(m->target_root, norm + m->single_top_len + 1, NULL);
    }
    return g_build_filename(m->target_root, norm, NULL);
}

static void
remove_partial_tree(const char *path)
{
    if (!path)
        return;
    g_autoptr(GFile) f = g_file_new_for_path(path);
    if (g_file_query_exists(f, NULL))
        delete_file_recursive_sync(f, NULL, NULL);
}

static gboolean
extract_archive_sync(GFile *archive_file,
                     GFile *dest_parent_dir,
                     GCancellable *cancellable,
                     char **out_folder_name,
                     guint *out_count,
                     guint *out_skipped,
                     char **out_skip_reason,
                     GError **error)
{
    g_autofree char *archive_path = g_file_get_path(archive_file);
    if (!archive_path) {
        g_autofree char *uri = g_file_get_uri(archive_file);
        if (!uri || !kajo_files_parse_archive_uri(uri, &archive_path, NULL)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Archive must be on a local or FUSE-mounted filesystem");
            return FALSE;
        }
    }

    g_autoptr(GFile) effective_parent = NULL;
    if (dest_parent_dir) {
        g_autofree char *dp_path = g_file_get_path(dest_parent_dir);
        if (dp_path)
            effective_parent = g_object_ref(dest_parent_dir);
    }
    if (!effective_parent) {
        g_autoptr(GFile) host_arch = g_file_new_for_path(archive_path);
        effective_parent = g_file_get_parent(host_arch);
    }
    if (!effective_parent) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Destination directory must have a local path");
        return FALSE;
    }

    guint n_top = 0;
    g_autofree char *single_top = NULL;
    if (!scan_archive_top_level(archive_path, cancellable, &n_top, &single_top, error))
        return FALSE;

    gboolean single = (n_top == 1 && single_top && single_top[0] != '\0');
    g_autofree char *name_src = single ? g_strdup(single_top)
                                       : strip_archive_stem_dup(archive_path);
    g_autoptr(GFile) target_item = resolve_unique_dest_child(effective_parent, name_src);
    g_autofree char *target_local = target_item ? g_file_get_path(target_item) : NULL;
    if (!target_local) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Destination directory must have a local path");
        return FALSE;
    }
    /* libarchive refuses to write "through" a symlinked ancestor: use the real path. */
    g_autofree char *target_root = kajo_files_resolve_real_path(target_local);

    if (!single && g_mkdir_with_parents(target_root, 0755) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create extraction folder '%s'", target_root);
        return FALSE;
    }

    FullExtractMap map = {
        .target_root = target_root,
        .single_top = single ? single_top : NULL,
        .single_top_len = single ? strlen(single_top) : 0,
    };
    ExtractOpts opts = { .regular_only = FALSE, .strip_exec = FALSE };
    ExtractStats stats = { 0 };

    gboolean ok = extract_selected_entries(archive_path, full_extract_map, &map, &opts, FALSE,
                                           cancellable, &stats, error);
    if (!ok || (stats.extracted == 0 && stats.skipped > 0)) {
        /* Never leave a half-extracted folder that this call created. */
        remove_partial_tree(target_root);
        if (ok) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Nothing could be extracted (%u entries skipped: %s)",
                        stats.skipped, stats.first_skip_reason ? stats.first_skip_reason : "unknown");
        }
        g_free(stats.first_skip_reason);
        return FALSE;
    }

    if (out_folder_name)
        *out_folder_name = g_file_get_basename(target_item);
    if (out_count)
        *out_count = stats.extracted;
    if (out_skipped)
        *out_skipped = stats.skipped;
    if (out_skip_reason)
        *out_skip_reason = g_steal_pointer(&stats.first_skip_reason);
    g_free(stats.first_skip_reason);
    return TRUE;
}

/* ── Extract one entry or a subtree to an explicit destination ── */

typedef struct {
    const char *inner_path;
    gsize       inner_len;
    const char *dest_path;
} SubpathMap;

static char *
subpath_map(const char *norm, gpointer data)
{
    const SubpathMap *m = data;
    if (strcmp(norm, m->inner_path) == 0)
        return g_strdup(m->dest_path);
    if (strncmp(norm, m->inner_path, m->inner_len) == 0 && norm[m->inner_len] == '/')
        return g_build_filename(m->dest_path, norm + m->inner_len + 1, NULL);
    return NULL;
}

static gboolean
extract_archive_subpath_sync(const char *archive_path,
                             const char *inner_path,
                             GFile *dest_target,
                             const ExtractOpts *opts,
                             GCancellable *cancellable,
                             GError **error)
{
    g_autofree char *dest_local = g_file_get_path(dest_target);
    if (!dest_local) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Destination must be a local filesystem path");
        return FALSE;
    }

    if (!inner_path || inner_path[0] == '\0') {
        g_autoptr(GFile) arch_file = g_file_new_for_path(archive_path);
        g_autoptr(GFile) parent = g_file_get_parent(dest_target);
        return extract_archive_sync(arch_file, parent, cancellable, NULL, NULL, NULL, NULL, error);
    }

    g_autofree char *dest_path = kajo_files_resolve_real_path(dest_local);
    gboolean existed = g_file_test(dest_path, G_FILE_TEST_EXISTS);

    SubpathMap map = { .inner_path = inner_path, .inner_len = strlen(inner_path), .dest_path = dest_path };
    ExtractStats stats = { 0 };
    gboolean ok = extract_selected_entries(archive_path, subpath_map, &map, opts,
                                           opts->regular_only, cancellable, &stats, error);

    if (ok && stats.extracted == 0) {
        if (stats.skipped > 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Entry '%s' was not extracted: %s", inner_path,
                        stats.first_skip_reason ? stats.first_skip_reason : "skipped");
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Archive entry '%s' not found", inner_path);
        }
        ok = FALSE;
    }
    if (!ok && !existed)
        remove_partial_tree(dest_path);
    g_free(stats.first_skip_reason);
    return ok;
}

static gboolean
delete_file_recursive_sync(GFile *file, GCancellable *cancellable, GError **error)
{
    g_autoptr(GFileInfo) info = g_file_query_info(
        file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        cancellable,
        error
    );
    if (!info)
        return FALSE;

    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
            file,
            G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            cancellable,
            error
        );
        if (!enumerator)
            return FALSE;

        while (TRUE) {
            GFileInfo *child_info = NULL;
            GFile *child_file = NULL;
            if (!g_file_enumerator_iterate(enumerator, &child_info, &child_file, cancellable, error))
                return FALSE;
            if (!child_info)
                break;
            if (!delete_file_recursive_sync(child_file, cancellable, error))
                return FALSE;
        }
    }

    return g_file_delete(file, cancellable, error);
}

static gboolean
copy_file_recursive_sync(GFile *src, GFile *dst, GCancellable *cancellable, GError **error)
{
    g_autoptr(GFileInfo) info = g_file_query_info(
        src,
        G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        cancellable,
        error
    );
    if (!info)
        return FALSE;

    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GError *mkdir_err = NULL;
        if (!g_file_make_directory(dst, cancellable, &mkdir_err)) {
            if (!g_error_matches(mkdir_err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                g_propagate_error(error, mkdir_err);
                return FALSE;
            }
            g_clear_error(&mkdir_err);
        }

        g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
            src,
            G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            cancellable,
            error
        );
        if (!enumerator)
            return FALSE;

        while (TRUE) {
            GFileInfo *child_info = NULL;
            GFile *child_src = NULL;
            if (!g_file_enumerator_iterate(enumerator, &child_info, &child_src, cancellable, error))
                return FALSE;
            if (!child_info)
                break;
            const char *name = g_file_info_get_name(child_info);
            g_autoptr(GFile) child_dst = g_file_get_child(dst, name);
            if (!copy_file_recursive_sync(child_src, child_dst, cancellable, error))
                return FALSE;
        }
        return TRUE;
    }

    return g_file_copy(
        src,
        dst,
        G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
        cancellable,
        NULL,
        NULL,
        error
    );
}

static GFile *
resolve_unique_dest_child(GFile *dest_dir, const char *requested_name)
{
    /* g_file_get_child() rejects absolute names (and returns NULL): reduce whatever we were
     * given to a plain component first. */
    g_autofree char *safe_name = g_path_get_basename(requested_name ? requested_name : "");
    if (safe_name[0] == '\0' || g_strcmp0(safe_name, "/") == 0 ||
        g_strcmp0(safe_name, ".") == 0 || g_strcmp0(safe_name, "..") == 0) {
        g_free(safe_name);
        safe_name = g_strdup("item");
    }
    const char *base_name = safe_name;
    GFile *candidate = g_file_get_child(dest_dir, base_name);
    if (!g_file_query_exists(candidate, NULL))
        return candidate;

    g_object_unref(candidate);

    static const char *const compound_exts[] = {
        ".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", NULL
    };
    g_autofree char *lower = g_ascii_strdown(base_name, -1);
    g_autofree char *stem = NULL;
    const char *ext = "";

    for (int i = 0; compound_exts[i] != NULL; i++) {
        gsize clen = strlen(compound_exts[i]);
        gsize blen = strlen(base_name);
        if (blen > clen && g_str_has_suffix(lower, compound_exts[i])) {
            stem = g_strndup(base_name, blen - clen);
            ext = base_name + (blen - clen);
            break;
        }
    }

    if (!stem) {
        const char *dot = strrchr(base_name, '.');
        if (dot && dot != base_name) {
            stem = g_strndup(base_name, (gsize)(dot - base_name));
            ext = dot;
        } else {
            stem = g_strdup(base_name);
        }
    }

    for (int i = 1; i < 1000; i++) {
        g_autofree char *try_name = (i == 1)
            ? g_strdup_printf("%s (copy)%s", stem, ext)
            : g_strdup_printf("%s (copy %d)%s", stem, i, ext);
        candidate = g_file_get_child(dest_dir, try_name);
        if (!g_file_query_exists(candidate, NULL))
            return candidate;
        g_object_unref(candidate);
    }

    return g_file_get_child(dest_dir, base_name);
}

static gboolean
configure_archive_writer_format(struct archive *a,
                                const char *filename,
                                GError **error)
{
    g_autofree char *lower = g_ascii_strdown(filename ? filename : ".zip", -1);
    int r = ARCHIVE_OK;

    if (g_str_has_suffix(lower, ".tar.zst") || g_str_has_suffix(lower, ".tzst")) {
        r = archive_write_set_format_pax_restricted(a);
        if (r == ARCHIVE_OK)
            r = archive_write_add_filter_zstd(a);
    } else if (g_str_has_suffix(lower, ".tar.gz") || g_str_has_suffix(lower, ".tgz")) {
        r = archive_write_set_format_pax_restricted(a);
        if (r == ARCHIVE_OK)
            r = archive_write_add_filter_gzip(a);
    } else if (g_str_has_suffix(lower, ".tar.xz") || g_str_has_suffix(lower, ".txz")) {
        r = archive_write_set_format_pax_restricted(a);
        if (r == ARCHIVE_OK)
            r = archive_write_add_filter_xz(a);
    } else if (g_str_has_suffix(lower, ".tar.bz2") || g_str_has_suffix(lower, ".tbz2")) {
        r = archive_write_set_format_pax_restricted(a);
        if (r == ARCHIVE_OK)
            r = archive_write_add_filter_bzip2(a);
    } else if (g_str_has_suffix(lower, ".tar")) {
        r = archive_write_set_format_pax_restricted(a);
    } else {
        r = archive_write_set_format_zip(a);
    }

    if (r < ARCHIVE_OK && r != ARCHIVE_WARN) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Archive format setup failed: %s", archive_error_string(a));
        return FALSE;
    }
    return TRUE;
}

static char *
ensure_supported_archive_extension(const char *raw_name)
{
    if (!raw_name || raw_name[0] == '\0')
        return g_strdup("archive.zip");

    g_autofree char *base = g_path_get_basename(raw_name);
    if (base[0] == '\0' || g_strcmp0(base, "/") == 0 || g_strcmp0(base, ".") == 0 ||
        g_strcmp0(base, "..") == 0) {
        g_free(base);
        base = g_strdup("archive");
    }
    g_autofree char *lower = g_ascii_strdown(base, -1);
    static const char *const valid_exts[] = {
        ".zip", ".tar.zst", ".tzst", ".tar.gz", ".tgz",
        ".tar.xz", ".txz", ".tar.bz2", ".tbz2", ".tar", NULL
    };
    for (int i = 0; valid_exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower, valid_exts[i]) && strlen(base) > strlen(valid_exts[i]))
            return g_strdup(base);
    }
    return g_strdup_printf("%s.zip", base);
}

static gboolean
compress_add_file_recursive(struct archive *a,
                            GFile *src_file,
                            const char *rel_path,
                            const char *skip_abs_path,
                            GCancellable *cancellable,
                            guint *inout_count,
                            GError **error)
{
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return FALSE;

    g_autofree char *local_path = g_file_get_path(src_file);
    if (!local_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Compression requires local filesystem items");
        return FALSE;
    }

    if (skip_abs_path && g_strcmp0(local_path, skip_abs_path) == 0)
        return TRUE;

    GStatBuf st;
    if (g_lstat(local_path, &st) != 0) {
        int errsv = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errsv),
                    "Unable to stat '%s': %s", rel_path, g_strerror(errsv));
        return FALSE;
    }

    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_mtime(entry, st.st_mtime, 0);

    if (S_ISDIR(st.st_mode)) {
        g_autofree char *dir_rel = g_str_has_suffix(rel_path, "/")
            ? g_strdup(rel_path)
            : g_strdup_printf("%s/", rel_path);
        archive_entry_set_pathname(entry, dir_rel);
        archive_entry_set_filetype(entry, AE_IFDIR);
        archive_entry_set_perm(entry, (st.st_mode & 07777) ? (st.st_mode & 07777) : 0755);
        archive_entry_set_size(entry, 0);

        if (archive_write_header(a, entry) < ARCHIVE_WARN) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Archive header write failed: %s", archive_error_string(a));
            archive_entry_free(entry);
            return FALSE;
        }
        archive_entry_free(entry);
        (*inout_count)++;

        g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
            src_file,
            G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            cancellable,
            error
        );
        if (!enumerator)
            return FALSE;

        while (TRUE) {
            GFileInfo *child_info = NULL;
            GFile *child_file = NULL;
            if (!g_file_enumerator_iterate(enumerator, &child_info, &child_file, cancellable, error))
                return FALSE;
            if (!child_info)
                break;
            const char *child_name = g_file_info_get_name(child_info);
            if (!child_name || child_name[0] == '\0')
                continue;
            g_autofree char *child_rel = g_strdup_printf("%s/%s", rel_path, child_name);
            if (!compress_add_file_recursive(a, child_file, child_rel, skip_abs_path, cancellable, inout_count, error))
                return FALSE;
        }
        return TRUE;
    }

    if (S_ISLNK(st.st_mode)) {
        g_autofree char *link_target = g_file_read_link(local_path, error);
        if (!link_target) {
            archive_entry_free(entry);
            return FALSE;
        }
        archive_entry_set_pathname(entry, rel_path);
        archive_entry_set_filetype(entry, AE_IFLNK);
        archive_entry_set_perm(entry, 0777);
        archive_entry_set_size(entry, 0);
        archive_entry_set_symlink(entry, link_target);

        if (archive_write_header(a, entry) < ARCHIVE_WARN) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Archive symlink write failed: %s", archive_error_string(a));
            archive_entry_free(entry);
            return FALSE;
        }
        archive_entry_free(entry);
        (*inout_count)++;
        return TRUE;
    }

    if (!S_ISREG(st.st_mode)) {
        archive_entry_free(entry);
        return TRUE;
    }

    archive_entry_set_pathname(entry, rel_path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, (st.st_mode & 07777) ? (st.st_mode & 07777) : 0644);
    archive_entry_set_size(entry, st.st_size);

    if (archive_write_header(a, entry) < ARCHIVE_WARN) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Archive header write failed: %s", archive_error_string(a));
        archive_entry_free(entry);
        return FALSE;
    }
    archive_entry_free(entry);

    g_autoptr(GFileInputStream) in = g_file_read(src_file, cancellable, error);
    if (!in)
        return FALSE;

    guint8 buf[65536];
    while (TRUE) {
        gssize n_read = g_input_stream_read(G_INPUT_STREAM(in), buf, sizeof(buf), cancellable, error);
        if (n_read < 0)
            return FALSE;
        if (n_read == 0)
            break;
        la_ssize_t written = archive_write_data(a, buf, (size_t)n_read);
        if (written < 0 || (gssize)written != n_read) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Archive data write failed: %s", archive_error_string(a));
            return FALSE;
        }
    }

    (*inout_count)++;
    return TRUE;
}

static gboolean
compress_archive_sync(GPtrArray *source_files,
                      GFile *dest_parent_dir,
                      const char *archive_filename,
                      GCancellable *cancellable,
                      char **out_created_name,
                      guint *out_count,
                      GError **error)
{
    if (!source_files || source_files->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "No source files selected for compression");
        return FALSE;
    }

    g_autofree char *norm_name = ensure_supported_archive_extension(archive_filename);
    g_autoptr(GFile) out_file = resolve_unique_dest_child(dest_parent_dir, norm_name);
    g_autofree char *out_path = g_file_get_path(out_file);
    if (!out_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Destination directory must have a local path");
        return FALSE;
    }

    struct archive *a = archive_write_new();
    if (!configure_archive_writer_format(a, norm_name, error)) {
        archive_write_free(a);
        return FALSE;
    }

    if (archive_write_open_filename(a, out_path) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create archive '%s': %s", norm_name, archive_error_string(a));
        archive_write_free(a);
        return FALSE;
    }

    guint written_count = 0;
    gboolean ok = TRUE;

    for (guint i = 0; i < source_files->len; i++) {
        GFile *src = g_ptr_array_index(source_files, i);
        g_autofree char *base = g_file_get_basename(src);
        if (!base || base[0] == '\0')
            continue;
        if (!compress_add_file_recursive(a, src, base, out_path, cancellable, &written_count, error)) {
            ok = FALSE;
            break;
        }
    }

    if (archive_write_close(a) != ARCHIVE_OK && ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to finalize archive: %s", archive_error_string(a));
        ok = FALSE;
    }
    archive_write_free(a);

    if (!ok) {
        g_unlink(out_path);
        return FALSE;
    }

    if (out_created_name)
        *out_created_name = g_file_get_basename(out_file);
    if (out_count)
        *out_count = written_count;
    return TRUE;
}

typedef enum {
    BATCH_OP_TRASH,
    BATCH_OP_DELETE_PERM,
    BATCH_OP_EMPTY_TRASH,
    BATCH_OP_RESTORE_TRASH,
    BATCH_OP_TRANSFER,
    BATCH_OP_EXTRACT_ARCHIVE,
    BATCH_OP_COMPRESS,
} BatchOpKind;

typedef struct {
    BatchOpKind kind;
    GPtrArray *files;          /* GFile* array */
    GPtrArray *restore_paths;  /* char* array for batch restore */
    GFile *dest_dir;           /* for transfer / extract / compress */
    char *archive_name;        /* for BATCH_OP_COMPRESS */
    gboolean is_move;
    KajoFileOpCallback callback;
    gpointer user_data;
} BatchOpContext;

static void
batch_op_context_free(BatchOpContext *ctx)
{
    if (!ctx) return;
    if (ctx->files)
        g_ptr_array_unref(ctx->files);
    if (ctx->restore_paths)
        g_ptr_array_unref(ctx->restore_paths);
    g_clear_object(&ctx->dest_dir);
    g_free(ctx->archive_name);
    g_free(ctx);
}

static void
batch_op_worker(GTask *task,
                gpointer source_object G_GNUC_UNUSED,
                gpointer task_data,
                GCancellable *cancellable)
{
    BatchOpContext *ctx = task_data;
    GError *error = NULL;
    guint count = ctx->files ? ctx->files->len : 0;

    switch (ctx->kind) {
    case BATCH_OP_TRASH:
        for (guint i = 0; i < count; i++) {
            GFile *f = g_ptr_array_index(ctx->files, i);
            if (!g_file_trash(f, cancellable, &error)) {
                g_task_return_error(task, error);
                return;
            }
        }
        g_task_return_pointer(task, g_strdup_printf("Moved %u item(s) to Trash", count), g_free);
        return;

    case BATCH_OP_DELETE_PERM:
        for (guint i = 0; i < count; i++) {
            GFile *f = g_ptr_array_index(ctx->files, i);
            if (!delete_file_recursive_sync(f, cancellable, &error)) {
                g_task_return_error(task, error);
                return;
            }
        }
        g_task_return_pointer(task, g_strdup_printf("Permanently deleted %u item(s)", count), g_free);
        return;

    case BATCH_OP_EMPTY_TRASH: {
        g_autoptr(GFile) trash_root = g_file_new_for_uri("trash:///");
        g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
            trash_root,
            G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            cancellable,
            &error
        );
        if (!enumerator) {
            g_task_return_error(task, error);
            return;
        }
        guint removed = 0;
        while (TRUE) {
            GFileInfo *info = NULL;
            GFile *child = NULL;
            if (!g_file_enumerator_iterate(enumerator, &info, &child, cancellable, &error)) {
                g_task_return_error(task, error);
                return;
            }
            if (!info)
                break;
            if (!delete_file_recursive_sync(child, cancellable, &error)) {
                g_task_return_error(task, error);
                return;
            }
            removed++;
        }
        g_task_return_pointer(task, g_strdup_printf("Emptied Trash (%u item(s) removed)", removed), g_free);
        return;
    }

    case BATCH_OP_RESTORE_TRASH: {
        if (!ctx->files || !ctx->restore_paths || ctx->files->len == 0 ||
            ctx->files->len != ctx->restore_paths->len) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No trash items specified");
            return;
        }
        for (guint i = 0; i < ctx->files->len; i++) {
            GFile *trash_file = g_ptr_array_index(ctx->files, i);
            const char *restore_path = g_ptr_array_index(ctx->restore_paths, i);
            g_autoptr(GFile) orig_target = g_file_new_for_path(restore_path);
            g_autoptr(GFile) parent = g_file_get_parent(orig_target);
            g_autofree char *base = g_file_get_basename(orig_target);
            if (parent) {
                g_autofree char *parent_path = g_file_get_path(parent);
                if (parent_path)
                    g_mkdir_with_parents(parent_path, 0755);
            }
            g_autoptr(GFile) target = (parent && base)
                ? resolve_unique_dest_child(parent, base)
                : g_object_ref(orig_target);
            if (!g_file_move(trash_file, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, NULL, NULL, &error)) {
                g_task_return_error(task, error);
                return;
            }
        }
        if (ctx->files->len == 1) {
            const char *first_path = g_ptr_array_index(ctx->restore_paths, 0);
            g_task_return_pointer(task, g_strdup_printf("Restored to %s", first_path), g_free);
        } else {
            g_task_return_pointer(task, g_strdup_printf("Restored %u items from Trash", ctx->files->len), g_free);
        }
        return;
    }

    case BATCH_OP_TRANSFER:
        for (guint i = 0; i < count; i++) {
            GFile *src = g_ptr_array_index(ctx->files, i);
            g_autofree char *src_uri = g_file_get_uri(src);
            g_autofree char *arch_path = NULL;
            g_autofree char *inner_path = NULL;

            if (src_uri && kajo_files_parse_archive_uri(src_uri, &arch_path, &inner_path)) {
                if (ctx->is_move) {
                    g_task_return_new_error(
                        task, G_IO_ERROR, G_IO_ERROR_READ_ONLY,
                        "Archive folders are read-only; cannot move items out of an archive"
                    );
                    return;
                }
                g_autofree char *arch_base = (inner_path && inner_path[0] != '\0')
                    ? g_path_get_basename(inner_path)
                    : strip_archive_stem_dup(arch_path);
                g_autoptr(GFile) dst = resolve_unique_dest_child(
                    ctx->dest_dir,
                    (arch_base && arch_base[0] != '\0') ? arch_base : "extracted"
                );
                const ExtractOpts copy_opts = { .regular_only = FALSE, .strip_exec = FALSE };
                if (!dst || !extract_archive_subpath_sync(arch_path, inner_path, dst, &copy_opts, cancellable, &error)) {
                    if (!error)
                        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                            "Invalid destination name");
                    g_task_return_error(task, error);
                    return;
                }
                continue;
            }

            g_autofree char *base = g_file_get_basename(src);
            if (!base) base = g_strdup("item");

            g_autoptr(GFile) dst = NULL;
            if (ctx->is_move) {
                g_autoptr(GFile) exact_dst = g_file_get_child(ctx->dest_dir, base);
                if (g_file_equal(src, exact_dst))
                    continue;
                dst = resolve_unique_dest_child(ctx->dest_dir, base);
                GError *move_err = NULL;
                if (!g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL, NULL, &move_err)) {
                    if (g_error_matches(move_err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
                        g_clear_error(&move_err);
                        if (!copy_file_recursive_sync(src, dst, cancellable, &error)) {
                            g_task_return_error(task, error);
                            return;
                        }
                        if (!delete_file_recursive_sync(src, cancellable, &error)) {
                            g_task_return_error(task, error);
                            return;
                        }
                    } else {
                        g_task_return_error(task, move_err);
                        return;
                    }
                }
            } else {
                dst = resolve_unique_dest_child(ctx->dest_dir, base);
                if (!copy_file_recursive_sync(src, dst, cancellable, &error)) {
                    g_task_return_error(task, error);
                    return;
                }
            }
        }
        g_task_return_pointer(
            task,
            g_strdup_printf("%s %u item(s)", ctx->is_move ? "Moved" : "Copied", count),
            g_free
        );
        return;

    case BATCH_OP_EXTRACT_ARCHIVE: {
        if (!ctx->files || ctx->files->len == 0) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No archive specified");
            return;
        }
        GFile *archive_file = g_ptr_array_index(ctx->files, 0);
        g_autofree char *folder_name = NULL;
        guint extracted = 0;
        guint skipped = 0;
        g_autofree char *skip_reason = NULL;
        if (!extract_archive_sync(archive_file, ctx->dest_dir, cancellable, &folder_name, &extracted,
                                  &skipped, &skip_reason, &error)) {
            g_task_return_error(task, error);
            return;
        }
        g_autofree char *done_msg = (skipped > 0)
            ? g_strdup_printf("Extracted %u entries into '%s' - %u skipped (%s)", extracted,
                              folder_name ? folder_name : "folder", skipped,
                              skip_reason ? skip_reason : "unsafe or unsupported entry")
            : g_strdup_printf("Extracted %u entries into '%s'", extracted,
                              folder_name ? folder_name : "folder");
        g_task_return_pointer(task, g_steal_pointer(&done_msg), g_free);
        return;
    }

    case BATCH_OP_COMPRESS: {
        g_autofree char *created_name = NULL;
        guint compressed_count = 0;
        if (!compress_archive_sync(ctx->files, ctx->dest_dir, ctx->archive_name, cancellable, &created_name, &compressed_count, &error)) {
            g_task_return_error(task, error);
            return;
        }
        g_task_return_pointer(
            task,
            g_strdup_printf("Compressed %u item(s) into '%s'", compressed_count, created_name ? created_name : "archive"),
            g_free
        );
        return;
    }
    }
}

static void
on_batch_task_done(GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    BatchOpContext *ctx = user_data;
    GError *error = NULL;
    g_autofree char *msg = g_task_propagate_pointer(G_TASK(res), &error);

    if (ctx->callback)
        ctx->callback(msg != NULL, msg, error, ctx->user_data);

    g_clear_error(&error);
    batch_op_context_free(ctx);
}

static GPtrArray *
clone_gfile_array(GPtrArray *files)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    if (!files) return arr;
    for (guint i = 0; i < files->len; i++) {
        gpointer p = g_ptr_array_index(files, i);
        if (KAJO_IS_FILE_ITEM(p)) {
            GFile *f = kajo_file_item_get_file(KAJO_FILE_ITEM(p));
            if (f) g_ptr_array_add(arr, g_object_ref(f));
        } else if (G_IS_FILE(p)) {
            g_ptr_array_add(arr, g_object_ref(p));
        }
    }
    return arr;
}

void
kajo_files_op_trash_async(GPtrArray *files,
                          GCancellable *cancellable,
                          KajoFileOpCallback callback,
                          gpointer user_data)
{
    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_TRASH;
    ctx->files = clone_gfile_array(files);
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

void
kajo_files_op_delete_permanent_async(GPtrArray *files,
                                     GCancellable *cancellable,
                                     KajoFileOpCallback callback,
                                     gpointer user_data)
{
    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_DELETE_PERM;
    ctx->files = clone_gfile_array(files);
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

void
kajo_files_op_empty_trash_async(GCancellable *cancellable,
                                KajoFileOpCallback callback,
                                gpointer user_data)
{
    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_EMPTY_TRASH;
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

void
kajo_files_op_restore_from_trash_async(GPtrArray *items,
                                       GCancellable *cancellable,
                                       KajoFileOpCallback callback,
                                       gpointer user_data)
{
    if (!items || items->len == 0) {
        if (callback) {
            g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No trash items selected");
            callback(FALSE, NULL, err, user_data);
        }
        return;
    }

    GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < items->len; i++) {
        gpointer p = g_ptr_array_index(items, i);
        if (!KAJO_IS_FILE_ITEM(p)) continue;
        KajoFileItem *it = KAJO_FILE_ITEM(p);
        const char *orig_path = kajo_file_item_get_trash_orig_path(it);
        GFile *f = kajo_file_item_get_file(it);
        if (orig_path && orig_path[0] != '\0' && f) {
            g_ptr_array_add(files, g_object_ref(f));
            g_ptr_array_add(paths, g_strdup(orig_path));
        }
    }

    if (files->len == 0) {
        g_ptr_array_unref(files);
        g_ptr_array_unref(paths);
        if (callback) {
            g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Original path metadata unavailable in Trash");
            callback(FALSE, NULL, err, user_data);
        }
        return;
    }

    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_RESTORE_TRASH;
    ctx->files = files;
    ctx->restore_paths = paths;
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

void
kajo_files_op_transfer_async(GPtrArray *source_files,
                             GFile *dest_dir,
                             gboolean is_move,
                             GCancellable *cancellable,
                             KajoFileOpCallback callback,
                             gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(dest_dir));

    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_TRANSFER;
    ctx->files = clone_gfile_array(source_files);
    ctx->dest_dir = g_object_ref(dest_dir);
    ctx->is_move = is_move;
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

void
kajo_files_op_extract_archive_async(GFile *archive_file,
                                    GFile *dest_parent_dir,
                                    GCancellable *cancellable,
                                    KajoFileOpCallback callback,
                                    gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(archive_file));

    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_EXTRACT_ARCHIVE;
    ctx->files = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(ctx->files, g_object_ref(archive_file));
    if (dest_parent_dir)
        ctx->dest_dir = g_object_ref(dest_parent_dir);
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

void
kajo_files_op_compress_async(GPtrArray *source_files,
                             GFile *dest_parent_dir,
                             const char *archive_filename,
                             GCancellable *cancellable,
                             KajoFileOpCallback callback,
                             gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(dest_parent_dir));

    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_COMPRESS;
    ctx->files = clone_gfile_array(source_files);
    ctx->dest_dir = g_object_ref(dest_parent_dir);
    ctx->archive_name = g_strdup((archive_filename && archive_filename[0] != '\0') ? archive_filename : "archive.zip");
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

typedef struct {
    char *archive_uri;
    KajoArchiveEntryExtractCallback callback;
    gpointer user_data;
} TempExtractCtx;

static void
temp_extract_ctx_free(TempExtractCtx *tctx)
{
    if (!tctx) return;
    g_free(tctx->archive_uri);
    g_free(tctx);
}

#define PREVIEW_CACHE_MAX_AGE_SEC (7 * 24 * 3600)

/* Removes top-level cache entries (directories/files, never following symlinks) whose mtime is
 * older than `max_age_sec`. Only direct children of `root` are considered. */
void
kajo_files_prune_cache_dir(const char *root, gint64 max_age_sec)
{
    if (!root || root[0] == '\0')
        return;
    g_autoptr(GDir) dir = g_dir_open(root, 0, NULL);
    if (!dir)
        return;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *child = g_build_filename(root, name, NULL);
        GStatBuf st;
        if (g_lstat(child, &st) != 0)
            continue;
        if (now - (gint64)st.st_mtime <= max_age_sec)
            continue;
        if (S_ISLNK(st.st_mode) || S_ISREG(st.st_mode)) {
            g_unlink(child);
        } else if (S_ISDIR(st.st_mode)) {
            g_autoptr(GFile) f = g_file_new_for_path(child);
            delete_file_recursive_sync(f, NULL, NULL);
        }
    }
}

static void
extract_entry_to_temp_worker(GTask *task,
                             gpointer source_object G_GNUC_UNUSED,
                             gpointer task_data,
                             GCancellable *cancellable)
{
    TempExtractCtx *tctx = task_data;
    GError *error = NULL;
    g_autofree char *arch_path = NULL;
    g_autofree char *inner_path = NULL;

    if (!kajo_files_parse_archive_uri(tctx->archive_uri, &arch_path, &inner_path) ||
        !inner_path || inner_path[0] == '\0') {
        g_task_return_new_error(
            task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid archive entry URI"
        );
        return;
    }

    GStatBuf st;
    gint64 mtime = 0;
    gint64 sz = 0;
    if (g_stat(arch_path, &st) == 0) {
        mtime = (gint64)st.st_mtime;
        sz = (gint64)st.st_size;
    }

    g_autofree char *cache_root = g_build_filename(g_get_user_cache_dir(), "kajo", "archive-preview", NULL);
    static gsize pruned_once = 0;
    if (g_once_init_enter(&pruned_once)) {
        kajo_files_prune_cache_dir(cache_root, PREVIEW_CACHE_MAX_AGE_SEC);
        g_once_init_leave(&pruned_once, 1);
    }

    g_autofree char *hash_input = g_strdup_printf(
        "%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s",
        arch_path, mtime, sz, inner_path
    );
    g_autofree char *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, hash_input, -1);
    g_autofree char *cache_dir = g_build_filename(cache_root, sha, NULL);

    if (g_mkdir_with_parents(cache_dir, 0700) != 0) {
        g_task_return_new_error(
            task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Failed to create temporary preview cache directory"
        );
        return;
    }

    g_autofree char *base = g_path_get_basename(inner_path);
    g_autofree char *dest_file_path = g_build_filename(
        cache_dir, (base && base[0] != '\0') ? base : "entry", NULL
    );
    g_autoptr(GFile) dest_file = g_file_new_for_path(dest_file_path);

    GStatBuf cached;
    if (g_lstat(dest_file_path, &cached) == 0 && S_ISREG(cached.st_mode)) {
        g_utime(cache_dir, NULL); /* keep recently used entries out of the age-based prune */
    } else {
        /* Extract to a private temp name, then rename: a killed process or a concurrent
         * extraction of the same entry can never leave a truncated file that later looks cached. */
        g_autofree char *part_path = g_strdup_printf("%s.part-%d-%08x", dest_file_path,
                                                     (int)getpid(), g_random_int());
        g_autoptr(GFile) part_file = g_file_new_for_path(part_path);
        /* Previews never keep links, special files or executable bits. */
        const ExtractOpts opts = { .regular_only = TRUE, .strip_exec = TRUE };
        if (!extract_archive_subpath_sync(arch_path, inner_path, part_file, &opts, cancellable, &error)) {
            g_unlink(part_path);
            g_task_return_error(task, error);
            return;
        }
        if (g_rename(part_path, dest_file_path) != 0) {
            int errsv = errno;
            g_unlink(part_path);
            g_task_return_new_error(task, G_IO_ERROR, g_io_error_from_errno(errsv),
                                    "Failed to finalize preview file: %s", g_strerror(errsv));
            return;
        }
    }

    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    g_task_return_pointer(task, g_steal_pointer(&dest_file), g_object_unref);
}

static void
on_extract_entry_to_temp_done(GObject *source G_GNUC_UNUSED,
                              GAsyncResult *res,
                              gpointer user_data)
{
    TempExtractCtx *tctx = user_data;
    GError *error = NULL;
    g_autoptr(GFile) temp_file = g_task_propagate_pointer(G_TASK(res), &error);

    if (tctx->callback)
        tctx->callback(temp_file, error, tctx->user_data);

    g_clear_error(&error);
    temp_extract_ctx_free(tctx);
}

void
kajo_files_op_extract_entry_to_temp_async(const char *archive_uri,
                                          GCancellable *cancellable,
                                          KajoArchiveEntryExtractCallback callback,
                                          gpointer user_data)
{
    TempExtractCtx *tctx = g_new0(TempExtractCtx, 1);
    tctx->archive_uri = g_strdup(archive_uri ? archive_uri : "");
    tctx->callback = callback;
    tctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_extract_entry_to_temp_done, tctx);
    g_task_set_task_data(task, tctx, NULL);
    g_task_run_in_thread(task, extract_entry_to_temp_worker);
}

/* ─── Clipboard (Wayland GdkClipboard) ─── */

#define CLIPBOARD_READ_CHUNK_BYTES (64 * 1024)
#define CLIPBOARD_MAX_TOTAL_BYTES  (4 * 1024 * 1024)

/* Used only in headless tests where gdk_display_get_default() == NULL */
static char *g_headless_clip_payload = NULL;

static gboolean
is_supported_clipboard_uri_scheme(const char *scheme)
{
    if (!scheme || scheme[0] == '\0')
        return FALSE;
    static const char *const allowed[] = {
        "file", "trash", "archive", "smb", "sftp", "ftp",
        "dav", "davs", "mtp", "afc", "gphoto2", NULL
    };
    for (int i = 0; allowed[i] != NULL; i++) {
        if (g_ascii_strcasecmp(scheme, allowed[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

gboolean
kajo_files_clipboard_buffer_append(GByteArray *buf, const guint8 *data, gsize len)
{
    g_return_val_if_fail(buf != NULL, FALSE);
    if (len == 0)
        return TRUE;
    if (!data || buf->len + len > CLIPBOARD_MAX_TOTAL_BYTES)
        return FALSE; /* never keep a partial buffer: a cut-off last URI could name another file */
    g_byte_array_append(buf, data, (guint)len);
    return TRUE;
}

gboolean
kajo_files_parse_clipboard_data(const char *mime_type,
                                const char *raw_data,
                                gsize raw_len,
                                GPtrArray **out_files,
                                gboolean *out_is_cut)
{
    if (out_files) *out_files = NULL;
    if (out_is_cut) *out_is_cut = FALSE;
    if (!raw_data || raw_len == 0)
        return FALSE;

    g_autofree char *text = g_strndup(raw_data, raw_len);
    g_auto(GStrv) lines = g_strsplit_set(text, "\r\n", -1);

    gboolean is_cut = FALSE;
    gboolean is_gnome_mime = (mime_type && g_strcmp0(mime_type, "x-special/gnome-copied-files") == 0);
    gboolean seen_header = FALSE;
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_object_unref);

    for (int i = 0; lines && lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);
        if (line[0] == '\0' || line[0] == '#')
            continue;

        if (!seen_header && (g_strcmp0(line, "cut") == 0 || g_strcmp0(line, "copy") == 0)) {
            is_cut = (g_strcmp0(line, "cut") == 0);
            seen_header = TRUE;
            continue;
        }
        seen_header = TRUE;

        if (strstr(line, "://") != NULL) {
            g_autofree char *scheme = g_uri_parse_scheme(line);
            if (!is_supported_clipboard_uri_scheme(scheme))
                continue;
            g_ptr_array_add(files, g_file_new_for_uri(line));
        } else if (line[0] == '/' && is_gnome_mime) {
            g_ptr_array_add(files, g_file_new_for_path(line));
        }
    }

    if (files->len == 0)
        return FALSE;

    if (out_files)
        *out_files = g_steal_pointer(&files);
    if (out_is_cut)
        *out_is_cut = is_cut;
    return TRUE;
}

void
kajo_files_clipboard_set(GPtrArray *files, gboolean is_cut)
{
    g_clear_pointer(&g_headless_clip_payload, g_free);

    g_autoptr(GPtrArray) gfiles = clone_gfile_array(files);
    if (!gfiles || gfiles->len == 0)
        return;

    GString *uri_list = g_string_new(NULL);
    GString *gnome_list = g_string_new(is_cut ? "cut\n" : "copy\n");
    for (guint i = 0; i < gfiles->len; i++) {
        GFile *f = g_ptr_array_index(gfiles, i);
        g_autofree char *uri = g_file_get_uri(f);
        if (uri) {
            g_string_append(uri_list, uri);
            g_string_append(uri_list, "\r\n");
            g_string_append(gnome_list, uri);
            g_string_append(gnome_list, "\n");
        }
    }

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        g_autoptr(GBytes) uri_bytes = g_string_free_to_bytes(uri_list);
        g_autoptr(GBytes) gnome_bytes = g_string_free_to_bytes(gnome_list);
        GdkContentProvider *providers[2] = {
            gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", gnome_bytes),
            gdk_content_provider_new_for_bytes("text/uri-list", uri_bytes),
        };
        g_autoptr(GdkContentProvider) union_prov = gdk_content_provider_new_union(providers, 2);
        gdk_clipboard_set_content(clipboard, union_prov);
    } else {
        g_string_free(uri_list, TRUE);
        g_headless_clip_payload = g_string_free(gnome_list, FALSE);
    }
}

gboolean
kajo_files_clipboard_has_files(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
        if (formats &&
            (gdk_content_formats_contain_mime_type(formats, "x-special/gnome-copied-files") ||
             gdk_content_formats_contain_mime_type(formats, "text/uri-list"))) {
            return TRUE;
        }
        return FALSE;
    }
    return (g_headless_clip_payload != NULL && g_headless_clip_payload[0] != '\0');
}

typedef struct {
    GFile *dest_dir;
    GCancellable *cancellable;
    KajoFileOpCallback callback;
    gpointer user_data;
    GInputStream *stream;
    GByteArray *buf;
    char *mime_used;
} ClipboardPasteCtx;

static void
clipboard_paste_ctx_free(ClipboardPasteCtx *pctx)
{
    if (!pctx) return;
    g_clear_object(&pctx->dest_dir);
    g_clear_object(&pctx->cancellable);
    g_clear_object(&pctx->stream);
    if (pctx->buf)
        g_byte_array_unref(pctx->buf);
    g_free(pctx->mime_used);
    g_free(pctx);
}

static void
on_clipboard_chunk_read(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ClipboardPasteCtx *pctx = user_data;
    GError *error = NULL;
    g_autoptr(GBytes) chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), res, &error);

    if (!chunk) {
        g_input_stream_close(G_INPUT_STREAM(source), NULL, NULL);
        if (pctx->callback)
            pctx->callback(FALSE, NULL, error, pctx->user_data);
        g_clear_error(&error);
        clipboard_paste_ctx_free(pctx);
        return;
    }

    gsize chunk_len = 0;
    const guint8 *chunk_data = g_bytes_get_data(chunk, &chunk_len);
    if (chunk_len > 0 && !kajo_files_clipboard_buffer_append(pctx->buf, chunk_data, chunk_len)) {
        g_input_stream_close(G_INPUT_STREAM(source), NULL, NULL);
        if (pctx->callback) {
            g_autoptr(GError) err = g_error_new_literal(
                G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                "Clipboard file list is too large to paste (over 4 MB)");
            pctx->callback(FALSE, NULL, err, pctx->user_data);
        }
        clipboard_paste_ctx_free(pctx);
        return;
    }
    if (chunk_len > 0) {
        /* Continue reading until EOF (chunk_len == 0) */
        g_input_stream_read_bytes_async(
            pctx->stream,
            CLIPBOARD_READ_CHUNK_BYTES,
            G_PRIORITY_DEFAULT,
            pctx->cancellable,
            on_clipboard_chunk_read,
            pctx
        );
        return;
    }

    g_input_stream_close(G_INPUT_STREAM(source), NULL, NULL);

    gboolean is_move = FALSE;
    g_autoptr(GPtrArray) files = NULL;
    if (!kajo_files_parse_clipboard_data(
            pctx->mime_used,
            (const char *)pctx->buf->data,
            pctx->buf->len,
            &files,
            &is_move)) {
        if (pctx->callback) {
            g_autoptr(GError) err = g_error_new_literal(
                G_IO_ERROR,
                G_IO_ERROR_NOT_FOUND,
                "No valid file URIs found in clipboard"
            );
            pctx->callback(FALSE, NULL, err, pctx->user_data);
        }
        clipboard_paste_ctx_free(pctx);
        return;
    }

    /* When pasting a Cut operation, clear the clipboard so a second Ctrl+V cannot re-move stale paths */
    if (is_move) {
        GdkDisplay *display = gdk_display_get_default();
        if (display) {
            GdkClipboard *clipboard = gdk_display_get_clipboard(display);
            gdk_clipboard_set_content(clipboard, NULL);
        }
    }

    kajo_files_op_transfer_async(files, pctx->dest_dir, is_move, pctx->cancellable, pctx->callback, pctx->user_data);
    clipboard_paste_ctx_free(pctx);
}

static void
on_clipboard_stream_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ClipboardPasteCtx *pctx = user_data;
    GError *error = NULL;
    const char *out_mime = NULL;
    GInputStream *stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), res, &out_mime, &error);
    if (!stream) {
        if (pctx->callback)
            pctx->callback(FALSE, NULL, error, pctx->user_data);
        g_clear_error(&error);
        clipboard_paste_ctx_free(pctx);
        return;
    }

    pctx->stream = stream;
    pctx->buf = g_byte_array_new();
    pctx->mime_used = g_strdup(out_mime);
    g_input_stream_read_bytes_async(
        stream,
        CLIPBOARD_READ_CHUNK_BYTES,
        G_PRIORITY_DEFAULT,
        pctx->cancellable,
        on_clipboard_chunk_read,
        pctx
    );
}

void
kajo_files_clipboard_paste_async(GFile *dest_dir,
                                 GCancellable *cancellable,
                                 KajoFileOpCallback callback,
                                 gpointer user_data)
{
    GdkDisplay *display = gdk_display_get_default();
    GdkClipboard *clipboard = display ? gdk_display_get_clipboard(display) : NULL;

    if (clipboard) {
        static const char *mimes[] = {
            "x-special/gnome-copied-files",
            "text/uri-list",
            NULL
        };
        GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
        if (formats &&
            (gdk_content_formats_contain_mime_type(formats, mimes[0]) ||
             gdk_content_formats_contain_mime_type(formats, mimes[1]))) {
            ClipboardPasteCtx *pctx = g_new0(ClipboardPasteCtx, 1);
            pctx->dest_dir = g_object_ref(dest_dir);
            if (cancellable) pctx->cancellable = g_object_ref(cancellable);
            pctx->callback = callback;
            pctx->user_data = user_data;
            gdk_clipboard_read_async(clipboard, mimes, G_PRIORITY_DEFAULT, cancellable, on_clipboard_stream_ready, pctx);
            return;
        }
    } else if (g_headless_clip_payload != NULL) {
        gboolean is_move = FALSE;
        g_autoptr(GPtrArray) files = NULL;
        if (kajo_files_parse_clipboard_data(
                "x-special/gnome-copied-files",
                g_headless_clip_payload,
                strlen(g_headless_clip_payload),
                &files,
                &is_move)) {
            if (is_move)
                g_clear_pointer(&g_headless_clip_payload, g_free);
            kajo_files_op_transfer_async(files, dest_dir, is_move, cancellable, callback, user_data);
            return;
        }
    }

    if (callback) {
        g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Clipboard is empty");
        callback(FALSE, NULL, err, user_data);
    }
}

char *
kajo_files_copy_items_paths_to_clipboard(GPtrArray *items)
{
    if (!items || items->len == 0)
        return NULL;

    GString *str = g_string_new(NULL);
    for (guint i = 0; i < items->len; i++) {
        KajoFileItem *item = g_ptr_array_index(items, i);
        if (!KAJO_IS_FILE_ITEM(item)) continue;
        GFile *file = kajo_file_item_get_file(item);
        g_autofree char *local_path = file ? g_file_get_path(file) : NULL;
        const char *text = local_path ? local_path : kajo_file_item_get_uri(item);
        if (!text) continue;
        if (str->len > 0)
            g_string_append_c(str, '\n');
        g_string_append(str, text);
    }

    if (str->len == 0) {
        g_string_free(str, TRUE);
        return NULL;
    }

    g_clear_pointer(&g_headless_clip_payload, g_free);

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        gdk_clipboard_set_text(clipboard, str->str);
    }

    return g_string_free(str, FALSE);
}

char *
kajo_files_copy_path_to_clipboard(KajoFileItem *item)
{
    if (!item) return NULL;
    g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(arr, g_object_ref(item));
    return kajo_files_copy_items_paths_to_clipboard(arr);
}

gboolean
kajo_files_toggle_executable(const char *local_path,
                             guint32 *out_new_mode,
                             GError **error)
{
    if (out_new_mode) *out_new_mode = 0;
    if (!local_path || local_path[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Executable toggle requires a local POSIX file");
        return FALSE;
    }

    GStatBuf st;
    if (g_stat(local_path, &st) != 0) {
        int errsv = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errsv), "stat failed: %s", g_strerror(errsv));
        return FALSE;
    }

    if (!S_ISREG(st.st_mode)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, "Executable toggle is only supported on regular files");
        return FALSE;
    }

    guint32 old_mode = (guint32)(st.st_mode & 07777U);
    guint32 new_mode = (old_mode & 0111U) ? (old_mode & ~0111U) : (old_mode | 0111U);

    if (g_chmod(local_path, (int)new_mode) != 0) {
        int errsv = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errsv), "chmod failed: %s", g_strerror(errsv));
        return FALSE;
    }

    if (out_new_mode)
        *out_new_mode = new_mode;
    return TRUE;
}

/* ─── Desktop App & Terminal Launchers ─── */

gboolean
kajo_files_open_item_with_app(KajoFileItem *item,
                              GAppInfo *app_info,
                              gboolean set_default,
                              GError **error)
{
    g_return_val_if_fail(KAJO_IS_FILE_ITEM(item), FALSE);
    g_return_val_if_fail(G_IS_APP_INFO(app_info), FALSE);

    const char *ctype = kajo_file_item_get_content_type(item);
    if (set_default && ctype && ctype[0] != '\0') {
        g_app_info_set_as_default_for_type(app_info, ctype, NULL);
    }

    GFile *file = kajo_file_item_get_file(item);
    const char *target_uri = kajo_file_item_get_target_uri(item);
    if (target_uri && target_uri[0] != '\0') {
        GList *uris = g_list_append(NULL, (gpointer)target_uri);
        gboolean ok = g_app_info_launch_uris(app_info, uris, NULL, error);
        g_list_free(uris);
        return ok;
    }

    if (file) {
        GList *files = g_list_append(NULL, file);
        gboolean ok = g_app_info_launch(app_info, files, NULL, error);
        g_list_free(files);
        return ok;
    }

    const char *uri = kajo_file_item_get_uri(item);
    if (uri) {
        GList *uris = g_list_append(NULL, (gpointer)uri);
        gboolean ok = g_app_info_launch_uris(app_info, uris, NULL, error);
        g_list_free(uris);
        return ok;
    }

    return FALSE;
}

void
kajo_files_open_item_default(KajoFileItem *item, GtkWindow *parent G_GNUC_UNUSED)
{
    if (!item) return;
    const char *uri = kajo_file_item_get_target_uri(item);
    if (!uri) uri = kajo_file_item_get_uri(item);
    if (!uri) return;

    g_autoptr(GError) err = NULL;
    if (g_app_info_launch_default_for_uri(uri, NULL, &err))
        return;

    const char *ctype = kajo_file_item_get_content_type(item);
    GFile *file = kajo_file_item_get_file(item);
    if (ctype && file) {
        g_autoptr(GAppInfo) app = g_app_info_get_default_for_type(ctype, FALSE);
        if (app) {
            GList *files = g_list_append(NULL, file);
            gboolean ok = g_app_info_launch(app, files, NULL, NULL);
            g_list_free(files);
            if (ok) return;
        }
    }

    g_autofree char *xdg_open = g_find_program_in_path("xdg-open");
    if (xdg_open) {
        g_autofree char *local_path = file ? g_file_get_path(file) : NULL;
        char *argv[] = { xdg_open, (char *)(local_path ? local_path : uri), NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
}

void
kajo_files_open_terminal_at(GFile *dir)
{
    if (!dir) return;
    g_autofree char *path = g_file_get_path(dir);
    if (!path) {
        g_autofree char *uri = g_file_get_uri(dir);
        g_autofree char *arch_path = NULL;
        if (uri && kajo_files_parse_archive_uri(uri, &arch_path, NULL)) {
            path = g_path_get_dirname(arch_path);
        }
    }
    if (!path)
        path = g_strdup(g_get_home_dir());

    const char *env_term = g_getenv("TERMINAL");
    const char *candidates[] = {
        env_term,
        "kitty",
        "foot",
        "alacritty",
        "wezterm",
        "gnome-terminal",
        "xterm",
        NULL
    };

    for (int i = 0; i < 7; i++) {
        const char *bin = candidates[i];
        if (!bin || bin[0] == '\0') continue;
        g_autofree char *resolved = g_find_program_in_path(bin);
        if (resolved) {
            char *argv[] = { resolved, NULL };
            g_spawn_async(
                path,
                argv,
                NULL,
                G_SPAWN_SEARCH_PATH,
                NULL,
                NULL,
                NULL,
                NULL
            );
            return;
        }
    }
}
