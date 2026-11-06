#include "../src/ide/ide_ts.h"

#include <glib.h>
#include <string.h>

typedef struct {
  GString *log; /* "token:text\n" per emitted token */
  const char *text;
} Collect;

static const char *
tok_name(IdeTsToken t)
{
  switch (t) {
    case IDE_TS_TOKEN_KEYWORD:    return "kw";
    case IDE_TS_TOKEN_CONTROL:    return "ctrl";
    case IDE_TS_TOKEN_FUNCTION:   return "fn";
    case IDE_TS_TOKEN_TYPE:       return "type";
    case IDE_TS_TOKEN_MACRO:      return "mac";
    case IDE_TS_TOKEN_VARIABLE:   return "var";
    case IDE_TS_TOKEN_STRING:     return "str";
    case IDE_TS_TOKEN_STRING_RAW: return "raw";
    case IDE_TS_TOKEN_NUMBER:     return "num";
    case IDE_TS_TOKEN_COMMENT:    return "cmt";
    case IDE_TS_TOKEN_OPERATOR:   return "op";
    default:                      return "none";
  }
}

static void
collect_cb(gsize s, gsize e, IdeTsToken t, gpointer ud)
{
  Collect *c = ud;
  g_string_append_printf(c->log, "%s:%.*s\n", tok_name(t), (int)(e - s), c->text + s);
}

static char *
tokens_of(IdeTsLang lang, const char *src)
{
  IdeTsDoc *doc = ide_ts_doc_new(lang);
  g_assert_nonnull(doc);
  gsize ds, de;
  g_assert_true(ide_ts_doc_update(doc, src, strlen(src), &ds, &de));
  Collect c = { g_string_new(NULL), src };
  ide_ts_doc_walk(doc, 0, strlen(src), collect_cb, &c);
  ide_ts_doc_free(doc);
  return g_string_free(c.log, FALSE);
}

#define ASSERT_HAS(log, needle) \
  do { \
    if (strstr((log), (needle)) == NULL) \
      g_error("missing token '%s' in:\n%s", (needle), (log)); \
  } while (0)
#define ASSERT_LACKS(log, needle) \
  g_assert_null(strstr((log), (needle)))

static void
test_lang_detection(void)
{
  if (ide_ts_lang_available(IDE_TS_LANG_JAVA)) {
    g_assert_cmpint(ide_ts_lang_for_path("/x/a.java"), ==, IDE_TS_LANG_JAVA);
    g_assert_cmpint(ide_ts_lang_for_path("java"), ==, IDE_TS_LANG_JAVA);
  }
  if (ide_ts_lang_available(IDE_TS_LANG_GO)) {
    g_assert_cmpint(ide_ts_lang_for_path("main.go"), ==, IDE_TS_LANG_GO);
  }
  if (ide_ts_lang_available(IDE_TS_LANG_C)) {
    g_assert_cmpint(ide_ts_lang_for_path("a.c"), ==, IDE_TS_LANG_C);
    g_assert_cmpint(ide_ts_lang_for_path("a.h"), ==, IDE_TS_LANG_C);
  }
  g_assert_cmpint(ide_ts_lang_for_path("README.md"), ==, IDE_TS_LANG_NONE);
  g_assert_cmpint(ide_ts_lang_for_path(NULL), ==, IDE_TS_LANG_NONE);
}

