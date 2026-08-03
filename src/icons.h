#ifndef SHELL_ICONS_H
#define SHELL_ICONS_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the icon system: add bundled icon directory to GTK search path,
 * load the icon-map.json mapping file. Call once after GdkDisplay is ready. */
void shell_icons_init(const gchar *icon_map_path);
void shell_icons_cleanup(void);

/* Resolve an app_id to the best available symbolic icon name.
 * Uses: exact match → glob pattern match → app_id-symbolic → fallback.
 * Returns a static/interned string — do not free. */
const gchar *shell_icons_resolve_app_id(const gchar *app_id);

/* Create a GtkImage widget for an app icon, guaranteed symbolic (monochrome). */
GtkWidget *shell_icons_create_app_icon_widget(const gchar *app_id, gint size);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_ICONS_H */
