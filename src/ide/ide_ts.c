#include "ide_ts.h"

#include <string.h>

#ifdef HAVE_TREE_SITTER
#include <tree_sitter/api.h>

/* Grammar entry points. Declared by hand: some distributions ship no header
 * for a grammar (tree-sitter-go), and the symbol is all we need. */
#ifdef HAVE_TS_C
const TSLanguage *tree_sitter_c(void);
#endif
#ifdef HAVE_TS_JAVA
const TSLanguage *tree_sitter_java(void);
#endif
#ifdef HAVE_TS_GO
const TSLanguage *tree_sitter_go(void);
#endif
#endif /* HAVE_TREE_SITTER */

/* ─── Language registry ─── */

gboolean
ide_ts_lang_available(IdeTsLang lang)
{
  switch (lang) {
#ifdef HAVE_TS_C
    case IDE_TS_LANG_C:
      return TRUE;
#endif
#ifdef HAVE_TS_JAVA
    case IDE_TS_LANG_JAVA:
      return TRUE;
#endif
#ifdef HAVE_TS_GO
    case IDE_TS_LANG_GO:
      return TRUE;
#endif
    default:
      return FALSE;
  }
}

const char *
ide_ts_lang_name(IdeTsLang lang)
{
  switch (lang) {
    case IDE_TS_LANG_C:    return "c";
    case IDE_TS_LANG_JAVA: return "java";
    case IDE_TS_LANG_GO:   return "go";
    default:               return "none";
  }
}

