#include "amiga_build.h"
#include "amiga_adf.h"

#include <ctype.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

void
amiga_build_step_free(AmigaBuildStep *s)
{
  if (!s) return;
  g_free(s->label);
  g_free(s->cwd);
  if (s->argv) g_ptr_array_unref(s->argv);
  g_free(s->out_artifact);
  g_free(s);
}

void
amiga_build_diag_free(AmigaBuildDiag *d)
{
  if (!d) return;
  g_free(d->file);
  g_free(d->message);
  g_free(d);
}

void
amiga_build_result_free(AmigaBuildResult *r)
{
  if (!r) return;
  g_free(r->log_text);
  if (r->diagnostics) g_ptr_array_unref(r->diagnostics);
  g_free(r->artifact_path);
  g_free(r);
}

static AmigaBuildStep *
step_new(AmigaBuildStepKind kind, const char *label, const char *cwd)
{
  AmigaBuildStep *s = g_new0(AmigaBuildStep, 1);
  s->kind = kind;
  s->label = g_strdup(label);
  s->cwd = g_strdup(cwd);
  s->argv = g_ptr_array_new_with_free_func(g_free);
  return s;
}

bool
amiga_build_parse_depfile(const char *dep_text, GPtrArray **out_deps)
{
  if (!dep_text || !out_deps) {
    return false;
  }
  *out_deps = NULL;

  /* Join `\` line continuations, locate first `:` separator, split prerequisites */
  g_autoptr(GString) joined = g_string_new("");
  for (const char *p = dep_text; *p; p++) {
    if (p[0] == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) {
      p++;
      if (*p == '\r' && p[1] == '\n') p++;
      g_string_append_c(joined, ' ');
      continue;
    }
    g_string_append_c(joined, *p);
  }

  const char *colon = strstr(joined->str, ": ");
  if (!colon) {
    colon = strchr(joined->str, ':');
  }
  if (!colon) {
    return false;
  }

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  const char *p = colon + 1;
  while (*p) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;
    GString *tok = g_string_new("");
    while (*p && !isspace((unsigned char)*p)) {
      if (p[0] == '\\' && p[1] == ' ') {
        g_string_append_c(tok, ' ');
        p += 2;
      } else {
        g_string_append_c(tok, *p++);
      }
    }
    if (tok->len > 0) {
      g_ptr_array_add(arr, g_string_free(tok, FALSE));
    } else {
      g_string_free(tok, TRUE);
    }
  }
  *out_deps = arr;
  return true;
}

bool
amiga_build_needs_rebuild(const char        *output_path,
                          const char *const *inputs,
                          size_t             n_inputs)
{
  if (!output_path) return true;
  GStatBuf st_out;
  if (g_stat(output_path, &st_out) != 0) {
    return true;
  }
  for (size_t i = 0; i < n_inputs; i++) {
    if (!inputs[i]) continue;
    GStatBuf st_in;
    if (g_stat(inputs[i], &st_in) != 0) {
      return true;
    }
    if (st_in.st_mtime > st_out.st_mtime) {
      return true;
    }
  }
  return false;
}

static bool
is_c_source(const char *s)
{
  return s && (g_str_has_suffix(s, ".c") || g_str_has_suffix(s, ".C"));
}

static char *
object_stem_for_source(const char *src)
{
  g_autofree char *base = g_path_get_basename(src);
  char *dot = strrchr(base, '.');
  if (dot) *dot = '\0';
  return g_strdup(base);
}

