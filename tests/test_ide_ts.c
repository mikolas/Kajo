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
  ASSERT_HAS(log, "mac:@Override");
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


typedef struct {
  IdeTsLang   lang;
  const char *source;
  const char *expect[24]; /* NULL-terminated "token:text" needles */
} LangCase;

static const LangCase kLangCases[] = {
  { IDE_TS_LANG_RUST,
    "use std::collections::HashMap;\n"
    "#[derive(Debug)]\n"
    "pub struct Item<'a> { name: &'a str }\n"
    "const MAX_N: usize = 10;\n"
    "fn main() {\n"
    "    let mut m: HashMap<String, i32> = HashMap::new();\n"
    "    m.insert(\"s\\n\".to_string(), 2.5);\n"
    "    println!(\"{}\", m);\n"
    "    match x { 1 => return, _ => loop { break; } }\n"
    "}\n",
    { "kw:use", "type:HashMap", "mac:#[derive(Debug)]", "type:Item", "mac:MAX_N", "type:usize",
      "fn:main", "kw:mut", "fn:new", "fn:insert", "fn:to_string", "mac:println", "ctrl:match",
      "ctrl:loop", "num:2.5", "str:\"s\\n\"", NULL } },
  { IDE_TS_LANG_CPP,
    "#include <vector>\n"
    "namespace app { template <typename T> class Box : public Base {\n"
    "  Box(int v) : v_(v) {} virtual ~Box() override; T get() const;\n"
    "}; }\n"
    "enum class Color { Red };\n"
    "auto f(std::string s) -> bool { auto x = new Box<int>(1); x->get(); s.size();\n"
    "  ns::fn(); this->v_ = nullptr; throw std::runtime_error(\"e\"); return true; }\n"
    "R\"raw(text)raw\";\n",
    { "kw:namespace", "kw:template", "type:Box", "fn:~Box", "kw:override", "fn:get", "mac:Red",
      "kw:auto", "kw:new", "fn:size", "kw:this", "mac:nullptr", "ctrl:throw", "fn:runtime_error",
      "raw:R\"raw(text)raw\"", "str:<vector>", NULL } },
  { IDE_TS_LANG_BASH,
    "#!/bin/bash\n"
    "NAME=\"x y\"; export PATH=$HOME/bin:$PATH\n"
    "f() { if [ -f \"$a\" ]; then echo \"ok ${a}\" >&2; fi; }\n"
    "for i in 1 2; do ls -l | grep 'x'; done\n"
    "cat <<EOT\nhere $x\nEOT\n",
    { "cmt:#!/bin/bash", "var:NAME", "kw:export", "var:HOME", "fn:f", "ctrl:if", "ctrl:then",
      "fn:echo", "ctrl:fi", "ctrl:for", "ctrl:do", "ctrl:done", "fn:ls", "fn:grep", "str:'x'",
      "mac:EOT", "op:|", NULL } },
  { IDE_TS_LANG_JAVASCRIPT,
    "const o = { key: 1, m() {} };\n"
    "class A extends B { static #p = 1; get x() { return this.#p; } }\n"
    "o.method(1); new Foo(); fn();\n"
    "const g = (x) => x * `t${x}`, MAX_N = /re+/g;\n"
    "const el = <Button onClick={() => go()}>hi</Button>;\n",
    { "kw:const", "fn:m", "kw:class", "type:A", "type:B", "kw:extends", "kw:this", "fn:method",
      "kw:new", "type:Foo", "fn:fn", "fn:g", "mac:MAX_N", "op:=>", "type:Button", "var:onClick",
      "fn:go", NULL } },
  { IDE_TS_LANG_TYPESCRIPT,
    "import { a } from \"mod\";\n"
    "export interface Foo<T> extends Bar { x: number; m(y: string): void }\n"
    "type U = string | null;\n"
    "enum E { A = 1 }\n"
    "@Component({})\n"
    "export class Svc<T> extends Base implements I {\n"
    "  private readonly n: number = 0x10;\n"
    "  constructor(public name: string) { super(); }\n"
    "  async run(a: T): Promise<void> { const r = await fetch(a); this.n++; return undefined; }\n"
    "}\n",
    { "kw:import", "str:\"mod\"", "kw:interface", "type:Foo", "type:number", "fn:m", "type:U",
      "type:E", "mac:A", "mac:@Component({})", "kw:implements", "kw:private", "kw:readonly",
      "num:0x10", "fn:constructor", "fn:run", "type:Promise", "ctrl:await", "fn:fetch",
      "mac:undefined", NULL } },
  { IDE_TS_LANG_TSX,
    "const App = (p: Props) => <Card title=\"t\">{p.name}</Card>;\n",
    { "kw:const", "type:Props", "type:Card", "var:title", "str:\"t\"", NULL } },
  { IDE_TS_LANG_LUA,
    "local M = {}\n"
    "-- c\n"
    "function M.f(a) local s = \"x\" .. [[raw]]; if a == nil then return #s end end\n"
    "function M:m() self.x = 0x10 end\n"
    "print(M.f(1))\n",
    { "kw:local", "cmt:-- c", "kw:function", "fn:f", "fn:m", "kw:self", "str:\"x\"", "str:[[raw]]",
      "ctrl:if", "mac:nil", "ctrl:then", "kw:end", "num:0x10", "fn:print", NULL } },
  { IDE_TS_LANG_JSON,
    "{\"name\": \"x\\n\", \"n\": [1, -2.5e3, true, null], \"o\": {\"k\": false}}\n",
    { "var:\"name\"", "str:\"x\\n\"", "num:1", "num:-2.5e3", "mac:true", "mac:null", "mac:false",
      "var:\"k\"", NULL } },
  { IDE_TS_LANG_TOML,
    "# c\ntitle = \"x\"\n[server.main]\nport = 8080\npi = 3.14\non = true\n"
    "when = 1979-05-27T07:32:00Z\n[[bins]]\nname = 'lit'\n",
    { "cmt:# c", "var:title", "str:\"x\"", "type:server", "type:main", "var:port", "num:8080",
      "num:3.14", "mac:true", "num:1979-05-27T07:32:00Z", "type:bins", "str:'lit'", NULL } },
  { IDE_TS_LANG_YAML,
    "# c\nname: x\nlist:\n  - 1\n  - true\n  - null\nanchor: &a value\nref: *a\n"
    "\"quoted\": 'single'\n",
    { "cmt:# c", "var:name", "var:list", "num:1", "mac:true", "mac:null", "mac:&a", "mac:*a",
      "str:\"quoted\"", "str:'single'", NULL } },
  { IDE_TS_LANG_CSS,
    "/* c */\n@media (min-width: 600px) { .a > #b:hover, h1 { color: #ff00aa; margin: 0 10px;\n"
    "font: \"Fira Code\"; width: calc(100% - 4px) !important; } }\n",
    { "cmt:/* c */", "kw:@media", "var:min-width", "type:a", "mac:b", "fn:hover", "type:h1",
      "var:color", "num:#ff00aa", "num:10px", "str:\"Fira Code\"", "fn:calc", "kw:!important", NULL } },
  { IDE_TS_LANG_HTML,
    "<!DOCTYPE html>\n<!-- c -->\n<div class=\"a\" id=b><p>Hello &amp; <b>x</b></p><br/></div>\n",
    { "kw:DOCTYPE html", "cmt:<!-- c -->", "type:div", "var:class", "str:\"a\"", "var:id", "str:b",
      "type:p", "mac:&amp;", "type:br", NULL } },
};

