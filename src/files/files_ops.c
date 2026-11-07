#include "files_ops.h"
#include <archive.h>
#include <archive_entry.h>
#include <string.h>

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

static gboolean
is_safe_relative_archive_path(const char *pathname)
{
    if (!pathname || pathname[0] == '\0' || pathname[0] == '/')
        return FALSE;

    g_auto(GStrv) parts = g_strsplit(pathname, "/", -1);
    for (int i = 0; parts && parts[i] != NULL; i++) {
        if (g_strcmp0(parts[i], "..") == 0)
            return FALSE;
    }
    return TRUE;
}

static gboolean
extract_archive_sync(GFile *archive_file,
                     GFile *dest_parent_dir,
                     GCancellable *cancellable,
                     char **out_folder_name,
                     guint *out_count,
                     GError **error)
{
    g_autofree char *archive_path = g_file_get_path(archive_file);
    if (!archive_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Archive must be on a local or FUSE-mounted filesystem");
        return FALSE;
    }

    g_autofree char *base = g_file_get_basename(archive_file);
    g_autofree char *stem = strip_archive_stem(base);
    g_autoptr(GFile) target_dir = resolve_unique_dest_child(dest_parent_dir, stem);
    g_autofree char *target_root = g_file_get_path(target_dir);
    if (!target_root) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Destination directory must have a local path");
        return FALSE;
    }

    if (g_mkdir_with_parents(target_root, 0755) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create extraction folder '%s'", target_root);
        return FALSE;
    }

    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(
        ext,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS
    );
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, archive_path, 32768) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Archive open failed: %s", archive_error_string(a));
        archive_read_free(a);
        archive_write_free(ext);
        return FALSE;
    }

    guint extracted = 0;
    struct archive_entry *entry = NULL;

    while (TRUE) {
        if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
            archive_read_free(a);
            archive_write_free(ext);
            return FALSE;
        }

        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_OK && r != ARCHIVE_WARN) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Archive read error: %s", archive_error_string(a));
            archive_read_free(a);
            archive_write_free(ext);
            return FALSE;
        }

        const char *rel_path = archive_entry_pathname(entry);
        if (!is_safe_relative_archive_path(rel_path))
            continue;

        g_autofree char *full_dest = g_build_filename(target_root, rel_path, NULL);
        archive_entry_set_pathname(entry, full_dest);

        r = archive_write_header(ext, entry);
        if (r >= ARCHIVE_WARN && archive_entry_size(entry) > 0) {
            const void *buff = NULL;
            size_t size = 0;
            la_int64_t offset = 0;
            while (TRUE) {
                int dr = archive_read_data_block(a, &buff, &size, &offset);
                if (dr == ARCHIVE_EOF)
                    break;
                if (dr < ARCHIVE_OK) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Archive data error: %s", archive_error_string(a));
                    archive_read_free(a);
                    archive_write_free(ext);
                    return FALSE;
                }
                if (archive_write_data_block(ext, buff, size, offset) < ARCHIVE_WARN) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Archive write error: %s", archive_error_string(ext));
                    archive_read_free(a);
                    archive_write_free(ext);
                    return FALSE;
                }
            }
        }
        archive_write_finish_entry(ext);
        extracted++;
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);

    if (out_folder_name)
        *out_folder_name = g_file_get_basename(target_dir);
    if (out_count)
        *out_count = extracted;
    return TRUE;
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
resolve_unique_dest_child(GFile *dest_dir, const char *base_name)
{
    GFile *candidate = g_file_get_child(dest_dir, base_name);
    if (!g_file_query_exists(candidate, NULL))
        return candidate;

    g_object_unref(candidate);

    const char *dot = strrchr(base_name, '.');
    g_autofree char *stem = NULL;
    const char *ext = "";
    if (dot && dot != base_name) {
        stem = g_strndup(base_name, (gsize)(dot - base_name));
        ext = dot;
    } else {
        stem = g_strdup(base_name);
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

typedef enum {
    BATCH_OP_TRASH,
    BATCH_OP_DELETE_PERM,
    BATCH_OP_EMPTY_TRASH,
    BATCH_OP_RESTORE_TRASH,
    BATCH_OP_TRANSFER,
    BATCH_OP_EXTRACT_ARCHIVE,
} BatchOpKind;

typedef struct {
    BatchOpKind kind;
    GPtrArray *files;     /* GFile* array */
    GFile *dest_dir;      /* for transfer / restore */
    char *restore_path;   /* for restore */
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
    g_clear_object(&ctx->dest_dir);
    g_free(ctx->restore_path);
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
        if (!ctx->files || ctx->files->len == 0) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No trash item specified");
            return;
        }
        GFile *trash_file = g_ptr_array_index(ctx->files, 0);
        g_autoptr(GFile) target = g_file_new_for_path(ctx->restore_path);
        g_autoptr(GFile) parent = g_file_get_parent(target);
        if (parent) {
            g_autofree char *parent_path = g_file_get_path(parent);
            if (parent_path)
                g_mkdir_with_parents(parent_path, 0755);
        }
        if (!g_file_move(trash_file, target, G_FILE_COPY_OVERWRITE, cancellable, NULL, NULL, &error)) {
            g_task_return_error(task, error);
            return;
        }
        g_task_return_pointer(task, g_strdup_printf("Restored to %s", ctx->restore_path), g_free);
        return;
    }

    case BATCH_OP_TRANSFER:
        for (guint i = 0; i < count; i++) {
            GFile *src = g_ptr_array_index(ctx->files, i);
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
        if (!extract_archive_sync(archive_file, ctx->dest_dir, cancellable, &folder_name, &extracted, &error)) {
            g_task_return_error(task, error);
            return;
        }
        g_task_return_pointer(
            task,
            g_strdup_printf("Extracted %u entries into '%s/'", extracted, folder_name ? folder_name : "folder"),
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
kajo_files_op_restore_from_trash_async(KajoFileItem *item,
                                       GCancellable *cancellable,
                                       KajoFileOpCallback callback,
                                       gpointer user_data)
{
    g_return_if_fail(KAJO_IS_FILE_ITEM(item));
    const char *orig_path = kajo_file_item_get_trash_orig_path(item);
    if (!orig_path || orig_path[0] == '\0') {
        if (callback) {
            g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Original path metadata unavailable in Trash");
            callback(FALSE, NULL, err, user_data);
        }
        return;
    }

    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_RESTORE_TRASH;
    ctx->files = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(ctx->files, g_object_ref(kajo_file_item_get_file(item)));
    ctx->restore_path = g_strdup(orig_path);
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
    g_return_if_fail(G_IS_FILE(dest_parent_dir));

    BatchOpContext *ctx = g_new0(BatchOpContext, 1);
    ctx->kind = BATCH_OP_EXTRACT_ARCHIVE;
    ctx->files = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(ctx->files, g_object_ref(archive_file));
    ctx->dest_dir = g_object_ref(dest_parent_dir);
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_batch_task_done, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, batch_op_worker);
}

/* ─── Clipboard (Internal + Wayland GdkClipboard) ─── */

static GPtrArray *g_clip_files = NULL;
static gboolean g_clip_is_cut = FALSE;

void
kajo_files_clipboard_set(GPtrArray *files, gboolean is_cut)
{
    if (g_clip_files) {
        g_ptr_array_unref(g_clip_files);
        g_clip_files = NULL;
    }

    g_clip_files = clone_gfile_array(files);
    g_clip_is_cut = is_cut;

    GdkDisplay *display = gdk_display_get_default();
    if (display && g_clip_files && g_clip_files->len > 0) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        GString *uri_list = g_string_new(NULL);
        GString *gnome_list = g_string_new(is_cut ? "cut\n" : "copy\n");
        for (guint i = 0; i < g_clip_files->len; i++) {
            GFile *f = g_ptr_array_index(g_clip_files, i);
            g_autofree char *uri = g_file_get_uri(f);
            if (uri) {
                g_string_append(uri_list, uri);
                g_string_append(uri_list, "\r\n");
                g_string_append(gnome_list, uri);
                g_string_append(gnome_list, "\n");
            }
        }
        g_autoptr(GBytes) uri_bytes = g_string_free_to_bytes(uri_list);
        g_autoptr(GBytes) gnome_bytes = g_string_free_to_bytes(gnome_list);
        GdkContentProvider *providers[2] = {
            gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", gnome_bytes),
            gdk_content_provider_new_for_bytes("text/uri-list", uri_bytes),
        };
        g_autoptr(GdkContentProvider) union_prov = gdk_content_provider_new_union(providers, 2);
        gdk_clipboard_set_content(clipboard, union_prov);
    }
}

gboolean
kajo_files_clipboard_has_files(void)
{
    if (g_clip_files != NULL && g_clip_files->len > 0)
        return TRUE;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
        if (formats &&
            (gdk_content_formats_contain_mime_type(formats, "x-special/gnome-copied-files") ||
             gdk_content_formats_contain_mime_type(formats, "text/uri-list"))) {
            return TRUE;
        }
    }
    return FALSE;
}