bool
amiga_build_plan_steps(const AmigaProject         *proj,
                       const AmigaTarget          *target,
                       const AmigaToolchainStatus *tc,
                       const char                 *staging_root,
                       GPtrArray                 **out_steps,
                       GError                    **error)
{
  if (!proj || !target || !out_steps) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid build plan args");
    return false;
  }
  *out_steps = NULL;
  const char *root = proj->root_dir ? proj->root_dir : ".";
  GPtrArray *steps = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_build_step_free);

  if (proj->build_mode == AMIGA_BUILD_EXTERNAL) {
    const char *raw_cmd = (proj->build_cmd && proj->build_cmd[0]) ? proj->build_cmd : "make -j4";
    AmigaBuildStep *st = step_new(AMIGA_STEP_SHELL_CMD, "External build", root);
    g_ptr_array_add(st->argv, g_strdup("/bin/sh"));
    g_ptr_array_add(st->argv, g_strdup("-c"));
    g_ptr_array_add(st->argv, g_strdup_printf("%s effect=%s", raw_cmd, target->name));
    if (target->exe_path) {
      st->out_artifact = g_build_filename(root, target->exe_path, NULL);
    }
    g_ptr_array_add(steps, st);
    *out_steps = steps;
    return true;
  }

  /* Asset pre-build steps (mtime cached) */
  for (guint i = 0; proj->assets && i < proj->assets->len; i++) {
    const AmigaAsset *a = g_ptr_array_index(proj->assets, i);
    if (!a->cmd || !a->cmd[0]) continue;

    g_autoptr(GPtrArray) abs_in = g_ptr_array_new_with_free_func(g_free);
    for (guint ii = 0; a->inputs && ii < a->inputs->len; ii++) {
      g_ptr_array_add(abs_in, g_build_filename(root, g_ptr_array_index(a->inputs, ii), NULL));
    }
    bool dirty = (a->outputs == NULL || a->outputs->len == 0);
    for (guint oi = 0; !dirty && oi < a->outputs->len; oi++) {
      g_autofree char *out_f = g_build_filename(root, g_ptr_array_index(a->outputs, oi), NULL);
      if (amiga_build_needs_rebuild(out_f, (const char *const *)abs_in->pdata, abs_in->len)) {
        dirty = true;
      }
    }

    AmigaBuildStep *as = step_new(AMIGA_STEP_SHELL_CMD, a->name ? a->name : "asset", root);
    as->skipped_up_to_date = !dirty;
    g_ptr_array_add(as->argv, g_strdup("/bin/sh"));
    g_ptr_array_add(as->argv, g_strdup("-c"));
    g_ptr_array_add(as->argv, g_strdup(a->cmd));
    g_ptr_array_add(steps, as);
  }

  /* Collect shared + target sources */
  g_autoptr(GPtrArray) all_sources = g_ptr_array_new();
  for (guint i = 0; proj->shared_sources && i < proj->shared_sources->len; i++) {
    g_ptr_array_add(all_sources, g_ptr_array_index(proj->shared_sources, i));
  }
  for (guint i = 0; target->sources && i < target->sources->len; i++) {
    g_ptr_array_add(all_sources, g_ptr_array_index(target->sources, i));
  }

  const char *vasm = (tc && tc->vasmm68k_mot) ? tc->vasmm68k_mot : "vasmm68k_mot";
  const char *gcc  = (tc && tc->m68k_elf_gcc) ? tc->m68k_elf_gcc : "m68k-amiga-elf-gcc";
  const char *ld   = (tc && tc->m68k_elf_ld) ? tc->m68k_elf_ld : "m68k-amiga-elf-ld";
  const char *e2h  = (tc && tc->elf2hunk) ? tc->elf2hunk : "elf2hunk";
  const char *vlnk = (tc && tc->vlink) ? tc->vlink : "vlink";
  const char *cpu  = target->cpu ? target->cpu : "68000";

  g_autoptr(GPtrArray) objs = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; i < all_sources->len; i++) {
    const char *src = g_ptr_array_index(all_sources, i);
    g_autofree char *stem = object_stem_for_source(src);
    g_autofree char *obj_rel = g_strdup_printf("obj/%s.o", stem);
    g_autofree char *dep_rel = g_strdup_printf("obj/%s.d", stem);
    g_autofree char *abs_src = g_build_filename(root, src, NULL);
    g_autofree char *abs_obj = g_build_filename(root, obj_rel, NULL);
    g_autofree char *abs_dep = g_build_filename(root, dep_rel, NULL);

    /* Incremental check against src + .d depfile prerequisites */
    g_autoptr(GPtrArray) dep_inputs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(dep_inputs, g_strdup(abs_src));
    g_autofree char *dep_txt = NULL;
    if (g_file_get_contents(abs_dep, &dep_txt, NULL, NULL)) {
      g_autoptr(GPtrArray) parsed = NULL;
      if (amiga_build_parse_depfile(dep_txt, &parsed) && parsed) {
        for (guint di = 0; di < parsed->len; di++) {
          const char *dp = g_ptr_array_index(parsed, di);
          g_ptr_array_add(dep_inputs, g_path_is_absolute(dp) ? g_strdup(dp) : g_build_filename(root, dp, NULL));
        }
      }
    }
    bool dirty = amiga_build_needs_rebuild(abs_obj, (const char *const *)dep_inputs->pdata, dep_inputs->len);

    AmigaBuildStep *cs = step_new(AMIGA_STEP_SHELL_CMD, src, root);
    cs->skipped_up_to_date = !dirty;
    cs->out_artifact = g_strdup(obj_rel);

    if (is_c_source(src)) {
      g_ptr_array_add(cs->argv, g_strdup(gcc));
      g_ptr_array_add(cs->argv, g_strdup("-c"));
      g_ptr_array_add(cs->argv, g_strdup("-g"));
      g_ptr_array_add(cs->argv, g_strdup("-O2"));
      g_ptr_array_add(cs->argv, g_strdup_printf("-mcpu=%s", cpu));
      g_ptr_array_add(cs->argv, g_strdup("-MMD"));
      g_ptr_array_add(cs->argv, g_strdup("-MF"));
      g_ptr_array_add(cs->argv, g_strdup(dep_rel));
    } else {
      const char *fmt = (target->toolchain == AMIGA_TOOLCHAIN_HUNK_VLINK) ? "-Fhunk"
                      : (target->toolchain == AMIGA_TOOLCHAIN_SINGLE_ASM) ? "-Fhunkexe" : "-Felf";
      g_ptr_array_add(cs->argv, g_strdup(vasm));
      g_ptr_array_add(cs->argv, g_strdup(fmt));
      g_ptr_array_add(cs->argv, g_strdup_printf("-m%s", cpu));
      g_ptr_array_add(cs->argv, g_strdup("-quiet"));
      if (target->toolchain == AMIGA_TOOLCHAIN_ELF2HUNK) {
        g_ptr_array_add(cs->argv, g_strdup("-dwarf=3"));
      } else {
        g_ptr_array_add(cs->argv, g_strdup("-linedebug"));
      }
      g_ptr_array_add(cs->argv, g_strdup("-dependall=make"));
      g_ptr_array_add(cs->argv, g_strdup("-depfile"));
      g_ptr_array_add(cs->argv, g_strdup(dep_rel));
    }

    for (guint ii = 0; target->includes && ii < target->includes->len; ii++) {
      const char *inc = g_ptr_array_index(target->includes, ii);
      const char *res_inc = (g_strcmp0(inc, "$SDKDIR") == 0 && tc && tc->sdk_include_dir)
                                ? tc->sdk_include_dir : inc;
      g_ptr_array_add(cs->argv, g_strdup_printf("-I%s", res_inc));
    }
    for (guint di = 0; target->defines && di < target->defines->len; di++) {
      g_ptr_array_add(cs->argv, g_strdup_printf("-D%s", (const char *)g_ptr_array_index(target->defines, di)));
    }
    g_ptr_array_add(cs->argv, g_strdup("-o"));
    g_ptr_array_add(cs->argv, g_strdup(obj_rel));
    g_ptr_array_add(cs->argv, g_strdup(src));

    g_ptr_array_add(steps, cs);
    g_ptr_array_add(objs, g_strdup(obj_rel));
  }

  g_autofree char *elf_rel = g_strdup_printf("out/%s.elf", target->name);
  g_autofree char *exe_rel = target->exe_path ? g_strdup(target->exe_path) : g_strdup_printf("out/%s.exe", target->name);

  if (target->toolchain == AMIGA_TOOLCHAIN_ELF2HUNK) {
    AmigaBuildStep *ls = step_new(AMIGA_STEP_SHELL_CMD, "Link ELF", root);
    ls->out_artifact = g_strdup(elf_rel);
    g_ptr_array_add(ls->argv, g_strdup(ld));
    g_ptr_array_add(ls->argv, g_strdup("--emit-relocs"));
    g_ptr_array_add(ls->argv, g_strdup("-Ttext=0"));
    g_ptr_array_add(ls->argv, g_strdup_printf("-Map=out/%s.map", target->name));
    g_ptr_array_add(ls->argv, g_strdup("-o"));
    g_ptr_array_add(ls->argv, g_strdup(elf_rel));
    for (guint oi = 0; oi < objs->len; oi++) {
      g_ptr_array_add(ls->argv, g_strdup(g_ptr_array_index(objs, oi)));
    }
    g_ptr_array_add(steps, ls);

    AmigaBuildStep *e2s = step_new(AMIGA_STEP_SHELL_CMD, "elf2hunk", root);
    e2s->out_artifact = g_strdup(exe_rel);
    g_ptr_array_add(e2s->argv, g_strdup(e2h));
    g_ptr_array_add(e2s->argv, g_strdup(elf_rel));
    g_ptr_array_add(e2s->argv, g_strdup(exe_rel));
    g_ptr_array_add(steps, e2s);
  } else if (target->toolchain == AMIGA_TOOLCHAIN_HUNK_VLINK) {
    AmigaBuildStep *vl = step_new(AMIGA_STEP_SHELL_CMD, "vlink AmigaHunk", root);
    vl->out_artifact = g_strdup(exe_rel);
    g_ptr_array_add(vl->argv, g_strdup(vlnk));
    g_ptr_array_add(vl->argv, g_strdup("-bamigahunk"));
    g_ptr_array_add(vl->argv, g_strdup("-o"));
    g_ptr_array_add(vl->argv, g_strdup(exe_rel));
    for (guint oi = 0; oi < objs->len; oi++) {
      g_ptr_array_add(vl->argv, g_strdup(g_ptr_array_index(objs, oi)));
    }
    g_ptr_array_add(steps, vl);
  }

  /* Final Packaging Step (DH0: dir or in-process ADFlib .adf) */
  if (staging_root != NULL) {
    if (target->kind == AMIGA_TARGET_OS_EXE) {
      AmigaBuildStep *ps = step_new(AMIGA_STEP_STAGE_DH0, "Stage DH0:", root);
      ps->out_artifact = g_build_filename(staging_root, "dh0", NULL);
      g_ptr_array_add(steps, ps);
    } else if (target->kind == AMIGA_TARGET_ADF_DOS) {
      AmigaBuildStep *ps = step_new(AMIGA_STEP_PACK_ADF_DOS, "Pack ADF (ADFlib)", root);
      ps->out_artifact = g_strdup_printf("%s/out/%s.adf", root, target->name);
      g_ptr_array_add(steps, ps);
    } else {
      AmigaBuildStep *ps = step_new(AMIGA_STEP_PACK_ADF_RAW, "Pack Raw ADF", root);
      ps->out_artifact = g_strdup_printf("%s/out/%s.adf", root, target->name);
      g_ptr_array_add(steps, ps);
    }
  }

  *out_steps = steps;
  return true;
}

