#include "ide_lsp_private.h"

#include <string.h>

/* ═══════════════════════════ JSON & text-edit helpers ═══════════════════════════ */

const char *
ide_lsp_json_str(JsonObject *o, const char *name)
{
  return (o != NULL && json_object_has_member(o, name) &&
          JSON_NODE_HOLDS_VALUE(json_object_get_member(o, name)))
             ? json_object_get_string_member(o, name)
             : NULL;
}

JsonObject *
ide_lsp_json_obj_member(JsonObject *o, const char *name)
{
  if (o == NULL || !json_object_has_member(o, name)) {
    return NULL;
  }
  JsonNode *n = json_object_get_member(o, name);
  return (n != NULL && JSON_NODE_HOLDS_OBJECT(n)) ? json_node_get_object(n) : NULL;
}

static char *
extract_lsp_doc(JsonNode *dn)
{
  if (dn == NULL || JSON_NODE_HOLDS_NULL(dn)) {
    return NULL;
  }
  if (JSON_NODE_HOLDS_VALUE(dn)) {
    return g_strdup(json_node_get_string(dn));
  }
  if (JSON_NODE_HOLDS_OBJECT(dn)) {
    return g_strdup(ide_lsp_json_str(json_node_get_object(dn), "value"));
  }
  return NULL;
}

static const char *
symbol_kind_badge(gint kind)
{
  switch (kind) {
    case 1: return "F";                        /* File */
    case 2: case 3: case 4: return "m";        /* Module, Namespace, Package */
    case 5: case 10: case 11: case 23: return "T"; /* Class, Enum, Interface, Struct */
    case 6: case 9: case 12: return "ƒ";       /* Method, Constructor, Function */
    case 7: case 8: case 13: return "v";       /* Property, Field, Variable */
    case 14: case 22: return "c";              /* Constant, EnumMember */
    default: return "•";
  }
}

IdeLspTextEdit *
ide_lsp_text_edit_new(guint line_start, guint char_start, guint line_end, guint char_end,
                      const char *new_text)
{
  IdeLspTextEdit *e = g_new0(IdeLspTextEdit, 1);
  e->line_start = line_start;
  e->char_start = char_start;
  e->line_end = line_end;
  e->char_end = char_end;
  e->new_text = g_strdup(new_text ? new_text : "");
  return e;
}

void
ide_lsp_text_edit_free(gpointer data)
{
  IdeLspTextEdit *e = data;
  if (e != NULL) {
    g_free(e->new_text);
    g_free(e);
  }
}

gboolean
ide_lsp_json_range_to_edit(JsonObject *range, const char *new_text, IdeLspTextEdit *out)
{
  if (range == NULL || !json_object_has_member(range, "start") ||
      !json_object_has_member(range, "end")) {
    return FALSE;
  }
  JsonObject *s = json_object_get_object_member(range, "start");
  JsonObject *e = json_object_get_object_member(range, "end");
  if (s == NULL || e == NULL) {
    return FALSE;
  }
  out->line_start = (guint)json_object_get_int_member_with_default(s, "line", 0);
  out->char_start = (guint)json_object_get_int_member_with_default(s, "character", 0);
  out->line_end = (guint)json_object_get_int_member_with_default(e, "line", 0);
  out->char_end = (guint)json_object_get_int_member_with_default(e, "character", 0);
  out->new_text = g_strdup(new_text ? new_text : "");
  return TRUE;
}

