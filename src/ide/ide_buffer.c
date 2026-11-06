#include "ide_buffer.h"

#include <ctype.h>
#include <string.h>

struct _IdeBuffer {
  GtkSourceBuffer          *source_buf;
  GFile                    *file;
  char                     *file_path;
  char                     *title;
  char                     *scheme_id;
  char                     *disk_baseline;
  char                     *agent_snapshot;
  GFileMonitor             *monitor;
  gulong                    monitor_handler_id;
  gulong                    changed_handler_id;
  gboolean                  saving_internally;
  gboolean                  external_reloaded;
  gboolean                  collision;
  IdeBufferExternalChangeCb external_cb;
  gpointer                  external_cb_data;
  GHashTable               *breakpoints;
  GPtrArray                *diagnostics;
  guint                     dap_pc_line;
  IdeDiffState             *line_diffs;
  gsize                     line_diffs_len;
};

void
ide_diagnostic_free(gpointer data)
{
  IdeDiagnostic *d = data;
  if (d == NULL) {
    return;
  }
  g_free(d->message);
  g_free(d->source);
  g_free(d);
}

guint
ide_utf8_byte_to_utf16_col(const char *line_utf8, gsize byte_offset)
{
  if (line_utf8 == NULL || byte_offset == 0) {
    return 0;
  }
  gsize max_len = strlen(line_utf8);
  if (byte_offset > max_len) {
    byte_offset = max_len;
  }

  guint utf16_units = 0;
  const char *p = line_utf8;
  const char *end = line_utf8 + byte_offset;

  while (p < end && *p != '\0') {
    gunichar ch = g_utf8_get_char_validated(p, end - p);
    if (ch == (gunichar)-1 || ch == (gunichar)-2) {
      utf16_units++;
      p++;
      continue;
    }
    utf16_units += (ch > 0xFFFF) ? 2 : 1;
    p = g_utf8_next_char(p);
  }
  return utf16_units;
}

gsize
ide_utf16_col_to_utf8_byte(const char *line_utf8, guint utf16_col)
{
  if (line_utf8 == NULL || utf16_col == 0) {
    return 0;
  }

  guint cur_utf16 = 0;
  const char *p = line_utf8;

  while (*p != '\0' && *p != '\n' && *p != '\r' && cur_utf16 < utf16_col) {
    gunichar ch = g_utf8_get_char_validated(p, -1);
    if (ch == (gunichar)-1 || ch == (gunichar)-2) {
      cur_utf16++;
      p++;
      continue;
    }
    guint step = (ch > 0xFFFF) ? 2 : 1;
    if (cur_utf16 + step > utf16_col) {
      break;
    }
    cur_utf16 += step;
    p = g_utf8_next_char(p);
  }
  return (gsize)(p - line_utf8);
}

static gboolean
find_balanced_pair(const char *text,
                   gsize       len,
                   gsize       cursor,
                   char        open_ch,
                   char        close_ch,
                   gboolean    inside,
                   gsize      *out_start,
                   gsize      *out_end)
{
  /* If cursor is directly on open_ch, scan forward from cursor */
  gssize open_pos = -1;
  int depth = 0;

  if (cursor < len && text[cursor] == open_ch) {
    open_pos = (gssize)cursor;
  } else {
    for (gssize i = (gssize)MIN(cursor, len > 0 ? len - 1 : 0); i >= 0; i--) {
      if (text[i] == close_ch && (gsize)i != cursor) {
        depth++;
      } else if (text[i] == open_ch) {
        if (depth == 0) {
          open_pos = i;
          break;
        }
        depth--;
      }
    }
  }

  /* If not enclosing cursor, search forward on current or following lines */
  if (open_pos < 0) {
    for (gsize i = cursor; i < len; i++) {
      if (text[i] == open_ch) {
        open_pos = (gssize)i;
        break;
      }
    }
  }
  if (open_pos < 0) {
    return FALSE;
  }

  depth = 0;
  gssize close_pos = -1;
  gboolean in_str = FALSE;
  char str_delim = '\0';

  for (gsize i = (gsize)open_pos; i < len; i++) {
    char c = text[i];
    if (in_str) {
      if (c == '\\' && i + 1 < len) {
        i++;
        continue;
      }
      if (c == str_delim) {
        in_str = FALSE;
      }
      continue;
    }
    if (c == '"' || c == '\'' || c == '`') {
      in_str = TRUE;
      str_delim = c;
      continue;
    }
    if (c == open_ch) {
      depth++;
    } else if (c == close_ch) {
      depth--;
      if (depth == 0) {
        close_pos = (gssize)i;
        break;
      }
    }
  }

  if (close_pos < 0) {
    return FALSE;
  }

  if (inside) {
    *out_start = (gsize)open_pos + 1;
    *out_end = (gsize)close_pos;
  } else {
    *out_start = (gsize)open_pos;
    *out_end = (gsize)close_pos + 1;
  }
  return TRUE;
}

