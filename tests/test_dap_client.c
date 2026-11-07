#include "../src/ide/ide_dap_client.h"
#include "../src/ide/ide_dap_launch.h"
#include "../src/ide/ide_dap_java.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
  GPtrArray *events;      /* char* "event" names in arrival order */
  gboolean   exited;
  gboolean   got;
  gboolean   ok;
  char      *msg;
  gint64     value;
  char      *text;
} Probe;

static gboolean
to_cb(gpointer d)
{
  *(gboolean *)d = TRUE;
  return G_SOURCE_REMOVE;
}

#define SPIN_UNTIL(cond) \
  do { \
    gboolean to_ = FALSE; \
    guint id_ = g_timeout_add_seconds(10, to_cb, &to_); \
    while (!(cond) && !to_) g_main_context_iteration(NULL, TRUE); \
    if (!to_) g_source_remove(id_); \
    g_assert_true(cond); \
  } while (0)

static void
on_event(IdeDapClient *c, const char *event, JsonObject *body, gpointer ud)
{
  (void)c;
  (void)body;
  Probe *p = ud;
  g_ptr_array_add(p->events, g_strdup(event));
}

static void
on_exit_cb(IdeDapClient *c, gpointer ud)
{
  (void)c;
  ((Probe *)ud)->exited = TRUE;
}

static void
on_resp(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer ud)
{
  (void)c;
  Probe *p = ud;
  p->got = TRUE;
  p->ok = ok;
  g_free(p->msg);
  p->msg = g_strdup(msg);
  g_free(p->text);
  p->text = NULL;
  if (body != NULL) {
    if (json_object_has_member(body, "result")) {
      p->text = g_strdup(json_object_get_string_member(body, "result"));
    }
    if (json_object_has_member(body, "stackFrames")) {
      p->value = json_array_get_length(json_object_get_array_member(body, "stackFrames"));
    }
  }
}

static gboolean
has_event(Probe *p, const char *name)
{
  for (guint i = 0; i < p->events->len; i++) {
    if (g_strcmp0(g_ptr_array_index(p->events, i), name) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static JsonNode *
obj1(const char *k, const char *sv, gint64 iv)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, k);
  if (sv != NULL) {
    json_builder_add_string_value(b, sv);
  } else {
    json_builder_add_int_value(b, iv);
  }
  json_builder_end_object(b);
  JsonNode *n = json_builder_get_root(b);
  g_object_unref(b);
  return n;
}

static IdeDapClient *
make_client(Probe *p)
{
  g_autofree char *script = g_build_filename(KAJO_SOURCE_DIR, "tests", "mock_dap_adapter.py", NULL);
  const char *argv[] = { "python3", script, NULL };
  IdeDapClient *c = ide_dap_client_new_process(argv, g_get_tmp_dir());
  ide_dap_client_set_callbacks(c, on_event, on_exit_cb, p);
  return c;
}

#define REQUEST_WAIT(client, cmd, args, p) \
  do { \
    (p)->got = FALSE; \
    ide_dap_client_request((client), (cmd), (args), on_resp, (p)); \
    SPIN_UNTIL((p)->got); \
  } while (0)

static void
test_session_flow(void)
{
  Probe p = { .events = g_ptr_array_new_with_free_func(g_free) };
  IdeDapClient *c = make_client(&p);
  g_assert_true(ide_dap_client_start(c, NULL));
  g_assert_true(ide_dap_client_is_alive(c));

  REQUEST_WAIT(c, "initialize", obj1("adapterID", "mock", 0), &p);
  g_assert_true(p.ok);

  REQUEST_WAIT(c, "launch", obj1("program", "/tmp/prog", 0), &p);
  g_assert_true(p.ok);
  SPIN_UNTIL(has_event(&p, "initialized"));

  /* breakpoint set + configurationDone triggers the first stop */
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "source");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "path");
  json_builder_add_string_value(b, "/src/main.c");
  json_builder_end_object(b);
  json_builder_set_member_name(b, "breakpoints");
  json_builder_begin_array(b);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "line");
  json_builder_add_int_value(b, 7);
  json_builder_end_object(b);
  json_builder_end_array(b);
  json_builder_end_object(b);
  JsonNode *bp = json_builder_get_root(b);
  g_object_unref(b);
  REQUEST_WAIT(c, "setBreakpoints", bp, &p);
  g_assert_true(p.ok);
  REQUEST_WAIT(c, "configurationDone", NULL, &p);
  SPIN_UNTIL(has_event(&p, "stopped"));

  REQUEST_WAIT(c, "stackTrace", obj1("threadId", NULL, 1), &p);
  g_assert_cmpint(p.value, ==, 2);

  REQUEST_WAIT(c, "evaluate", obj1("expression", "x+1", 0), &p);
  g_assert_cmpstr(p.text, ==, "x+1 = 42");

  /* failures surface with their message */
  REQUEST_WAIT(c, "bogus", NULL, &p);
  g_assert_false(p.ok);
  g_assert_nonnull(strstr(p.msg, "unsupported"));

  /* run to the end: the mock stops once more, then terminates */
  g_ptr_array_set_size(p.events, 0);
  REQUEST_WAIT(c, "continue", obj1("threadId", NULL, 1), &p);
  SPIN_UNTIL(has_event(&p, "stopped"));
  g_ptr_array_set_size(p.events, 0);
  REQUEST_WAIT(c, "continue", obj1("threadId", NULL, 1), &p);
  SPIN_UNTIL(has_event(&p, "terminated") && has_event(&p, "exited"));

  REQUEST_WAIT(c, "disconnect", NULL, &p);
  g_assert_true(ide_dap_client_wait_exit(c, 5000));
  g_assert_true(p.exited);
  g_assert_false(ide_dap_client_is_alive(c));

  /* requests after exit fail immediately */
  p.got = FALSE;
  ide_dap_client_request(c, "threads", NULL, on_resp, &p);
  g_assert_true(p.got);
  g_assert_false(p.ok);

  ide_dap_client_free(c);
  g_ptr_array_unref(p.events);
  g_free(p.msg);
  g_free(p.text);
}

