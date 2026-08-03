#include <gtk/gtk.h>
#include "widget.h"
#include "../compositor/compositor.h"
#include "../icons.h"

#define MAX_ICONS_PER_WORKSPACE 5
#define APP_ICON_SIZE 18

typedef struct {
    ShellWidget base;
    GtkWidget *box;           /* main horizontal container */
    ShellCompositor *compositor;
} WorkspacesWidget;

static void workspaces_rebuild(WorkspacesWidget *ws);

/* --- Icon resolution --- */

/* --- Click handlers --- */

static void
on_workspace_label_clicked(GtkButton *button, gpointer user_data)
{
    WorkspacesWidget *ws = user_data;
    guint64 workspace_id = (guint64)(guintptr)g_object_get_data(G_OBJECT(button), "workspace-id");

    if (ws->compositor != NULL) {
        shell_compositor_switch_workspace(ws->compositor, workspace_id);
    }
}

static void
on_app_icon_clicked(GtkButton *button, gpointer user_data)
{
    WorkspacesWidget *ws = user_data;
    guint64 window_id = (guint64)(guintptr)g_object_get_data(G_OBJECT(button), "window-id");

    if (ws->compositor != NULL) {
        shell_compositor_focus_window(ws->compositor, window_id);
    }
}

/* --- Scroll handler --- */

static gboolean
on_scroll(GtkEventControllerScroll *controller,
          gdouble dx, gdouble dy,
          gpointer user_data)
{
    (void)controller;
    (void)dx;
    WorkspacesWidget *ws = user_data;

    if (ws->compositor == NULL)
        return FALSE;

    const GArray *workspaces = shell_compositor_get_workspaces(ws->compositor);
    if (workspaces == NULL || workspaces->len == 0)
        return FALSE;

    /* Find currently focused workspace index */
    gint focused_idx = -1;
    for (guint i = 0; i < workspaces->len; i++) {
        const CompositorWorkspace *workspace = &g_array_index(workspaces, CompositorWorkspace, i);
        if (workspace->is_focused) {
            focused_idx = (gint)i;
            break;
        }
    }

    if (focused_idx < 0)
        return FALSE;

    /* Scroll up = previous, scroll down = next */
    gint target_idx;
    if (dy < 0) {
        target_idx = focused_idx - 1;
    } else if (dy > 0) {
        target_idx = focused_idx + 1;
    } else {
        return FALSE;
    }

    if (target_idx < 0 || target_idx >= (gint)workspaces->len)
        return FALSE;

    const CompositorWorkspace *target = &g_array_index(workspaces, CompositorWorkspace, target_idx);
    shell_compositor_switch_workspace(ws->compositor, target->id);

    return TRUE;
}

/* --- Rebuild --- */

