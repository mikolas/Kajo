#include "ide_ts.h"

#include <string.h>

#ifdef HAVE_TREE_SITTER
#include <tree_sitter/api.h>
#endif

/* ─── Language table ─── */

/* Grammar entry points are declared by hand: several distributions ship no
 * header for a grammar (tree-sitter-go, -cpp, -json, -toml), and the exported
 * symbol is all that is needed. Absent grammars resolve to NULL. */
typedef const void *(*IdeTsGrammarFn)(void);

#define DECLARE_GRAMMAR(UP, lo) \
  const void *tree_sitter_##lo(void); \
  static IdeTsGrammarFn const kGrammar_##UP = tree_sitter_##lo;
#define MISSING_GRAMMAR(UP) static IdeTsGrammarFn const kGrammar_##UP = NULL;

#ifdef HAVE_TS_C
DECLARE_GRAMMAR(C, c)
#else
MISSING_GRAMMAR(C)
#endif
#ifdef HAVE_TS_JAVA
DECLARE_GRAMMAR(JAVA, java)
#else
MISSING_GRAMMAR(JAVA)
#endif
#ifdef HAVE_TS_GO
DECLARE_GRAMMAR(GO, go)
#else
MISSING_GRAMMAR(GO)
#endif
#ifdef HAVE_TS_CPP
DECLARE_GRAMMAR(CPP, cpp)
#else
MISSING_GRAMMAR(CPP)
#endif
#ifdef HAVE_TS_RUST
DECLARE_GRAMMAR(RUST, rust)
#else
MISSING_GRAMMAR(RUST)
#endif
#ifdef HAVE_TS_BASH
DECLARE_GRAMMAR(BASH, bash)
#else
MISSING_GRAMMAR(BASH)
#endif
#ifdef HAVE_TS_JAVASCRIPT
DECLARE_GRAMMAR(JAVASCRIPT, javascript)
#else
MISSING_GRAMMAR(JAVASCRIPT)
#endif
#ifdef HAVE_TS_TYPESCRIPT
DECLARE_GRAMMAR(TYPESCRIPT, typescript)
#else
MISSING_GRAMMAR(TYPESCRIPT)
#endif
#ifdef HAVE_TS_TSX
DECLARE_GRAMMAR(TSX, tsx)
#else
MISSING_GRAMMAR(TSX)
#endif
#ifdef HAVE_TS_JSON
DECLARE_GRAMMAR(JSON, json)
#else
MISSING_GRAMMAR(JSON)
#endif
#ifdef HAVE_TS_TOML
DECLARE_GRAMMAR(TOML, toml)
#else
MISSING_GRAMMAR(TOML)
#endif
#ifdef HAVE_TS_YAML
DECLARE_GRAMMAR(YAML, yaml)
#else
MISSING_GRAMMAR(YAML)
#endif
#ifdef HAVE_TS_CSS
DECLARE_GRAMMAR(CSS, css)
#else
MISSING_GRAMMAR(CSS)
#endif
#ifdef HAVE_TS_HTML
DECLARE_GRAMMAR(HTML, html)
#else
MISSING_GRAMMAR(HTML)
#endif
#ifdef HAVE_TS_LUA
DECLARE_GRAMMAR(LUA, lua)
#else
MISSING_GRAMMAR(LUA)
#endif
#ifdef HAVE_TS_M68K
DECLARE_GRAMMAR(M68K, m68k)
#else
MISSING_GRAMMAR(M68K)
#endif

typedef struct {
  IdeTsLang       lang;
  const char     *name;
  gboolean        base_anon;   /* color keyword / operator tokens generically */
  const char     *exts[10];    /* file suffixes */
  const char     *ids[8];      /* bare language ids (Markdown fences, previews) */
  const char     *files[8];    /* exact base file names */
} IdeTsLangInfo;