GPtrArray *
ide_lsp_parse_text_edits(JsonNode *edits_node)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);
  if (edits_node == NULL || !JSON_NODE_HOLDS_ARRAY(edits_node)) {
    return out;
  }
  JsonArray *arr = json_node_get_array(edits_node);
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonNode *n = json_array_get_element(arr, i);
    if (!JSON_NODE_HOLDS_OBJECT(n)) {
      continue;
    }
    JsonObject *o = json_node_get_object(n);
    IdeLspTextEdit tmp = { 0 };
    /* TextEdit {range,newText} or InsertReplaceEdit {insert,replace,newText} */
    JsonObject *range = json_object_has_member(o, "range")
                            ? json_object_get_object_member(o, "range")
                            : (json_object_has_member(o, "replace")
                                   ? json_object_get_object_member(o, "replace")
                                   : NULL);
    const char *text = json_object_has_member(o, "newText")
                           ? json_object_get_string_member(o, "newText")
                           : "";
    if (ide_lsp_json_range_to_edit(range, text, &tmp)) {
      IdeLspTextEdit *e = g_memdup2(&tmp, sizeof tmp);
      g_ptr_array_add(out, e);
    }
  }
  return out;
}

/* Byte offset of LSP position (line, UTF-16 character) in `text`, or -1. */
static gssize
lsp_pos_to_byte(const char *text, gsize len, guint line, guint character)
{
  gsize i = 0;
  guint cur = 0;
  while (cur < line) {
    const char *nl = memchr(text + i, '\n', len - i);
    if (nl == NULL) {
      return -1;
    }
    i = (gsize)(nl - text) + 1;
    cur++;
  }
  guint units = 0;
  while (i < len && text[i] != '\n' && units < character) {
    gunichar ch = g_utf8_get_char_validated(text + i, (gssize)(len - i));
    gsize step = 1;
    if (ch != (gunichar)-1 && ch != (gunichar)-2) {
      step = (gsize)(g_utf8_next_char(text + i) - (text + i));
      units += (ch >= 0x10000) ? 2 : 1;
    } else {
      units++;
    }
    i += step;
  }
  return (gssize)i;
}

gint
ide_lsp_edit_cmp(gconstpointer a, gconstpointer b)
{
  const IdeLspTextEdit *x = *(const IdeLspTextEdit *const *)a;
  const IdeLspTextEdit *y = *(const IdeLspTextEdit *const *)b;
  if (x->line_start != y->line_start) {
    return x->line_start < y->line_start ? -1 : 1;
  }
  if (x->char_start != y->char_start) {
    return x->char_start < y->char_start ? -1 : 1;
  }
  return 0;
}

char *
ide_lsp_apply_edits_to_string(const char *text, const GPtrArray *edits)
{
  g_return_val_if_fail(text != NULL, NULL);
  gsize len = strlen(text);
  if (edits == NULL || edits->len == 0) {
    return g_strdup(text);
  }

  g_autoptr(GPtrArray) sorted = g_ptr_array_new();
  for (guint i = 0; i < edits->len; i++) {
    g_ptr_array_add(sorted, g_ptr_array_index((GPtrArray *)edits, i));
  }
  g_ptr_array_sort(sorted, ide_lsp_edit_cmp);

  GString *out = g_string_new(NULL);
  gsize cursor = 0;
  for (guint i = 0; i < sorted->len; i++) {
    const IdeLspTextEdit *e = g_ptr_array_index(sorted, i);
    gssize s = lsp_pos_to_byte(text, len, e->line_start, e->char_start);
    gssize en = lsp_pos_to_byte(text, len, e->line_end, e->char_end);
    if (s < 0 || en < s || (gsize)s < cursor) {
      g_string_free(out, TRUE);
      return NULL;
    }
    g_string_append_len(out, text + cursor, (gssize)((gsize)s - cursor));
    g_string_append(out, e->new_text);
    cursor = (gsize)en;
  }
  g_string_append_len(out, text + cursor, (gssize)(len - cursor));
  return g_string_free(out, FALSE);
}

