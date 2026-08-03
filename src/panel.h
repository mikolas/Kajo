#ifndef SHELL_PANEL_H
#define SHELL_PANEL_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellApp      ShellApp;
typedef struct _ShellAutohide ShellAutohide;

typedef enum {
    PANEL_WIDGET_LEFT = 0,
    PANEL_WIDGET_CENTER,
    PANEL_WIDGET_RIGHT
} PanelWidgetPosition;

typedef struct _ShellPanel ShellPanel;

struct _ShellPanel {
    GtkWindow     *window;
    GtkWidget     *container;
    GtkBox        *box_left;
    GtkBox        *box_center;
    GtkBox        *box_right;
    ShellAutohide *autohide;
    ShellApp      *app;
    GPtrArray     *widgets;
};

ShellPanel *shell_panel_new(ShellApp *app);
void        shell_panel_add_widget(ShellPanel *panel, PanelWidgetPosition position, GtkWidget *widget);
void        shell_panel_register_popover(ShellPanel *panel, GtkPopover *popover);
void        shell_panel_show(ShellPanel *panel);
void        shell_panel_destroy(ShellPanel *panel);
GtkWindow     *shell_panel_get_window(ShellPanel *panel);
GtkWidget     *shell_panel_get_container(ShellPanel *panel);
ShellAutohide *shell_panel_get_autohide(ShellPanel *panel);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_PANEL_H */