static const IdeTsLangInfo kLangs[] = {
  { IDE_TS_LANG_C, "c", TRUE,
    { ".c", ".h", NULL }, { "c", "h", NULL }, { NULL } },
  { IDE_TS_LANG_JAVA, "java", TRUE,
    { ".java", NULL }, { "java", NULL }, { NULL } },
  { IDE_TS_LANG_GO, "go", TRUE,
    { ".go", NULL }, { "go", "golang", NULL }, { NULL } },
  { IDE_TS_LANG_CPP, "cpp", TRUE,
    { ".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".h++", NULL },
    { "cpp", "c++", "cc", "cxx", NULL }, { NULL } },
  { IDE_TS_LANG_RUST, "rust", TRUE,
    { ".rs", NULL }, { "rust", "rs", NULL }, { NULL } },
  { IDE_TS_LANG_BASH, "bash", TRUE,
    { ".sh", ".bash", ".zsh", NULL }, { "sh", "bash", "zsh", "shell", NULL },
    { ".bashrc", ".bash_profile", ".bash_aliases", ".zshrc", ".zshenv", ".profile", "PKGBUILD", NULL } },
  { IDE_TS_LANG_JAVASCRIPT, "javascript", TRUE,
    { ".js", ".mjs", ".cjs", ".jsx", NULL }, { "js", "javascript", "jsx", "node", NULL }, { NULL } },
  { IDE_TS_LANG_TYPESCRIPT, "typescript", TRUE,
    { ".ts", ".mts", ".cts", NULL }, { "ts", "typescript", NULL }, { NULL } },
  { IDE_TS_LANG_TSX, "tsx", TRUE,
    { ".tsx", NULL }, { "tsx", NULL }, { NULL } },
  { IDE_TS_LANG_JSON, "json", FALSE,
    { ".json", ".geojson", ".webmanifest", NULL }, { "json", NULL }, { NULL } },
  { IDE_TS_LANG_TOML, "toml", FALSE,
    { ".toml", NULL }, { "toml", NULL }, { "Cargo.lock", NULL } },
  { IDE_TS_LANG_YAML, "yaml", FALSE,
    { ".yaml", ".yml", NULL }, { "yaml", "yml", NULL }, { ".clang-format", ".clang-tidy", NULL } },
  { IDE_TS_LANG_CSS, "css", FALSE,
    { ".css", NULL }, { "css", NULL }, { NULL } },
  { IDE_TS_LANG_HTML, "html", FALSE,
    { ".html", ".htm", ".xhtml", NULL }, { "html", "htm", NULL }, { NULL } },
  { IDE_TS_LANG_LUA, "lua", TRUE,
    { ".lua", NULL }, { "lua", NULL }, { NULL } },
  { IDE_TS_LANG_M68K, "m68k", TRUE,
    { ".s", ".S", ".asm", ".ASM", ".i", ".I", NULL },
    { "m68k", "68k", "asm", "vasm", "vasmmot", NULL }, { NULL } },
};

static IdeTsGrammarFn
grammar_fn_for(IdeTsLang lang)
{
  switch (lang) {
    case IDE_TS_LANG_C:          return kGrammar_C;
    case IDE_TS_LANG_JAVA:       return kGrammar_JAVA;
    case IDE_TS_LANG_GO:         return kGrammar_GO;
    case IDE_TS_LANG_CPP:        return kGrammar_CPP;
    case IDE_TS_LANG_RUST:       return kGrammar_RUST;
    case IDE_TS_LANG_BASH:       return kGrammar_BASH;
    case IDE_TS_LANG_JAVASCRIPT: return kGrammar_JAVASCRIPT;
    case IDE_TS_LANG_TYPESCRIPT: return kGrammar_TYPESCRIPT;
    case IDE_TS_LANG_TSX:        return kGrammar_TSX;
    case IDE_TS_LANG_JSON:       return kGrammar_JSON;
    case IDE_TS_LANG_TOML:       return kGrammar_TOML;
    case IDE_TS_LANG_YAML:       return kGrammar_YAML;
    case IDE_TS_LANG_CSS:        return kGrammar_CSS;
    case IDE_TS_LANG_HTML:       return kGrammar_HTML;
    case IDE_TS_LANG_LUA:        return kGrammar_LUA;
    case IDE_TS_LANG_M68K:       return kGrammar_M68K;
    default:                     return NULL;
  }
}

static const IdeTsLangInfo *
lang_info(IdeTsLang lang)
{
  for (gsize i = 0; i < G_N_ELEMENTS(kLangs); i++) {
    if (kLangs[i].lang == lang) {
      return &kLangs[i];
    }
  }
  return NULL;
}

gboolean
ide_ts_lang_available(IdeTsLang lang)
{
  return lang_info(lang) != NULL && grammar_fn_for(lang) != NULL;
}

const char *
ide_ts_lang_name(IdeTsLang lang)
{
  const IdeTsLangInfo *info = lang_info(lang);
  return info != NULL ? info->name : "none";
}

IdeTsLang
ide_ts_lang_for_path(const char *path_or_id)
{
  if (path_or_id == NULL || path_or_id[0] == '\0') {
    return IDE_TS_LANG_NONE;
  }

  const char *base = strrchr(path_or_id, '/');
  base = (base != NULL) ? base + 1 : path_or_id;

  for (gsize i = 0; i < G_N_ELEMENTS(kLangs); i++) {
    const IdeTsLangInfo *li = &kLangs[i];
    gboolean hit = FALSE;
    for (gsize k = 0; !hit && li->files[k] != NULL; k++) {
      hit = (strcmp(base, li->files[k]) == 0);
    }
    for (gsize k = 0; !hit && li->exts[k] != NULL; k++) {
      hit = g_str_has_suffix(base, li->exts[k]);
    }
    for (gsize k = 0; !hit && li->ids[k] != NULL; k++) {
      hit = (g_ascii_strcasecmp(path_or_id, li->ids[k]) == 0);
    }
    if (hit) {
      return ide_ts_lang_available(li->lang) ? li->lang : IDE_TS_LANG_NONE;
    }
  }
  return IDE_TS_LANG_NONE;
}

#ifndef HAVE_TREE_SITTER

/* Tree-sitter not available at build time: inert stubs, callers use the lexer. */
struct _IdeTsDoc { IdeTsLang lang; };
IdeTsDoc *ide_ts_doc_new(IdeTsLang lang) { (void)lang; return NULL; }
void ide_ts_doc_free(IdeTsDoc *doc) { (void)doc; }
IdeTsLang ide_ts_doc_get_lang(const IdeTsDoc *doc) { (void)doc; return IDE_TS_LANG_NONE; }
gboolean ide_ts_doc_update(IdeTsDoc *doc, const char *text, gsize len, gsize *ds, gsize *de)
{ (void)doc; (void)text; (void)len; (void)ds; (void)de; return FALSE; }
void ide_ts_doc_walk(const IdeTsDoc *doc, gsize from, gsize to, IdeTsTokenFunc cb, gpointer ud)
{ (void)doc; (void)from; (void)to; (void)cb; (void)ud; }
gboolean ide_ts_doc_has_error(const IdeTsDoc *doc) { (void)doc; return FALSE; }
gboolean ide_ts_doc_find_object(const IdeTsDoc *doc, gsize c, gboolean in, char t, gsize *s, gsize *e)
{ (void)doc; (void)c; (void)in; (void)t; (void)s; (void)e; return FALSE; }
gboolean ide_ts_query_check(IdeTsLang lang, guint32 *error_offset)
{ (void)lang; if (error_offset) *error_offset = 0; return TRUE; }

#else /* HAVE_TREE_SITTER */

struct _IdeTsDoc {
  IdeTsLang            lang;
  const IdeTsLangInfo *info;
  TSParser            *parser;
  TSTree              *tree;
  char                *text;
  gsize                len;
};

IdeTsDoc *
ide_ts_doc_new(IdeTsLang lang)
{
  const IdeTsLangInfo *info = lang_info(lang);
  IdeTsGrammarFn fn = grammar_fn_for(lang);
  if (info == NULL || fn == NULL) {
    return NULL;
  }
  IdeTsDoc *doc = g_new0(IdeTsDoc, 1);
  doc->lang = lang;
  doc->info = info;
  doc->parser = ts_parser_new();
  if (!ts_parser_set_language(doc->parser, (const TSLanguage *)fn())) {
    ts_parser_delete(doc->parser);
    g_free(doc);
    return NULL;
  }
  return doc;
}

void
ide_ts_doc_free(IdeTsDoc *doc)
{
  if (doc == NULL) {
    return;
  }
  if (doc->tree != NULL) {
    ts_tree_delete(doc->tree);
  }
  if (doc->parser != NULL) {
    ts_parser_delete(doc->parser);
  }
  g_free(doc->text);
  g_free(doc);
}

IdeTsLang
ide_ts_doc_get_lang(const IdeTsDoc *doc)
{
  return doc != NULL ? doc->lang : IDE_TS_LANG_NONE;
}

/* ─── Incremental parsing ─── */

static TSPoint
point_at(const char *text, gsize byte)
{
  TSPoint pt = { 0, 0 };
  for (gsize i = 0; i < byte; i++) {
    if (text[i] == '\n') {
      pt.row++;
      pt.column = 0;
    } else {
      pt.column++;
    }
  }
  return pt;
}

/* Multi-line leaf-ish nodes (strings, comments, heredocs, raw text) are
 * painted as one span; widen the dirty window so such a span is never cut. */
static gboolean
is_span_node(const char *type)
{
  static const char *const kNeedles[] = {
    "string", "comment", "literal", "template", "raw", "heredoc", "text_block", "scalar", NULL
  };
  for (gsize i = 0; kNeedles[i] != NULL; i++) {
    if (strstr(type, kNeedles[i]) != NULL) {
      return TRUE;
    }
  }
  return FALSE;
}

static void
widen_to_span_nodes(const IdeTsDoc *doc, gsize *ds, gsize *de)
{
  TSNode root = ts_tree_root_node(doc->tree);
  gsize pts[2] = { *ds, *de };
  for (gsize k = 0; k < 2; k++) {
    TSNode n = ts_node_descendant_for_byte_range(root, (uint32_t)pts[k], (uint32_t)pts[k]);
    while (!ts_node_is_null(n)) {
      if (is_span_node(ts_node_type(n))) {
        *ds = MIN(*ds, (gsize)ts_node_start_byte(n));
        *de = MAX(*de, (gsize)ts_node_end_byte(n));
      }
      n = ts_node_parent(n);
    }
  }
}

gboolean
ide_ts_doc_update(IdeTsDoc   *doc,
                  const char *text,
                  gsize       len,
                  gsize      *dirty_start,
                  gsize      *dirty_end)
{
  g_return_val_if_fail(doc != NULL, FALSE);
  g_return_val_if_fail(text != NULL, FALSE);

  gsize ds = 0;
  gsize de = len;

  if (doc->tree != NULL && doc->text != NULL) {
    gsize old_len = doc->len;
    gsize max_common = MIN(old_len, len);
    gsize pre = 0;
    while (pre < max_common && doc->text[pre] == text[pre]) {
      pre++;
    }
    if (pre == old_len && pre == len) {
      if (dirty_start) *dirty_start = 0;
      if (dirty_end) *dirty_end = 0;
      return TRUE; /* unchanged */
    }
    gsize suf = 0;
    while (suf < max_common - pre &&
           doc->text[old_len - 1 - suf] == text[len - 1 - suf]) {
      suf++;
    }

    TSInputEdit edit;
    edit.start_byte = (uint32_t)pre;
    edit.old_end_byte = (uint32_t)(old_len - suf);
    edit.new_end_byte = (uint32_t)(len - suf);
    edit.start_point = point_at(doc->text, pre);
    edit.old_end_point = point_at(doc->text, old_len - suf);
    edit.new_end_point = point_at(text, len - suf);
    ts_tree_edit(doc->tree, &edit);

    TSTree *old_tree = doc->tree;
    TSTree *new_tree = ts_parser_parse_string(doc->parser, old_tree, text, (uint32_t)len);
    if (new_tree == NULL) {
      return FALSE;
    }

    ds = pre;
    de = len - suf;
    uint32_t n_ranges = 0;
    TSRange *ranges = ts_tree_get_changed_ranges(old_tree, new_tree, &n_ranges);
    for (uint32_t i = 0; i < n_ranges; i++) {
      ds = MIN(ds, (gsize)ranges[i].start_byte);
      de = MAX(de, (gsize)ranges[i].end_byte);
    }
    free(ranges);

    ts_tree_delete(old_tree);
    doc->tree = new_tree;
  } else {
    TSTree *tree = ts_parser_parse_string(doc->parser, NULL, text, (uint32_t)len);
    if (tree == NULL) {
      return FALSE;
    }
    if (doc->tree != NULL) {
      ts_tree_delete(doc->tree);
    }
    doc->tree = tree;
  }

  g_free(doc->text);
  doc->text = g_strndup(text, len);
  doc->len = len;

  de = MIN(de, len);
  ds = MIN(ds, de);
  widen_to_span_nodes(doc, &ds, &de);

  if (dirty_start) *dirty_start = ds;
  if (dirty_end) *dirty_end = MIN(de, len);
  return TRUE;
}

gboolean
ide_ts_doc_has_error(const IdeTsDoc *doc)
{
  if (doc == NULL || doc->tree == NULL) {
    return FALSE;
  }
  return ts_node_has_error(ts_tree_root_node(doc->tree));
}

/* ─── Classification ─── */

typedef struct {
  const IdeTsDoc *doc;
  gsize           from;
  gsize           to;
  guint8         *paint; /* one IdeTsToken per byte of [from, to) */
} WalkCtx;

/* Paints [s, e) (clipped to the walk window) with `tok`. Later paints win. */
static void
paint_span(const WalkCtx *ctx, gsize s, gsize e, IdeTsToken tok)
{
  if (e <= ctx->from || s >= ctx->to) {
    return;
  }
  s = MAX(s, ctx->from);
  e = MIN(e, ctx->to);
  memset(ctx->paint + (s - ctx->from), (int)tok, e - s);
}

static gboolean
type_is(const char *type, const char *name)
{
  return strcmp(type, name) == 0;
}

static gboolean
type_in(const char *type, const char *const *names)
{
  for (gsize i = 0; names[i] != NULL; i++) {
    if (strcmp(type, names[i]) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_upper_snake(const char *s, gsize len)
{
  if (len < 2 || !(s[0] >= 'A' && s[0] <= 'Z')) {
    return FALSE;
  }
  for (gsize i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean
is_pascal(const char *s, gsize len)
{
  if (len < 2 || !(s[0] >= 'A' && s[0] <= 'Z')) {
    return FALSE;
  }
  for (gsize i = 1; i < len; i++) {
    if (s[i] >= 'a' && s[i] <= 'z') {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_keyword_text(const char *type)
{
  if (type[0] == '\0') {
    return FALSE;
  }
  for (const char *p = type; *p; p++) {
    if (!g_ascii_isalpha(*p) && *p != '_') {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean
is_operator_text(const char *type)
{
  if (type[0] == '\0') {
    return FALSE;
  }
  for (const char *p = type; *p; p++) {
    if (strchr("+-*/%&|^~!<>=?:", *p) == NULL) {
      return FALSE;
    }
  }
  return TRUE;
}

static IdeTsToken
classify_anonymous(IdeTsLang lang, const char *type, TSNode parent_node)
{
  const char *ptype = ts_node_is_null(parent_node) ? "" : ts_node_type(parent_node);

  if (type[0] == '@' && type[1] == '\0' &&
      (type_is(ptype, "annotation") || type_is(ptype, "marker_annotation"))) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (type[0] == '#' && (lang == IDE_TS_LANG_C || lang == IDE_TS_LANG_CPP)) {
    return IDE_TS_TOKEN_KEYWORD; /* #include, #define, #ifdef, ... */
  }
  if (is_keyword_text(type)) {
    static const char *const kControl[] = {
      "if", "else", "for", "while", "do", "switch", "case", "default", "break",
      "continue", "return", "goto", "throw", "try", "catch", "finally", "yield",
      "defer", "go", "select", "fallthrough", "elif", "then", "fi", "done", "esac",
      "until", "match", "loop", "repeat", "await", "rescue", NULL
    };
    if (type_in(type, kControl)) {
      return IDE_TS_TOKEN_CONTROL;
    }
    if (type_is(ptype, "sized_type_specifier")) {
      return IDE_TS_TOKEN_TYPE; /* unsigned, long, short, signed */
    }
    return IDE_TS_TOKEN_KEYWORD;
  }
  if (is_operator_text(type)) {
    return IDE_TS_TOKEN_OPERATOR;
  }
  return IDE_TS_TOKEN_NONE;
}

/* GLib scope-guard macros (g_autoptr(T) x, g_autofree char *p) are invisible to
 * the C grammar, which then mis-parses the declaration. Color them as the
 * declaration keywords they act as, and rescue builtin type names that the
 * recovery turned into identifiers. */
static gboolean
is_c_scope_macro(const char *txt, gsize len)
{
  static const char *const kMacros[] = {
    "g_autoptr", "g_autofree", "g_auto", "g_autolist", "g_autoslist", "g_autoqueue", NULL
  };
  for (gsize i = 0; kMacros[i] != NULL; i++) {
    if (strlen(kMacros[i]) == len && strncmp(kMacros[i], txt, len) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_c_builtin_type_word(const char *txt, gsize len)
{
  static const char *const kTypes[] = {
    "char", "int", "long", "short", "float", "double", "void", "unsigned", "signed", "bool", NULL
  };
  for (gsize i = 0; kTypes[i] != NULL; i++) {
    if (strlen(kTypes[i]) == len && strncmp(kTypes[i], txt, len) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static IdeTsToken
classify_identifier_fallback(IdeTsLang lang, const char *txt, gsize len)
{
  /* Contextual roles (method vs field, declaration names, calls, enum members,
   * annotations, scoped names) come from the per-language highlight query in
   * ide_ts_queries.c. Only the convention-based guesses live here. */
  if (lang == IDE_TS_LANG_C && is_c_scope_macro(txt, len)) {
    return IDE_TS_TOKEN_KEYWORD;
  }
  if (lang == IDE_TS_LANG_C && is_c_builtin_type_word(txt, len)) {
    return IDE_TS_TOKEN_TYPE;
  }
  if (is_upper_snake(txt, len)) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (lang == IDE_TS_LANG_JAVA && is_pascal(txt, len)) {
    return IDE_TS_TOKEN_TYPE; /* Math, System, Collections in expression position */
  }
  return IDE_TS_TOKEN_VARIABLE;
}


/* The C grammar keeps a #define body opaque (preproc_arg). Tokenize it with a
 * tiny scanner so macro bodies are not left uncolored. */
static void
emit_preproc_arg(const WalkCtx *ctx, gsize s, gsize e)
{
  static const char *const kCtrl[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", NULL
  };
  static const char *const kKw[] = {
    "static", "const", "struct", "enum", "union", "typedef", "extern", "inline",
    "volatile", "sizeof", "register", "restrict", NULL
  };
  static const char *const kTypes[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "bool", NULL
  };
  const char *t = ctx->doc->text;
  gsize i = s;
  while (i < e) {
    char c = t[i];
    if (g_ascii_isspace(c)) {
      i++;
    } else if (c == '"' || c == '\'') {
      gsize st = i++;
      while (i < e && t[i] != c) {
        if (t[i] == '\\' && i + 1 < e) {
          i++;
        }
        i++;
      }
      i = MIN(i + 1, e);
      paint_span(ctx, st, i, IDE_TS_TOKEN_STRING);
    } else if (c == '/' && i + 1 < e && t[i + 1] == '*') {
      gsize st = i;
      i += 2;
      while (i + 1 < e && !(t[i] == '*' && t[i + 1] == '/')) {
        i++;
      }
      i = MIN(i + 2, e);
      paint_span(ctx, st, i, IDE_TS_TOKEN_COMMENT);
    } else if (c == '/' && i + 1 < e && t[i + 1] == '/') {
      paint_span(ctx, i, e, IDE_TS_TOKEN_COMMENT);
      return;
    } else if (g_ascii_isdigit(c)) {
      gsize st = i;
      while (i < e && (g_ascii_isalnum(t[i]) || t[i] == '.' || t[i] == '_')) {
        i++;
      }
      paint_span(ctx, st, i, IDE_TS_TOKEN_NUMBER);
    } else if (g_ascii_isalpha(c) || c == '_') {
      gsize st = i;
      while (i < e && (g_ascii_isalnum(t[i]) || t[i] == '_')) {
        i++;
      }
      g_autofree char *w = g_strndup(t + st, i - st);
      gsize k = i;
      while (k < e && (t[k] == ' ' || t[k] == '\t')) {
        k++;
      }
      IdeTsToken tok = IDE_TS_TOKEN_NONE;
      if (type_in(w, kCtrl)) {
        tok = IDE_TS_TOKEN_CONTROL;
      } else if (type_in(w, kKw)) {
        tok = IDE_TS_TOKEN_KEYWORD;
      } else if (type_in(w, kTypes) || (i - st > 2 && g_str_has_suffix(w, "_t"))) {
        tok = IDE_TS_TOKEN_TYPE;
      } else if (strcmp(w, "NULL") == 0 || strcmp(w, "true") == 0 || strcmp(w, "false") == 0 ||
                 is_upper_snake(w, i - st)) {
        tok = IDE_TS_TOKEN_MACRO;
      } else if (k < e && t[k] == '(') {
        tok = IDE_TS_TOKEN_FUNCTION;
      } else {
        tok = IDE_TS_TOKEN_VARIABLE;
      }
      paint_span(ctx, st, i, tok);
    } else {
      gsize st = i;
      while (i < e && strchr("+-*/%&|^~!<>=?:", t[i]) != NULL) {
        i++;
      }
      if (i > st) {
        paint_span(ctx, st, i, IDE_TS_TOKEN_OPERATOR);
      } else {
        i++;
      }
    }
  }
}

/* Returns TRUE if the node was fully handled (no descent needed). */
static gboolean
visit_node(const WalkCtx *ctx, TSNode n)
{
  const char *type = ts_node_type(n);
  gsize s = ts_node_start_byte(n);
  gsize e = ts_node_end_byte(n);
  gboolean named = ts_node_is_named(n);
  uint32_t children = ts_node_child_count(n);
  const char *txt = ctx->doc->text + s;
  gsize len = e - s;

#define EMIT(tok) paint_span(ctx, s, e, (tok))

  if (ts_node_is_missing(n) || len == 0) {
    return TRUE;
  }

  if (named) {
    if (strstr(type, "comment") != NULL) {
      EMIT(IDE_TS_TOKEN_COMMENT);
      return TRUE;
    }

    static const char *const kStringContainers[] = {
      "string_literal", "char_literal", "character_literal", "system_lib_string",
      "interpreted_string_literal", "rune_literal", NULL
    };
    if (type_in(type, kStringContainers)) {
      EMIT(IDE_TS_TOKEN_STRING);
      return TRUE;
    }
    if (type_is(type, "raw_string_literal")) {
      EMIT(IDE_TS_TOKEN_STRING_RAW);
      return TRUE;
    }

    static const char *const kNumbers[] = {
      "number_literal", "int_literal", "float_literal", "imaginary_literal",
      "integer_literal", "number", "integer", "float", "integer_value", "float_value",
      "decimal_integer_literal", "hex_integer_literal", "octal_integer_literal",
      "binary_integer_literal", "decimal_floating_point_literal",
      "hex_floating_point_literal", NULL
    };
    if (type_in(type, kNumbers)) {
      EMIT(IDE_TS_TOKEN_NUMBER);
      return TRUE;
    }

    static const char *const kConstants[] = {
      "true", "false", "null", "nil", "iota", "null_literal", "nullptr", "undefined", NULL
    };
    if (type_in(type, kConstants)) {
      EMIT(IDE_TS_TOKEN_MACRO);
      return TRUE;
    }
    if (type_is(type, "this") || type_is(type, "super")) {
      EMIT(IDE_TS_TOKEN_KEYWORD);
      return TRUE;
    }

    static const char *const kTypes[] = {
      "type_identifier", "primitive_type", "integral_type", "floating_point_type",
      "boolean_type", "void_type", "predefined_type", NULL
    };
    if (type_in(type, kTypes)) {
      EMIT(ctx->doc->lang == IDE_TS_LANG_C && is_c_scope_macro(txt, len) ? IDE_TS_TOKEN_KEYWORD
                                                                          : IDE_TS_TOKEN_TYPE);
      return TRUE;
    }

    if (type_is(type, "preproc_arg")) {
      emit_preproc_arg(ctx, s, e);
      return TRUE;
    }
    if (type_is(type, "preproc_directive")) {
      EMIT(IDE_TS_TOKEN_KEYWORD);
      return TRUE;
    }

    if (children == 0 || type_is(type, "identifier")) {
      IdeTsToken tok = IDE_TS_TOKEN_NONE;
      gboolean is_ident = type_is(type, "identifier") || type_is(type, "field_identifier") ||
                          type_is(type, "statement_identifier") ||
                          type_is(type, "package_identifier") ||
                          type_is(type, "label_name") || type_is(type, "blank_identifier");
      if (is_ident) {
        tok = classify_identifier_fallback(ctx->doc->lang, txt, len);
      }
      if (tok != IDE_TS_TOKEN_NONE) {
        EMIT(tok);
      }
      return TRUE;
    }
    return FALSE; /* structural node: descend */
  }

  /* anonymous token (keyword / operator / punctuation) */
  if (!ctx->doc->info->base_anon) {
    return TRUE;
  }
  IdeTsToken tok = classify_anonymous(ctx->doc->lang, type, ts_node_parent(n));
  if (tok != IDE_TS_TOKEN_NONE) {
    EMIT(tok);
  }
  return TRUE;
#undef EMIT
}

/* ─── Query layer ───
 *
 * C, Java and Go are classified by the hand-written code above. Every other
 * language is driven by tree-sitter queries (ide_ts_queries.c): capture names
 * map onto token classes, later patterns override earlier ones, and the common
 * textual predicates (#eq? #not-eq? #match? #not-match? #any-of?) are evaluated
 * here since libtree-sitter leaves them to the host. */

typedef enum {
  PRED_EQ,
  PRED_NOT_EQ,
  PRED_MATCH,
  PRED_NOT_MATCH,
  PRED_ANY_OF,
} PredOp;

typedef struct {
  PredOp     op;
  uint32_t   capture;
  int        other_capture; /* -1 when comparing against a string */
  char      *str;
  GRegex    *regex;
  GPtrArray *alternatives;
} QueryPred;

static void
query_pred_free(gpointer data)
{
  QueryPred *p = data;
  g_free(p->str);
  g_clear_pointer(&p->regex, g_regex_unref);
  g_clear_pointer(&p->alternatives, g_ptr_array_unref);
  g_free(p);
}

typedef struct {
  TSQuery   *query;
  GPtrArray **preds;     /* per pattern: GPtrArray<QueryPred*> or NULL */
  guint      n_patterns;
  int       *cap_tokens; /* per capture id: IdeTsToken, or -1 to ignore */
  guint      n_captures;
} QueryData;

static int
token_for_capture(const char *name, gsize len)
{
  static const struct { const char *name; int token; } kMap[] = {
    { "keyword",         IDE_TS_TOKEN_KEYWORD },
    { "keyword.control", IDE_TS_TOKEN_CONTROL },
    { "function",        IDE_TS_TOKEN_FUNCTION },
    { "type",            IDE_TS_TOKEN_TYPE },
    { "constant",        IDE_TS_TOKEN_MACRO },
    { "variable",        IDE_TS_TOKEN_VARIABLE },
    { "string",          IDE_TS_TOKEN_STRING },
    { "string.raw",      IDE_TS_TOKEN_STRING_RAW },
    { "number",          IDE_TS_TOKEN_NUMBER },
    { "comment",         IDE_TS_TOKEN_COMMENT },
    { "operator",        IDE_TS_TOKEN_OPERATOR },
    { "none",            IDE_TS_TOKEN_NONE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(kMap); i++) {
    if (strlen(kMap[i].name) == len && strncmp(kMap[i].name, name, len) == 0) {
      return kMap[i].token;
    }
  }
  return -1; /* helper capture (e.g. @_name used only by predicates) */
}

static void
query_data_free(gpointer data)
{
  QueryData *qd = data;
  if (qd == NULL) {
    return;
  }
  if (qd->preds != NULL) {
    for (guint i = 0; i < qd->n_patterns; i++) {
      g_clear_pointer(&qd->preds[i], g_ptr_array_unref);
    }
    g_free(qd->preds);
  }
  g_free(qd->cap_tokens);
  if (qd->query != NULL) {
    ts_query_delete(qd->query);
  }
  g_free(qd);
}

static void
parse_predicates(QueryData *qd)
{
  for (guint pat = 0; pat < qd->n_patterns; pat++) {
    uint32_t n_steps = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(qd->query, pat, &n_steps);
    uint32_t i = 0;
    while (i < n_steps) {
      uint32_t j = i;
      while (j < n_steps && steps[j].type != TSQueryPredicateStepTypeDone) {
        j++;
      }
      /* steps[i..j) is one predicate: name, then arguments */
      if (j > i && steps[i].type == TSQueryPredicateStepTypeString) {
        uint32_t len = 0;
        const char *name = ts_query_string_value_for_id(qd->query, steps[i].value_id, &len);
        PredOp op = PRED_EQ;
        gboolean known = TRUE;
        if (strcmp(name, "eq?") == 0) op = PRED_EQ;
        else if (strcmp(name, "not-eq?") == 0) op = PRED_NOT_EQ;
        else if (strcmp(name, "match?") == 0) op = PRED_MATCH;
        else if (strcmp(name, "not-match?") == 0) op = PRED_NOT_MATCH;
        else if (strcmp(name, "any-of?") == 0) op = PRED_ANY_OF;
        else known = FALSE;

        if (known && j - i >= 3 && steps[i + 1].type == TSQueryPredicateStepTypeCapture) {
          QueryPred *p = g_new0(QueryPred, 1);
          p->op = op;
          p->capture = steps[i + 1].value_id;
          p->other_capture = -1;
          if (op == PRED_ANY_OF) {
            p->alternatives = g_ptr_array_new_with_free_func(g_free);
            for (uint32_t k = i + 2; k < j; k++) {
              uint32_t sl = 0;
              const char *sv = ts_query_string_value_for_id(qd->query, steps[k].value_id, &sl);
              g_ptr_array_add(p->alternatives, g_strndup(sv, sl));
            }
          } else if (steps[i + 2].type == TSQueryPredicateStepTypeCapture) {
            p->other_capture = (int)steps[i + 2].value_id;
          } else {
            uint32_t sl = 0;
            const char *sv = ts_query_string_value_for_id(qd->query, steps[i + 2].value_id, &sl);
            p->str = g_strndup(sv, sl);
            if (op == PRED_MATCH || op == PRED_NOT_MATCH) {
              p->regex = g_regex_new(p->str, G_REGEX_OPTIMIZE, 0, NULL);
            }
          }
          if (qd->preds[pat] == NULL) {
            qd->preds[pat] = g_ptr_array_new_with_free_func(query_pred_free);
          }
          g_ptr_array_add(qd->preds[pat], p);
        }
      }
      i = j + 1;
    }
  }
}

static QueryData *
query_data_compile(IdeTsLang lang, guint32 *error_offset)
{
  const char *src = ide_ts_query_source(lang);
  IdeTsGrammarFn fn = grammar_fn_for(lang);
  if (src == NULL || fn == NULL) {
    return NULL;
  }
  uint32_t err_off = 0;
  TSQueryError err_type = TSQueryErrorNone;
  TSQuery *q = ts_query_new((const TSLanguage *)fn(), src, (uint32_t)strlen(src), &err_off, &err_type);
  if (q == NULL) {
    if (error_offset != NULL) {
      *error_offset = err_off;
    }
    return NULL;
  }

  QueryData *qd = g_new0(QueryData, 1);
  qd->query = q;
  qd->n_patterns = ts_query_pattern_count(q);
  qd->preds = g_new0(GPtrArray *, qd->n_patterns ? qd->n_patterns : 1);
  qd->n_captures = ts_query_capture_count(q);
  qd->cap_tokens = g_new0(int, qd->n_captures ? qd->n_captures : 1);
  for (guint i = 0; i < qd->n_captures; i++) {
    uint32_t nl = 0;
    const char *nm = ts_query_capture_name_for_id(q, i, &nl);
    qd->cap_tokens[i] = token_for_capture(nm, nl);
  }
  parse_predicates(qd);
  return qd;
}

/* One compiled query per language, shared by all documents. */
static QueryData *
query_data_for(IdeTsLang lang)
{
  static GHashTable *cache = NULL;
  if (cache == NULL) {
    cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, query_data_free);
  }
  gpointer key = GINT_TO_POINTER((int)lang);
  gpointer val = NULL;
  if (g_hash_table_lookup_extended(cache, key, NULL, &val)) {
    return val;
  }
  guint32 off = 0;
  QueryData *qd = query_data_compile(lang, &off);
  if (qd == NULL && ide_ts_query_source(lang) != NULL) {
    g_warning("ide-ts: highlight query for %s is invalid (offset %u); using base highlighting only",
              ide_ts_lang_name(lang), off);
  }
  g_hash_table_insert(cache, key, qd);
  return qd;
}

gboolean
ide_ts_query_check(IdeTsLang lang, guint32 *error_offset)
{
  if (error_offset != NULL) {
    *error_offset = 0;
  }
  if (ide_ts_query_source(lang) == NULL || !ide_ts_lang_available(lang)) {
    return TRUE;
  }
  QueryData *qd = query_data_compile(lang, error_offset);
  if (qd == NULL) {
    return FALSE;
  }
  query_data_free(qd);
  return TRUE;
}

static gboolean
node_text_equals(const IdeTsDoc *doc, TSNode node, const char *str)
{
  gsize s = ts_node_start_byte(node);
  gsize e = ts_node_end_byte(node);
  return strlen(str) == e - s && strncmp(doc->text + s, str, e - s) == 0;
}

static gboolean
find_capture(const TSQueryMatch *m, uint32_t id, TSNode *out)
{
  for (uint16_t i = 0; i < m->capture_count; i++) {
    if (m->captures[i].index == id) {
      *out = m->captures[i].node;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
predicates_pass(const IdeTsDoc *doc, const QueryData *qd, const TSQueryMatch *m)
{
  GPtrArray *preds = qd->preds[m->pattern_index];
  if (preds == NULL) {
    return TRUE;
  }
  for (guint i = 0; i < preds->len; i++) {
    const QueryPred *p = g_ptr_array_index(preds, i);
    TSNode node;
    if (!find_capture(m, p->capture, &node)) {
      continue; /* optional capture absent: predicate does not apply */
    }
    gsize s = ts_node_start_byte(node);
    gsize e = ts_node_end_byte(node);
    gboolean ok = TRUE;
    switch (p->op) {
      case PRED_EQ:
      case PRED_NOT_EQ: {
        gboolean eq;
        if (p->other_capture >= 0) {
          TSNode other;
          if (!find_capture(m, (uint32_t)p->other_capture, &other)) {
            continue;
          }
          gsize os = ts_node_start_byte(other);
          gsize oe = ts_node_end_byte(other);
          eq = (oe - os == e - s) && strncmp(doc->text + s, doc->text + os, e - s) == 0;
        } else {
          eq = node_text_equals(doc, node, p->str);
        }
        ok = (p->op == PRED_EQ) ? eq : !eq;
        break;
      }
      case PRED_MATCH:
      case PRED_NOT_MATCH: {
        gboolean hit = FALSE;
        if (p->regex != NULL) {
          g_autofree char *t = g_strndup(doc->text + s, e - s);
          hit = g_regex_match(p->regex, t, 0, NULL);
        }
        ok = (p->op == PRED_MATCH) ? hit : !hit;
        break;
      }
      case PRED_ANY_OF: {
        ok = FALSE;
        for (guint k = 0; p->alternatives != NULL && k < p->alternatives->len; k++) {
          if (node_text_equals(doc, node, g_ptr_array_index(p->alternatives, k))) {
            ok = TRUE;
            break;
          }
        }
        break;
      }
    }
    if (!ok) {
      return FALSE;
    }
  }
  return TRUE;
}

typedef struct {
  guint    pattern;
  guint    seq;
  gsize    start;
  gsize    end;
  int      token;
} CaptureSpan;

static gint
capture_span_cmp(gconstpointer a, gconstpointer b)
{
  const CaptureSpan *x = a;
  const CaptureSpan *y = b;
  if (x->pattern != y->pattern) {
    return x->pattern < y->pattern ? -1 : 1;
  }
  return x->seq < y->seq ? -1 : (x->seq > y->seq);
}

static void
run_query_pass(const WalkCtx *ctx, const QueryData *qd)
{
  const IdeTsDoc *doc = ctx->doc;
  TSQueryCursor *qc = ts_query_cursor_new();
  ts_query_cursor_set_byte_range(qc, (uint32_t)ctx->from, (uint32_t)ctx->to);
  ts_query_cursor_exec(qc, qd->query, ts_tree_root_node(doc->tree));

  g_autoptr(GArray) spans = g_array_new(FALSE, FALSE, sizeof(CaptureSpan));
  TSQueryMatch m;
  guint seq = 0;
  while (ts_query_cursor_next_match(qc, &m)) {
    if (!predicates_pass(doc, qd, &m)) {
      continue;
    }
    for (uint16_t i = 0; i < m.capture_count; i++) {
      int tok = qd->cap_tokens[m.captures[i].index];
      if (tok < 0) {
        continue;
      }
      CaptureSpan sp = {
        m.pattern_index, seq++,
        ts_node_start_byte(m.captures[i].node),
        ts_node_end_byte(m.captures[i].node),
        tok
      };
      g_array_append_val(spans, sp);
    }
  }
  ts_query_cursor_delete(qc);

  g_array_sort(spans, capture_span_cmp);
  for (guint i = 0; i < spans->len; i++) {
    const CaptureSpan *sp = &g_array_index(spans, CaptureSpan, i);
    paint_span(ctx, sp->start, sp->end, (IdeTsToken)sp->token);
  }
}

/* ─── Text objects ─── */

static gboolean
node_in_list(TSNode n, const char *const *list)
{
  return type_in(ts_node_type(n), list);
}

static gboolean
node_is_braced(const IdeTsDoc *doc, TSNode n)
{
  gsize s = ts_node_start_byte(n);
  gsize e = ts_node_end_byte(n);
  if (e <= s + 1 || doc->text[s] != '{' || doc->text[e - 1] != '}') {
    return FALSE;
  }
  const char *t = ts_node_type(n);
  return strstr(t, "string") == NULL && strstr(t, "comment") == NULL &&
         strstr(t, "template") == NULL && strstr(t, "interpol") == NULL;
}

/* Finds the `{ ... }` body of a declaration: the `body` field, or the first
 * braced descendant (Go: type_declaration -> type_spec -> struct_type -> list). */
static gboolean
find_brace_body(const IdeTsDoc *doc, TSNode n, int depth, TSNode *out)
{
  TSNode body = ts_node_child_by_field_name(n, "body", 4);
  if (!ts_node_is_null(body)) {
    if (node_is_braced(doc, body)) {
      *out = body;
      return TRUE;
    }
    if (depth < 3 && find_brace_body(doc, body, depth + 1, out)) {
      return TRUE;
    }
    *out = body; /* expression body (arrow function, Lua block, Python-like) */
    return TRUE;
  }
  if (depth >= 4) {
    return FALSE;
  }
  uint32_t cnt = ts_node_named_child_count(n);
  for (uint32_t i = 0; i < cnt; i++) {
    TSNode c = ts_node_named_child(n, i);
    if (node_is_braced(doc, c)) {
      *out = c;
      return TRUE;
    }
    if (find_brace_body(doc, c, depth + 1, out)) {
      return TRUE;
    }
  }
  return FALSE;
}

static void
inside_of_braces(const IdeTsDoc *doc, TSNode n, gsize *s, gsize *e)
{
  gsize bs = ts_node_start_byte(n);
  gsize be = ts_node_end_byte(n);
  if (node_is_braced(doc, n)) {
    bs++;
    be--;
    gsize p = bs;
    while (p < be && (doc->text[p] == ' ' || doc->text[p] == '\t')) {
      p++;
    }
    /* multi-line body: keep the first line's indentation, drop the line break;
     * single-line body: trim the padding spaces. */
    bs = (p < be && doc->text[p] == '\n') ? p + 1 : p;
    while (be > bs && (doc->text[be - 1] == ' ' || doc->text[be - 1] == '\t')) {
      be--;
    }
    if (be > bs && doc->text[be - 1] == '\n') {
      be--;
    }
  }
  *s = bs;
  *e = be;
}

static const char *const kFunctionKinds[] = {
  "function_definition", "function_declaration", "function_item", "method_declaration",
  "method_definition", "constructor_declaration", "lambda_expression", "arrow_function",
  "function_expression", "generator_function_declaration", "generator_function",
  "func_literal", "function", "abstract_method_signature",
  "macro_definition", "repeat", NULL
};

static const char *const kClassKinds[] = {
  "class_declaration", "class_specifier", "struct_specifier", "union_specifier",
  "enum_specifier", "interface_declaration", "enum_declaration", "record_declaration",
  "annotation_type_declaration", "type_declaration", "struct_item", "enum_item",
  "trait_item", "impl_item", "union_item", "class", "abstract_class_declaration",
  "namespace_definition", "module", "internal_module", "object_type", NULL
};

static const char *const kListKinds[] = {
  "arguments", "argument_list", "parameter_list", "parameters", "formal_parameters",
  "type_arguments", "type_parameters", "array", "array_initializer", "array_expression",
  "initializer_list", "list", "tuple_expression", "table_constructor", "literal_value",
  "type_parameter_list", "template_argument_list", "template_parameter_list",
  "enumerator_list", "inferred_parameters", "expression_list",
  "operand_list", "option_list", "symbol_list", NULL
};

gboolean
ide_ts_doc_find_object(const IdeTsDoc *doc,
                       gsize           cursor_byte,
                       gboolean        inside,
                       char            target,
                       gsize          *out_start,
                       gsize          *out_end)
{
  g_return_val_if_fail(out_start != NULL && out_end != NULL, FALSE);
  if (doc == NULL || doc->tree == NULL || doc->text == NULL) {
    return FALSE;
  }
  cursor_byte = MIN(cursor_byte, doc->len);

  TSNode root = ts_tree_root_node(doc->tree);
  TSNode leaf = ts_node_descendant_for_byte_range(root, (uint32_t)cursor_byte, (uint32_t)cursor_byte);
  if (ts_node_is_null(leaf)) {
    return FALSE;
  }

  for (TSNode n = leaf; !ts_node_is_null(n); n = ts_node_parent(n)) {
    gsize ns = ts_node_start_byte(n);
    gsize ne = ts_node_end_byte(n);

    if (target == 'f' || target == 'c') {
      gboolean match = (target == 'f') ? node_in_list(n, kFunctionKinds) : node_in_list(n, kClassKinds);
      if (!match) {
        continue;
      }
      TSNode body;
      if (!find_brace_body(doc, n, 0, &body)) {
        continue; /* e.g. `struct foo *p` or a prototype without a body */
      }
      if (!inside) {
        *out_start = ns;
        *out_end = ne;
        return TRUE;
      }
      inside_of_braces(doc, body, out_start, out_end);
      return TRUE;
    }

    if (target == 'b') {
      if (!node_is_braced(doc, n)) {
        continue;
      }
      if (!inside) {
        *out_start = ns;
        *out_end = ne;
      } else {
        inside_of_braces(doc, n, out_start, out_end);
      }
      return TRUE;
    }

    if (target == 'a') {
      if (!node_in_list(n, kListKinds)) {
        continue;
      }
      uint32_t cnt = ts_node_named_child_count(n);
      TSNode pick;
      gboolean found = FALSE;
      for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_named_child(n, i);
        if (strstr(ts_node_type(c), "comment") != NULL) {
          continue;
        }
        gsize cs = ts_node_start_byte(c);
        gsize ce = ts_node_end_byte(c);
        if (cursor_byte <= ce && (cursor_byte >= cs || !found)) {
          pick = c;
          found = TRUE;
          if (cursor_byte >= cs) {
            break; /* containing element */
          }
          break;   /* cursor in the gap before this element */
        }
      }
      if (!found) {
        continue;
      }
      gsize s = ts_node_start_byte(pick);
      gsize e = ts_node_end_byte(pick);
      if (!inside) {
        /* take one adjacent comma (and the whitespace after it) */
        gsize k = e;
        while (k < ne && (doc->text[k] == ' ' || doc->text[k] == '\t')) {
          k++;
        }
        if (k < ne && doc->text[k] == ',') {
          e = k + 1;
          while (e < ne && (doc->text[e] == ' ' || doc->text[e] == '\t')) {
            e++;
          }
        } else {
          gsize b = s;
          while (b > ns && (doc->text[b - 1] == ' ' || doc->text[b - 1] == '\t')) {
            b--;
          }
          if (b > ns && doc->text[b - 1] == ',') {
            s = b - 1;
          }
        }
      }
      *out_start = s;
      *out_end = e;
      return TRUE;
    }
    return FALSE;
  }
  return FALSE;
}

/* ─── Walk ─── */

void
ide_ts_doc_walk(const IdeTsDoc *doc,
                gsize           from,
                gsize           to,
                IdeTsTokenFunc  cb,
                gpointer        user_data)
{
  if (doc == NULL || doc->tree == NULL || cb == NULL) {
    return;
  }
  to = MIN(to, doc->len);
  if (to <= from) {
    return;
  }

  g_autofree guint8 *paint = g_malloc0(to - from);
  WalkCtx ctx = { doc, from, to, paint };

  /* Pass 1: structural base (comments, strings, numbers, keywords, types and,
   * for C / Java / Go, the full identifier classification). */
  TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(doc->tree));
  for (;;) {
    TSNode n = ts_tree_cursor_current_node(&cursor);
    gsize s = ts_node_start_byte(n);
    gsize e = ts_node_end_byte(n);
    gboolean handled = TRUE;

    if (e > from && s < to) {
      handled = visit_node(&ctx, n);
    }
    if (!handled && ts_tree_cursor_goto_first_child(&cursor)) {
      continue;
    }
    gboolean done = FALSE;
    while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
      if (!ts_tree_cursor_goto_parent(&cursor)) {
        done = TRUE;
        break;
      }
    }
    if (done) {
      break;
    }
  }
  ts_tree_cursor_delete(&cursor);

  /* Pass 2: language queries refine the base. */
  QueryData *qd = query_data_for(doc->lang);
  if (qd != NULL) {
    run_query_pass(&ctx, qd);
  }

  /* Emit contiguous runs of equal tokens. */
  gsize i = 0;
  gsize n = to - from;
  while (i < n) {
    guint8 tok = paint[i];
    gsize j = i + 1;
    while (j < n && paint[j] == tok) {
      j++;
    }
    if (tok != IDE_TS_TOKEN_NONE) {
      cb(from + i, from + j, (IdeTsToken)tok, user_data);
    }
    i = j;
  }
}

#endif /* HAVE_TREE_SITTER */
