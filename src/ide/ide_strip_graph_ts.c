#include "ide_strip_graph_private.h"

#include <string.h>

/* ═══════════════════════════ Lexical & Keyword Helpers ═══════════════════════════ */

gboolean
ide_strip_graph_is_ident_start(char c)
{
  return g_ascii_isalpha(c) || c == '_';
}

gboolean
ide_strip_graph_is_ident_char(char c)
{
  return g_ascii_isalnum(c) || c == '_';
}

static gboolean
str_in_list(const char *s, const char *const *list)
{
  if (s == NULL) {
    return FALSE;
  }
  for (gsize i = 0; list[i] != NULL; i++) {
    if (strcmp(s, list[i]) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_control_or_decl_keyword(const char *word)
{
  static const char *const kKw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "sizeof", "typeof", "alignof", "_Alignof",
    "offsetof", "typedef", "struct", "enum", "union", "class", "interface",
    "trait", "impl", "namespace", "extern", "static", "const", "volatile",
    "inline", "register", "restrict", "catch", "try", "finally", "throw",
    "throws", "synchronized", "match", "loop", "unsafe", "async", "await",
    "def", "elif", "with", "lambda", "func", "fn", "function", "type", "pub",
    "package", "import", "from", "export", "new", "delete", "select", "defer",
    "go", "where", "when", "guard", "assert", "yield", "defined", NULL
  };
  return str_in_list(word, kKw);
}

static gboolean
is_primitive_type_word(const char *word)
{
  static const char *const kPrim[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "bool", "_Bool", "auto", "wchar_t", "char16_t", "char32_t",
    "i8", "i16", "i32", "i64", "i128", "isize",
    "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64", "str",
    "byte", "rune", "string", "error", "any", "int8", "int16", "int32", "int64",
    "uint", "uint8", "uint16", "uint32", "uint64", "uintptr", "float32", "float64",
    "boolean", "number", "unknown", "never", "undefined", "null", "object", NULL
  };
  return str_in_list(word, kPrim);
}

/* Skips C/C++/Go/Rust/Java/JS comments and string/char literals at index `*pi`.
 * Returns TRUE if something was skipped. */
gboolean
ide_strip_graph_skip_comment_or_string_c(const char *t, gsize len, gsize *pi)
{
  gsize i = *pi;
  if (i >= len) {
    return FALSE;
  }
  char c = t[i];
  if (c == '/' && i + 1 < len && t[i + 1] == '/') {
    i += 2;
    while (i < len && t[i] != '\n') {
      i++;
    }
    *pi = i;
    return TRUE;
  }
  if (c == '/' && i + 1 < len && t[i + 1] == '*') {
    i += 2;
    while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) {
      i++;
    }
    *pi = MIN(i + 2, len);
    return TRUE;
  }
  if (c == '"' || c == '\'' || c == '`') {
    char q = c;
    i++;
    while (i < len && t[i] != q) {
      if (t[i] == '\\' && q != '`' && i + 1 < len) {
        i += 2;
      } else {
        i++;
      }
    }
    *pi = MIN(i + 1, len);
    return TRUE;
  }
  return FALSE;
}

/* Returns TRUE if a top-level reference to a function name at [ws..we) is a
 * C/C++ forward declaration prototype (`void foo(int x);`) rather than a call
 * or function-pointer reference. */
gboolean
ide_strip_graph_looks_like_c_prototype(const char *t, gsize len, gsize ws, gsize we)
{
  gsize b = ws;
  while (b > 0 && (t[b - 1] == ' ' || t[b - 1] == '\t' || t[b - 1] == '*')) {
    b--;
  }
  if (b == 0 || !ide_strip_graph_is_ident_char(t[b - 1])) {
    return FALSE;
  }
  gsize f = we;
  while (f < len && (t[f] == ' ' || t[f] == '\t')) {
    f++;
  }
  if (f >= len || t[f] != '(') {
    return FALSE;
  }
  int pdepth = 1;
  f++;
  while (f < len && pdepth > 0) {
    if (ide_strip_graph_skip_comment_or_string_c(t, len, &f)) {
      continue;
    }
    if (t[f] == '(') pdepth++;
    else if (t[f] == ')') pdepth--;
    f++;
  }
  if (pdepth != 0) {
    return FALSE;
  }
  gsize limit = MIN(len, f + 80);
  while (f < limit) {
    if (ide_strip_graph_skip_comment_or_string_c(t, limit, &f)) {
      continue;
    }
    if (t[f] == ';') {
      return TRUE;
    }
    if (t[f] == '{' || t[f] == '=') {
      return FALSE;
    }
    f++;
  }
  return FALSE;
}

/* ═══════════════════════════ Fast Definition Extractor ═══════════════════════════ */

static void
add_sym_def(GPtrArray *defs, GHashTable *seen, const char *file_path, const char *rel_path,
            const char *name, guint start_line, guint end_line, int kind)
{
  if (name == NULL || name[0] == '\0' || is_control_or_decl_keyword(name)) {
    return;
  }
  if (end_line < start_line) {
    end_line = start_line;
  }
  g_autofree char *key = g_strdup_printf("%s:%u:%d", name, start_line, kind);
  if (g_hash_table_contains(seen, key)) {
    return;
  }
  g_hash_table_add(seen, g_steal_pointer(&key));

  SymDef *d = g_new0(SymDef, 1);
  d->name = g_strdup(name);
  d->file_path = g_strdup(file_path);
  d->rel_path = g_strdup(rel_path);
  d->start_line = start_line;
  d->end_line = end_line;
  d->kind = kind;
  d->cached_child_count = -1;
  g_ptr_array_add(defs, d);
}

typedef struct {
  char *text;
  gsize byte;
} ScanTok;

static void
scan_tok_clear(ScanTok *toks, guint n)
{
  for (guint i = 0; i < n; i++) {
    g_free(toks[i].text);
  }
}

/* Tokenizes a declaration header slice [start..end) into identifiers and key
 * punctuation (`(`, `)`, `<`, `>`, `=`, `*`, `:`, `!`), skipping comments/strings. */
static GArray *
tokenize_header(const char *t, gsize start, gsize end)
{
  GArray *toks = g_array_new(FALSE, FALSE, sizeof(ScanTok));
  gsize i = start;
  while (i < end) {
    if (ide_strip_graph_skip_comment_or_string_c(t, end, &i)) {
      continue;
    }
    char c = t[i];
    if (g_ascii_isspace(c)) {
      i++;
      continue;
    }
    if (ide_strip_graph_is_ident_start(c)) {
      gsize s = i;
      while (i < end && ide_strip_graph_is_ident_char(t[i])) {
        i++;
      }
      ScanTok tk = { g_strndup(t + s, i - s), s };
      g_array_append_val(toks, tk);
    } else if (c == '=' && i + 1 < end && t[i + 1] == '>') {
      ScanTok tk = { g_strdup("=>"), i };
      g_array_append_val(toks, tk);
      i += 2;
    } else if (c == '-' && i + 1 < end && t[i + 1] == '>') {
      ScanTok tk = { g_strdup("->"), i };
      g_array_append_val(toks, tk);
      i += 2;
    } else if (c == ':' && i + 1 < end && t[i + 1] == ':') {
      ScanTok tk = { g_strdup("::"), i };
      g_array_append_val(toks, tk);
      i += 2;
    } else if (strchr("()<>=*:,![]", c) != NULL) {
      char buf[2] = { c, '\0' };
      ScanTok tk = { g_strdup(buf), i };
      g_array_append_val(toks, tk);
      i++;
    } else {
      i++;
    }
  }
  return toks;
}

static gboolean
tok_is(const GArray *toks, guint i, const char *s)
{
  return i < toks->len && strcmp(g_array_index(toks, ScanTok, i).text, s) == 0;
}

static gboolean
tok_is_ident(const GArray *toks, guint i)
{
  if (i >= toks->len) {
    return FALSE;
  }
  const char *s = g_array_index(toks, ScanTok, i).text;
  return s != NULL && ide_strip_graph_is_ident_start(s[0]);
}

/* Finds matching closing brace `}` starting from `{` at `open_pos`. */
static gsize
find_matching_brace(const char *t, gsize len, gsize open_pos)
{
  int depth = 0;
  gsize i = open_pos;
  while (i < len) {
    if (ide_strip_graph_skip_comment_or_string_c(t, len, &i)) {
      continue;
    }
    if (t[i] == '{') {
      depth++;
    } else if (t[i] == '}') {
      depth--;
      if (depth == 0) {
        return i;
      }
    }
    i++;
  }
  return len > 0 ? len - 1 : 0;
}

/* Analyzes a braced declaration header [hdr_start .. open_brace) and appends
 * discovered symbol(s) to `defs`. */
static void
classify_braced_decl(const char *t, gsize len, const LineIndex *li,
                     gsize hdr_start, gsize open_brace, gsize close_brace,
                     const char *file_path, const char *rel_path,
                     GPtrArray *defs, GHashTable *seen)
{
  GArray *toks = tokenize_header(t, hdr_start, open_brace);
  if (toks->len == 0) {
    g_array_free(toks, TRUE);
    return;
  }

  guint start_line = ide_strip_graph_line_index_line_for_byte(li, g_array_index(toks, ScanTok, 0).byte);
  guint end_line = ide_strip_graph_line_index_line_for_byte(li, close_brace);

  /* 1. Check for Rust `macro_rules! name { ... }` */
  if (toks->len >= 3 && tok_is(toks, 0, "macro_rules") && tok_is(toks, 1, "!") && tok_is_ident(toks, 2)) {
    add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, 2).text,
                start_line, end_line, IDE_STRIP_SYM_MACRO);
    goto out;
  }

  /* 2. Check for JS/TS arrow function: `const foo = [async] (...) => {` */
  gboolean has_arrow = FALSE;
  gboolean has_top_eq = FALSE;
  int pdepth = 0;
  int adepth = 0;
  for (guint i = 0; i < toks->len; i++) {
    if (tok_is(toks, i, "(")) pdepth++;
    else if (tok_is(toks, i, ")") && pdepth > 0) pdepth--;
    else if (tok_is(toks, i, "<")) adepth++;
    else if (tok_is(toks, i, ">") && adepth > 0) adepth--;
    else if (pdepth == 0 && adepth == 0) {
      if (tok_is(toks, i, "=>")) has_arrow = TRUE;
      if (tok_is(toks, i, "=")) has_top_eq = TRUE;
    }
  }
  if (has_arrow && has_top_eq) {
    for (guint i = 0; i + 1 < toks->len; i++) {
      if (tok_is(toks, i + 1, "=") && tok_is_ident(toks, i)) {
        add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, i).text,
                    start_line, end_line, IDE_STRIP_SYM_FUNC);
        break;
      }
    }
    goto out;
  }
  if (has_top_eq) {
    /* Variable/array/struct initializer (`static const T arr[] = { ... }`) */
    goto out;
  }

  /* 3. Check for type declarations: struct / enum / union / class / interface / trait / record / Go `type X struct` */
  gboolean is_typedef = tok_is(toks, 0, "typedef");
  gboolean is_go_type = tok_is(toks, 0, "type") && toks->len >= 3 && tok_is_ident(toks, 1);
  if (is_go_type && (tok_is(toks, 2, "struct") || tok_is(toks, 2, "interface"))) {
    add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, 1).text,
                start_line, end_line, IDE_STRIP_SYM_TYPE);
    goto out;
  }

  gint type_kw_idx = -1;
  gboolean has_paren_before_type_kw = FALSE;
  for (guint i = 0; i < toks->len; i++) {
    const char *s = g_array_index(toks, ScanTok, i).text;
    if (strcmp(s, "(") == 0) {
      has_paren_before_type_kw = TRUE;
      break;
    }
    if (strcmp(s, "struct") == 0 || strcmp(s, "enum") == 0 || strcmp(s, "union") == 0 ||
        strcmp(s, "class") == 0 || strcmp(s, "interface") == 0 ||
        strcmp(s, "trait") == 0 || strcmp(s, "record") == 0) {
      type_kw_idx = (gint)i;
      break;
    }
  }

  /* Distinguish `struct Foo *func(int x) {` (function returning struct!) from `struct Foo {` */
  gboolean has_top_paren = FALSE;
  for (guint i = 0; i < toks->len; i++) {
    if (tok_is(toks, i, "(")) {
      has_top_paren = TRUE;
      break;
    }
  }
  gboolean is_record_type = (type_kw_idx >= 0 &&
                             strcmp(g_array_index(toks, ScanTok, (guint)type_kw_idx).text, "record") == 0);

  if (type_kw_idx >= 0 && !has_paren_before_type_kw && (!has_top_paren || is_record_type)) {
    if ((guint)type_kw_idx + 1 < toks->len && tok_is_ident(toks, (guint)type_kw_idx + 1)) {
      const char *tname = g_array_index(toks, ScanTok, (guint)type_kw_idx + 1).text;
      add_sym_def(defs, seen, file_path, rel_path, tname, start_line, end_line, IDE_STRIP_SYM_TYPE);
    }
    if (is_typedef && close_brace + 1 < len) {
      /* Capture trailing typedef alias after `}`: `typedef struct { ... } Alias;` */
      gsize k = close_brace + 1;
      while (k < len && (g_ascii_isspace(t[k]) || t[k] == '*')) {
        k++;
      }
      if (k < len && ide_strip_graph_is_ident_start(t[k])) {
        gsize as = k;
        while (k < len && ide_strip_graph_is_ident_char(t[k])) {
          k++;
        }
        g_autofree char *alias = g_strndup(t + as, k - as);
        guint alias_end = ide_strip_graph_line_index_line_for_byte(li, k > 0 ? k - 1 : close_brace);
        add_sym_def(defs, seen, file_path, rel_path, alias, start_line, alias_end, IDE_STRIP_SYM_TYPE);
      }
    }
    goto out;
  }

  /* 4. Function / method definition: locate parameter list `(...)` and the identifier before it */
  if (has_top_paren) {
    gint chosen_name_idx = -1;
    pdepth = 0;
    adepth = 0;
    for (guint i = 0; i < toks->len; i++) {
      if (tok_is(toks, i, "<")) {
        adepth++;
        continue;
      }
      if (tok_is(toks, i, ">") && adepth > 0) {
        adepth--;
        continue;
      }
      if (tok_is(toks, i, "(")) {
        if (pdepth == 0 && adepth == 0 && i > 0) {
          gint k = (gint)i - 1;
          /* Skip generic params `<T>` immediately before `(` */
          if (k >= 0 && tok_is(toks, (guint)k, ">")) {
            int gdepth = 1;
            k--;
            while (k >= 0 && gdepth > 0) {
              if (tok_is(toks, (guint)k, ">")) gdepth++;
              else if (tok_is(toks, (guint)k, "<")) gdepth--;
              k--;
            }
          }
          if (k >= 0 && tok_is_ident(toks, (guint)k)) {
            const char *cand = g_array_index(toks, ScanTok, (guint)k).text;
            /* For Go `func (r *T) Name(...)`, `func` precedes the first `(` -> skip it! */
            if (!is_control_or_decl_keyword(cand)) {
              chosen_name_idx = k;
              break;
            }
          }
        }
        pdepth++;
      } else if (tok_is(toks, i, ")") && pdepth > 0) {
        pdepth--;
      }
    }

    if (chosen_name_idx >= 0) {
      const char *fname = g_array_index(toks, ScanTok, (guint)chosen_name_idx).text;
      add_sym_def(defs, seen, file_path, rel_path, fname, start_line, end_line, IDE_STRIP_SYM_FUNC);
    }
  }

