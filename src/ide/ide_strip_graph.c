#include "ide_strip_graph_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void
ide_strip_graph_sym_def_free(gpointer data)
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

char *
ide_strip_graph_canonicalize_dir(const char *path)
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

  g_autofree char *canon_root = ide_strip_graph_canonicalize_dir(workspace_root);
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

void
ide_strip_graph_line_index_init(LineIndex *li, const char *text, gsize len)
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

void
ide_strip_graph_line_index_clear(LineIndex *li)
{
  g_clear_pointer(&li->starts, g_array_unref);
  li->len = 0;
}

guint
ide_strip_graph_line_index_count(const LineIndex *li)
{
  return (li != NULL && li->starts != NULL && li->starts->len > 0) ? li->starts->len : 1u;
}

guint
ide_strip_graph_line_index_line_for_byte(const LineIndex *li, gsize byte)
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

gsize
ide_strip_graph_line_index_byte_for_line(const LineIndex *li, guint line_1based)
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

gsize
ide_strip_graph_line_index_end_byte_for_line(const LineIndex *li, guint line_1based)
{
  if (li == NULL || li->starts == NULL || li->starts->len == 0) {
    return 0;
  }
  if (line_1based == 0 || line_1based >= li->starts->len) {
    return li->len;
  }
  return g_array_index(li->starts, gsize, line_1based);
}

void
ide_strip_graph_cached_file_text_free(gpointer data)
{
  CachedFileText *cf = data;
  if (cf == NULL) {
    return;
  }
  ide_strip_graph_line_index_clear(&cf->lines);
  g_free(cf->text);
  g_free(cf);
}

const CachedFileText *
ide_strip_graph_get_cached_text(GHashTable *cache, const char *file_path,
                                const char *override_path, const char *override_text,
                                gsize override_len)
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
  ide_strip_graph_line_index_init(&cf->lines, cf->text, cf->len);
  g_hash_table_insert(cache, g_strdup(file_path), cf);
  return cf;
}

/* ═══════════════════════════ Peek Reader ═══════════════════════════ */

static void
trim_trailing_blank_lines(GString *out)
{
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
}