static gboolean
find_quote_object(const char *text,
                  gsize       len,
                  gsize       cursor,
                  gboolean    inside,
                  gsize      *out_start,
                  gsize      *out_end)
{
  /* Find current line boundaries */
  gsize line_start = cursor;
  while (line_start > 0 && text[line_start - 1] != '\n') {
    line_start--;
  }
  gsize line_end = cursor;
  while (line_end < len && text[line_end] != '\n') {
    line_end++;
  }

  for (gsize i = line_start; i < line_end; i++) {
    char q = text[i];
    if (q != '"' && q != '\'' && q != '`') {
      continue;
    }
    gsize open_idx = i;
    gsize close_idx = open_idx + 1;
    while (close_idx < line_end) {
      if (text[close_idx] == '\\' && close_idx + 1 < line_end) {
        close_idx += 2;
        continue;
      }
      if (text[close_idx] == q) {
        break;
      }
      close_idx++;
    }
    if (close_idx < line_end && text[close_idx] == q) {
      if (cursor <= close_idx) {
        *out_start = inside ? (open_idx + 1) : open_idx;
        *out_end = inside ? close_idx : (close_idx + 1);
        return TRUE;
      }
      i = close_idx;
    }
  }
  return FALSE;
}

static gboolean
find_function_or_class_object(const char *text,
                              gsize       len,
                              gsize       cursor,
                              gboolean    inside,
                              gsize      *out_start,
                              gsize      *out_end)
{
  gsize body_start = 0;
  gsize body_end = 0;
  if (!find_balanced_pair(text, len, cursor, '{', '}', inside, &body_start, &body_end)) {
    return FALSE;
  }
  if (inside) {
    /* Trim leading newline and trailing whitespace before closing brace if multi-line */
    if (body_start < body_end && text[body_start] == '\n') {
      body_start++;
    }
    *out_start = body_start;
    *out_end = body_end;
    return TRUE;
  }

  /* For 'around' (m a f / m a c), extend start backward to the beginning of the signature/declaration */
  gsize sig_start = body_start;
  while (sig_start > 0 && (text[sig_start - 1] == ' ' || text[sig_start - 1] == '\t' ||
                           text[sig_start - 1] == '\n' || text[sig_start - 1] == '\r')) {
    sig_start--;
  }
  while (sig_start > 0 && text[sig_start - 1] != '\n' && text[sig_start - 1] != ';' &&
         text[sig_start - 1] != '}') {
    sig_start--;
  }
  /* Also include a return-type line directly above if it's C-style (e.g. `static void\nfunc_name(...)`) */
  if (sig_start > 1 && text[sig_start - 1] == '\n') {
    gsize prev_end = sig_start - 1;
    gsize prev_start = prev_end;
    while (prev_start > 0 && text[prev_start - 1] != '\n') {
      prev_start--;
    }
    gboolean has_ident = FALSE;
    gboolean ends_with_semi_or_brace = FALSE;
    for (gsize i = prev_start; i < prev_end; i++) {
      if (g_ascii_isalnum(text[i]) || text[i] == '_') {
        has_ident = TRUE;
      }
      if (text[i] == ';' || text[i] == '}' || text[i] == '{' || text[i] == '#') {
        ends_with_semi_or_brace = TRUE;
      }
    }
    if (has_ident && !ends_with_semi_or_brace) {
      sig_start = prev_start;
    }
  }

  *out_start = sig_start;
  *out_end = (body_end < len && text[body_end] == '\n') ? (body_end + 1) : body_end;
  return TRUE;
}

static gboolean
find_statement_or_paragraph(const char *text,
                            gsize       len,
                            gsize       cursor,
                            gboolean    inside,
                            gsize      *out_start,
                            gsize      *out_end)
{
  if (len == 0) {
    return FALSE;
  }
  gsize p = MIN(cursor, len - 1);
  gsize start = p;
  while (start > 0) {
    if (text[start - 1] == '\n' && start >= 2 && text[start - 2] == '\n') {
      break;
    }
    start--;
  }
  gsize end = p;
  while (end < len) {
    if (text[end] == '\n' && (end + 1 >= len || text[end + 1] == '\n')) {
      if (!inside && end + 1 < len) {
        end += 2;
      } else {
        end += 1;
      }
      break;
    }
    end++;
  }
  *out_start = start;
  *out_end = end;
  return (*out_end > *out_start);
}