char *
ide_lsp_strip_snippet(const char *snippet)
{
  if (snippet == NULL) {
    return g_strdup("");
  }
  GString *out = g_string_new(NULL);
  const char *p = snippet;
  while (*p != '\0') {
    if (*p == '\\' && p[1] != '\0') {
      g_string_append_c(out, p[1]);
      p += 2;
    } else if (*p == '$') {
      p++;
      if (*p == '{') {
        /* ${n:default} / ${n|a,b|} / ${n} */
        const char *q = p + 1;
        while (g_ascii_isdigit(*q)) {
          q++;
        }
        if (*q == ':') {
          q++;
          int depth = 1;
          while (*q != '\0' && depth > 0) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) break; }
            if (depth > 0) g_string_append_c(out, *q);
            q++;
          }
          p = (*q == '}') ? q + 1 : q;
        } else {
          while (*q != '\0' && *q != '}') {
            q++;
          }
          p = (*q == '}') ? q + 1 : q;
        }
      } else {
        while (g_ascii_isdigit(*p)) {
          p++;
        }
      }
    } else {
      g_string_append_c(out, *p++);
    }
  }
  return g_string_free(out, FALSE);
}

char *
ide_lsp_hover_to_markdown(JsonNode *contents)
{
  if (contents == NULL || JSON_NODE_HOLDS_NULL(contents)) {
    return g_strdup("");
  }
  if (JSON_NODE_HOLDS_VALUE(contents)) {
    return g_strdup(json_node_get_string(contents) ? json_node_get_string(contents) : "");
  }
  if (JSON_NODE_HOLDS_OBJECT(contents)) {
    JsonObject *o = json_node_get_object(contents);
    const char *value = json_object_has_member(o, "value") ? json_object_get_string_member(o, "value") : "";
    if (json_object_has_member(o, "language")) {
      return g_strdup_printf("```%s\n%s\n```", json_object_get_string_member(o, "language"), value);
    }
    return g_strdup(value);
  }
  if (JSON_NODE_HOLDS_ARRAY(contents)) {
    GString *out = g_string_new(NULL);
    JsonArray *arr = json_node_get_array(contents);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
      g_autofree char *part = ide_lsp_hover_to_markdown(json_array_get_element(arr, i));
      if (part[0] == '\0') {
        continue;
      }
      if (out->len > 0) {
        g_string_append(out, "\n\n");
      }
      g_string_append(out, part);
    }
    return g_string_free(out, FALSE);
  }
  return g_strdup("");
}

const char *
ide_lsp_detect_server_for_file(const char *file_path, const char **out_server_id)
{
  if (out_server_id != NULL) {
    *out_server_id = "none";
  }
  const IdeLspServerSpec *spec = ide_lsp_server_spec_for_file(file_path);
  if (spec == NULL) {
    return NULL;
  }
  if (out_server_id != NULL) {
    *out_server_id = spec->server_id;
  }
  return spec->binary;
}

/* ═══════════════════════════ Completion items ═══════════════════════════ */

void
ide_completion_item_free(gpointer data)
{
  IdeCompletionItem *it = data;
  if (it == NULL) {
    return;
  }
  g_free(it->label);
  g_free(it->detail);
  g_free(it->insert_text);
  g_free(it->documentation);
  g_free(it->filter_text);
  g_free(it->sort_text);
  g_free(it->edit.new_text);
  g_clear_pointer(&it->extra_edits, g_ptr_array_unref);
  g_free(it);
}

static const char *
completion_badge(gint kind)
{
  switch (kind) {
    case 2: case 3: case 4: return "ƒ";           /* method, function, constructor */
    case 5: case 6: case 10: return "v";          /* field, variable, property */
    case 7: case 8: case 22: case 13: return "T"; /* class, interface, struct, enum */
    case 9: return "m";                           /* module */
    case 14: return "k";                          /* keyword */
    case 21: case 20: return "c";                 /* constant, enum member */
    default: return "•";
  }
}