static void
workspaces_rebuild(WorkspacesWidget *ws)
{
    /* Remove all existing children */
    GtkWidget *child = gtk_widget_get_first_child(ws->box);
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(ws->box), child);
        child = next;
    }

    if (ws->compositor == NULL) {
        GtkWidget *label = gtk_label_new("\xe2\x80\x94"); /* em dash */
        gtk_box_append(GTK_BOX(ws->box), label);
        return;
    }

    const GArray *workspaces = shell_compositor_get_workspaces(ws->compositor);
    if (workspaces == NULL || workspaces->len == 0) {
        GtkWidget *label = gtk_label_new("\xe2\x80\x94");
        gtk_box_append(GTK_BOX(ws->box), label);
        return;
    }

    /* Sort workspace indices by idx for display order */
    guint *sorted_indices = g_new(guint, workspaces->len);
    for (guint i = 0; i < workspaces->len; i++)
        sorted_indices[i] = i;

    /* Simple insertion sort by workspace idx (small array, ~3-10 items) */
    for (guint i = 1; i < workspaces->len; i++) {
        guint key = sorted_indices[i];
        const CompositorWorkspace *key_ws = &g_array_index(workspaces, CompositorWorkspace, key);
        gint j = (gint)i - 1;
        while (j >= 0) {
            const CompositorWorkspace *cmp_ws = &g_array_index(workspaces, CompositorWorkspace, sorted_indices[j]);
            if (cmp_ws->idx <= key_ws->idx) break;
            sorted_indices[j + 1] = sorted_indices[j];
            j--;
        }
        sorted_indices[j + 1] = key;
    }

    for (guint si = 0; si < workspaces->len; si++) {
        guint i = sorted_indices[si];
        const CompositorWorkspace *workspace = &g_array_index(workspaces, CompositorWorkspace, i);

        /* Create workspace sub-box */
        GtkWidget *workspace_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(workspace_box, "workspace");

        if (workspace->is_active)
            gtk_widget_add_css_class(workspace_box, "active");
        if (workspace->is_focused)
            gtk_widget_add_css_class(workspace_box, "focused");
        if (workspace->is_urgent)
            gtk_widget_add_css_class(workspace_box, "urgent");

        /* Workspace label button */
        gchar *label_text;
        if (workspace->name != NULL && workspace->name[0] != '\0') {
            label_text = g_strdup(workspace->name);
        } else {
            label_text = g_strdup_printf("%u", workspace->idx);
        }

        GtkWidget *label_btn = gtk_button_new_with_label(label_text);
        g_free(label_text);
        gtk_button_set_has_frame(GTK_BUTTON(label_btn), FALSE);

        gchar *ws_tt = g_strdup_printf("Workspace #%u (%s)", workspace->idx, workspace->name ? workspace->name : "Active");
        gtk_widget_set_tooltip_text(label_btn, ws_tt);
        g_free(ws_tt);

        g_object_set_data(G_OBJECT(label_btn), "workspace-id",
                          (gpointer)(guintptr)workspace->id);
        g_signal_connect(label_btn, "clicked",
                         G_CALLBACK(on_workspace_label_clicked), ws);
        gtk_box_append(GTK_BOX(workspace_box), label_btn);

        /* App icons for windows on this workspace */
        GArray *windows = shell_compositor_get_windows_for_workspace(
            ws->compositor, workspace->id);

        if (windows != NULL) {
            guint count = windows->len;
            guint show_count = (count > MAX_ICONS_PER_WORKSPACE)
                                   ? MAX_ICONS_PER_WORKSPACE : count;

            gint icon_sz = shell_widget_get_icon_size((ShellWidget *)ws);
            for (guint j = 0; j < show_count; j++) {
                CompositorWindow *win = &g_array_index(windows, CompositorWindow, j);

                GtkWidget *icon = shell_icons_create_app_icon_widget(win->app_id, icon_sz);

                GtkWidget *icon_btn = gtk_button_new();
                gtk_button_set_child(GTK_BUTTON(icon_btn), icon);
                gtk_button_set_has_frame(GTK_BUTTON(icon_btn), FALSE);
                gtk_widget_add_css_class(icon_btn, "workspace-app-icon");

                gchar *win_tt = g_strdup_printf("%s — %s",
                    win->app_id ? win->app_id : "Window",
                    (win->title && *win->title) ? win->title : "Untitled");
                gtk_widget_set_tooltip_text(icon_btn, win_tt);
                g_free(win_tt);

                g_object_set_data(G_OBJECT(icon_btn), "window-id",
                                  (gpointer)(guintptr)win->id);
                g_signal_connect(icon_btn, "clicked",
                                 G_CALLBACK(on_app_icon_clicked), ws);

                gtk_box_append(GTK_BOX(workspace_box), icon_btn);
            }

            /* Show "+N" if more windows than max */
            if (count > MAX_ICONS_PER_WORKSPACE) {
                gchar *overflow_text = g_strdup_printf("+%u",
                    count - MAX_ICONS_PER_WORKSPACE);
                GtkWidget *overflow_label = gtk_label_new(overflow_text);
                g_free(overflow_text);
                gtk_box_append(GTK_BOX(workspace_box), overflow_label);
            }

            /* Free the windows array (elements cleared automatically by g_array_set_clear_func) */
            g_array_free(windows, TRUE);
        }

        gtk_box_append(GTK_BOX(ws->box), workspace_box);
    }

    g_free(sorted_indices);
}

/* --- Compositor callbacks --- */

static void
on_workspaces_changed(ShellCompositor *compositor, gpointer userdata)
{
    (void)compositor;
    WorkspacesWidget *ws = userdata;
    workspaces_rebuild(ws);
}

static void
on_windows_changed(ShellCompositor *compositor, gpointer userdata)
{
    (void)compositor;
    WorkspacesWidget *ws = userdata;
    workspaces_rebuild(ws);
}

/* --- Widget class methods --- */

static ShellWidget *
workspaces_create(ShellCompositor *compositor)
{
    WorkspacesWidget *ws = g_new0(WorkspacesWidget, 1);
    ws->compositor = compositor;

    ws->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(ws->box, "shell-widget");
    gtk_widget_add_css_class(ws->box, "shell-widget-workspaces");

    /* Scroll controller for workspace cycling */
    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
        GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_scroll), ws);
    gtk_widget_add_controller(ws->box, scroll_ctrl);

    /* Show placeholder until enabled */
    GtkWidget *label = gtk_label_new("\xe2\x80\x94");
    gtk_box_append(GTK_BOX(ws->box), label);

    return (ShellWidget *)ws;
}

static void
workspaces_destroy(ShellWidget *widget)
{
    WorkspacesWidget *ws = (WorkspacesWidget *)widget;
    if (ws) {
        if (ws->compositor) {
            shell_compositor_remove_callbacks(ws->compositor, ws);
        }
        g_free(ws);
    }
}

static GtkWidget *
workspaces_get_widget(ShellWidget *widget)
{
    WorkspacesWidget *ws = (WorkspacesWidget *)widget;
    return ws->box;
}

static void
workspaces_enable(ShellWidget *widget)
{
    WorkspacesWidget *ws = (WorkspacesWidget *)widget;

    if (ws->compositor != NULL) {
        shell_compositor_on_workspaces_changed(ws->compositor,
                                               on_workspaces_changed, ws);
        shell_compositor_on_windows_changed(ws->compositor,
                                            on_windows_changed, ws);
        workspaces_rebuild(ws);
    }
}

static void
workspaces_disable(ShellWidget *widget)
{
    (void)widget;
    /* Callbacks remain registered but widget won't crash if called after disable.
     * Full unsubscription will be handled in destroy via compositor cleanup. */
}

const ShellWidgetClass workspaces_widget_class = {
    .id         = "workspaces",
    .name       = "Workspaces",
    .create     = workspaces_create,
    .destroy    = workspaces_destroy,
    .get_widget = workspaces_get_widget,
    .enable     = workspaces_enable,
    .disable    = workspaces_disable,
};
