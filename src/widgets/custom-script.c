#include "widget.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const ShellWidgetClass custom_script_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;

    gchar *exec_cmd;
    gchar *click_left_cmd;
    gchar *click_right_cmd;
    gint interval_sec;
    guint timer_id;
    guint watch_id;
    gpointer active_sd;
} CustomScriptWidget;

static void run_script_update(CustomScriptWidget *cs);

static void
on_script_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    CustomScriptWidget *cs = user_data;
    if (cs && cs->click_left_cmd != NULL && cs->click_left_cmd[0] != '\0') {
        g_spawn_command_line_async(cs->click_left_cmd, NULL);
    }
}

static void
on_script_right_clicked(GtkGestureClick *gesture G_GNUC_UNUSED,
                        gint n_press G_GNUC_UNUSED,
                        gdouble x G_GNUC_UNUSED,
                        gdouble y G_GNUC_UNUSED,
                        gpointer user_data)
{
    CustomScriptWidget *cs = user_data;
    if (cs && cs->click_right_cmd != NULL && cs->click_right_cmd[0] != '\0') {
        g_spawn_command_line_async(cs->click_right_cmd, NULL);
    }
}

void
custom_script_widget_configure(ShellWidget *widget, const ShellWidgetConfig *wc)
{
    CustomScriptWidget *cs = (CustomScriptWidget *)widget;
    if (!cs || !wc)
        return;

    if (wc->interval_sec > 0)
        cs->interval_sec = wc->interval_sec;

    if (wc->exec_cmd) {
        g_free(cs->exec_cmd);
        cs->exec_cmd = g_strdup(wc->exec_cmd);
    }
    if (wc->click_left_cmd) {
        g_free(cs->click_left_cmd);
        cs->click_left_cmd = g_strdup(wc->click_left_cmd);
    }
    if (wc->click_right_cmd) {
        g_free(cs->click_right_cmd);
        cs->click_right_cmd = g_strdup(wc->click_right_cmd);
    }
}

typedef struct {
    CustomScriptWidget *cs;
    GIOChannel *channel;
} ScriptData;

static void
script_data_free(gpointer user_data)
{
    ScriptData *sd = user_data;
    if (sd) {
        if (sd->cs && sd->cs->active_sd == sd) {
            sd->cs->active_sd = NULL;
        }
        if (sd->channel) {
            g_io_channel_unref(sd->channel);
            sd->channel = NULL;
        }
        g_free(sd);
    }
}

static gboolean
on_script_channel_io(GIOChannel *source, GIOCondition condition G_GNUC_UNUSED, gpointer user_data)
{
    ScriptData *sd = user_data;
    gchar *line = NULL;
    gsize length = 0;

    if (g_io_channel_read_line(source, &line, &length, NULL, NULL) == G_IO_STATUS_NORMAL && line) {
        g_strstrip(line);
        if (sd && sd->cs && sd->cs->label) {
            gtk_label_set_text(GTK_LABEL(sd->cs->label), line);
            shell_widget_apply_mode_visibility(sd->cs->base.mode, sd->cs->icon_img, sd->cs->label);
        }
        g_free(line);
    }

    if (sd && sd->cs) {
        sd->cs->watch_id = 0;
    }
    return G_SOURCE_REMOVE;
}

static void
run_script_update(CustomScriptWidget *cs)
{
    if (cs->exec_cmd == NULL || cs->exec_cmd[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(cs->label), "Custom");
        return;
    }

    gchar **argv = NULL;
    gint argc = 0;
    if (!g_shell_parse_argv(cs->exec_cmd, &argc, &argv, NULL)) return;

    gint out_fd = -1;
    GPid pid = 0;
    if (g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &pid, NULL, &out_fd, NULL, NULL)) {
        g_spawn_close_pid(pid);
        GIOChannel *channel = g_io_channel_unix_new(out_fd);
        g_io_channel_set_close_on_unref(channel, TRUE);
        g_io_channel_set_encoding(channel, NULL, NULL);

        if (cs->active_sd) {
            ((ScriptData *)cs->active_sd)->cs = NULL;
            cs->active_sd = NULL;
        }
        if (cs->watch_id != 0) {
            g_source_remove(cs->watch_id);
            cs->watch_id = 0;
        }

        ScriptData *sd = g_new0(ScriptData, 1);
        sd->cs = cs;
        sd->channel = channel;
        cs->active_sd = sd;
        cs->watch_id = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, G_IO_IN | G_IO_HUP, on_script_channel_io, sd, script_data_free);
    }
    g_strfreev(argv);
}

