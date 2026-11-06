#include "../src/ide/ide_dap_client.h"
#include "../src/ide/ide_dap_launch.h"

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
  IdeDapLaunch *jl = ide_dap_launch_resolve(ws, j, TRUE);
  g_assert_nonnull(jl->error);
  g_assert_nonnull(strstr(jl->error, "java-debug"));
  ide_dap_launch_free(jl);

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
  g_test_add_func("/ide/dap-launch/config-and-meson", test_launch_from_config_and_meson);
  g_test_add_func("/ide/dap-launch/java-and-detection", test_launch_java_go_messages);
  return g_test_run();
}
