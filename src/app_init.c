#include "app_init.h"
#include "config.h"
#include "icons.h"

ShellTheme *
kajo_app_init_theme(void)
{
    g_autoptr(KajoThemeCfgRef) config = kajo_config_store_acquire_theme(NULL);
    const gchar *css_path = (config && config->css_path && *config->css_path)
        ? config->css_path : NULL;
    ShellTheme *theme = shell_theme_new(css_path);
    shell_theme_apply(theme, gdk_display_get_default());
    shell_icons_init();
    return theme;
}

void
kajo_app_shutdown_theme(ShellTheme *theme)
{
    if (theme)
        shell_theme_destroy(theme);
    shell_icons_cleanup();
}