out:
  scan_tok_clear((ScanTok *)toks->data, toks->len);
  g_array_free(toks, TRUE);
}

/* Extracts single-line / non-braced `typedef ... Alias;` and Go `type X Y` / Rust `type X = Y;` */
static void
classify_semicolon_decl(const char *t, const LineIndex *li,
                        gsize hdr_start, gsize semi_pos,
                        const char *file_path, const char *rel_path,
                        GPtrArray *defs, GHashTable *seen)
{
  GArray *toks = tokenize_header(t, hdr_start, semi_pos);
  if (toks->len < 3) {
    scan_tok_clear((ScanTok *)toks->data, toks->len);
    g_array_free(toks, TRUE);
    return;
  }
  guint sline = ide_strip_graph_line_index_line_for_byte(li, g_array_index(toks, ScanTok, 0).byte);
  guint eline = ide_strip_graph_line_index_line_for_byte(li, semi_pos);

  if (tok_is(toks, 0, "typedef")) {
    gboolean found = FALSE;
    for (guint i = 1; i + 2 < toks->len; i++) {
      if (tok_is(toks, i, "(") && tok_is(toks, i + 1, "*") && tok_is_ident(toks, i + 2)) {
        add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, i + 2).text,
                    sline, eline, IDE_STRIP_SYM_TYPE);
        found = TRUE;
        break;
      }
    }
    if (!found) {
      for (gint i = (gint)toks->len - 1; i >= 1; i--) {
        if (tok_is_ident(toks, (guint)i)) {
          add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, (guint)i).text,
                      sline, eline, IDE_STRIP_SYM_TYPE);
          break;
        }
      }
    }
  } else {
    for (guint i = 0; i + 2 < toks->len; i++) {
      if (tok_is(toks, i, "type") && tok_is_ident(toks, i + 1) && tok_is(toks, i + 2, "=")) {
        add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, i + 1).text,
                    sline, eline, IDE_STRIP_SYM_TYPE);
        break;
      }
    }
  }

  scan_tok_clear((ScanTok *)toks->data, toks->len);
  g_array_free(toks, TRUE);
}

