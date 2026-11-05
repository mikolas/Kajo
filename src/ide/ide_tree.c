#include "ide_tree.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
  char     *abs_path;
  char     *rel_path;
  char     *name;
  gboolean  is_dir;
  guint     depth;
  char      git_status[4];
} IdeTreeEntry;

struct _IdeTree {
  char             *workspace_root;
  IdeTreeFileOpenCb open_cb;
  gpointer          open_cb_data;

  GtkWidget        *root_box;
  GtkWidget        *header_title;
  GtkWidget        *prompt_revealer;
  GtkWidget        *prompt_label;
  GtkWidget        *prompt_entry;
  GtkWidget        *list_box;
  GtkWidget        *scrolled;

  GHashTable       *expanded_dirs;
  GHashTable       *git_status_map;
  GPtrArray        *entries;
  gint              selected_idx;
  gboolean          prompt_is_dir;
  gboolean          has_pending_scroll;
  double            pending_scroll_val;
};

static void
ide_tree_entry_free(gpointer data)
{
  IdeTreeEntry *e = data;
  if (e == NULL) {
    return;
  }
  g_free(e->abs_path);
  g_free(e->rel_path);
  g_free(e->name);
  g_free(e);
}

GHashTable *
ide_tree_parse_git_porcelain(const char *porcelain_text)
{
  GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (porcelain_text == NULL || porcelain_text[0] == '\0') {
    return map;
  }

  g_auto(GStrv) lines = g_strsplit(porcelain_text, "\n", -1);
  for (gsize i = 0; lines[i] != NULL; i++) {
    const char *line = lines[i];
    if (strlen(line) < 4) {
      continue;
    }
    char x = line[0];
    char y = line[1];
    const char *path = line + 3;
    while (*path == ' ') {
      path++;
    }
    /* Handle rename arrow "old -> new" */
    const char *arrow = strstr(path, " -> ");
    if (arrow != NULL) {
      path = arrow + 4;
    }
    if (*path == '\0') {
      continue;
    }

    const char *badge = "M";
    if (x == '?' || y == '?') {
      badge = "U";
    } else if (x == 'A' || y == 'A') {
      badge = "A";
    } else if (x == 'D' || y == 'D') {
      badge = "D";
    } else if (x == 'M' || y == 'M' || x == 'R' || y == 'R') {
      badge = "M";
    }

    g_autofree char *clean_path = g_strdup(path);
    g_strchomp(clean_path);
    /* Trim trailing slash on untracked directories */
    gsize plen = strlen(clean_path);
    if (plen > 1 && clean_path[plen - 1] == '/') {
      clean_path[plen - 1] = '\0';
    }
    g_hash_table_replace(map, g_strdup(clean_path), g_strdup(badge));
  }
  return map;
}

static void
ide_tree_update_git_status(IdeTree *tree)
{
  g_clear_pointer(&tree->git_status_map, g_hash_table_destroy);
  if (tree->workspace_root == NULL) {
    tree->git_status_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return;
  }

  const char *argv[] = {
    "git", "-C", tree->workspace_root, "status", "--porcelain=v1", "-uall", NULL
  };
  g_autofree char *stdout_buf = NULL;
  gint exit_status = 0;
  if (g_spawn_sync(NULL,
                   (char **)argv,
                   NULL,
                   G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                   NULL,
                   NULL,
                   &stdout_buf,
                   NULL,
                   &exit_status,
                   NULL) &&
      exit_status == 0 && stdout_buf != NULL) {
    tree->git_status_map = ide_tree_parse_git_porcelain(stdout_buf);
  } else {
    tree->git_status_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  }
}

static gint
cmp_dir_entry(gconstpointer a, gconstpointer b)
{
  const IdeTreeEntry *ea = *(const IdeTreeEntry *const *)a;
  const IdeTreeEntry *eb = *(const IdeTreeEntry *const *)b;
  if (ea->is_dir != eb->is_dir) {
    return ea->is_dir ? -1 : 1;
  }
  return g_ascii_strcasecmp(ea->name, eb->name);
}

