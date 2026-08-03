#ifndef SHELL_WIDGET_REGISTRY_H
#define SHELL_WIDGET_REGISTRY_H

#include <glib.h>
#include "widget.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellWidgetRegistry ShellWidgetRegistry;

struct _ShellWidgetRegistry {
    GPtrArray *classes;
};

ShellWidgetRegistry *shell_widget_registry_new(void);
const ShellWidgetClass *shell_widget_registry_find(ShellWidgetRegistry *registry, const char *id);
void shell_widget_registry_destroy(ShellWidgetRegistry *registry);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_WIDGET_REGISTRY_H */
