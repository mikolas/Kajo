/* Semantic token highlighter for kajo-ide (11 token classes, scheme-aware
 * palettes, Markdown/code-fence aware). Split out of ide_buffer.c; the public
 * entry point is ide_highlight_source_buffer() declared in ide_buffer.h. */
#include "ide_buffer.h"
#include "ide_ts.h"

#include <ctype.h>
#include <string.h>

/* ─── Semantic Token Highlighter (UI Gallery & koodieditori Parity) ─── */

typedef struct {
  const char *id;
  const char *keyword;
  const char *control;
  const char *function;
  const char *type;
  const char *macro;
  const char *variable;
  const char *string;
  const char *escape;
  const char *number;
  const char *comment;
  const char *operator_fg;
} IdeSchemePalette;

static const IdeSchemePalette kSchemePalettes[] = {
  {
    "kajo-metro-dark",
    "#569cd6", /* keyword: storage / declaration */
    "#c586c0", /* control: if, return, switch, for */
    "#dcdcaa", /* function: declarations & calls */
    "#4ec9b0", /* type: structs, typedefs, primitives */
    "#4fc1ff", /* macro: constants, enums, #define */
    "#9cdcfe", /* variable: parameters, locals, fields */
    "#ce9178", /* string */
    "#d7ba7d", /* escape / format specifier */
    "#b5cea8", /* number */
    "#6a9955", /* comment */
    "#d4d4d4"  /* operator */
  },
  {
    "kajo-oled-pitch",
    "#00d8f0",
    "#ff6ac1",
    "#ffe66d",
    "#3dd675",
    "#c792ea",
    "#d8f8ff",
    "#ff9e64",
    "#00d8f0",
    "#c792ea",
    "#637777",
    "#d8d8d8"
  },
  {
    "kajo-nord-frost",
    "#81a1c1",
    "#b48ead",
    "#88c0d0",
    "#8fbcbb",
    "#5e81ac",
    "#d8dee9",
    "#a3be8c",
    "#ebcb8b",
    "#b48ead",
    "#616e88",
    "#81a1c1"
  },
  {
    "kajo-amber-crt",
    "#ffd700",
    "#ffcc33",
    "#ffe066",
    "#ff9900",
    "#ffc233",
    "#ffb000",
    "#ff8000",
    "#ffd700",
    "#ffc233",
    "#8c6200",
    "#ffb000"
  },
  {
    "kajo-monokai-metro",
    "#ff6188",
    "#ff6188",
    "#a9dc76",
    "#78dce8",
    "#ab9df2",
    "#fcfcfa",
    "#ffd866",
    "#fc9867",
    "#ab9df2",
    "#727072",
    "#ff6188"
  },
  {
    "kajo-tokyo-night",
    "#7aa2f7",
    "#bb9af7",
    "#7dcfff",
    "#2ac3de",
    "#ff9e64",
    "#c0caf5",
    "#9ece6a",
    "#e0af68",
    "#ff9e64",
    "#565f89",
    "#89ddff"
  },
  {
    "kajo-catppuccin-mocha",
    "#89b4fa",
    "#cba6f7",
    "#f9e2af",
    "#94e2d5",
    "#fab387",
    "#cdd6f4",
    "#a6e3a1",
    "#f5c2e7",
    "#fab387",
    "#6c7086",
    "#94e2d5"
  },
  {
    "kajo-gruvbox-dark",
    "#fb4934",
    "#d3869b",
    "#fabd2f",
    "#8ec07c",
    "#fe8019",
    "#83a598",
    "#b8bb26",
    "#fe8019",
    "#d3869b",
    "#928374",
    "#ebdbb2"
  },
  {
    "kajo-solarized-light",
    "#268bd2",
    "#859900",
    "#b58900",
    "#2aa198",
    "#cb4b16",
    "#586e75",
    "#2aa198",
    "#d33682",
    "#d33682",
    "#93a1a1",
    "#657b83"
  }
};