gboolean
ide_find_text_object(const char *text,
                     gsize       cursor_byte,
                     gboolean    inside,
                     char        target,
                     gsize      *out_start_byte,
                     gsize      *out_end_byte)
{
  g_return_val_if_fail(out_start_byte != NULL, FALSE);
  g_return_val_if_fail(out_end_byte != NULL, FALSE);

  if (text == NULL || text[0] == '\0') {
    return FALSE;
  }
  gsize len = strlen(text);
  if (cursor_byte > len) {
    cursor_byte = len;
  }

  switch (target) {
    case 'b':
    case '{':
    case '}':
      return find_balanced_pair(text, len, cursor_byte, '{', '}', inside, out_start_byte, out_end_byte);
    case 'a':
    case '[':
    case ']':
      return find_balanced_pair(text, len, cursor_byte, '[', ']', inside, out_start_byte, out_end_byte);
    case 'p':
    case '(':
    case ')':
      return find_balanced_pair(text, len, cursor_byte, '(', ')', inside, out_start_byte, out_end_byte);
    case 'q':
    case '"':
    case '\'':
      return find_quote_object(text, len, cursor_byte, inside, out_start_byte, out_end_byte);
    case 'f':
    case 'c':
      return find_function_or_class_object(text, len, cursor_byte, inside, out_start_byte, out_end_byte);
    case 's':
      return find_statement_or_paragraph(text, len, cursor_byte, inside, out_start_byte, out_end_byte);
    default:
      return FALSE;
  }
}

IdeDiffState *
ide_compute_line_diffs(const char *old_text,
                       const char *new_text,
                       gsize      *out_line_count)
{
  g_return_val_if_fail(out_line_count != NULL, NULL);

  if (new_text == NULL) {
    *out_line_count = 0;
    return NULL;
  }

  g_auto(GStrv) new_lines = g_strsplit(new_text, "\n", -1);
  gsize n_new = g_strv_length(new_lines);
  if (n_new == 0) {
    n_new = 1;
  }
  *out_line_count = n_new;

  IdeDiffState *states = g_new0(IdeDiffState, n_new);
  if (old_text == NULL) {
    return states;
  }

  g_auto(GStrv) old_lines = g_strsplit(old_text, "\n", -1);
  gsize n_old = g_strv_length(old_lines);

  /* Fast two-pointer + lookahead line comparison */
  gsize i = 0;
  gsize j = 0;
  while (i < n_old && j < n_new) {
    if (g_strcmp0(old_lines[i], new_lines[j]) == 0) {
      states[j] = IDE_DIFF_NONE;
      i++;
      j++;
      continue;
    }

    /* Check if new_lines[j..j+k] was inserted before old_lines[i] */
    gssize found_new = -1;
    for (gsize k = 1; k <= 8 && (j + k) < n_new; k++) {
      if (g_strcmp0(old_lines[i], new_lines[j + k]) == 0) {
        found_new = (gssize)k;
        break;
      }
    }
    if (found_new > 0) {
      for (gssize k = 0; k < found_new; k++) {
        states[j + k] = IDE_DIFF_ADDED;
      }
      j += (gsize)found_new;
      continue;
    }

    /* Check if old_lines[i..i+k] was deleted before new_lines[j] */
    gssize found_old = -1;
    for (gsize k = 1; k <= 8 && (i + k) < n_old; k++) {
      if (g_strcmp0(old_lines[i + k], new_lines[j]) == 0) {
        found_old = (gssize)k;
        break;
      }
    }
    if (found_old > 0) {
      states[j] = IDE_DIFF_DELETED;
      i += (gsize)found_old + 1;
      j++;
      continue;
    }

    /* Otherwise line j was modified in-place */
    states[j] = IDE_DIFF_MODIFIED;
    i++;
    j++;
  }

  while (j < n_new) {
    states[j++] = IDE_DIFF_ADDED;
  }

  return states;
}

