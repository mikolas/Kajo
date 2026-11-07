#define _DEFAULT_SOURCE
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <stdlib.h>
#include "../src/files/files_model.h"
#include "../src/files/files_ops.h"

static void
test_format_size(void)
{
    g_autofree char *s_dir = kajo_files_format_size(4096, TRUE);
    g_assert_cmpstr(s_dir, ==, "[ FOLDER ]");

    g_autofree char *s_b = kajo_files_format_size(512, FALSE);
    g_assert_cmpstr(s_b, ==, "512 B");

    g_autofree char *s_kb = kajo_files_format_size(1536, FALSE);
    g_assert_cmpstr(s_kb, ==, "1.5 KB");

    g_autofree char *s_mb = kajo_files_format_size(5ULL * 1024ULL * 1024ULL, FALSE);
    g_assert_cmpstr(s_mb, ==, "5.0 MB");

    g_autofree char *s_gb = kajo_files_format_size(2ULL * 1024ULL * 1024ULL * 1024ULL, FALSE);
    g_assert_cmpstr(s_gb, ==, "2.00 GB");
}

static void
test_format_permissions(void)
{
    g_autofree char *p_file = kajo_files_format_permissions(0644, FALSE, FALSE);
    g_assert_cmpstr(p_file, ==, "-rw-r--r-- (0644)");

    g_autofree char *p_dir = kajo_files_format_permissions(0755, TRUE, FALSE);
    g_assert_cmpstr(p_dir, ==, "drwxr-xr-x (0755)");

    g_autofree char *p_sym = kajo_files_format_permissions(0777, FALSE, TRUE);
    g_assert_cmpstr(p_sym, ==, "lrwxrwxrwx (0777)");
}

static void
test_sorting_and_filtering(void)
{
    g_autoptr(KajoFileItem) dir_b = kajo_file_item_new_manual(
        NULL, "src", "src", "inode/directory", 4096, 1000, 0755, TRUE, FALSE
    );
    g_autoptr(KajoFileItem) dir_a = kajo_file_item_new_manual(
        NULL, "build", "build", "inode/directory", 4096, 2000, 0755, TRUE, FALSE
    );
    g_autoptr(KajoFileItem) file_z = kajo_file_item_new_manual(
        NULL, "zeta.c", "zeta.c", "text/x-csrc", 2048, 3000, 0644, FALSE, FALSE
    );
    g_autoptr(KajoFileItem) file_a = kajo_file_item_new_manual(
        NULL, "alpha.md", "alpha.md", "text/markdown", 16384, 500, 0644, FALSE, FALSE
    );
    g_autoptr(KajoFileItem) file_hidden = kajo_file_item_new_manual(
        NULL, ".gitignore", ".gitignore", "text/plain", 128, 4000, 0644, FALSE, TRUE
    );

    /* Directories always sort before files regardless of field/direction */
    g_assert_cmpint(kajo_file_item_compare(dir_b, file_a, KAJO_SORT_NAME, TRUE), <, 0);
    g_assert_cmpint(kajo_file_item_compare(dir_b, file_a, KAJO_SORT_SIZE, FALSE), <, 0);

    /* Directory name comparison */
    g_assert_cmpint(kajo_file_item_compare(dir_a, dir_b, KAJO_SORT_NAME, TRUE), <, 0);
    g_assert_cmpint(kajo_file_item_compare(dir_a, dir_b, KAJO_SORT_NAME, FALSE), >, 0);

    /* File size comparison */
    g_assert_cmpint(kajo_file_item_compare(file_z, file_a, KAJO_SORT_SIZE, TRUE), <, 0);
    g_assert_cmpint(kajo_file_item_compare(file_z, file_a, KAJO_SORT_SIZE, FALSE), >, 0);

    /* File mtime comparison */
    g_assert_cmpint(kajo_file_item_compare(file_a, file_z, KAJO_SORT_MODIFIED, TRUE), <, 0);

    /* Hidden & search filter */
    g_assert_false(kajo_file_item_matches_filter(file_hidden, FALSE, NULL));
    g_assert_true(kajo_file_item_matches_filter(file_hidden, TRUE, NULL));
    g_assert_true(kajo_file_item_matches_filter(file_a, FALSE, "ALPHA"));
    g_assert_false(kajo_file_item_matches_filter(file_a, FALSE, "nonexistent"));

    /* Text / code detector */
    g_assert_true(kajo_files_is_text_or_code("text/markdown", "SPEC.md"));
    g_assert_true(kajo_files_is_text_or_code("application/octet-stream", "meson.build"));
    g_assert_false(kajo_files_is_text_or_code("image/png", "photo.png"));

    /* Archive detector */
    g_assert_true(kajo_files_is_archive("application/zip", "release.zip"));
    g_assert_true(kajo_files_is_archive("application/octet-stream", "source.tar.zst"));
    g_assert_false(kajo_files_is_archive("video/x-matroska", "movie.mkv"));

    /* Default sort direction per field */
    g_assert_true(kajo_file_sort_field_default_ascending(KAJO_SORT_NAME));
    g_assert_true(kajo_file_sort_field_default_ascending(KAJO_SORT_TYPE));
    g_assert_false(kajo_file_sort_field_default_ascending(KAJO_SORT_SIZE));
    g_assert_false(kajo_file_sort_field_default_ascending(KAJO_SORT_MODIFIED));

    /* Natural filename collation (Episode_2 < Episode_10 < Episode_100) */
    g_autoptr(KajoFileItem) ep2 = kajo_file_item_new_manual(
        NULL, "Episode_2.mkv", "Episode_2.mkv", "video/x-matroska", 1024, 100, 0644, FALSE, FALSE
    );
    g_autoptr(KajoFileItem) ep10 = kajo_file_item_new_manual(
        NULL, "Episode_10.mkv", "Episode_10.mkv", "video/x-matroska", 1024, 100, 0644, FALSE, FALSE
    );
    g_autoptr(KajoFileItem) ep100 = kajo_file_item_new_manual(
        NULL, "Episode_100.mkv", "Episode_100.mkv", "video/x-matroska", 1024, 100, 0644, FALSE, FALSE
    );
    g_assert_cmpint(kajo_file_item_compare_full(ep2, ep10, KAJO_SORT_NAME, TRUE, TRUE), <, 0);
    g_assert_cmpint(kajo_file_item_compare_full(ep10, ep100, KAJO_SORT_NAME, TRUE, TRUE), <, 0);
    g_assert_cmpint(kajo_file_item_compare_full(ep2, ep10, KAJO_SORT_NAME, FALSE, TRUE), >, 0);

    /* Tie-breaking on descending size/mtime sort remains natural A->Z */
    g_assert_cmpint(kajo_file_item_compare_full(ep2, ep10, KAJO_SORT_SIZE, FALSE, TRUE), <, 0);

    /* Folders First toggle: when FALSE, folders and files interleave by field */
    g_assert_cmpint(kajo_file_item_compare_full(file_a, dir_b, KAJO_SORT_NAME, TRUE, FALSE), <, 0);
    g_assert_cmpint(kajo_file_item_compare_full(file_a, dir_b, KAJO_SORT_NAME, TRUE, TRUE), >, 0);

    /* Sort field & view mode string serialization roundtrip */
    g_assert_cmpstr(kajo_file_sort_field_to_string(KAJO_SORT_NAME), ==, "name");
    g_assert_cmpstr(kajo_file_sort_field_to_string(KAJO_SORT_SIZE), ==, "size");
    g_assert_cmpstr(kajo_file_sort_field_to_string(KAJO_SORT_MODIFIED), ==, "modified");
    g_assert_cmpstr(kajo_file_sort_field_to_string(KAJO_SORT_TYPE), ==, "type");
    g_assert_cmpint(kajo_file_sort_field_from_string("size"), ==, KAJO_SORT_SIZE);
    g_assert_cmpint(kajo_file_sort_field_from_string("modified"), ==, KAJO_SORT_MODIFIED);
    g_assert_cmpint(kajo_file_sort_field_from_string("type"), ==, KAJO_SORT_TYPE);
    g_assert_cmpint(kajo_file_sort_field_from_string("unknown"), ==, KAJO_SORT_NAME);

    g_assert_cmpstr(kajo_file_view_mode_to_string(KAJO_VIEW_GRID), ==, "grid");
    g_assert_cmpstr(kajo_file_view_mode_to_string(KAJO_VIEW_LIST), ==, "list");
    g_assert_cmpint(kajo_file_view_mode_from_string("list"), ==, KAJO_VIEW_LIST);
    g_assert_cmpint(kajo_file_view_mode_from_string("grid"), ==, KAJO_VIEW_GRID);
    g_assert_cmpint(kajo_file_view_mode_from_string(NULL), ==, KAJO_VIEW_GRID);
}