IdeCompletionItem *
ide_lsp_parse_completion_item(JsonObject *o, const char *prefix)
{
  const char *label = ide_lsp_json_str(o, "label");
  if (label == NULL) {
    return NULL;
  }
  IdeCompletionItem *it = g_new0(IdeCompletionItem, 1);
  it->label = g_strdup(label);
  const char *ft = ide_lsp_json_str(o, "filterText");
  it->filter_text = g_strdup((ft != NULL && ft[0] != '\0') ? ft : label);
  const char *st = ide_lsp_json_str(o, "sortText");
  it->sort_text = g_strdup(st ? st : label);
  it->detail = g_strdup(ide_lsp_json_str(o, "detail"));
  if (it->detail == NULL && json_object_has_member(o, "labelDetails")) {
    JsonObject *ld = json_object_get_object_member(o, "labelDetails");
    const char *ld_det = ide_lsp_json_str(ld, "detail");
    it->detail = g_strdup(ld_det ? ld_det : ide_lsp_json_str(ld, "description"));
  }
  g_strlcpy(it->kind_badge, completion_badge((gint)json_object_get_int_member_with_default(o, "kind", 0)),
            sizeof(it->kind_badge));

  if (json_object_has_member(o, "documentation")) {
    it->documentation = extract_lsp_doc(json_object_get_member(o, "documentation"));
  }

  gboolean snippet = json_object_get_int_member_with_default(o, "insertTextFormat", 1) == 2;
  const char *ins = ide_lsp_json_str(o, "insertText");
  const char *new_text = ins ? ins : label;

  if (json_object_has_member(o, "textEdit") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "textEdit"))) {
    JsonObject *te = json_object_get_object_member(o, "textEdit");
    JsonObject *range = json_object_has_member(te, "range") ? json_object_get_object_member(te, "range")
                        : (json_object_has_member(te, "replace") ? json_object_get_object_member(te, "replace") : NULL);
    const char *te_nt = ide_lsp_json_str(te, "newText");
    const char *nt = te_nt ? te_nt : new_text;
    g_autofree char *stripped = snippet ? ide_lsp_strip_snippet(nt) : g_strdup(nt);
    IdeLspTextEdit tmp = { 0 };
    if (ide_lsp_json_range_to_edit(range, stripped, &tmp)) {
      it->edit = tmp;
      it->has_edit = TRUE;
      new_text = it->edit.new_text;
    }
  }
  if (it->has_edit) {
    it->insert_text = g_strdup(it->edit.new_text);
  } else {
    it->insert_text = snippet ? ide_lsp_strip_snippet(new_text) : g_strdup(new_text);
  }

  if (json_object_has_member(o, "additionalTextEdits")) {
    it->extra_edits = ide_lsp_parse_text_edits(json_object_get_member(o, "additionalTextEdits"));
    if (it->extra_edits->len == 0) {
      g_clear_pointer(&it->extra_edits, g_ptr_array_unref);
    }
  }

  if (prefix == NULL || prefix[0] == '\0') {
    it->score = 1;
  } else if (g_str_has_prefix(it->filter_text, prefix)) {
    it->score = 10000 - (gint)strlen(it->filter_text);
  } else {
    it->score = ide_fuzzy_score(prefix, it->filter_text);
  }
  if (it->score <= 0) {
    ide_completion_item_free(it);
    return NULL;
  }
  return it;
}

gint
ide_lsp_completion_cmp(gconstpointer a, gconstpointer b)
{
  const IdeCompletionItem *x = *(const IdeCompletionItem *const *)a;
  const IdeCompletionItem *y = *(const IdeCompletionItem *const *)b;
  if (x->score != y->score) {
    return x->score > y->score ? -1 : 1;
  }
  gint c = g_strcmp0(x->sort_text, y->sort_text);
  return c != 0 ? c : g_strcmp0(x->label, y->label);
}

/* ═══════════════════════════ Locations ═══════════════════════════ */

void
ide_lsp_location_free(gpointer data)
{
  IdeLspLocation *loc = data;
  if (loc == NULL) {
    return;
  }
  g_free(loc->path);
  g_free(loc->preview);
  g_free(loc);
}

