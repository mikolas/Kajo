#ifndef KAJO_IDE_EDITOR_OPS_H
#define KAJO_IDE_EDITOR_OPS_H

/* Shared window-agnostic modal buffer motion & edit executor for kajo-ide and
 * sibling editor windows (e.g. kajo-amiga). */

#include "ide_buffer.h"
#include "ide_fsm.h"
#include <gtksourceview/gtksource.h>

G_BEGIN_DECLS

typedef enum {
  IDE_EDITOR_OP_UNHANDLED = 0,
  IDE_EDITOR_OP_DONE,
  IDE_EDITOR_OP_SCROLL,
  IDE_EDITOR_OP_POST_EDIT,
} IdeEditorOpOutcome;

typedef struct {
  GtkSourceView *source_view;
  IdeBuffer     *buffer;
  IdeFsm        *fsm;
  char         **yank_register;
} IdeEditorHost;

IdeEditorOpOutcome ide_editor_ops_execute(IdeEditorHost   *host,
                                          const IdeAction *act,
                                          const char     **out_toast);

char              *ide_editor_translate_gdk_key(guint keyval, GdkModifierType state);

G_END_DECLS

#endif /* KAJO_IDE_EDITOR_OPS_H */