static void
extract_defs_brace_lang(const char *t, gsize len, const LineIndex *li,
                        const char *file_path, const char *rel_path,
                        GPtrArray *defs, GHashTable *seen)
{
  int brace_depth = 0;
  gsize stmt_start = 0;
  gsize i = 0;

  while (i < len) {
    if (ide_strip_graph_skip_comment_or_string_c(t, len, &i)) {
      continue;
    }

    /* Preprocessor directive at line start (C/C++) */
    if (t[i] == '#' && (i == 0 || t[i - 1] == '\n')) {
      gsize dir_start = i;
      gsize p = i + 1;
      while (p < len && (t[p] == ' ' || t[p] == '\t')) {
        p++;
      }
      gboolean is_define = (p + 6 <= len && strncmp(t + p, "define", 6) == 0 &&
                            (p + 6 == len || g_ascii_isspace(t[p + 6])));
      char *macro_name = NULL;
      if (is_define) {
        p += 6;
        while (p < len && (t[p] == ' ' || t[p] == '\t')) {
          p++;
        }
        if (p < len && ide_strip_graph_is_ident_start(t[p])) {
          gsize ms = p;
          while (p < len && ide_strip_graph_is_ident_char(t[p])) {
            p++;
          }
          macro_name = g_strndup(t + ms, p - ms);
        }
      }
      while (i < len) {
        if (t[i] == '\n') {
          gboolean cont = (i > dir_start && t[i - 1] == '\\');
          if (!cont && i > dir_start + 1 && t[i - 1] == '\r' && t[i - 2] == '\\') {
            cont = TRUE;
          }
          i++;
          if (!cont) {
            break;
          }
        } else {
          i++;
        }
      }
      if (macro_name != NULL) {
        guint sline = ide_strip_graph_line_index_line_for_byte(li, dir_start);
        guint eline = ide_strip_graph_line_index_line_for_byte(li, i > 0 ? i - 1 : dir_start);
        add_sym_def(defs, seen, file_path, rel_path, macro_name, sline, eline, IDE_STRIP_SYM_MACRO);
        g_free(macro_name);
      }
      stmt_start = i;
      continue;
    }

    char c = t[i];
    if (c == '{') {
      if (brace_depth <= 1) {
        gsize close_b = find_matching_brace(t, len, i);
        classify_braced_decl(t, len, li, stmt_start, i, close_b, file_path, rel_path, defs, seen);
      }
      brace_depth++;
      stmt_start = i + 1;
      i++;
    } else if (c == '}') {
      if (brace_depth > 0) {
        brace_depth--;
      }
      stmt_start = i + 1;
      i++;
    } else if (c == ';') {
      if (brace_depth == 0) {
        classify_semicolon_decl(t, li, stmt_start, i, file_path, rel_path, defs, seen);
      }
      stmt_start = i + 1;
      i++;
    } else {
      i++;
    }
  }
}