typedef struct {
    GMainLoop *loop;
    GPtrArray *items;
    gboolean op_ok;
    gboolean op_cancelled;
} AsyncTestState;

static void
on_test_enum_done(GPtrArray *items, GError *error, gpointer user_data)
{
    AsyncTestState *st = user_data;
    g_assert_no_error(error);
    st->items = items;
    g_main_loop_quit(st->loop);
}

static void
on_test_op_done(gboolean success, const char *message G_GNUC_UNUSED, GError *error, gpointer user_data)
{
    AsyncTestState *st = user_data;
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        st->op_cancelled = TRUE;
        st->op_ok = FALSE;
        g_main_loop_quit(st->loop);
        return;
    }
    g_assert_no_error(error);
    st->op_ok = success;
    g_main_loop_quit(st->loop);
}

static void
test_async_enum_and_ops(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *tmp_path = g_dir_make_tmp("kajo-files-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_autoptr(GFile) tmp_dir = g_file_new_for_path(tmp_path);

    AsyncTestState st = { 0 };
    st.loop = g_main_loop_new(NULL, FALSE);

    /* 1. Create folder via kajo_files_op_mkdir_async */
    kajo_files_op_mkdir_async(tmp_dir, "subdir", NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    /* 2. Create a sample file */
    g_autofree char *hello_path = g_build_filename(tmp_path, "hello.txt", NULL);
    g_assert_true(g_file_set_contents(hello_path, "Hello Kajo Files!\n", -1, &err));
    g_assert_no_error(err);

    /* 3. Enumerate tmp_dir */
    kajo_files_enumerate_async(tmp_dir, NULL, on_test_enum_done, &st);
    g_main_loop_run(st.loop);
    g_assert_nonnull(st.items);
    g_assert_cmpuint(st.items->len, ==, 2);
    g_ptr_array_unref(st.items);
    st.items = NULL;

    /* 4. Rename hello.txt -> renamed.txt */
    g_autoptr(GFile) hello_file = g_file_new_for_path(hello_path);
    kajo_files_op_rename_async(hello_file, "renamed.txt", NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    /* 5. Create empty file via kajo_files_op_create_file_async */
    kajo_files_op_create_file_async(tmp_dir, "created_empty.txt", NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);
    g_autoptr(GFile) created_file = g_file_get_child(tmp_dir, "created_empty.txt");
    g_assert_true(g_file_query_exists(created_file, NULL));

    /* 6. Verify non-clobbering move: moving a file into a directory that already has a file of the same name */
    g_autoptr(GFile) subdir_file = g_file_get_child(tmp_dir, "subdir");
    g_autofree char *sub_collision_path = g_build_filename(tmp_path, "subdir", "created_empty.txt", NULL);
    g_assert_true(g_file_set_contents(sub_collision_path, "Existing content\n", -1, &err));
    g_assert_no_error(err);

    g_autoptr(GPtrArray) move_arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(move_arr, g_object_ref(created_file));
    kajo_files_op_transfer_async(move_arr, subdir_file, TRUE, NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    /* Both the original in subdir and the renamed moved file must exist */
    g_autoptr(GFile) sub_orig = g_file_new_for_path(sub_collision_path);
    g_autoptr(GFile) sub_moved = g_file_get_child(subdir_file, "created_empty (copy).txt");
    g_assert_true(g_file_query_exists(sub_orig, NULL));
    g_assert_true(g_file_query_exists(sub_moved, NULL));

    /* 6b. Verify GCancellable cancellation of transfer op */
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    g_cancellable_cancel(cancel);
    st.op_cancelled = FALSE;
    g_autoptr(GPtrArray) cancel_arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(cancel_arr, g_object_ref(sub_orig));
    kajo_files_op_transfer_async(cancel_arr, tmp_dir, FALSE, cancel, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_cancelled);

    /* 7. Clean up tmp_dir recursively via kajo_files_op_delete_permanent_async */
    g_autoptr(GPtrArray) del_arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(del_arr, g_object_ref(tmp_dir));
    kajo_files_op_delete_permanent_async(del_arr, NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);
    g_assert_false(g_file_query_exists(tmp_dir, NULL));

    g_main_loop_unref(st.loop);
}

static gint
bench_sort_cb(gconstpointer a, gconstpointer b)
{
    const KajoFileItem *ia = *(const KajoFileItem *const *)a;
    const KajoFileItem *ib = *(const KajoFileItem *const *)b;
    return kajo_file_item_compare_full(ia, ib, KAJO_SORT_NAME, TRUE, TRUE);
}

static void
test_large_directory_scale(void)
{
    /* Verify that sorting + filtering 5,000 items completes in well under 200ms */
    gint64 t0 = g_get_monotonic_time();
    g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);

    for (int i = 0; i < 5000; i++) {
        g_autofree char *nm = g_strdup_printf("asset_texture_%04d.png", 5000 - i);
        KajoFileItem *it = kajo_file_item_new_manual(
            NULL, nm, nm, "image/png", (guint64)(i * 1024), 1700000000 + i, 0644, (i % 50 == 0), FALSE
        );
        g_ptr_array_add(items, it);
    }

    g_ptr_array_sort(items, bench_sort_cb);

    guint matched = 0;
    for (guint i = 0; i < items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(items, i);
        if (kajo_file_item_matches_filter(it, FALSE, "0042"))
            matched++;
    }
    g_assert_cmpuint(matched, ==, 1);

    gint64 elapsed_ms = (g_get_monotonic_time() - t0) / 1000;
    g_assert_cmpint(elapsed_ms, <, 500);
}

static void
test_breadcrumb_chain_and_selection_restore(void)
{
    /* 1. Full path chain from '/' through '$HOME' to leaf directory */
    const char *home = g_get_home_dir();
    g_autofree char *deep_path = g_build_filename(home, "projects", "kajo", "src", NULL);
    g_autoptr(GFile) deep_dir = g_file_new_for_path(deep_path);
    g_autoptr(GPtrArray) chain = kajo_files_build_breadcrumb_chain(deep_dir);
    g_assert_nonnull(chain);
    g_assert_cmpuint(chain->len, >=, 4);

    const KajoBreadcrumbSegment *root_seg = g_ptr_array_index(chain, 0);
    g_assert_cmpstr(root_seg->label, ==, "/");
    g_assert_cmpstr(root_seg->uri, ==, "file:///");

    const KajoBreadcrumbSegment *leaf_seg = g_ptr_array_index(chain, chain->len - 1);
    g_assert_cmpstr(leaf_seg->label, ==, "src");

    gboolean found_home_badge = FALSE;
    for (guint i = 0; i < chain->len; i++) {
        const KajoBreadcrumbSegment *s = g_ptr_array_index(chain, i);
        if (g_str_has_prefix(s->label, "⌂ ")) {
            found_home_badge = TRUE;
            break;
        }
    }
    g_assert_true(found_home_badge);

    /* Virtual URI chain */
    g_autoptr(GFile) trash_dir = g_file_new_for_uri("trash:///");
    g_autoptr(GPtrArray) trash_chain = kajo_files_build_breadcrumb_chain(trash_dir);
    g_assert_cmpuint(trash_chain->len, ==, 1);
    const KajoBreadcrumbSegment *tseg = g_ptr_array_index(trash_chain, 0);
    g_assert_cmpstr(tseg->label, ==, "🗑 TRASH");

    /* 2. Multi-selection URI matching across re-enumeration (newly allocated KajoFileItem instances) */
    g_autoptr(GFile) fa = g_file_new_for_path("/tmp/alpha.txt");
    g_autoptr(GFile) fb = g_file_new_for_path("/tmp/bravo.txt");
    g_autoptr(GFile) fc = g_file_new_for_path("/tmp/charlie.txt");
    g_autoptr(GFile) fd = g_file_new_for_path("/tmp/delta.txt");

    g_autoptr(GPtrArray) new_visible = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(new_visible, kajo_file_item_new_manual(fa, "alpha.txt", "alpha.txt", "text/plain", 10, 1, 0644, FALSE, FALSE));
    g_ptr_array_add(new_visible, kajo_file_item_new_manual(fb, "bravo.txt", "bravo.txt", "text/plain", 20, 2, 0644, FALSE, FALSE));
    g_ptr_array_add(new_visible, kajo_file_item_new_manual(fc, "charlie.txt", "charlie.txt", "text/plain", 30, 3, 0644, FALSE, FALSE));
    g_ptr_array_add(new_visible, kajo_file_item_new_manual(fd, "delta.txt", "delta.txt", "text/plain", 40, 4, 0644, FALSE, FALSE));

    g_autoptr(GPtrArray) prev_uris = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(prev_uris, g_strdup("file:///tmp/bravo.txt"));
    g_ptr_array_add(prev_uris, g_strdup("file:///tmp/delta.txt"));
    g_ptr_array_add(prev_uris, g_strdup("file:///tmp/deleted_file.txt"));

    g_autoptr(GtkBitset) matched = kajo_files_match_selection_uris(new_visible, prev_uris);
    g_assert_nonnull(matched);
    g_assert_cmpuint(gtk_bitset_get_size(matched), ==, 2);
    g_assert_false(gtk_bitset_contains(matched, 0));
    g_assert_true(gtk_bitset_contains(matched, 1));
    g_assert_false(gtk_bitset_contains(matched, 2));
    g_assert_true(gtk_bitset_contains(matched, 3));
}

static void
test_clipboard_buffer_cap_never_truncates(void)
{
    g_autoptr(GByteArray) buf = g_byte_array_new();
    guint8 chunk[64 * 1024];
    memset(chunk, 'a', sizeof(chunk));

    /* 4 MB fits exactly (64 chunks of 64 KB), one more byte must be refused untouched */
    for (int i = 0; i < 64; i++)
        g_assert_true(kajo_files_clipboard_buffer_append(buf, chunk, sizeof(chunk)));
    g_assert_cmpuint(buf->len, ==, 4u * 1024u * 1024u);
    g_assert_false(kajo_files_clipboard_buffer_append(buf, chunk, 1));
    g_assert_cmpuint(buf->len, ==, 4u * 1024u * 1024u);

    /* empty chunk = EOF marker, always accepted */
    g_assert_true(kajo_files_clipboard_buffer_append(buf, chunk, 0));
}

static void
test_rebuild_selection_policy(void)
{
    /* previously selected items still present: always restore */
    g_assert_cmpint(kajo_files_rebuild_selection_action(TRUE, FALSE, FALSE), ==, KAJO_REBUILD_SEL_RESTORE);
    g_assert_cmpint(kajo_files_rebuild_selection_action(TRUE, TRUE, TRUE), ==, KAJO_REBUILD_SEL_RESTORE);
    /* new folder or active search: land on the first entry */
    g_assert_cmpint(kajo_files_rebuild_selection_action(FALSE, TRUE, FALSE), ==, KAJO_REBUILD_SEL_FIRST);
    g_assert_cmpint(kajo_files_rebuild_selection_action(FALSE, FALSE, TRUE), ==, KAJO_REBUILD_SEL_FIRST);
    /* monitor event / sort / hidden toggle / op reload with a cleared selection: stay cleared */
    g_assert_cmpint(kajo_files_rebuild_selection_action(FALSE, FALSE, FALSE), ==, KAJO_REBUILD_SEL_CLEAR);
}

static void
test_clipboard_parsing_and_exec_toggle(void)
{
    /* 1. x-special/gnome-copied-files with 'cut' */
    gboolean is_cut = FALSE;
    g_autoptr(GPtrArray) parsed_cut = NULL;
    const char *gnome_cut = "cut\nfile:///tmp/one.txt\nfile:///tmp/two%20space.txt\n";
    g_assert_true(kajo_files_parse_clipboard_data(
        "x-special/gnome-copied-files", gnome_cut, strlen(gnome_cut), &parsed_cut, &is_cut
    ));
    g_assert_nonnull(parsed_cut);
    g_assert_true(is_cut);
    g_assert_cmpuint(parsed_cut->len, ==, 2);

    /* 2. text/uri-list with comments, CRLF, and rejected http:// browser URLs */
    is_cut = TRUE;
    g_autoptr(GPtrArray) parsed_uris = NULL;
    const char *uri_list =
        "# Comment line\r\n"
        "https://example.com/archive.zip\r\n"
        "http://example.com/index.html\r\n"
        "file:///tmp/local_item.c\r\n";
    g_assert_true(kajo_files_parse_clipboard_data(
        "text/uri-list", uri_list, strlen(uri_list), &parsed_uris, &is_cut
    ));
    g_assert_nonnull(parsed_uris);
    g_assert_false(is_cut);
    g_assert_cmpuint(parsed_uris->len, ==, 1);
    g_autofree char *only_path = g_file_get_path(g_ptr_array_index(parsed_uris, 0));
    g_assert_cmpstr(only_path, ==, "/tmp/local_item.c");

    /* 3. Pure web URLs in text/uri-list produce 0 valid file items and return FALSE */
    g_autoptr(GPtrArray) parsed_web = NULL;
    const char *web_only = "https://kajo.org/docs\n";
    g_assert_false(kajo_files_parse_clipboard_data(
        "text/uri-list", web_only, strlen(web_only), &parsed_web, &is_cut
    ));
    g_assert_null(parsed_web);

    /* 4. Executable bit toggle on a real POSIX file */
    g_autoptr(GError) err = NULL;
    g_autofree char *tmp_dir = g_dir_make_tmp("kajo-exec-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_autofree char *script_path = g_build_filename(tmp_dir, "run.sh", NULL);
    g_assert_true(g_file_set_contents(script_path, "#!/bin/sh\necho ok\n", -1, &err));
    g_assert_no_error(err);
    g_assert_cmpint(g_chmod(script_path, 0644), ==, 0);

    guint32 mode_after_on = 0;
    g_assert_true(kajo_files_toggle_executable(script_path, &mode_after_on, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(mode_after_on, ==, 0755U);

    guint32 mode_after_off = 0;
    g_assert_true(kajo_files_toggle_executable(script_path, &mode_after_off, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(mode_after_off, ==, 0644U);

    /* Calling toggle_executable on a directory must fail cleanly */
    g_assert_false(kajo_files_toggle_executable(tmp_dir, NULL, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_unlink(script_path);
    g_rmdir(tmp_dir);
}

typedef struct {
    GMainLoop *loop;
    GFile *temp_file;
    GError *error;
} TempExtractTestState;

static void
on_test_temp_extract_done(GFile *temp_file, GError *error, gpointer user_data)
{
    TempExtractTestState *st = user_data;
    if (error)
        st->error = g_error_copy(error);
    if (temp_file)
        st->temp_file = g_object_ref(temp_file);
    g_main_loop_quit(st->loop);
}

static KajoFileItem *
find_item_by_name(GPtrArray *items, const char *name)
{
    if (!items || !name) return NULL;
    for (guint i = 0; i < items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(items, i);
        if (g_strcmp0(kajo_file_item_get_name(it), name) == 0)
            return it;
    }
    return NULL;
}

static void
test_archive_virtual_folders_and_ops(void)
{
    /* 1. URI building, parsing, and traversal rejection */
    g_autofree char *built_root = kajo_files_build_archive_uri("/tmp/my archive.zip", NULL);
    g_assert_cmpstr(built_root, ==, "archive:///tmp/my%20archive.zip!/");

    g_autofree char *arch_path = NULL;
    g_autofree char *inner_path = NULL;
    g_assert_true(kajo_files_parse_archive_uri(built_root, &arch_path, &inner_path));
    g_assert_cmpstr(arch_path, ==, "/tmp/my archive.zip");
    g_assert_cmpstr(inner_path, ==, "");
    g_clear_pointer(&arch_path, g_free);
    g_clear_pointer(&inner_path, g_free);

    g_autofree char *built_sub = kajo_files_build_archive_uri("/tmp/my archive.zip", "/src/core/main.c/");
    g_assert_cmpstr(built_sub, ==, "archive:///tmp/my%20archive.zip!/src/core/main.c");
    g_assert_true(kajo_files_parse_archive_uri(built_sub, &arch_path, &inner_path));
    g_assert_cmpstr(arch_path, ==, "/tmp/my archive.zip");
    g_assert_cmpstr(inner_path, ==, "src/core/main.c");
    g_clear_pointer(&arch_path, g_free);
    g_clear_pointer(&inner_path, g_free);

    g_assert_false(kajo_files_parse_archive_uri("archive:///tmp/foo.zip!/../etc/passwd", NULL, NULL));
    g_assert_false(kajo_files_parse_archive_uri("file:///tmp/foo.zip", NULL, NULL));

    /* 2. Parent URI navigation inside and out of archive:// */
    g_autoptr(GFile) f_deep = g_file_new_for_uri("archive:///tmp/dir/bundle.zip!/src/sub");
    g_autofree char *p1 = kajo_files_get_parent_uri(f_deep);
    g_assert_cmpstr(p1, ==, "archive:///tmp/dir/bundle.zip!/src");

    g_autoptr(GFile) f_one = g_file_new_for_uri(p1);
    g_autofree char *p2 = kajo_files_get_parent_uri(f_one);
    g_assert_cmpstr(p2, ==, "archive:///tmp/dir/bundle.zip!/");

    g_autoptr(GFile) f_root = g_file_new_for_uri(p2);
    g_autofree char *p3 = kajo_files_get_parent_uri(f_root);
    g_assert_cmpstr(p3, ==, "file:///tmp/dir");

    /* 3. Breadcrumb chain for archive:///tmp/dir/bundle.zip!/src/core */
    g_autoptr(GFile) bc_file = g_file_new_for_uri("archive:///tmp/dir/bundle.zip!/src/core");
    g_autoptr(GPtrArray) bc_chain = kajo_files_build_breadcrumb_chain(bc_file);
    g_assert_nonnull(bc_chain);
    g_assert_cmpuint(bc_chain->len, ==, 6);
    const KajoBreadcrumbSegment *seg_arch = g_ptr_array_index(bc_chain, 3);
    g_assert_cmpstr(seg_arch->label, ==, "📦 bundle.zip");
    g_assert_cmpstr(seg_arch->uri, ==, "archive:///tmp/dir/bundle.zip!/");
    const KajoBreadcrumbSegment *seg_src = g_ptr_array_index(bc_chain, 4);
    g_assert_cmpstr(seg_src->label, ==, "src");
    g_assert_cmpstr(seg_src->uri, ==, "archive:///tmp/dir/bundle.zip!/src");
    const KajoBreadcrumbSegment *seg_core = g_ptr_array_index(bc_chain, 5);
    g_assert_cmpstr(seg_core->label, ==, "core");
    g_assert_cmpstr(seg_core->uri, ==, "archive:///tmp/dir/bundle.zip!/src/core");

    /* 4. Create sample workspace and compress to .zip, .tar.gz, and .tar.zst */
    g_autoptr(GError) err = NULL;
    g_autofree char *tmp_path = g_dir_make_tmp("kajo-archive-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_autoptr(GFile) tmp_dir = g_file_new_for_path(tmp_path);

    g_autofree char *src_root = g_build_filename(tmp_path, "src_tree", NULL);
    g_autofree char *nested_dir = g_build_filename(src_root, "nested", NULL);
    g_autofree char *deep_dir = g_build_filename(nested_dir, "deep", NULL);
    g_assert_cmpint(g_mkdir_with_parents(deep_dir, 0755), ==, 0);

    g_autofree char *alpha_path = g_build_filename(src_root, "alpha.txt", NULL);
    g_autofree char *beta_path = g_build_filename(nested_dir, "beta.txt", NULL);
    g_autofree char *gamma_path = g_build_filename(deep_dir, "gamma.c", NULL);
    g_assert_true(g_file_set_contents(alpha_path, "Alpha payload\n", -1, &err));
    g_assert_true(g_file_set_contents(beta_path, "Beta payload\n", -1, &err));
    g_assert_true(g_file_set_contents(gamma_path, "int main(void) { return 0; }\n", -1, &err));

    AsyncTestState st = { 0 };
    st.loop = g_main_loop_new(NULL, FALSE);

    /* Compress multi-item selection (alpha.txt + nested/) into multi.zip and multi.tar.gz */
    g_autoptr(GPtrArray) multi_sources = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(multi_sources, g_file_new_for_path(alpha_path));
    g_ptr_array_add(multi_sources, g_file_new_for_path(nested_dir));

    kajo_files_op_compress_async(multi_sources, tmp_dir, "multi.zip", NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    kajo_files_op_compress_async(multi_sources, tmp_dir, "multi.tar.gz", NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    /* Compress single-root directory (nested/) into single.tar.zst */
    g_autoptr(GPtrArray) single_sources = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(single_sources, g_file_new_for_path(nested_dir));
    kajo_files_op_compress_async(single_sources, tmp_dir, "single.tar.zst", NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    /* 5. Enumerate archive://<tmp>/multi.zip!/ and nested subfolders */
    g_autofree char *multi_zip_path = g_build_filename(tmp_path, "multi.zip", NULL);
    g_autofree char *uri_zip_root = kajo_files_build_archive_uri(multi_zip_path, "");
    g_autoptr(GFile) gf_zip_root = g_file_new_for_uri(uri_zip_root);

    kajo_files_enumerate_async(gf_zip_root, NULL, on_test_enum_done, &st);
    g_main_loop_run(st.loop);
    g_assert_nonnull(st.items);
    g_assert_cmpuint(st.items->len, ==, 2);
    KajoFileItem *it_alpha = find_item_by_name(st.items, "alpha.txt");
    KajoFileItem *it_nested = find_item_by_name(st.items, "nested");
    g_assert_nonnull(it_alpha);
    g_assert_false(kajo_file_item_is_dir(it_alpha));
    g_assert_nonnull(it_nested);
    g_assert_true(kajo_file_item_is_dir(it_nested));
    g_ptr_array_unref(st.items);
    st.items = NULL;

    /* Enumerate archive://<tmp>/multi.zip!/nested */
    g_autofree char *uri_zip_nested = kajo_files_build_archive_uri(multi_zip_path, "nested");
    g_autoptr(GFile) gf_zip_nested = g_file_new_for_uri(uri_zip_nested);
    kajo_files_enumerate_async(gf_zip_nested, NULL, on_test_enum_done, &st);
    g_main_loop_run(st.loop);
    g_assert_nonnull(st.items);
    g_assert_cmpuint(st.items->len, ==, 2);
    KajoFileItem *it_beta = find_item_by_name(st.items, "beta.txt");
    KajoFileItem *it_deep = find_item_by_name(st.items, "deep");
    g_assert_nonnull(it_beta);
    g_assert_false(kajo_file_item_is_dir(it_beta));
    g_assert_nonnull(it_deep);
    g_assert_true(kajo_file_item_is_dir(it_deep));
    g_autofree char *beta_archive_uri = g_strdup(kajo_file_item_get_uri(it_beta));
    g_ptr_array_unref(st.items);
    st.items = NULL;

    /* 6. Extract single entry to temp for Quick Look / Open */
    TempExtractTestState tst = { 0 };
    tst.loop = st.loop;
    kajo_files_op_extract_entry_to_temp_async(beta_archive_uri, NULL, on_test_temp_extract_done, &tst);
    g_main_loop_run(st.loop);
    g_assert_no_error(tst.error);
    g_assert_nonnull(tst.temp_file);
    g_autofree char *temp_beta_path = g_file_get_path(tst.temp_file);
    g_autofree char *temp_beta_contents = NULL;
    g_assert_true(g_file_get_contents(temp_beta_path, &temp_beta_contents, NULL, &err));
    g_assert_cmpstr(temp_beta_contents, ==, "Beta payload\n");
    g_clear_object(&tst.temp_file);

    /* 7. Copy/Paste from archive:// URI into a normal folder via kajo_files_op_transfer_async */
    g_autofree char *paste_dir_path = g_build_filename(tmp_path, "paste_out", NULL);
    g_assert_cmpint(g_mkdir_with_parents(paste_dir_path, 0755), ==, 0);
    g_autoptr(GFile) paste_dir = g_file_new_for_path(paste_dir_path);
    g_autoptr(GPtrArray) copy_from_arch = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(copy_from_arch, g_object_ref(gf_zip_nested));

    kajo_files_op_transfer_async(copy_from_arch, paste_dir, FALSE, NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);
    g_autofree char *pasted_gamma = g_build_filename(paste_dir_path, "nested", "deep", "gamma.c", NULL);
    g_assert_true(g_file_test(pasted_gamma, G_FILE_TEST_IS_REGULAR));

    /* 8. Smart Extraction: multi-root tarbomb wraps in <stem>/, single-root extracts directly without double nesting */
    g_autofree char *ext_multi_dir_path = g_build_filename(tmp_path, "ext_multi", NULL);
    g_autofree char *ext_single_dir_path = g_build_filename(tmp_path, "ext_single", NULL);
    g_assert_cmpint(g_mkdir_with_parents(ext_multi_dir_path, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(ext_single_dir_path, 0755), ==, 0);
    g_autoptr(GFile) ext_multi_dir = g_file_new_for_path(ext_multi_dir_path);
    g_autoptr(GFile) ext_single_dir = g_file_new_for_path(ext_single_dir_path);

    g_autoptr(GFile) multi_targz_file = g_file_get_child(tmp_dir, "multi.tar.gz");
    kajo_files_op_extract_archive_async(multi_targz_file, ext_multi_dir, NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);
    /* Wrapped inside ext_multi/multi/ */
    g_autofree char *wrapped_alpha = g_build_filename(ext_multi_dir_path, "multi", "alpha.txt", NULL);
    g_autofree char *wrapped_gamma = g_build_filename(ext_multi_dir_path, "multi", "nested", "deep", "gamma.c", NULL);
    g_assert_true(g_file_test(wrapped_alpha, G_FILE_TEST_IS_REGULAR));
    g_assert_true(g_file_test(wrapped_gamma, G_FILE_TEST_IS_REGULAR));

    g_autoptr(GFile) single_tarzst_file = g_file_get_child(tmp_dir, "single.tar.zst");
    kajo_files_op_extract_archive_async(single_tarzst_file, ext_single_dir, NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);
    /* Extracted directly to ext_single/nested/ without ext_single/single/nested/ double-nesting */
    g_autofree char *direct_beta = g_build_filename(ext_single_dir_path, "nested", "beta.txt", NULL);
    g_autofree char *double_nested = g_build_filename(ext_single_dir_path, "single", "nested", "beta.txt", NULL);
    g_assert_true(g_file_test(direct_beta, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(double_nested, G_FILE_TEST_EXISTS));

    /* Cleanup tmp_dir */
    g_autoptr(GPtrArray) del_arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(del_arr, g_object_ref(tmp_dir));
    kajo_files_op_delete_permanent_async(del_arr, NULL, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    g_main_loop_unref(st.loop);
}

/* ─── Archive hardening tests (string handling, extraction safety, preview extraction) ─── */

typedef struct {
    const char *path;      /* raw bytes, deliberately not normalized */
    int         type;      /* AE_IFREG / AE_IFDIR / AE_IFLNK */
    const char *data;
    unsigned    perm;
    const char *symlink;
    const char *hardlink;
} TarSpec;

static void
write_test_tar(const char *file, const TarSpec *specs, gsize n)
{
    struct archive *w = archive_write_new();
    archive_write_set_format_gnutar(w);
    g_assert_cmpint(archive_write_open_filename(w, file), ==, ARCHIVE_OK);
    for (gsize i = 0; i < n; i++) {
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, specs[i].path);
        archive_entry_set_filetype(e, (mode_t)specs[i].type);
        archive_entry_set_perm(e, specs[i].perm ? specs[i].perm : 0644);
        archive_entry_set_mtime(e, 1700000000, 0);
        gsize len = specs[i].data ? strlen(specs[i].data) : 0;
        archive_entry_set_size(e, specs[i].type == AE_IFREG ? (la_int64_t)len : 0);
        if (specs[i].symlink)
            archive_entry_set_symlink(e, specs[i].symlink);
        if (specs[i].hardlink) {
            archive_entry_set_hardlink(e, specs[i].hardlink);
            archive_entry_set_size(e, 0);
        }
        g_assert_cmpint(archive_write_header(w, e), >=, ARCHIVE_WARN);
        if (len > 0 && specs[i].type == AE_IFREG && !specs[i].hardlink)
            archive_write_data(w, specs[i].data, len);
        archive_entry_free(e);
    }
    archive_write_close(w);
    archive_write_free(w);
}

typedef struct {
    GMainLoop *loop;
    gboolean   ok;
    char      *msg;
    GError    *err;
} MsgState;

static void
on_msg_done(gboolean success, const char *message, GError *error, gpointer user_data)
{
    MsgState *st = user_data;
    st->ok = success;
    g_free(st->msg);
    st->msg = message ? g_strdup(message) : NULL;
    g_clear_error(&st->err);
    if (error)
        st->err = g_error_copy(error);
    g_main_loop_quit(st->loop);
}

static void
msg_state_clear(MsgState *st)
{
    g_free(st->msg);
    st->msg = NULL;
    g_clear_error(&st->err);
}

static void
test_archive_uri_roundtrip_table(void)
{
    /* Every awkward inner name must survive build -> GIO normalization -> parse byte for byte. */
    const char *names[] = {
        "plain.txt", "a b", "100%", "50%2F", "a#b", "q?x", "p+q", "a&b;c", "it's",
        "(x)[y]", "back\\slash", "new\nline", "é日本", "a:b", "a@b", "a=b", "wow!", "dir!",
        "\xff\xfe.bin", NULL
    };
    for (int i = 0; names[i]; i++) {
        g_autofree char *u = kajo_files_build_archive_uri("/tmp/pack.zip", names[i]);
        g_assert_nonnull(u);
        g_autoptr(GFile) f = g_file_new_for_uri(u);
        g_autofree char *norm_uri = g_file_get_uri(f);
        g_autofree char *ap = NULL;
        g_autofree char *ip = NULL;
        g_assert_true(kajo_files_parse_archive_uri(norm_uri, &ap, &ip));
        g_assert_cmpstr(ap, ==, "/tmp/pack.zip");
        g_assert_cmpstr(ip, ==, names[i]);
    }

    /* Hostile URIs: encoded NUL, encoded slash, parent segments */
    g_assert_false(kajo_files_parse_archive_uri("archive:///tmp/a%00b.zip!/x", NULL, NULL));
    g_assert_false(kajo_files_parse_archive_uri("archive:///tmp/a.zip!/a%2Fb", NULL, NULL));
    g_assert_false(kajo_files_parse_archive_uri("archive:///tmp/a.zip!/x/../../y", NULL, NULL));
    g_assert_false(kajo_files_parse_archive_uri("archive:///tmp/../etc/a.zip!/x", NULL, NULL));

    /* A directory whose name ends in '!' puts an earlier "!/" in front of the real delimiter
     * once GIO has decoded %21: the parser must pick the split that names an existing file. */
    g_autofree char *tmp = g_dir_make_tmp("kajo-bang-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *bang_dir = g_build_filename(tmp, "wow!", NULL);
    g_assert_cmpint(g_mkdir_with_parents(bang_dir, 0755), ==, 0);
    g_autofree char *pack = g_build_filename(bang_dir, "pack.zip", NULL);
    g_assert_true(g_file_set_contents(pack, "x", -1, NULL));

    g_autofree char *u = kajo_files_build_archive_uri(pack, "in/ner.txt");
    g_autoptr(GFile) f = g_file_new_for_uri(u);
    g_autofree char *norm_uri = g_file_get_uri(f);
    g_assert_nonnull(strstr(norm_uri, "wow!/pack.zip!/in/ner.txt"));
    g_autofree char *ap = NULL;
    g_autofree char *ip = NULL;
    g_assert_true(kajo_files_parse_archive_uri(norm_uri, &ap, &ip));
    g_assert_cmpstr(ap, ==, pack);
    g_assert_cmpstr(ip, ==, "in/ner.txt");

    g_unlink(pack);
    g_rmdir(bang_dir);
    g_rmdir(tmp);
}

static void
test_archive_path_normalizer(void)
{
    g_autofree char *a = kajo_files_normalize_archive_path("/abs/x.txt");
    g_assert_cmpstr(a, ==, "abs/x.txt");
    g_autofree char *b = kajo_files_normalize_archive_path("./a//b/./c/");
    g_assert_cmpstr(b, ==, "a/b/c");
    g_autofree char *c = kajo_files_normalize_archive_path("./");
    g_assert_cmpstr(c, ==, "");
    g_assert_null(kajo_files_normalize_archive_path("a/../b"));
    g_assert_null(kajo_files_normalize_archive_path(".."));
    g_autofree char *d = kajo_files_normalize_archive_path("..foo/.bar");
    g_assert_cmpstr(d, ==, "..foo/.bar");
}

static void
test_real_path_and_prune(void)
{
    g_autofree char *tmp = g_dir_make_tmp("kajo-real-XXXXXX", NULL);
    g_autofree char *real = g_build_filename(tmp, "real", NULL);
    g_autofree char *link = g_build_filename(tmp, "link", NULL);
    g_assert_cmpint(g_mkdir_with_parents(real, 0755), ==, 0);
    g_assert_cmpint(symlink("real", link), ==, 0);

    g_autofree char *via_link = g_build_filename(link, "out", "new", NULL);
    g_autofree char *resolved = kajo_files_resolve_real_path(via_link);
    g_autofree char *want_prefix = realpath(tmp, NULL);
    g_autofree char *want = g_build_filename(want_prefix, "real", "out", "new", NULL);
    g_assert_cmpstr(resolved, ==, want);
    g_assert_null(kajo_files_resolve_real_path("relative/path"));

    /* prune: old child dirs go, fresh ones stay, symlinks are never followed */
    g_autofree char *root = g_build_filename(tmp, "cache", NULL);
    g_autofree char *old_dir = g_build_filename(root, "old", NULL);
    g_autofree char *new_dir = g_build_filename(root, "new", NULL);
    g_autofree char *victim = g_build_filename(tmp, "victim.txt", NULL);
    g_autofree char *old_link = g_build_filename(root, "oldlink", NULL);
    g_assert_cmpint(g_mkdir_with_parents(old_dir, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(new_dir, 0755), ==, 0);
    g_autofree char *old_file = g_build_filename(old_dir, "f.txt", NULL);
    g_assert_true(g_file_set_contents(old_file, "x", -1, NULL));
    g_assert_true(g_file_set_contents(victim, "keep", -1, NULL));
    g_assert_cmpint(symlink(victim, old_link), ==, 0);
    struct utimbuf ut = { time(NULL) - 10 * 24 * 3600, time(NULL) - 10 * 24 * 3600 };
    g_assert_cmpint(utime(old_dir, &ut), ==, 0);

    kajo_files_prune_cache_dir(root, 7 * 24 * 3600);
    g_assert_false(g_file_test(old_dir, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(new_dir, G_FILE_TEST_IS_DIR));
    g_assert_true(g_file_test(victim, G_FILE_TEST_EXISTS)); /* the link target survives */

    g_unlink(old_link);
    g_unlink(victim);
    g_rmdir(new_dir);
    g_rmdir(root);
    g_unlink(link);
    g_rmdir(real);
    g_rmdir(tmp);
}

static void
test_archive_extraction_hardening(void)
{
    g_autofree char *tmp = g_dir_make_tmp("kajo-hard-XXXXXX", NULL);
    g_autofree char *real = g_build_filename(tmp, "real", NULL);
    g_autofree char *link = g_build_filename(tmp, "link", NULL);
    g_assert_cmpint(g_mkdir_with_parents(real, 0755), ==, 0);
    g_assert_cmpint(symlink("real", link), ==, 0);

    g_autofree char *tar = g_build_filename(tmp, "pkg.tar", NULL);
    const TarSpec specs[] = {
        { "pkg/", AE_IFDIR, NULL, 0777, NULL, NULL },
        { "pkg/a.txt", AE_IFREG, "abc", 0777, NULL, NULL },
        { "pkg/hl", AE_IFREG, NULL, 0644, NULL, "pkg/a.txt" },
        { "pkg/ln", AE_IFLNK, NULL, 0777, "a.txt", NULL },
        { "/pkg/abs.txt", AE_IFREG, "absolute", 0644, NULL, NULL },
        { "pkg/../evil.txt", AE_IFREG, "evil", 0644, NULL, NULL },
        { "pkg/sub/deep.txt", AE_IFREG, "deep", 0644, NULL, NULL },
        { "pkg/orphan", AE_IFREG, NULL, 0644, NULL, "elsewhere/missing" },
    };
    write_test_tar(tar, specs, G_N_ELEMENTS(specs));

    /* Destination reached through a symlinked parent (like a symlinked /home or ~/Downloads). */
    g_autofree char *dest_via_link = g_build_filename(link, "out", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dest_via_link, 0755), ==, 0);
    g_autoptr(GFile) arch = g_file_new_for_path(tar);
    g_autoptr(GFile) dest = g_file_new_for_path(dest_via_link);

    MsgState st = { .loop = g_main_loop_new(NULL, FALSE) };
    kajo_files_op_extract_archive_async(arch, dest, NULL, on_msg_done, &st);
    g_main_loop_run(st.loop);
    g_assert_no_error(st.err);
    g_assert_true(st.ok);
    /* two entries could not be extracted: the ".." one and the hardlink to an unknown target */
    g_assert_nonnull(strstr(st.msg, "2 skipped"));

    g_autofree char *out = g_build_filename(real, "out", "pkg", NULL);
    g_autofree char *a_txt = g_build_filename(out, "a.txt", NULL);
    g_autofree char *hl = g_build_filename(out, "hl", NULL);
    g_autofree char *ln = g_build_filename(out, "ln", NULL);
    g_autofree char *abs_txt = g_build_filename(out, "abs.txt", NULL);
    g_autofree char *deep = g_build_filename(out, "sub", "deep.txt", NULL);
    g_autofree char *evil_in = g_build_filename(real, "out", "evil.txt", NULL);
    g_autofree char *evil_up = g_build_filename(real, "evil.txt", NULL);
    g_autofree char *orphan = g_build_filename(out, "orphan", NULL);

    g_autofree char *content = NULL;
    g_assert_true(g_file_get_contents(a_txt, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "abc");
    g_free(content);
    content = NULL;
    g_assert_true(g_file_get_contents(hl, &content, NULL, NULL));   /* hardlink resolved inside the target */
    g_assert_cmpstr(content, ==, "abc");
    g_assert_true(g_file_test(ln, G_FILE_TEST_IS_SYMLINK));
    g_assert_true(g_file_test(abs_txt, G_FILE_TEST_IS_REGULAR));    /* absolute name stays inside */
    g_assert_true(g_file_test(deep, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(evil_in, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(evil_up, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(orphan, G_FILE_TEST_EXISTS));        /* not turned into an empty file */

    /* archive modes are sanitized: 0777 in the archive must not become world/group writable */
    GStatBuf sb;
    g_assert_cmpint(g_stat(a_txt, &sb), ==, 0);
    g_assert_cmpuint(sb.st_mode & 0022, ==, 0);
    g_assert_cmpuint(sb.st_mode & 0700, ==, 0700);

    msg_state_clear(&st);

    /* An archive where nothing can be extracted fails and leaves no folder behind. */
    g_autofree char *bad_tar = g_build_filename(tmp, "bad.tar", NULL);
    const TarSpec bad[] = { { "../x.txt", AE_IFREG, "x", 0644, NULL, NULL } };
    write_test_tar(bad_tar, bad, 1);
    g_autoptr(GFile) bad_arch = g_file_new_for_path(bad_tar);
    g_autoptr(GFile) tmp_dir_file = g_file_new_for_path(tmp);
    kajo_files_op_extract_archive_async(bad_arch, tmp_dir_file, NULL, on_msg_done, &st);
    g_main_loop_run(st.loop);
    g_assert_false(st.ok);
    g_assert_nonnull(st.err);
    g_autofree char *bad_dir = g_build_filename(tmp, "bad", NULL);
    g_assert_false(g_file_test(bad_dir, G_FILE_TEST_EXISTS));
    msg_state_clear(&st);

    g_main_loop_unref(st.loop);

    g_autoptr(GFile) cleanup = g_file_new_for_path(tmp);
    GPtrArray *del = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(del, g_object_ref(cleanup));
    AsyncTestState ast = { .loop = g_main_loop_new(NULL, FALSE) };
    kajo_files_op_delete_permanent_async(del, NULL, on_test_op_done, &ast);
    g_main_loop_run(ast.loop);
    g_main_loop_unref(ast.loop);
    g_ptr_array_unref(del);
}

static void
test_archive_listing_and_preview_safety(void)
{
    g_autofree char *tmp = g_dir_make_tmp("kajo-prev-XXXXXX", NULL);
    g_autofree char *tar = g_build_filename(tmp, "mix.tar", NULL);
    const TarSpec specs[] = {
        { "a", AE_IFREG, "i am a file named a", 0644, NULL, NULL },
        { "a/b.txt", AE_IFREG, "inside", 0644, NULL, NULL },
        { "run.sh", AE_IFREG, "#!/bin/sh\necho hi\n", 0755, NULL, NULL },
        { "link", AE_IFLNK, NULL, 0777, "/etc/hostname", NULL },
        { "\xff\xfe.txt", AE_IFREG, "bytes", 0644, NULL, NULL },
        { "/abs/x.txt", AE_IFREG, "abs", 0644, NULL, NULL },
    };
    write_test_tar(tar, specs, G_N_ELEMENTS(specs));

    g_autofree char *root_uri = kajo_files_build_archive_uri(tar, "");
    g_autoptr(GFile) root = g_file_new_for_uri(root_uri);
    AsyncTestState st = { .loop = g_main_loop_new(NULL, FALSE) };
    kajo_files_enumerate_async(root, NULL, on_test_enum_done, &st);
    g_main_loop_run(st.loop);
    g_assert_nonnull(st.items);

    /* "a" as a file and "a/b.txt" as a path: a single directory entry named "a" */
    guint a_count = 0;
    for (guint i = 0; i < st.items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(st.items, i);
        if (g_strcmp0(kajo_file_item_get_name(it), "a") == 0) {
            a_count++;
            g_assert_true(kajo_file_item_is_dir(it));
        }
        g_assert_true(g_utf8_validate(kajo_file_item_get_display_name(it), -1, NULL));
        const char *sym = kajo_file_item_get_symlink_target(it);
        if (sym)
            g_assert_true(g_utf8_validate(sym, -1, NULL));
    }
    g_assert_cmpuint(a_count, ==, 1);
    KajoFileItem *abs_dir = find_item_by_name(st.items, "abs"); /* absolute names are listed... */
    g_assert_nonnull(abs_dir);
    KajoFileItem *odd = NULL;
    for (guint i = 0; i < st.items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(st.items, i);
        if (g_str_has_suffix(kajo_file_item_get_name(it), ".txt") &&
            !g_utf8_validate(kajo_file_item_get_name(it), -1, NULL))
            odd = it;
    }
    g_assert_nonnull(odd); /* the non-UTF-8 name is preserved raw, only the display name is repaired */

    /* ...and extractable, because listing and extraction share one normalizer */
    TempExtractTestState ts = { .loop = g_main_loop_new(NULL, FALSE) };
    g_autofree char *abs_uri = kajo_files_build_archive_uri(tar, "abs/x.txt");
    kajo_files_op_extract_entry_to_temp_async(abs_uri, NULL, on_test_temp_extract_done, &ts);
    g_main_loop_run(ts.loop);
    g_assert_no_error(ts.error);
    g_assert_nonnull(ts.temp_file);
    g_autofree char *abs_tmp_path = g_file_get_path(ts.temp_file);
    g_autofree char *abs_content = NULL;
    g_assert_true(g_file_get_contents(abs_tmp_path, &abs_content, NULL, NULL));
    g_assert_cmpstr(abs_content, ==, "abs");
    g_clear_object(&ts.temp_file);

    /* byte-exact round trip for the non-UTF-8 entry */
    g_autofree char *odd_uri = g_strdup(kajo_file_item_get_uri(odd));
    kajo_files_op_extract_entry_to_temp_async(odd_uri, NULL, on_test_temp_extract_done, &ts);
    g_main_loop_run(ts.loop);
    g_assert_no_error(ts.error);
    g_assert_nonnull(ts.temp_file);
    g_clear_object(&ts.temp_file);

    /* previews never keep executable bits */
    g_autofree char *sh_uri = kajo_files_build_archive_uri(tar, "run.sh");
    kajo_files_op_extract_entry_to_temp_async(sh_uri, NULL, on_test_temp_extract_done, &ts);
    g_main_loop_run(ts.loop);
    g_assert_no_error(ts.error);
    g_autofree char *sh_path = g_file_get_path(ts.temp_file);
    GStatBuf sb;
    g_assert_cmpint(g_stat(sh_path, &sb), ==, 0);
    g_assert_cmpuint(sb.st_mode & 0111, ==, 0);
    g_assert_cmpuint(sb.st_mode & 0600, ==, 0600);
    g_clear_object(&ts.temp_file);

    /* previews refuse symlink entries instead of following them */
    g_autofree char *ln_uri = kajo_files_build_archive_uri(tar, "link");
    kajo_files_op_extract_entry_to_temp_async(ln_uri, NULL, on_test_temp_extract_done, &ts);
    g_main_loop_run(ts.loop);
    g_assert_nonnull(ts.error);
    g_assert_null(ts.temp_file);
    g_clear_error(&ts.error);

    g_main_loop_unref(ts.loop);
    g_ptr_array_unref(st.items);
    g_main_loop_unref(st.loop);

    g_unlink(tar);
    g_rmdir(tmp);
}

static void
test_archive_root_copy_and_odd_archive_names(void)
{
    g_autofree char *tmp = g_dir_make_tmp("kajo-root-XXXXXX", NULL);
    g_autofree char *tar = g_build_filename(tmp, "multi.tar", NULL);
    const TarSpec specs[] = {
        { "one.txt", AE_IFREG, "1", 0644, NULL, NULL },
        { "two.txt", AE_IFREG, "2", 0644, NULL, NULL },
    };
    write_test_tar(tar, specs, G_N_ELEMENTS(specs));

    /* Copying the archive root used to feed a full path to the name helper. */
    g_autofree char *dest_path = g_build_filename(tmp, "dest", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dest_path, 0755), ==, 0);
    g_autoptr(GFile) dest = g_file_new_for_path(dest_path);
    g_autofree char *root_uri = kajo_files_build_archive_uri(tar, "");
    g_autoptr(GFile) root = g_file_new_for_uri(root_uri);
    g_autoptr(GPtrArray) src = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(src, g_object_ref(root));

    MsgState st = { .loop = g_main_loop_new(NULL, FALSE) };
    kajo_files_op_transfer_async(src, dest, FALSE, NULL, on_msg_done, &st);
    g_main_loop_run(st.loop);
    g_assert_no_error(st.err);
    g_assert_true(st.ok);
    g_autofree char *copied = g_build_filename(dest_path, "multi", "one.txt", NULL);
    g_assert_true(g_file_test(copied, G_FILE_TEST_IS_REGULAR));
    msg_state_clear(&st);

    /* Archive names that are not a plain component never escape or crash. */
    g_autofree char *src_file_path = g_build_filename(tmp, "payload.txt", NULL);
    g_assert_true(g_file_set_contents(src_file_path, "payload", -1, NULL));
    g_autoptr(GFile) src_file = g_file_new_for_path(src_file_path);
    g_autoptr(GPtrArray) comp_src = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(comp_src, g_object_ref(src_file));
    kajo_files_op_compress_async(comp_src, dest, "/", NULL, on_msg_done, &st);
    g_main_loop_run(st.loop);
    g_assert_no_error(st.err);
    g_assert_true(st.ok);
    g_autofree char *made = g_build_filename(dest_path, "archive.zip", NULL);
    g_assert_true(g_file_test(made, G_FILE_TEST_IS_REGULAR));
    msg_state_clear(&st);
    g_main_loop_unref(st.loop);

    g_autoptr(GFile) cleanup = g_file_new_for_path(tmp);
    GPtrArray *del = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(del, g_object_ref(cleanup));
    AsyncTestState ast = { .loop = g_main_loop_new(NULL, FALSE) };
    kajo_files_op_delete_permanent_async(del, NULL, on_test_op_done, &ast);
    g_main_loop_run(ast.loop);
    g_main_loop_unref(ast.loop);
    g_ptr_array_unref(del);
}

int
main(int argc, char *argv[])
{
    g_autofree char *cache_tmp = g_dir_make_tmp("kajo-test-cache-XXXXXX", NULL);
    g_setenv("XDG_CACHE_HOME", cache_tmp, TRUE);
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/files/format-size", test_format_size);
    g_test_add_func("/files/format-permissions", test_format_permissions);
    g_test_add_func("/files/sorting-and-filtering", test_sorting_and_filtering);
    g_test_add_func("/files/async-enum-and-ops", test_async_enum_and_ops);
    g_test_add_func("/files/large-directory-scale", test_large_directory_scale);
    g_test_add_func("/files/breadcrumb-and-selection-restore", test_breadcrumb_chain_and_selection_restore);
    g_test_add_func("/files/clipboard-and-exec-toggle", test_clipboard_parsing_and_exec_toggle);
    g_test_add_func("/files/clipboard-cap-no-truncation", test_clipboard_buffer_cap_never_truncates);
    g_test_add_func("/files/rebuild-selection-policy", test_rebuild_selection_policy);
    g_test_add_func("/files/archive-uri-roundtrip-table", test_archive_uri_roundtrip_table);
    g_test_add_func("/files/archive-path-normalizer", test_archive_path_normalizer);
    g_test_add_func("/files/real-path-and-prune", test_real_path_and_prune);
    g_test_add_func("/files/archive-extraction-hardening", test_archive_extraction_hardening);
    g_test_add_func("/files/archive-listing-and-preview-safety", test_archive_listing_and_preview_safety);
    g_test_add_func("/files/archive-root-copy-and-odd-names", test_archive_root_copy_and_odd_archive_names);
    g_test_add_func("/files/archive-virtual-folders-and-ops", test_archive_virtual_folders_and_ops);

    return g_test_run();
}
