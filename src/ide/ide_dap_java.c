#include "ide_dap_java.h"

#include <string.h>

typedef struct {
  char               *file_path;
  char               *workspace_root;
  IdeJavaExecFn       exec;
  gpointer            host;
  IdeJavaDebugReadyCb ready_cb;
  gpointer            ready_data;

  char               *main_class;
  char               *project_name;
  JsonNode           *module_paths;
  JsonNode           *class_paths;
  char               *java_exec;
} Prep;

static void
prep_free(Prep *p)
{
  g_free(p->file_path);
  g_free(p->workspace_root);
  g_free(p->main_class);
  g_free(p->project_name);
  if (p->module_paths) json_node_unref(p->module_paths);
  if (p->class_paths) json_node_unref(p->class_paths);
  g_free(p->java_exec);
  g_free(p);
}

static void
fail(Prep *p, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
fail(Prep *p, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  g_autofree char *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  IdeJavaDebugReadyCb cb = p->ready_cb;
  gpointer ud = p->ready_data;
  prep_free(p);
  cb(NULL, NULL, msg, ud);
}

/* jdtls answers an unknown command with this; it means the bundle isn't loaded. */
static gboolean
looks_like_missing_bundle(const char *error)
{
  return error != NULL && (strstr(error, "No delegateCommandHandler") != NULL ||
                           strstr(error, "command not found") != NULL ||
                           strstr(error, "Unknown command") != NULL);
}

static void
fail_command(Prep *p, const char *what, const char *error)
{
  if (looks_like_missing_bundle(error)) {
    fail(p, "jdtls has no java-debug bundle loaded (%s). Install it with scripts/install-java-debug.sh and restart the IDE", what);
  } else {
    fail(p, "%s failed: %s", what, error ? error : "no result");
  }
}

static JsonNode *
string_array_node(const char *s)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  json_builder_add_string_value(b, s);
  json_builder_end_array(b);
  return json_builder_get_root(b);
}

static JsonNode *
two_string_array(const char *a, const char *b_)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  json_builder_add_string_value(b, a);
  json_builder_add_string_value(b, b_);
  json_builder_end_array(b);
  return json_builder_get_root(b);
}

static JsonNode *
build_launch_args(Prep *p)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "mainClass");
  json_builder_add_string_value(b, p->main_class);
  json_builder_set_member_name(b, "projectName");
  json_builder_add_string_value(b, p->project_name ? p->project_name : "");
  json_builder_set_member_name(b, "classPaths");
  json_builder_add_value(b, p->class_paths ? json_node_copy(p->class_paths) : json_node_new(JSON_NODE_ARRAY));
  json_builder_set_member_name(b, "modulePaths");
  json_builder_add_value(b, p->module_paths ? json_node_copy(p->module_paths) : json_node_new(JSON_NODE_ARRAY));
  if (p->java_exec != NULL) {
    json_builder_set_member_name(b, "javaExec");
    json_builder_add_string_value(b, p->java_exec);
  }
  json_builder_set_member_name(b, "cwd");
  json_builder_add_string_value(b, p->workspace_root);
  json_builder_set_member_name(b, "args");
  json_builder_add_string_value(b, "");
  json_builder_set_member_name(b, "vmArgs");
  json_builder_add_string_value(b, "");
  json_builder_set_member_name(b, "console");
  json_builder_add_string_value(b, "internalConsole");
  json_builder_set_member_name(b, "stopOnEntry");
  json_builder_add_boolean_value(b, FALSE);
  json_builder_set_member_name(b, "noDebug");
  json_builder_add_boolean_value(b, FALSE);
  json_builder_end_object(b);
  return json_builder_get_root(b);
}

static void
on_connected(GObject *src, GAsyncResult *res, gpointer user_data)
{
  Prep *p = user_data;
  GError *error = NULL;
  GSocketConnection *conn = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &error);
  g_object_unref(src);
  if (conn == NULL) {
    fail(p, "cannot connect to the Java debug server: %s", error ? error->message : "?");
    g_clear_error(&error);
    return;
  }
  JsonNode *launch = build_launch_args(p);
  IdeJavaDebugReadyCb cb = p->ready_cb;
  gpointer ud = p->ready_data;
  prep_free(p);
  cb(G_IO_STREAM(conn), launch, NULL, ud);
}

