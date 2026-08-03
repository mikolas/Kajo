#include <gtk/gtk.h>
#include "widget.h"
#include "../compositor/compositor.h"

typedef struct {
    ShellWidget      base;
    GtkWidget       *button;         /* GtkMenuButton */
    GtkWidget       *label;          /* Panel label */
    GtkWidget       *popover;        /* GtkPopover */
    GtkWidget       *popover_title;  /* Full title label */
    GtkWidget       *popover_appid;  /* App ID label */
    GtkWidget       *popover_ws;     /* Workspace ID label */
    GtkWidget       *close_btn;      /* Close Window button */
    guint64          focused_win_id;
    ShellCompositor *compositor;
} WindowWidget;

static void on_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    WindowWidget *win = user_data;
    if (win->compositor && win->focused_win_id > 0) {
        shell_compositor_close_window(win->compositor, win->focused_win_id);
        if (win->popover)
            gtk_popover_popdown(GTK_POPOVER(win->popover));
    }
}

static void window_update(WindowWidget *win)
{
    if (win->compositor == NULL) {
        gtk_label_set_text(GTK_LABEL(win->label), "");
        win->focused_win_id = 0;
        return;
    }

    const CompositorWindow *focused = shell_compositor_get_focused_window(win->compositor);

    if (focused == NULL) {
        gtk_label_set_text(GTK_LABEL(win->label), "");
        gtk_label_set_text(GTK_LABEL(win->popover_title), "No Active Window");
        gtk_label_set_text(GTK_LABEL(win->popover_appid), "App ID: --");
        gtk_label_set_text(GTK_LABEL(win->popover_ws), "Workspace: --");
        gtk_widget_set_sensitive(win->close_btn, FALSE);
        win->focused_win_id = 0;
    } else {
        win->focused_win_id = focused->id;
        gtk_label_set_text(GTK_LABEL(win->label), focused->title != NULL ? focused->title : "");

        gtk_label_set_text(GTK_LABEL(win->popover_title), focused->title != NULL ? focused->title : "Untitled");

        gchar *appid_str = g_strdup_printf("App ID: %s", focused->app_id != NULL ? focused->app_id : "unknown");
        gtk_label_set_text(GTK_LABEL(win->popover_appid), appid_str);
        g_free(appid_str);

        gchar *ws_str = g_strdup_printf("Workspace: %" G_GUINT64_FORMAT, focused->workspace_id);
        gtk_label_set_text(GTK_LABEL(win->popover_ws), ws_str);
        g_free(ws_str);

        gtk_widget_set_sensitive(win->close_btn, TRUE);
    }
    shell_widget_apply_mode_visibility(win->base.mode, NULL, win->label);
}

static void on_window_focus_changed(ShellCompositor *compositor G_GNUC_UNUSED, gpointer userdata)
{
    WindowWidget *win = userdata;
    window_update(win);
}

static ShellWidget *window_create(ShellCompositor *compositor)
{
    WindowWidget *win = g_new0(WindowWidget, 1);
    win->compositor = compositor;

    /* Panel label inside menu button */
    win->label = gtk_label_new("");
    gtk_label_set_max_width_chars(GTK_LABEL(win->label), 50);
    gtk_label_set_ellipsize(GTK_LABEL(win->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(win->label), TRUE);

    /* Menu button */
    win->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(win->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(win->button), win->label);
    gtk_widget_add_css_class(win->button, "flat");
    gtk_widget_add_css_class(win->button, "shell-widget");
    gtk_widget_add_css_class(win->button, "shell-widget-window");

    /* Popover content box */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    win->popover_title = gtk_label_new("No Active Window");
    gtk_widget_add_css_class(win->popover_title, "heading");
    gtk_label_set_xalign(GTK_LABEL(win->popover_title), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(win->popover_title), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(win->popover_title), 40);
    gtk_box_append(GTK_BOX(box), win->popover_title);

    win->popover_appid = gtk_label_new("App ID: --");
    gtk_label_set_xalign(GTK_LABEL(win->popover_appid), 0.0f);
    gtk_box_append(GTK_BOX(box), win->popover_appid);

    win->popover_ws = gtk_label_new("Workspace: --");
    gtk_label_set_xalign(GTK_LABEL(win->popover_ws), 0.0f);
    gtk_box_append(GTK_BOX(box), win->popover_ws);

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    win->close_btn = gtk_button_new_with_label("Close Window");
    gtk_widget_add_css_class(win->close_btn, "destructive-action");
    g_signal_connect(win->close_btn, "clicked", G_CALLBACK(on_close_clicked), win);
    gtk_box_append(GTK_BOX(box), win->close_btn);

    /* Attach popover */
    win->popover = gtk_popover_new();
    gtk_widget_add_css_class(win->popover, "shell-popover");
    gtk_popover_set_child(GTK_POPOVER(win->popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(win->button), win->popover);

    return (ShellWidget *)win;
}

static void window_destroy(ShellWidget *widget)
{
    WindowWidget *win = (WindowWidget *)widget;
    if (win) {
        if (win->compositor) {
            shell_compositor_remove_callbacks(win->compositor, win);
        }
        g_free(win);
    }
}

static GtkWidget *window_get_widget(ShellWidget *widget)
{
    WindowWidget *win = (WindowWidget *)widget;
    return win->button;
}

static void window_enable(ShellWidget *widget)
{
    WindowWidget *win = (WindowWidget *)widget;

    if (win->compositor != NULL) {
        shell_compositor_on_window_focus_changed(win->compositor, on_window_focus_changed, win);
        shell_compositor_on_windows_changed(win->compositor, on_window_focus_changed, win);
        window_update(win);
    }
}

static void window_disable(ShellWidget *widget)
{
    WindowWidget *win = (WindowWidget *)widget;
    (void)win;
}

const ShellWidgetClass window_widget_class = {
    .id         = "window",
    .name       = "Window",
    .create     = window_create,
    .destroy    = window_destroy,
    .get_widget = window_get_widget,
    .enable     = window_enable,
    .disable    = window_disable,
};