bool
amiga_build_parse_diagnostic(const char *line, AmigaBuildDiag *out_diag)
{
  if (!line || !out_diag) return false;
  memset(out_diag, 0, sizeof(*out_diag));

  /* 1. GCC style: `src/main.c:12:5: error: ...` or `warning:` */
  const char *p = strchr(line, ':');
  if (p && isdigit((unsigned char)p[1])) {
    char *end1 = NULL;
    long ln = strtol(p + 1, &end1, 10);
    if (ln > 0 && end1 && *end1 == ':') {
      long col = 1;
      const char *msg_start = end1 + 1;
      if (isdigit((unsigned char)msg_start[0])) {
        char *end2 = NULL;
        col = strtol(msg_start, &end2, 10);
        if (end2 && *end2 == ':') msg_start = end2 + 1;
      }
      while (*msg_start == ' ') msg_start++;
      AmigaBuildDiagSeverity sev = AMIGA_DIAG_ERROR;
      if (g_str_has_prefix(msg_start, "warning:")) { sev = AMIGA_DIAG_WARNING; msg_start += 8; }
      else if (g_str_has_prefix(msg_start, "note:")) { sev = AMIGA_DIAG_INFO; msg_start += 5; }
      else if (g_str_has_prefix(msg_start, "error:")) { sev = AMIGA_DIAG_ERROR; msg_start += 6; }
      while (*msg_start == ' ') msg_start++;

      out_diag->file = g_strndup(line, (gsize)(p - line));
      out_diag->line = (guint)ln;
      out_diag->col = (guint)MAX(1, col);
      out_diag->severity = sev;
      out_diag->message = g_strdup(msg_start);
      g_strchomp(out_diag->message);
      return true;
    }
  }

  /* 2. vasmm68k_mot style: `error 3007 in line 42 of "effects/bobs.asm": ...` */
  const char *in_line = strstr(line, "in line ");
  if (in_line != NULL) {
    bool is_err = (strstr(line, "error") != NULL || strstr(line, "fatal") != NULL);
    bool is_warn = (strstr(line, "warning") != NULL);
    if (is_err || is_warn) {
      long ln = strtol(in_line + 8, NULL, 10);
      const char *q1 = strchr(in_line, '"');
      const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
      if (ln > 0 && q1 && q2 && q2 > q1 + 1) {
        const char *colon2 = strchr(q2, ':');
        const char *msg = colon2 ? colon2 + 1 : (q2 + 1);
        while (*msg == ' ') msg++;
        out_diag->file = g_strndup(q1 + 1, (gsize)(q2 - q1 - 1));
        out_diag->line = (guint)ln;
        out_diag->col = 1;
        out_diag->severity = is_warn ? AMIGA_DIAG_WARNING : AMIGA_DIAG_ERROR;
        out_diag->message = g_strdup(msg);
        g_strchomp(out_diag->message);
        return true;
      }
    }
  }

  return false;
}