static gboolean
should_ignore_name(const char *name)
{
  if (name == NULL || name[0] == '\0') {
    return TRUE;
  }
  return (g_strcmp0(name, ".git") == 0 ||
          g_strcmp0(name, "builddir") == 0 ||
          g_strcmp0(name, "node_modules") == 0 ||
          g_strcmp0(name, "__pycache__") == 0 ||
          g_strcmp0(name, ".cache") == 0);
}

static void
ide_tree_collect_dir_recursive(IdeTree    *tree,
                               const char *dir_path,
                               guint       depth)
{
  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (dir == NULL) {
    return;
  }

  g_autoptr(GPtrArray) local = g_ptr_array_new_with_free_func(ide_tree_entry_free);
  const char *name = NULL;
  gsize root_len = strlen(tree->workspace_root);

  while ((name = g_dir_read_name(dir)) != NULL) {
    if (should_ignore_name(name)) {
      continue;
    }
    char *abs_path = g_build_filename(dir_path, name, NULL);
    gboolean is_dir = g_file_test(abs_path, G_FILE_TEST_IS_DIR);

    const char *rel = abs_path;
    if (g_str_has_prefix(abs_path, tree->workspace_root)) {
      rel = abs_path + root_len;
      while (*rel == '/') {
        rel++;
      }
    }

    IdeTreeEntry *e = g_new0(IdeTreeEntry, 1);
    e->abs_path = abs_path;
    e->rel_path = g_strdup(rel);
    e->name = g_strdup(name);
    e->is_dir = is_dir;
    e->depth = depth;

    const char *st = g_hash_table_lookup(tree->git_status_map, e->rel_path);
    if (st != NULL) {
      g_strlcpy(e->git_status, st, sizeof(e->git_status));
    }
    g_ptr_array_add(local, e);
  }
  g_dir_close(dir);

  g_ptr_array_sort(local, cmp_dir_entry);

  for (guint i = 0; i < local->len; i++) {
    IdeTreeEntry *e = g_ptr_array_index(local, i);
    g_ptr_array_steal_index(local, i);
    i--;
    g_ptr_array_add(tree->entries, e);

    if (e->is_dir && g_hash_table_contains(tree->expanded_dirs, e->abs_path)) {
      ide_tree_collect_dir_recursive(tree, e->abs_path, depth + 1);
    }
  }
}

static void
ide_tree_ensure_row_visible(IdeTree *tree, GtkListBoxRow *row)
{
  if (!tree || !tree->scrolled || !row) return;
  GtkAdjustment *vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tree->scrolled));
  if (!vadj) return;

  graphene_point_t pt = GRAPHENE_POINT_INIT(0, 0);
  if (!gtk_widget_compute_point(
          GTK_WIDGET(row), tree->list_box, &GRAPHENE_POINT_INIT(0, 0), &pt)) {
    return;
  }
  int row_h = gtk_widget_get_height(GTK_WIDGET(row));
  if (row_h <= 0) row_h = 24;

  double cur = gtk_adjustment_get_value(vadj);
  double page = gtk_adjustment_get_page_size(vadj);
  if (page <= 0) return;

  if (pt.y < cur) {
    gtk_adjustment_set_value(vadj, pt.y);
  } else if (pt.y + row_h > cur + page) {
    gtk_adjustment_set_value(vadj, pt.y + row_h - page);
  }
}

static void
ide_tree_select_index_internal(IdeTree *tree, gint idx, gboolean scroll_into_view)
{
  if (tree->entries->len == 0) {
    tree->selected_idx = -1;
    return;
  }
  idx = CLAMP(idx, 0, (gint)tree->entries->len - 1);
  tree->selected_idx = idx;
  GtkListBoxRow *row =
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(tree->list_box), idx);
  if (row != NULL) {
    gtk_list_box_select_row(GTK_LIST_BOX(tree->list_box), row);
    if (scroll_into_view) {
      ide_tree_ensure_row_visible(tree, row);
    }
  }
}

static void
ide_tree_select_index(IdeTree *tree, gint idx)
{
  ide_tree_select_index_internal(tree, idx, TRUE);
}

static void
on_tree_row_selected(GtkListBox    *box,
                     GtkListBoxRow *row,
                     gpointer       user_data)
{
  (void)box;
  IdeTree *tree = user_data;
  if (row != NULL) {
    gint idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && (guint)idx < tree->entries->len) {
      tree->selected_idx = idx;
    }
  }
}

