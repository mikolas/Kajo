#ifndef SHELL_POSTURE_MANAGER_H
#define SHELL_POSTURE_MANAGER_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellApp            ShellApp;
typedef struct _ShellPostureManager ShellPostureManager;

typedef enum {
    SHELL_POSTURE_UNKNOWN = 0,
    SHELL_POSTURE_LAPTOP,
    SHELL_POSTURE_FOLDED
} ShellPostureState;

ShellPostureManager *shell_posture_manager_new(ShellApp *app);
void                 shell_posture_manager_destroy(ShellPostureManager *self);
void                 shell_posture_manager_reload_config(ShellPostureManager *self);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_POSTURE_MANAGER_H */
