#ifndef KAJO_AMIGA_PROJECT_UI_H
#define KAJO_AMIGA_PROJECT_UI_H

#include "amiga_project.h"
#include <gtk/gtk.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef void (*AmigaActionCb)(gpointer user_data);
typedef void (*AmigaDirSelectedCb)(const char *dir_path, gpointer user_data);
typedef void (*AmigaProjectSavedCb)(AmigaProject *proj, gpointer user_data);

/* 1. Welcome / Start Screen */
GtkWidget *amiga_welcome_view_new(const AmigaToolchainStatus *tc,
                                  AmigaActionCb               on_new,
                                  AmigaActionCb               on_open,
                                  AmigaDirSelectedCb          on_open_recent,
                                  AmigaActionCb               on_attach,
                                  AmigaActionCb               on_settings,
                                  gpointer                    user_data);

/* 2. Centered Aero + Metro New Project Wizard Modal (Ctrl+Shift+N) */
void amiga_project_wizard_present(GtkWindow                  *parent,
                                  const AmigaToolchainStatus *tc,
                                  AmigaDirSelectedCb          on_created,
                                  gpointer                    user_data);

/* 3. Center-Pane [⚙ kajo-amiga.toml] Setup View with [ FORM | RAW TOML ] + 4 tabs */
#define AMIGA_TYPE_PROJECT_SETUP_VIEW (amiga_project_setup_view_get_type())
G_DECLARE_FINAL_TYPE(AmigaProjectSetupView, amiga_project_setup_view, AMIGA, PROJECT_SETUP_VIEW, GtkBox)

AmigaProjectSetupView *amiga_project_setup_view_new(AmigaProjectSavedCb on_saved,
                                                    gpointer            user_data);
void                   amiga_project_setup_view_bind(AmigaProjectSetupView      *self,
                                                     AmigaProject               *proj,
                                                     const AmigaToolchainStatus *tc);

G_END_DECLS

#endif /* KAJO_AMIGA_PROJECT_UI_H */