static void
extract_defs_python(const char *t, gsize len, const LineIndex *li,
                    const char *file_path, const char *rel_path,
                    GPtrArray *defs, GHashTable *seen)
{
  (void)len;
  guint total = ide_strip_graph_line_index_count(li);
  for (guint ln = 1; ln <= total; ln++) {
    gsize s = ide_strip_graph_line_index_byte_for_line(li, ln);
    gsize e = ide_strip_graph_line_index_end_byte_for_line(li, ln);
    gsize p = s;
    guint indent = 0;
    while (p < e && (t[p] == ' ' || t[p] == '\t')) {
      indent += (t[p] == '\t') ? 4u : 1u;
      p++;
    }
    if (p >= e || t[p] == '#') {
      continue;
    }
    int kind = -1;
    if (p + 10 < e && strncmp(t + p, "async def ", 10) == 0) {
      p += 10;
      kind = IDE_STRIP_SYM_FUNC;
    } else if (p + 4 < e && strncmp(t + p, "def ", 4) == 0) {
      p += 4;
      kind = IDE_STRIP_SYM_FUNC;
    } else if (p + 6 < e && strncmp(t + p, "class ", 6) == 0) {
      p += 6;
      kind = IDE_STRIP_SYM_TYPE;
    }
    if (kind < 0) {
      continue;
    }
    while (p < e && g_ascii_isspace(t[p])) {
      p++;
    }
    if (p >= e || !ide_strip_graph_is_ident_start(t[p])) {
      continue;
    }
    gsize ns = p;
    while (p < e && ide_strip_graph_is_ident_char(t[p])) {
      p++;
    }
    g_autofree char *name = g_strndup(t + ns, p - ns);

    guint end_ln = ln;
    for (guint k = ln + 1; k <= total; k++) {
      gsize ks = ide_strip_graph_line_index_byte_for_line(li, k);
      gsize ke = ide_strip_graph_line_index_end_byte_for_line(li, k);
      gsize kp = ks;
      guint kindent = 0;
      while (kp < ke && (t[kp] == ' ' || t[kp] == '\t')) {
        kindent += (t[kp] == '\t') ? 4u : 1u;
        kp++;
      }
      if (kp >= ke || t[kp] == '\n' || t[kp] == '\r' || t[kp] == '#') {
        continue;
      }
      if (kindent <= indent) {
        break;
      }
      end_ln = k;
    }
    add_sym_def(defs, seen, file_path, rel_path, name, ln, end_ln, kind);
  }
}

static void
extract_defs_lua(const char *t, gsize len, const LineIndex *li,
                 const char *file_path, const char *rel_path,
                 GPtrArray *defs, GHashTable *seen)
{
  (void)len;
  guint total = ide_strip_graph_line_index_count(li);
  for (guint ln = 1; ln <= total; ln++) {
    gsize s = ide_strip_graph_line_index_byte_for_line(li, ln);
    gsize e = ide_strip_graph_line_index_end_byte_for_line(li, ln);
    gsize p = s;
    while (p < e && (t[p] == ' ' || t[p] == '\t')) {
      p++;
    }
    if (p + 6 < e && strncmp(t + p, "local ", 6) == 0) {
      p += 6;
      while (p < e && (t[p] == ' ' || t[p] == '\t')) {
        p++;
      }
    }
    if (p + 9 >= e || strncmp(t + p, "function ", 9) != 0) {
      continue;
    }
    p += 9;
    while (p < e && (t[p] == ' ' || t[p] == '\t')) {
      p++;
    }
    gsize ns = p;
    while (p < e && (ide_strip_graph_is_ident_char(t[p]) || t[p] == '.' || t[p] == ':')) {
      p++;
    }
    if (p == ns) {
      continue;
    }
    gsize tail = p;
    while (tail > ns && t[tail - 1] != '.' && t[tail - 1] != ':') {
      tail--;
    }
    g_autofree char *name = g_strndup(t + tail, p - tail);
    guint end_ln = MIN(total, ln + 20);
    for (guint k = ln + 1; k <= total; k++) {
      gsize ks = ide_strip_graph_line_index_byte_for_line(li, k);
      gsize ke = ide_strip_graph_line_index_end_byte_for_line(li, k);
      if (ke - ks >= 3 && strncmp(t + ks, "end", 3) == 0 &&
          (ks + 3 == ke || g_ascii_isspace(t[ks + 3]))) {
        end_ln = k;
        break;
      }
    }
    add_sym_def(defs, seen, file_path, rel_path, name, ln, end_ln, IDE_STRIP_SYM_FUNC);
  }
}

