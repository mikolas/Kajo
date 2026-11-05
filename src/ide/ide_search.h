#ifndef KAJO_IDE_SEARCH_H
#define KAJO_IDE_SEARCH_H

#include "ide_buffer.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  char           *file_path;
  guint           line_1based;
  guint           col_1based;
  IdeDiagSeverity severity;
  char           *message;
} IdeBuildIssue;

typedef struct _IdeSearchDrawer IdeSearchDrawer;

typedef void (*IdeSearchJumpCb)(const char *abs_path,
                                guint       line_1based,
                                guint       col_1based,
                                gpointer    user_data);

typedef void (*IdeSearchStatusCb)(const char *status_msg, gpointer user_data);

/* Pure testable Search/Replace & Compiler Error Regex Parser */
char          *ide_replace_text_all(const char *source_text,
                                    const char *find_str,
                                    const char *replace_str,
                                    gboolean    case_sensitive,
                                    gboolean    whole_word,
                                    gboolean    use_regex,
                                    guint      *out_match_count);

IdeBuildIssue *ide_parse_compiler_error_line(const char *workspace_root,
                                             const char *raw_line);
void           ide_build_issue_free(gpointer data);

/* Ephemeral Drawer UI for Build/Quickfix (Space o) and Search & Replace (Space s r / s p) */
IdeSearchDrawer *ide_search_drawer_new(const char       *workspace_root,
                                       IdeSearchJumpCb   jump_cb,
                                       IdeSearchStatusCb status_cb,
                                       gpointer          user_data);
void             ide_search_drawer_free(IdeSearchDrawer *drawer);

GtkWidget       *ide_search_drawer_get_widget(IdeSearchDrawer *drawer);
void             ide_search_drawer_set_workspace_root(IdeSearchDrawer *drawer,
                                                      const char      *workspace_root);

void             ide_search_drawer_show_build_output(IdeSearchDrawer *drawer);
void             ide_search_drawer_run_build(IdeSearchDrawer *drawer);
void             ide_search_drawer_append_output_line(IdeSearchDrawer *drawer,
                                                      const char      *line);

void             ide_search_drawer_open_replace(IdeSearchDrawer *drawer,
                                                IdeBuffer       *active_buf,
                                                gboolean         project_wide);

void             ide_search_drawer_move_down(IdeSearchDrawer *drawer);
void             ide_search_drawer_move_up(IdeSearchDrawer *drawer);
void             ide_search_drawer_jump_selected(IdeSearchDrawer *drawer);
gboolean         ide_search_drawer_step_error(IdeSearchDrawer *drawer, int delta);

void             ide_search_drawer_next_field(IdeSearchDrawer *drawer);
void             ide_search_drawer_prev_field(IdeSearchDrawer *drawer);
void             ide_search_drawer_toggle_case(IdeSearchDrawer *drawer);
void             ide_search_drawer_toggle_word(IdeSearchDrawer *drawer);
void             ide_search_drawer_toggle_regex(IdeSearchDrawer *drawer);
void             ide_search_drawer_append_char(IdeSearchDrawer *drawer, const char *ch);
void             ide_search_drawer_backspace(IdeSearchDrawer *drawer);
guint            ide_search_drawer_apply_all(IdeSearchDrawer *drawer, GPtrArray *open_buffers);

G_END_DECLS

#endif /* KAJO_IDE_SEARCH_H */