char *
ide_format_unified_diff(const char *rel_path,
                        const char *old_text,
                        const char *new_text)
{
  const char *label = (rel_path != NULL && rel_path[0] != '\0') ? rel_path : "buffer";
  if (g_strcmp0(old_text ? old_text : "", new_text ? new_text : "") == 0) {
    return g_strdup("");
  }

  g_auto(GStrv) old_lines = g_strsplit(old_text ? old_text : "", "\n", -1);
  g_auto(GStrv) new_lines = g_strsplit(new_text ? new_text : "", "\n", -1);
  gsize n_old = g_strv_length(old_lines);
  gsize n_new = g_strv_length(new_lines);

  GString *out = g_string_new(NULL);
  g_string_append_printf(out, "--- a/%s\n+++ b/%s\n", label, label);

  gsize i = 0;
  gsize j = 0;
  while (i < n_old || j < n_new) {
    if (i < n_old && j < n_new && g_strcmp0(old_lines[i], new_lines[j]) == 0) {
      i++;
      j++;
      continue;
    }

    gsize hunk_old_start = i + 1;
    gsize hunk_new_start = j + 1;
    GString *body = g_string_new(NULL);
    gsize old_cnt = 0;
    gsize new_cnt = 0;

    if (i > 0 && j > 0) {
      g_string_append_printf(body, " %s\n", old_lines[i - 1]);
      hunk_old_start = i;
      hunk_new_start = j;
      old_cnt++;
      new_cnt++;
    }

    while (i < n_old || j < n_new) {
      if (i < n_old && j < n_new && g_strcmp0(old_lines[i], new_lines[j]) == 0) {
        g_string_append_printf(body, " %s\n", old_lines[i]);
        old_cnt++;
        new_cnt++;
        i++;
        j++;
        break;
      }

      /* Check if new_lines[j..j+k] was inserted before old_lines[i] */
      gssize sync_new = -1;
      if (i < n_old) {
        for (gsize k = 1; k <= 8 && (j + k) < n_new; k++) {
          if (g_strcmp0(old_lines[i], new_lines[j + k]) == 0) {
            sync_new = (gssize)k;
            break;
          }
        }
      }
      if (sync_new > 0) {
        for (gssize k = 0; k < sync_new; k++) {
          g_string_append_printf(body, "+%s\n", new_lines[j]);
          new_cnt++;
          j++;
        }
        continue;
      }

      /* Check if old_lines[i..i+k] was deleted before new_lines[j] */
      gssize sync_old = -1;
      if (j < n_new) {
        for (gsize k = 1; k <= 8 && (i + k) < n_old; k++) {
          if (g_strcmp0(old_lines[i + k], new_lines[j]) == 0) {
            sync_old = (gssize)k;
            break;
          }
        }
      }
      if (sync_old > 0) {
        for (gssize k = 0; k < sync_old; k++) {
          g_string_append_printf(body, "-%s\n", old_lines[i]);
          old_cnt++;
          i++;
        }
        continue;
      }

      if (i < n_old) {
        g_string_append_printf(body, "-%s\n", old_lines[i]);
        old_cnt++;
        i++;
      }
      if (j < n_new) {
        g_string_append_printf(body, "+%s\n", new_lines[j]);
        new_cnt++;
        j++;
      }
    }

    g_string_append_printf(out,
                           "@@ -%" G_GSIZE_FORMAT ",%" G_GSIZE_FORMAT " +%" G_GSIZE_FORMAT ",%" G_GSIZE_FORMAT " @@\n%s",
                           hunk_old_start,
                           old_cnt,
                           hunk_new_start,
                           new_cnt,
                           body->str);
    g_string_free(body, TRUE);
  }

  return g_string_free(out, FALSE);
}

char *
ide_resolve_alternate_path(const char *file_path)
{
  if (file_path == NULL || file_path[0] == '\0') {
    return NULL;
  }

  /* Go: foo_test.go <-> foo.go */
  if (g_str_has_suffix(file_path, "_test.go")) {
    gsize base_len = strlen(file_path) - strlen("_test.go");
    g_autofree char *prefix = g_strndup(file_path, base_len);
    return g_strconcat(prefix, ".go", NULL);
  }
  if (g_str_has_suffix(file_path, ".go")) {
    gsize base_len = strlen(file_path) - strlen(".go");
    g_autofree char *prefix = g_strndup(file_path, base_len);
    return g_strconcat(prefix, "_test.go", NULL);
  }

  /* C/C++ implementation -> header */
  static const char *const kImplExts[] = { ".c", ".cpp", ".cc", ".cxx", NULL };
  static const char *const kHdrExts[] = { ".h", ".hpp", ".hh", NULL };

  for (gsize i = 0; kImplExts[i] != NULL; i++) {
    if (g_str_has_suffix(file_path, kImplExts[i])) {
      gsize base_len = strlen(file_path) - strlen(kImplExts[i]);
      g_autofree char *prefix = g_strndup(file_path, base_len);
      for (gsize h = 0; kHdrExts[h] != NULL; h++) {
        char *cand = g_strconcat(prefix, kHdrExts[h], NULL);
        if (g_file_test(cand, G_FILE_TEST_EXISTS)) {
          return cand;
        }
        g_free(cand);
      }
      return g_strconcat(prefix, ".h", NULL);
    }
  }

  /* C/C++ header -> implementation */
  for (gsize h = 0; kHdrExts[h] != NULL; h++) {
    if (g_str_has_suffix(file_path, kHdrExts[h])) {
      gsize base_len = strlen(file_path) - strlen(kHdrExts[h]);
      g_autofree char *prefix = g_strndup(file_path, base_len);
      for (gsize i = 0; kImplExts[i] != NULL; i++) {
        char *cand = g_strconcat(prefix, kImplExts[i], NULL);
        if (g_file_test(cand, G_FILE_TEST_EXISTS)) {
          return cand;
        }
        g_free(cand);
      }
      return g_strconcat(prefix, ".c", NULL);
    }
  }

  return NULL;
}

