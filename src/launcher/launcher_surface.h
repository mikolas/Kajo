#ifndef SHELL_LAUNCHER_SURFACE_H
#define SHELL_LAUNCHER_SURFACE_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellLauncherSurface ShellLauncherSurface;
typedef struct _ShellCompositor ShellCompositor;

ShellLauncherSurface *shell_launcher_surface_new(ShellCompositor *compositor);
void                  shell_launcher_surface_show(ShellLauncherSurface *ls);
void                  shell_launcher_surface_hide(ShellLauncherSurface *ls);
void                  shell_launcher_surface_toggle(ShellLauncherSurface *ls);
void                  shell_launcher_surface_destroy(ShellLauncherSurface *ls);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_LAUNCHER_SURFACE_H */