static void
test_launch_failure_reported(void)
{
  Probe p = { .events = g_ptr_array_new_with_free_func(g_free) };
  IdeDapClient *c = make_client(&p);
  g_assert_true(ide_dap_client_start(c, NULL));
  REQUEST_WAIT(c, "initialize", obj1("adapterID", "mock", 0), &p);
  REQUEST_WAIT(c, "launch", obj1("program", "/nonexistent", 0), &p);
  g_assert_false(p.ok);
  g_assert_cmpstr(p.msg, ==, "program not found");
  ide_dap_client_stop(c);
  g_assert_true(ide_dap_client_wait_exit(c, 5000));
  ide_dap_client_free(c);
  g_ptr_array_unref(p.events);
  g_free(p.msg);
}

static void
test_pending_failed_when_adapter_dies(void)
{
  Probe p = { .events = g_ptr_array_new_with_free_func(g_free) };
  IdeDapClient *c = make_client(&p);
  g_assert_true(ide_dap_client_start(c, NULL));
  REQUEST_WAIT(c, "initialize", obj1("adapterID", "mock", 0), &p);
  /* disconnect makes the mock exit without a second reply to the next request */
  ide_dap_client_request(c, "disconnect", NULL, NULL, NULL);
  p.got = FALSE;
  ide_dap_client_request(c, "threads", NULL, on_resp, &p);
  SPIN_UNTIL(p.got || p.exited);
  g_assert_true(ide_dap_client_wait_exit(c, 5000));
  SPIN_UNTIL(p.got);
  g_assert_false(p.ok);
  ide_dap_client_free(c);
  g_ptr_array_unref(p.events);
  g_free(p.msg);
}