gboolean
ide_save_file_atomic(const char  *path,
                     const char  *contents,
                     gssize       len,
                     GError     **error)
{
  g_return_val_if_fail(path != NULL && path[0] != '\0', FALSE);
  if (contents == NULL) {
    contents = "";
    len = 0;
  } else if (len < 0) {
    len = (gssize)strlen(contents);
  }

  g_autoptr(GFile) file = g_file_new_for_path(path);
  return g_file_replace_contents(file,
                                 contents,
                                 (gsize)len,
                                 NULL,
                                 FALSE,
                                 G_FILE_CREATE_NONE,
                                 NULL,
                                 NULL,
                                 error);
}

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
init_and_clear_syn_tags(GtkTextBuffer  *tbuf,
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

  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(tbuf, &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->kw,   &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->ctrl, &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->fn,   &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->type, &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->mac,  &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->var,  &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->str,  &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->esc,  &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->num,  &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->cmt,  &start, &end);
  gtk_text_buffer_remove_tag(tbuf, out->op,   &start, &end);
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
  init_and_clear_syn_tags(tbuf, scheme_id, &tags);

  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(tbuf, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(tbuf, &start, &end, FALSE);
  if (text == NULL || text[0] == '\0') {
    return;
  }
  gsize len = strlen(text);

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

static void
on_source_buffer_text_changed(GtkTextBuffer *tbuf, gpointer user_data)
{
  (void)tbuf;
  IdeBuffer *buf = user_data;
  if (buf == NULL || buf->source_buf == NULL) {
    return;
  }
  ide_highlight_source_buffer(buf->source_buf, buf->file_path, buf->scheme_id);
}

void
ide_buffer_set_style_scheme(IdeBuffer *buf, const char *scheme_id)
{
  if (buf == NULL || buf->source_buf == NULL) {
    return;
  }

  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  static gboolean search_path_added = FALSE;
  if (!search_path_added) {
    search_path_added = TRUE;
    gtk_source_style_scheme_manager_append_search_path(sm, KAJO_SOURCE_DIR "/data/ide");
    g_autofree char *user_ide_dir = g_build_filename(g_get_user_data_dir(), "kajo", "ide", NULL);
    gtk_source_style_scheme_manager_append_search_path(sm, user_ide_dir);
  }

  const char *wanted = (scheme_id && scheme_id[0] != '\0') ? scheme_id : "kajo-metro-dark";
  g_free(buf->scheme_id);
  buf->scheme_id = g_strdup(wanted);

  GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(sm, wanted);
  if (scheme == NULL) {
    scheme = gtk_source_style_scheme_manager_get_scheme(sm, "kajo-metro-dark");
  }
  if (scheme == NULL) {
    scheme = gtk_source_style_scheme_manager_get_scheme(sm, "Adwaita-dark");
  }
  if (scheme == NULL) {
    scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic-dark");
  }
  if (scheme != NULL) {
    gtk_source_buffer_set_style_scheme(buf->source_buf, scheme);
  }

  ide_highlight_source_buffer(buf->source_buf, buf->file_path, buf->scheme_id);
}

static void
ide_buffer_apply_style_and_language(IdeBuffer *buf)
{
  if (buf->source_buf == NULL) {
    return;
  }

  if (buf->file_path != NULL) {
    GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
    GtkSourceLanguage *lang = gtk_source_language_manager_guess_language(lm, buf->file_path, NULL);
    if (lang != NULL) {
      gtk_source_buffer_set_language(buf->source_buf, lang);
    }
  }

  ide_buffer_set_style_scheme(buf, buf->scheme_id ? buf->scheme_id : "kajo-metro-dark");
}

static void
on_file_monitor_changed(GFileMonitor      *monitor,
                        GFile             *file,
                        GFile             *other_file,
                        GFileMonitorEvent  event_type,
                        gpointer           user_data)
{
  IdeBuffer *buf = user_data;
  if (buf->saving_internally) {
    return;
  }
  if (event_type != G_FILE_MONITOR_EVENT_CHANGED &&
      event_type != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
      event_type != G_FILE_MONITOR_EVENT_CREATED) {
    return;
  }

  if (ide_buffer_get_modified(buf)) {
    /* Dirty buffer protection: never overwrite unsaved user edits */
    buf->collision = TRUE;
    if (buf->external_cb != NULL) {
      buf->external_cb(buf, FALSE, buf->external_cb_data);
    }
    return;
  }

  /* Record pre-agent snapshot if not already recorded before hot-reloading */
  if (buf->agent_snapshot == NULL && buf->disk_baseline != NULL) {
    buf->agent_snapshot = g_strdup(buf->disk_baseline);
  }

  if (ide_buffer_load_from_disk(buf, NULL)) {
    buf->external_reloaded = TRUE;
    buf->collision = FALSE;
    if (buf->external_cb != NULL) {
      buf->external_cb(buf, TRUE, buf->external_cb_data);
    }
  }
}

IdeBuffer *
ide_buffer_new(const char *file_path)
{
  IdeBuffer *buf = g_new0(IdeBuffer, 1);
  buf->source_buf = gtk_source_buffer_new(NULL);
  buf->scheme_id = g_strdup("kajo-metro-dark");
  gtk_source_buffer_set_highlight_syntax(buf->source_buf, FALSE);
  gtk_source_buffer_set_highlight_matching_brackets(buf->source_buf, TRUE);
  buf->changed_handler_id = g_signal_connect(buf->source_buf,
                                             "changed",
                                             G_CALLBACK(on_source_buffer_text_changed),
                                             buf);

  buf->breakpoints = g_hash_table_new(g_direct_hash, g_direct_equal);
  buf->diagnostics = g_ptr_array_new_with_free_func(ide_diagnostic_free);

  if (file_path != NULL && file_path[0] != '\0') {
    buf->file = g_file_new_for_path(file_path);
    buf->file_path = g_file_get_path(buf->file);
    if (buf->file_path == NULL) {
      buf->file_path = g_strdup(file_path);
    }
    buf->title = g_path_get_basename(buf->file_path);
    buf->monitor = g_file_monitor_file(buf->file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (buf->monitor != NULL) {
      buf->monitor_handler_id = g_signal_connect(buf->monitor,
                                                 "changed",
                                                 G_CALLBACK(on_file_monitor_changed),
                                                 buf);
    }
  } else {
    buf->title = g_strdup("[No Name]");
  }

  ide_buffer_apply_style_and_language(buf);
  return buf;
}

void
ide_buffer_free(IdeBuffer *buf)
{
  if (buf == NULL) {
    return;
  }
  if (buf->monitor != NULL) {
    if (buf->monitor_handler_id != 0) {
      g_signal_handler_disconnect(buf->monitor, buf->monitor_handler_id);
    }
    g_file_monitor_cancel(buf->monitor);
    g_clear_object(&buf->monitor);
  }
  if (buf->source_buf != NULL && buf->changed_handler_id != 0) {
    g_signal_handler_disconnect(buf->source_buf, buf->changed_handler_id);
    buf->changed_handler_id = 0;
  }
  g_clear_object(&buf->file);
  g_clear_object(&buf->source_buf);
  g_free(buf->file_path);
  g_free(buf->title);
  g_free(buf->scheme_id);
  g_free(buf->disk_baseline);
  g_free(buf->agent_snapshot);
  g_clear_pointer(&buf->breakpoints, g_hash_table_destroy);
  g_clear_pointer(&buf->diagnostics, g_ptr_array_unref);
  g_free(buf->line_diffs);
  g_free(buf);
}

GtkSourceBuffer *
ide_buffer_get_source_buffer(IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, NULL);
  return buf->source_buf;
}

const char *
ide_buffer_get_file_path(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, NULL);
  return buf->file_path;
}

const char *
ide_buffer_get_title(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, "[No Name]");
  return buf->title;
}