static IdeLspLocation *
parse_location_object(JsonObject *o)
{
  if (o == NULL) {
    return NULL;
  }
  const char *uri = ide_lsp_json_str(o, "targetUri");
  if (uri == NULL) {
    uri = ide_lsp_json_str(o, "uri");
  }
  JsonObject *range = ide_lsp_json_obj_member(o, "targetSelectionRange");
  if (range == NULL) {
    range = ide_lsp_json_obj_member(o, "range");
  }
  if (range == NULL) {
    range = ide_lsp_json_obj_member(o, "targetRange");
  }
  if (uri == NULL || range == NULL) {
    return NULL;
  }
  JsonObject *start = ide_lsp_json_obj_member(range, "start");
  if (start == NULL) {
    return NULL;
  }
  char *path = g_filename_from_uri(uri, NULL, NULL);
  if (path == NULL) {
    return NULL;
  }
  gint64 line0 = MAX(0, json_object_get_int_member_with_default(start, "line", 0));
  gint64 ch0 = MAX(0, json_object_get_int_member_with_default(start, "character", 0));

  IdeLspLocation *loc = g_new0(IdeLspLocation, 1);
  loc->path = path;
  loc->line_1based = (guint)line0 + 1;
  loc->col_1based = (guint)ch0 + 1;
  return loc;
}

GPtrArray *
ide_lsp_parse_locations(JsonNode *result)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_lsp_location_free);
  if (result == NULL || JSON_NODE_HOLDS_NULL(result)) {
    return out;
  }
  if (JSON_NODE_HOLDS_OBJECT(result)) {
    IdeLspLocation *loc = parse_location_object(json_node_get_object(result));
    if (loc != NULL) {
      g_ptr_array_add(out, loc);
    }
    return out;
  }
  if (!JSON_NODE_HOLDS_ARRAY(result)) {
    return out;
  }
  JsonArray *arr = json_node_get_array(result);
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonNode *n = json_array_get_element(arr, i);
    if (n == NULL || !JSON_NODE_HOLDS_OBJECT(n)) {
      continue;
    }
    IdeLspLocation *loc = parse_location_object(json_node_get_object(n));
    if (loc != NULL) {
      g_ptr_array_add(out, loc);
    }
  }
  return out;
}

/* ═══════════════════════════ Signature Help ═══════════════════════════ */

void
ide_lsp_signature_info_free(gpointer data)
{
  IdeLspSignatureInfo *sig = data;
  if (sig == NULL) {
    return;
  }
  g_free(sig->label);
  g_free(sig->documentation);
  g_clear_pointer(&sig->param_labels, g_ptr_array_unref);
  g_free(sig);
}

static gboolean
json_int_opt(JsonObject *o, const char *name, gint64 *out)
{
  if (o == NULL || !json_object_has_member(o, name)) {
    return FALSE;
  }
  JsonNode *n = json_object_get_member(o, name);
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_is_null(n)) {
    return FALSE;
  }
  GType t = json_node_get_value_type(n);
  if (t != G_TYPE_INT64 && t != G_TYPE_DOUBLE) {
    return FALSE;
  }
  *out = json_node_get_int(n);
  return TRUE;
}