bool
amiga_build_stage_dh0(const AmigaProject *proj,
                      const AmigaTarget  *target,
                      const char         *exe_path,
                      const char         *dh0_dir,
                      GError            **error)
{
  if (!target || !exe_path || !dh0_dir) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid DH0 staging args");
    return false;
  }
  g_autofree char *s_dir = g_build_filename(dh0_dir, "s", NULL);
  g_mkdir_with_parents(s_dir, 0755);

  g_autofree char *exe_base = g_strdup_printf("%s.exe", target->name);
  g_autofree char *dst_exe = g_build_filename(dh0_dir, exe_base, NULL);
  g_autofree char *exe_bytes = NULL;
  gsize exe_len = 0;
  if (!g_file_get_contents(exe_path, &exe_bytes, &exe_len, error)) {
    return false;
  }
  if (!g_file_set_contents(dst_exe, exe_bytes, (gssize)exe_len, error)) {
    return false;
  }

  const char *root = (proj && proj->root_dir) ? proj->root_dir : ".";
  for (guint i = 0; target->extra_files && i < target->extra_files->len; i++) {
    const char *rel = g_ptr_array_index(target->extra_files, i);
    g_autofree char *src = g_build_filename(root, rel, NULL);
    g_autofree char *base = g_path_get_basename(rel);
    g_autofree char *dst = g_build_filename(dh0_dir, base, NULL);
    g_autofree char *buf = NULL;
    gsize len = 0;
    if (!g_file_get_contents(src, &buf, &len, error)) return false;
    if (!g_file_set_contents(dst, buf, (gssize)len, error)) return false;
  }

  g_autofree char *startup = target->startup_sequence
                                 ? g_strdup(target->startup_sequence)
                                 : g_strdup_printf("DH0:%s\n", exe_base);
  g_autofree char *startup_path = g_build_filename(s_dir, "startup-sequence", NULL);
  return g_file_set_contents(startup_path, startup, -1, error);
}