char *
ide_buffer_get_text(IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL && buf->source_buf != NULL, g_strdup(""));
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buf->source_buf), &start, &end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(buf->source_buf), &start, &end, FALSE);
}

void
ide_buffer_set_text(IdeBuffer *buf, const char *text)
{
  g_return_if_fail(buf != NULL && buf->source_buf != NULL);
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(buf->source_buf);
  GtkTextIter start, end;
  gtk_text_buffer_begin_user_action(tbuf);
  gtk_text_buffer_get_bounds(tbuf, &start, &end);
  gtk_text_buffer_delete(tbuf, &start, &end);
  gtk_text_buffer_get_start_iter(tbuf, &start);
  gtk_text_buffer_insert(tbuf, &start, text ? text : "", -1);
  gtk_text_buffer_end_user_action(tbuf);
  ide_buffer_recompute_diffs(buf);
}

gboolean
ide_buffer_load_from_disk(IdeBuffer *buf, GError **error)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  if (buf->file_path == NULL) {
    return FALSE;
  }

  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(buf->file_path, &contents, &len, error)) {
    return FALSE;
  }

  /* Ensure valid UTF-8 for GtkTextBuffer */
  g_autofree char *valid_utf8 = NULL;
  const char *text_to_set = contents;
  if (!g_utf8_validate(contents, (gssize)len, NULL)) {
    valid_utf8 = g_utf8_make_valid(contents, (gssize)len);
    text_to_set = valid_utf8;
  }

  gtk_text_buffer_begin_irreversible_action(GTK_TEXT_BUFFER(buf->source_buf));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buf->source_buf), text_to_set, -1);
  gtk_text_buffer_end_irreversible_action(GTK_TEXT_BUFFER(buf->source_buf));
  gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(buf->source_buf), FALSE);

  g_free(buf->disk_baseline);
  buf->disk_baseline = g_strdup(text_to_set);
  buf->collision = FALSE;
  ide_buffer_recompute_diffs(buf);
  return TRUE;
}

