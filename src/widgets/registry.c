#include "registry.h"
#include <string.h>

extern const ShellWidgetClass clock_widget_class;
extern const ShellWidgetClass workspaces_widget_class;
extern const ShellWidgetClass window_widget_class;
extern const ShellWidgetClass keyboard_layout_widget_class;
extern const ShellWidgetClass volume_widget_class;
extern const ShellWidgetClass battery_widget_class;
extern const ShellWidgetClass cpu_mem_widget_class;
extern const ShellWidgetClass custom_script_widget_class;
extern const ShellWidgetClass power_widget_class;
extern const ShellWidgetClass network_widget_class;
extern const ShellWidgetClass bluetooth_widget_class;
extern const ShellWidgetClass media_widget_class;
extern const ShellWidgetClass idle_inhibitor_widget_class;
extern const ShellWidgetClass dnd_widget_class;
extern const ShellWidgetClass privacy_widget_class;
extern const ShellWidgetClass tray_widget_class;
extern const ShellWidgetClass launcher_widget_class;
extern const ShellWidgetClass notifications_widget_class;
extern const ShellWidgetClass control_center_widget_class;
extern const ShellWidgetClass theme_widget_class;

static void register_builtin_widgets(ShellWidgetRegistry *registry)
{
    g_ptr_array_add(registry->classes, (gpointer)&launcher_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&control_center_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&theme_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&clock_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&workspaces_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&window_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&keyboard_layout_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&volume_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&battery_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&cpu_mem_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&custom_script_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&power_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&network_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&bluetooth_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&media_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&idle_inhibitor_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&dnd_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&privacy_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&tray_widget_class);
    g_ptr_array_add(registry->classes, (gpointer)&notifications_widget_class);
}

ShellWidgetRegistry *shell_widget_registry_new(void)
{
    ShellWidgetRegistry *registry = g_new0(ShellWidgetRegistry, 1);
    registry->classes = g_ptr_array_new();
    register_builtin_widgets(registry);
    return registry;
}

const ShellWidgetClass *shell_widget_registry_find(ShellWidgetRegistry *registry, const char *id)
{
    for (guint i = 0; i < registry->classes->len; i++) {
        const ShellWidgetClass *cls = g_ptr_array_index(registry->classes, i);
        if (strcmp(cls->id, id) == 0) {
            return cls;
        }
    }
    return NULL;
}

void shell_widget_registry_destroy(ShellWidgetRegistry *registry)
{
    if (registry == NULL) {
        return;
    }
    g_ptr_array_free(registry->classes, TRUE);
    g_free(registry);
}
