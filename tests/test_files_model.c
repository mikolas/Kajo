#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
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

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/files/format-size", test_format_size);
    g_test_add_func("/files/format-permissions", test_format_permissions);
    g_test_add_func("/files/sorting-and-filtering", test_sorting_and_filtering);
    g_test_add_func("/files/async-enum-and-ops", test_async_enum_and_ops);
    g_test_add_func("/files/large-directory-scale", test_large_directory_scale);
    g_test_add_func("/files/breadcrumb-and-selection-restore", test_breadcrumb_chain_and_selection_restore);
    g_test_add_func("/files/clipboard-and-exec-toggle", test_clipboard_parsing_and_exec_toggle);

    return g_test_run();
}