static void
on_start_session(JsonNode *result, const char *error, gpointer user_data)
{
  Prep *p = user_data;
  gint64 port = 0;
  if (error == NULL && result != NULL && JSON_NODE_HOLDS_VALUE(result)) {
    port = json_node_get_int(result);
  }
  if (port <= 0 || port > 65535) {
    fail_command(p, "starting the Java debug session", error ? error : "jdtls returned no port");
    return;
  }
  GSocketClient *client = g_socket_client_new();
  g_socket_client_set_timeout(client, 10);
  g_socket_client_connect_to_host_async(client, "127.0.0.1", (guint16)port, NULL, on_connected, p);
}

static void
on_java_exec(JsonNode *result, const char *error, gpointer user_data)
{
  Prep *p = user_data;
  /* Not fatal: jdtls falls back to its own JVM when no executable is given. */
  if (error == NULL && result != NULL && JSON_NODE_HOLDS_VALUE(result) && json_node_get_string(result) != NULL) {
    p->java_exec = g_strdup(json_node_get_string(result));
  }
  if (!p->exec("vscode.java.startDebugSession", NULL, on_start_session, p, p->host)) {
    fail(p, "the Java language server is no longer running");
  }
}

static void
on_classpath(JsonNode *result, const char *error, gpointer user_data)
{
  Prep *p = user_data;
  if (error != NULL || result == NULL || !JSON_NODE_HOLDS_ARRAY(result) ||
      json_array_get_length(json_node_get_array(result)) < 2) {
    fail_command(p, "resolving the classpath", error ? error : "unexpected result");
    return;
  }
  JsonArray *arr = json_node_get_array(result);
  p->module_paths = json_node_copy(json_array_get_element(arr, 0));
  p->class_paths = json_node_copy(json_array_get_element(arr, 1));
  if (!p->exec("vscode.java.resolveJavaExecutable", two_string_array(p->main_class, p->project_name ? p->project_name : ""),
               on_java_exec, p, p->host)) {
    fail(p, "the Java language server is no longer running");
  }
}

static void
on_main_classes(JsonNode *result, const char *error, gpointer user_data)
{
  Prep *p = user_data;
  if (error != NULL || result == NULL || !JSON_NODE_HOLDS_ARRAY(result)) {
    fail_command(p, "resolving main classes", error ? error : "unexpected result");
    return;
  }

  JsonArray *arr = json_node_get_array(result);
  JsonObject *pick = NULL;
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonObject *o = json_array_get_object_element(arr, i);
    if (o == NULL) {
      continue;
    }
    const char *fp = json_object_get_string_member_with_default(o, "filePath", NULL);
    if (fp != NULL && g_strcmp0(fp, p->file_path) == 0) {
      pick = o;
      break;
    }
  }
  if (pick == NULL) {
    g_autofree char *base = g_path_get_basename(p->file_path);
    fail(p, "No runnable main method found in %s (%u main class%s in the project)", base,
         json_array_get_length(arr), json_array_get_length(arr) == 1 ? "" : "es");
    return;
  }
  p->main_class = g_strdup(json_object_get_string_member_with_default(pick, "mainClass", ""));
  p->project_name = g_strdup(json_object_get_string_member_with_default(pick, "projectName", ""));

  if (!p->exec("vscode.java.resolveClasspath", two_string_array(p->main_class, p->project_name), on_classpath, p, p->host)) {
    fail(p, "the Java language server is no longer running");
  }
}

void
ide_java_debug_prepare(const char *file_path, const char *workspace_root, IdeJavaExecFn exec,
                       gpointer host_data, IdeJavaDebugReadyCb ready_cb, gpointer user_data)
{
  g_return_if_fail(exec != NULL && ready_cb != NULL);
  Prep *p = g_new0(Prep, 1);
  p->file_path = g_strdup(file_path);
  p->workspace_root = g_strdup(workspace_root ? workspace_root : ".");
  p->exec = exec;
  p->host = host_data;
  p->ready_cb = ready_cb;
  p->ready_data = user_data;

  g_autofree char *root_uri = g_filename_to_uri(p->workspace_root, NULL, NULL);
  if (!exec("vscode.java.resolveMainClass", string_array_node(root_uri ? root_uri : ""), on_main_classes, p, host_data)) {
    fail(p, "No ready Java language server for this file (is jdtls installed and initialized?)");
  }
}
