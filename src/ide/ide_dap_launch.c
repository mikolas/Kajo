#include "ide_dap_launch.h"
#include "ide_lsp_servers.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

const char *
ide_dap_detect_adapter_for_file(const char *file_path)
{
  if (file_path == NULL || file_path[0] == '\0') {
    return "gdb -i=dap";
  }
  if (g_str_has_suffix(file_path, ".go")) {
    return "dlv dap";
  }
  if (g_str_has_suffix(file_path, ".java")) {
    return "java-debug (jdtls)";
  }
  if (g_str_has_suffix(file_path, ".py")) {
    return "python3 -m debugpy.adapter";
  }
  return "gdb -i=dap";
}

void
ide_dap_launch_free(IdeDapLaunch *l)
{
  if (l == NULL) {
    return;
  }
  g_free(l->adapter_label);
  g_strfreev(l->adapter_argv);
  g_free(l->request);
  if (l->arguments != NULL) {
    json_node_unref(l->arguments);
  }
  g_free(l->program);
  g_free(l->cwd);
  g_free(l->error);
  g_free(l);
}

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

/* ─── adapters ─── */

static char *
find_tool(const char *name)
{
  char *p = g_find_program_in_path(name);
  if (p != NULL) {
    return p;
  }
  /* common non-PATH install locations */
  g_autofree char *gobin = g_build_filename(g_get_home_dir(), "go", "bin", name, NULL);
  if (g_file_test(gobin, G_FILE_TEST_IS_EXECUTABLE)) {
    return g_steal_pointer(&gobin);
  }
  return NULL;
}

static gboolean
set_native_adapter(IdeDapLaunch *l)
{
  g_autofree char *gdb = find_tool("gdb");
  if (gdb != NULL) {
    l->adapter_label = g_strdup("gdb -i=dap");
    l->adapter_argv = g_new0(char *, 3);
    l->adapter_argv[0] = g_steal_pointer(&gdb);
    l->adapter_argv[1] = g_strdup("-i=dap");
    return TRUE;
  }
  g_autofree char *lldb = find_tool("lldb-dap");
  if (lldb == NULL) {
    lldb = find_tool("lldb-vscode");
  }
  if (lldb != NULL) {
    l->adapter_label = g_strdup("lldb-dap");
    l->adapter_argv = g_new0(char *, 2);
    l->adapter_argv[0] = g_steal_pointer(&lldb);
    return TRUE;
  }
  l->error = g_strdup("No native debug adapter found: install gdb (14+, `gdb -i=dap`) or lldb-dap");
  return FALSE;
}

static JsonNode *
launch_args(const char *program, const char *cwd, char **args, gboolean stop_at_main, gboolean gdb)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "program");
  json_builder_add_string_value(b, program);
  json_builder_set_member_name(b, "args");
  json_builder_begin_array(b);
  for (gsize i = 0; args != NULL && args[i] != NULL; i++) {
    json_builder_add_string_value(b, args[i]);
  }
  json_builder_end_array(b);
  json_builder_set_member_name(b, "cwd");
  json_builder_add_string_value(b, cwd);
  if (stop_at_main) {
    json_builder_set_member_name(b, gdb ? "stopAtBeginningOfMainSubprogram" : "stopOnEntry");
    json_builder_add_boolean_value(b, TRUE);
  }
  json_builder_end_object(b);
  return json_builder_get_root(b);
}