static void
test_missing_adapter(void)
{
  Probe p = { .events = g_ptr_array_new_with_free_func(g_free) };
  const char *argv[] = { "/nonexistent/adapter", NULL };
  IdeDapClient *c = ide_dap_client_new_process(argv, NULL);
  GError *err = NULL;
  g_assert_false(ide_dap_client_start(c, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);
  ide_dap_client_free(c);
  g_ptr_array_unref(p.events);
}


/* ─── Java debug preparation against scripted jdtls answers + a TCP mock adapter ─── */

typedef struct {
  GPtrArray *commands;   /* char* commands in order */
  guint16    port;
  const char *fail_command;   /* answer this command with an error */
  const char *fail_message;
  gboolean   no_main;
  gboolean   done;
  char      *error;
  JsonNode  *launch;
  GIOStream *conn;
} JavaProbe;

static JsonNode *
J(const char *s)
{
  return json_from_string(s, NULL);
}

static gboolean
fake_exec(const char *command, JsonNode *args, IdeJavaExecCb cb, gpointer cb_data, gpointer host)
{
  JavaProbe *jp = host;
  g_ptr_array_add(jp->commands, g_strdup(command));
  if (args != NULL) {
    json_node_unref(args);
  }
  if (jp->fail_command != NULL && g_strcmp0(command, jp->fail_command) == 0) {
    cb(NULL, jp->fail_message, cb_data);
    return TRUE;
  }
  g_autoptr(JsonNode) res = NULL;
  if (g_strcmp0(command, "vscode.java.resolveMainClass") == 0) {
    res = jp->no_main ? J("[]")
                      : J("[{\"mainClass\":\"demo.Other\",\"projectName\":\"demo\",\"filePath\":\"/ws/src/Other.java\"},"
                          "{\"mainClass\":\"demo.App\",\"projectName\":\"demo\",\"filePath\":\"/ws/src/App.java\"}]");
  } else if (g_strcmp0(command, "vscode.java.resolveClasspath") == 0) {
    res = J("[[],[\"/ws/target/classes\",\"/m2/lib.jar\"]]");
  } else if (g_strcmp0(command, "vscode.java.resolveJavaExecutable") == 0) {
    res = J("\"/usr/lib/jvm/x/bin/java\"");
  } else if (g_strcmp0(command, "vscode.java.startDebugSession") == 0) {
    g_autofree char *num = g_strdup_printf("%u", jp->port);
    res = J(num);
  }
  cb(res, NULL, cb_data);
  return TRUE;
}

static void
java_ready(GIOStream *conn, JsonNode *launch, const char *error, gpointer ud)
{
  JavaProbe *jp = ud;
  jp->done = TRUE;
  jp->conn = conn;
  jp->launch = launch;
  jp->error = g_strdup(error);
}

static guint16
free_port(void)
{
  GSocket *s = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL);
  g_autoptr(GInetAddress) lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) sa = g_inet_socket_address_new(lo, 0);
  g_socket_bind(s, sa, TRUE, NULL);
  g_autoptr(GSocketAddress) bound = g_socket_get_local_address(s, NULL);
  guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
  g_object_unref(s);
  return port;
}

static void
test_java_prepare_and_tcp_session(void)
{
  JavaProbe jp = { .commands = g_ptr_array_new_with_free_func(g_free), .port = free_port() };

  /* TCP DAP server standing in for java-debug's server */
  g_autofree char *script = g_build_filename(KAJO_SOURCE_DIR, "tests", "mock_dap_adapter.py", NULL);
  g_autofree char *port_s = g_strdup_printf("%u", jp.port);
  g_autoptr(GSubprocess) srv = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "python3", script, "--listen", port_s, NULL);
  g_usleep(400 * 1000);

  ide_java_debug_prepare("/ws/src/App.java", "/ws", fake_exec, &jp, java_ready, &jp);
  SPIN_UNTIL(jp.done);
  g_assert_null(jp.error);
  g_assert_nonnull(jp.conn);

  const char *want[] = { "vscode.java.resolveMainClass", "vscode.java.resolveClasspath",
                         "vscode.java.resolveJavaExecutable", "vscode.java.startDebugSession" };
  g_assert_cmpuint(jp.commands->len, ==, G_N_ELEMENTS(want));
  for (guint i = 0; i < G_N_ELEMENTS(want); i++) {
    g_assert_cmpstr(g_ptr_array_index(jp.commands, i), ==, want[i]);
  }

  JsonObject *la = json_node_get_object(jp.launch);
  g_assert_cmpstr(json_object_get_string_member(la, "mainClass"), ==, "demo.App");
  g_assert_cmpstr(json_object_get_string_member(la, "projectName"), ==, "demo");
  g_assert_cmpstr(json_object_get_string_member(la, "javaExec"), ==, "/usr/lib/jvm/x/bin/java");
  g_assert_cmpuint(json_array_get_length(json_object_get_array_member(la, "classPaths")), ==, 2);
  g_assert_cmpstr(json_object_get_string_member(la, "cwd"), ==, "/ws");

  /* the returned connection speaks DAP */
  Probe p = { .events = g_ptr_array_new_with_free_func(g_free) };
  IdeDapClient *c = ide_dap_client_new_streams(g_io_stream_get_input_stream(jp.conn),
                                               g_io_stream_get_output_stream(jp.conn));
  ide_dap_client_set_callbacks(c, on_event, on_exit_cb, &p);
  g_assert_true(ide_dap_client_start(c, NULL));
  REQUEST_WAIT(c, "initialize", obj1("adapterID", "java", 0), &p);
  g_assert_true(p.ok);
  REQUEST_WAIT(c, "launch", json_node_copy(jp.launch), &p);
  g_assert_true(p.ok);
  SPIN_UNTIL(has_event(&p, "initialized"));
  REQUEST_WAIT(c, "disconnect", NULL, &p);
  g_assert_true(ide_dap_client_wait_exit(c, 5000));

  ide_dap_client_free(c);
  g_object_unref(jp.conn);
  json_node_unref(jp.launch);
  g_free(jp.error);
  g_subprocess_force_exit(srv);
  g_ptr_array_unref(jp.commands);
  g_ptr_array_unref(p.events);
  g_free(p.msg);
  g_free(p.text);
}

