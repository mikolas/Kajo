#ifndef KAJO_PORTAL_SETTINGS_H
#define KAJO_PORTAL_SETTINGS_H

#include <gio/gio.h>
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoPortalSettings KajoPortalSettings;

#define KAJO_PORTAL_SETTINGS_BUS_NAME    "org.freedesktop.impl.portal.desktop.kajo"
#define KAJO_PORTAL_SETTINGS_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define KAJO_PORTAL_SETTINGS_INTERFACE   "org.freedesktop.impl.portal.Settings"

/* Own org.freedesktop.impl.portal.desktop.kajo on the session bus and export
 * org.freedesktop.impl.portal.Settings at /org/freedesktop/portal/desktop. */
KajoPortalSettings *kajo_portal_settings_new(const KajoThemeCfg *initial_theme);

/* Export on an existing GDBusConnection without claiming a well-known name (for unit tests). */
KajoPortalSettings *kajo_portal_settings_new_for_connection(GDBusConnection    *connection,
                                                            const KajoThemeCfg *initial_theme);

/* Recompute appearance values from theme.toml and emit SettingChanged for any key that changed. */
void kajo_portal_settings_update_from_theme(KajoPortalSettings *self,
                                            const KajoThemeCfg *theme);

void kajo_portal_settings_destroy(KajoPortalSettings *self);

/* Pure helpers exposed for unit testing. */
gboolean kajo_portal_settings_namespace_matches(const char *pattern, const char *ns);
gboolean kajo_portal_settings_parse_accent_rgb(const char *hex,
                                               double     *out_r,
                                               double     *out_g,
                                               double     *out_b);

G_END_DECLS

#endif /* KAJO_PORTAL_SETTINGS_H */