typedef struct {
    GFile *dest_dir;
    GCancellable *cancellable;
    KajoFileOpCallback callback;
    gpointer user_data;
    GInputStream *stream;
    char *mime_used;
} ClipboardPasteCtx;

static void
clipboard_paste_ctx_free(ClipboardPasteCtx *pctx)
{
    if (!pctx) return;
    g_clear_object(&pctx->dest_dir);
    g_clear_object(&pctx->cancellable);
    g_clear_object(&pctx->stream);
    g_free(pctx->mime_used);
    g_free(pctx);
}

static void
on_clipboard_bytes_read(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ClipboardPasteCtx *pctx = user_data;
    GError *error = NULL;
    g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), res, &error);
    g_input_stream_close(G_INPUT_STREAM(source), NULL, NULL);

    if (!bytes) {
        if (pctx->callback)
            pctx->callback(FALSE, NULL, error, pctx->user_data);
        g_clear_error(&error);
        clipboard_paste_ctx_free(pctx);
        return;
    }

    gsize len = 0;
    const char *raw = g_bytes_get_data(bytes, &len);
    g_autofree char *text = raw ? g_strndup(raw, len) : g_strdup("");
    g_auto(GStrv) lines = g_strsplit_set(text, "\r\n", -1);

    gboolean is_move = FALSE;
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_object_unref);

    for (int i = 0; lines && lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (i == 0 && (g_strcmp0(line, "cut") == 0 || g_strcmp0(line, "copy") == 0)) {
            is_move = (g_strcmp0(line, "cut") == 0);
            continue;
        }
        if (strstr(line, "://") != NULL) {
            g_ptr_array_add(files, g_file_new_for_uri(line));
        } else if (line[0] == '/') {
            g_ptr_array_add(files, g_file_new_for_path(line));
        }
    }

    if (files->len == 0) {
        if (pctx->callback) {
            g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No file URIs found in clipboard");
            pctx->callback(FALSE, NULL, err, pctx->user_data);
        }
        clipboard_paste_ctx_free(pctx);
        return;
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
    pctx->mime_used = g_strdup(out_mime);
    g_input_stream_read_bytes_async(
        stream,
        256 * 1024,
        G_PRIORITY_DEFAULT,
        pctx->cancellable,
        on_clipboard_bytes_read,
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

    if (g_clip_files && g_clip_files->len > 0 && (!clipboard || gdk_clipboard_is_local(clipboard))) {
        gboolean is_move = g_clip_is_cut;
        kajo_files_op_transfer_async(g_clip_files, dest_dir, is_move, cancellable, callback, user_data);
        if (is_move) {
            g_ptr_array_unref(g_clip_files);
            g_clip_files = NULL;
            g_clip_is_cut = FALSE;
        }
        return;
    }

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
    }

    if (callback) {
        g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Clipboard is empty");
        callback(FALSE, NULL, err, user_data);
    }
}

char *
kajo_files_copy_path_to_clipboard(KajoFileItem *item)
{
    if (!item) return NULL;

    GFile *file = kajo_file_item_get_file(item);
    g_autofree char *local_path = file ? g_file_get_path(file) : NULL;
    const char *text = local_path ? local_path : kajo_file_item_get_uri(item);
    if (!text) return NULL;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        gdk_clipboard_set_text(clipboard, text);
    }

    return g_strdup(text);
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