static void
test_query_languages(void)
{
  for (gsize i = 0; i < G_N_ELEMENTS(kLangCases); i++) {
    const LangCase *c = &kLangCases[i];
    if (!ide_ts_lang_available(c->lang)) {
      continue;
    }
    g_autofree char *log = tokens_of(c->lang, c->source);
    for (gsize k = 0; c->expect[k] != NULL; k++) {
      if (strstr(log, c->expect[k]) == NULL) {
        g_error("[%s] missing token '%s' in:\n%s", ide_ts_lang_name(c->lang), c->expect[k], log);
      }
    }
  }
}

/* Appending a line and inserting a blank line must re-parse to exactly the
 * tokens of a fresh parse, for every language. */
static void
test_incremental_all_languages(void)
{
  for (gsize i = 0; i < G_N_ELEMENTS(kLangCases); i++) {
    const LangCase *c = &kLangCases[i];
    if (!ide_ts_lang_available(c->lang)) {
      continue;
    }
    const char *nl = strchr(c->source, '\n');
    g_assert_nonnull(nl);
    g_autofree char *edited = g_strdup_printf("%.*s\n\n%s\n", (int)(nl - c->source), c->source, nl + 1);
    check_incremental(c->lang, c->source, edited);
  }
}

/* Detection by path, base name and bare language id. */
static void
test_extended_detection(void)
{
  struct { const char *path; IdeTsLang lang; } cases[] = {
    { "/x/a.cpp", IDE_TS_LANG_CPP }, { "b.hpp", IDE_TS_LANG_CPP }, { "c++", IDE_TS_LANG_CPP },
    { "lib.rs", IDE_TS_LANG_RUST }, { "run.sh", IDE_TS_LANG_BASH }, { "/p/PKGBUILD", IDE_TS_LANG_BASH },
    { ".zshrc", IDE_TS_LANG_BASH }, { "a.js", IDE_TS_LANG_JAVASCRIPT }, { "a.jsx", IDE_TS_LANG_JAVASCRIPT },
    { "a.ts", IDE_TS_LANG_TYPESCRIPT }, { "a.tsx", IDE_TS_LANG_TSX }, { "a.json", IDE_TS_LANG_JSON },
    { "Cargo.toml", IDE_TS_LANG_TOML }, { "a.yml", IDE_TS_LANG_YAML }, { "a.css", IDE_TS_LANG_CSS },
    { "index.html", IDE_TS_LANG_HTML }, { "init.lua", IDE_TS_LANG_LUA }, { "yaml", IDE_TS_LANG_YAML },
    { "README.md", IDE_TS_LANG_NONE }, { "notes.txt", IDE_TS_LANG_NONE }, { "Makefile", IDE_TS_LANG_NONE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    IdeTsLang want = ide_ts_lang_available(cases[i].lang) ? cases[i].lang : IDE_TS_LANG_NONE;
    g_assert_cmpint(ide_ts_lang_for_path(cases[i].path), ==, want);
  }
}


/* Source with a '@' marking the cursor; returns the selected text or "<none>". */
static char *
select_object(IdeTsLang lang, const char *marked, gboolean inside, char target)
{
  const char *at = strchr(marked, '@');
  g_assert_nonnull(at);
  g_autofree char *src = g_strdup_printf("%.*s%s", (int)(at - marked), marked, at + 1);
  gsize cursor = (gsize)(at - marked);

  IdeTsDoc *doc = ide_ts_doc_new(lang);
  g_assert_nonnull(doc);
  gsize ds, de, s = 0, e = 0;
  g_assert_true(ide_ts_doc_update(doc, src, strlen(src), &ds, &de));
  char *out = ide_ts_doc_find_object(doc, cursor, inside, target, &s, &e)
                  ? g_strndup(src + s, e - s)
                  : g_strdup("<none>");
  ide_ts_doc_free(doc);
  return out;
}

#define ASSERT_OBJ(lang, src, inside, target, want) \
  do { \
    g_autofree char *got_ = select_object((lang), (src), (inside), (target)); \
    g_assert_cmpstr(got_, ==, (want)); \
  } while (0)

static void
test_text_objects_java(void)
{
  IdeTsLang J = IDE_TS_LANG_JAVA;
  ASSERT_OBJ(J, "class A {\n  int f(int a, String b) { return a@ + 1; }\n}\n", TRUE, 'f', "return a + 1;");
  ASSERT_OBJ(J, "class A {\n  int f(int a, String b) { return a@ + 1; }\n}\n", FALSE, 'f',
             "int f(int a, String b) { return a + 1; }");
  ASSERT_OBJ(J, "class A {\n  int f(int a, String b) { return a@ + 1; }\n}\n", TRUE, 'c',
             "  int f(int a, String b) { return a + 1; }");
  ASSERT_OBJ(J, "class A {\n  int f(int a, String b) { return a@ + 1; }\n}\n", FALSE, 'c',
             "class A {\n  int f(int a, String b) { return a + 1; }\n}");
  ASSERT_OBJ(J, "class A { void g() { if (x) { call(1,@ 2); } } }\n", TRUE, 'b', "call(1, 2);");
  ASSERT_OBJ(J, "class A { void g() { if (x) { call(1,@ 2); } } }\n", FALSE, 'b', "{ call(1, 2); }");
  ASSERT_OBJ(J, "class A { void g() { if (x) { call(1, @2); } } }\n", TRUE, 'a', "2");
  ASSERT_OBJ(J, "class A { void g() { call(@1, 2); } }\n", FALSE, 'a', "1, ");
  ASSERT_OBJ(J, "class A { void g(int a, String @b) {} }\n", FALSE, 'a', ", String b");
  ASSERT_OBJ(J, "int @x;\n", TRUE, 'f', "<none>");
}

static void
test_text_objects_go(void)
{
  IdeTsLang G = IDE_TS_LANG_GO;
  ASSERT_OBJ(G, "package m\nfunc f(a int) int {\n\treturn a@ + 1\n}\n", TRUE, 'f', "\treturn a + 1");
  ASSERT_OBJ(G, "package m\nfunc f(a int) int {\n\treturn a@ + 1\n}\n", FALSE, 'f',
             "func f(a int) int {\n\treturn a + 1\n}");
  ASSERT_OBJ(G, "package m\ntype S struct {\n\tName@ string\n}\n", TRUE, 'c', "\tName string");
  ASSERT_OBJ(G, "package m\ntype S struct {\n\tName@ string\n}\n", FALSE, 'c',
             "type S struct {\n\tName string\n}");
  ASSERT_OBJ(G, "package m\nfunc f() { g(1, @2, 3) }\n", TRUE, 'a', "2");
  ASSERT_OBJ(G, "package m\nfunc f() { x := func() int { return@ 1 }; _ = x }\n", TRUE, 'f', "return 1");
}

static void
test_text_objects_c_rust_js_lua(void)
{
  ASSERT_OBJ(IDE_TS_LANG_C, "static int add(int a, int b) { return a +@ b; }\n", TRUE, 'f', "return a + b;");
  ASSERT_OBJ(IDE_TS_LANG_C, "struct P { int x;@ int y; };\n", TRUE, 'c', "int x; int y;");
  /* a struct reference without a body is not a class object */
  ASSERT_OBJ(IDE_TS_LANG_C, "void f(struct P *@p) { }\n", TRUE, 'c', "<none>");
  ASSERT_OBJ(IDE_TS_LANG_RUST, "fn main() { let x = foo(1,@ 2); }\n", TRUE, 'f', "let x = foo(1, 2);");
  ASSERT_OBJ(IDE_TS_LANG_RUST, "struct S { a: i32 }\nimpl S { fn get(&self) -> i32 { self.a@ } }\n", TRUE, 'c',
             "fn get(&self) -> i32 { self.a }");
  ASSERT_OBJ(IDE_TS_LANG_JAVASCRIPT, "const g = (x) => x *@ 2;\n", TRUE, 'f', "x * 2");
  ASSERT_OBJ(IDE_TS_LANG_JAVASCRIPT, "class A { m(a, b) { return a@; } }\n", FALSE, 'f', "m(a, b) { return a; }");
  ASSERT_OBJ(IDE_TS_LANG_LUA, "function f(a)\n  return a@ + 1\nend\n", TRUE, 'f', "return a + 1");
}

/* Every bundled highlight query must compile against the installed grammar. */
static void
test_queries_compile(void)
{
  for (int l = IDE_TS_LANG_C; l < IDE_TS_LANG_COUNT; l++) {
    guint32 off = 0;
    if (!ide_ts_query_check((IdeTsLang)l, &off)) {
      const char *src = ide_ts_query_source((IdeTsLang)l);
      g_error("query for %s invalid at byte %u: ...%.60s", ide_ts_lang_name((IdeTsLang)l), off,
              src != NULL && off < strlen(src) ? src + off : "");
    }
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide-ts/lang-detection", test_lang_detection);
  g_test_add_func("/ide-ts/queries-compile", test_queries_compile);
  if (ide_ts_lang_available(IDE_TS_LANG_JAVA)) {
    g_test_add_func("/ide-ts/objects-java", test_text_objects_java);
  }
  if (ide_ts_lang_available(IDE_TS_LANG_GO)) {
    g_test_add_func("/ide-ts/objects-go", test_text_objects_go);
  }
  if (ide_ts_lang_available(IDE_TS_LANG_C) && ide_ts_lang_available(IDE_TS_LANG_RUST) &&
      ide_ts_lang_available(IDE_TS_LANG_JAVASCRIPT) && ide_ts_lang_available(IDE_TS_LANG_LUA)) {
    g_test_add_func("/ide-ts/objects-misc", test_text_objects_c_rust_js_lua);
  }
  g_test_add_func("/ide-ts/detection-extended", test_extended_detection);
  g_test_add_func("/ide-ts/query-languages", test_query_languages);
  g_test_add_func("/ide-ts/incremental-all", test_incremental_all_languages);

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
