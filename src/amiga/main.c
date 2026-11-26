#include "amiga_build.h"
#include "amiga_project.h"
#include "amiga_window.h"

#include "../app_init.h"
#include "../ide/ide_buffer.h"
#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ShellTheme *g_amiga_theme = NULL;

static void
ensure_amiga_theme(void)
{
  if (!g_amiga_theme) {
    gtk_source_init();
    ide_buffer_register_scheme_paths();
    g_amiga_theme = kajo_app_init_theme();
  }
}

typedef struct {
  int   exit_code;
  bool  done;
} CliBuildCtx;

static void
on_cli_build_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer ud)
{
  CliBuildCtx *c = ud;
  g_autoptr(GError) err = NULL;
  AmigaBuildResult *br = amiga_build_run_finish(res, &err);
  if (err) {
    g_printerr("Build error: %s\n", err->message);
    c->exit_code = 1;
  } else {
    if (br && br->log_text && br->log_text[0]) {
      g_print("%s\n", br->log_text);
    }
    c->exit_code = (br && br->success) ? 0 : 1;
    amiga_build_result_free(br);
  }
  c->done = true;
}

static int
run_headless_cli_build(const char *dir, const char *target_name)
{
  g_autoptr(GError) err = NULL;
  AmigaProject *proj = amiga_project_load_dir(dir ? dir : ".", &err);
  if (!proj) {
    g_printerr("kajo-amiga --build: %s\n", err ? err->message : "project not found");
    return 1;
  }
  const AmigaTarget *t = target_name ? amiga_project_find_target(proj, target_name) : NULL;
  if (!t && proj->default_target) t = amiga_project_find_target(proj, proj->default_target);
  if (!t && proj->targets->len > 0) t = g_ptr_array_index(proj->targets, 0);
  if (!t) {
    g_printerr("kajo-amiga --build: no matching target\n");
    amiga_project_free(proj);
    return 1;
  }

  AmigaToolchainStatus *tc = amiga_project_probe_toolchain(NULL);
  g_autofree char *staging = g_build_filename(
      g_get_user_cache_dir(), "kajo", "amiga",
      proj->name ? proj->name : "default", NULL);

  CliBuildCtx ctx = { .exit_code = 1, .done = false };
  amiga_build_run_async(proj, t, tc, staging, NULL, on_cli_build_done, &ctx);
  while (!ctx.done) {
    g_main_context_iteration(NULL, TRUE);
  }

  amiga_toolchain_status_free(tc);
  amiga_project_free(proj);
  return ctx.exit_code;
}

typedef struct {
  char *attach_hp;
  char *preview_mode;
} AppCliOpts;

static void
on_app_activate(GtkApplication *app, gpointer ud)
{
  AppCliOpts *opts = ud;
  ensure_amiga_theme();
  KajoAmigaWindow *win = kajo_amiga_window_new(app);
  if (opts->preview_mode) {
    kajo_amiga_window_set_preview_mode(win, opts->preview_mode);
  } else if (opts->attach_hp) {
    g_autofree char *host = g_strdup(opts->attach_hp);
    uint16_t port = 2345;
    char *colon = strrchr(host, ':');
    if (colon) {
      *colon = '\0';
      port = (uint16_t)strtoul(colon + 1, NULL, 10);
    }
    kajo_amiga_window_open_attach(win, host, port ? port : 2345);
  }
  gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_open(GtkApplication *app,
            GFile         **files,
            gint            n_files,
            const gchar    *hint G_GNUC_UNUSED,
            gpointer        ud G_GNUC_UNUSED)
{
  ensure_amiga_theme();
  KajoAmigaWindow *win = kajo_amiga_window_new(app);
  if (n_files > 0 && files[0]) {
    g_autofree char *p = g_file_get_path(files[0]);
    if (p) kajo_amiga_window_open_project_dir(win, p);
  }
  gtk_window_present(GTK_WINDOW(win));
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED)
{
  kajo_app_shutdown_theme(g_amiga_theme);
  g_amiga_theme = NULL;
}

int
main(int argc, char *argv[])
{
  /* Check headless --build before spinning up GTK/Wayland display */
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--build") == 0) {
      const char *target = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : NULL;
      const char *dir = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : ".";
      return run_headless_cli_build(dir, target);
    }
  }

  AppCliOpts opts = {0};
  GPtrArray *filtered = g_ptr_array_new();
  g_ptr_array_add(filtered, argv[0]);
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--attach") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') opts.attach_hp = g_strdup(argv[++i]);
      else opts.attach_hp = g_strdup("127.0.0.1:2345");
    } else if (g_strcmp0(argv[i], "--preview-mode") == 0 && i + 1 < argc) {
      opts.preview_mode = g_strdup(argv[++i]);
    } else {
      g_ptr_array_add(filtered, argv[i]);
    }
  }
  g_ptr_array_add(filtered, NULL);

  g_set_prgname("org.kajo.Amiga");
  g_set_application_name("Kajo Amiga");

  g_autoptr(GtkApplication) app = gtk_application_new(
      "org.kajo.Amiga", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), &opts);
  g_signal_connect(app, "open", G_CALLBACK(on_app_open), &opts);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

  int f_argc = (int)filtered->len - 1;
  char **f_argv = (char **)filtered->pdata;
  int rc = g_application_run(G_APPLICATION(app), f_argc, f_argv);

  g_ptr_array_unref(filtered);
  g_free(opts.attach_hp);
  g_free(opts.preview_mode);
  return rc;
}
