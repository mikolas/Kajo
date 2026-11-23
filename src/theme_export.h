#ifndef KAJO_THEME_EXPORT_H
#define KAJO_THEME_EXPORT_H

#include <glib.h>
#include "config/kajo_config_types.h"

G_BEGIN_DECLS

/* Opt-in exporter applying Kajo's dark Aero+Metro palette & 0px geometry to external
 * GTK3, GTK4/libadwaita, and Qt 6 (qt6ct) apps, with exact pre-Kajo snapshot & revert. */

gboolean kajo_theme_export_qt6ct_available(void);
gboolean kajo_theme_export_has_backup(const char *config_dir);

/* Returns comma-separated names of detected unused third-party CSS files (e.g. "dank-colors.css")
 * inside gtk-3.0 / gtk-4.0, or NULL if none found. Caller frees. */
char    *kajo_theme_export_detect_leftovers(const char *config_dir);

/* Pure generators exposed for unit testing. Caller frees returned strings. */
const char *kajo_theme_export_nearest_gnome_accent(const char *hex_color);
char       *kajo_theme_export_generate_gtk4_css(const KajoThemeCfg *cfg);
char       *kajo_theme_export_generate_gtk3_css(const KajoThemeCfg *cfg);
char       *kajo_theme_export_generate_qt6ct_colors(const KajoThemeCfg *cfg);
char       *kajo_theme_export_apply_managed_block(const char *existing_content,
                                                  const char *import_filename);
char       *kajo_theme_export_strip_managed_block(const char *existing_content,
                                                  gboolean   *out_had_block);

/* Synchronize external toolkit theming according to cfg->theme_external_gtk / cfg->theme_external_qt:
 * - Takes an atomic backup of original user files & GSettings on first enable (never overwritten).
 * - Applies enabled layers and reverts disabled layers back to their exact backed-up defaults.
 * - `config_dir` == NULL uses g_get_user_config_dir(); `touch_system_env` controls GSettings &
 *   `systemctl --user` env updates (pass FALSE in unit tests). */
gboolean kajo_theme_export_sync(const KajoThemeCfg *cfg,
                                const char         *config_dir,
                                gboolean            touch_system_env,
                                GError            **error);

/* Explicitly restore EVERY backed-up file/GSettings value and remove all Kajo-managed
 * external theme files and the backup snapshot. */
gboolean kajo_theme_export_revert_all(const char *config_dir,
                                      gboolean    touch_system_env,
                                      GError    **error);

G_END_DECLS

#endif /* KAJO_THEME_EXPORT_H */
