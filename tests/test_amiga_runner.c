#include "../src/amiga/amiga_runner.h"

#include <glib.h>
#include <string.h>

static bool
argv_contains(GPtrArray *argv, const char *needle)
{
  for (guint i = 0; i < argv->len; i++) {
    const char *s = g_ptr_array_index(argv, i);
    if (g_strcmp0(s, needle) == 0) return true;
  }
  return false;
}

static void
test_build_argv_profiles(void)
{
  AmigaHwProfile *p = amiga_hw_profile_new("a1200_aga");
  g_free(p->model); p->model = g_strdup("A1200");
  g_free(p->cpu); p->cpu = g_strdup("68020");
  p->chip_kb = 2048;
  p->slow_kb = 0;
  p->fast_kb = 8192;
  p->ntsc = true;

  AmigaRunnerLaunchSpec spec = {
    .amiberry_bin = "/usr/bin/amiberry",
    .profile = p,
    .target_kind = AMIGA_TARGET_ADF_DOS,
    .target_name = "demo",
    .adf_path = "/tmp/demo.adf",
    .gdb_port = 2346,
    .uaeboard_guest_output = true,
  };
  g_autoptr(GError) err = NULL;
  g_autoptr(GPtrArray) argv = NULL;
  g_assert_true(amiga_runner_build_argv(&spec, &argv, &err));
  g_assert_no_error(err);
  g_assert_true(argv_contains(argv, "--model"));
  g_assert_true(argv_contains(argv, "A1200"));
  g_assert_true(argv_contains(argv, "/tmp/demo.adf"));
  g_assert_true(argv_contains(argv, "uaeboard=1"));
  g_assert_true(argv_contains(argv, "debugging_features=segtracker,gdbserver"));
  g_assert_true(argv_contains(argv, "gdb_port=2346"));
  g_assert_true(argv_contains(argv, "cpu_threaded=false"));
  g_assert_true(argv_contains(argv, "ntsc=true"));

  /* os_exe requires dh0_dir */
  AmigaRunnerLaunchSpec bad = spec;
  bad.target_kind = AMIGA_TARGET_OS_EXE;
  bad.dh0_dir = NULL;
  g_autoptr(GPtrArray) bad_argv = NULL;
  g_assert_false(amiga_runner_build_argv(&bad, &bad_argv, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);

  amiga_hw_profile_free(p);
}

typedef struct {
  int  got_stdout;
  int  got_stderr;
  bool exited;
  int  exit_status;
} RunnerCtx;

static void
on_runner_log(AmigaRunner *r G_GNUC_UNUSED, const char *line, bool is_err, gpointer ud)
{
  RunnerCtx *c = ud;
  if (!is_err && g_strcmp0(line, "out_hi") == 0) c->got_stdout++;
  if (is_err && g_strcmp0(line, "err_hi") == 0) c->got_stderr++;
}

static void
on_runner_exit(AmigaRunner *r G_GNUC_UNUSED, int status, gpointer ud)
{
  RunnerCtx *c = ud;
  c->exited = true;
  c->exit_status = status;
}

static void
test_async_process_logs_and_exit(void)
{
  g_autoptr(AmigaRunner) runner = amiga_runner_new();
  RunnerCtx ctx = {0};
  amiga_runner_set_callbacks(runner, on_runner_log, on_runner_exit, &ctx);

  const char *argv[] = { "/bin/sh", "-c", "echo out_hi; echo err_hi >&2", NULL };
  g_autoptr(GError) err = NULL;
  g_assert_true(amiga_runner_start_argv(runner, argv, NULL, 2345, true, &err));
  g_assert_no_error(err);
  g_assert_true(amiga_runner_is_running(runner));
  g_assert_true(amiga_runner_owns_process(runner));

  while (!ctx.exited) {
    g_main_context_iteration(NULL, TRUE);
  }
  g_assert_false(amiga_runner_is_running(runner));
  g_assert_cmpint(ctx.exit_status, ==, 0);
  g_assert_cmpint(ctx.got_stdout, ==, 1);
  g_assert_cmpint(ctx.got_stderr, ==, 1);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/runner/build-argv-profiles", test_build_argv_profiles);
  g_test_add_func("/amiga/runner/async-process-logs-and-exit", test_async_process_logs_and_exit);
  return g_test_run();
}
