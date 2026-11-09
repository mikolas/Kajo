#ifndef KAJO_MOD_WINDOW_H
#define KAJO_MOD_WINDOW_H

#include <gtk/gtk.h>
#include "mod_vis.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KAJO_TYPE_MOD_WINDOW (kajo_mod_window_get_type())
G_DECLARE_FINAL_TYPE(KajoModWindow, kajo_mod_window, KAJO, MOD_WINDOW, GtkApplicationWindow)

KajoModWindow *kajo_mod_window_new(GtkApplication *app, int initial_width, int initial_height);
void           kajo_mod_window_set_simulated_column_width(KajoModWindow *self, int col_width);
void           kajo_mod_window_open_paths(KajoModWindow *self, const char * const *paths, int n_paths);

#ifdef __cplusplus
}
#endif

#endif /* KAJO_MOD_WINDOW_H */
