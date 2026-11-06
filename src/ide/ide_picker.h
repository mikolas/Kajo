#ifndef KAJO_IDE_PICKER_H
#define KAJO_IDE_PICKER_H

#include "ide_fsm.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  IDE_PICKER_FILES = 0,
  IDE_PICKER_BUFFERS,
  IDE_PICKER_GREP,
  IDE_PICKER_HELP
} IdePickerMode;

typedef struct _IdePicker IdePicker;

typedef void (*IdePickerOpenFileCb)(const char *abs_path,
                                    guint       line_1based,
                                    guint       col_1based,
                                    gpointer    user_data);

typedef void (*IdePickerCommandCb)(IdeActionType action, gpointer user_data);

/* Pure testable subsequence fuzzy scorer (>0 on match, 0 on mismatch) */
gint       ide_fuzzy_score(const char *query, const char *candidate);

IdePicker *ide_picker_new(const char         *workspace_root,
                          IdePickerOpenFileCb open_file_cb,
                          IdePickerCommandCb  command_cb,
                          gpointer            user_data);
void       ide_picker_free(IdePicker *picker);

GtkWidget    *ide_picker_get_widget(IdePicker *picker);
gboolean      ide_picker_is_visible(const IdePicker *picker);
IdePickerMode ide_picker_get_mode(const IdePicker *picker);
gboolean      ide_picker_has_query(const IdePicker *picker);

void       ide_picker_open_files(IdePicker *picker, const char *workspace_root);
void       ide_picker_open_buffers(IdePicker *picker, GPtrArray *open_buffers);
void       ide_picker_open_grep(IdePicker *picker, const char *workspace_root);
void       ide_picker_open_help(IdePicker *picker);
void       ide_picker_close(IdePicker *picker);

void       ide_picker_move_next(IdePicker *picker);
void       ide_picker_move_prev(IdePicker *picker);
void       ide_picker_page_down(IdePicker *picker);
void       ide_picker_page_up(IdePicker *picker);
void       ide_picker_append_input(IdePicker *picker, const char *ch);
void       ide_picker_backspace(IdePicker *picker);
void       ide_picker_confirm(IdePicker *picker);

G_END_DECLS

#endif /* KAJO_IDE_PICKER_H */