static void
test_java_prepare_errors(void)
{
  /* no main method in the file */
  JavaProbe a = { .commands = g_ptr_array_new_with_free_func(g_free), .no_main = TRUE };
  ide_java_debug_prepare("/ws/src/App.java", "/ws", fake_exec, &a, java_ready, &a);
  SPIN_UNTIL(a.done);
  g_assert_nonnull(a.error);
  g_assert_nonnull(strstr(a.error, "No runnable main method"));
  g_assert_null(a.conn);
  g_free(a.error);
  g_ptr_array_unref(a.commands);

  /* bundle missing: jdtls rejects the command -> actionable message */
  JavaProbe b = { .commands = g_ptr_array_new_with_free_func(g_free),
                  .fail_command = "vscode.java.resolveMainClass",
                  .fail_message = "No delegateCommandHandler for vscode.java.resolveMainClass" };
  ide_java_debug_prepare("/ws/src/App.java", "/ws", fake_exec, &b, java_ready, &b);
  SPIN_UNTIL(b.done);
  g_assert_nonnull(strstr(b.error, "install-java-debug.sh"));
  g_free(b.error);
  g_ptr_array_unref(b.commands);

  /* debug server port closed */
  JavaProbe c = { .commands = g_ptr_array_new_with_free_func(g_free), .port = free_port() };
  ide_java_debug_prepare("/ws/src/App.java", "/ws", fake_exec, &c, java_ready, &c);
  SPIN_UNTIL(c.done);
  g_assert_nonnull(strstr(c.error, "cannot connect"));
  g_free(c.error);
  g_ptr_array_unref(c.commands);
}

/* ─── launch resolution ─── */

static void
touch(const char *dir, const char *name, const char *contents)
{
  g_autofree char *p = g_build_filename(dir, name, NULL);
  g_autofree char *parent = g_path_get_dirname(p);
  g_mkdir_with_parents(parent, 0755);
  g_file_set_contents(p, contents, -1, NULL);
}