IdeTsLang
ide_ts_lang_for_path(const char *path_or_id)
{
  if (path_or_id == NULL || path_or_id[0] == '\0') {
    return IDE_TS_LANG_NONE;
  }

  IdeTsLang lang = IDE_TS_LANG_NONE;
  if (g_str_has_suffix(path_or_id, ".c") || g_str_has_suffix(path_or_id, ".h") ||
      g_ascii_strcasecmp(path_or_id, "c") == 0 || g_ascii_strcasecmp(path_or_id, "h") == 0) {
    lang = IDE_TS_LANG_C;
  } else if (g_str_has_suffix(path_or_id, ".java") ||
             g_ascii_strcasecmp(path_or_id, "java") == 0) {
    lang = IDE_TS_LANG_JAVA;
  } else if (g_str_has_suffix(path_or_id, ".go") ||
             g_ascii_strcasecmp(path_or_id, "go") == 0 ||
             g_ascii_strcasecmp(path_or_id, "golang") == 0) {
    lang = IDE_TS_LANG_GO;
  }
  return ide_ts_lang_available(lang) ? lang : IDE_TS_LANG_NONE;
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

#else /* HAVE_TREE_SITTER */

struct _IdeTsDoc {
  IdeTsLang  lang;
  TSParser  *parser;
  TSTree    *tree;
  char      *text;
  gsize      len;
};

static const TSLanguage *
grammar_for(IdeTsLang lang)
{
  switch (lang) {
#ifdef HAVE_TS_C
    case IDE_TS_LANG_C:    return tree_sitter_c();
#endif
#ifdef HAVE_TS_JAVA
    case IDE_TS_LANG_JAVA: return tree_sitter_java();
#endif
#ifdef HAVE_TS_GO
    case IDE_TS_LANG_GO:   return tree_sitter_go();
#endif
    default:               return NULL;
  }
}

IdeTsDoc *
ide_ts_doc_new(IdeTsLang lang)
{
  const TSLanguage *grammar = grammar_for(lang);
  if (grammar == NULL) {
    return NULL;
  }
  IdeTsDoc *doc = g_new0(IdeTsDoc, 1);
  doc->lang = lang;
  doc->parser = ts_parser_new();
  if (!ts_parser_set_language(doc->parser, grammar)) {
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
  IdeTsTokenFunc  cb;
  gpointer        user_data;
} WalkCtx;

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
is_field_of(TSNode parent, const char *field, TSNode child)
{
  if (ts_node_is_null(parent)) {
    return FALSE;
  }
  TSNode f = ts_node_child_by_field_name(parent, field, (uint32_t)strlen(field));
  return !ts_node_is_null(f) && ts_node_eq(f, child);
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
classify_anonymous(const char *type, TSNode parent_node)
{
  const char *ptype = ts_node_is_null(parent_node) ? "" : ts_node_type(parent_node);

  if (type[0] == '@' && type[1] == '\0' &&
      (type_is(ptype, "annotation") || type_is(ptype, "marker_annotation"))) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (type[0] == '#') {
    return IDE_TS_TOKEN_KEYWORD; /* #include, #define, #ifdef, ... */
  }
  if (is_keyword_text(type)) {
    static const char *const kControl[] = {
      "if", "else", "for", "while", "do", "switch", "case", "default", "break",
      "continue", "return", "goto", "throw", "try", "catch", "finally", "yield",
      "defer", "go", "select", "fallthrough", NULL
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

static IdeTsToken
classify_identifier_c(TSNode n, const char *type, const char *txt, gsize len)
{
  TSNode parent = ts_node_parent(n);
  const char *ptype = ts_node_is_null(parent) ? "" : ts_node_type(parent);

  if (type_is(type, "field_identifier")) {
    if (type_is(ptype, "field_expression")) {
      TSNode gp = ts_node_parent(parent);
      if (!ts_node_is_null(gp) && type_is(ts_node_type(gp), "call_expression") &&
          is_field_of(gp, "function", parent)) {
        return IDE_TS_TOKEN_FUNCTION;
      }
    }
    return IDE_TS_TOKEN_VARIABLE;
  }
  if (type_is(type, "statement_identifier")) {
    return IDE_TS_TOKEN_VARIABLE;
  }

  /* plain identifier */
  if (type_is(ptype, "function_declarator") && is_field_of(parent, "declarator", n)) {
    return IDE_TS_TOKEN_FUNCTION;
  }
  if (type_is(ptype, "call_expression") && is_field_of(parent, "function", n)) {
    return IDE_TS_TOKEN_FUNCTION;
  }
  if ((type_is(ptype, "preproc_def") || type_is(ptype, "preproc_function_def")) &&
      is_field_of(parent, "name", n)) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (type_is(ptype, "enumerator") && is_field_of(parent, "name", n)) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (is_upper_snake(txt, len)) {
    return IDE_TS_TOKEN_MACRO;
  }
  return IDE_TS_TOKEN_VARIABLE;
}

static IdeTsToken
classify_identifier_java(TSNode n, const char *txt, gsize len)
{
  TSNode parent = ts_node_parent(n);
  const char *ptype = ts_node_is_null(parent) ? "" : ts_node_type(parent);

  static const char *const kTypeDecls[] = {
    "class_declaration", "interface_declaration", "enum_declaration",
    "record_declaration", "annotation_type_declaration", NULL
  };

  if ((type_is(ptype, "method_declaration") || type_is(ptype, "constructor_declaration")) &&
      is_field_of(parent, "name", n)) {
    return IDE_TS_TOKEN_FUNCTION;
  }
  if (type_is(ptype, "method_invocation")) {
    if (is_field_of(parent, "name", n)) {
      return IDE_TS_TOKEN_FUNCTION;
    }
    if (is_field_of(parent, "object", n)) {
      return is_pascal(txt, len) || is_upper_snake(txt, len) ? IDE_TS_TOKEN_TYPE
                                                              : IDE_TS_TOKEN_VARIABLE;
    }
  }
  if (type_in(ptype, kTypeDecls) && is_field_of(parent, "name", n)) {
    return IDE_TS_TOKEN_TYPE;
  }
  if (type_is(ptype, "enum_constant") && is_field_of(parent, "name", n)) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (type_is(ptype, "annotation") || type_is(ptype, "marker_annotation")) {
    if (is_field_of(parent, "name", n)) {
      return IDE_TS_TOKEN_MACRO;
    }
  }
  if (type_is(ptype, "scoped_identifier") || type_is(ptype, "scoped_type_identifier")) {
    TSNode gp = ts_node_parent(parent);
    while (!ts_node_is_null(gp) && (type_is(ts_node_type(gp), "scoped_identifier") ||
                                    type_is(ts_node_type(gp), "scoped_type_identifier"))) {
      gp = ts_node_parent(gp);
    }
    if (!ts_node_is_null(gp) && (type_is(ts_node_type(gp), "annotation") ||
                                 type_is(ts_node_type(gp), "marker_annotation"))) {
      return IDE_TS_TOKEN_MACRO;
    }
    return is_pascal(txt, len) ? IDE_TS_TOKEN_TYPE : IDE_TS_TOKEN_VARIABLE;
  }
  if (type_is(ptype, "field_access") && is_field_of(parent, "field", n)) {
    return is_upper_snake(txt, len) ? IDE_TS_TOKEN_MACRO : IDE_TS_TOKEN_VARIABLE;
  }
  if (is_upper_snake(txt, len)) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (is_pascal(txt, len)) {
    return IDE_TS_TOKEN_TYPE; /* Math, System, Collections in expression position */
  }
  return IDE_TS_TOKEN_VARIABLE;
}

static IdeTsToken
classify_identifier_go(TSNode n, const char *type, const char *txt, gsize len)
{
  TSNode parent = ts_node_parent(n);
  const char *ptype = ts_node_is_null(parent) ? "" : ts_node_type(parent);

  if (type_is(type, "field_identifier")) {
    if (type_is(ptype, "selector_expression")) {
      TSNode gp = ts_node_parent(parent);
      if (!ts_node_is_null(gp) && type_is(ts_node_type(gp), "call_expression") &&
          is_field_of(gp, "function", parent)) {
        return IDE_TS_TOKEN_FUNCTION;
      }
    }
    if (type_is(ptype, "method_elem") || type_is(ptype, "method_spec") ||
        type_is(ptype, "method_declaration")) {
      return IDE_TS_TOKEN_FUNCTION;
    }
    return IDE_TS_TOKEN_VARIABLE;
  }
  if (type_is(type, "package_identifier") || type_is(type, "label_name") ||
      type_is(type, "blank_identifier")) {
    return IDE_TS_TOKEN_VARIABLE;
  }

  /* plain identifier */
  if (type_is(ptype, "function_declaration") && is_field_of(parent, "name", n)) {
    return IDE_TS_TOKEN_FUNCTION;
  }
  if (type_is(ptype, "call_expression") && is_field_of(parent, "function", n)) {
    return IDE_TS_TOKEN_FUNCTION;
  }
  if (type_is(ptype, "const_spec")) {
    return IDE_TS_TOKEN_MACRO;
  }
  if (is_upper_snake(txt, len)) {
    return IDE_TS_TOKEN_MACRO;
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
      ctx->cb(st, i, IDE_TS_TOKEN_STRING, ctx->user_data);
    } else if (c == '/' && i + 1 < e && t[i + 1] == '*') {
      gsize st = i;
      i += 2;
      while (i + 1 < e && !(t[i] == '*' && t[i + 1] == '/')) {
        i++;
      }
      i = MIN(i + 2, e);
      ctx->cb(st, i, IDE_TS_TOKEN_COMMENT, ctx->user_data);
    } else if (c == '/' && i + 1 < e && t[i + 1] == '/') {
      ctx->cb(i, e, IDE_TS_TOKEN_COMMENT, ctx->user_data);
      return;
    } else if (g_ascii_isdigit(c)) {
      gsize st = i;
      while (i < e && (g_ascii_isalnum(t[i]) || t[i] == '.' || t[i] == '_')) {
        i++;
      }
      ctx->cb(st, i, IDE_TS_TOKEN_NUMBER, ctx->user_data);
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
      ctx->cb(st, i, tok, ctx->user_data);
    } else {
      gsize st = i;
      while (i < e && strchr("+-*/%&|^~!<>=?:", t[i]) != NULL) {
        i++;
      }
      if (i > st) {
        ctx->cb(st, i, IDE_TS_TOKEN_OPERATOR, ctx->user_data);
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

#define EMIT(tok) ctx->cb(s, e, (tok), ctx->user_data)

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
      "decimal_integer_literal", "hex_integer_literal", "octal_integer_literal",
      "binary_integer_literal", "decimal_floating_point_literal",
      "hex_floating_point_literal", NULL
    };
    if (type_in(type, kNumbers)) {
      EMIT(IDE_TS_TOKEN_NUMBER);
      return TRUE;
    }

    static const char *const kConstants[] = {
      "true", "false", "null", "nil", "iota", "null_literal", NULL
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
      "boolean_type", "void_type", NULL
    };
    if (type_in(type, kTypes)) {
      EMIT(IDE_TS_TOKEN_TYPE);
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
        switch (ctx->doc->lang) {
          case IDE_TS_LANG_C:
            tok = classify_identifier_c(n, type, txt, len);
            break;
          case IDE_TS_LANG_JAVA:
            tok = classify_identifier_java(n, txt, len);
            break;
          case IDE_TS_LANG_GO:
            tok = classify_identifier_go(n, type, txt, len);
            break;
          default:
            break;
        }
      }
      if (tok != IDE_TS_TOKEN_NONE) {
        EMIT(tok);
      }
      return TRUE;
    }
    return FALSE; /* structural node: descend */
  }

  /* anonymous token (keyword / operator / punctuation) */
  IdeTsToken tok = classify_anonymous(type, ts_node_parent(n));
  if (tok != IDE_TS_TOKEN_NONE) {
    EMIT(tok);
  }
  return TRUE;
#undef EMIT
}

void
ide_ts_doc_walk(const IdeTsDoc *doc,
                gsize           from,
                gsize           to,
                IdeTsTokenFunc  cb,
                gpointer        user_data)
{
  if (doc == NULL || doc->tree == NULL || cb == NULL || to <= from) {
    return;
  }

  WalkCtx ctx = { doc, from, to, cb, user_data };
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
    while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
      if (!ts_tree_cursor_goto_parent(&cursor)) {
        ts_tree_cursor_delete(&cursor);
        return;
      }
    }
  }
}

#endif /* HAVE_TREE_SITTER */