typedef struct {
  AmigaProject         *proj;
  AmigaTarget          *target;
  GPtrArray            *steps;
  guint                 cur_step;
  GString              *log;
  GPtrArray            *diags;
  char                 *artifact_path;
} AsyncBuildCtx;

static void
async_build_ctx_free(gpointer data)
{
  AsyncBuildCtx *ctx = data;
  if (!ctx) return;
  amiga_project_free(ctx->proj);
  amiga_target_free(ctx->target);
  if (ctx->steps) g_ptr_array_unref(ctx->steps);
  if (ctx->log) g_string_free(ctx->log, TRUE);
  if (ctx->diags) g_ptr_array_unref(ctx->diags);
  g_free(ctx->artifact_path);
  g_free(ctx);
}

static void run_next_step(GTask *task);

static void
finish_build_task(GTask *task, bool success, GError *err)
{
  if (err) {
    g_task_return_error(task, err);
    g_object_unref(task);
    return;
  }
  AsyncBuildCtx *ctx = g_task_get_task_data(task);
  AmigaBuildResult *res = g_new0(AmigaBuildResult, 1);
  res->success = success;
  res->log_text = g_string_free(g_steal_pointer(&ctx->log), FALSE);
  res->diagnostics = g_steal_pointer(&ctx->diags);
  res->artifact_path = g_steal_pointer(&ctx->artifact_path);
  g_task_return_pointer(task, res, (GDestroyNotify)amiga_build_result_free);
  g_object_unref(task);
}

