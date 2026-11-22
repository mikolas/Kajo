#include "ide_dap_private.h"

#include <string.h>

/* ═══════════════════════════ rows & trees ═══════════════════════════ */

static void
clear_list(GtkWidget *list_box)
{
  GtkWidget *child = gtk_widget_get_first_child(list_box);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(list_box), child);
    child = next;
  }
}

static GtkWidget *
append_row(GtkWidget *list_box, const char *text, gint index)
{
  GtkWidget *row = gtk_list_box_row_new();
  gtk_widget_add_css_class(row, "ide-tree-row");
  GtkWidget *lbl = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(lbl, "ide-tree-name");
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
  g_object_set_data(G_OBJECT(row), "dap-index", GINT_TO_POINTER(index));
  gtk_list_box_append(GTK_LIST_BOX(list_box), row);
  return row;
}

static DapVarNode *
dap_var_node_new(IdeDapView *dap,
                 const char *name,
                 const char *type,
                 const char *value,
                 gint64      var_ref,
                 guint       depth,
                 gboolean    is_scope)
{
  DapVarNode *n = g_new0(DapVarNode, 1);
  n->id = ++dap->next_node_id;
  n->name = g_strdup(name ? name : "?");
  n->type = (type != NULL && type[0] != '\0') ? g_strdup(type) : NULL;
  n->value = g_strdup(value ? value : "");
  n->var_ref = var_ref;
  n->depth = depth;
  n->is_scope = is_scope;
  return n;
}

static DapVarNode *
find_var_node_by_id(GPtrArray *nodes, guint id)
{
  if (nodes == NULL || id == 0) {
    return NULL;
  }
  for (guint i = 0; i < nodes->len; i++) {
    DapVarNode *n = g_ptr_array_index(nodes, i);
    if (n->id == id) {
      return n;
    }
    if (n->children != NULL) {
      DapVarNode *found = find_var_node_by_id(n->children, id);
      if (found != NULL) {
        return found;
      }
    }
  }
  return NULL;
}

static DapWatchItem *
find_watch_by_id(IdeDapView *dap, guint watch_id)
{
  if (dap->watches == NULL || watch_id == 0) {
    return NULL;
  }
  for (guint i = 0; i < dap->watches->len; i++) {
    DapWatchItem *w = g_ptr_array_index(dap->watches, i);
    if (w->id == watch_id) {
      return w;
    }
  }
  return NULL;
}

static char *
format_var_node_label(const DapVarNode *node)
{
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < node->depth * 2; i++) {
    g_string_append_c(s, ' ');
  }
  if (node->var_ref > 0) {
    g_string_append(s, node->expanded ? "▾ " : "▸ ");
  } else {
    g_string_append(s, "  ");
  }
  if (node->is_scope) {
    g_string_append(s, node->name ? node->name : "Scope");
    if (node->loading) {
      g_string_append(s, " (loading...)");
    } else if (node->expanded && node->children_loaded &&
               (node->children == NULL || node->children->len == 0)) {
      g_string_append(s, " (empty)");
    }
  } else {
    g_string_append(s, node->name ? node->name : "?");
    if (node->type != NULL && node->type[0] != '\0') {
      g_string_append_printf(s, " : %s", node->type);
    }
    g_string_append_printf(s, " = %s", node->value ? node->value : "");
    if (node->loading) {
      g_string_append(s, " (loading...)");
    }
  }
  return g_string_free(s, FALSE);
}

static void
append_var_tree_rows(GtkWidget *list_box, GPtrArray *nodes, guint watch_id)
{
  if (nodes == NULL) {
    return;
  }
  for (guint i = 0; i < nodes->len; i++) {
    DapVarNode *node = g_ptr_array_index(nodes, i);
    g_autofree char *label = format_var_node_label(node);
    GtkWidget *row = append_row(list_box, label, -1);
    g_object_set_data(G_OBJECT(row), "dap-node-id", GUINT_TO_POINTER(node->id));
    g_object_set_data(G_OBJECT(row), "dap-watch-id", GUINT_TO_POINTER(watch_id));
    if (node->expanded && node->children != NULL) {
      append_var_tree_rows(list_box, node->children, watch_id);
    }
  }
}