static void
on_tree_row_activated(GtkListBox    *box,
                      GtkListBoxRow *row,
                      gpointer       user_data)
{
  (void)box;
  IdeTree *tree = user_data;
  if (row == NULL) {
    return;
  }
  gint idx = gtk_list_box_row_get_index(row);
  if (idx < 0 || (guint)idx >= tree->entries->len) {
    return;
  }
  tree->selected_idx = idx;
  ide_tree_toggle_or_open(tree);
}

static const char *
get_symbolic_icon_for_entry(const IdeTreeEntry *e, gboolean expanded)
{
  if (e->is_dir) {
    return expanded ? "folder-open-symbolic" : "folder-symbolic";
  }
  if (g_str_has_suffix(e->name, ".c") || g_str_has_suffix(e->name, ".h") ||
      g_str_has_suffix(e->name, ".cpp") || g_str_has_suffix(e->name, ".rs") ||
      g_str_has_suffix(e->name, ".py") || g_str_has_suffix(e->name, ".go") ||
      g_str_has_suffix(e->name, ".sh")) {
    return "text-x-script-symbolic";
  }
  if (g_str_has_suffix(e->name, ".md")) {
    return "accessories-text-editor-symbolic";
  }
  return "text-x-generic-symbolic";
}

void
ide_tree_refresh(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);

  /* Preserve current scroll offset and selected path across refresh */
  GtkAdjustment *vadj =
      tree->scrolled
          ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tree->scrolled))
          : NULL;
  double saved_scroll = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

  g_autofree char *saved_path = NULL;
  if (tree->selected_idx >= 0 && (guint)tree->selected_idx < tree->entries->len) {
    const IdeTreeEntry *sel = g_ptr_array_index(tree->entries, tree->selected_idx);
    saved_path = g_strdup(sel->abs_path);
  }

  GtkWidget *child = gtk_widget_get_first_child(tree->list_box);
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(tree->list_box), child);
    child = next;
  }

  g_ptr_array_set_size(tree->entries, 0);
  if (tree->workspace_root == NULL) {
    return;
  }

  ide_tree_update_git_status(tree);
  ide_tree_collect_dir_recursive(tree, tree->workspace_root, 0);

  gint restored_idx = tree->selected_idx;

  for (guint i = 0; i < tree->entries->len; i++) {
    const IdeTreeEntry *e = g_ptr_array_index(tree->entries, i);
    if (saved_path && g_strcmp0(e->abs_path, saved_path) == 0) {
      restored_idx = (gint)i;
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focusable(row, FALSE);
    gtk_widget_add_css_class(row, "ide-tree-row");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(hbox, (int)(8 + e->depth * 14));
    gtk_widget_set_margin_end(hbox, 8);
    gtk_widget_set_margin_top(hbox, 2);
    gtk_widget_set_margin_bottom(hbox, 2);

    gboolean expanded =
        e->is_dir && g_hash_table_contains(tree->expanded_dirs, e->abs_path);

    GtkWidget *arrow_lbl =
        gtk_label_new(e->is_dir ? (expanded ? "▾" : "▸") : " ");
    gtk_widget_add_css_class(arrow_lbl, "ide-tree-arrow");
    gtk_widget_set_valign(arrow_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), arrow_lbl);

    GtkWidget *icon_img =
        gtk_image_new_from_icon_name(get_symbolic_icon_for_entry(e, expanded));
    gtk_image_set_pixel_size(GTK_IMAGE(icon_img), 14);
    gtk_widget_add_css_class(icon_img, "ide-tree-icon");
    gtk_widget_set_valign(icon_img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), icon_img);

    GtkWidget *name_lbl = gtk_label_new(e->name);
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name_lbl, TRUE);
    gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(name_lbl, "ide-tree-name");
    gtk_box_append(GTK_BOX(hbox), name_lbl);

    if (e->git_status[0] != '\0') {
      GtkWidget *badge = gtk_label_new(e->git_status);
      gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(badge, "shell-popover-badge");
      if (e->git_status[0] == 'M') {
        gtk_widget_add_css_class(badge, "ide-git-modified");
      } else if (e->git_status[0] == 'A') {
        gtk_widget_add_css_class(badge, "ide-git-added");
      } else if (e->git_status[0] == 'U') {
        gtk_widget_add_css_class(badge, "ide-git-untracked");
      } else if (e->git_status[0] == 'D') {
        gtk_widget_add_css_class(badge, "ide-git-deleted");
      }
      gtk_box_append(GTK_BOX(hbox), badge);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(GTK_LIST_BOX(tree->list_box), row);
  }

  if (tree->entries->len > 0) {
    ide_tree_select_index_internal(tree, MAX(0, restored_idx), FALSE);
  }
  if (vadj != NULL && saved_scroll > 0.0) {
    tree->has_pending_scroll = TRUE;
    tree->pending_scroll_val = saved_scroll;
    gtk_adjustment_set_value(vadj, saved_scroll);
  }
}

