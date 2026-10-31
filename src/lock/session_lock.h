#ifndef SHELL_SESSION_LOCK_H
#define SHELL_SESSION_LOCK_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellApp         ShellApp;
typedef struct _ShellSessionLock ShellSessionLock;

ShellSessionLock *shell_session_lock_new(ShellApp *app);
void              shell_session_lock_destroy(ShellSessionLock *self);

void              shell_session_lock_activate(ShellSessionLock *self);
void              shell_session_lock_unlock(ShellSessionLock *self);
gboolean          shell_session_lock_is_locked(ShellSessionLock *self);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_SESSION_LOCK_H */