static void
rebuild_locals_list(IdeDapView *dap)
{
  clear_list(dap->locals_list_box);
  if (dap->var_roots == NULL || dap->var_roots->len == 0) {
    append_row(dap->locals_list_box, "(no locals in this frame)", -1);
    return;
  }
  append_var_tree_rows(dap->locals_list_box, dap->var_roots, 0);
}

static void
on_watch_remove_clicked(GtkButton *btn, gpointer user_data)
{
  IdeDapView *dap = user_data;
  guint watch_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "dap-watch-id"));
  for (guint i = 0; i < dap->watches->len; i++) {
    DapWatchItem *w = g_ptr_array_index(dap->watches, i);
    if (w->id == watch_id) {
      ide_dap_view_remove_watch(dap, i);
      return;
    }
  }
}

void
ide_dap_ui_rebuild_watch_list(IdeDapView *dap)
{
  if (dap->watch_list_box == NULL) {
    return;
  }
  clear_list(dap->watch_list_box);
  for (guint i = 0; i < dap->watches->len; i++) {
    DapWatchItem *w = g_ptr_array_index(dap->watches, i);
    const char *arrow = (w->var_ref > 0) ? (w->expanded ? "▾ " : "▸ ") : "  ";
    const char *val = (w->value != NULL) ? w->value : "(paused to evaluate)";
    g_autofree char *text = NULL;
    if (w->has_error) {
      text = g_strdup_printf("%s%s = error: %s", arrow, w->expression, val);
    } else if (w->type != NULL && w->type[0] != '\0') {
      text = g_strdup_printf("%s%s : %s = %s%s", arrow, w->expression, w->type, val,
                             w->loading ? " (loading...)" : "");
    } else {
      text = g_strdup_printf("%s%s = %s%s", arrow, w->expression, val,
                             w->loading ? " (loading...)" : "");
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "ide-tree-row");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(lbl, "ide-tree-name");
    gtk_box_append(GTK_BOX(hbox), lbl);

    GtkWidget *rm_btn = gtk_button_new_with_label("×");
    gtk_widget_set_focusable(rm_btn, FALSE);
    gtk_widget_set_valign(rm_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(rm_btn, "ide-tab-close");
    g_object_set_data(G_OBJECT(rm_btn), "dap-watch-id", GUINT_TO_POINTER(w->id));
    g_signal_connect(rm_btn, "clicked", G_CALLBACK(on_watch_remove_clicked), dap);
    gtk_box_append(GTK_BOX(hbox), rm_btn);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    g_object_set_data(G_OBJECT(row), "dap-watch-id", GUINT_TO_POINTER(w->id));
    g_object_set_data(G_OBJECT(row), "dap-node-id", GUINT_TO_POINTER(0));
    gtk_list_box_append(GTK_LIST_BOX(dap->watch_list_box), row);

    if (w->expanded && w->children != NULL) {
      append_var_tree_rows(dap->watch_list_box, w->children, w->id);
    }
  }
}

static void
dap_watch_reset(DapWatchItem *w, const char *placeholder)
{
  g_free(w->value);
  w->value = g_strdup(placeholder);
  g_clear_pointer(&w->type, g_free);
  w->var_ref = 0;
  w->expanded = FALSE;
  w->loading = FALSE;
  w->children_loaded = FALSE;
  w->has_error = FALSE;
  g_clear_pointer(&w->children, g_ptr_array_unref);
}

void
ide_dap_ui_clear_stopped(IdeDapView *dap)
{
  dap->scope_gen++;
  clear_list(dap->stack_list_box);
  clear_list(dap->locals_list_box);
  g_ptr_array_set_size(dap->frames, 0);
  g_ptr_array_set_size(dap->var_roots, 0);
  for (guint i = 0; i < dap->watches->len; i++) {
    dap_watch_reset(g_ptr_array_index(dap->watches, i), "(paused to evaluate)");
  }
  ide_dap_ui_rebuild_watch_list(dap);
  if (dap->frame_cb != NULL) {
    dap->frame_cb(NULL, 0, dap->ctx_data);
  }
}

/* ═══════════════════════════ scopes / vars / stack ═══════════════════════════ */

typedef struct {
  IdeDapView *dap;
  guint       generation;
  guint       scope_gen;
  guint       node_id;    /* 0 = root locals or top-level watch */
  guint       watch_id;   /* 0 = locals tree, > 0 = watch tree */
} VarToken;

static VarToken *
var_token_new(IdeDapView *dap, guint node_id, guint watch_id)
{
  VarToken *t = g_new0(VarToken, 1);
  t->dap = dap;
  t->generation = dap->generation;
  t->scope_gen = dap->scope_gen;
  t->node_id = node_id;
  t->watch_id = watch_id;
  return t;
}

static gboolean
var_token_valid(const VarToken *t)
{
  return t->generation == t->dap->generation &&
         t->scope_gen == t->dap->scope_gen &&
         t->dap->client != NULL;
}

static void
populate_var_nodes(IdeDapView *dap, GPtrArray *target, JsonArray *vars, guint depth)
{
  g_ptr_array_set_size(target, 0);
  if (vars == NULL) {
    return;
  }
  for (guint i = 0; i < json_array_get_length(vars); i++) {
    JsonObject *v = json_array_get_object_element(vars, i);
    const char *name = json_object_get_string_member_with_default(v, "name", "?");
    const char *type = json_object_get_string_member_with_default(v, "type", NULL);
    const char *val = json_object_get_string_member_with_default(v, "value", "");
    gint64 ref = json_object_get_int_member_with_default(v, "variablesReference", 0);
    g_ptr_array_add(target, dap_var_node_new(dap, name, type, val, ref, depth, FALSE));
  }
}

static void
apply_var_node_children(IdeDapView *dap, DapVarNode *node, gboolean ok, JsonArray *vars)
{
  if (node == NULL) {
    return;
  }
  node->loading = FALSE;
  if (ok && vars != NULL) {
    node->children_loaded = TRUE;
    if (node->children == NULL) {
      node->children = g_ptr_array_new_with_free_func(ide_dap_var_node_free);
    }
    populate_var_nodes(dap, node->children, vars, node->depth + 1);
  } else {
    node->expanded = FALSE;
  }
}

static void
on_variables(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  (void)msg;
  VarToken *t = user_data;
  if (!var_token_valid(t)) {
    g_free(t);
    return;
  }

  IdeDapView *dap = t->dap;
  JsonArray *vars = (ok && body != NULL && json_object_has_member(body, "variables"))
                        ? json_object_get_array_member(body, "variables")
                        : NULL;

  if (t->watch_id > 0) {
    DapWatchItem *w = find_watch_by_id(dap, t->watch_id);
    if (w != NULL) {
      if (t->node_id == 0) {
        w->loading = FALSE;
        if (ok && vars != NULL) {
          w->children_loaded = TRUE;
          if (w->children == NULL) {
            w->children = g_ptr_array_new_with_free_func(ide_dap_var_node_free);
          }
          populate_var_nodes(dap, w->children, vars, 1);
        } else {
          w->expanded = FALSE;
        }
      } else {
        apply_var_node_children(dap, find_var_node_by_id(w->children, t->node_id), ok, vars);
      }
      ide_dap_ui_rebuild_watch_list(dap);
    }
  } else if (t->node_id == 0) {
    if (ok && vars != NULL) {
      populate_var_nodes(dap, dap->var_roots, vars, 0);
    }
    rebuild_locals_list(dap);
  } else {
    DapVarNode *node = find_var_node_by_id(dap->var_roots, t->node_id);
    if (node != NULL) {
      apply_var_node_children(dap, node, ok, vars);
      rebuild_locals_list(dap);
    }
  }
  g_free(t);
}

static void
on_scopes(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)msg;
  VarToken *t = user_data;
  if (!var_token_valid(t)) {
    g_free(t);
    return;
  }

  IdeDapView *dap = t->dap;
  g_ptr_array_set_size(dap->var_roots, 0);

  if (ok && body != NULL && json_object_has_member(body, "scopes")) {
    JsonArray *scopes = json_object_get_array_member(body, "scopes");
    g_autoptr(GPtrArray) eligible = g_ptr_array_new();
    for (guint i = 0; i < json_array_get_length(scopes); i++) {
      JsonObject *sc = json_array_get_object_element(scopes, i);
      if (json_object_get_boolean_member_with_default(sc, "expensive", FALSE)) {
        continue;
      }
      gint64 ref = json_object_get_int_member_with_default(sc, "variablesReference", 0);
      if (ref > 0) {
        g_ptr_array_add(eligible, sc);
      }
    }

    if (eligible->len == 0) {
      rebuild_locals_list(dap);
    } else if (eligible->len == 1) {
      JsonObject *sc = g_ptr_array_index(eligible, 0);
      gint64 ref = json_object_get_int_member_with_default(sc, "variablesReference", 0);
      ide_dap_client_request(c, "variables", ide_dap_args_object("variablesReference", ref, NULL, 0),
                             on_variables, var_token_new(dap, 0, 0));
    } else {
      for (guint i = 0; i < eligible->len; i++) {
        JsonObject *sc = g_ptr_array_index(eligible, i);
        const char *name = json_object_get_string_member_with_default(sc, "name", "Scope");
        gint64 ref = json_object_get_int_member_with_default(sc, "variablesReference", 0);
        DapVarNode *sn = dap_var_node_new(dap, name, NULL, "", ref, 0, TRUE);
        g_ptr_array_add(dap->var_roots, sn);
      }
      DapVarNode *first = g_ptr_array_index(dap->var_roots, 0);
      first->expanded = TRUE;
      first->loading = TRUE;
      rebuild_locals_list(dap);
      ide_dap_client_request(c, "variables", ide_dap_args_object("variablesReference", first->var_ref, NULL, 0),
                             on_variables, var_token_new(dap, first->id, 0));
    }
  } else {
    rebuild_locals_list(dap);
  }
  g_free(t);
}

