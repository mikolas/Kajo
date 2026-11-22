#include "ide_strip_graph.h"
#include "ide_ts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRIP_MAX_FILES       2500u
#define STRIP_MAX_FILE_BYTES  (512u * 1024u)
#define STRIP_DEFAULT_PEEK    5u

/* ═══════════════════════════ Internal Types ═══════════════════════════ */

typedef struct {
  char  *name;
  char  *file_path;          /* canonical absolute path */
  char  *rel_path;           /* relative to workspace_root */
  guint  start_line;         /* 1-based */
  guint  end_line;           /* 1-based */
  int    kind;               /* IdeStripSymKind */
  gint   cached_child_count; /* -1 = uncomputed */
} SymDef;

typedef struct {
  char      *file_path;
  char      *rel_path;
  GPtrArray *defs;           /* owned SymDef* */
  guint      line_count;
} FileEntry;

struct _IdeStripGraph {
  char         *workspace_root; /* canonical, no trailing slash (except "/") */
  gboolean      indexed;
  GHashTable   *files;          /* canonical path -> FileEntry* */
  GHashTable   *by_name;        /* char* name -> GPtrArray<SymDef*> (borrowed) */
  GCancellable *alive;          /* cancelled on free to guard async LSP cbs */
};

typedef struct {
  GArray *starts; /* gsize byte offset of start of each line (0-indexed) */
  gsize   len;
} LineIndex;

typedef struct {
  char      *text;
  gsize      len;
  LineIndex  lines;
} CachedFileText;

/* ═══════════════════════════ Entry Lifecycle ═══════════════════════════ */

void
ide_strip_graph_entry_free(gpointer data)
{
  IdeStripGraphEntry *e = data;
  if (e == NULL) {
    return;
  }
  g_free(e->name);
  g_free(e->file_path);
  g_free(e->rel_path);
  g_free(e->peek_code);
  g_free(e);
}

static void
sym_def_free(gpointer data)
{
  SymDef *d = data;
  if (d == NULL) {
    return;
  }
  g_free(d->name);
  g_free(d->file_path);
  g_free(d->rel_path);
  g_free(d);
}

static void
file_entry_free(gpointer data)
{
  FileEntry *fe = data;
  if (fe == NULL) {
    return;
  }
  g_free(fe->file_path);
  g_free(fe->rel_path);
  g_clear_pointer(&fe->defs, g_ptr_array_unref);
  g_free(fe);
}

static void
ptr_array_unref_cb(gpointer data)
{
  if (data != NULL) {
    g_ptr_array_unref(data);
  }
}

/* ═══════════════════════════ Path & Workspace Filter ═══════════════════════════ */

static char *
canonicalize_dir_clean(const char *path)
{
  if (path == NULL || path[0] == '\0') {
    return NULL;
  }
  char *canon = g_canonicalize_filename(path, NULL);
  if (canon == NULL) {
    return NULL;
  }
  gsize n = strlen(canon);
  while (n > 1 && canon[n - 1] == '/') {
    canon[--n] = '\0';
  }
  return canon;
}