static void
test_java_tokens(void)
{
  const char *src =
    "package org.example.app;\n"
    "import java.util.List;\n"
    "@Override\n"
    "public sealed record P(int x, String name) implements Q {\n"
    "  static final int MAX_SIZE = 0x1F;\n"
    "  <T> List<T> run(T a) throws IOException {\n"
    "    // note\n"
    "    String s = \"hi\\n\" + Math.abs(a.size());\n"
    "    if (this.x > 0) { return null; }\n"
    "    return list.stream().map(v -> v).toList();\n"
    "  }\n"
    "}\n";
  g_autofree char *log = tokens_of(IDE_TS_LANG_JAVA, src);

  ASSERT_HAS(log, "kw:package");
  ASSERT_HAS(log, "kw:import");
  ASSERT_HAS(log, "mac:@\n");
  ASSERT_HAS(log, "mac:Override");
  ASSERT_HAS(log, "kw:public");
  ASSERT_HAS(log, "kw:sealed");
  ASSERT_HAS(log, "kw:implements");
  ASSERT_HAS(log, "kw:throws");
  ASSERT_HAS(log, "type:int");
  ASSERT_HAS(log, "type:String");
  ASSERT_HAS(log, "type:List");
  ASSERT_HAS(log, "type:P"); /* record name */
  ASSERT_HAS(log, "mac:MAX_SIZE");
  ASSERT_HAS(log, "num:0x1F");
  ASSERT_HAS(log, "fn:run");
  ASSERT_HAS(log, "fn:abs");
  ASSERT_HAS(log, "fn:size");
  ASSERT_HAS(log, "fn:stream");
  ASSERT_HAS(log, "type:Math");
  ASSERT_HAS(log, "var:list");
  ASSERT_HAS(log, "cmt:// note");
  ASSERT_HAS(log, "str:\"hi\\n\"");
  ASSERT_HAS(log, "ctrl:if");
  ASSERT_HAS(log, "ctrl:return");
  ASSERT_HAS(log, "mac:null");
  ASSERT_HAS(log, "kw:this");
  ASSERT_HAS(log, "op:->");
}

static void
test_go_tokens(void)
{
  const char *src =
    "package main\n"
    "import \"fmt\"\n"
    "const Limit = 10\n"
    "type Server struct { Name string }\n"
    "func (s *Server) Start(n int) error {\n"
    "\t// run\n"
    "\tfor i := range n { fmt.Println(i, `raw\\n`, 'x', 3.5) }\n"
    "\tdefer s.stop()\n"
    "\tif s == nil { return nil }\n"
    "\treturn len(s.Name) > 0\n"
    "}\n";
  g_autofree char *log = tokens_of(IDE_TS_LANG_GO, src);

  ASSERT_HAS(log, "kw:package");
  ASSERT_HAS(log, "kw:import");
  ASSERT_HAS(log, "kw:const");
  ASSERT_HAS(log, "kw:type");
  ASSERT_HAS(log, "kw:struct");
  ASSERT_HAS(log, "kw:func");
  ASSERT_HAS(log, "kw:range");
  ASSERT_HAS(log, "mac:Limit");
  ASSERT_HAS(log, "type:Server");
  ASSERT_HAS(log, "type:string");
  ASSERT_HAS(log, "type:error");
  ASSERT_HAS(log, "type:int");
  ASSERT_HAS(log, "fn:Start");
  ASSERT_HAS(log, "fn:Println");
  ASSERT_HAS(log, "fn:stop");
  ASSERT_HAS(log, "fn:len");
  ASSERT_HAS(log, "ctrl:for");
  ASSERT_HAS(log, "ctrl:defer");
  ASSERT_HAS(log, "ctrl:if");
  ASSERT_HAS(log, "ctrl:return");
  ASSERT_HAS(log, "mac:nil");
  ASSERT_HAS(log, "num:3.5");
  ASSERT_HAS(log, "cmt:// run");
  ASSERT_HAS(log, "raw:`raw\\n`");
  ASSERT_HAS(log, "str:\"fmt\"");
  ASSERT_HAS(log, "str:'x'");
  ASSERT_HAS(log, "op::=");
}

static void
test_c_tokens(void)
{
  const char *src =
    "#include <stdio.h>\n"
    "#define BUF_MAX 64\n"
    "typedef struct { unsigned long id; char *name; } Item;\n"
    "static int add(int a, size_t b) {\n"
    "  /* sum */\n"
    "  if (a < BUF_MAX) { printf(\"%d\\n\", a); }\n"
    "  return item->count + helper(a);\n"
    "}\n";
  g_autofree char *log = tokens_of(IDE_TS_LANG_C, src);

  ASSERT_HAS(log, "kw:#include");
  ASSERT_HAS(log, "str:<stdio.h>");
  ASSERT_HAS(log, "kw:#define");
  ASSERT_HAS(log, "mac:BUF_MAX");
  ASSERT_HAS(log, "kw:typedef");
  ASSERT_HAS(log, "type:unsigned");
  ASSERT_HAS(log, "type:long");
  ASSERT_HAS(log, "type:Item");
  ASSERT_HAS(log, "kw:static");
  ASSERT_HAS(log, "type:int");
  ASSERT_HAS(log, "type:size_t");
  ASSERT_HAS(log, "fn:add");
  ASSERT_HAS(log, "fn:printf");
  ASSERT_HAS(log, "fn:helper");
  ASSERT_HAS(log, "var:count");
  ASSERT_HAS(log, "ctrl:if");
  ASSERT_HAS(log, "ctrl:return");
  ASSERT_HAS(log, "cmt:/* sum */");
  ASSERT_HAS(log, "num:64");
}