static void
extract_defs_asm(const char *t, gsize len, const LineIndex *li,
                 const char *file_path, const char *rel_path,
                 GPtrArray *defs, GHashTable *seen)
{
  (void)len;
  guint total = ide_strip_graph_line_index_count(li);
  SymDef *prev = NULL;
  for (guint ln = 1; ln <= total; ln++) {
    gsize s = ide_strip_graph_line_index_byte_for_line(li, ln);
    gsize e = ide_strip_graph_line_index_end_byte_for_line(li, ln);
    if (s >= e || !ide_strip_graph_is_ident_start(t[s])) {
      continue;
    }
    gsize p = s;
    while (p < e && ide_strip_graph_is_ident_char(t[p])) {
      p++;
    }
    if (p < e && t[p] == ':') {
      if (prev != NULL && ln > prev->start_line) {
        prev->end_line = ln - 1;
      }
      g_autofree char *name = g_strndup(t + s, p - s);
      guint before = defs->len;
      add_sym_def(defs, seen, file_path, rel_path, name, ln, total, IDE_STRIP_SYM_FUNC);
      if (defs->len > before) {
        prev = g_ptr_array_index(defs, defs->len - 1);
      }
    }
  }
}

GPtrArray *
ide_strip_graph_extract_file_defs(const char *file_path, const char *rel_path,
                                  const char *text, gsize len, const LineIndex *li)
{
  GPtrArray *defs = g_ptr_array_new_with_free_func(ide_strip_graph_sym_def_free);
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (text == NULL || len == 0) {
    return defs;
  }

  if (file_path != NULL && g_str_has_suffix(file_path, ".py")) {
    extract_defs_python(text, len, li, file_path, rel_path, defs, seen);
  } else if (file_path != NULL && g_str_has_suffix(file_path, ".lua")) {
    extract_defs_lua(text, len, li, file_path, rel_path, defs, seen);
  } else if (file_path != NULL && (g_str_has_suffix(file_path, ".s") ||
                                   g_str_has_suffix(file_path, ".S") ||
                                   g_str_has_suffix(file_path, ".asm"))) {
    extract_defs_asm(text, len, li, file_path, rel_path, defs, seen);
  } else {
    extract_defs_brace_lang(text, len, li, file_path, rel_path, defs, seen);
  }
  return defs;
}

/* ═══════════════════════════ Enclosing Symbol Finder ═══════════════════════════ */

typedef struct {
  const char *text;
  IdeTsToken  want;
  char       *found;
} FirstTokenCtx;

static void
on_first_token(gsize s, gsize e, IdeTsToken tok, gpointer user_data)
{
  FirstTokenCtx *ctx = user_data;
  if (ctx->found != NULL || tok != ctx->want || e <= s) {
    return;
  }
  g_autofree char *word = g_strndup(ctx->text + s, e - s);
  if (!is_control_or_decl_keyword(word)) {
    ctx->found = g_steal_pointer(&word);
  }
}

static char *
extract_name_after_brace_typedef(const char *text, gsize len, gsize close_end)
{
  gsize k = close_end;
  while (k < len && (g_ascii_isspace(text[k]) || text[k] == '*')) {
    k++;
  }
  if (k < len && ide_strip_graph_is_ident_start(text[k])) {
    gsize s = k;
    while (k < len && ide_strip_graph_is_ident_char(text[k])) {
      k++;
    }
    g_autofree char *cand = g_strndup(text + s, k - s);
    if (!is_control_or_decl_keyword(cand)) {
      return g_steal_pointer(&cand);
    }
  }
  return NULL;
}

static gboolean
find_enclosing_in_defs(const GPtrArray *defs, guint cursor_line,
                       char **out_name, guint *out_start, guint *out_end, int *out_kind)
{
  const SymDef *best = NULL;
  guint best_span = G_MAXUINT;
  for (guint i = 0; defs != NULL && i < defs->len; i++) {
    const SymDef *d = g_ptr_array_index(defs, i);
    if (cursor_line >= d->start_line && cursor_line <= d->end_line) {
      guint span = d->end_line - d->start_line;
      if (best == NULL || span < best_span) {
        best = d;
        best_span = span;
      }
    }
  }
  if (best == NULL) {
    return FALSE;
  }
  if (out_name) *out_name = g_strdup(best->name);
  if (out_start) *out_start = best->start_line;
  if (out_end) *out_end = best->end_line;
  if (out_kind) *out_kind = best->kind;
  return TRUE;
}

