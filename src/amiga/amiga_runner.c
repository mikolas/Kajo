#include "amiga_runner.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>

struct _AmigaRunner {
  GObject parent_instance;

  GSubprocess      *proc;
  GCancellable     *cancellable;
  bool              running;
  bool              owns_process;
  uint16_t          gdb_port;

  AmigaRunnerLogCb  log_cb;
  AmigaRunnerExitCb exit_cb;
  gpointer          user_data;
};

G_DEFINE_FINAL_TYPE(AmigaRunner, amiga_runner, G_TYPE_OBJECT)

static void
amiga_runner_dispose(GObject *obj)
{
  AmigaRunner *self = AMIGA_RUNNER(obj);
  amiga_runner_stop(self);
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_clear_object(&self->proc);
  G_OBJECT_CLASS(amiga_runner_parent_class)->dispose(obj);
}

static void
amiga_runner_class_init(AmigaRunnerClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = amiga_runner_dispose;
}

static void
amiga_runner_init(AmigaRunner *self)
{
  self->cancellable = g_cancellable_new();
}

static bool
is_valid_profile_model(const char *m)
{
  return m && (g_strcmp0(m, "A500") == 0 ||
               g_strcmp0(m, "A500P") == 0 ||
               g_strcmp0(m, "A600") == 0 ||
               g_strcmp0(m, "A1200") == 0 ||
               g_strcmp0(m, "A2000") == 0 ||
               g_strcmp0(m, "A4000") == 0);
}

bool
amiga_runner_build_argv(const AmigaRunnerLaunchSpec *spec,
                        GPtrArray                  **out_argv,
                        GError                     **error)
{
  if (!spec || !spec->amiberry_bin || !spec->amiberry_bin[0] || !out_argv) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Missing amiberry binary path");
    return false;
  }
  *out_argv = NULL;

  const AmigaHwProfile *p = spec->profile;
  GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(argv, g_strdup(spec->amiberry_bin));

  if (p && is_valid_profile_model(p->model)) {
    g_ptr_array_add(argv, g_strdup("--model"));
    g_ptr_array_add(argv, g_strdup(p->model));
  }

  /* Packaging mount */
  if (spec->target_kind == AMIGA_TARGET_FLAT_BIN) {
    g_ptr_array_unref(argv);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "flat_bin targets produce a flat binary and cannot boot Amiberry directly (use Attach :2345)");
    return false;
  } else if (spec->target_kind == AMIGA_TARGET_EXE_DH0) {
    if (!spec->dh0_dir || !spec->dh0_dir[0]) {
      g_ptr_array_unref(argv);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "exe_dh0 launch requires dh0_dir");
      return false;
    }
    g_ptr_array_add(argv, g_strdup("-m"));
    g_ptr_array_add(argv, g_strdup_printf("DH0:%s", spec->dh0_dir));
  } else {
    if (!spec->adf_path || !spec->adf_path[0]) {
      g_ptr_array_unref(argv);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "exe_adf launch requires adf_path");
      return false;
    }
    g_ptr_array_add(argv, g_strdup("-0"));
    g_ptr_array_add(argv, g_strdup(spec->adf_path));
    if (spec->uaeboard_guest_output) {
      g_ptr_array_add(argv, g_strdup("-s"));
      g_ptr_array_add(argv, g_strdup("uaeboard=1"));
    }
  }

  const char *kick = (spec->kickstart_override && spec->kickstart_override[0])
                         ? spec->kickstart_override
                         : (p && p->kickstart && p->kickstart[0] ? p->kickstart : NULL);
  if (kick) {
    g_ptr_array_add(argv, g_strdup("-s"));
    g_ptr_array_add(argv, g_strdup_printf("kickstart_rom_file=%s", kick));
  }

  if (p) {
    if (p->cpu && p->cpu[0]) {
      g_ptr_array_add(argv, g_strdup("-s"));
      g_ptr_array_add(argv, g_strdup_printf("cpu_model=%s", p->cpu));
    }
    if (p->chip_kb > 0) {
      uint32_t half_mb = (p->chip_kb + 511u) / 512u;
      g_ptr_array_add(argv, g_strdup("-s"));
      g_ptr_array_add(argv, g_strdup_printf("chipmem_size=%u", half_mb));
    }
    if (p->slow_kb > 0) {
      uint32_t q_mb = (p->slow_kb + 255u) / 256u;
      g_ptr_array_add(argv, g_strdup("-s"));
      g_ptr_array_add(argv, g_strdup_printf("bogomem_size=%u", q_mb));
    }
    if (p->fast_kb > 0) {
      uint32_t mb = (p->fast_kb + 1023u) / 1024u;
      g_ptr_array_add(argv, g_strdup("-s"));
      g_ptr_array_add(argv, g_strdup_printf("fastmem_size=%u", mb));
    }
    g_ptr_array_add(argv, g_strdup("-s"));
    g_ptr_array_add(argv, g_strdup_printf("ntsc=%s", p->ntsc ? "true" : "false"));
    g_ptr_array_add(argv, g_strdup("-s"));
    g_ptr_array_add(argv, g_strdup_printf("cpu_cycle_exact=%s", p->cycle_exact ? "true" : "false"));
  }

  uint16_t port = spec->gdb_port ? spec->gdb_port : 2345;
  g_ptr_array_add(argv, g_strdup("-G"));
  g_ptr_array_add(argv, g_strdup("--log"));
  g_ptr_array_add(argv, g_strdup("-s"));
  g_ptr_array_add(argv, g_strdup("debugging_features=segtracker,gdbserver"));
  g_ptr_array_add(argv, g_strdup("-s"));
  g_ptr_array_add(argv, g_strdup_printf("gdb_port=%u", (unsigned)port));
  g_ptr_array_add(argv, g_strdup("-s"));
  g_ptr_array_add(argv, g_strdup("cpu_threaded=false"));

  if (p && p->extra_args && p->extra_args[0]) {
    g_auto(GStrv) extras = NULL;
    if (g_shell_parse_argv(p->extra_args, NULL, &extras, NULL) && extras) {
      for (int i = 0; extras[i] != NULL; i++) {
        g_ptr_array_add(argv, g_strdup(extras[i]));
      }
    }
  }

  g_ptr_array_add(argv, NULL);
  *out_argv = argv;
  return true;
}