gboolean
ide_buffer_save(IdeBuffer *buf, GError **error)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  if (buf->file_path == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME, "Buffer has no file path");
    return FALSE;
  }

  g_autofree char *text = ide_buffer_get_text(buf);
  buf->saving_internally = TRUE;
  gboolean ok = ide_save_file_atomic(buf->file_path, text, -1, error);
  buf->saving_internally = FALSE;

  if (ok) {
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(buf->source_buf), FALSE);
    g_free(buf->disk_baseline);
    buf->disk_baseline = g_strdup(text);
    buf->collision = FALSE;
    ide_buffer_recompute_diffs(buf);
  }
  return ok;
}

gboolean
ide_buffer_get_modified(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL && buf->source_buf != NULL, FALSE);
  return gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(buf->source_buf));
}

gboolean
ide_buffer_get_external_reloaded(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  return buf->external_reloaded;
}

void
ide_buffer_clear_external_reloaded(IdeBuffer *buf)
{
  g_return_if_fail(buf != NULL);
  buf->external_reloaded = FALSE;
}

gboolean
ide_buffer_get_collision(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  return buf->collision;
}

void
ide_buffer_set_external_change_callback(IdeBuffer                *buf,
                                        IdeBufferExternalChangeCb cb,
                                        gpointer                  user_data)
{
  g_return_if_fail(buf != NULL);
  buf->external_cb = cb;
  buf->external_cb_data = user_data;
}

gboolean
ide_buffer_has_breakpoint(const IdeBuffer *buf, guint line_1based)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  return g_hash_table_contains(buf->breakpoints, GUINT_TO_POINTER(line_1based));
}

gboolean
ide_buffer_toggle_breakpoint(IdeBuffer *buf, guint line_1based)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  if (line_1based == 0) {
    return FALSE;
  }
  gpointer key = GUINT_TO_POINTER(line_1based);
  if (g_hash_table_contains(buf->breakpoints, key)) {
    g_hash_table_remove(buf->breakpoints, key);
    return FALSE;
  }
  g_hash_table_add(buf->breakpoints, key);
  return TRUE;
}

static gint
cmp_guint(gconstpointer a, gconstpointer b)
{
  guint ua = *(const guint *)a;
  guint ub = *(const guint *)b;
  return (ua > ub) - (ua < ub);
}

GArray *
ide_buffer_get_breakpoints(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, NULL);
  GArray *arr = g_array_new(FALSE, FALSE, sizeof(guint));
  GHashTableIter iter;
  gpointer key = NULL;
  g_hash_table_iter_init(&iter, buf->breakpoints);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    guint line = GPOINTER_TO_UINT(key);
    g_array_append_val(arr, line);
  }
  g_array_sort(arr, cmp_guint);
  return arr;
}