static void
on_watch_evaluate(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  VarToken *t = user_data;
  if (!var_token_valid(t)) {
    g_free(t);
    return;
  }

  IdeDapView *dap = t->dap;
  DapWatchItem *w = find_watch_by_id(dap, t->watch_id);
  if (w != NULL) {
    g_clear_pointer(&w->value, g_free);
    g_clear_pointer(&w->type, g_free);
    g_clear_pointer(&w->children, g_ptr_array_unref);
    w->expanded = FALSE;
    w->loading = FALSE;
    w->children_loaded = FALSE;
    if (ok && body != NULL) {
      w->has_error = FALSE;
      w->value = g_strdup(json_object_get_string_member_with_default(body, "result", ""));
      const char *tp = json_object_get_string_member_with_default(body, "type", NULL);
      if (tp != NULL && tp[0] != '\0') {
        w->type = g_strdup(tp);
      }
      w->var_ref = json_object_get_int_member_with_default(body, "variablesReference", 0);
    } else {
      w->has_error = TRUE;
      w->var_ref = 0;
      w->value = g_strdup(msg ? msg : "evaluation failed");
    }
    ide_dap_ui_rebuild_watch_list(dap);
  }
  g_free(t);
}

void
ide_dap_ui_evaluate_watch(IdeDapView *dap, DapWatchItem *w)
{
  if (dap->state != DAP_STOPPED || dap->client == NULL || w == NULL) {
    return;
  }
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "expression");
  json_builder_add_string_value(b, w->expression);
  json_builder_set_member_name(b, "context");
  json_builder_add_string_value(b, "watch");
  json_builder_set_member_name(b, "frameId");
  json_builder_add_int_value(b, dap->frame_id);
  json_builder_end_object(b);
  ide_dap_client_request(dap->client, "evaluate", json_builder_get_root(b),
                         on_watch_evaluate, var_token_new(dap, 0, w->id));
}

