#ifndef SHELL_ICONS_H
#define SHELL_ICONS_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the icon system: add bundled icon directory to GTK search path,
 * load the icon-map.json mapping file. Call once after GdkDisplay is ready. */
void shell_icons_init(void);
void shell_icons_cleanup(void);

/* Resolve an app_id to the best available themed icon name or absolute path
 * to a regular image file. Returns a static/interned string — do not free. */
const gchar *shell_icons_resolve_app_id(const gchar *app_id);

/* Apply the resolved icon (themed icon name or GFileIcon for an absolute file
 * path) onto an existing GtkImage, optionally setting pixel_size if > 0. */
void shell_icons_set_image_for_app_id(GtkImage *image, const gchar *app_id, gint pixel_size);

/* Create a GtkImage widget for an app icon. */
GtkWidget *shell_icons_create_app_icon_widget(const gchar *app_id, gint size);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_ICONS_H */
