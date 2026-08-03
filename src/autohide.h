#ifndef SHELL_AUTOHIDE_H
#define SHELL_AUTOHIDE_H

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellPanel  ShellPanel;
typedef struct _ShellConfig ShellConfig;

typedef enum {
    AUTOHIDE_HIDDEN = 0,
    AUTOHIDE_REVEALING,
    AUTOHIDE_VISIBLE,
    AUTOHIDE_HIDING
} AutohideState;

typedef struct _ShellAutohide ShellAutohide;

struct _ShellAutohide {
    AutohideState  state;
    ShellPanel    *panel;
    guint          hide_timeout_id;
    guint          tick_callback_id;

    gboolean       enabled;
    gint           hide_delay;
    gint           reveal_duration;
    gint           panel_height;
    gint           popover_count;
    GtkLayerShellEdge edge;

    /* Animation state */
    gint64         anim_start_time;
    gint           anim_start_margin;
    gint           anim_target_margin;
};

ShellAutohide *shell_autohide_new(ShellPanel *panel, ShellConfig *config);
void           shell_autohide_enter(ShellAutohide *self);
void           shell_autohide_leave(ShellAutohide *self);
void           shell_autohide_lock(ShellAutohide *self);
void           shell_autohide_unlock(ShellAutohide *self);
void           shell_autohide_destroy(ShellAutohide *self);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_AUTOHIDE_H */
