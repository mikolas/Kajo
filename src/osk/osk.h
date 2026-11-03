#pragma once

#include <glib.h>
#include <gtk/gtk.h>

typedef struct _ShellApp ShellApp;
typedef struct _ShellOSK ShellOSK;

ShellOSK *shell_osk_new(ShellApp *app);
void      shell_osk_destroy(ShellOSK *self);

void      shell_osk_show(ShellOSK *self, gboolean manual_pin);
void      shell_osk_hide(ShellOSK *self);
void      shell_osk_toggle(ShellOSK *self);
gboolean  shell_osk_is_visible(ShellOSK *self);
void      shell_osk_apply_config(ShellOSK *self);
