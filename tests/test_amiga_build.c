#include "../src/amiga/amiga_build.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

static void
rm_rf(const char *path)
{
  if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
    return;
  }
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
      const char *name;
      while ((name = g_dir_read_name(d)) != NULL) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        rm_rf(child);
      }
      g_dir_close(d);
    }
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

static void
test_depfile_and_mtime_rebuild(void)
{
  const char *dep =
      "build/main.o: src/main.c \\\n"
      "  include/custom.h include/exec/types.h \\\n"
      "  include/my\\ header.h\n";

  g_autoptr(GPtrArray) deps = NULL;
  g_assert_true(amiga_build_parse_depfile(dep, &deps));
  g_assert_cmpuint(deps->len, ==, 4);
  g_assert_cmpstr((const char *)g_ptr_array_index(deps, 0), ==, "src/main.c");
  g_assert_cmpstr((const char *)g_ptr_array_index(deps, 1), ==, "include/custom.h");
  g_assert_cmpstr((const char *)g_ptr_array_index(deps, 3), ==, "include/my header.h");

  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-mtime-XXXXXX", &err);
  g_assert_no_error(err);

  g_autofree char *src = g_build_filename(tmp, "main.c", NULL);
  g_autofree char *obj = g_build_filename(tmp, "main.o", NULL);
  g_assert_true(g_file_set_contents(src, "int x;\n", -1, &err));

  /* Missing output triggers rebuild */
  const char *inputs[] = { src };
  g_assert_true(amiga_build_needs_rebuild(obj, inputs, 1));

  /* Output newer than input skips rebuild */
  g_assert_true(g_file_set_contents(obj, "OBJ", -1, &err));
  struct utimbuf ut_src = { .actime = 1700000000, .modtime = 1700000000 };
  struct utimbuf ut_obj = { .actime = 1700000100, .modtime = 1700000100 };
  g_assert_cmpint(utime(src, &ut_src), ==, 0);
  g_assert_cmpint(utime(obj, &ut_obj), ==, 0);
  g_assert_false(amiga_build_needs_rebuild(obj, inputs, 1));

  /* Touched input triggers rebuild */
  struct utimbuf ut_src_new = { .actime = 1700000200, .modtime = 1700000200 };
  g_assert_cmpint(utime(src, &ut_src_new), ==, 0);
  g_assert_true(amiga_build_needs_rebuild(obj, inputs, 1));

  rm_rf(tmp);
}