static void
on_tree_vadj_changed(GtkAdjustment *vadj, gpointer user_data)
{
  IdeTree *tree = user_data;
  if (!tree || !tree->has_pending_scroll) return;
  double upper = gtk_adjustment_get_upper(vadj);
  double page = gtk_adjustment_get_page_size(vadj);
  if (upper > page && upper > 10.0) {
    double target = CLAMP(tree->pending_scroll_val, 0.0, MAX(0.0, upper - page));
    tree->has_pending_scroll = FALSE;
    gtk_adjustment_set_value(vadj, target);
  }
}

IdeTree *
ide_tree_new(const char       *workspace_root,
             IdeTreeFileOpenCb open_cb,
             gpointer          user_data)
{
  IdeTree *tree = g_new0(IdeTree, 1);
  tree->open_cb = open_cb;
  tree->open_cb_data = user_data;
  tree->expanded_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  tree->entries = g_ptr_array_new_with_free_func(ide_tree_entry_free);
  tree->selected_idx = 0;

  tree->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(tree->root_box, "ide-tree-sidebar");
  gtk_widget_set_size_request(tree->root_box, 180, -1);

  /* Header strip */
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "ide-tree-header");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  tree->header_title = gtk_label_new("PROJECT TREE");
  gtk_label_set_xalign(GTK_LABEL(tree->header_title), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(tree->header_title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(tree->header_title, TRUE);
  gtk_widget_set_valign(tree->header_title, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(tree->header_title, "ide-tree-header-title");
  gtk_box_append(GTK_BOX(hdr), tree->header_title);

  GtkWidget *hint = gtk_label_new("[a/A/d/r]");
  gtk_widget_set_valign(hint, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(hint, "shell-popover-badge");
  gtk_box_append(GTK_BOX(hdr), hint);
  gtk_box_append(GTK_BOX(tree->root_box), hdr);

  /* Inline Create File / Folder Prompt Bar */
  tree->prompt_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(tree->prompt_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(pbox, "files-prompt-bar");
  gtk_widget_set_margin_start(pbox, 6);
  gtk_widget_set_margin_end(pbox, 6);
  gtk_widget_set_margin_bottom(pbox, 6);

  tree->prompt_label = gtk_label_new("NEW FILE:");
  gtk_label_set_xalign(GTK_LABEL(tree->prompt_label), 0.0f);
  gtk_widget_add_css_class(tree->prompt_label, "shell-popover-badge");
  gtk_box_append(GTK_BOX(pbox), tree->prompt_label);

  tree->prompt_entry = gtk_entry_new();
  gtk_widget_set_focusable(tree->prompt_entry, FALSE);
  gtk_widget_add_css_class(tree->prompt_entry, "shell-launcher-input");
  gtk_box_append(GTK_BOX(pbox), tree->prompt_entry);

  gtk_revealer_set_child(GTK_REVEALER(tree->prompt_revealer), pbox);
  gtk_box_append(GTK_BOX(tree->root_box), tree->prompt_revealer);

  /* Scrollable Tree ListBox */
  tree->scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree->scrolled),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(tree->scrolled, TRUE);

  tree->list_box = gtk_list_box_new();
  gtk_widget_set_focusable(tree->list_box, FALSE);
  gtk_widget_add_css_class(tree->list_box, "ide-tree-listbox");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(tree->list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(tree->list_box), FALSE);
  g_signal_connect(tree->list_box, "row-selected", G_CALLBACK(on_tree_row_selected), tree);
  g_signal_connect(tree->list_box, "row-activated", G_CALLBACK(on_tree_row_activated), tree);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree->scrolled), tree->list_box);
  GtkWidget *vp = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(tree->scrolled));
  if (GTK_IS_VIEWPORT(vp)) {
    gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(vp), FALSE);
  }
  GtkAdjustment *vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tree->scrolled));
  if (vadj != NULL) {
    g_signal_connect(vadj, "changed", G_CALLBACK(on_tree_vadj_changed), tree);
  }
  gtk_box_append(GTK_BOX(tree->root_box), tree->scrolled);

  ide_tree_set_workspace_root(tree, workspace_root);
  return tree;
}

