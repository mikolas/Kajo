#include <glib.h>
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
}

typedef struct {
    GMainLoop *loop;
    GPtrArray *items;
    gboolean op_ok;
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
    kajo_files_op_mkdir_async(tmp_dir, "subdir", on_test_op_done, &st);
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
    kajo_files_op_rename_async(hello_file, "renamed.txt", on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);

    /* 5. Clean up tmp_dir recursively via kajo_files_op_delete_permanent_async */
    g_autoptr(GPtrArray) del_arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(del_arr, g_object_ref(tmp_dir));
    kajo_files_op_delete_permanent_async(del_arr, on_test_op_done, &st);
    g_main_loop_run(st.loop);
    g_assert_true(st.op_ok);
    g_assert_false(g_file_query_exists(tmp_dir, NULL));

    g_main_loop_unref(st.loop);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/files/format-size", test_format_size);
    g_test_add_func("/files/format-permissions", test_format_permissions);
    g_test_add_func("/files/sorting-and-filtering", test_sorting_and_filtering);
    g_test_add_func("/files/async-enum-and-ops", test_async_enum_and_ops);

    return g_test_run();
}
