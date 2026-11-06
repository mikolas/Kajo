#ifndef KAJO_APP_INIT_H
#define KAJO_APP_INIT_H

#include "theme.h"

G_BEGIN_DECLS

/* Standalone-app boot sequence shared by kajo-files, -player, -ide and -settings:
 * load config, create + apply the ShellTheme to the default display, and
 * initialise ShellIcons (AGENTS.md §3.3). Returns the theme (owned by caller). */
ShellTheme *kajo_app_init_theme(void);

/* Counterpart: destroys the theme (may be NULL) and cleans up ShellIcons. */
void kajo_app_shutdown_theme(ShellTheme *theme);

G_END_DECLS

#endif /* KAJO_APP_INIT_H */
