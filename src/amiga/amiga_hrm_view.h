#ifndef KAJO_AMIGA_HRM_VIEW_H
#define KAJO_AMIGA_HRM_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_HRM_VIEW (amiga_hrm_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaHrmView, amiga_hrm_view, AMIGA, HRM_VIEW, GtkBox)

AmigaHrmView *amiga_hrm_view_new(void);

void amiga_hrm_view_set_hrm_dir(AmigaHrmView *self, const char *hrm_dir);
void amiga_hrm_view_lookup(AmigaHrmView *self, const char *query);

G_END_DECLS

#endif /* KAJO_AMIGA_HRM_VIEW_H */