gboolean
ide_strip_graph_find_enclosing_symbol(IdeStripGraph *g,
                                      const char    *file_path,
                                      const char    *text,
                                      guint          cursor_line_1based,
                                      char         **out_name,
                                      guint         *out_start_line,
                                      guint         *out_end_line,
                                      int           *out_kind)
{
  if (out_name) *out_name = NULL;
  if (out_start_line) *out_start_line = 1;
  if (out_end_line) *out_end_line = 1;
  if (out_kind) *out_kind = IDE_STRIP_SYM_FILE;

  g_autofree char *loaded = NULL;
  gsize len = 0;
  if (text != NULL) {
    len = strlen(text);
  } else if (file_path != NULL && g_file_get_contents(file_path, &loaded, &len, NULL)) {
    text = loaded;
  } else {
    return FALSE;
  }

  LineIndex li;
  ide_strip_graph_line_index_init(&li, text, len);
  guint total_lines = ide_strip_graph_line_index_count(&li);
  guint clamped_line = CLAMP(cursor_line_1based == 0 ? 1 : cursor_line_1based, 1, total_lines);

  /* Point cursor_byte at the first non-whitespace byte of clamped_line so
   * tree-sitter lands directly inside declarations/statements on that line. */
  gsize line_s = ide_strip_graph_line_index_byte_for_line(&li, clamped_line);
  gsize line_e = ide_strip_graph_line_index_end_byte_for_line(&li, clamped_line);
  gsize cursor_byte = line_s;
  while (cursor_byte < line_e && (text[cursor_byte] == ' ' || text[cursor_byte] == '\t')) {
    cursor_byte++;
  }
  if (cursor_byte >= line_e) {
    cursor_byte = line_s;
  }

  /* 1. Tree-sitter primary path */
  IdeTsLang lang = ide_ts_lang_for_path(file_path);
  if (lang != IDE_TS_LANG_NONE) {
    IdeTsDoc *doc = ide_ts_doc_new(lang);
    if (doc != NULL && ide_ts_doc_update(doc, text, len, NULL, NULL)) {
      gsize obj_s = 0, obj_e = 0;
      int kind = IDE_STRIP_SYM_FUNC;
      gboolean hit = ide_ts_doc_find_object(doc, cursor_byte, FALSE, 'f', &obj_s, &obj_e);
      if (!hit) {
        hit = ide_ts_doc_find_object(doc, cursor_byte, FALSE, 'c', &obj_s, &obj_e);
        kind = IDE_STRIP_SYM_TYPE;
      }
      if (hit && obj_e > obj_s) {
        gsize body_s = obj_e, body_e = obj_e;
        ide_ts_doc_find_object(doc, cursor_byte, TRUE,
                               kind == IDE_STRIP_SYM_FUNC ? 'f' : 'c',
                               &body_s, &body_e);
        gsize header_end = (body_s > obj_s && body_s <= obj_e) ? body_s : obj_e;
        FirstTokenCtx fctx = {
          text,
          kind == IDE_STRIP_SYM_FUNC ? IDE_TS_TOKEN_FUNCTION : IDE_TS_TOKEN_TYPE,
          NULL
        };
        ide_ts_doc_walk(doc, obj_s, header_end, on_first_token, &fctx);

        /* Anonymous `typedef struct { ... } Alias;` fallback */
        if (fctx.found == NULL && kind == IDE_STRIP_SYM_TYPE) {
          fctx.found = extract_name_after_brace_typedef(text, len, obj_e);
        }
        if (fctx.found != NULL) {
          if (out_name) *out_name = g_steal_pointer(&fctx.found);
          if (out_start_line) *out_start_line = ide_strip_graph_line_index_line_for_byte(&li, obj_s);
          if (out_end_line) *out_end_line = ide_strip_graph_line_index_line_for_byte(&li, obj_e > 0 ? obj_e - 1 : 0);
          if (out_kind) *out_kind = kind;
          ide_ts_doc_free(doc);
          ide_strip_graph_line_index_clear(&li);
          return TRUE;
        }
      }
    }
    if (doc != NULL) {
      ide_ts_doc_free(doc);
    }
  }

  /* 2. Fast heuristic / indexed definitions fallback */
  g_autofree char *rel = file_path ? g_path_get_basename(file_path) : g_strdup("<file>");
  g_autoptr(GPtrArray) defs = ide_strip_graph_extract_file_defs(file_path, rel, text, len, &li);
  if (find_enclosing_in_defs(defs, clamped_line, out_name, out_start_line, out_end_line, out_kind)) {
    ide_strip_graph_line_index_clear(&li);
    return TRUE;
  }

  /* 3. Outside any symbol -> full-file fallback [1..line_count] */
  (void)g;
  if (out_name) *out_name = g_strdup(rel);
  if (out_start_line) *out_start_line = 1;
  if (out_end_line) *out_end_line = total_lines;
  if (out_kind) *out_kind = IDE_STRIP_SYM_FILE;
  ide_strip_graph_line_index_clear(&li);
  return TRUE;
}

/* ═══════════════════════════ Lazy Child Count & Target Extraction ═══════════════════════════ */

/* Fast lexical call/type counter inside [def->start_line .. def->end_line] for lazy child_count. */
guint
ide_strip_graph_compute_child_count(IdeStripGraph *g, SymDef *def, GHashTable *file_cache,
                                    const char *override_path, const char *override_text,
                                    gsize override_len)
{
  if (def == NULL) {
    return 0;
  }
  if (def->cached_child_count >= 0) {
    return (guint)def->cached_child_count;
  }
  const CachedFileText *cf = ide_strip_graph_get_cached_text(file_cache, def->file_path,
                                                             override_path, override_text, override_len);
  if (cf == NULL || cf->text == NULL || cf->len == 0) {
    def->cached_child_count = 0;
    return 0;
  }
  gsize s = ide_strip_graph_line_index_byte_for_line(&cf->lines, def->start_line);
  gsize e = ide_strip_graph_line_index_end_byte_for_line(&cf->lines, def->end_line);
  e = MIN(e, cf->len);

  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean want_type = (def->kind == IDE_STRIP_SYM_TYPE);
  gsize i = s;
  while (i < e) {
    if (ide_strip_graph_skip_comment_or_string_c(cf->text, e, &i)) {
      continue;
    }
    if (ide_strip_graph_is_ident_start(cf->text[i])) {
      gsize is = i;
      while (i < e && ide_strip_graph_is_ident_char(cf->text[i])) {
        i++;
      }
      gsize ilen = i - is;
      if (ilen < 2 || ilen > 128) {
        continue;
      }
      if (!want_type) {
        gsize k = i;
        while (k < e && (cf->text[k] == ' ' || cf->text[k] == '\t')) {
          k++;
        }
        if (k >= e || cf->text[k] != '(') {
          continue;
        }
      }
      g_autofree char *word = g_strndup(cf->text + is, ilen);
      if (strcmp(word, def->name) == 0 || is_control_or_decl_keyword(word) ||
          (want_type && is_primitive_type_word(word))) {
        continue;
      }
      if (g_hash_table_contains(seen, word)) {
        continue;
      }
      SymDef *target = ide_strip_graph_resolve_symbol(g, word, def->file_path, want_type);
      if (target != NULL && target != def) {
        g_hash_table_add(seen, g_steal_pointer(&word));
      }
    } else {
      i++;
    }
  }

  def->cached_child_count = (gint)g_hash_table_size(seen);
  return (guint)def->cached_child_count;
}