static gboolean
is_excluded_dir_name(const char *seg, gsize len)
{
  static const char *const kExcluded[] = {
    ".git", "builddir", "build", "node_modules", "target", "subprojects", NULL
  };
  for (gsize i = 0; kExcluded[i] != NULL; i++) {
    if (strlen(kExcluded[i]) == len && strncmp(seg, kExcluded[i], len) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
rel_has_excluded_segment(const char *rel)
{
  if (rel == NULL || rel[0] == '\0') {
    return FALSE;
  }
  const char *p = rel;
  while (*p != '\0') {
    while (*p == '/') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    const char *seg = p;
    while (*p != '\0' && *p != '/') {
      p++;
    }
    if (is_excluded_dir_name(seg, (gsize)(p - seg))) {
      return TRUE;
    }
  }
  return FALSE;
}

gboolean
ide_strip_path_in_workspace(const char *path, const char *workspace_root)
{
  if (path == NULL || path[0] == '\0' ||
      workspace_root == NULL || workspace_root[0] == '\0') {
    return FALSE;
  }

  g_autofree char *decoded = NULL;
  const char *raw_path = path;
  if (g_str_has_prefix(path, "file://")) {
    decoded = g_filename_from_uri(path, NULL, NULL);
    if (decoded == NULL || decoded[0] == '\0') {
      return FALSE;
    }
    raw_path = decoded;
  } else if (strstr(path, "://") != NULL ||
             g_str_has_prefix(path, "jdt:") ||
             g_str_has_prefix(path, "jar:")) {
    return FALSE;
  }

  g_autofree char *canon_root = canonicalize_dir_clean(workspace_root);
  if (canon_root == NULL) {
    return FALSE;
  }

  g_autofree char *canon_path = g_canonicalize_filename(raw_path, canon_root);
  if (canon_path == NULL) {
    return FALSE;
  }
  gsize plen = strlen(canon_path);
  while (plen > 1 && canon_path[plen - 1] == '/') {
    canon_path[--plen] = '\0';
  }

  gsize rlen = strlen(canon_root);
  const char *rel = NULL;
  if (rlen == 1 && canon_root[0] == '/') {
    if (canon_path[0] != '/') {
      return FALSE;
    }
    rel = canon_path + 1;
  } else {
    if (plen < rlen || strncmp(canon_path, canon_root, rlen) != 0) {
      return FALSE;
    }
    if (canon_path[rlen] != '\0' && canon_path[rlen] != '/') {
      return FALSE;
    }
    rel = canon_path + rlen;
  }

  if (rel_has_excluded_segment(rel)) {
    return FALSE;
  }
  return TRUE;
}

static char *
make_rel_path(const char *canon_path, const char *canon_root)
{
  if (canon_path == NULL) {
    return g_strdup("");
  }
  if (canon_root == NULL || canon_root[0] == '\0') {
    return g_path_get_basename(canon_path);
  }
  gsize rlen = strlen(canon_root);
  if (rlen == 1 && canon_root[0] == '/') {
    return g_strdup(canon_path[0] == '/' ? canon_path + 1 : canon_path);
  }
  if (strncmp(canon_path, canon_root, rlen) == 0 && canon_path[rlen] == '/') {
    return g_strdup(canon_path + rlen + 1);
  }
  return g_path_get_basename(canon_path);
}

static gboolean
is_supported_source_file(const char *name)
{
  if (name == NULL) {
    return FALSE;
  }
  static const char *const kExts[] = {
    ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh",
    ".go", ".rs", ".java", ".js", ".jsx", ".mjs",
    ".ts", ".tsx", ".py", ".lua", ".s", ".S", ".asm", NULL
  };
  for (gsize i = 0; kExts[i] != NULL; i++) {
    if (g_str_has_suffix(name, kExts[i])) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_header_file(const char *path)
{
  if (path == NULL) {
    return FALSE;
  }
  return g_str_has_suffix(path, ".h") || g_str_has_suffix(path, ".hpp") ||
         g_str_has_suffix(path, ".hh");
}

/* ═══════════════════════════ Line Index Helpers ═══════════════════════════ */

static void
line_index_init(LineIndex *li, const char *text, gsize len)
{
  li->starts = g_array_new(FALSE, FALSE, sizeof(gsize));
  li->len = len;
  gsize zero = 0;
  g_array_append_val(li->starts, zero);
  for (gsize i = 0; i < len; i++) {
    if (text[i] == '\n' && i + 1 <= len) {
      gsize next = i + 1;
      if (next < len) {
        g_array_append_val(li->starts, next);
      }
    }
  }
}

static void
line_index_clear(LineIndex *li)
{
  g_clear_pointer(&li->starts, g_array_unref);
  li->len = 0;
}

static guint
line_index_count(const LineIndex *li)
{
  return (li != NULL && li->starts != NULL && li->starts->len > 0) ? li->starts->len : 1u;
}

static guint
line_index_line_for_byte(const LineIndex *li, gsize byte)
{
  if (li == NULL || li->starts == NULL || li->starts->len == 0) {
    return 1;
  }
  guint lo = 0;
  guint hi = li->starts->len - 1;
  while (lo < hi) {
    guint mid = lo + (hi - lo + 1) / 2;
    gsize off = g_array_index(li->starts, gsize, mid);
    if (off <= byte) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo + 1;
}

static gsize
line_index_byte_for_line(const LineIndex *li, guint line_1based)
{
  if (li == NULL || li->starts == NULL || li->starts->len == 0) {
    return 0;
  }
  if (line_1based <= 1) {
    return 0;
  }
  guint idx = MIN(line_1based - 1, li->starts->len - 1);
  return g_array_index(li->starts, gsize, idx);
}

static gsize
line_index_end_byte_for_line(const LineIndex *li, guint line_1based)
{
  if (li == NULL || li->starts == NULL || li->starts->len == 0) {
    return 0;
  }
  if (line_1based == 0 || line_1based >= li->starts->len) {
    return li->len;
  }
  return g_array_index(li->starts, gsize, line_1based);
}

static void
cached_file_text_free(gpointer data)
{
  CachedFileText *cf = data;
  if (cf == NULL) {
    return;
  }
  line_index_clear(&cf->lines);
  g_free(cf->text);
  g_free(cf);
}

static const CachedFileText *
get_cached_file_text(GHashTable *cache, const char *file_path,
                     const char *override_path, const char *override_text, gsize override_len)
{
  if (file_path == NULL) {
    return NULL;
  }
  CachedFileText *cf = g_hash_table_lookup(cache, file_path);
  if (cf != NULL) {
    return cf;
  }
  cf = g_new0(CachedFileText, 1);
  if (override_path != NULL && override_text != NULL && strcmp(file_path, override_path) == 0) {
    cf->text = g_strndup(override_text, override_len);
    cf->len = override_len;
  } else {
    gsize len = 0;
    char *contents = NULL;
    if (!g_file_get_contents(file_path, &contents, &len, NULL)) {
      g_free(cf);
      return NULL;
    }
    cf->text = contents;
    cf->len = len;
  }
  line_index_init(&cf->lines, cf->text, cf->len);
  g_hash_table_insert(cache, g_strdup(file_path), cf);
  return cf;
}

/* ═══════════════════════════ Peek Reader ═══════════════════════════ */

static char *
slice_peek_from_memory(const char *text, gsize len, const LineIndex *li,
                       guint start_line, guint max_lines)
{
  if (text == NULL || max_lines == 0) {
    return g_strdup("");
  }
  if (start_line == 0) {
    start_line = 1;
  }
  guint total = line_index_count(li);
  if (start_line > total) {
    return g_strdup("");
  }
  guint end_line = MIN(total, start_line + max_lines - 1);
  gsize s = line_index_byte_for_line(li, start_line);
  gsize e = line_index_end_byte_for_line(li, end_line);
  if (e < s || s > len) {
    return g_strdup("");
  }
  e = MIN(e, len);

  GString *out = g_string_new(NULL);
  for (gsize i = s; i < e; i++) {
    if (text[i] != '\r') {
      g_string_append_c(out, text[i]);
    }
  }
  /* Trim trailing blank/whitespace-only lines and trailing newline */
  while (out->len > 0) {
    if (out->str[out->len - 1] == '\n') {
      g_string_truncate(out, out->len - 1);
      continue;
    }
    /* Check if the last line is whitespace-only */
    gsize line_start = out->len;
    while (line_start > 0 && out->str[line_start - 1] != '\n') {
      line_start--;
    }
    gboolean blank = TRUE;
    for (gsize k = line_start; k < out->len; k++) {
      if (out->str[k] != ' ' && out->str[k] != '\t') {
        blank = FALSE;
        break;
      }
    }
    if (blank) {
      g_string_truncate(out, line_start);
    } else {
      break;
    }
  }
  return g_string_free(out, FALSE);
}

char *
ide_strip_graph_read_peek(const char *file_path,
                          guint       start_line,
                          guint       max_lines,
                          const char *override_text)
{
  if (max_lines == 0) {
    return g_strdup("");
  }
  if (start_line == 0) {
    start_line = 1;
  }

  if (override_text != NULL) {
    LineIndex li;
    gsize len = strlen(override_text);
    line_index_init(&li, override_text, len);
    char *res = slice_peek_from_memory(override_text, len, &li, start_line, max_lines);
    line_index_clear(&li);
    return res;
  }

  if (file_path == NULL || file_path[0] == '\0') {
    return NULL;
  }
  FILE *fp = fopen(file_path, "r");
  if (fp == NULL) {
    return NULL;
  }

  GString *out = g_string_new(NULL);
  guint cur_line = 1;
  guint end_line = start_line + max_lines - 1;
  char chunk[512];
  while (fgets(chunk, sizeof(chunk), fp) != NULL) {
    gsize n = strlen(chunk);
    gboolean has_nl = (n > 0 && chunk[n - 1] == '\n');
    if (cur_line >= start_line && cur_line <= end_line) {
      for (gsize i = 0; i < n; i++) {
        if (chunk[i] != '\r') {
          g_string_append_c(out, chunk[i]);
        }
      }
    }
    if (has_nl) {
      if (cur_line >= end_line) {
        break;
      }
      cur_line++;
    }
  }
  fclose(fp);

  /* Trim trailing blank lines & trailing newline */
  while (out->len > 0) {
    if (out->str[out->len - 1] == '\n') {
      g_string_truncate(out, out->len - 1);
      continue;
    }
    gsize line_start = out->len;
    while (line_start > 0 && out->str[line_start - 1] != '\n') {
      line_start--;
    }
    gboolean blank = TRUE;
    for (gsize k = line_start; k < out->len; k++) {
      if (out->str[k] != ' ' && out->str[k] != '\t') {
        blank = FALSE;
        break;
      }
    }
    if (blank) {
      g_string_truncate(out, line_start);
    } else {
      break;
    }
  }
  return g_string_free(out, FALSE);
}

/* ═══════════════════════════ Lexical & Keyword Helpers ═══════════════════════════ */

static gboolean
is_ident_start(char c)
{
  return g_ascii_isalpha(c) || c == '_';
}

static gboolean
is_ident_char(char c)
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
static gboolean
skip_comment_or_string_c(const char *t, gsize len, gsize *pi)
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
    if (skip_comment_or_string_c(t, end, &i)) {
      continue;
    }
    char c = t[i];
    if (g_ascii_isspace(c)) {
      i++;
      continue;
    }
    if (is_ident_start(c)) {
      gsize s = i;
      while (i < end && is_ident_char(t[i])) {
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
  return s != NULL && is_ident_start(s[0]);
}

/* Finds matching closing brace `}` starting from `{` at `open_pos`. */
static gsize
find_matching_brace(const char *t, gsize len, gsize open_pos)
{
  int depth = 0;
  gsize i = open_pos;
  while (i < len) {
    if (skip_comment_or_string_c(t, len, &i)) {
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

  guint start_line = line_index_line_for_byte(li, g_array_index(toks, ScanTok, 0).byte);
  guint end_line = line_index_line_for_byte(li, close_brace);

  /* 1. Check for Rust `macro_rules! name { ... }` */
  if (toks->len >= 3 && tok_is(toks, 0, "macro_rules") && tok_is(toks, 1, "!") && tok_is_ident(toks, 2)) {
    add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, 2).text,
                start_line, end_line, IDE_STRIP_SYM_MACRO);
    scan_tok_clear((ScanTok *)toks->data, toks->len);
    g_array_free(toks, TRUE);
    return;
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
    scan_tok_clear((ScanTok *)toks->data, toks->len);
    g_array_free(toks, TRUE);
    return;
  }
  if (has_top_eq) {
    /* Variable/array/struct initializer (`static const T arr[] = { ... }`) */
    scan_tok_clear((ScanTok *)toks->data, toks->len);
    g_array_free(toks, TRUE);
    return;
  }

  /* 3. Check for type declarations: struct / enum / union / class / interface / trait / record / Go `type X struct` */
  gboolean is_typedef = tok_is(toks, 0, "typedef");
  gboolean is_go_type = tok_is(toks, 0, "type") && toks->len >= 3 && tok_is_ident(toks, 1);
  if (is_go_type && (tok_is(toks, 2, "struct") || tok_is(toks, 2, "interface"))) {
    add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, 1).text,
                start_line, end_line, IDE_STRIP_SYM_TYPE);
    scan_tok_clear((ScanTok *)toks->data, toks->len);
    g_array_free(toks, TRUE);
    return;
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
      if (k < len && is_ident_start(t[k])) {
        gsize as = k;
        while (k < len && is_ident_char(t[k])) {
          k++;
        }
        g_autofree char *alias = g_strndup(t + as, k - as);
        guint alias_end = line_index_line_for_byte(li, k > 0 ? k - 1 : close_brace);
        add_sym_def(defs, seen, file_path, rel_path, alias, start_line, alias_end, IDE_STRIP_SYM_TYPE);
      }
    }
    scan_tok_clear((ScanTok *)toks->data, toks->len);
    g_array_free(toks, TRUE);
    return;
  }

  /* 4. Function / method definition: locate parameter list `(...)` and the identifier before it */
  if (has_top_paren) {
    /* Find the parameter list `(`:
     * - Skip Go method receiver `func (r *T) Name(...)` -> pick the `(` right after Name.
     * - Skip trailing macro attributes like `G_GNUC_PRINTF(2, 3)` by preferring the first
     *   top-level `(` that is preceded by an identifier (after skipping `<...>` generics). */
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
  guint sline = line_index_line_for_byte(li, g_array_index(toks, ScanTok, 0).byte);
  guint eline = line_index_line_for_byte(li, semi_pos);

  if (tok_is(toks, 0, "typedef")) {
    /* Function pointer typedef: `typedef ret (*Name)(...)` */
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
      /* Normal typedef: last identifier before `;` or `[` */
      for (gint i = (gint)toks->len - 1; i >= 1; i--) {
        if (tok_is_ident(toks, (guint)i)) {
          add_sym_def(defs, seen, file_path, rel_path, g_array_index(toks, ScanTok, (guint)i).text,
                      sline, eline, IDE_STRIP_SYM_TYPE);
          break;
        }
      }
    }
  } else {
    /* Rust/TS `type Name = ...;` */
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
    if (skip_comment_or_string_c(t, len, &i)) {
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
        if (p < len && is_ident_start(t[p])) {
          gsize ms = p;
          while (p < len && is_ident_char(t[p])) {
            p++;
          }
          macro_name = g_strndup(t + ms, p - ms);
        }
      }
      /* Advance to end of preprocessor directive (respecting backslash continuation) */
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
        guint sline = line_index_line_for_byte(li, dir_start);
        guint eline = line_index_line_for_byte(li, i > 0 ? i - 1 : dir_start);
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
  guint total = line_index_count(li);
  for (guint ln = 1; ln <= total; ln++) {
    gsize s = line_index_byte_for_line(li, ln);
    gsize e = line_index_end_byte_for_line(li, ln);
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
    if (p >= e || !is_ident_start(t[p])) {
      continue;
    }
    gsize ns = p;
    while (p < e && is_ident_char(t[p])) {
      p++;
    }
    g_autofree char *name = g_strndup(t + ns, p - ns);

    /* Find end line by indentation */
    guint end_ln = ln;
    for (guint k = ln + 1; k <= total; k++) {
      gsize ks = line_index_byte_for_line(li, k);
      gsize ke = line_index_end_byte_for_line(li, k);
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
  guint total = line_index_count(li);
  for (guint ln = 1; ln <= total; ln++) {
    gsize s = line_index_byte_for_line(li, ln);
    gsize e = line_index_end_byte_for_line(li, ln);
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
    while (p < e && (is_ident_char(t[p]) || t[p] == '.' || t[p] == ':')) {
      p++;
    }
    if (p == ns) {
      continue;
    }
    /* Take tail identifier after `.` or `:` */
    gsize tail = p;
    while (tail > ns && t[tail - 1] != '.' && t[tail - 1] != ':') {
      tail--;
    }
    g_autofree char *name = g_strndup(t + tail, p - tail);
    guint end_ln = MIN(total, ln + 20);
    for (guint k = ln + 1; k <= total; k++) {
      gsize ks = line_index_byte_for_line(li, k);
      gsize ke = line_index_end_byte_for_line(li, k);
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
  guint total = line_index_count(li);
  SymDef *prev = NULL;
  for (guint ln = 1; ln <= total; ln++) {
    gsize s = line_index_byte_for_line(li, ln);
    gsize e = line_index_end_byte_for_line(li, ln);
    if (s >= e || !is_ident_start(t[s])) {
      continue;
    }
    gsize p = s;
    while (p < e && is_ident_char(t[p])) {
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

static GPtrArray *
extract_file_defs(const char *file_path, const char *rel_path,
                  const char *text, gsize len, const LineIndex *li)
{
  GPtrArray *defs = g_ptr_array_new_with_free_func(sym_def_free);
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

/* ═══════════════════════════ Workspace Index Lifecycle ═══════════════════════════ */

static void
clear_index_tables(IdeStripGraph *g)
{
  g_hash_table_remove_all(g->by_name);
  g_hash_table_remove_all(g->files);
  g->indexed = FALSE;
}

static void
reset_cached_child_counts(IdeStripGraph *g)
{
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, g->files);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    FileEntry *fe = v;
    for (guint i = 0; i < fe->defs->len; i++) {
      SymDef *d = g_ptr_array_index(fe->defs, i);
      d->cached_child_count = -1;
    }
  }
}

static void
index_single_file(IdeStripGraph *g, const char *canon_path)
{
  if (g == NULL || canon_path == NULL || !ide_strip_path_in_workspace(canon_path, g->workspace_root)) {
    return;
  }
  if (!is_supported_source_file(canon_path)) {
    return;
  }

  g_autoptr(GFile) gf = g_file_new_for_path(canon_path);
  g_autoptr(GFileInfo) info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                                    G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info == NULL || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
    return;
  }
  if ((guint64)g_file_info_get_size(info) > STRIP_MAX_FILE_BYTES) {
    return;
  }

  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(canon_path, &contents, &len, NULL)) {
    return;
  }

  LineIndex li;
  line_index_init(&li, contents, len);

  FileEntry *fe = g_new0(FileEntry, 1);
  fe->file_path = g_strdup(canon_path);
  fe->rel_path = make_rel_path(canon_path, g->workspace_root);
  fe->line_count = line_index_count(&li);
  fe->defs = extract_file_defs(fe->file_path, fe->rel_path, contents, len, &li);
  line_index_clear(&li);

  for (guint i = 0; i < fe->defs->len; i++) {
    SymDef *d = g_ptr_array_index(fe->defs, i);
    GPtrArray *bucket = g_hash_table_lookup(g->by_name, d->name);
    if (bucket == NULL) {
      bucket = g_ptr_array_new();
      g_hash_table_insert(g->by_name, g_strdup(d->name), bucket);
    }
    g_ptr_array_add(bucket, d);
  }
  g_hash_table_insert(g->files, g_strdup(fe->file_path), fe);
}

static void
scan_dir_recursive(IdeStripGraph *g, const char *dir_path, guint *file_budget)
{
  if (*file_budget >= STRIP_MAX_FILES) {
    return;
  }
  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (dir == NULL) {
    return;
  }
  const char *name = NULL;
  while (*file_budget < STRIP_MAX_FILES && (name = g_dir_read_name(dir)) != NULL) {
    if (name[0] == '.') {
      continue; /* skip hidden files & directories (.git, .cache, ...) */
    }
    if (is_excluded_dir_name(name, strlen(name))) {
      continue;
    }
    g_autofree char *full = g_build_filename(dir_path, name, NULL);
    if (g_file_test(full, G_FILE_TEST_IS_SYMLINK)) {
      continue; /* avoid symlink loops */
    }
    if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
      scan_dir_recursive(g, full, file_budget);
    } else if (g_file_test(full, G_FILE_TEST_IS_REGULAR) && is_supported_source_file(name)) {
      g_autofree char *canon = canonicalize_dir_clean(full);
      if (canon != NULL && !g_hash_table_contains(g->files, canon)) {
        index_single_file(g, canon);
        (*file_budget)++;
      }
    }
  }
  g_dir_close(dir);
}

IdeStripGraph *
ide_strip_graph_new(const char *workspace_root)
{
  IdeStripGraph *g = g_new0(IdeStripGraph, 1);
  g->workspace_root = canonicalize_dir_clean(workspace_root ? workspace_root : ".");
  g->files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, file_entry_free);
  g->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ptr_array_unref_cb);
  g->alive = g_cancellable_new();
  return g;
}

void
ide_strip_graph_free(IdeStripGraph *g)
{
  if (g == NULL) {
    return;
  }
  if (g->alive != NULL) {
    g_cancellable_cancel(g->alive);
    g_clear_object(&g->alive);
  }
  g_clear_pointer(&g->by_name, g_hash_table_destroy);
  g_clear_pointer(&g->files, g_hash_table_destroy);
  g_free(g->workspace_root);
  g_free(g);
}

void
ide_strip_graph_set_workspace_root(IdeStripGraph *g, const char *workspace_root)
{
  g_return_if_fail(g != NULL);
  char *next = canonicalize_dir_clean(workspace_root ? workspace_root : ".");
  if (g_strcmp0(g->workspace_root, next) == 0) {
    g_free(next);
    return;
  }
  g_free(g->workspace_root);
  g->workspace_root = next;
  clear_index_tables(g);
}

const char *
ide_strip_graph_get_workspace_root(const IdeStripGraph *g)
{
  return g != NULL ? g->workspace_root : NULL;
}

void
ide_strip_graph_ensure_indexed(IdeStripGraph *g)
{
  if (g == NULL || g->indexed || g->workspace_root == NULL) {
    return;
  }
  guint budget = 0;
  scan_dir_recursive(g, g->workspace_root, &budget);
  g->indexed = TRUE;
}

void
ide_strip_graph_invalidate_file(IdeStripGraph *g, const char *file_path)
{
  if (g == NULL || file_path == NULL || !g->indexed) {
    return;
  }
  g_autofree char *canon = canonicalize_dir_clean(file_path);
  if (canon == NULL) {
    return;
  }

  FileEntry *old_fe = g_hash_table_lookup(g->files, canon);
  if (old_fe != NULL) {
    for (guint i = 0; i < old_fe->defs->len; i++) {
      SymDef *d = g_ptr_array_index(old_fe->defs, i);
      GPtrArray *bucket = g_hash_table_lookup(g->by_name, d->name);
      if (bucket != NULL) {
        g_ptr_array_remove_fast(bucket, d);
        if (bucket->len == 0) {
          g_hash_table_remove(g->by_name, d->name);
        }
      }
    }
    g_hash_table_remove(g->files, canon);
  }

  if (g_file_test(canon, G_FILE_TEST_IS_REGULAR)) {
    index_single_file(g, canon);
  }
  reset_cached_child_counts(g);
}

guint
ide_strip_graph_file_count(const IdeStripGraph *g)
{
  return (g != NULL && g->files != NULL) ? g_hash_table_size(g->files) : 0u;
}

guint
ide_strip_graph_symbol_count(const IdeStripGraph *g)
{
  if (g == NULL || g->files == NULL) {
    return 0u;
  }
  guint total = 0;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, g->files);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    FileEntry *fe = v;
    total += fe->defs->len;
  }
  return total;
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
  if (k < len && is_ident_start(text[k])) {
    gsize s = k;
    while (k < len && is_ident_char(text[k])) {
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
  line_index_init(&li, text, len);
  guint total_lines = line_index_count(&li);
  guint clamped_line = CLAMP(cursor_line_1based == 0 ? 1 : cursor_line_1based, 1, total_lines);

  /* Point cursor_byte at the first non-whitespace byte of clamped_line so
   * tree-sitter lands directly inside declarations/statements on that line. */
  gsize line_s = line_index_byte_for_line(&li, clamped_line);
  gsize line_e = line_index_end_byte_for_line(&li, clamped_line);
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
          if (out_start_line) *out_start_line = line_index_line_for_byte(&li, obj_s);
          if (out_end_line) *out_end_line = line_index_line_for_byte(&li, obj_e > 0 ? obj_e - 1 : 0);
          if (out_kind) *out_kind = kind;
          ide_ts_doc_free(doc);
          line_index_clear(&li);
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
  g_autoptr(GPtrArray) defs = extract_file_defs(file_path, rel, text, len, &li);
  if (find_enclosing_in_defs(defs, clamped_line, out_name, out_start_line, out_end_line, out_kind)) {
    line_index_clear(&li);
    return TRUE;
  }

  /* 3. Outside any symbol -> full-file fallback [1..line_count] */
  (void)g;
  if (out_name) *out_name = g_strdup(rel);
  if (out_start_line) *out_start_line = 1;
  if (out_end_line) *out_end_line = total_lines;
  if (out_kind) *out_kind = IDE_STRIP_SYM_FILE;
  line_index_clear(&li);
  return TRUE;
}

/* ═══════════════════════════ Symbol Resolution & Child Count ═══════════════════════════ */

static SymDef *
resolve_workspace_symbol(IdeStripGraph *g, const char *name, const char *prefer_file,
                         gboolean want_type)
{
  if (g == NULL || name == NULL || name[0] == '\0') {
    return NULL;
  }
  GPtrArray *bucket = g_hash_table_lookup(g->by_name, name);
  if (bucket == NULL || bucket->len == 0) {
    return NULL;
  }

  SymDef *best = NULL;
  int best_score = -1;
  for (guint i = 0; i < bucket->len; i++) {
    SymDef *d = g_ptr_array_index(bucket, i);
    if (!ide_strip_path_in_workspace(d->file_path, g->workspace_root)) {
      continue;
    }
    int score = 0;
    if (want_type) {
      if (d->kind != IDE_STRIP_SYM_TYPE) {
        continue;
      }
      score += 100;
    } else {
      if (d->kind == IDE_STRIP_SYM_FUNC) score += 100;
      else if (d->kind == IDE_STRIP_SYM_MACRO) score += 60;
      else if (d->kind == IDE_STRIP_SYM_TYPE) score += 20;
      else continue;
    }
    if (prefer_file != NULL && strcmp(d->file_path, prefer_file) == 0) {
      score += 40;
    }
    if (!is_header_file(d->file_path)) {
      score += 10;
    }
    if (score > best_score) {
      best_score = score;
      best = d;
    }
  }
  return best;
}

/* Fast lexical call/type counter inside [def->start_line .. def->end_line] for lazy child_count. */
static guint
compute_child_count(IdeStripGraph *g, SymDef *def, GHashTable *file_cache,
                    const char *override_path, const char *override_text, gsize override_len)
{
  if (def == NULL) {
    return 0;
  }
  if (def->cached_child_count >= 0) {
    return (guint)def->cached_child_count;
  }
  const CachedFileText *cf = get_cached_file_text(file_cache, def->file_path,
                                                  override_path, override_text, override_len);
  if (cf == NULL || cf->text == NULL || cf->len == 0) {
    def->cached_child_count = 0;
    return 0;
  }
  gsize s = line_index_byte_for_line(&cf->lines, def->start_line);
  gsize e = line_index_end_byte_for_line(&cf->lines, def->end_line);
  e = MIN(e, cf->len);

  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean want_type = (def->kind == IDE_STRIP_SYM_TYPE);
  gsize i = s;
  while (i < e) {
    if (skip_comment_or_string_c(cf->text, e, &i)) {
      continue;
    }
    if (is_ident_start(cf->text[i])) {
      gsize is = i;
      while (i < e && is_ident_char(cf->text[i])) {
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
      SymDef *target = resolve_workspace_symbol(g, word, def->file_path, want_type);
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

/* ═══════════════════════════ Source-Order Target Extraction ═══════════════════════════ */

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
  h->line_1based = line_index_line_for_byte(ctx->li, s);
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
  /* Skip optional generic params `<...>` */
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
    if (skip_comment_or_string_c(t, len, &i)) {
      continue;
    }
    if (t[i] == '(') pdepth++;
    else if (t[i] == ')') pdepth--;
    i++;
  }
  if (pdepth != 0) {
    return FALSE;
  }
  /* Allow trailing qualifiers (`const`, `-> Ret`, `: Ret`, `throws X`) before `{` */
  gsize limit = MIN(len, i + 120);
  while (i < limit) {
    if (skip_comment_or_string_c(t, limit, &i)) {
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
    if (skip_comment_or_string_c(t, range_e, &i)) {
      continue;
    }
    if (t[i] == '#') {
      /* Skip Python comments or C preprocessor lines */
      while (i < range_e && t[i] != '\n') {
        i++;
      }
      continue;
    }
    if (is_ident_start(t[i])) {
      gsize s = i;
      while (i < range_e && is_ident_char(t[i])) {
        i++;
      }
      gsize k = i;
      while (k < len && (t[k] == ' ' || t[k] == '\t')) {
        k++;
      }
      if (k < len && t[k] == '(') {
        RawTokHit *h = g_new0(RawTokHit, 1);
        h->name = g_strndup(t + s, i - s);
        h->line_1based = line_index_line_for_byte(li, s);
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
    if (skip_comment_or_string_c(t, range_e, &i)) {
      continue;
    }
    if (is_ident_start(t[i])) {
      gsize s = i;
      while (i < range_e && is_ident_char(t[i])) {
        i++;
      }
      gsize ilen = i - s;
      /* Capture PascalCase types or `_t` typedefs */
      gboolean is_pascal = (ilen >= 2 && t[s] >= 'A' && t[s] <= 'Z' && t[s + 1] >= 'a' && t[s + 1] <= 'z');
      gboolean is_suffix_t = (ilen > 2 && t[i - 2] == '_' && t[i - 1] == 't');
      if (is_pascal || is_suffix_t) {
        RawTokHit *h = g_new0(RawTokHit, 1);
        h->name = g_strndup(t + s, ilen);
        h->line_1based = line_index_line_for_byte(li, s);
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

  const CachedFileText *cf = get_cached_file_text(file_cache, def->file_path,
                                                  override_path, override_text, override_len);
  if (cf != NULL) {
    e->peek_code = slice_peek_from_memory(cf->text, cf->len, &cf->lines,
                                          def->start_line, STRIP_DEFAULT_PEEK);
  } else {
    e->peek_code = ide_strip_graph_read_peek(def->file_path, def->start_line,
                                             STRIP_DEFAULT_PEEK, NULL);
  }
  e->child_count = compute_child_count(g, def, file_cache, override_path, override_text, override_len);
  return e;
}

static gint
impact_entry_cmp(gconstpointer a, gconstpointer b)
{
  const IdeStripGraphEntry *x = *(const IdeStripGraphEntry *const *)a;
  const IdeStripGraphEntry *y = *(const IdeStripGraphEntry *const *)b;
  gint rc = g_strcmp0(x->rel_path, y->rel_path);
  if (rc != 0) {
    return rc;
  }
  if (x->call_line != y->call_line) {
    return x->call_line < y->call_line ? -1 : 1;
  }
  return g_strcmp0(x->name, y->name);
}

/* Returns TRUE if a top-level reference to a function name at [ws..we) is a
 * C/C++ forward declaration prototype (`void foo(int x);`) rather than a call
 * or function-pointer reference. */
static gboolean
looks_like_c_prototype_site(const char *t, gsize len, gsize ws, gsize we)
{
  /* Check backward on the same line (skipping whitespace & '*') for a return-type token */
  gsize b = ws;
  while (b > 0 && (t[b - 1] == ' ' || t[b - 1] == '\t' || t[b - 1] == '*')) {
    b--;
  }
  if (b == 0 || !is_ident_char(t[b - 1])) {
    return FALSE;
  }
  /* Check forward for `(...)` followed by `;` */
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
    if (skip_comment_or_string_c(t, len, &f)) {
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
    if (skip_comment_or_string_c(t, limit, &f)) {
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

static GPtrArray *
extract_impact_fallback(IdeStripGraph *g, const char *canon_file, const char *text, gsize len,
                        guint start_line, GHashTable *file_cache)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
  g_autofree char *target_name = NULL;
  guint sym_s = 1, sym_e = 1;
  int sym_kind = IDE_STRIP_SYM_FILE;
  if (!ide_strip_graph_find_enclosing_symbol(g, canon_file, text, start_line,
                                             &target_name, &sym_s, &sym_e, &sym_kind) ||
      target_name == NULL || sym_kind == IDE_STRIP_SYM_FILE) {
    return out;
  }

  gsize tlen = strlen(target_name);
  g_autoptr(GHashTable) dedup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, g->files);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    FileEntry *fe = v;
    const CachedFileText *cf = get_cached_file_text(file_cache, fe->file_path,
                                                    canon_file, text, len);
    if (cf == NULL || cf->text == NULL || cf->len < tlen) {
      continue;
    }
    gboolean same_file = (canon_file != NULL && strcmp(fe->file_path, canon_file) == 0);
    gsize i = 0;
    while (i < cf->len) {
      if (skip_comment_or_string_c(cf->text, cf->len, &i)) {
        continue;
      }
      if (is_ident_start(cf->text[i])) {
        gsize ws = i;
        while (i < cf->len && is_ident_char(cf->text[i])) {
          i++;
        }
        if (i - ws != tlen || strncmp(cf->text + ws, target_name, tlen) != 0) {
          continue;
        }
        guint ref_line = line_index_line_for_byte(&cf->lines, ws);
        if (same_file && ref_line >= sym_s && ref_line <= sym_e) {
          continue; /* skip self-definition / recursive self-call */
        }

        /* Map ref_line to enclosing caller symbol in fe->defs */
        const SymDef *caller = NULL;
        guint best_span = G_MAXUINT;
        for (guint d = 0; d < fe->defs->len; d++) {
          const SymDef *cand = g_ptr_array_index(fe->defs, d);
          if (ref_line >= cand->start_line && ref_line <= cand->end_line) {
            guint sp = cand->end_line - cand->start_line;
            if (caller == NULL || sp < best_span) {
              caller = cand;
              best_span = sp;
            }
          }
        }
        if (caller == NULL && sym_kind == IDE_STRIP_SYM_FUNC &&
            looks_like_c_prototype_site(cf->text, cf->len, ws, i)) {
          continue; /* skip forward declaration prototypes */
        }

        const char *cname = caller ? caller->name : fe->rel_path;
        guint cs = caller ? caller->start_line : 1;
        guint ce = caller ? caller->end_line : fe->line_count;
        int ckind = caller ? caller->kind : IDE_STRIP_SYM_FILE;
        g_autofree char *dkey = g_strdup_printf("%s:%s:%u", fe->file_path, cname, cs);
        IdeStripGraphEntry *existing = g_hash_table_lookup(dedup, dkey);
        if (existing != NULL) {
          existing->call_count++;
          continue;
        }

        IdeStripGraphEntry *e = g_new0(IdeStripGraphEntry, 1);
        e->name = g_strdup(cname);
        e->file_path = g_strdup(fe->file_path);
        e->rel_path = g_strdup(fe->rel_path);
        e->start_line = cs;
        e->end_line = ce;
        e->call_line = ref_line;
        e->call_count = 1;
        e->kind = ckind;
        e->peek_code = slice_peek_from_memory(cf->text, cf->len, &cf->lines,
                                              ref_line, STRIP_DEFAULT_PEEK);
        e->child_count = caller
                             ? compute_child_count(g, (SymDef *)caller, file_cache,
                                                   canon_file, text, len)
                             : 0;
        e->is_fallback = TRUE;
        g_ptr_array_add(out, e);
        g_hash_table_insert(dedup, g_steal_pointer(&dkey), e);
      } else {
        i++;
      }
    }
  }

  g_ptr_array_sort(out, impact_entry_cmp);
  return out;
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

  g_autofree char *canon_file = file_path ? canonicalize_dir_clean(file_path) : NULL;
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
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cached_file_text_free);

  if (lens_mode == IDE_STRIP_GRAPH_LENS_IMPACT) {
    g_ptr_array_unref(out);
    return extract_impact_fallback(g, canon_file, text, len, start_line, file_cache);
  }
  if (lens_mode == IDE_STRIP_GRAPH_LENS_DIFF) {
    return out;
  }

  LineIndex li;
  line_index_init(&li, text, len);
  guint total = line_index_count(&li);
  guint s_line = (start_line == 0) ? 1 : MIN(start_line, total);
  guint e_line = (end_line == 0 || end_line > total) ? total : MAX(end_line, s_line);

  gsize range_s = line_index_byte_for_line(&li, s_line);
  gsize range_e = line_index_end_byte_for_line(&li, e_line);

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

    SymDef *def = resolve_workspace_symbol(g, h->name, canon_file, want_types);
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

  line_index_clear(&li);
  if (out_ext_suppressed != NULL) {
    *out_ext_suppressed = ext_suppressed;
  }
  return out;
}

/* ═══════════════════════════ DIFF Lens Across Open Buffers ═══════════════════════════ */

GPtrArray *
ide_strip_graph_extract_diff_targets(IdeStripGraph *g, GPtrArray *open_buffers)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
  if (g == NULL || open_buffers == NULL) {
    return out;
  }
  ide_strip_graph_ensure_indexed(g);

  g_autoptr(GHashTable) file_cache =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cached_file_text_free);
  g_autoptr(GHashTable) dedup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint b = 0; b < open_buffers->len; b++) {
    IdeBuffer *buf = g_ptr_array_index(open_buffers, b);
    if (buf == NULL) {
      continue;
    }
    const char *path = ide_buffer_get_file_path(buf);
    if (!ide_strip_path_in_workspace(path, g->workspace_root)) {
      continue;
    }
    g_autofree char *canon = canonicalize_dir_clean(path);
    g_autofree char *rel = make_rel_path(canon, g->workspace_root);
    gboolean is_agent = ide_buffer_has_agent_changes(buf);
    gboolean is_dirty = ide_buffer_get_modified(buf);

    g_autofree char *text = ide_buffer_get_text(buf);
    if (text == NULL) {
      continue;
    }
    gsize len = strlen(text);
    LineIndex li;
    line_index_init(&li, text, len);
    guint total = line_index_count(&li);

    gboolean added_any_for_buf = FALSE;
    for (guint ln = 1; ln <= total; ln++) {
      IdeDiffState ds = ide_buffer_get_line_diff(buf, ln);
      if (ds == IDE_DIFF_NONE) {
        continue;
      }
      g_autofree char *sym_name = NULL;
      guint s_line = 1, e_line = total;
      int kind = IDE_STRIP_SYM_FILE;
      ide_strip_graph_find_enclosing_symbol(g, canon, text, ln, &sym_name, &s_line, &e_line, &kind);
      if (sym_name == NULL) {
        sym_name = g_strdup(rel);
      }

      g_autofree char *dkey = g_strdup_printf("%s:%s:%u", canon, sym_name, s_line);
      IdeStripGraphEntry *existing = g_hash_table_lookup(dedup, dkey);
      if (existing != NULL) {
        existing->call_count++;
        if (ds == IDE_DIFF_AGENT || is_agent) {
          existing->is_agent_modified = TRUE;
        }
        existing->is_dirty |= is_dirty;
        added_any_for_buf = TRUE;
        continue;
      }

      IdeStripGraphEntry *e = g_new0(IdeStripGraphEntry, 1);
      e->name = g_steal_pointer(&sym_name);
      e->file_path = g_strdup(canon);
      e->rel_path = g_strdup(rel);
      e->start_line = s_line;
      e->end_line = e_line;
      e->call_line = ln;
      e->call_count = 1;
      e->kind = kind;
      e->peek_code = slice_peek_from_memory(text, len, &li, s_line, STRIP_DEFAULT_PEEK);
      e->is_agent_modified = (ds == IDE_DIFF_AGENT || is_agent);
      e->is_dirty = is_dirty;
      e->is_fallback = FALSE;
      SymDef *def = resolve_workspace_symbol(g, e->name, canon, kind == IDE_STRIP_SYM_TYPE);
      if (def != NULL) {
        e->child_count = compute_child_count(g, def, file_cache, canon, text, len);
      }
      g_ptr_array_add(out, e);
      g_hash_table_insert(dedup, g_steal_pointer(&dkey), e);
      added_any_for_buf = TRUE;
    }

    if (!added_any_for_buf && (is_agent || is_dirty)) {
      IdeStripGraphEntry *e = g_new0(IdeStripGraphEntry, 1);
      e->name = g_strdup(rel);
      e->file_path = g_strdup(canon);
      e->rel_path = g_strdup(rel);
      e->start_line = 1;
      e->end_line = total;
      e->call_line = 1;
      e->call_count = 1;
      e->kind = IDE_STRIP_SYM_FILE;
      e->peek_code = slice_peek_from_memory(text, len, &li, 1, STRIP_DEFAULT_PEEK);
      e->is_agent_modified = is_agent;
      e->is_dirty = is_dirty;
      e->is_fallback = FALSE;
      g_ptr_array_add(out, e);
    }
    line_index_clear(&li);
  }
  return out;
}

/* ═══════════════════════════ Async LSP Raw Query Bridge ═══════════════════════════ */

typedef struct {
  IdeStripGraph       *graph;
  GCancellable        *alive;
  IdeLspManager       *lsp;
  IdeBuffer           *buf;
  char                *source_path;
  int                  lens_mode;
  IdeStripGraphAsyncCb cb;
  gpointer             user_data;
} StripLspQuery;

static void
strip_lsp_query_free(StripLspQuery *q)
{
  if (q == NULL) {
    return;
  }
  g_clear_object(&q->alive);
  g_free(q->source_path);
  g_free(q);
}

static guint
utf16_col_for_buf_line(IdeBuffer *buf, guint line0, guint col_1based)
{
  if (col_1based <= 1) {
    return 0;
  }
  g_autofree char *text = ide_buffer_get_text(buf);
  if (text == NULL) {
    return col_1based - 1;
  }
  LineIndex li;
  gsize len = strlen(text);
  line_index_init(&li, text, len);
  guint target_line = MIN(line0 + 1, line_index_count(&li));
  gsize s = line_index_byte_for_line(&li, target_line);
  gsize e = line_index_end_byte_for_line(&li, target_line);
  line_index_clear(&li);
  if (e > s && text[e - 1] == '\n') {
    e--;
  }
  g_autofree char *line = g_strndup(text + s, e - s);
  glong chars = g_utf8_strlen(line, -1);
  glong off = MIN((glong)(col_1based - 1), chars);
  const char *p = g_utf8_offset_to_pointer(line, off);
  return ide_utf8_byte_to_utf16_col(line, (gsize)(p - line));
}

static int
kind_from_badge(const char *badge)
{
  if (badge == NULL) {
    return IDE_STRIP_SYM_FUNC;
  }
  if (strcmp(badge, "T") == 0) return IDE_STRIP_SYM_TYPE;
  if (strcmp(badge, "F") == 0) return IDE_STRIP_SYM_FILE;
  if (strcmp(badge, "c") == 0) return IDE_STRIP_SYM_MACRO;
  return IDE_STRIP_SYM_FUNC;
}

static gint
entry_by_call_line_cmp(gconstpointer a, gconstpointer b)
{
  const IdeStripGraphEntry *x = *(const IdeStripGraphEntry *const *)a;
  const IdeStripGraphEntry *y = *(const IdeStripGraphEntry *const *)b;
  if (x->call_line != y->call_line) {
    return x->call_line < y->call_line ? -1 : 1;
  }
  return g_strcmp0(x->name, y->name);
}

static void
on_lsp_references_cb(JsonNode *result, const char *error, gpointer user_data)
{
  StripLspQuery *q = user_data;
  if (g_cancellable_is_cancelled(q->alive)) {
    strip_lsp_query_free(q);
    return;
  }

  g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
  if (error != NULL || result == NULL) {
    q->cb(entries, 0, error != NULL, q->user_data);
    strip_lsp_query_free(q);
    return;
  }

  IdeStripGraph *g = q->graph;
  ide_strip_graph_ensure_indexed(g);

  g_autoptr(GPtrArray) locs = ide_lsp_parse_locations(result);
  g_autoptr(GHashTable) file_cache =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cached_file_text_free);
  g_autoptr(GHashTable) dedup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint ext_suppressed = 0;
  if (JSON_NODE_HOLDS_ARRAY(result)) {
    guint raw_len = json_array_get_length(json_node_get_array(result));
    if (raw_len > locs->len) {
      ext_suppressed += raw_len - locs->len; /* non-file URIs such as jdt:// */
    }
  }

  for (guint i = 0; i < locs->len; i++) {
    const IdeLspLocation *loc = g_ptr_array_index(locs, i);
    if (!ide_strip_path_in_workspace(loc->path, g->workspace_root)) {
      ext_suppressed++;
      continue;
    }
    g_autofree char *canon = canonicalize_dir_clean(loc->path);
    g_autofree char *rel = make_rel_path(canon, g->workspace_root);

    g_autofree char *sym_name = NULL;
    guint s_line = loc->line_1based, e_line = loc->line_1based;
    int kind = IDE_STRIP_SYM_FILE;
    ide_strip_graph_find_enclosing_symbol(g, canon, NULL, loc->line_1based,
                                          &sym_name, &s_line, &e_line, &kind);
    if (sym_name == NULL) {
      sym_name = g_strdup(rel);
    }

    g_autofree char *dkey = g_strdup_printf("%s:%s:%u", canon, sym_name, s_line);
    IdeStripGraphEntry *existing = g_hash_table_lookup(dedup, dkey);
    if (existing != NULL) {
      existing->call_count++;
      continue;
    }

    IdeStripGraphEntry *e = g_new0(IdeStripGraphEntry, 1);
    e->name = g_steal_pointer(&sym_name);
    e->file_path = g_strdup(canon);
    e->rel_path = g_strdup(rel);
    e->start_line = s_line;
    e->end_line = e_line;
    e->call_line = loc->line_1based;
    e->call_count = 1;
    e->kind = kind;
    e->peek_code = ide_strip_graph_read_peek(canon, loc->line_1based, STRIP_DEFAULT_PEEK, NULL);
    SymDef *def = resolve_workspace_symbol(g, e->name, canon, kind == IDE_STRIP_SYM_TYPE);
    if (def != NULL) {
      e->child_count = compute_child_count(g, def, file_cache, NULL, NULL, 0);
    }
    e->is_fallback = FALSE;
    g_ptr_array_add(entries, e);
    g_hash_table_insert(dedup, g_steal_pointer(&dkey), e);
  }

  q->cb(entries, ext_suppressed, FALSE, q->user_data);
  strip_lsp_query_free(q);
}

static void
extract_from_ranges_for_entry(JsonNode *result, const IdeLspCallEntry *ce,
                              guint *out_call_line, guint *out_call_count)
{
  *out_call_line = 0;
  *out_call_count = 1;
  if (result == NULL || !JSON_NODE_HOLDS_ARRAY(result) || ce == NULL) {
    return;
  }
  JsonArray *arr = json_node_get_array(result);
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonNode *el = json_array_get_element(arr, i);
    if (el == NULL || !JSON_NODE_HOLDS_OBJECT(el)) {
      continue;
    }
    JsonObject *o = json_node_get_object(el);
    if (!json_object_has_member(o, "to")) {
      continue;
    }
    JsonNode *tn = json_object_get_member(o, "to");
    if (tn == NULL || !JSON_NODE_HOLDS_OBJECT(tn)) {
      continue;
    }
    JsonObject *to = json_node_get_object(tn);
    const char *nm = json_object_has_member(to, "name") ? json_object_get_string_member(to, "name") : NULL;
    const char *uri = json_object_has_member(to, "uri") ? json_object_get_string_member(to, "uri") : NULL;
    if (g_strcmp0(nm, ce->name) != 0 || uri == NULL) {
      continue;
    }
    g_autofree char *p = g_filename_from_uri(uri, NULL, NULL);
    if (g_strcmp0(p, ce->path) != 0) {
      continue;
    }
    if (!json_object_has_member(o, "fromRanges")) {
      return;
    }
    JsonNode *rn = json_object_get_member(o, "fromRanges");
    if (rn == NULL || !JSON_NODE_HOLDS_ARRAY(rn)) {
      return;
    }
    JsonArray *rarr = json_node_get_array(rn);
    guint rlen = json_array_get_length(rarr);
    if (rlen == 0) {
      return;
    }
    *out_call_count = rlen;
    JsonNode *r0 = json_array_get_element(rarr, 0);
    if (r0 != NULL && JSON_NODE_HOLDS_OBJECT(r0)) {
      JsonObject *ro = json_node_get_object(r0);
      if (json_object_has_member(ro, "start")) {
        JsonObject *st = json_object_get_object_member(ro, "start");
        if (st != NULL) {
          gint64 ln0 = MAX(0, json_object_get_int_member_with_default(st, "line", 0));
          *out_call_line = (guint)ln0 + 1;
        }
      }
    }
    return;
  }
}

static void
on_lsp_outgoing_calls_cb(JsonNode *result, const char *error, gpointer user_data)
{
  StripLspQuery *q = user_data;
  if (g_cancellable_is_cancelled(q->alive)) {
    strip_lsp_query_free(q);
    return;
  }

  g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
  if (error != NULL || result == NULL) {
    q->cb(entries, 0, error != NULL, q->user_data);
    strip_lsp_query_free(q);
    return;
  }

  IdeStripGraph *g = q->graph;
  ide_strip_graph_ensure_indexed(g);

  g_autoptr(GPtrArray) calls = ide_lsp_parse_call_hierarchy(result, TRUE);
  g_autoptr(GHashTable) file_cache =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cached_file_text_free);
  guint ext_suppressed = 0;
  if (JSON_NODE_HOLDS_ARRAY(result)) {
    guint raw_len = json_array_get_length(json_node_get_array(result));
    if (raw_len > calls->len) {
      ext_suppressed += raw_len - calls->len; /* non-file URIs such as jdt:// */
    }
  }

  for (guint i = 0; i < calls->len; i++) {
    const IdeLspCallEntry *ce = g_ptr_array_index(calls, i);
    if (!ide_strip_path_in_workspace(ce->path, g->workspace_root)) {
      ext_suppressed++;
      continue;
    }
    g_autofree char *canon = canonicalize_dir_clean(ce->path);
    g_autofree char *rel = make_rel_path(canon, g->workspace_root);

    guint call_line = 0;
    guint call_count = 1;
    extract_from_ranges_for_entry(result, ce, &call_line, &call_count);

    IdeStripGraphEntry *e = g_new0(IdeStripGraphEntry, 1);
    e->name = g_strdup(ce->name);
    e->file_path = g_strdup(canon);
    e->rel_path = g_strdup(rel);
    e->start_line = ce->line_1based;
    e->end_line = ce->line_1based;
    e->call_line = call_line > 0 ? call_line : ce->line_1based;
    e->call_count = call_count;
    e->kind = kind_from_badge(ce->kind_badge);
    e->peek_code = ide_strip_graph_read_peek(canon, ce->line_1based, STRIP_DEFAULT_PEEK, NULL);
    e->is_fallback = FALSE;

    SymDef *def = resolve_workspace_symbol(g, ce->name, canon, e->kind == IDE_STRIP_SYM_TYPE);
    if (def != NULL) {
      e->end_line = def->end_line;
      e->child_count = compute_child_count(g, def, file_cache, NULL, NULL, 0);
    }
    g_ptr_array_add(entries, e);
  }

  g_ptr_array_sort(entries, entry_by_call_line_cmp);
  q->cb(entries, ext_suppressed, FALSE, q->user_data);
  strip_lsp_query_free(q);
}

static void
on_lsp_prepare_call_hierarchy_cb(JsonNode *result, const char *error, gpointer user_data)
{
  StripLspQuery *q = user_data;
  if (g_cancellable_is_cancelled(q->alive)) {
    strip_lsp_query_free(q);
    return;
  }

  JsonNode *first = NULL;
  if (error == NULL && result != NULL) {
    if (JSON_NODE_HOLDS_ARRAY(result)) {
      JsonArray *arr = json_node_get_array(result);
      if (json_array_get_length(arr) > 0) {
        JsonNode *n0 = json_array_get_element(arr, 0);
        if (n0 != NULL && JSON_NODE_HOLDS_OBJECT(n0)) {
          first = n0;
        }
      }
    } else if (JSON_NODE_HOLDS_OBJECT(result)) {
      first = result;
    }
  }

  if (first == NULL) {
    g_autoptr(GPtrArray) empty = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
    q->cb(empty, 0, error != NULL, q->user_data);
    strip_lsp_query_free(q);
    return;
  }

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "item");
  json_builder_add_value(b, json_node_copy(first));
  json_builder_end_object(b);

  if (!ide_lsp_manager_request_raw(q->lsp, q->buf, "callHierarchy/outgoingCalls",
                                   json_builder_get_root(b),
                                   on_lsp_outgoing_calls_cb, q)) {
    g_autoptr(GPtrArray) empty = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
    q->cb(empty, 0, TRUE, q->user_data);
    strip_lsp_query_free(q);
  }
}

gboolean
ide_strip_graph_query_lsp_async(IdeStripGraph       *g,
                                IdeLspManager       *lsp,
                                IdeBuffer           *buf,
                                guint                line_1based,
                                guint                col_1based,
                                int                  lens_mode,
                                IdeStripGraphAsyncCb cb,
                                gpointer             user_data)
{
  if (g == NULL || lsp == NULL || buf == NULL || cb == NULL) {
    return FALSE;
  }
  if (lens_mode != IDE_STRIP_GRAPH_LENS_CALLS && lens_mode != IDE_STRIP_GRAPH_LENS_IMPACT) {
    return FALSE;
  }
  if (!ide_lsp_manager_is_ready(lsp, buf)) {
    return FALSE;
  }

  const char *path = ide_buffer_get_file_path(buf);
  if (path == NULL) {
    return FALSE;
  }
  g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
  if (uri == NULL) {
    return FALSE;
  }

  guint line0 = (line_1based > 0) ? (line_1based - 1) : 0;
  guint utf16 = utf16_col_for_buf_line(buf, line0, col_1based);

  StripLspQuery *q = g_new0(StripLspQuery, 1);
  q->graph = g;
  q->alive = g_object_ref(g->alive);
  q->lsp = lsp;
  q->buf = buf;
  q->source_path = g_strdup(path);
  q->lens_mode = lens_mode;
  q->cb = cb;
  q->user_data = user_data;

  if (lens_mode == IDE_STRIP_GRAPH_LENS_IMPACT) {
    JsonNode *params = ide_lsp_position_params(uri, line0, utf16);
    JsonObject *po = json_node_get_object(params);
    JsonObject *ctx = json_object_new();
    json_object_set_boolean_member(ctx, "includeDeclaration", FALSE);
    json_object_set_object_member(po, "context", ctx);

    if (!ide_lsp_manager_request_raw(lsp, buf, "textDocument/references",
                                     params, on_lsp_references_cb, q)) {
      strip_lsp_query_free(q);
      return FALSE;
    }
    return TRUE;
  }

  /* IDE_STRIP_LENS_CALLS: prepareCallHierarchy -> outgoingCalls */
  JsonNode *params = ide_lsp_position_params(uri, line0, utf16);
  if (!ide_lsp_manager_request_raw(lsp, buf, "textDocument/prepareCallHierarchy",
                                   params, on_lsp_prepare_call_hierarchy_cb, q)) {
    strip_lsp_query_free(q);
    return FALSE;
  }
  return TRUE;
}