static const IdeSchemePalette *
lookup_scheme_palette(const char *scheme_id)
{
  if (scheme_id != NULL && *scheme_id != '\0') {
    for (gsize i = 0; i < G_N_ELEMENTS(kSchemePalettes); i++) {
      if (g_strcmp0(kSchemePalettes[i].id, scheme_id) == 0) {
        return &kSchemePalettes[i];
      }
    }
  }
  return &kSchemePalettes[0];
}

typedef struct {
  GtkTextTag *kw;
  GtkTextTag *ctrl;
  GtkTextTag *fn;
  GtkTextTag *type;
  GtkTextTag *mac;
  GtkTextTag *var;
  GtkTextTag *str;
  GtkTextTag *esc;
  GtkTextTag *num;
  GtkTextTag *cmt;
  GtkTextTag *op;
} IdeSynTags;

static GtkTextTag *
ensure_syn_tag(GtkTextBuffer *tbuf,
               GtkTextTagTable *table,
               const char    *name,
               const char    *fg_hex,
               gboolean       bold,
               gboolean       italic)
{
  GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
  if (tag == NULL) {
    tag = gtk_text_buffer_create_tag(tbuf,
                                     name,
                                     "foreground", fg_hex,
                                     "weight", bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                                     "style", italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
                                     NULL);
  } else {
    g_object_set(tag,
                 "foreground", fg_hex,
                 "weight", bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                 "style", italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
                 NULL);
  }
  return tag;
}

static void
init_syn_tags(GtkTextBuffer  *tbuf,
              const char     *scheme_id,
              IdeSynTags     *out)
{
  const IdeSchemePalette *pal = lookup_scheme_palette(scheme_id);
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(tbuf);

  out->kw   = ensure_syn_tag(tbuf, table, "ide-syn-kw",   pal->keyword,     TRUE,  FALSE);
  out->ctrl = ensure_syn_tag(tbuf, table, "ide-syn-ctrl", pal->control,     TRUE,  FALSE);
  out->fn   = ensure_syn_tag(tbuf, table, "ide-syn-fn",   pal->function,    FALSE, FALSE);
  out->type = ensure_syn_tag(tbuf, table, "ide-syn-type", pal->type,        FALSE, FALSE);
  out->mac  = ensure_syn_tag(tbuf, table, "ide-syn-mac",  pal->macro,       FALSE, FALSE);
  out->var  = ensure_syn_tag(tbuf, table, "ide-syn-var",  pal->variable,    FALSE, FALSE);
  out->str  = ensure_syn_tag(tbuf, table, "ide-syn-str",  pal->string,      FALSE, FALSE);
  out->esc  = ensure_syn_tag(tbuf, table, "ide-syn-esc",  pal->escape,      FALSE, FALSE);
  out->num  = ensure_syn_tag(tbuf, table, "ide-syn-num",  pal->number,      FALSE, FALSE);
  out->cmt  = ensure_syn_tag(tbuf, table, "ide-syn-cmt",  pal->comment,     FALSE, TRUE);
  out->op   = ensure_syn_tag(tbuf, table, "ide-syn-op",   pal->operator_fg, FALSE, FALSE);
}

static void
clear_syn_tags(GtkTextBuffer *tbuf,
               const IdeSynTags *t,
               const GtkTextIter *start,
               const GtkTextIter *end)
{
  GtkTextTag *all[] = { t->kw, t->ctrl, t->fn, t->type, t->mac, t->var,
                        t->str, t->esc, t->num, t->cmt, t->op };
  for (gsize i = 0; i < G_N_ELEMENTS(all); i++) {
    gtk_text_buffer_remove_tag(tbuf, all[i], start, end);
  }
}