static void
evaluate_all_watches(IdeDapView *dap)
{
  if (dap->watches == NULL || dap->watches->len == 0) {
    return;
  }
  for (guint i = 0; i < dap->watches->len; i++) {
    DapWatchItem *w = g_ptr_array_index(dap->watches, i);
    dap_watch_reset(w, "(evaluating...)");
    ide_dap_ui_evaluate_watch(dap, w);
  }
  ide_dap_ui_rebuild_watch_list(dap);
}

static void
select_frame(IdeDapView *dap, guint index)
{
  if (index >= dap->frames->len) {
    return;
  }
  DapFrame *f = g_ptr_array_index(dap->frames, index);
  dap->frame_id = f->id;
  dap->scope_gen++;
  g_ptr_array_set_size(dap->var_roots, 0);
  clear_list(dap->locals_list_box);
  if (dap->frame_cb != NULL && f->path != NULL) {
    dap->frame_cb(f->path, f->line, dap->ctx_data);
  }
  if (dap->client != NULL) {
    ide_dap_client_request(dap->client, "scopes", ide_dap_args_object("frameId", f->id, NULL, 0), on_scopes,
                           var_token_new(dap, 0, 0));
    evaluate_all_watches(dap);
  }
}

static void
on_stack_trace(IdeDapClient *c, gboolean ok, JsonObject *body, const char *msg, gpointer user_data)
{
  (void)c;
  (void)msg;
  DapToken *t = user_data;
  if (ide_dap_token_valid(t) && ok && body != NULL && json_object_has_member(body, "stackFrames")) {
    IdeDapView *dap = t->dap;
    clear_list(dap->stack_list_box);
    g_ptr_array_set_size(dap->frames, 0);
    JsonArray *arr = json_object_get_array_member(body, "stackFrames");
    for (guint i = 0; i < json_array_get_length(arr); i++) {
      JsonObject *fo = json_array_get_object_element(arr, i);
      DapFrame *f = g_new0(DapFrame, 1);
      f->id = json_object_get_int_member_with_default(fo, "id", 0);
      f->name = g_strdup(json_object_get_string_member_with_default(fo, "name", "?"));
      f->line = (guint)json_object_get_int_member_with_default(fo, "line", 0);
      JsonObject *src = json_object_has_member(fo, "source") ? json_object_get_object_member(fo, "source") : NULL;
      if (src != NULL) {
        f->path = g_strdup(json_object_get_string_member_with_default(src, "path", NULL));
      }
      g_ptr_array_add(dap->frames, f);

      g_autofree char *base = f->path ? g_path_get_basename(f->path) : g_strdup("??");
      g_autofree char *label = g_strdup_printf("%s#%u %s  %s:%u", i == 0 ? "▶ " : "  ", i, f->name, base, f->line);
      append_row(dap->stack_list_box, label, (gint)i);
    }
    if (dap->frames->len > 0) {
      GtkListBoxRow *r0 = gtk_list_box_get_row_at_index(GTK_LIST_BOX(dap->stack_list_box), 0);
      if (r0 != NULL) {
        gtk_list_box_select_row(GTK_LIST_BOX(dap->stack_list_box), r0);
      }
      /* prefer the first frame that has source we can show */
      guint pick = 0;
      for (guint i = 0; i < dap->frames->len; i++) {
        DapFrame *f = g_ptr_array_index(dap->frames, i);
        if (f->path != NULL && g_file_test(f->path, G_FILE_TEST_EXISTS)) {
          pick = i;
          break;
        }
      }
      select_frame(dap, pick);
    }
  }
  g_free(t);
}