/* An incremental re-parse must produce exactly the tokens of a fresh parse,
 * and report a dirty range that covers the edit. */
static void
check_incremental(IdeTsLang lang, const char *before, const char *after)
{
  IdeTsDoc *doc = ide_ts_doc_new(lang);
  gsize ds, de;
  g_assert_true(ide_ts_doc_update(doc, before, strlen(before), &ds, &de));
  g_assert_cmpuint(ds, ==, 0);
  g_assert_cmpuint(de, ==, strlen(before));

  g_assert_true(ide_ts_doc_update(doc, after, strlen(after), &ds, &de));
  g_assert_cmpuint(de, >, ds);
  g_assert_cmpuint(de, <=, strlen(after));

  Collect inc = { g_string_new(NULL), after };
  ide_ts_doc_walk(doc, 0, strlen(after), collect_cb, &inc);
  ide_ts_doc_free(doc);

  g_autofree char *fresh = tokens_of(lang, after);
  g_assert_cmpstr(inc.log->str, ==, fresh);
  g_string_free(inc.log, TRUE);
}

static void
test_incremental_matches_fresh(void)
{
  check_incremental(IDE_TS_LANG_JAVA,
                    "class A { int f() { return 1; } }\n",
                    "class A { int f() { return 1 + g(2); } }\n");
  check_incremental(IDE_TS_LANG_GO,
                    "package m\nfunc f() int { return 1 }\n",
                    "package m\n// c\nfunc f() int { return 1 }\n");
  check_incremental(IDE_TS_LANG_C,
                    "int f(void) { return 1; }\n",
                    "int f(void) { /* open\n");
  /* deletion + multibyte */
  check_incremental(IDE_TS_LANG_JAVA,
                    "class A { String s = \"h\xC3\xA9llo\"; int x; }\n",
                    "class A { String s = \"h\xC3\xA9\"; }\n");
}

static void
test_unchanged_update_is_empty(void)
{
  const char *src = "package m\n";
  IdeTsDoc *doc = ide_ts_doc_new(IDE_TS_LANG_GO);
  gsize ds, de;
  g_assert_true(ide_ts_doc_update(doc, src, strlen(src), &ds, &de));
  g_assert_true(ide_ts_doc_update(doc, src, strlen(src), &ds, &de));
  g_assert_cmpuint(de, ==, ds);
  ide_ts_doc_free(doc);
}

static void
test_error_tolerance(void)
{
  IdeTsDoc *doc = ide_ts_doc_new(IDE_TS_LANG_JAVA);
  const char *src = "class A { void f( { int x = ; \n";
  gsize ds, de;
  g_assert_true(ide_ts_doc_update(doc, src, strlen(src), &ds, &de));
  g_assert_true(ide_ts_doc_has_error(doc));
  Collect c = { g_string_new(NULL), src };
  ide_ts_doc_walk(doc, 0, strlen(src), collect_cb, &c);
  g_assert_nonnull(strstr(c.log->str, "kw:class"));
  g_string_free(c.log, TRUE);
  ide_ts_doc_free(doc);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide-ts/lang-detection", test_lang_detection);

  if (ide_ts_lang_available(IDE_TS_LANG_JAVA)) {
    g_test_add_func("/ide-ts/java", test_java_tokens);
  }
  if (ide_ts_lang_available(IDE_TS_LANG_GO)) {
    g_test_add_func("/ide-ts/go", test_go_tokens);
  }
  if (ide_ts_lang_available(IDE_TS_LANG_C)) {
    g_test_add_func("/ide-ts/c", test_c_tokens);
  }
  if (ide_ts_lang_available(IDE_TS_LANG_JAVA) && ide_ts_lang_available(IDE_TS_LANG_GO) &&
      ide_ts_lang_available(IDE_TS_LANG_C)) {
    g_test_add_func("/ide-ts/incremental", test_incremental_matches_fresh);
    g_test_add_func("/ide-ts/unchanged", test_unchanged_update_is_empty);
    g_test_add_func("/ide-ts/error-tolerance", test_error_tolerance);
  }
  return g_test_run();
}
