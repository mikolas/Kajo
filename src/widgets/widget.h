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

static inline void
shell_spawn_settings(const char *page, GtkWidget *popover)
{
    if (popover && GTK_IS_POPOVER(popover)) {
        gtk_popover_popdown(GTK_POPOVER(popover));
    }

    g_autofree gchar *bin = g_find_program_in_path("kajo-settings");
    if (!bin) {
        g_autofree gchar *local_bin = g_build_filename(
            g_get_home_dir(), ".local", "bin", "kajo-settings", NULL);
        if (g_file_test(local_bin, G_FILE_TEST_IS_EXECUTABLE)) {
            bin = g_steal_pointer(&local_bin);
        }
    }
    if (!bin && g_file_test("/usr/bin/kajo-settings", G_FILE_TEST_IS_EXECUTABLE)) {
        bin = g_strdup("/usr/bin/kajo-settings");
    }
    if (!bin) {
        bin = g_strdup("kajo-settings");
    }

    g_autofree gchar *cmd = (page && *page)
        ? g_strdup_printf("%s --page %s", bin, page)
        : g_strdup(bin);

    g_spawn_command_line_async(cmd, NULL);
}

#ifdef __cplusplus
}
#endif

#endif /* SHELL_WIDGET_H */
