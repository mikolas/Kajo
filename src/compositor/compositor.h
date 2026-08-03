#ifndef SHELL_COMPOSITOR_H
#define SHELL_COMPOSITOR_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellCompositor ShellCompositor;

typedef struct {
    guint64 id;
    guint8  idx;
    gchar  *name;       /* NULL if unnamed */
    gchar  *output;     /* output name, NULL if disconnected */
    gboolean is_active;
    gboolean is_focused;
    gboolean is_urgent;
    guint64  active_window_id;  /* 0 if none */
} CompositorWorkspace;

typedef struct {
    guint64  id;
    gchar   *title;
    gchar   *app_id;
    guint64  workspace_id;
    gboolean is_focused;
} CompositorWindow;

typedef struct {
    gchar **names;      /* NULL-terminated array of layout names */
    guint8  current_idx;
} CompositorKeyboardLayouts;

/* Callback types */
typedef void (*CompositorWorkspacesCallback)(ShellCompositor *compositor, gpointer userdata);
typedef void (*CompositorWindowFocusCallback)(ShellCompositor *compositor, gpointer userdata);
typedef void (*CompositorKeyboardLayoutCallback)(ShellCompositor *compositor, gpointer userdata);

/* Subscribe to window list changes (any window open/close/change) */
typedef void (*CompositorWindowsChangedCallback)(ShellCompositor *compositor, gpointer userdata);

/* Interface */
ShellCompositor *shell_compositor_new(void);
void             shell_compositor_destroy(ShellCompositor *compositor);

/* Queries - return internal state (do not free) */
const GArray   *shell_compositor_get_workspaces(ShellCompositor *compositor);
const CompositorWindow *shell_compositor_get_focused_window(ShellCompositor *compositor);
const CompositorKeyboardLayouts *shell_compositor_get_keyboard_layouts(ShellCompositor *compositor);

/* Get all windows */
const GArray *shell_compositor_get_windows(ShellCompositor *compositor);

/* Get windows for a specific workspace (caller must free returned GArray with g_array_free(arr, TRUE)) */
GArray *shell_compositor_get_windows_for_workspace(ShellCompositor *compositor, guint64 workspace_id);

/* Subscribe to changes */
void shell_compositor_on_workspaces_changed(ShellCompositor *compositor, CompositorWorkspacesCallback cb, gpointer userdata);
void shell_compositor_on_window_focus_changed(ShellCompositor *compositor, CompositorWindowFocusCallback cb, gpointer userdata);
void shell_compositor_on_keyboard_layout_changed(ShellCompositor *compositor, CompositorKeyboardLayoutCallback cb, gpointer userdata);
void shell_compositor_on_windows_changed(ShellCompositor *compositor, CompositorWindowsChangedCallback cb, gpointer userdata);
void shell_compositor_remove_callbacks(ShellCompositor *compositor, gpointer userdata);

/* Focus a specific window */
void shell_compositor_focus_window(ShellCompositor *compositor, guint64 window_id);
void shell_compositor_focus_app(ShellCompositor *compositor, const gchar *app_name, const gchar *desktop_entry, const gchar *app_icon);

/* Close a window */
void shell_compositor_close_window(ShellCompositor *compositor, guint64 window_id);

/* Actions */
void shell_compositor_switch_workspace(ShellCompositor *compositor, guint64 workspace_id);
void shell_compositor_switch_keyboard_layout_idx(ShellCompositor *compositor, guint8 idx);
void niri_send_action(const char *json_action);
gchar *niri_send_ipc_request(const char *json_req);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_COMPOSITOR_H */
