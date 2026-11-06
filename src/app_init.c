#include "app_init.h"
#include "config.h"
#include "icons.h"

ShellTheme *
kajo_app_init_theme(void)
{
    ShellConfig *config = shell_config_load(NULL);
    ShellTheme *theme = shell_theme_new(config ? config->css_path : NULL);
    shell_theme_apply(theme, gdk_display_get_default());
    shell_icons_init(config ? config->icon_map_path : NULL);
    if (config)
        shell_config_destroy(config);
    return theme;
}

void
kajo_app_shutdown_theme(ShellTheme *theme)
{
    if (theme)
        shell_theme_destroy(theme);
    shell_icons_cleanup();
}