GPtrArray *
ide_lsp_parse_signature_help(JsonNode *result,
                             guint    *out_active_sig,
                             gint     *out_active_param)
{
  if (out_active_sig != NULL) {
    *out_active_sig = 0;
  }
  if (out_active_param != NULL) {
    *out_active_param = -1;
  }
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_lsp_signature_info_free);
  if (result == NULL || !JSON_NODE_HOLDS_OBJECT(result)) {
    return out;
  }

  JsonObject *ro = json_node_get_object(result);
  gint64 top_sig_val = 0;
  guint active_sig = 0;
  if (json_int_opt(ro, "activeSignature", &top_sig_val) && top_sig_val >= 0) {
    active_sig = (guint)top_sig_val;
  }
  gint64 top_param_val = -1;
  gint top_active_param = -1;
  if (json_int_opt(ro, "activeParameter", &top_param_val) && top_param_val >= 0) {
    top_active_param = (gint)top_param_val;
  }

  if (!json_object_has_member(ro, "signatures") ||
      !JSON_NODE_HOLDS_ARRAY(json_object_get_member(ro, "signatures"))) {
    return out;
  }
  JsonArray *sarr = json_object_get_array_member(ro, "signatures");
  for (guint i = 0; i < json_array_get_length(sarr); i++) {
    JsonNode *sn = json_array_get_element(sarr, i);
    if (sn == NULL || !JSON_NODE_HOLDS_OBJECT(sn)) {
      continue;
    }
    JsonObject *so = json_node_get_object(sn);
    const char *label = ide_lsp_json_str(so, "label");
    if (label == NULL) {
      continue;
    }
    IdeLspSignatureInfo *sig = g_new0(IdeLspSignatureInfo, 1);
    sig->label = g_strdup(label);
    if (json_object_has_member(so, "documentation")) {
      sig->documentation = extract_lsp_doc(json_object_get_member(so, "documentation"));
    }
    sig->param_labels = g_ptr_array_new_with_free_func(g_free);

    gint64 per_sig_param = -1;
    if (json_int_opt(so, "activeParameter", &per_sig_param) && per_sig_param >= 0) {
      sig->active_param = (gint)per_sig_param;
    } else {
      sig->active_param = top_active_param;
    }

    if (json_object_has_member(so, "parameters") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(so, "parameters"))) {
      JsonArray *parr = json_object_get_array_member(so, "parameters");
      gsize label_len = strlen(sig->label);
      for (guint j = 0; j < json_array_get_length(parr); j++) {
        JsonNode *pn = json_array_get_element(parr, j);
        if (pn == NULL || !JSON_NODE_HOLDS_OBJECT(pn)) {
          continue;
        }
        JsonObject *po = json_node_get_object(pn);
        if (!json_object_has_member(po, "label")) {
          continue;
        }
        JsonNode *ln = json_object_get_member(po, "label");
        if (ln != NULL && JSON_NODE_HOLDS_VALUE(ln) && !json_node_is_null(ln)) {
          const char *pl = json_node_get_string(ln);
          if (pl != NULL) {
            g_ptr_array_add(sig->param_labels, g_strdup(pl));
          }
        } else if (ln != NULL && JSON_NODE_HOLDS_ARRAY(ln)) {
          /* ParameterInformation.label as [start, end] UTF-16 offsets in sig->label */
          JsonArray *offs = json_node_get_array(ln);
          if (json_array_get_length(offs) >= 2) {
            gint64 u0 = MAX(0, json_array_get_int_element(offs, 0));
            gint64 u1 = MAX(u0, json_array_get_int_element(offs, 1));
            gsize b0 = MIN(ide_utf16_col_to_utf8_byte(sig->label, (guint)u0), label_len);
            gsize b1 = MIN(ide_utf16_col_to_utf8_byte(sig->label, (guint)u1), label_len);
            if (b1 > b0) {
              g_ptr_array_add(sig->param_labels, g_strndup(sig->label + b0, b1 - b0));
            }
          }
        }
      }
    }

    g_ptr_array_add(out, sig);
  }

  if (out->len > 0) {
    if (active_sig >= out->len) {
      active_sig = out->len - 1;
    }
    const IdeLspSignatureInfo *cur = g_ptr_array_index(out, active_sig);
    if (out_active_sig != NULL) {
      *out_active_sig = active_sig;
    }
    if (out_active_param != NULL) {
      *out_active_param = cur->active_param;
    }
  } else if (out_active_param != NULL) {
    *out_active_param = top_active_param;
  }
  return out;
}

/* Locates the `param_idx`-th comma-separated parameter inside the first balanced
 * `(...)` in `label`, ignoring commas nested in `()`, `<>`, `[]`. */
