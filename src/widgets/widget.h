#ifndef SHELL_WIDGET_H
#define SHELL_WIDGET_H

#include <gtk/gtk.h>
#include "../config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellCompositor ShellCompositor;
typedef struct _ShellApp ShellApp;

typedef struct _ShellWidgetClass ShellWidgetClass;

typedef struct _ShellWidget {
    const ShellWidgetClass *klass;
    ShellWidgetMode         mode;
    ShellApp               *app;
} ShellWidget;

struct _ShellWidgetClass {
    const char *id;
    const char *name;
    ShellWidget *(*create)(ShellCompositor *compositor);
    void (*destroy)(ShellWidget *widget);
    GtkWidget *(*get_widget)(ShellWidget *widget);
    void (*enable)(ShellWidget *widget);
    void (*disable)(ShellWidget *widget);
};

static inline void
shell_widget_apply_mode_visibility(ShellWidgetMode mode, GtkWidget *icon_img, GtkWidget *label)
{
    if (mode == WIDGET_MODE_ICON) {
        if (icon_img) gtk_widget_set_visible(icon_img, TRUE);
        if (label) gtk_widget_set_visible(label, FALSE);
    } else if (mode == WIDGET_MODE_LABEL) {
        if (icon_img) gtk_widget_set_visible(icon_img, FALSE);
        if (label) gtk_widget_set_visible(label, TRUE);
    } else { /* WIDGET_MODE_FULL */
        if (icon_img) gtk_widget_set_visible(icon_img, TRUE);
        if (label) gtk_widget_set_visible(label, TRUE);
    }
}

ShellConfig *shell_app_get_config(ShellApp *self);

static inline gint
shell_widget_get_icon_size(ShellWidget *widget)
{
    if (widget && widget->app) {
        ShellConfig *cfg = shell_app_get_config(widget->app);
        if (cfg && cfg->icon_size > 0)
            return cfg->icon_size;
    }
    return 24;
}

#ifdef __cplusplus
}
#endif

#endif /* SHELL_WIDGET_H */