static void
test_launch_from_config_and_meson(void)
{
  g_autofree char *ws = g_dir_make_tmp("kajo-dap-XXXXXX", NULL);

  /* nothing built */
  touch(ws, "src/a.c", "int main(void){return 0;}\n");
  g_autofree char *src = g_build_filename(ws, "src/a.c", NULL);
  IdeDapLaunch *none = ide_dap_launch_resolve(ws, src, TRUE);
  g_assert_nonnull(none->error);
  g_assert_nonnull(strstr(none->error, "build the project"));
  ide_dap_launch_free(none);

  /* meson introspection picks the executable that owns the file's directory */
  touch(ws, "builddir/app", "\x7f" "ELF....");
  touch(ws, "builddir/other", "\x7f" "ELF....");
  g_autofree char *app = g_build_filename(ws, "builddir/app", NULL);
  g_autofree char *other = g_build_filename(ws, "builddir/other", NULL);
  g_autofree char *intro = g_strdup_printf(
      "[{\"name\":\"other\",\"type\":\"executable\",\"filename\":[\"%s\"],\"target_sources\":[{\"sources\":[\"%s/lib/x.c\"]}]},"
      " {\"name\":\"app\",\"type\":\"executable\",\"filename\":[\"%s\"],\"target_sources\":[{\"sources\":[\"%s\"]}]}]",
      other, ws, app, src);
  touch(ws, "builddir/meson-info/intro-targets.json", intro);
  IdeDapLaunch *m = ide_dap_launch_resolve(ws, src, TRUE);
  if (m->error == NULL) { /* a native adapter exists on this machine */
    g_assert_cmpstr(m->program, ==, app);
    JsonObject *a = json_node_get_object(m->arguments);
    g_assert_cmpstr(json_object_get_string_member(a, "program"), ==, app);
    g_assert_false(json_object_has_member(a, "stopAtBeginningOfMainSubprogram")); /* has breakpoints */
  }
  ide_dap_launch_free(m);

  IdeDapLaunch *nb = ide_dap_launch_resolve(ws, src, FALSE);
  if (nb->error == NULL && g_str_has_prefix(nb->adapter_label, "gdb")) {
    JsonObject *a = json_node_get_object(nb->arguments);
    g_assert_true(json_object_get_boolean_member(a, "stopAtBeginningOfMainSubprogram"));
  }
  ide_dap_launch_free(nb);

  /* .kajo/debug.json wins */
  touch(ws, ".kajo/debug.json", "{\"program\":\"bin/tool\",\"args\":[\"-v\",\"x\"],\"cwd\":\"run\"}");
  IdeDapLaunch *cfg = ide_dap_launch_resolve(ws, src, TRUE);
  g_autofree char *want = g_build_filename(ws, "bin/tool", NULL);
  g_assert_cmpstr(cfg->program, ==, want);
  g_autofree char *wcwd = g_build_filename(ws, "run", NULL);
  g_assert_cmpstr(cfg->cwd, ==, wcwd);
  if (cfg->error == NULL) {
    JsonArray *args = json_object_get_array_member(json_node_get_object(cfg->arguments), "args");
    g_assert_cmpuint(json_array_get_length(args), ==, 2);
    g_assert_cmpstr(json_array_get_string_element(args, 1), ==, "x");
  }
  ide_dap_launch_free(cfg);

  touch(ws, ".kajo/debug.json", "{ nope");
  IdeDapLaunch *bad = ide_dap_launch_resolve(ws, src, TRUE);
  g_assert_nonnull(bad->error);
  ide_dap_launch_free(bad);
}

static void
test_launch_java_go_messages(void)
{
  g_autofree char *ws = g_dir_make_tmp("kajo-dap-j-XXXXXX", NULL);
  touch(ws, "A.java", "class A {}\n");
  g_autofree char *j = g_build_filename(ws, "A.java", NULL);
  g_setenv("KAJO_JAVA_DEBUG_JAR", "/nonexistent/java-debug.jar", TRUE);
  IdeDapLaunch *jl = ide_dap_launch_resolve(ws, j, TRUE);
  g_assert_nonnull(jl->error);
  g_assert_nonnull(strstr(jl->error, "install-java-debug.sh"));
  ide_dap_launch_free(jl);

  g_autofree char *jar = g_build_filename(ws, "debug.jar", NULL);
  g_file_set_contents(jar, "x", -1, NULL);
  g_setenv("KAJO_JAVA_DEBUG_JAR", jar, TRUE);
  IdeDapLaunch *ok = ide_dap_launch_resolve(ws, j, TRUE);
  g_assert_null(ok->error);
  g_assert_true(ok->java);
  g_assert_cmpstr(ok->adapter_label, ==, "java-debug (jdtls)");
  ide_dap_launch_free(ok);
  g_unsetenv("KAJO_JAVA_DEBUG_JAR");

  g_assert_cmpstr(ide_dap_detect_adapter_for_file("x.go"), ==, "dlv dap");
  g_assert_cmpstr(ide_dap_detect_adapter_for_file("x.c"), ==, "gdb -i=dap");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide/dap-client/session-flow", test_session_flow);
  g_test_add_func("/ide/dap-client/launch-failure", test_launch_failure_reported);
  g_test_add_func("/ide/dap-client/adapter-dies", test_pending_failed_when_adapter_dies);
  g_test_add_func("/ide/dap-client/missing-adapter", test_missing_adapter);
  g_test_add_func("/ide/dap-java/prepare-and-tcp-session", test_java_prepare_and_tcp_session);
  g_test_add_func("/ide/dap-java/errors", test_java_prepare_errors);
  g_test_add_func("/ide/dap-launch/config-and-meson", test_launch_from_config_and_meson);
  g_test_add_func("/ide/dap-launch/java-and-detection", test_launch_java_go_messages);
  return g_test_run();
}