static void
on_subprocess_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  GSubprocess *proc = G_SUBPROCESS(source);
  AsyncBuildCtx *ctx = g_task_get_task_data(task);

  g_autofree char *out_buf = NULL;
  g_autofree char *err_buf = NULL;
  g_autoptr(GError) err = NULL;
  if (!g_subprocess_communicate_utf8_finish(proc, res, &out_buf, &err_buf, &err)) {
    finish_build_task(task, false, g_steal_pointer(&err));
    return;
  }

  if (out_buf && *out_buf) g_string_append(ctx->log, out_buf);
  if (err_buf && *err_buf) {
    g_string_append(ctx->log, err_buf);
    g_auto(GStrv) lines = g_strsplit(err_buf, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
      AmigaBuildDiag d;
      if (amiga_build_parse_diagnostic(lines[i], &d)) {
        g_ptr_array_add(ctx->diags, g_memdup2(&d, sizeof(d)));
      }
    }
  }

  if (!g_subprocess_get_successful(proc)) {
    finish_build_task(task, false, NULL);
    return;
  }

  AmigaBuildStep *step = g_ptr_array_index(ctx->steps, ctx->cur_step);
  if (step->out_artifact) {
    g_free(ctx->artifact_path);
    ctx->artifact_path = g_strdup(step->out_artifact);
  }
  ctx->cur_step++;
  run_next_step(task);
}

static void
run_next_step(GTask *task)
{
  AsyncBuildCtx *ctx = g_task_get_task_data(task);
  GCancellable *canc = g_task_get_cancellable(task);
  if (canc && g_cancellable_is_cancelled(canc)) {
    finish_build_task(task, false, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Build cancelled"));
    return;
  }

  while (ctx->cur_step < ctx->steps->len) {
    AmigaBuildStep *step = g_ptr_array_index(ctx->steps, ctx->cur_step);
    if (step->skipped_up_to_date) {
      g_string_append_printf(ctx->log, "[skip up-to-date] %s\n", step->label ? step->label : "");
      ctx->cur_step++;
      continue;
    }

    if (step->kind == AMIGA_STEP_STAGE_DH0) {
      g_autoptr(GError) err = NULL;
      const char *root = ctx->proj->root_dir ? ctx->proj->root_dir : ".";
      g_autofree char *exe = ctx->target->exe_path
                                 ? g_build_filename(root, ctx->target->exe_path, NULL)
                                 : g_strdup_printf("%s/out/%s.exe", root, ctx->target->name);
      if (!amiga_build_stage_dh0(ctx->proj, ctx->target, exe, step->out_artifact, &err)) {
        g_string_append_printf(ctx->log, "DH0 staging failed: %s\n", err->message);
        finish_build_task(task, false, NULL);
        return;
      }
      g_free(ctx->artifact_path);
      ctx->artifact_path = g_strdup(step->out_artifact);
      ctx->cur_step++;
      continue;
    }

    if (step->kind == AMIGA_STEP_PACK_ADF_DOS) {
      const char *root = ctx->proj->root_dir ? ctx->proj->root_dir : ".";
      g_autofree char *exe = g_strdup_printf("%s/out/%s.exe", root, ctx->target->name);
      g_autofree char *startup = g_strdup_printf("DF0:%s.exe\n", ctx->target->name);
      AmigaAdfFileEntry files[2] = {
        { .guest_path = "s/startup-sequence", .bytes = (const uint8_t *)startup, .bytes_len = strlen(startup) },
        { .guest_path = ctx->target->name, .host_path = exe },
      };
      AmigaAdfDosSpec spec = {
        .adf_path = step->out_artifact,
        .volume_name = ctx->target->name,
        .ffs = false,
        .bootable = true,
        .files = files,
        .n_files = 2,
      };
      g_autoptr(GError) err = NULL;
      if (!amiga_adf_create_dos(&spec, &err)) {
        g_string_append_printf(ctx->log, "ADFlib pack failed: %s\n", err->message);
        finish_build_task(task, false, NULL);
        return;
      }
      g_free(ctx->artifact_path);
      ctx->artifact_path = g_strdup(step->out_artifact);
      ctx->cur_step++;
      continue;
    }

    if (step->kind == AMIGA_STEP_PACK_ADF_RAW) {
      const char *root = ctx->proj->root_dir ? ctx->proj->root_dir : ".";
      g_autofree char *exe = g_strdup_printf("%s/out/%s.exe", root, ctx->target->name);
      g_autofree char *payload = NULL;
      gsize payload_len = 0;
      g_autoptr(GError) err = NULL;
      if (!g_file_get_contents(exe, &payload, &payload_len, &err) ||
          !amiga_adf_create_raw(step->out_artifact, NULL, 0,
                                (const uint8_t *)payload, payload_len, &err)) {
        g_string_append_printf(ctx->log, "Raw ADF pack failed: %s\n", err ? err->message : "error");
        finish_build_task(task, false, NULL);
        return;
      }
      g_free(ctx->artifact_path);
      ctx->artifact_path = g_strdup(step->out_artifact);
      ctx->cur_step++;
      continue;
    }

    /* Shell/Compiler GSubprocess step */
    g_autofree char *obj_dir = g_build_filename(step->cwd ? step->cwd : ".", "obj", NULL);
    g_autofree char *out_dir = g_build_filename(step->cwd ? step->cwd : ".", "out", NULL);
    g_mkdir_with_parents(obj_dir, 0755);
    g_mkdir_with_parents(out_dir, 0755);

    g_ptr_array_add(step->argv, NULL);
    g_autoptr(GSubprocessLauncher) launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    if (step->cwd) {
      g_subprocess_launcher_set_cwd(launcher, step->cwd);
    }
    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) proc =
        g_subprocess_launcher_spawnv(launcher, (const char *const *)step->argv->pdata, &err);
    g_ptr_array_remove_index(step->argv, step->argv->len - 1);
    if (!proc) {
      g_string_append_printf(ctx->log, "Spawn failed (%s): %s\n", step->label ? step->label : "", err->message);
      finish_build_task(task, false, NULL);
      return;
    }
    g_string_append_printf(ctx->log, "→ %s\n", step->label ? step->label : "");
    g_subprocess_communicate_utf8_async(proc, NULL, canc, on_subprocess_done, task);
    return;
  }

  finish_build_task(task, true, NULL);
}