void
ide_tree_free(IdeTree *tree)
{
  if (tree == NULL) {
    return;
  }
  g_free(tree->workspace_root);
  g_clear_pointer(&tree->expanded_dirs, g_hash_table_destroy);
  g_clear_pointer(&tree->git_status_map, g_hash_table_destroy);
  g_clear_pointer(&tree->entries, g_ptr_array_unref);
  g_free(tree);
}

GtkWidget *
ide_tree_get_widget(IdeTree *tree)
{
  g_return_val_if_fail(tree != NULL, NULL);
  return tree->root_box;
}

void
ide_tree_set_workspace_root(IdeTree *tree, const char *workspace_root)
{
  g_return_if_fail(tree != NULL);
  g_free(tree->workspace_root);
  tree->workspace_root = g_strdup(workspace_root ? workspace_root : ".");

  g_autofree char *base = g_path_get_basename(tree->workspace_root);
  g_autofree char *upper = g_ascii_strup(base, -1);
  g_autofree char *hdr = g_strdup_printf("WORKSPACE: %s", upper);
  gtk_label_set_text(GTK_LABEL(tree->header_title), hdr);

  ide_tree_refresh(tree);
}

void
ide_tree_move_up(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  ide_tree_select_index(tree, tree->selected_idx - 1);
}

void
ide_tree_move_down(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  ide_tree_select_index(tree, tree->selected_idx + 1);
}

void
ide_tree_page_up(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  ide_tree_select_index(tree, tree->selected_idx - 10);
}

void
ide_tree_page_down(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  ide_tree_select_index(tree, tree->selected_idx + 10);
}

void
ide_tree_move_top(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  ide_tree_select_index(tree, 0);
}

void
ide_tree_move_bottom(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  if (tree->entries->len > 0) {
    ide_tree_select_index(tree, (gint)tree->entries->len - 1);
  }
}

void
ide_tree_toggle_or_open(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  if (tree->selected_idx < 0 || (guint)tree->selected_idx >= tree->entries->len) {
    return;
  }
  const IdeTreeEntry *e = g_ptr_array_index(tree->entries, tree->selected_idx);
  if (e->is_dir) {
    if (g_hash_table_contains(tree->expanded_dirs, e->abs_path)) {
      g_hash_table_remove(tree->expanded_dirs, e->abs_path);
    } else {
      g_hash_table_add(tree->expanded_dirs, g_strdup(e->abs_path));
    }
    ide_tree_refresh(tree);
  } else if (tree->open_cb != NULL) {
    tree->open_cb(e->abs_path, tree->open_cb_data);
  }
}

void
ide_tree_collapse_or_parent(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  if (tree->selected_idx < 0 || (guint)tree->selected_idx >= tree->entries->len) {
    return;
  }
  const IdeTreeEntry *e = g_ptr_array_index(tree->entries, tree->selected_idx);
  if (e->is_dir && g_hash_table_contains(tree->expanded_dirs, e->abs_path)) {
    g_hash_table_remove(tree->expanded_dirs, e->abs_path);
    ide_tree_refresh(tree);
    return;
  }
  /* Move selection up to parent folder */
  if (e->depth > 0) {
    for (gint i = tree->selected_idx - 1; i >= 0; i--) {
      const IdeTreeEntry *cand = g_ptr_array_index(tree->entries, i);
      if (cand->is_dir && cand->depth < e->depth) {
        ide_tree_select_index(tree, i);
        break;
      }
    }
  }
}