char *
ide_strip_graph_slice_peek(const char *text, gsize len, const LineIndex *li,
                           guint start_line, guint max_lines)
{
  if (text == NULL || max_lines == 0) {
    return g_strdup("");
  }
  if (start_line == 0) {
    start_line = 1;
  }
  guint total = ide_strip_graph_line_index_count(li);
  if (start_line > total) {
    return g_strdup("");
  }
  guint end_line = MIN(total, start_line + max_lines - 1);
  gsize s = ide_strip_graph_line_index_byte_for_line(li, start_line);
  gsize e = ide_strip_graph_line_index_end_byte_for_line(li, end_line);
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
  trim_trailing_blank_lines(out);
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
    ide_strip_graph_line_index_init(&li, override_text, len);
    char *res = ide_strip_graph_slice_peek(override_text, len, &li, start_line, max_lines);
    ide_strip_graph_line_index_clear(&li);
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

  trim_trailing_blank_lines(out);
  return g_string_free(out, FALSE);
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
  ide_strip_graph_line_index_init(&li, contents, len);

  FileEntry *fe = g_new0(FileEntry, 1);
  fe->file_path = g_strdup(canon_path);
  fe->rel_path = make_rel_path(canon_path, g->workspace_root);
  fe->line_count = ide_strip_graph_line_index_count(&li);
  fe->defs = ide_strip_graph_extract_file_defs(fe->file_path, fe->rel_path, contents, len, &li);
  ide_strip_graph_line_index_clear(&li);

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
      g_autofree char *canon = ide_strip_graph_canonicalize_dir(full);
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
  g->workspace_root = ide_strip_graph_canonicalize_dir(workspace_root ? workspace_root : ".");
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
  char *next = ide_strip_graph_canonicalize_dir(workspace_root ? workspace_root : ".");
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
  g_autofree char *canon = ide_strip_graph_canonicalize_dir(file_path);
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

SymDef *
ide_strip_graph_resolve_symbol(IdeStripGraph *g, const char *name, const char *prefer_file,
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

/* ═══════════════════════════ IMPACT & DIFF Lenses ═══════════════════════════ */

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

GPtrArray *
ide_strip_graph_extract_impact_fallback(IdeStripGraph *g, const char *canon_file,
                                        const char *text, gsize len,
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
    const CachedFileText *cf = ide_strip_graph_get_cached_text(file_cache, fe->file_path,
                                                               canon_file, text, len);
    if (cf == NULL || cf->text == NULL || cf->len < tlen) {
      continue;
    }
    gboolean same_file = (canon_file != NULL && strcmp(fe->file_path, canon_file) == 0);
    gsize i = 0;
    while (i < cf->len) {
      if (ide_strip_graph_skip_comment_or_string_c(cf->text, cf->len, &i)) {
        continue;
      }
      if (ide_strip_graph_is_ident_start(cf->text[i])) {
        gsize ws = i;
        while (i < cf->len && ide_strip_graph_is_ident_char(cf->text[i])) {
          i++;
        }
        if (i - ws != tlen || strncmp(cf->text + ws, target_name, tlen) != 0) {
          continue;
        }
        guint ref_line = ide_strip_graph_line_index_line_for_byte(&cf->lines, ws);
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
            ide_strip_graph_looks_like_c_prototype(cf->text, cf->len, ws, i)) {
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
        e->peek_code = ide_strip_graph_slice_peek(cf->text, cf->len, &cf->lines,
                                                  ref_line, STRIP_DEFAULT_PEEK);
        e->child_count = caller
                             ? ide_strip_graph_compute_child_count(g, (SymDef *)caller, file_cache,
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
ide_strip_graph_extract_diff_targets(IdeStripGraph *g, GPtrArray *open_buffers)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_strip_graph_entry_free);
  if (g == NULL || open_buffers == NULL) {
    return out;
  }
  ide_strip_graph_ensure_indexed(g);

  g_autoptr(GHashTable) file_cache =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ide_strip_graph_cached_file_text_free);
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
    g_autofree char *canon = ide_strip_graph_canonicalize_dir(path);
    g_autofree char *rel = make_rel_path(canon, g->workspace_root);
    gboolean is_agent = ide_buffer_has_agent_changes(buf);
    gboolean is_dirty = ide_buffer_get_modified(buf);

    g_autofree char *text = ide_buffer_get_text(buf);
    if (text == NULL) {
      continue;
    }
    gsize len = strlen(text);
    LineIndex li;
    ide_strip_graph_line_index_init(&li, text, len);
    guint total = ide_strip_graph_line_index_count(&li);

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
      e->peek_code = ide_strip_graph_slice_peek(text, len, &li, s_line, STRIP_DEFAULT_PEEK);
      e->is_agent_modified = (ds == IDE_DIFF_AGENT || is_agent);
      e->is_dirty = is_dirty;
      e->is_fallback = FALSE;
      SymDef *def = ide_strip_graph_resolve_symbol(g, e->name, canon, kind == IDE_STRIP_SYM_TYPE);
      if (def != NULL) {
        e->child_count = ide_strip_graph_compute_child_count(g, def, file_cache, canon, text, len);
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
      e->peek_code = ide_strip_graph_slice_peek(text, len, &li, 1, STRIP_DEFAULT_PEEK);
      e->is_agent_modified = is_agent;
      e->is_dirty = is_dirty;
      e->is_fallback = FALSE;
      g_ptr_array_add(out, e);
    }
    ide_strip_graph_line_index_clear(&li);
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
  ide_strip_graph_line_index_init(&li, text, len);
  guint target_line = MIN(line0 + 1, ide_strip_graph_line_index_count(&li));
  gsize s = ide_strip_graph_line_index_byte_for_line(&li, target_line);
  gsize e = ide_strip_graph_line_index_end_byte_for_line(&li, target_line);
  ide_strip_graph_line_index_clear(&li);
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
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ide_strip_graph_cached_file_text_free);
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
    g_autofree char *canon = ide_strip_graph_canonicalize_dir(loc->path);
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
    SymDef *def = ide_strip_graph_resolve_symbol(g, e->name, canon, kind == IDE_STRIP_SYM_TYPE);
    if (def != NULL) {
      e->child_count = ide_strip_graph_compute_child_count(g, def, file_cache, NULL, NULL, 0);
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
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ide_strip_graph_cached_file_text_free);
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
    g_autofree char *canon = ide_strip_graph_canonicalize_dir(ce->path);
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

    SymDef *def = ide_strip_graph_resolve_symbol(g, ce->name, canon, e->kind == IDE_STRIP_SYM_TYPE);
    if (def != NULL) {
      e->end_line = def->end_line;
      e->child_count = ide_strip_graph_compute_child_count(g, def, file_cache, NULL, NULL, 0);
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
