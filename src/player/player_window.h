#ifndef KAJO_PLAYER_WINDOW_H
#define KAJO_PLAYER_WINDOW_H

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define KAJO_TYPE_PLAYER_WINDOW (kajo_player_window_get_type())
G_DECLARE_FINAL_TYPE(KajoPlayerWindow, kajo_player_window, KAJO, PLAYER_WINDOW, GtkApplicationWindow)

KajoPlayerWindow *kajo_player_window_new(GtkApplication *app);

void kajo_player_window_open_file(KajoPlayerWindow *self, GFile *file);
void kajo_player_window_open_files(KajoPlayerWindow *self, GFile **files, int n_files);

G_END_DECLS

#endif /* KAJO_PLAYER_WINDOW_H */
