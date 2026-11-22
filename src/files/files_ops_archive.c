#define _DEFAULT_SOURCE
#include "files_ops_private.h"
#include <archive.h>
#include <archive_entry.h>
#include <utime.h>

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
char *
files_ops_strip_archive_stem_dup(const char *path)
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
                            const KajoArchiveExtractOpts *opts,
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
                         const KajoArchiveExtractOpts *opts,
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
        files_ops_delete_file_recursive_sync(f, NULL, NULL);
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
                                       : files_ops_strip_archive_stem_dup(archive_path);
    g_autoptr(GFile) target_item = files_ops_resolve_unique_dest_child(effective_parent, name_src);
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
    KajoArchiveExtractOpts opts = { .regular_only = FALSE, .strip_exec = FALSE };
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

gboolean
files_ops_extract_archive_subpath_sync(const char *archive_path,
                                       const char *inner_path,
                                       GFile *dest_target,
                                       const KajoArchiveExtractOpts *opts,
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

/* ─── Archive compression engine ─── */

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
    g_autoptr(GFile) out_file = files_ops_resolve_unique_dest_child(dest_parent_dir, norm_name);
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

/* ─── Async Archive Extract & Compress Tasks ─── */

typedef struct {
    GPtrArray *files;
    GFile *dest_dir;
    char *archive_name;
    KajoFileOpCallback callback;
    gpointer user_data;
} ArchiveOpContext;

static void
archive_op_context_free(ArchiveOpContext *ctx)
{
    if (!ctx) return;
    if (ctx->files)
        g_ptr_array_unref(ctx->files);
    g_clear_object(&ctx->dest_dir);
    g_free(ctx->archive_name);
    g_free(ctx);
}

static void
on_archive_op_task_done(GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    ArchiveOpContext *ctx = user_data;
    GError *error = NULL;
    g_autofree char *msg = g_task_propagate_pointer(G_TASK(res), &error);

    if (ctx->callback)
        ctx->callback(msg != NULL, msg, error, ctx->user_data);

    g_clear_error(&error);
    archive_op_context_free(ctx);
}

static void
extract_archive_worker(GTask *task,
                       gpointer source_object G_GNUC_UNUSED,
                       gpointer task_data,
                       GCancellable *cancellable)
{
    ArchiveOpContext *ctx = task_data;
    GError *error = NULL;

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
}

static void
compress_archive_worker(GTask *task,
                        gpointer source_object G_GNUC_UNUSED,
                        gpointer task_data,
                        GCancellable *cancellable)
{
    ArchiveOpContext *ctx = task_data;
    GError *error = NULL;
    g_autofree char *created_name = NULL;
    guint compressed_count = 0;

    if (!compress_archive_sync(ctx->files, ctx->dest_dir, ctx->archive_name, cancellable,
                               &created_name, &compressed_count, &error)) {
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(
        task,
        g_strdup_printf("Compressed %u item(s) into '%s'", compressed_count,
                        created_name ? created_name : "archive"),
        g_free
    );
}

void
kajo_files_op_extract_archive_async(GFile *archive_file,
                                    GFile *dest_parent_dir,
                                    GCancellable *cancellable,
                                    KajoFileOpCallback callback,
                                    gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(archive_file));

    ArchiveOpContext *ctx = g_new0(ArchiveOpContext, 1);
    ctx->files = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(ctx->files, g_object_ref(archive_file));
    if (dest_parent_dir)
        ctx->dest_dir = g_object_ref(dest_parent_dir);
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_archive_op_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, extract_archive_worker);
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

    ArchiveOpContext *ctx = g_new0(ArchiveOpContext, 1);
    ctx->files = files_ops_clone_gfile_array(source_files);
    ctx->dest_dir = g_object_ref(dest_parent_dir);
    ctx->archive_name = g_strdup((archive_filename && archive_filename[0] != '\0') ? archive_filename : "archive.zip");
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_archive_op_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, compress_archive_worker);
}

/* ─── Virtual archive:// Entry Preview Cache Extraction ─── */

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
            files_ops_delete_file_recursive_sync(f, NULL, NULL);
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
        const KajoArchiveExtractOpts opts = { .regular_only = TRUE, .strip_exec = TRUE };
        if (!files_ops_extract_archive_subpath_sync(arch_path, inner_path, part_file, &opts, cancellable, &error)) {
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
