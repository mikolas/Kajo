#ifndef SHELL_IDLE_MANAGER_H
#define SHELL_IDLE_MANAGER_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellApp         ShellApp;
typedef struct _ShellIdleManager ShellIdleManager;

typedef void (*ShellIdleInhibitChangedFunc)(gboolean inhibited, gpointer user_data);

ShellIdleManager *shell_idle_manager_new(ShellApp *app);
void              shell_idle_manager_destroy(ShellIdleManager *self);

void              shell_idle_manager_reload_config(ShellIdleManager *self);

void              shell_idle_manager_set_inhibited(ShellIdleManager *self, gboolean inhibited);
gboolean          shell_idle_manager_get_inhibited(ShellIdleManager *self);

void              shell_idle_manager_add_listener(ShellIdleManager           *self,
                                                  ShellIdleInhibitChangedFunc callback,
                                                  gpointer                    user_data);
void              shell_idle_manager_remove_listener(ShellIdleManager           *self,
                                                     ShellIdleInhibitChangedFunc callback,
                                                     gpointer                    user_data);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_IDLE_MANAGER_H */