void
ide_dap_ui_request_stack(IdeDapView *dap)
{
  JsonNode *args = ide_dap_args_object("threadId", dap->thread_id, "levels", 30);
  ide_dap_client_request(dap->client, "stackTrace", args, on_stack_trace, ide_dap_token_new(dap));
}

/* ═══════════════════════════ row activations & panels ═══════════════════════════ */

static void
on_stack_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void)box;
  IdeDapView *dap = user_data;
  gint idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "dap-index"));
  if (dap->state == DAP_STOPPED && idx >= 0) {
    select_frame(dap, (guint)idx);
  }
}

static void
toggle_var_node(IdeDapView *dap, DapVarNode *node, guint watch_id)
{
  if (node == NULL || node->var_ref <= 0) {
    return;
  }
  if (node->expanded) {
    node->expanded = FALSE;
  } else {
    node->expanded = TRUE;
    if (!node->children_loaded && !node->loading) {
      node->loading = TRUE;
      ide_dap_client_request(dap->client, "variables", ide_dap_args_object("variablesReference", node->var_ref, NULL, 0),
                             on_variables, var_token_new(dap, node->id, watch_id));
    }
  }
  if (watch_id > 0) {
    ide_dap_ui_rebuild_watch_list(dap);
  } else {
    rebuild_locals_list(dap);
  }
}