typedef struct {
  GWeakRef         weak_runner;
  GDataInputStream *stream;
  bool             is_stderr;
} RunnerPipeCtx;

static void
runner_pipe_ctx_free(RunnerPipeCtx *c)
{
  if (!c) return;
  g_weak_ref_clear(&c->weak_runner);
  g_clear_object(&c->stream);
  g_free(c);
}

static void on_pipe_line_read(GObject *src, GAsyncResult *res, gpointer ud);

static void
schedule_next_pipe_read(AmigaRunner *runner, RunnerPipeCtx *ctx)
{
  g_data_input_stream_read_line_async(
      ctx->stream, G_PRIORITY_DEFAULT, runner->cancellable, on_pipe_line_read, ctx);
}

static void
on_pipe_line_read(GObject *src, GAsyncResult *res, gpointer ud)
{
  RunnerPipeCtx *ctx = ud;
  g_autoptr(GError) err = NULL;
  gsize len = 0;
  g_autofree char *line = g_data_input_stream_read_line_finish_utf8(
      G_DATA_INPUT_STREAM(src), res, &len, &err);

  if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    runner_pipe_ctx_free(ctx);
    return;
  }

  g_autoptr(AmigaRunner) runner = g_weak_ref_get(&ctx->weak_runner);
  if (!runner || line == NULL) {
    runner_pipe_ctx_free(ctx);
    return;
  }

  if (runner->log_cb) {
    runner->log_cb(runner, line, ctx->is_stderr, runner->user_data);
  }
  schedule_next_pipe_read(runner, ctx);
}

static void
on_proc_exited(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  (void)g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);

  g_autoptr(AmigaRunner) runner = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);

  if (!runner || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return;
  }
  runner->running = false;
  int st = g_subprocess_get_if_exited(G_SUBPROCESS(src))
               ? g_subprocess_get_exit_status(G_SUBPROCESS(src))
               : -1;
  g_clear_object(&runner->proc);
  if (runner->exit_cb) {
    runner->exit_cb(runner, st, runner->user_data);
  }
}