static gboolean
word_in_list(const char *word, gsize len, const char *const *list)
{
  for (gsize i = 0; list[i] != NULL; i++) {
    if (strncmp(word, list[i], len) == 0 && list[i][len] == '\0') {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_control_keyword(const char *w, gsize len)
{
  static const char *const kControlWords[] = {
    "if", "else", "elif", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "defer", "go", "select", "match",
    "try", "catch", "except", "finally", "raise", "throw", "yield", "async",
    "await", "loop", NULL
  };
  return word_in_list(w, len, kControlWords);
}

static gboolean
is_declaration_keyword(const char *w, gsize len)
{
  static const char *const kDeclWords[] = {
    "static", "const", "struct", "enum", "union", "typedef", "extern",
    "inline", "volatile", "register", "restrict", "auto", "sizeof",
    "alignof", "offsetof", "_Generic", "_Static_assert", "constexpr",
    "func", "package", "import", "var", "type", "interface", "map", "chan",
    "range", "fn", "let", "mut", "pub", "impl", "trait", "use", "mod",
    "crate", "where", "unsafe", "ref", "move", "dyn", "def", "class",
    "from", "as", "with", "pass", "lambda", "global", "nonlocal", "in",
    "is", "and", "or", "not", "del", "assert", "namespace", "template",
    "typename", "virtual", "override", "public", "private", "protected",
    "new", "delete", "friend", "using", "operator", "export", "function",
    NULL
  };
  return word_in_list(w, len, kDeclWords);
}

static gboolean
is_builtin_type_word(const char *w, gsize len)
{
  static const char *const kTypeWords[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "bool", "_Bool", "va_list", "FILE", "DIR",
    "gboolean", "gchar", "guchar", "gint", "guint", "gshort", "gushort",
    "glong", "gulong", "gint8", "guint8", "gint16", "guint16", "gint32",
    "guint32", "gint64", "guint64", "gfloat", "gdouble", "gsize", "gssize",
    "goffset", "gintptr", "guintptr", "gpointer", "gconstpointer",
    "gunichar", "GType", "GQuark",
    "string", "byte", "rune", "error", "any", "int8", "int16", "int32",
    "int64", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr",
    "float32", "float64", "complex64", "complex128",
    "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
    "u128", "usize", "f32", "f64", "str", "Self",
    NULL
  };
  if (word_in_list(w, len, kTypeWords)) {
    return TRUE;
  }
  if (len > 2 && w[len - 2] == '_' && (w[len - 1] == 't' || w[len - 1] == 'T')) {
    return TRUE;
  }
  return FALSE;
}

static gboolean
is_constant_or_macro_word(const char *w, gsize len)
{
  static const char *const kConstWords[] = {
    "TRUE", "FALSE", "NULL", "true", "false", "nil", "null", "nullptr",
    "iota", "None", "G_GNUC_UNUSED", "G_BEGIN_DECLS", "G_END_DECLS",
    "__LINE__", "__FILE__", "__func__", "__DATE__", "__TIME__",
    NULL
  };
  if (word_in_list(w, len, kConstWords)) {
    return TRUE;
  }
  /* UPPER_SNAKE_CASE identifier (len >= 2, all A-Z / 0-9 / _, at least one A-Z) */
  if (len >= 2 && (w[0] >= 'A' && w[0] <= 'Z')) {
    gboolean has_lower = FALSE;
    for (gsize i = 0; i < len; i++) {
      char c = w[i];
      if (c >= 'a' && c <= 'z') {
        has_lower = TRUE;
        break;
      }
    }
    if (!has_lower) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_pascal_case_type(const char *w, gsize len)
{
  if (len < 2 || !(w[0] >= 'A' && w[0] <= 'Z')) {
    return FALSE;
  }
  for (gsize i = 1; i < len; i++) {
    if (w[i] >= 'a' && w[i] <= 'z') {
      return TRUE;
    }
  }
  return FALSE;
}

typedef struct {
  GtkTextBuffer *tbuf;
  const char    *base;
  gsize          cur_byte;
  GtkTextIter    cur_iter;
} IdeTagCursor;

static void
apply_tag_at_bytes(IdeTagCursor *tc,
                   GtkTextTag   *tag,
                   gsize         start_byte,
                   gsize         end_byte)
{
  if (tag == NULL || end_byte <= start_byte) {
    return;
  }
  if (start_byte < tc->cur_byte) {
    gtk_text_buffer_get_start_iter(tc->tbuf, &tc->cur_iter);
    tc->cur_byte = 0;
  }
  if (start_byte > tc->cur_byte) {
    glong delta = g_utf8_pointer_to_offset(tc->base + tc->cur_byte,
                                           tc->base + start_byte);
    if (delta > 0) {
      gtk_text_iter_forward_chars(&tc->cur_iter, (gint)delta);
    }
    tc->cur_byte = start_byte;
  }
  GtkTextIter start_it = tc->cur_iter;
  glong span_chars = g_utf8_pointer_to_offset(tc->base + start_byte,
                                              tc->base + end_byte);
  if (span_chars > 0) {
    gtk_text_iter_forward_chars(&tc->cur_iter, (gint)span_chars);
    tc->cur_byte = end_byte;
    gtk_text_buffer_apply_tag(tc->tbuf, tag, &start_it, &tc->cur_iter);
  }
}

static void
highlight_string_span(IdeTagCursor     *tc,
                      const IdeSynTags *tags,
                      const char       *text,
                      gsize             s_start,
                      gsize             s_end)
{
  apply_tag_at_bytes(tc, tags->str, s_start, s_end);

  /* Highlight escape sequences (\n, \t, \x.., etc.) and printf specifiers (%s, %d, %u) */
  for (gsize k = s_start + 1; k + 1 < s_end; k++) {
    if (text[k] == '\\' && k + 1 < s_end - 1) {
      gsize esc_end = k + 2;
      if (text[k + 1] == 'x' || text[k + 1] == 'u' || text[k + 1] == 'U') {
        while (esc_end < s_end - 1 && g_ascii_isxdigit(text[esc_end])) {
          esc_end++;
        }
      }
      apply_tag_at_bytes(tc, tags->esc, k, esc_end);
      k = esc_end - 1;
    } else if (text[k] == '%' && k + 1 < s_end - 1 && text[k + 1] != ' ') {
      gsize fmt_end = k + 1;
      if (text[fmt_end] == '%') {
        fmt_end++;
      } else {
        while (fmt_end < s_end - 1 &&
               strchr("-+ #0123456789.*hlLzjt", text[fmt_end]) != NULL) {
          fmt_end++;
        }
        if (fmt_end < s_end - 1 && g_ascii_isalpha(text[fmt_end])) {
          fmt_end++;
        }
      }
      if (fmt_end > k + 1) {
        apply_tag_at_bytes(tc, tags->esc, k, fmt_end);
        k = fmt_end - 1;
      }
    }
  }
}

static void
highlight_code_region(IdeTagCursor     *tc,
                      const IdeSynTags *tags,
                      const char       *text,
                      gsize             region_start,
                      gsize             region_end,
                      gboolean          hash_line_comments)
{
  gsize i = region_start;
  gboolean at_line_start = TRUE;

  while (i < region_end) {
    char c = text[i];

    if (c == '\n') {
      at_line_start = TRUE;
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    /* 1. Single-line // comment */
    if (c == '/' && i + 1 < region_end && text[i + 1] == '/') {
      gsize start = i;
      i += 2;
      while (i < region_end && text[i] != '\n') {
        i++;
      }
      apply_tag_at_bytes(tc, tags->cmt, start, i);
      continue;
    }

    /* 2. Multi-line C block comment */
    if (c == '/' && i + 1 < region_end && text[i + 1] == '*') {
      gsize start = i;
      i += 2;
      while (i + 1 < region_end && !(text[i] == '*' && text[i + 1] == '/')) {
        i++;
      }
      if (i + 1 < region_end) {
        i += 2;
      } else {
        i = region_end;
      }
      apply_tag_at_bytes(tc, tags->cmt, start, i);
      at_line_start = FALSE;
      continue;
    }

    /* 3. '#' Preprocessor (C/C++) or '#' Comment (Python/Shell/TOML/YAML/Meson) */
    if (c == '#') {
      if (hash_line_comments) {
        gsize start = i;
        while (i < region_end && text[i] != '\n') {
          i++;
        }
        apply_tag_at_bytes(tc, tags->cmt, start, i);
        continue;
      } else if (at_line_start) {
        gsize start = i++;
        while (i < region_end && (text[i] == ' ' || text[i] == '\t')) {
          i++;
        }
        gsize kw_start = i;
        while (i < region_end && (g_ascii_isalnum(text[i]) || text[i] == '_')) {
          i++;
        }
        apply_tag_at_bytes(tc, tags->ctrl, start, i);

        gsize kw_len = i - kw_start;
        if (kw_len == 7 && strncmp(text + kw_start, "include", 7) == 0) {
          while (i < region_end && (text[i] == ' ' || text[i] == '\t')) {
            i++;
          }
          if (i < region_end && (text[i] == '<' || text[i] == '"')) {
            char end_ch = (text[i] == '<') ? '>' : '"';
            gsize inc_start = i++;
            while (i < region_end && text[i] != '\n' && text[i] != end_ch) {
              i++;
            }
            if (i < region_end && text[i] == end_ch) {
              i++;
            }
            apply_tag_at_bytes(tc, tags->str, inc_start, i);
          }
        }
        at_line_start = FALSE;
        continue;
      }
    }

    at_line_start = FALSE;

    /* 4. String & character literals ("...", '...', `...`) */
    if (c == '"' || c == '\'' || c == '`') {
      char quote = c;
      gsize start = i++;
      while (i < region_end) {
        if (text[i] == '\\' && quote != '`') {
          i += (i + 1 < region_end) ? 2 : 1;
          continue;
        }
        if (text[i] == quote) {
          i++;
          break;
        }
        if (text[i] == '\n' && quote != '`') {
          break;
        }
        i++;
      }
      highlight_string_span(tc, tags, text, start, i);
      continue;
    }

    /* 5. Numeric literals */
    if (g_ascii_isdigit(c) ||
        (c == '.' && i + 1 < region_end && g_ascii_isdigit(text[i + 1]))) {
      gsize start = i++;
      while (i < region_end &&
             (g_ascii_isalnum(text[i]) || text[i] == '.' || text[i] == '_')) {
        if ((text[i] == 'e' || text[i] == 'E' || text[i] == 'p' || text[i] == 'P') &&
            i + 1 < region_end && (text[i + 1] == '+' || text[i + 1] == '-')) {
          i += 2;
          continue;
        }
        i++;
      }
      apply_tag_at_bytes(tc, tags->num, start, i);
      continue;
    }

    /* 6. Identifiers, Keywords, Types, Macros, Functions, Variables */
    if (g_ascii_isalpha(c) || c == '_') {
      gsize start = i++;
      while (i < region_end && (g_ascii_isalnum(text[i]) || text[i] == '_')) {
        i++;
      }
      gsize len = i - start;
      const char *word = text + start;

      /* Look ahead past spaces/tabs for '(' */
      gsize la = i;
      while (la < region_end && (text[la] == ' ' || text[la] == '\t')) {
        la++;
      }
      gboolean followed_by_paren = (la < region_end && text[la] == '(');

      /* Check if preceded by '->' or '.' */
      gboolean is_member = FALSE;
      if (start >= region_start + 1 && text[start - 1] == '.') {
        is_member = TRUE;
      } else if (start >= region_start + 2 &&
                 text[start - 2] == '-' && text[start - 1] == '>') {
        is_member = TRUE;
      }

      if (is_control_keyword(word, len)) {
        apply_tag_at_bytes(tc, tags->ctrl, start, i);
      } else if (is_declaration_keyword(word, len)) {
        apply_tag_at_bytes(tc, tags->kw, start, i);
      } else if (is_builtin_type_word(word, len)) {
        apply_tag_at_bytes(tc, tags->type, start, i);
      } else if (is_constant_or_macro_word(word, len)) {
        apply_tag_at_bytes(tc, tags->mac, start, i);
      } else if (followed_by_paren) {
        apply_tag_at_bytes(tc, tags->fn, start, i);
      } else if (is_pascal_case_type(word, len)) {
        apply_tag_at_bytes(tc, tags->type, start, i);
      } else if (is_member || len >= 1) {
        apply_tag_at_bytes(tc, tags->var, start, i);
      }
      continue;
    }

    /* 7. Operators & Punctuation */
    if (strchr("+-*/%=!<>&|^~?:", c) != NULL) {
      gsize start = i++;
      while (i < region_end && strchr("+-*/%=!<>&|^~?:", text[i]) != NULL) {
        i++;
      }
      apply_tag_at_bytes(tc, tags->op, start, i);
      continue;
    }

    /* Advance past UTF-8 character or punctuation delimiter */
    const char *next = g_utf8_next_char(text + i);
    gsize step = (gsize)(next - (text + i));
    i += (step > 0) ? step : 1;
  }
}

static void
highlight_markdown_document(IdeTagCursor     *tc,
                            const IdeSynTags *tags,
                            const char       *text,
                            gsize             len)
{
  gsize pos = 0;
  gboolean in_fence = FALSE;
  gsize fence_code_start = 0;

  while (pos < len) {
    gsize line_end = pos;
    while (line_end < len && text[line_end] != '\n') {
      line_end++;
    }

    gsize trimmed = pos;
    while (trimmed < line_end && (text[trimmed] == ' ' || text[trimmed] == '\t')) {
      trimmed++;
    }

    /* Fenced code block ``` */
    if (trimmed + 3 <= line_end && strncmp(text + trimmed, "```", 3) == 0) {
      if (in_fence) {
        if (pos > fence_code_start) {
          highlight_code_region(tc, tags, text, fence_code_start, pos, FALSE);
        }
        apply_tag_at_bytes(tc, tags->ctrl, trimmed, line_end);
        in_fence = FALSE;
      } else {
        apply_tag_at_bytes(tc, tags->ctrl, trimmed, line_end);
        in_fence = TRUE;
        fence_code_start = (line_end < len) ? (line_end + 1) : len;
      }
      pos = (line_end < len) ? (line_end + 1) : len;
      continue;
    }

    if (in_fence) {
      pos = (line_end < len) ? (line_end + 1) : len;
      continue;
    }

    /* ATX Heading (# .. ######) */
    if (trimmed < line_end && text[trimmed] == '#') {
      apply_tag_at_bytes(tc, tags->kw, trimmed, line_end);
      pos = (line_end < len) ? (line_end + 1) : len;
      continue;
    }

    /* Blockquote (> ...) */
    if (trimmed < line_end && text[trimmed] == '>') {
      apply_tag_at_bytes(tc, tags->cmt, trimmed, line_end);
      pos = (line_end < len) ? (line_end + 1) : len;
      continue;
    }

    /* Bullet / Task list prefix */
    if (trimmed + 1 < line_end &&
        (text[trimmed] == '-' || text[trimmed] == '*') &&
        text[trimmed + 1] == ' ') {
      gsize marker_end = trimmed + 2;
      if (trimmed + 5 <= line_end &&
          text[trimmed + 2] == '[' && text[trimmed + 4] == ']') {
        marker_end = trimmed + 5;
      }
      apply_tag_at_bytes(tc, tags->mac, trimmed, marker_end);
    }

    /* Inline spans: `code`, **bold**, [title](url) */
    for (gsize k = trimmed; k < line_end; k++) {
      if (text[k] == '`') {
        gsize close = k + 1;
        while (close < line_end && text[close] != '`') {
          close++;
        }
        if (close < line_end) {
          apply_tag_at_bytes(tc, tags->str, k, close + 1);
          k = close;
        }
      } else if (k + 1 < line_end && text[k] == '*' && text[k + 1] == '*') {
        gsize close = k + 2;
        while (close + 1 < line_end &&
               !(text[close] == '*' && text[close + 1] == '*')) {
          close++;
        }
        if (close + 1 < line_end) {
          apply_tag_at_bytes(tc, tags->mac, k, close + 2);
          k = close + 1;
        }
      } else if (text[k] == '[') {
        gsize rb = k + 1;
        while (rb < line_end && text[rb] != ']') {
          rb++;
        }
        if (rb + 1 < line_end && text[rb + 1] == '(') {
          gsize rp = rb + 2;
          while (rp < line_end && text[rp] != ')') {
            rp++;
          }
          if (rp < line_end) {
            apply_tag_at_bytes(tc, tags->type, k, rb + 1);
            apply_tag_at_bytes(tc, tags->kw, rb + 1, rp + 1);
            k = rp;
          }
        }
      }
    }

    pos = (line_end < len) ? (line_end + 1) : len;
  }

  if (in_fence && fence_code_start < len) {
    highlight_code_region(tc, tags, text, fence_code_start, len, FALSE);
  }
}

/* ─── Tree-sitter path ─── */

typedef struct {
  IdeTsDoc *doc;
} IdeTsState;

static void
ide_ts_state_free(gpointer data)
{
  IdeTsState *st = data;
  if (st != NULL) {
    ide_ts_doc_free(st->doc);
    g_free(st);
  }
}

typedef struct {
  IdeTagCursor     *tc;
  const IdeSynTags *tags;
  const char       *text;
} IdeTsApply;

static void
apply_ts_token(gsize start, gsize end, IdeTsToken token, gpointer user_data)
{
  IdeTsApply *ap = user_data;
  const IdeSynTags *t = ap->tags;
  switch (token) {
    case IDE_TS_TOKEN_KEYWORD:    apply_tag_at_bytes(ap->tc, t->kw, start, end); break;
    case IDE_TS_TOKEN_CONTROL:    apply_tag_at_bytes(ap->tc, t->ctrl, start, end); break;
    case IDE_TS_TOKEN_FUNCTION:   apply_tag_at_bytes(ap->tc, t->fn, start, end); break;
    case IDE_TS_TOKEN_TYPE:       apply_tag_at_bytes(ap->tc, t->type, start, end); break;
    case IDE_TS_TOKEN_MACRO:      apply_tag_at_bytes(ap->tc, t->mac, start, end); break;
    case IDE_TS_TOKEN_VARIABLE:   apply_tag_at_bytes(ap->tc, t->var, start, end); break;
    case IDE_TS_TOKEN_STRING:     highlight_string_span(ap->tc, t, ap->text, start, end); break;
    case IDE_TS_TOKEN_STRING_RAW: apply_tag_at_bytes(ap->tc, t->str, start, end); break;
    case IDE_TS_TOKEN_NUMBER:     apply_tag_at_bytes(ap->tc, t->num, start, end); break;
    case IDE_TS_TOKEN_COMMENT:    apply_tag_at_bytes(ap->tc, t->cmt, start, end); break;
    case IDE_TS_TOKEN_OPERATOR:   apply_tag_at_bytes(ap->tc, t->op, start, end); break;
    default: break;
  }
}

const IdeTsDoc *
ide_highlight_get_ts_doc(GtkSourceBuffer *sbuf)
{
  IdeTsState *st = sbuf != NULL ? g_object_get_data(G_OBJECT(sbuf), "ide-ts-state") : NULL;
  return st != NULL ? st->doc : NULL;
}

/* Parses (incrementally) and re-tags only the dirty region. Returns FALSE when
 * the language has no usable grammar so the caller can use the lexer. */
static gboolean
highlight_with_treesitter(GtkSourceBuffer  *sbuf,
                          IdeTsLang         lang,
                          const IdeSynTags *tags,
                          const char       *text,
                          gsize             len)
{
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(sbuf);
  IdeTsState *st = g_object_get_data(G_OBJECT(sbuf), "ide-ts-state");

  if (st != NULL && ide_ts_doc_get_lang(st->doc) != lang) {
    g_object_set_data(G_OBJECT(sbuf), "ide-ts-state", NULL);
    st = NULL;
  }
  gboolean fresh = (st == NULL);
  if (fresh) {
    IdeTsDoc *doc = ide_ts_doc_new(lang);
    if (doc == NULL) {
      return FALSE;
    }
    st = g_new0(IdeTsState, 1);
    st->doc = doc;
    g_object_set_data_full(G_OBJECT(sbuf), "ide-ts-state", st, ide_ts_state_free);
  }

  gsize ds = 0, de = 0;
  if (!ide_ts_doc_update(st->doc, text, len, &ds, &de)) {
    g_object_set_data(G_OBJECT(sbuf), "ide-ts-state", NULL);
    return FALSE;
  }
  if (fresh) {
    ds = 0;
    de = len;
  }
  if (de <= ds) {
    return TRUE; /* unchanged */
  }

  /* Widen to whole lines so partially covered tokens are re-tagged cleanly. */
  while (ds > 0 && text[ds - 1] != '\n') {
    ds--;
  }
  while (de < len && text[de] != '\n') {
    de++;
  }

  GtkTextIter it_start, it_end;
  gtk_text_buffer_get_iter_at_offset(tbuf, &it_start,
                                     (gint)g_utf8_pointer_to_offset(text, text + ds));
  gtk_text_buffer_get_iter_at_offset(tbuf, &it_end,
                                     (gint)g_utf8_pointer_to_offset(text, text + de));
  clear_syn_tags(tbuf, tags, &it_start, &it_end);

  IdeTagCursor tc;
  tc.tbuf = tbuf;
  tc.base = text;
  tc.cur_byte = 0;
  gtk_text_buffer_get_start_iter(tbuf, &tc.cur_iter);

  IdeTsApply ap = { &tc, tags, text };
  ide_ts_doc_walk(st->doc, ds, de, apply_ts_token, &ap);
  return TRUE;
}

void
ide_highlight_source_buffer(GtkSourceBuffer *sbuf,
                            const char      *file_path,
                            const char      *scheme_id)
{
  if (sbuf == NULL) {
    return;
  }

  /* Disable GtkSourceView's sparse 2005 regex tags so our semantic token pass
   * has full deterministic control over token colors while keeping the style
   * scheme's canvas background, current-line, selection, and bracket match. */
  gtk_source_buffer_set_highlight_syntax(sbuf, FALSE);

  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(sbuf);
  IdeSynTags tags;
  init_syn_tags(tbuf, scheme_id, &tags);

  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(tbuf, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(tbuf, &start, &end, FALSE);
  if (text == NULL || text[0] == '\0') {
    clear_syn_tags(tbuf, &tags, &start, &end);
    g_object_set_data(G_OBJECT(sbuf), "ide-ts-state", NULL);
    return;
  }
  gsize len = strlen(text);

  IdeTsLang ts_lang = ide_ts_lang_for_path(file_path);
  if (ts_lang != IDE_TS_LANG_NONE &&
      highlight_with_treesitter(sbuf, ts_lang, &tags, text, len)) {
    return;
  }
  g_object_set_data(G_OBJECT(sbuf), "ide-ts-state", NULL);
  clear_syn_tags(tbuf, &tags, &start, &end);

  IdeTagCursor tc;
  tc.tbuf = tbuf;
  tc.base = text;
  tc.cur_byte = 0;
  gtk_text_buffer_get_start_iter(tbuf, &tc.cur_iter);

  gboolean is_md = (file_path != NULL &&
                    (g_str_has_suffix(file_path, ".md") ||
                     g_str_has_suffix(file_path, ".markdown")));
  if (is_md) {
    highlight_markdown_document(&tc, &tags, text, len);
    return;
  }

  gboolean hash_comments = (file_path != NULL &&
                            (g_str_has_suffix(file_path, ".py") ||
                             g_str_has_suffix(file_path, ".sh") ||
                             g_str_has_suffix(file_path, ".toml") ||
                             g_str_has_suffix(file_path, ".yaml") ||
                             g_str_has_suffix(file_path, ".yml") ||
                             g_str_has_suffix(file_path, "meson.build") ||
                             g_str_has_suffix(file_path, "Makefile") ||
                             g_ascii_strcasecmp(file_path, "python") == 0 ||
                             g_ascii_strcasecmp(file_path, "py") == 0 ||
                             g_ascii_strcasecmp(file_path, "sh") == 0 ||
                             g_ascii_strcasecmp(file_path, "bash") == 0 ||
                             g_ascii_strcasecmp(file_path, "zsh") == 0 ||
                             g_ascii_strcasecmp(file_path, "toml") == 0 ||
                             g_ascii_strcasecmp(file_path, "yaml") == 0));

  highlight_code_region(&tc, &tags, text, 0, len, hash_comments);
}