void
ide_buffer_clear_diagnostics(IdeBuffer *buf)
{
  g_return_if_fail(buf != NULL);
  g_ptr_array_set_size(buf->diagnostics, 0);
}

void
ide_buffer_add_diagnostic(IdeBuffer      *buf,
                          guint           line_1based,
                          guint           col_1based,
                          IdeDiagSeverity severity,
                          const char     *message,
                          const char     *source)
{
  g_return_if_fail(buf != NULL);
  IdeDiagnostic *d = g_new0(IdeDiagnostic, 1);
  d->line = MAX(1u, line_1based);
  d->col = MAX(1u, col_1based);
  d->severity = severity;
  d->message = g_strdup(message ? message : "");
  d->source = g_strdup(source ? source : "lsp");
  g_ptr_array_add(buf->diagnostics, d);
}

const GPtrArray *
ide_buffer_get_diagnostics(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, NULL);
  return buf->diagnostics;
}

const IdeDiagnostic *
ide_buffer_get_line_diagnostic(const IdeBuffer *buf, guint line_1based)
{
  g_return_val_if_fail(buf != NULL, NULL);
  const IdeDiagnostic *best = NULL;
  for (guint i = 0; i < buf->diagnostics->len; i++) {
    const IdeDiagnostic *d = g_ptr_array_index(buf->diagnostics, i);
    if (d->line == line_1based) {
      if (best == NULL || d->severity > best->severity) {
        best = d;
      }
    }
  }
  return best;
}

void
ide_buffer_set_dap_pc_line(IdeBuffer *buf, guint line_1based)
{
  g_return_if_fail(buf != NULL);
  buf->dap_pc_line = line_1based;
}

guint
ide_buffer_get_dap_pc_line(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, 0);
  return buf->dap_pc_line;
}

void
ide_buffer_recompute_diffs(IdeBuffer *buf)
{
  g_return_if_fail(buf != NULL);
  g_autofree char *cur = ide_buffer_get_text(buf);
  const char *base = (buf->agent_snapshot != NULL) ? buf->agent_snapshot : buf->disk_baseline;

  g_free(buf->line_diffs);
  buf->line_diffs = ide_compute_line_diffs(base, cur, &buf->line_diffs_len);

  if (buf->agent_snapshot != NULL && buf->line_diffs != NULL) {
    for (gsize i = 0; i < buf->line_diffs_len; i++) {
      if (buf->line_diffs[i] != IDE_DIFF_NONE) {
        buf->line_diffs[i] = IDE_DIFF_AGENT;
      }
    }
  }
}

IdeDiffState
ide_buffer_get_line_diff(const IdeBuffer *buf, guint line_1based)
{
  g_return_val_if_fail(buf != NULL, IDE_DIFF_NONE);
  if (line_1based == 0 || buf->line_diffs == NULL || (gsize)line_1based > buf->line_diffs_len) {
    return IDE_DIFF_NONE;
  }
  return buf->line_diffs[line_1based - 1];
}

void
ide_buffer_record_agent_snapshot(IdeBuffer *buf)
{
  g_return_if_fail(buf != NULL);
  if (buf->agent_snapshot == NULL) {
    buf->agent_snapshot = ide_buffer_get_text(buf);
  }
}

gboolean
ide_buffer_has_agent_changes(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  if (buf->agent_snapshot == NULL) {
    return FALSE;
  }
  g_autofree char *cur = ide_buffer_get_text((IdeBuffer *)buf);
  return g_strcmp0(buf->agent_snapshot, cur) != 0;
}

const char *
ide_buffer_get_agent_snapshot(const IdeBuffer *buf)
{
  g_return_val_if_fail(buf != NULL, NULL);
  return buf->agent_snapshot;
}

void
ide_buffer_accept_agent_changes(IdeBuffer *buf)
{
  g_return_if_fail(buf != NULL);
  g_clear_pointer(&buf->agent_snapshot, g_free);
  ide_buffer_recompute_diffs(buf);
}

gboolean
ide_buffer_revert_agent_changes(IdeBuffer *buf, GError **error)
{
  g_return_val_if_fail(buf != NULL, FALSE);
  if (buf->agent_snapshot == NULL) {
    return TRUE;
  }
  ide_buffer_set_text(buf, buf->agent_snapshot);
  g_clear_pointer(&buf->agent_snapshot, g_free);
  if (buf->file_path != NULL) {
    return ide_buffer_save(buf, error);
  }
  return TRUE;
}
