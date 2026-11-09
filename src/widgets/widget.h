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
        if (label) gtk_widget_set_visible(label, icon_img == NULL);
    } else if (mode == WIDGET_MODE_LABEL) {
        if (icon_img) gtk_widget_set_visible(icon_img, label == NULL);
        if (label) gtk_widget_set_visible(label, TRUE);
    } else { /* WIDGET_MODE_FULL */
        if (icon_img) gtk_widget_set_visible(icon_img, TRUE);
        if (label) gtk_widget_set_visible(label, TRUE);
    }
}

static inline GtkWidget *
shell_widget_pack_bar_box(GtkWidget *icon_img, GtkWidget *label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    if (icon_img) {
        gtk_widget_set_valign(icon_img, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), icon_img);
    }
    if (label) {
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), label);
    }
    return box;
}

static inline GtkPopover *
shell_widget_create_popover_shell(const char  *width_class,
                                  const char  *title_text,
                                  const char  *badge_text,
                                  GtkWidget  **out_badge,
                                  GtkWidget  **out_body,
                                  GtkWidget  **out_footer)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "shell-popover");

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(outer, (width_class && *width_class) ? width_class : "popover-compact");

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(header, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(header, "shell-popover-header");

    GtkWidget *title_lbl = gtk_label_new(title_text ? title_text : "");
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0f);
    gtk_widget_set_hexpand(title_lbl, TRUE);
    gtk_widget_set_valign(title_lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(title_lbl, "shell-popover-title");
    gtk_box_append(GTK_BOX(header), title_lbl);

    if (badge_text || out_badge) {
        GtkWidget *badge_lbl = gtk_label_new(badge_text ? badge_text : "[ -- ]");
        gtk_widget_set_valign(badge_lbl, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(badge_lbl, "shell-popover-badge");
        gtk_box_append(GTK_BOX(header), badge_lbl);
        if (out_badge)
            *out_badge = badge_lbl;
    }

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(body, "shell-popover-body");
    if (out_body)
        *out_body = body;

    gtk_box_append(GTK_BOX(outer), header);
    gtk_box_append(GTK_BOX(outer), body);

    if (out_footer) {
        GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(footer, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(footer, "shell-popover-footer");
        gtk_box_append(GTK_BOX(outer), footer);
        *out_footer = footer;
    }

    gtk_popover_set_child(GTK_POPOVER(popover), outer);
    return GTK_POPOVER(popover);
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

void volume_widget_change_volume(ShellWidget *widget, gint delta_percent);
void volume_widget_toggle_mute(ShellWidget *widget);
void custom_script_widget_configure(ShellWidget *widget, const ShellWidgetConfig *wc);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_WIDGET_H */
