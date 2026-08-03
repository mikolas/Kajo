#ifndef SHELL_OSD_H
#define SHELL_OSD_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellOSD ShellOSD;

ShellOSD *shell_osd_new(void);
void      shell_osd_show_volume(ShellOSD *osd, gint percent, gboolean muted);
void      shell_osd_show_brightness(ShellOSD *osd, gint percent);
void      shell_osd_destroy(ShellOSD *osd);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_OSD_H */