static gboolean
on_cs_timer_tick(gpointer user_data)
{
    CustomScriptWidget *cs = user_data;
    run_script_update(cs);
    return G_SOURCE_CONTINUE;
}

static ShellWidget *
custom_script_widget_create(ShellCompositor *compositor)
{
    CustomScriptWidget *cs = g_new0(CustomScriptWidget, 1);
    cs->base.klass = &custom_script_widget_class;
    cs->compositor = compositor;
    cs->interval_sec = 5;
    cs->exec_cmd = g_strdup("echo 'Script'");

    cs->button = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(cs->button), FALSE);
    gtk_widget_add_css_class(cs->button, "shell-widget");
    gtk_widget_add_css_class(cs->button, "shell-widget-custom");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    cs->icon_img = gtk_image_new_from_icon_name("fl-terminal-symbolic");
    cs->label = gtk_label_new("Script");

    gtk_box_append(GTK_BOX(hbox), cs->icon_img);
    gtk_box_append(GTK_BOX(hbox), cs->label);
    gtk_button_set_child(GTK_BUTTON(cs->button), hbox);

    g_signal_connect(cs->button, "clicked", G_CALLBACK(on_script_btn_clicked), cs);

    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), 3);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_script_right_clicked), cs);
    gtk_widget_add_controller(cs->button, GTK_EVENT_CONTROLLER(right_click));

    return (ShellWidget *)cs;
}

static void
custom_script_widget_destroy(ShellWidget *widget)
{
    CustomScriptWidget *cs = (CustomScriptWidget *)widget;
    if (cs == NULL)
        return;

    if (cs->button) {
        g_signal_handlers_disconnect_by_data(cs->button, cs);
    }

    if (cs->timer_id != 0) {
        g_source_remove(cs->timer_id);
        cs->timer_id = 0;
    }

    if (cs->active_sd) {
        ((ScriptData *)cs->active_sd)->cs = NULL;
        cs->active_sd = NULL;
    }

    if (cs->watch_id != 0) {
        g_source_remove(cs->watch_id);
        cs->watch_id = 0;
    }

    g_free(cs->exec_cmd);
    g_free(cs->click_left_cmd);
    g_free(cs->click_right_cmd);
    g_free(cs);
}

static GtkWidget *
custom_script_widget_get_widget(ShellWidget *widget)
{
    CustomScriptWidget *cs = (CustomScriptWidget *)widget;
    return cs->button;
}

static void
custom_script_widget_enable(ShellWidget *widget)
{
    CustomScriptWidget *cs = (CustomScriptWidget *)widget;
    if (!cs)
        return;

    if (cs->icon_img) {
        gtk_image_set_pixel_size(GTK_IMAGE(cs->icon_img), shell_widget_get_icon_size(widget));
    }
    shell_widget_apply_mode_visibility(widget->mode, cs->icon_img, cs->label);

    run_script_update(cs);
    if (cs->timer_id == 0) {
        gint interval = cs->interval_sec > 0 ? cs->interval_sec : 5;
        cs->timer_id = g_timeout_add_seconds((guint)interval, on_cs_timer_tick, cs);
    }
}

static void
custom_script_widget_disable(ShellWidget *widget)
{
    CustomScriptWidget *cs = (CustomScriptWidget *)widget;
    if (!cs)
        return;

    if (cs->timer_id != 0) {
        g_source_remove(cs->timer_id);
        cs->timer_id = 0;
    }
    if (cs->active_sd) {
        ((ScriptData *)cs->active_sd)->cs = NULL;
        cs->active_sd = NULL;
    }
    if (cs->watch_id != 0) {
        g_source_remove(cs->watch_id);
        cs->watch_id = 0;
    }
}

const ShellWidgetClass custom_script_widget_class = {
    .id = "custom-script",
    .name = "Custom Script Execution",
    .create = custom_script_widget_create,
    .destroy = custom_script_widget_destroy,
    .get_widget = custom_script_widget_get_widget,
    .enable = custom_script_widget_enable,
    .disable = custom_script_widget_disable,
};