void
ide_tree_prompt_create_file(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  tree->prompt_is_dir = FALSE;
  gtk_label_set_text(GTK_LABEL(tree->prompt_label), "NEW FILE (Enter / Esc):");
  gtk_editable_set_text(GTK_EDITABLE(tree->prompt_entry), "");
  gtk_revealer_set_reveal_child(GTK_REVEALER(tree->prompt_revealer), TRUE);
}

void
ide_tree_prompt_create_dir(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  tree->prompt_is_dir = TRUE;
  gtk_label_set_text(GTK_LABEL(tree->prompt_label), "NEW FOLDER (Enter / Esc):");
  gtk_editable_set_text(GTK_EDITABLE(tree->prompt_entry), "");
  gtk_revealer_set_reveal_child(GTK_REVEALER(tree->prompt_revealer), TRUE);
}

void
ide_tree_prompt_cancel(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  gtk_revealer_set_reveal_child(GTK_REVEALER(tree->prompt_revealer), FALSE);
}

static char *
ide_tree_get_selected_dir(IdeTree *tree)
{
  if (tree->selected_idx >= 0 && (guint)tree->selected_idx < tree->entries->len) {
    const IdeTreeEntry *e = g_ptr_array_index(tree->entries, tree->selected_idx);
    if (e->is_dir) {
      return g_strdup(e->abs_path);
    }
    return g_path_get_dirname(e->abs_path);
  }
  return g_strdup(tree->workspace_root ? tree->workspace_root : ".");
}

gboolean
ide_tree_prompt_confirm(IdeTree *tree)
{
  g_return_val_if_fail(tree != NULL, FALSE);
  const char *name = gtk_editable_get_text(GTK_EDITABLE(tree->prompt_entry));
  if (name == NULL || name[0] == '\0') {
    ide_tree_prompt_cancel(tree);
    return FALSE;
  }

  g_autofree char *parent_dir = ide_tree_get_selected_dir(tree);
  g_autofree char *target = g_build_filename(parent_dir, name, NULL);
  gboolean ok = FALSE;

  if (tree->prompt_is_dir) {
    ok = (g_mkdir_with_parents(target, 0755) == 0);
  } else {
    g_autofree char *dir_of_target = g_path_get_dirname(target);
    g_mkdir_with_parents(dir_of_target, 0755);
    ok = g_file_set_contents(target, "", 0, NULL);
  }

  gtk_revealer_set_reveal_child(GTK_REVEALER(tree->prompt_revealer), FALSE);
  g_hash_table_add(tree->expanded_dirs, g_strdup(parent_dir));
  ide_tree_refresh(tree);

  if (ok && !tree->prompt_is_dir && tree->open_cb != NULL) {
    tree->open_cb(target, tree->open_cb_data);
  }
  return ok;
}

void
ide_tree_prompt_append_char(IdeTree *tree, const char *ch)
{
  g_return_if_fail(tree != NULL);
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(tree->prompt_entry));
  g_autofree char *next = g_strconcat(cur ? cur : "", ch ? ch : "", NULL);
  gtk_editable_set_text(GTK_EDITABLE(tree->prompt_entry), next);
}

void
ide_tree_prompt_backspace(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(tree->prompt_entry));
  if (cur == NULL || cur[0] == '\0') {
    return;
  }
  glong chars = g_utf8_strlen(cur, -1);
  if (chars <= 1) {
    gtk_editable_set_text(GTK_EDITABLE(tree->prompt_entry), "");
    return;
  }
  const char *end = g_utf8_offset_to_pointer(cur, chars - 1);
  g_autofree char *trimmed = g_strndup(cur, (gsize)(end - cur));
  gtk_editable_set_text(GTK_EDITABLE(tree->prompt_entry), trimmed);
}

void
ide_tree_delete_selected(IdeTree *tree)
{
  g_return_if_fail(tree != NULL);
  if (tree->selected_idx < 0 || (guint)tree->selected_idx >= tree->entries->len) {
    return;
  }
  const IdeTreeEntry *e = g_ptr_array_index(tree->entries, tree->selected_idx);
  g_autoptr(GFile) f = g_file_new_for_path(e->abs_path);
  /* Prefer moving to trash so user files are recoverable */
  if (!g_file_trash(f, NULL, NULL)) {
    g_file_delete(f, NULL, NULL);
  }
  ide_tree_refresh(tree);
}