/* .kajo/debug.json: {"adapter":"gdb|lldb-dap|dlv","program":"…","args":[…],"cwd":"…"} */
static IdeDapLaunch *
from_config(const char *ws, gboolean has_bp)
{
  g_autofree char *cfg = g_build_filename(ws, ".kajo", "debug.json", NULL);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!g_file_test(cfg, G_FILE_TEST_EXISTS)) {
    return NULL;
  }
  IdeDapLaunch *l = g_new0(IdeDapLaunch, 1);
  l->request = g_strdup("launch");
  if (!json_parser_load_from_file(parser, cfg, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    l->error = g_strdup(".kajo/debug.json is not valid JSON");
    return l;
  }
  JsonObject *o = json_node_get_object(json_parser_get_root(parser));
  const char *prog = json_object_get_string_member_with_default(o, "program", NULL);
  if (prog == NULL) {
    l->error = g_strdup(".kajo/debug.json needs a \"program\"");
    return l;
  }
  l->program = g_path_is_absolute(prog) ? g_strdup(prog) : g_build_filename(ws, prog, NULL);
  const char *cwd = json_object_get_string_member_with_default(o, "cwd", NULL);
  l->cwd = cwd ? (g_path_is_absolute(cwd) ? g_strdup(cwd) : g_build_filename(ws, cwd, NULL)) : g_strdup(ws);

  g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
  JsonArray *ja = json_object_has_member(o, "args") ? json_object_get_array_member(o, "args") : NULL;
  for (guint i = 0; ja != NULL && i < json_array_get_length(ja); i++) {
    g_ptr_array_add(args, g_strdup(json_array_get_string_element(ja, i)));
  }
  g_ptr_array_add(args, NULL);

  const char *adapter = json_object_get_string_member_with_default(o, "adapter", "gdb");
  if (g_strcmp0(adapter, "dlv") == 0) {
    g_autofree char *dlv = find_tool("dlv");
    if (dlv == NULL) {
      l->error = g_strdup("Delve (dlv) is not installed: go install github.com/go-delve/delve/cmd/dlv@latest");
      return l;
    }
    l->adapter_label = g_strdup("dlv dap");
    l->adapter_argv = g_new0(char *, 3);
    l->adapter_argv[0] = g_steal_pointer(&dlv);
    l->adapter_argv[1] = g_strdup("dap");
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mode");
    json_builder_add_string_value(b, "debug");
    json_builder_set_member_name(b, "program");
    json_builder_add_string_value(b, l->program);
    json_builder_set_member_name(b, "cwd");
    json_builder_add_string_value(b, l->cwd);
    json_builder_end_object(b);
    l->arguments = json_builder_get_root(b);
    return l;
  }
  if (!set_native_adapter(l)) {
    return l;
  }
  l->arguments = launch_args(l->program, l->cwd, (char **)args->pdata, !has_bp, g_str_has_prefix(l->adapter_label, "gdb"));
  return l;
}

IdeDapLaunch *
ide_dap_launch_resolve(const char *workspace_root, const char *active_file, gboolean has_breakpoints)
{
  const char *ws = workspace_root ? workspace_root : ".";

  IdeDapLaunch *cfg = from_config(ws, has_breakpoints);
  if (cfg != NULL) {
    return cfg;
  }

  IdeDapLaunch *l = g_new0(IdeDapLaunch, 1);
  l->request = g_strdup("launch");
  l->cwd = g_strdup(ws);

  if (active_file != NULL && g_str_has_suffix(active_file, ".go")) {
    g_autofree char *dlv = find_tool("dlv");
    if (dlv == NULL) {
      l->error = g_strdup("Delve (dlv) is not installed: go install github.com/go-delve/delve/cmd/dlv@latest");
      return l;
    }
    l->adapter_label = g_strdup("dlv dap");
    l->adapter_argv = g_new0(char *, 3);
    l->adapter_argv[0] = g_steal_pointer(&dlv);
    l->adapter_argv[1] = g_strdup("dap");
    l->program = g_path_get_dirname(active_file);
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mode");
    json_builder_add_string_value(b, g_str_has_suffix(active_file, "_test.go") ? "test" : "debug");
    json_builder_set_member_name(b, "program");
    json_builder_add_string_value(b, l->program);
    json_builder_set_member_name(b, "cwd");
    json_builder_add_string_value(b, ws);
    json_builder_end_object(b);
    l->arguments = json_builder_get_root(b);
    return l;
  }

  if (active_file != NULL && g_str_has_suffix(active_file, ".java")) {
    g_autofree char *jar = ide_lsp_find_java_debug_jar();
    if (jar == NULL) {
      l->error = g_strdup("Java debugging needs the java-debug bundle for jdtls: run scripts/install-java-debug.sh, then restart the IDE");
      return l;
    }
    l->java = TRUE;
    l->adapter_label = g_strdup("java-debug (jdtls)");
    l->program = g_strdup(active_file);
    return l;
  }

  /* C / C++ / Rust (and anything else native): find a built executable */
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
    const char *dirs[] = { "builddir", "build", "bin", "cmake-build-debug", ".", NULL };
    for (gsize i = 0; dirs[i] != NULL && program == NULL; i++) {
      g_autofree char *d = g_build_filename(ws, dirs[i], NULL);
      program = newest_executable_in(d, g_strcmp0(dirs[i], ".") == 0 ? 0 : 2);
    }
  }
  if (program == NULL) {
    l->error = g_strdup("No built executable found: build the project first (Space r b) or add .kajo/debug.json");
    return l;
  }

  if (!set_native_adapter(l)) {
    g_free(program);
    return l;
  }
  l->program = program;
  l->wants_stop_at_main = !has_breakpoints;
  l->arguments = launch_args(program, ws, NULL, !has_breakpoints, g_str_has_prefix(l->adapter_label, "gdb"));
  return l;
}
