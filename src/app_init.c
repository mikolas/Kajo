#include "app_init.h"
#include "config.h"
#include "icons.h"
#include <string.h>

static gboolean
has_debug_token(const char *val, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = val;
    while (p && *p) {
        const char *sep = strchr(p, ',');
        size_t flen = sep ? (size_t)(sep - p) : strlen(p);
        if (flen == tlen && strncmp(p, token, tlen) == 0)
            return TRUE;
        p = sep ? (sep + 1) : NULL;
    }
    return FALSE;
}

void
kajo_app_disable_portal_client(void)
{
    const char *cur = g_getenv("GDK_DEBUG");
    if (!cur || !*cur) {
        g_setenv("GDK_DEBUG", "no-portals", TRUE);
    } else if (!has_debug_token(cur, "no-portals")) {
        g_autofree gchar *merged = g_strdup_printf("%s,no-portals", cur);
        g_setenv("GDK_DEBUG", merged, TRUE);
    }
    g_setenv("GIO_USE_PORTALS", "0", TRUE);
    g_setenv("GTK_USE_PORTAL", "0", TRUE);
}

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