static void
test_diagnostics_and_stage_dh0(void)
{
  AmigaBuildDiag d = {0};

  /* GCC error */
  g_assert_true(amiga_build_parse_diagnostic(
      "src/main.c:42:13: error: expected ';' before '}' token", &d));
  g_assert_cmpstr(d.file, ==, "src/main.c");
  g_assert_cmpuint(d.line, ==, 42);
  g_assert_cmpuint(d.col, ==, 13);
  g_assert_cmpint(d.severity, ==, AMIGA_DIAG_ERROR);
  g_free(d.file);
  g_free(d.message);
  memset(&d, 0, sizeof(d));

  /* GCC warning without column */
  g_assert_true(amiga_build_parse_diagnostic(
      "src/copper.c:10: warning: unused variable 'i'", &d));
  g_assert_cmpstr(d.file, ==, "src/copper.c");
  g_assert_cmpuint(d.line, ==, 10);
  g_assert_cmpint(d.severity, ==, AMIGA_DIAG_WARNING);
  g_free(d.file);
  g_free(d.message);
  memset(&d, 0, sizeof(d));

  /* vasmm68k_mot error */
  g_assert_true(amiga_build_parse_diagnostic(
      "error 2 in line 17 of \"src/irq.s\": unknown mnemonic <badop>", &d));
  g_assert_cmpstr(d.file, ==, "src/irq.s");
  g_assert_cmpuint(d.line, ==, 17);
  g_assert_cmpint(d.severity, ==, AMIGA_DIAG_ERROR);
  g_free(d.file);
  g_free(d.message);

  /* Stage DH0 */
  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-dh0-XXXXXX", &err);
  g_assert_no_error(err);

  g_autofree char *proj_dir = g_build_filename(tmp, "proj", NULL);
  g_autofree char *dh0_dir = g_build_filename(tmp, "dh0", NULL);
  g_mkdir_with_parents(proj_dir, 0755);

  g_autofree char *exe = g_build_filename(proj_dir, "demo.exe", NULL);
  g_autofree char *extra = g_build_filename(proj_dir, "song.mod", NULL);
  g_assert_true(g_file_set_contents(exe, "HUNK", -1, &err));
  g_assert_true(g_file_set_contents(extra, "MOD!", -1, &err));

  AmigaProject *proj = amiga_project_new();
  proj->root_dir = g_strdup(proj_dir);
  AmigaTarget *t = amiga_target_new("demo");
  t->kind = AMIGA_TARGET_OS_EXE;
  t->startup_sequence = g_strdup("c/demo.exe\n");
  g_ptr_array_add(t->extra_files, g_strdup("song.mod"));
  g_ptr_array_add(proj->targets, t);

  g_assert_true(amiga_build_stage_dh0(proj, t, exe, dh0_dir, &err));
  g_assert_no_error(err);

  g_autofree char *st_exe = g_build_filename(dh0_dir, "demo.exe", NULL);
  g_autofree char *st_seq = g_build_filename(dh0_dir, "s", "startup-sequence", NULL);
  g_autofree char *st_ext = g_build_filename(dh0_dir, "song.mod", NULL);
  g_assert_true(g_file_test(st_exe, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(st_seq, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(st_ext, G_FILE_TEST_EXISTS));

  amiga_project_free(proj);
  rm_rf(tmp);
}

static void
on_build_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
  GAsyncResult **out = user_data;
  *out = g_object_ref(res);
}

static void
test_async_external_build_success_fail_and_cancel(void)
{
  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-async-XXXXXX", &err);
  g_assert_no_error(err);

  /* 1. Passing external build command */
  AmigaProject *proj = amiga_project_new();
  proj->root_dir = g_strdup(tmp);
  proj->build_mode = AMIGA_BUILD_EXTERNAL;
  proj->build_cmd = g_strdup("mkdir -p build && printf HUNK > build/out.exe && echo built ok");

  AmigaTarget *t = amiga_target_new("out");
  t->kind = AMIGA_TARGET_OS_EXE;
  t->exe_path = g_strdup("build/out.exe");
  g_ptr_array_add(proj->targets, t);

  GAsyncResult *res = NULL;
  amiga_build_run_async(proj, t, NULL, tmp, NULL, on_build_done, &res);
  while (res == NULL) {
    g_main_context_iteration(NULL, TRUE);
  }
  AmigaBuildResult *br = amiga_build_run_finish(res, &err);
  g_clear_object(&res);
  g_assert_no_error(err);
  g_assert_nonnull(br);
  g_assert_true(br->success);
  g_assert_nonnull(strstr(br->log_text, "built ok"));
  amiga_build_result_free(br);

  /* 2. Failing external build command emits diagnostic */
  g_free(proj->build_cmd);
  proj->build_cmd = g_strdup("echo 'src/bad.c:9:2: error: undefined symbol' >&2; exit 2");
  amiga_build_run_async(proj, t, NULL, tmp, NULL, on_build_done, &res);
  while (res == NULL) {
    g_main_context_iteration(NULL, TRUE);
  }
  br = amiga_build_run_finish(res, &err);
  g_clear_object(&res);
  g_assert_no_error(err);
  g_assert_nonnull(br);
  g_assert_false(br->success);
  g_assert_cmpuint(br->diagnostics->len, >=, 1);
  AmigaBuildDiag *d0 = g_ptr_array_index(br->diagnostics, 0);
  g_assert_cmpstr(d0->file, ==, "src/bad.c");
  g_assert_cmpuint(d0->line, ==, 9);
  amiga_build_result_free(br);

  /* 3. Native build DAG planning sanity check (fake executable compiler in PATH) */
  proj->build_mode = AMIGA_BUILD_NATIVE;
  g_autofree char *src_dir = g_build_filename(tmp, "src", NULL);
  g_mkdir_with_parents(src_dir, 0755);
  g_autofree char *main_s = g_build_filename(src_dir, "main.s", NULL);
  g_assert_true(g_file_set_contents(main_s, "\trts\n", -1, &err));
  g_ptr_array_add(t->sources, g_strdup("src/main.s"));
  t->toolchain = AMIGA_TOOLCHAIN_SINGLE_ASM;

  g_autofree char *fake_vasm = g_build_filename(tmp, "vasmm68k_mot", NULL);
  g_assert_true(g_file_set_contents(fake_vasm, "#!/bin/sh\nexit 0\n", -1, &err));
  g_chmod(fake_vasm, 0755);

  const char *extra_bins[] = { tmp, NULL };
  AmigaToolchainStatus *tc = amiga_project_probe_toolchain(extra_bins);
  g_autoptr(GPtrArray) steps = NULL;
  g_assert_true(amiga_build_plan_steps(proj, t, tc, tmp, &steps, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(steps->len, >=, 1);
  amiga_toolchain_status_free(tc);

  /* 4. Pre-cancelled async call returns G_IO_ERROR_CANCELLED */
  proj->build_mode = AMIGA_BUILD_EXTERNAL;
  g_free(proj->build_cmd);
  proj->build_cmd = g_strdup("sleep 10");
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  amiga_build_run_async(proj, t, NULL, tmp, cancel, on_build_done, &res);
  while (res == NULL) {
    g_main_context_iteration(NULL, TRUE);
  }
  br = amiga_build_run_finish(res, &err);
  g_clear_object(&res);
  g_assert_null(br);
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&err);

  amiga_project_free(proj);
  rm_rf(tmp);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/build/depfile-and-mtime-rebuild", test_depfile_and_mtime_rebuild);
  g_test_add_func("/amiga/build/diagnostics-and-stage-dh0", test_diagnostics_and_stage_dh0);
  g_test_add_func("/amiga/build/async-external-build", test_async_external_build_success_fail_and_cancel);
  return g_test_run();
}
