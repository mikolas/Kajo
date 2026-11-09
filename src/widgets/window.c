#include <gtk/gtk.h>
#include "widget.h"
#include "../icons.h"
#include "../compositor/compositor.h"

typedef struct {
    ShellWidget      base;
    GtkWidget       *button;         /* GtkMenuButton */
    GtkWidget       *icon_img;       /* Focused app icon */
    GtkWidget       *label;          /* Panel label */
    GtkWidget       *popover;        /* GtkPopover */
    GtkWidget       *popover_title;  /* Full title label (.row-title) */
    GtkWidget       *popover_appid;  /* App ID label (.row-subtitle) */
    GtkWidget       *popover_ws;     /* Workspace badge in header */
    GtkWidget       *close_btn;      /* Close Window button in footer */
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
        gtk_widget_set_visible(win->button, FALSE);
        return;
    }

    const CompositorWindow *focused = shell_compositor_get_focused_window(win->compositor);

    if (focused == NULL) {
        if (win->popover)
            gtk_popover_popdown(GTK_POPOVER(win->popover));
        gtk_label_set_text(GTK_LABEL(win->label), "");
        gtk_image_set_from_icon_name(GTK_IMAGE(win->icon_img), "tb-app-window-symbolic");
        gtk_label_set_text(GTK_LABEL(win->popover_title), "No Active Window");
        gtk_label_set_text(GTK_LABEL(win->popover_appid), "App ID: --");
        gtk_label_set_text(GTK_LABEL(win->popover_ws), "[ WS -- ]");
        gtk_widget_set_sensitive(win->close_btn, FALSE);
        win->focused_win_id = 0;
        gtk_widget_set_visible(win->button, FALSE);
    } else {
        win->focused_win_id = focused->id;
        const gchar *icon_name = (focused->app_id && *focused->app_id)
            ? shell_icons_resolve_app_id(focused->app_id)
            : "tb-app-window-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(win->icon_img),
                                     (icon_name && *icon_name) ? icon_name : "tb-app-window-symbolic");
        gtk_label_set_text(GTK_LABEL(win->label),
                           (focused->title && *focused->title) ? focused->title :
                           (focused->app_id ? focused->app_id : "Window"));

        gtk_label_set_text(GTK_LABEL(win->popover_title),
                           (focused->title && *focused->title) ? focused->title : "Untitled");

        gchar *appid_str = g_strdup_printf("App ID: %s",
                                           (focused->app_id && *focused->app_id) ? focused->app_id : "unknown");
        gtk_label_set_text(GTK_LABEL(win->popover_appid), appid_str);
        g_free(appid_str);

        gchar *ws_str = g_strdup_printf("[ WS %" G_GUINT64_FORMAT " ]", focused->workspace_id);
        gtk_label_set_text(GTK_LABEL(win->popover_ws), ws_str);
        g_free(ws_str);

        gtk_widget_set_sensitive(win->close_btn, TRUE);
        gtk_widget_set_visible(win->button, TRUE);
    }
    shell_widget_apply_mode_visibility(win->base.mode, win->icon_img, win->label);
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

    win->icon_img = gtk_image_new_from_icon_name("tb-app-window-symbolic");
    win->label = gtk_label_new("");
    gtk_label_set_max_width_chars(GTK_LABEL(win->label), 50);
    gtk_label_set_ellipsize(GTK_LABEL(win->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(win->label), TRUE);

    GtkWidget *hbox = shell_widget_pack_bar_box(win->icon_img, win->label);

    /* Menu button */
    win->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(win->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(win->button), hbox);
    gtk_widget_add_css_class(win->button, "flat");
    gtk_widget_add_css_class(win->button, "shell-widget");
    gtk_widget_add_css_class(win->button, "shell-widget-window");
    gtk_widget_set_visible(win->button, FALSE);

    /* 3-Zone Popover */
    GtkWidget *body = NULL;
    GtkWidget *footer = NULL;
    win->popover = GTK_WIDGET(shell_widget_create_popover_shell(
        "popover-compact", "ACTIVE WINDOW", "[ WS -- ]",
        &win->popover_ws, &body, &footer));

    win->popover_title = gtk_label_new("No Active Window");
    gtk_widget_add_css_class(win->popover_title, "row-title");
    gtk_label_set_xalign(GTK_LABEL(win->popover_title), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(win->popover_title), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(win->popover_title), 40);
    gtk_box_append(GTK_BOX(body), win->popover_title);

    win->popover_appid = gtk_label_new("App ID: --");
    gtk_widget_add_css_class(win->popover_appid, "row-subtitle");
    gtk_label_set_xalign(GTK_LABEL(win->popover_appid), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(win->popover_appid), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(body), win->popover_appid);

    win->close_btn = gtk_button_new_with_label("CLOSE WINDOW");
    gtk_widget_add_css_class(win->close_btn, "destructive-action");
    gtk_widget_set_hexpand(win->close_btn, TRUE);
    gtk_widget_set_halign(win->close_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(win->close_btn, GTK_ALIGN_CENTER);
    g_signal_connect(win->close_btn, "clicked", G_CALLBACK(on_close_clicked), win);
    gtk_box_append(GTK_BOX(footer), win->close_btn);

    gtk_menu_button_set_popover(GTK_MENU_BUTTON(win->button), win->popover);

    return (ShellWidget *)win;
}

static void window_destroy(ShellWidget *widget)
{
    WindowWidget *win = (WindowWidget *)widget;
    if (win) {
        if (win->close_btn)
            g_signal_handlers_disconnect_by_data(win->close_btn, win);
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
    if (win && win->icon_img)
        gtk_image_set_pixel_size(GTK_IMAGE(win->icon_img), shell_widget_get_icon_size(widget));

    if (win && win->compositor != NULL) {
        shell_compositor_on_window_focus_changed(win->compositor, on_window_focus_changed, win);
        shell_compositor_on_windows_changed(win->compositor, on_window_focus_changed, win);
        window_update(win);
    }
}

static void window_disable(ShellWidget *widget)
{
    WindowWidget *win = (WindowWidget *)widget;
    if (win && win->compositor) {
        shell_compositor_remove_callbacks(win->compositor, win);
    }
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
