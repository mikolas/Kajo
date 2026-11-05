#ifndef KAJO_IDE_TREE_H
#define KAJO_IDE_TREE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _IdeTree IdeTree;

typedef void (*IdeTreeFileOpenCb)(const char *abs_path, gpointer user_data);

/* Pure testable Git porcelain status parser: maps relative path -> "M", "A", "U", "D" */
GHashTable *ide_tree_parse_git_porcelain(const char *porcelain_text);

IdeTree    *ide_tree_new(const char       *workspace_root,
                         IdeTreeFileOpenCb open_cb,
                         gpointer          user_data);
void        ide_tree_free(IdeTree *tree);

GtkWidget  *ide_tree_get_widget(IdeTree *tree);
void        ide_tree_set_workspace_root(IdeTree *tree, const char *workspace_root);
void        ide_tree_refresh(IdeTree *tree);
char       *ide_tree_format_git_summary(IdeTree *tree);

void        ide_tree_move_up(IdeTree *tree);
void        ide_tree_move_down(IdeTree *tree);
void        ide_tree_page_up(IdeTree *tree);
void        ide_tree_page_down(IdeTree *tree);
void        ide_tree_move_top(IdeTree *tree);
void        ide_tree_move_bottom(IdeTree *tree);
void        ide_tree_toggle_or_open(IdeTree *tree);
void        ide_tree_collapse_or_parent(IdeTree *tree);

void        ide_tree_prompt_create_file(IdeTree *tree);
void        ide_tree_prompt_create_dir(IdeTree *tree);
void        ide_tree_prompt_cancel(IdeTree *tree);
gboolean    ide_tree_prompt_confirm(IdeTree *tree);
void        ide_tree_prompt_append_char(IdeTree *tree, const char *ch);
void        ide_tree_prompt_backspace(IdeTree *tree);
void        ide_tree_delete_selected(IdeTree *tree);

G_END_DECLS

#endif /* KAJO_IDE_TREE_H */
