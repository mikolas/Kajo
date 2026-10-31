#ifndef SHELL_SCREENSAVER_H
#define SHELL_SCREENSAVER_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellApp         ShellApp;
typedef struct _ShellScreensaver ShellScreensaver;

ShellScreensaver *shell_screensaver_new(ShellApp *app);
void              shell_screensaver_destroy(ShellScreensaver *self);

void              shell_screensaver_start(ShellScreensaver *self);
void              shell_screensaver_stop(ShellScreensaver *self);
gboolean          shell_screensaver_is_active(ShellScreensaver *self);

void              shell_screensaver_set_dpms(gboolean power_on);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_SCREENSAVER_H */