static void
on_locals_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void)box;
  IdeDapView *dap = user_data;
  if (dap->state != DAP_STOPPED || dap->client == NULL) {
    return;
  }
  guint node_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "dap-node-id"));
  toggle_var_node(dap, find_var_node_by_id(dap->var_roots, node_id), 0);
}

static void
on_watch_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void)box;
  IdeDapView *dap = user_data;
  if (dap->state != DAP_STOPPED || dap->client == NULL) {
    return;
  }
  guint watch_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "dap-watch-id"));
  guint node_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "dap-node-id"));
  DapWatchItem *w = find_watch_by_id(dap, watch_id);
  if (w == NULL) {
    return;
  }
  if (node_id == 0) {
    if (w->var_ref <= 0) {
      return;
    }
    if (w->expanded) {
      w->expanded = FALSE;
    } else {
      w->expanded = TRUE;
      if (!w->children_loaded && !w->loading) {
        w->loading = TRUE;
        ide_dap_client_request(dap->client, "variables", ide_dap_args_object("variablesReference", w->var_ref, NULL, 0),
                               on_variables, var_token_new(dap, 0, w->id));
      }
    }
    ide_dap_ui_rebuild_watch_list(dap);
    return;
  }
  toggle_var_node(dap, find_var_node_by_id(w->children, node_id), w->id);
}

static void
on_watch_entry_activate(GtkEntry *entry, gpointer user_data)
{
  IdeDapView *dap = user_data;
  const char *expr = gtk_editable_get_text(GTK_EDITABLE(entry));
  ide_dap_view_add_watch(dap, expr);
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static GtkWidget *
append_scrolled_list(GtkWidget  *parent,
                     const char *title,
                     int         top_margin,
                     gboolean    single_click,
                     GCallback   on_activate,
                     IdeDapView *dap)
{
  GtkWidget *lbl = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  if (top_margin > 0) {
    gtk_widget_set_margin_top(lbl, top_margin);
  }
  gtk_widget_add_css_class(lbl, "ide-tree-header-title");
  gtk_box_append(GTK_BOX(parent), lbl);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkWidget *list = gtk_list_box_new();
  gtk_widget_set_focusable(list, FALSE);
  if (single_click) {
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
  }
  g_signal_connect(list, "row-activated", on_activate, dap);
  gtk_widget_add_css_class(list, "ide-tree-listbox");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  gtk_box_append(GTK_BOX(parent), scroll);
  return list;
}

void
ide_dap_ui_build_panels(IdeDapView *dap, GtkWidget *cols)
{
  /* Col 1: Call Stack */
  GtkWidget *c1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(c1, TRUE);
  dap->stack_list_box = append_scrolled_list(c1, "CALL STACK (THREADS)", 0, FALSE,
                                             G_CALLBACK(on_stack_row_activated), dap);
  gtk_box_append(GTK_BOX(cols), c1);

  /* Col 2: Locals & Scopes + Watches */
  GtkWidget *c2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(c2, TRUE);
  dap->locals_list_box = append_scrolled_list(c2, "LOCALS & SCOPES", 0, TRUE,
                                              G_CALLBACK(on_locals_row_activated), dap);
  dap->watch_list_box = append_scrolled_list(c2, "WATCHES", 4, TRUE,
                                             G_CALLBACK(on_watch_row_activated), dap);

  dap->watch_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(dap->watch_entry), "Add watch expression...");
  gtk_widget_add_css_class(dap->watch_entry, "shell-launcher-input");
  g_signal_connect(dap->watch_entry, "activate", G_CALLBACK(on_watch_entry_activate), dap);
  gtk_box_append(GTK_BOX(c2), dap->watch_entry);

  gtk_box_append(GTK_BOX(cols), c2);
}
