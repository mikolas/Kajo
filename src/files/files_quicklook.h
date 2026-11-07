#ifndef KAJO_FILES_QUICKLOOK_H
#define KAJO_FILES_QUICKLOOK_H

#include <gtk/gtk.h>
#include "files_model.h"

G_BEGIN_DECLS

typedef enum {
    KAJO_QL_ACTION_OPEN_DEFAULT = 0,
    KAJO_QL_ACTION_OPEN_TERMINAL,
    KAJO_QL_ACTION_RENAME,
    KAJO_QL_ACTION_TRASH,
    KAJO_QL_ACTION_TOGGLE_EXEC,
    KAJO_QL_ACTION_PREV_ITEM,
    KAJO_QL_ACTION_NEXT_ITEM,
} KajoQuickLookAction;

typedef void (*KajoQuickLookActionCallback)(KajoQuickLookAction action,
                                            KajoFileItem *item,
                                            gpointer user_data);

typedef struct _KajoQuickLook KajoQuickLook;

KajoQuickLook *kajo_quicklook_new(GtkOverlay *parent_overlay,
                                  GtkWidget *inspector_container,
                                  KajoQuickLookActionCallback action_cb,
                                  gpointer user_data);

void kajo_quicklook_free(KajoQuickLook *self);

/* Updates the right-hand Inspector card AND (if visible) the Space Quick Look modal */
void kajo_quicklook_set_item(KajoQuickLook *self, KajoFileItem *item, GFile *current_dir);

/* Toggles / shows / hides the Space Quick Look modal overlay */
void     kajo_quicklook_toggle(KajoQuickLook *self);
void     kajo_quicklook_show(KajoQuickLook *self);
void     kajo_quicklook_hide(KajoQuickLook *self);
gboolean kajo_quicklook_is_visible(const KajoQuickLook *self);

G_END_DECLS

#endif /* KAJO_FILES_QUICKLOOK_H */