static AmigaProject *
dup_project_minimal(const AmigaProject *src)
{
  g_autofree char *txt = amiga_project_serialize_toml(src);
  AmigaProject *cp = NULL;
  amiga_project_parse_toml(txt, src->root_dir, &cp, NULL);
  return cp ? cp : amiga_project_new();
}

static AmigaTarget *
dup_target_minimal(const AmigaTarget *src)
{
  AmigaTarget *t = amiga_target_new(src->name);
  t->kind = src->kind;
  t->toolchain = src->toolchain;
  g_free(t->cpu); t->cpu = g_strdup(src->cpu);
  for (guint i = 0; src->sources && i < src->sources->len; i++) g_ptr_array_add(t->sources, g_strdup(g_ptr_array_index(src->sources, i)));
  for (guint i = 0; src->includes && i < src->includes->len; i++) g_ptr_array_add(t->includes, g_strdup(g_ptr_array_index(src->includes, i)));
  for (guint i = 0; src->defines && i < src->defines->len; i++) g_ptr_array_add(t->defines, g_strdup(g_ptr_array_index(src->defines, i)));
  for (guint i = 0; src->extra_files && i < src->extra_files->len; i++) g_ptr_array_add(t->extra_files, g_strdup(g_ptr_array_index(src->extra_files, i)));
  t->startup_sequence = g_strdup(src->startup_sequence);
  t->exe_path = g_strdup(src->exe_path);
  return t;
}

void
amiga_build_run_async(const AmigaProject         *proj,
                      const AmigaTarget          *target,
                      const AmigaToolchainStatus *tc,
                      const char                 *staging_root,
                      GCancellable               *cancellable,
                      GAsyncReadyCallback         callback,
                      gpointer                    user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  GError *err = NULL;
  GPtrArray *steps = NULL;
  if (!amiga_build_plan_steps(proj, target, tc, staging_root, &steps, &err)) {
    g_task_return_error(task, err);
    g_object_unref(task);
    return;
  }

  AsyncBuildCtx *ctx = g_new0(AsyncBuildCtx, 1);
  ctx->proj = dup_project_minimal(proj);
  ctx->target = dup_target_minimal(target);
  ctx->steps = steps;
  ctx->log = g_string_new("");
  ctx->diags = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_build_diag_free);
  g_task_set_task_data(task, ctx, async_build_ctx_free);

  run_next_step(task);
}

AmigaBuildResult *
amiga_build_run_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}