typedef struct {
  char *name;
  guint line_1based;
  gsize start_byte;
  gsize end_byte;
} RawTokHit;

static void
raw_tok_hit_free(gpointer data)
{
  RawTokHit *h = data;
  if (h != NULL) {
    g_free(h->name);
    g_free(h);
  }
}

typedef struct {
  const char      *text;
  const LineIndex *li;
  IdeTsToken       want;
  GPtrArray       *hits; /* RawTokHit* */
} TsCollectCtx;

static void
on_ts_collect_token(gsize s, gsize e, IdeTsToken tok, gpointer user_data)
{
  TsCollectCtx *ctx = user_data;
  if (tok != ctx->want || e <= s) {
    return;
  }
  RawTokHit *h = g_new0(RawTokHit, 1);
  h->name = g_strndup(ctx->text + s, e - s);
  h->line_1based = ide_strip_graph_line_index_line_for_byte(ctx->li, s);
  h->start_byte = s;
  h->end_byte = e;
  g_ptr_array_add(ctx->hits, h);
}

/* Checks whether a function token at [start_byte..end_byte) is actually a
 * function definition header (`name(...) {`) rather than a call expression. */
static gboolean
looks_like_func_definition_site(const char *t, gsize len, gsize end_byte)
{
  gsize i = end_byte;
  while (i < len && g_ascii_isspace(t[i])) {
    i++;
  }
  if (i < len && t[i] == '<') {
    int adepth = 1;
    i++;
    while (i < len && adepth > 0) {
      if (t[i] == '<') adepth++;
      else if (t[i] == '>') adepth--;
      i++;
    }
    while (i < len && g_ascii_isspace(t[i])) {
      i++;
    }
  }
  if (i >= len || t[i] != '(') {
    return FALSE;
  }
  int pdepth = 1;
  i++;
  while (i < len && pdepth > 0) {
    if (ide_strip_graph_skip_comment_or_string_c(t, len, &i)) {
      continue;
    }
    if (t[i] == '(') pdepth++;
    else if (t[i] == ')') pdepth--;
    i++;
  }
  if (pdepth != 0) {
    return FALSE;
  }
  gsize limit = MIN(len, i + 120);
  while (i < limit) {
    if (ide_strip_graph_skip_comment_or_string_c(t, limit, &i)) {
      continue;
    }
    if (t[i] == '{') {
      return TRUE;
    }
    if (t[i] == ';' || t[i] == ',' || t[i] == ')' || t[i] == '=') {
      return FALSE;
    }
    i++;
  }
  return FALSE;
}

static void
collect_fallback_calls(const char *t, gsize len, const LineIndex *li,
                       gsize range_s, gsize range_e, GPtrArray *hits)
{
  gsize i = range_s;
  while (i < range_e) {
    if (ide_strip_graph_skip_comment_or_string_c(t, range_e, &i)) {
      continue;
    }
    if (t[i] == '#') {
      while (i < range_e && t[i] != '\n') {
        i++;
      }
      continue;
    }
    if (ide_strip_graph_is_ident_start(t[i])) {
      gsize s = i;
      while (i < range_e && ide_strip_graph_is_ident_char(t[i])) {
        i++;
      }
      gsize k = i;
      while (k < len && (t[k] == ' ' || t[k] == '\t')) {
        k++;
      }
      if (k < len && t[k] == '(') {
        RawTokHit *h = g_new0(RawTokHit, 1);
        h->name = g_strndup(t + s, i - s);
        h->line_1based = ide_strip_graph_line_index_line_for_byte(li, s);
        h->start_byte = s;
        h->end_byte = i;
        g_ptr_array_add(hits, h);
      }
    } else {
      i++;
    }
  }
}

static void
collect_fallback_types(const char *t, gsize len, const LineIndex *li,
                       gsize range_s, gsize range_e, GPtrArray *hits)
{
  (void)len;
  gsize i = range_s;
  while (i < range_e) {
    if (ide_strip_graph_skip_comment_or_string_c(t, range_e, &i)) {
      continue;
    }
    if (ide_strip_graph_is_ident_start(t[i])) {
      gsize s = i;
      while (i < range_e && ide_strip_graph_is_ident_char(t[i])) {
        i++;
      }
      gsize ilen = i - s;
      gboolean is_pascal = (ilen >= 2 && t[s] >= 'A' && t[s] <= 'Z' && t[s + 1] >= 'a' && t[s + 1] <= 'z');
      gboolean is_suffix_t = (ilen > 2 && t[i - 2] == '_' && t[i - 1] == 't');
      if (is_pascal || is_suffix_t) {
        RawTokHit *h = g_new0(RawTokHit, 1);
        h->name = g_strndup(t + s, ilen);
        h->line_1based = ide_strip_graph_line_index_line_for_byte(li, s);
        h->start_byte = s;
        h->end_byte = i;
        g_ptr_array_add(hits, h);
      }
    } else {
      i++;
    }
  }
}