static gboolean
find_nth_param_span(const char *label, gint param_idx, gsize *start_out, gsize *end_out)
{
  if (label == NULL || param_idx < 0) {
    return FALSE;
  }
  const char *open = strchr(label, '(');
  if (open == NULL) {
    return FALSE;
  }
  const char *p = open + 1;
  const char *seg_start = p;
  int paren_depth = 0;
  int angle_depth = 0;
  int bracket_depth = 0;
  gint cur_idx = 0;

  while (*p != '\0') {
    char c = *p;
    if (c == '(') {
      paren_depth++;
    } else if (c == ')') {
      if (paren_depth == 0) {
        if (cur_idx == param_idx) {
          const char *s = seg_start;
          const char *e = p;
          while (s < e && g_ascii_isspace(*s)) s++;
          while (e > s && g_ascii_isspace(e[-1])) e--;
          if (e > s) {
            *start_out = (gsize)(s - label);
            *end_out = (gsize)(e - label);
            return TRUE;
          }
        }
        return FALSE;
      }
      paren_depth--;
    } else if (c == '<') {
      angle_depth++;
    } else if (c == '>' && angle_depth > 0) {
      angle_depth--;
    } else if (c == '[') {
      bracket_depth++;
    } else if (c == ']' && bracket_depth > 0) {
      bracket_depth--;
    } else if (c == ',' && paren_depth == 0 && angle_depth == 0 && bracket_depth == 0) {
      if (cur_idx == param_idx) {
        const char *s = seg_start;
        const char *e = p;
        while (s < e && g_ascii_isspace(*s)) s++;
        while (e > s && g_ascii_isspace(e[-1])) e--;
        if (e > s) {
          *start_out = (gsize)(s - label);
          *end_out = (gsize)(e - label);
          return TRUE;
        }
        return FALSE;
      }
      cur_idx++;
      seg_start = p + 1;
    }
    p++;
  }
  return FALSE;
}

char *
ide_lsp_format_signature_pango(const IdeLspSignatureInfo *sig, gint active_param)
{
  if (sig == NULL || sig->label == NULL) {
    return g_strdup("");
  }
  const char *label = sig->label;
  gint idx = (active_param >= 0) ? active_param : sig->active_param;
  gsize span_start = 0;
  gsize span_end = 0;
  gboolean found = FALSE;

  if (idx >= 0 && sig->param_labels != NULL && (guint)idx < sig->param_labels->len) {
    /* Walk parameters sequentially after '(' so duplicate parameter types
     * (e.g. `add(int, int)`) highlight the right occurrence. */
    const char *open = strchr(label, '(');
    const char *cursor = (open != NULL) ? (open + 1) : label;
    gboolean seq_ok = TRUE;
    for (guint k = 0; k <= (guint)idx; k++) {
      const char *p = g_ptr_array_index(sig->param_labels, k);
      if (p == NULL || p[0] == '\0') {
        seq_ok = FALSE;
        break;
      }
      const char *hit = strstr(cursor, p);
      if (hit == NULL) {
        seq_ok = FALSE;
        break;
      }
      gsize plen = strlen(p);
      if (k == (guint)idx) {
        span_start = (gsize)(hit - label);
        span_end = span_start + plen;
        found = TRUE;
      }
      cursor = hit + plen;
    }
    if (!found && !seq_ok) {
      const char *p = g_ptr_array_index(sig->param_labels, (guint)idx);
      if (p != NULL && p[0] != '\0') {
        const char *hit = strstr(label, p);
        if (hit != NULL) {
          span_start = (gsize)(hit - label);
          span_end = span_start + strlen(p);
          found = TRUE;
        }
      }
    }
  }

  if (!found && idx >= 0) {
    found = find_nth_param_span(label, idx, &span_start, &span_end);
  }

  if (found && span_end > span_start) {
    g_autofree char *before = g_markup_escape_text(label, (gssize)span_start);
    g_autofree char *mid = g_markup_escape_text(label + span_start, (gssize)(span_end - span_start));
    g_autofree char *after = g_markup_escape_text(label + span_end, -1);
    return g_strdup_printf("%s<span foreground=\"#00d8f0\" weight=\"bold\">%s</span>%s",
                           before, mid, after);
  }
  return g_markup_escape_text(label, -1);
}

