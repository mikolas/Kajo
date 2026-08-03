#ifndef SHELL_APP_H
#define SHELL_APP_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellConfig          ShellConfig;
typedef struct _ShellTheme           ShellTheme;
typedef struct _ShellPanel           ShellPanel;
typedef struct _ShellWidgetRegistry  ShellWidgetRegistry;
typedef struct _ShellCompositor      ShellCompositor;
typedef struct _ShellOSD             ShellOSD;
typedef struct _ShellNotificationDaemon ShellNotificationDaemon;
typedef struct _ShellLauncherSurface ShellLauncherSurface;
typedef struct _ShellIPCSocket       ShellIPCSocket;

typedef struct _ShellApp ShellApp;

struct _ShellApp {
    GtkApplication          *gtk_app;
    ShellConfig             *config;
    ShellTheme              *theme;
    ShellPanel              *panel;
    ShellWidgetRegistry     *registry;
    ShellCompositor         *compositor;
    ShellOSD                *osd;
    ShellNotificationDaemon *notifications;
    ShellLauncherSurface    *launcher;
    ShellIPCSocket          *ipc;
    GPtrArray               *active_widgets;
};

ShellApp    *shell_app_new(GtkApplication *app);
void         shell_app_activate(ShellApp *self);
void         shell_app_destroy(ShellApp *self);
ShellConfig *shell_app_get_config(ShellApp *self);
ShellNotificationDaemon *shell_app_get_notification_daemon(ShellApp *self);
ShellPanel  *shell_app_get_panel(ShellApp *self);
ShellCompositor *shell_app_get_compositor(ShellApp *self);
ShellOSD    *shell_app_get_osd(ShellApp *self);
void         shell_brightness_change(ShellApp *self, gint delta_percent);
void         shell_volume_change(ShellApp *self, gint delta_percent);
void         shell_volume_toggle_mute(ShellApp *self);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_APP_H */