AmigaRunner *
amiga_runner_new(void)
{
  return g_object_new(AMIGA_TYPE_RUNNER, NULL);
}

void
amiga_runner_set_callbacks(AmigaRunner       *runner,
                           AmigaRunnerLogCb   log_cb,
                           AmigaRunnerExitCb  exit_cb,
                           gpointer           user_data)
{
  g_return_if_fail(AMIGA_IS_RUNNER(runner));
  runner->log_cb = log_cb;
  runner->exit_cb = exit_cb;
  runner->user_data = user_data;
}

bool
amiga_runner_start_argv(AmigaRunner       *runner,
                        const char *const *argv,
                        const char        *cwd,
                        uint16_t           gdb_port,
                        bool               owns_process,
                        GError           **error)
{
  g_return_val_if_fail(AMIGA_IS_RUNNER(runner), false);
  if (!argv || !argv[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Empty runner argv");
    return false;
  }
  amiga_runner_stop(runner);

  g_cancellable_cancel(runner->cancellable);
  g_clear_object(&runner->cancellable);
  runner->cancellable = g_cancellable_new();

  g_autoptr(GSubprocessLauncher) launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  if (cwd && cwd[0]) {
    g_subprocess_launcher_set_cwd(launcher, cwd);
  }

  GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, error);
  if (!proc) {
    return false;
  }

  runner->proc = proc;
  runner->running = true;
  runner->owns_process = owns_process;
  runner->gdb_port = gdb_port ? gdb_port : 2345;

  GInputStream *out_s = g_subprocess_get_stdout_pipe(proc);
  GInputStream *err_s = g_subprocess_get_stderr_pipe(proc);
  if (out_s) {
    RunnerPipeCtx *c = g_new0(RunnerPipeCtx, 1);
    g_weak_ref_init(&c->weak_runner, runner);
    c->stream = g_data_input_stream_new(out_s);
    c->is_stderr = false;
    schedule_next_pipe_read(runner, c);
  }
  if (err_s) {
    RunnerPipeCtx *c = g_new0(RunnerPipeCtx, 1);
    g_weak_ref_init(&c->weak_runner, runner);
    c->stream = g_data_input_stream_new(err_s);
    c->is_stderr = true;
    schedule_next_pipe_read(runner, c);
  }

  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, runner);
  g_subprocess_wait_async(proc, runner->cancellable, on_proc_exited, weak);
  return true;
}

bool
amiga_runner_start(AmigaRunner                 *runner,
                   const AmigaRunnerLaunchSpec *spec,
                   const char                  *cwd,
                   GError                     **error)
{
  g_autoptr(GPtrArray) argv = NULL;
  if (!amiga_runner_build_argv(spec, &argv, error)) {
    return false;
  }
  uint16_t port = (spec && spec->gdb_port) ? spec->gdb_port : 2345;
  return amiga_runner_start_argv(
      runner, (const char *const *)argv->pdata, cwd, port, true, error);
}

void
amiga_runner_stop(AmigaRunner *runner)
{
  g_return_if_fail(AMIGA_IS_RUNNER(runner));
  if (runner->cancellable) {
    g_cancellable_cancel(runner->cancellable);
  }
  if (runner->proc && runner->owns_process) {
    g_subprocess_send_signal(runner->proc, SIGTERM);
    g_subprocess_force_exit(runner->proc);
  }
  g_clear_object(&runner->proc);
  runner->running = false;
}

bool
amiga_runner_is_running(AmigaRunner *runner)
{
  g_return_val_if_fail(AMIGA_IS_RUNNER(runner), false);
  return runner->running;
}

bool
amiga_runner_owns_process(AmigaRunner *runner)
{
  g_return_val_if_fail(AMIGA_IS_RUNNER(runner), false);
  return runner->owns_process;
}

uint16_t
amiga_runner_get_gdb_port(AmigaRunner *runner)
{
  g_return_val_if_fail(AMIGA_IS_RUNNER(runner), 0);
  return runner->gdb_port;
}