/* ═══════════════════════════ Call Hierarchy ═══════════════════════════ */

void
ide_lsp_call_entry_free(gpointer data)
{
  IdeLspCallEntry *ce = data;
  if (ce == NULL) {
    return;
  }
  g_free(ce->name);
  g_free(ce->detail);
  g_free(ce->path);
  g_free(ce);
}

static JsonObject *
first_from_range(JsonObject *o)
{
  if (o == NULL || !json_object_has_member(o, "fromRanges")) {
    return NULL;
  }
  JsonNode *rn = json_object_get_member(o, "fromRanges");
  if (rn == NULL || !JSON_NODE_HOLDS_ARRAY(rn)) {
    return NULL;
  }
  JsonArray *rarr = json_node_get_array(rn);
  if (json_array_get_length(rarr) == 0) {
    return NULL;
  }
  JsonNode *r0 = json_array_get_element(rarr, 0);
  return (r0 != NULL && JSON_NODE_HOLDS_OBJECT(r0)) ? json_node_get_object(r0) : NULL;
}

GPtrArray *
ide_lsp_parse_call_hierarchy(JsonNode *result, gboolean outgoing)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(ide_lsp_call_entry_free);
  if (result == NULL || !JSON_NODE_HOLDS_ARRAY(result)) {
    return out;
  }
  JsonArray *arr = json_node_get_array(result);
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonNode *n = json_array_get_element(arr, i);
    if (n == NULL || !JSON_NODE_HOLDS_OBJECT(n)) {
      continue;
    }
    JsonObject *o = json_node_get_object(n);
    JsonObject *item = ide_lsp_json_obj_member(o, outgoing ? "to" : "from");
    if (item == NULL) {
      continue;
    }
    const char *name = ide_lsp_json_str(item, "name");
    const char *uri = ide_lsp_json_str(item, "uri");
    if (name == NULL || uri == NULL) {
      continue;
    }
    char *path = g_filename_from_uri(uri, NULL, NULL);
    if (path == NULL) {
      continue;
    }

    JsonObject *range = NULL;
    if (!outgoing) {
      /* Incoming calls: fromRanges[0] is the call site inside `from.uri`. */
      range = first_from_range(o);
      if (range == NULL) {
        range = ide_lsp_json_obj_member(item, "selectionRange");
      }
      if (range == NULL) {
        range = ide_lsp_json_obj_member(item, "range");
      }
    } else {
      /* Outgoing calls: `to.selectionRange` is the callee inside `to.uri`. */
      range = ide_lsp_json_obj_member(item, "selectionRange");
      if (range == NULL) {
        range = ide_lsp_json_obj_member(item, "range");
      }
      if (range == NULL) {
        range = first_from_range(o);
      }
    }

    guint line0 = 0;
    guint ch0 = 0;
    JsonObject *start = ide_lsp_json_obj_member(range, "start");
    if (start != NULL) {
      line0 = (guint)MAX(0, json_object_get_int_member_with_default(start, "line", 0));
      ch0 = (guint)MAX(0, json_object_get_int_member_with_default(start, "character", 0));
    }

    IdeLspCallEntry *ce = g_new0(IdeLspCallEntry, 1);
    ce->name = g_strdup(name);
    ce->detail = g_strdup(ide_lsp_json_str(item, "detail"));
    ce->path = path;
    ce->line_1based = line0 + 1;
    ce->col_1based = ch0 + 1;
    ce->outgoing = outgoing;
    gint kind = (gint)json_object_get_int_member_with_default(item, "kind", 12);
    g_strlcpy(ce->kind_badge, symbol_kind_badge(kind), sizeof(ce->kind_badge));
    g_ptr_array_add(out, ce);
  }
  return out;
}