static IdeStripGraphEntry *
entry_from_sym_def(IdeStripGraph *g, SymDef *def, guint call_line, gboolean is_fallback,
                   GHashTable *file_cache, const char *override_path,
                   const char *override_text, gsize override_len)
{
  IdeStripGraphEntry *e = g_new0(IdeStripGraphEntry, 1);
  e->name = g_strdup(def->name);
  e->file_path = g_strdup(def->file_path);
  e->rel_path = g_strdup(def->rel_path);
  e->start_line = def->start_line;
  e->end_line = def->end_line;
  e->call_line = call_line;
  e->call_count = 1;
  e->kind = def->kind;
  e->is_fallback = is_fallback;

  const CachedFileText *cf = ide_strip_graph_get_cached_text(file_cache, def->file_path,
                                                             override_path, override_text, override_len);
  if (cf != NULL) {
    e->peek_code = ide_strip_graph_slice_peek(cf->text, cf->len, &cf->lines,
                                              def->start_line, STRIP_DEFAULT_PEEK);
  } else {
    e->peek_code = ide_strip_graph_read_peek(def->file_path, def->start_line,
                                             STRIP_DEFAULT_PEEK, NULL);
  }
  e->child_count = ide_strip_graph_compute_child_count(g, def, file_cache,
                                                       override_path, override_text, override_len);
  return e;
}

GPtrArray *
ide_strip_graph_extract_targets(IdeStripGraph *g,
                                const char    *file_path,
                                const char    *text,
                                guint          start_line,
                                guint          end_line,
                                int            lens_mode,
                                guint         *out_ext_suppressed)
{
  if (out_ext_suppressed != NULL) {
    *out_ext_suppressed = 0;
  }
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
  if (g == NULL) {
    return out;
  }
  ide_strip_graph_ensure_indexed(g);

  g_autofree char *canon_file = file_path ? ide_strip_graph_canonicalize_dir(file_path) : NULL;
  g_autofree char *loaded = NULL;
  gsize len = 0;
  if (text != NULL) {
    len = strlen(text);
  } else if (canon_file != NULL && g_file_get_contents(canon_file, &loaded, &len, NULL)) {
    text = loaded;
  } else {
    return out;
  }

  g_autoptr(GHashTable) file_cache =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ide_strip_graph_cached_file_text_free);

  if (lens_mode == IDE_STRIP_GRAPH_LENS_IMPACT) {
    g_ptr_array_unref(out);
    return ide_strip_graph_extract_impact_fallback(g, canon_file, text, len, start_line, file_cache);
  }
  if (lens_mode == IDE_STRIP_GRAPH_LENS_DIFF) {
    return out;
  }

  LineIndex li;
  ide_strip_graph_line_index_init(&li, text, len);
  guint total = ide_strip_graph_line_index_count(&li);
  guint s_line = (start_line == 0) ? 1 : MIN(start_line, total);
  guint e_line = (end_line == 0 || end_line > total) ? total : MAX(end_line, s_line);

  gsize range_s = ide_strip_graph_line_index_byte_for_line(&li, s_line);
  gsize range_e = ide_strip_graph_line_index_end_byte_for_line(&li, e_line);

  g_autofree char *enclosing_name = NULL;
  guint enc_s = 0, enc_e = 0;
  int enc_kind = IDE_STRIP_SYM_FILE;
  if (s_line > 1 || e_line < total) {
    ide_strip_graph_find_enclosing_symbol(g, canon_file ? canon_file : file_path,
                                          text, s_line, &enclosing_name, &enc_s, &enc_e, &enc_kind);
    if (enc_kind == IDE_STRIP_SYM_FILE) {
      g_clear_pointer(&enclosing_name, g_free);
    }
  }

  gboolean want_types = (lens_mode == IDE_STRIP_GRAPH_LENS_TYPES);
  g_autoptr(GPtrArray) hits = g_ptr_array_new_with_free_func(raw_tok_hit_free);

  IdeTsLang lang = ide_ts_lang_for_path(file_path);
  gboolean used_ts = FALSE;
  if (lang != IDE_TS_LANG_NONE) {
    IdeTsDoc *doc = ide_ts_doc_new(lang);
    if (doc != NULL && ide_ts_doc_update(doc, text, len, NULL, NULL)) {
      TsCollectCtx cctx = {
        text, &li,
        want_types ? IDE_TS_TOKEN_TYPE : IDE_TS_TOKEN_FUNCTION,
        hits
      };
      ide_ts_doc_walk(doc, range_s, range_e, on_ts_collect_token, &cctx);
      used_ts = TRUE;
    }
    if (doc != NULL) {
      ide_ts_doc_free(doc);
    }
  }
  if (!used_ts) {
    if (want_types) {
      collect_fallback_types(text, len, &li, range_s, range_e, hits);
    } else {
      collect_fallback_calls(text, len, &li, range_s, range_e, hits);
    }
  }

  g_autoptr(GHashTable) dedup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint ext_suppressed = 0;

  for (guint i = 0; i < hits->len; i++) {
    const RawTokHit *h = g_ptr_array_index(hits, i);
    if (h->name == NULL || h->name[0] == '\0' || is_control_or_decl_keyword(h->name)) {
      continue;
    }
    if (want_types && is_primitive_type_word(h->name)) {
      continue;
    }
    if (enclosing_name != NULL && strcmp(h->name, enclosing_name) == 0) {
      continue; /* skip self-call / own enclosing name */
    }
    if (!want_types && looks_like_func_definition_site(text, len, h->end_byte)) {
      continue; /* skip function definition headers inside scope */
    }

    SymDef *def = ide_strip_graph_resolve_symbol(g, h->name, canon_file, want_types);
    if (def == NULL) {
      ext_suppressed++;
      continue;
    }

    /* Skip referencing a type's own definition line */
    if (want_types && canon_file != NULL && strcmp(def->file_path, canon_file) == 0 &&
        h->line_1based >= def->start_line && h->line_1based <= def->end_line) {
      continue;
    }

    g_autofree char *dkey = g_strdup_printf("%s:%s:%u", def->file_path, def->name, def->start_line);
    IdeStripGraphEntry *existing = g_hash_table_lookup(dedup, dkey);
    if (existing != NULL) {
      existing->call_count++;
      continue;
    }

    IdeStripGraphEntry *entry = entry_from_sym_def(g, def, h->line_1based, TRUE,
                                                   file_cache, canon_file, text, len);
    g_ptr_array_add(out, entry);
    g_hash_table_insert(dedup, g_steal_pointer(&dkey), entry);
  }

  ide_strip_graph_line_index_clear(&li);
  if (out_ext_suppressed != NULL) {
    *out_ext_suppressed = ext_suppressed;
  }
  return out;
}
