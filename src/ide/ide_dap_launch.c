#include "ide_dap_launch.h"
#include "ide_lsp_servers.h"
#include "ide_native_exe.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

const char *
ide_dap_detect_adapter_for_file(const char *file_path)
{
  if (file_path == NULL || file_path[0] == '\0') {
    return "none";
  }
  if (g_str_has_suffix(file_path, ".go")) {
    return "dlv dap";
  }
  if (g_str_has_suffix(file_path, ".java")) {
    return "java-debug (jdtls)";
  }
  if (g_str_has_suffix(file_path, ".c") ||
      g_str_has_suffix(file_path, ".cc") ||
      g_str_has_suffix(file_path, ".cpp") ||
      g_str_has_suffix(file_path, ".cxx") ||
      g_str_has_suffix(file_path, ".h") ||
      g_str_has_suffix(file_path, ".hpp") ||
      g_str_has_suffix(file_path, ".rs")) {
    return "gdb -i=dap";
  }
  return "none";
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
  char *program = ide_find_native_executable(ws, active_file);
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
