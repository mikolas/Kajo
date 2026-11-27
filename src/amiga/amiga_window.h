#ifndef KAJO_AMIGA_WINDOW_H
#define KAJO_AMIGA_WINDOW_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

#define KAJO_TYPE_AMIGA_WINDOW (kajo_amiga_window_get_type())
G_DECLARE_FINAL_TYPE(KajoAmigaWindow, kajo_amiga_window, KAJO, AMIGA_WINDOW, GtkApplicationWindow)

KajoAmigaWindow *kajo_amiga_window_new(GtkApplication *app);

void kajo_amiga_window_open_project_dir(KajoAmigaWindow *self, const char *dir_path);
void kajo_amiga_window_open_attach(KajoAmigaWindow *self, const char *host, uint16_t port);

/* Deterministic screenshot/preview states for scripts/ui-shot.sh:
 * "welcome" | "project" | "attach" | "setup" | "wizard" */
void kajo_amiga_window_set_preview_mode(KajoAmigaWindow *self, const char *mode);

G_END_DECLS

#endif /* KAJO_AMIGA_WINDOW_H */
