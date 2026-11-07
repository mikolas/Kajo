#include "ide_native_exe.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

static gboolean
is_regular_executable(const char *path)
{
  struct stat st;
  if (g_stat(path, (GStatBuf *)&st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & S_IXUSR) == 0) {
    return FALSE;
  }
  /* ELF magic: skips scripts and shared objects named like executables */
  FILE *f = g_fopen(path, "rb");
  if (f == NULL) {
    return FALSE;
  }
  unsigned char magic[4] = { 0 };
  size_t n = fread(magic, 1, 4, f);
  fclose(f);
  return n == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

/* ─── discovery of the debuggee ─── */

static char *
meson_executable_for(const char *ws, const char *file)
{
  const char *bds[] = { "builddir", "build", NULL };
  for (gsize b = 0; bds[b] != NULL; b++) {
    g_autofree char *intro = g_build_filename(ws, bds[b], "meson-info", "intro-targets.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, intro, NULL)) {
      continue;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root)) {
      continue;
    }
    g_autofree char *file_dir = file ? g_path_get_dirname(file) : NULL;
    char *best = NULL;
    int best_score = -1;
    JsonArray *arr = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
      JsonObject *t = json_array_get_object_element(arr, i);
      if (t == NULL || g_strcmp0(json_object_get_string_member_with_default(t, "type", ""), "executable") != 0) {
        continue;
      }
      JsonArray *fn = json_object_get_array_member(t, "filename");
      if (fn == NULL || json_array_get_length(fn) == 0) {
        continue;
      }
      const char *exe = json_array_get_string_element(fn, 0);
      if (exe == NULL || !g_file_test(exe, G_FILE_TEST_EXISTS)) {
        continue;
      }
      int score = 0;
      JsonArray *srcs = json_object_get_array_member(t, "target_sources");
      for (guint s = 0; srcs != NULL && s < json_array_get_length(srcs); s++) {
        JsonObject *so = json_array_get_object_element(srcs, s);
        JsonArray *files = so ? json_object_get_array_member(so, "sources") : NULL;
        for (guint k = 0; files != NULL && k < json_array_get_length(files); k++) {
          const char *sp = json_array_get_string_element(files, k);
          if (sp == NULL || file == NULL) {
            continue;
          }
          if (g_strcmp0(sp, file) == 0) {
            score = MAX(score, 2);
          } else if (file_dir != NULL) {
            g_autofree char *sd = g_path_get_dirname(sp);
            if (g_strcmp0(sd, file_dir) == 0) {
              score = MAX(score, 1);
            }
          }
        }
      }
      if (score > best_score) {
        best_score = score;
        g_free(best);
        best = g_strdup(exe);
      }
    }
    if (best != NULL) {
      return best;
    }
  }
  return NULL;
}

/* Newest ELF executable under `dir` (depth <= 2), skipping CMake internals. */
static char *
newest_executable_in(const char *dir, int depth)
{
  g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
  if (d == NULL) {
    return NULL;
  }
  char *best = NULL;
  gint64 best_mtime = -1;
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL) {
    if (g_strcmp0(name, "CMakeFiles") == 0 || g_str_has_suffix(name, ".p") || g_str_has_suffix(name, ".so") ||
        g_str_has_prefix(name, ".")) {
      continue;
    }
    g_autofree char *path = g_build_filename(dir, name, NULL);
    char *cand = NULL;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      if (depth > 0) {
        cand = newest_executable_in(path, depth - 1);
      }
    } else if (is_regular_executable(path)) {
      cand = g_strdup(path);
    }
    if (cand != NULL) {
      GStatBuf st;
      gint64 mt = g_stat(cand, &st) == 0 ? (gint64)st.st_mtime : 0;
      if (mt > best_mtime) {
        best_mtime = mt;
        g_free(best);
        best = cand;
      } else {
        g_free(cand);
      }
    }
  }
  return best;
}

static char *
cargo_package_name(const char *ws)
{
  g_autofree char *toml = g_build_filename(ws, "Cargo.toml", NULL);
  g_autofree char *contents = NULL;
  if (!g_file_get_contents(toml, &contents, NULL, NULL)) {
    return NULL;
  }
  g_autoptr(GRegex) rx = g_regex_new("^\\s*name\\s*=\\s*\"([^\"]+)\"", G_REGEX_MULTILINE, 0, NULL);
  g_autoptr(GMatchInfo) mi = NULL;
  if (rx != NULL && g_regex_match(rx, contents, 0, &mi)) {
    return g_match_info_fetch(mi, 1);
  }
  return NULL;
}


char *
ide_find_native_executable(const char *ws, const char *active_file)
{
  char *program = NULL;
  g_autofree char *cargo = g_build_filename(ws, "Cargo.toml", NULL);
  if (g_file_test(cargo, G_FILE_TEST_EXISTS)) {
    g_autofree char *name = cargo_package_name(ws);
    if (name != NULL) {
      program = g_build_filename(ws, "target", "debug", name, NULL);
      if (!g_file_test(program, G_FILE_TEST_IS_EXECUTABLE)) {
        g_clear_pointer(&program, g_free);
      }
    }
  }
  if (program == NULL) {
    program = meson_executable_for(ws, active_file);
  }
  if (program == NULL && active_file != NULL) {
    /* single-file programs: prog.c -> ./prog (also under common build dirs) */
    g_autofree char *base = g_path_get_basename(active_file);
    char *dot = strrchr(base, '.');
    if (dot != NULL) {
      *dot = '\0';
    }
    g_autofree char *src_dir = g_path_get_dirname(active_file);
    const char *where[] = { src_dir, ws, NULL };
    for (gsize i = 0; where[i] != NULL && program == NULL; i++) {
      g_autofree char *cand = g_build_filename(where[i], base, NULL);
      if (is_regular_executable(cand)) {
        program = g_steal_pointer(&cand);
      }
    }
  }
  if (program == NULL) {
    const char *dirs[] = { "builddir", "build", "bin", "cmake-build-debug", NULL };
    for (gsize i = 0; dirs[i] != NULL && program == NULL; i++) {
      g_autofree char *d = g_build_filename(ws, dirs[i], NULL);
      program = newest_executable_in(d, 2);
    }
  }
  if (program == NULL) {
    /* Makefile projects often drop the binary in the root, named like the folder */
    g_autofree char *ws_name = g_path_get_basename(ws);
    g_autofree char *cand = g_build_filename(ws, ws_name, NULL);
    if (is_regular_executable(cand)) {
      program = g_steal_pointer(&cand);
    }
  }
  return program;
}
